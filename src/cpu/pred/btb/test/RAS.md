# Return Address Stack (RAS) Design and Implementation

## Overview

The Return Address Stack (RAS) is a critical component of the branch prediction unit that predicts return addresses for function calls. The BTBRAS implementation features a sophisticated dual-stack architecture designed to handle speculative execution and provide high accuracy for return address prediction.

## Architecture

### Dual-Stack Design

The BTBRAS employs two separate stacks to handle different phases of execution:

1. **Committed Stack** (`stack[]`)
   - Stores confirmed return addresses that have been committed
   - Managed by `nsp` (non-speculative stack pointer)
   - Size: `numEntries` (configurable, typically 16)
   - Contains only addresses that have been verified through the commit stage

2. **Speculative Stack** (`inflightStack[]`)
   - Stores speculative return addresses during prediction
   - Managed by `TOSR` (Top of Stack Read), `TOSW` (Top of Stack Write), and `BOS` (Bottom of Stack)
   - Size: `numInflightEntries` (configurable, typically 8)
   - Forms a circular buffer to handle pointer wraparound

### Key Data Structures

#### RASEntry
```cpp
typedef struct RASEntry {
    RASEssential data;
    // Constructors for different initialization scenarios
} RASEntry;
```

#### RASEssential
```cpp
typedef struct RASEssential {
    Addr retAddr;    // Return address
    unsigned ctr;    // Reference counter for duplicate addresses
} RASEssential;
```

#### RASInflightEntry
```cpp
typedef struct RASInflightEntry {
    RASEssential data;
    int nos;         // Next-on-stack pointer (linked list structure)
} RASInflightEntry;
```

#### RASMeta
```cpp
typedef struct RASMeta {
    int ssp;         // Speculative stack pointer
    int sctr;        // Speculative counter
    int TOSR;        // Top of stack read pointer
    int TOSW;        // Top of stack write pointer
    bool willPush;   // Prediction flag
    Addr target;     // Predicted target address
} RASMeta;
```

## State Management

### Pointers and Counters

- **ssp** (Speculative Stack Pointer): Points to the top of speculative operations
- **nsp** (Non-speculative Stack Pointer): Points to the top of committed stack
- **sctr** (Speculative Counter): Counter for speculative duplicate addresses
- **TOSR** (Top of Stack Read): Read pointer for inflight queue
- **TOSW** (Top of Stack Write): Write pointer for inflight queue
- **BOS** (Bottom of Stack): Bottom pointer for inflight queue
- **maxCtr**: Maximum value for counters (derived from `ctrWidth`)

### Counter-Based Duplicate Handling

The RAS implements an optimization for recursive functions by using counters instead of storing duplicate addresses:

- When the same return address is pushed multiple times, increment the counter
- When popping, decrement the counter until it reaches zero before moving the pointer
- This saves stack space and improves efficiency for recursive calls

## Operational Flow

### 1. Prediction Phase (`putPCHistory`)

```cpp
void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                  std::vector<FullBTBPrediction> &stagePreds)
```

- Creates new metadata (`RASMeta`)
- Fills prediction stages with current top-of-stack address
- Prepares for potential speculative updates

### 2. Speculative Update (`specUpdateHist`)

```cpp
void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
```

- **For Call Instructions**: Executes `push(retAddr)`
  - Calculates return address as `pc + size`
  - Updates speculative pointers and counters
  - Adds entry to inflight queue
  
- **For Return Instructions**: Executes `pop()`
  - Removes top entry from inflight queue or committed stack
  - Updates pointers and counters accordingly

### 3. Recovery Phase (`recoverHist`)

```cpp
void recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, 
                 int shamt, bool cond_taken)
```

- Restores RAS state to a previous checkpoint using stored metadata
- Handles mispredictions by rolling back speculative changes
- Re-applies operations if the branch was actually taken

### 4. Commit Phase (`update`)

```cpp
void update(const FetchStream &entry)
```

- **For Call Instructions**: Executes `push_stack(retAddr)`
  - Moves speculative results to committed stack
  - Updates `nsp` and committed counters
  
- **For Return Instructions**: Executes `pop_stack()`
  - Commits the pop operation to the committed stack

## Stack Operations

### Speculative Operations

#### `push(Addr retAddr)`
1. Check if return address matches current top with available counter space
2. If match: increment `sctr`
3. If no match: advance `ssp`, reset `sctr`
4. Create inflight entry and update inflight pointers
5. Link new entry in inflight queue

#### `pop()`
1. Check if `TOSR` points to valid inflight entry
2. If valid: use inflight data, advance `TOSR`
3. If invalid: use committed stack data
4. Update speculative counters and pointers

### Commit Operations

#### `push_stack(Addr retAddr)`
1. Check if return address matches committed top
2. If match and counter not at max: increment counter
3. If no match: advance `nsp`, store new address with counter = 0

#### `pop_stack()`
1. Check committed top counter
2. If counter > 0: decrement counter
3. If counter = 0: move `nsp` backward

## Pointer Management

### Circular Buffer Operations

The inflight stack uses circular buffer management:

```cpp
void inflightPtrInc(int &ptr) { ptr = (ptr + 1) % numInflightEntries; }
void inflightPtrDec(int &ptr) { /* Handle wrap-around to numInflightEntries-1 */ }
```

### Range Checking

```cpp
bool inflightInRange(int &ptr)
```

Determines if a pointer is within the valid range of the inflight queue, handling wrap-around cases:

- If `TOSW > BOS`: simple range check
- If `TOSW < BOS`: wrapped range check
- If `TOSW == BOS`: empty queue

## Error Handling and Edge Cases

### Stack Overflow
- Inflight stack: Circular buffer naturally handles overflow by reusing slots
- Committed stack: Circular buffer with modulo arithmetic

### Stack Underflow
- Pop on empty stack: Returns default address (0x80000000L)
- Graceful degradation without crashes

### Counter Overflow
- Counters are capped at `maxCtr` to prevent overflow
- Additional pushes of same address are ignored when at maximum

## Testing Considerations

### Critical Test Cases

1. **Basic Operations**: Push, pop, multiple pushes
2. **Counter Behavior**: Duplicate addresses, counter limits
3. **Stack Limits**: Full stack behavior, overflow handling
4. **Recovery**: State restoration after mispredictions
5. **Commit Flow**: Speculative to committed transition
6. **Edge Cases**: Empty stack operations, pointer wraparound
7. **Complex Scenarios**: Nested calls, mixed operations

### Test Infrastructure Requirements

- Helper functions for creating BTB entries
- State verification utilities
- Scenario simulation capabilities

## Performance Characteristics

- **Prediction Accuracy**: High accuracy for well-behaved call/return patterns
- **Hardware Cost**: Moderate (two small stacks + control logic)
- **Latency**: Low latency for prediction, higher for complex recovery
- **Scalability**: Configurable stack sizes for different performance requirements

## Integration with Branch Prediction

The RAS integrates with the broader branch prediction framework:

- Receives prediction requests via `putPCHistory`
- Provides return targets through prediction metadata
- Coordinates with other predictors through the BTB infrastructure
- Supports speculative execution and recovery mechanisms

This dual-stack architecture provides robust return address prediction while maintaining the flexibility needed for speculative execution in modern out-of-order processors.