# DecoupledBPUWithBTB Design Document

## Overview

The `DecoupledBPUWithBTB` is a decoupled branch prediction unit implementation using a BTB-based design for GEM5. It separates branch prediction from instruction fetch, enabling more accurate predictions through multiple prediction stages. The predictor uses several components including UBTB (micro-BTB), BTB (Branch Target Buffer), and TAGE predictor to generate predictions at different pipeline stages.

## Key Data Structures

### 1. Prediction Components

```
TimedBaseBTBPredictor (Base class)
├── DefaultBTB (UBTB)  
├── DefaultBTB (BTB)
└── BTBTAGE (TAGE)
```

Each predictor has a `numDelay` member indicating its pipeline stage delay:
- UBTB: 0 cycle delay (immediate prediction)
- BTB/TAGE: 1 cycle delay
- ITTAGE: 2 cycles delay (not included in this implementation)

### 2. Branch Information Structures

#### BranchInfo
Basic branch information:
```cpp
struct BranchInfo {
    Addr pc;          // Branch instruction address
    Addr target;      // Target address
    bool isCond;      // Is conditional branch
    bool isIndirect;  // Is indirect jump
    bool isCall;      // Is function call
    bool isReturn;    // Is function return
    uint8_t size;     // Instruction size
};
```

#### BTBEntry
Extends BranchInfo with BTB-specific fields:
```cpp
struct BTBEntry : BranchInfo {
    bool valid;       // Entry validity
    int ctr;          // Prediction counter
    Addr tag;         // BTB tag
};
```

### 3. Prediction Results

#### FullBTBPrediction
Integrates predictions from all predictors:
```cpp
struct FullBTBPrediction {
    Addr bbStart;                     // Basic block start address
    std::vector<BTBEntry> btbEntries; // BTB prediction entries
    std::map<Addr, bool> condTakens;  // Conditional branch predictions
    std::map<Addr, Addr> indirectTargets; // Indirect jump targets
    Addr returnTarget;                // Return address prediction
};
```

### 4. Fetch Management Structures

#### FetchTarget
Manages prediction and execution information for a continuous instruction sequence:
```cpp
struct FetchTarget {
    // Basic information
    Addr startPC;          // Start address
    Addr predEndPC;        // Predicted end address
    bool predTaken;        // Predicted taken
    
    // Prediction state
    BranchInfo predBranchInfo;  // Predicted branch info
    std::vector<BTBEntry> predBTBEntries;  // BTB predictions
    
    // Execution results (updated after resolution)
    bool exeTaken;         // Actual taken
    BranchInfo exeBranchInfo;  // Actual branch info
    
    // Statistics
    int fetchInstNum;     // Number of fetched instructions
    int commitInstNum;    // Number of committed instructions
};
```

#### FtqEntry (Fetch Target Queue Entry)
Manages fetch requests for the fetch unit:
```cpp
struct FtqEntry {
    Addr startPC;         // Fetch start address
    Addr endPC;           // Fetch end address
    Addr takenPC;         // Branch instruction address
    bool taken;           // Branch taken flag
    Addr target;          // Jump target
    FetchTargetId fsqID;  // Corresponding fetch stream ID
    
    // Loop related
    bool inLoop;          // In loop flag
    int iter;             // Iteration count
    bool isExit;          // Loop exit flag
};
```

### 5. Queue Management

- `FetchTargetQueue`: Manages fetch targets (FTQ)
- `std::map<FetchTargetId, FetchTarget> FetchTargetQueue`: Manages fetch streams (FSQ)

## Key Functions

### 1. Prediction Flow

#### `tick()`
Main prediction cycle function that:
- Monitors FSQ size for statistics
- Generates final predictions if PC and history received
- Tries to enqueue new predictions if not squashing
- Decrements override bubbles counter
- Requests new predictions if FSQ not full

#### `generateFinalPredAndCreateBubbles()`
- Collects predictions from all stages
- Selects the most accurate prediction
- Calculates necessary bubbles
- Updates prediction state

#### `tryEnqFetchTarget()`
- Checks for new prediction results
- Creates new fetch streams
- Updates prediction state

#### `processNewPrediction()`
- Uses various predictor components for prediction
- Integrates prediction results
- Creates new FetchTarget entries

#### `tryEnqFetchTarget()`
- Adds prediction results to FTQ
- Updates fetch state

### 2. Prediction Recovery

#### `controlSquash()`
- Handles branch mispredictions
- Restores predictor state
- Updates history information

#### `nonControlSquash()`
- Handles pipeline flushes not caused by branches
- Restores predictor state

#### `trapSquash()`
- Handles trap-induced squashes
- Restores predictor state

#### `update()`
- Updates predictor state
- Commits verified prediction results

### 3. History Management

#### `histShiftIn()`
- Updates branch history
- Shifts in new branch outcomes

#### `checkHistory()`
- Validates history consistency

## Implementation Details

### Prediction Pipeline

The prediction process is pipelined, with multiple predictions in flight simultaneously at different stages:

```
clock   gen Pred N    enq FSQ    enq FTQ and use
------  ---------   ---------   --------- 
0       gen Pred 0   -           -        
1       gen Pred 1   FSQ0        -        
2       gen Pred 2   FSQ1        FTQ0     
3       gen Pred 3   FSQ2        FTQ1     
4       gen Pred 4   FSQ3        FTQ2     
```

important variables:
- `sentPCHist`: Indicates that PC and history have been sent to predictors, get prediction from BP, waiting for final prediction
- `receivedPred`: Indicates that final prediction has been generated, waiting to be enqueued
- `squashing`: Prevents new final predictions enqueued to FSQ/FTQ when a branch prediction error occurs
- `numOverrideBubbles`: Controls the delay bubbles when higher-stage predictors cover lower-stage predictions


In each clock cycle:
1. `tick()`: Process all pipeline stages
   - If `sentPCHist==true`, call `generateFinalPredAndCreateBubbles()`
   - If `!squashing`, try enqueue FSQ and FTQ
   - If new prediction is needed, call `putPCHistory()` of each component and set `sentPCHist=true`


### Detailed pipeline operations

1. **Generate prediction stage** (`putPCHistory`)
   - Each predictor component generates predictions based on current PC and history
   - Results are stored in the `predsOfEachStage` array

2. **Final prediction formation stage** (`generateFinalPredAndCreateBubbles`)
   - Selects the most accurate prediction from each stage
   - Calculates the number of needed coverage bubbles
   - Stores results in `finalPred`
   - Sets `receivedPred = true`

3. **FSQ enqueue stage** (`tryEnqFetchTarget`)
   - Creates new `FetchTarget` entries
   - Adds entries to `FetchTargetQueue`, assigns `fsqId`
   - Updates global history based on predictions
   - Resets `receivedPred = false`
   - Increments `fsqId` for the next stream

4. **FTQ enqueue stage** (`tryEnqFetchTarget`)
   - Converts FSQ entries to FTQ entries
   - Adds entries to `fetchTargetQueue` for the fetch unit

### Typical Timing State Transitions

Assuming normal prediction pipeline operation, the main operations and states for each clock cycle are as follows:

| Clk | Gen Pred  |  FSQ Queue | FTQ Queue | sentPCHist | receivedPred |
|-----|-----------|------------|-----------|------------|--------------|
| 0   | Gen Pred0 |  -         | -         | true       | false        |
| 1   | Gen Pred1 |  FSQ0      | -         | true       | true         |
| 2   | Gen Pred2 |  FSQ1      | FTQ0      | true       | true         |
| 3   | Gen Pred3 |  FSQ2      | FTQ1      | true       | true         |

This pipelined processing allows the predictor to generate a new prediction each cycle while handling subsequent stages of previous predictions, increasing throughput.

### Recovery Process

When a misprediction is detected:
1. `squashing` is set to true, which prevents new predictions
2. Branch history is restored to the point of the misprediction
3. All downstream fetch streams and targets are flushed
4. State flags are reset and prediction resumes from the correct path

### BTB Configuration

- Default BTB:
  * 2048 entries
  * 20-bit tags
  * 8-way set associative
  * 1 cycle delay

- Micro-BTB (UBTB):
  * 32 entries
  * 38-bit tags
  * 32-way set associative
  * 0 cycle delay

### TAGE Configuration

- Single TAGE predictor instance
- Uses branch history for direction prediction

## Testing Considerations

For unit testing the `DecoupledBPUWithBTB`, consider:

1. Testing each predictor component individually (already done)
2. Testing the integrated prediction flow
3. Testing recovery mechanisms after mispredictions
4. Testing history management and consistency
5. Testing fetch queue behavior
6. Testing prediction accuracy under different branch patterns
