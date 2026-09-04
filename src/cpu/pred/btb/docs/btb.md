# Branch Target Buffer (BTB) Design Document

## 1. Overview

The Branch Target Buffer (BTB) is a critical component in modern branch prediction systems. It serves as a cache-like structure that stores information about recently encountered branches, including their target addresses and branch types. The BTB enables the processor to predict branch targets early in the pipeline, reducing branch penalties and improving overall performance.

This document describes the design and implementation of the DefaultBTB in gem5, which models a set-associative BTB with MRU (Most Recently Used) replacement policy and support for half-aligned mode.

Notion: BTB entry is different from FTB entry.
One BTB entry is one branch.
One FTB entry is one Fetch Block, contains multiple branches.
For BTB, 32B Fetch Block(FBlock) is the basic unit, one FBlock may contains multiple BTB entries,
all of the BTB entries in one FBlock are predicted in the same way(same index, different tag/ different ways).
Then, BTB entries in one FBlock are send to TAGE, TAGE will predict each BTB entry independently,
TAGE will find the first predicted as taken branch, and return it's target address to BTB as the next predict pc.

## 2. Key Design Principles

### 2.1 Multi-level BTB Hierarchy

The BTB implementation supports a multi-level hierarchy:
- **L0 BTB**: Zero-delay, smaller BTB that provides immediate predictions
- **L1 BTB**: Larger BTB with higher accuracy but longer access time

This hierarchical approach balances prediction latency and accuracy.

### 2.2 Decoupled Frontend Support

The BTB is designed to work with gem5's decoupled frontend pipeline, where:
- Predictions are made early in the pipeline
- Multiple predictions are tracked simultaneously
- Execution results are used to update prediction state

### 2.3 Half-Aligned Mode

The BTB supports a half-aligned mode that allows it to access and predict branches across 32-byte block boundaries. In this mode, the BTB effectively looks up two consecutive 32-byte blocks for each prediction, providing better coverage for branches near block boundaries.
Now Fetch Width is max 64B, contains 2 32B FBlock.

## 3. Data Structures

### 3.1 Branch Information

```cpp
struct BranchInfo {
    Addr pc;                  // Branch instruction address
    Addr target;              // Branch target address
    bool isCond;              // Is conditional branch
    bool isIndirect;          // Is indirect branch
    bool isCall;              // Is function call
    bool isReturn;            // Is function return
    uint8_t size;             // Instruction size
    // Helper methods omitted for brevity
};
```

### 3.2 BTB Entry

```cpp
struct BTBEntry : BranchInfo {
    bool valid;               // Entry is valid
    int ctr;                  // Saturating counter (-2 to 1)
    Addr tag;                 // Tag bits for lookup
};
```

### 3.3 Ticked BTB Entry

```cpp
struct TickedBTBEntry : BTBEntry {
    uint64_t tick;            // Timestamp for MRU replacement
};
```

### 3.4 BTB Structure

```cpp
// Set-associative organization
std::vector<BTBSet> btb;      // Array of sets
using BTBSet = std::vector<TickedBTBEntry>; // Each set contains ways

// MRU tracking
std::vector<BTBHeap> mruList; // One heap per set
using BTBHeap = std::vector<BTBSetIter>; // Tracks entry age
```

### 3.5 Prediction Metadata

```cpp
struct BTBMeta {
    std::vector<BTBEntry> hit_entries;     // Hit entries in L1 BTB
    std::vector<BTBEntry> l0_hit_entries;  // Hit entries in L0 BTB
};
```

## 4. Key Algorithms

### 4.1 BTB Lookup

The lookup process in half-aligned mode:

1. Calculate 32B aligned address: `alignedPC = block_pc & ~(blockSize - 1)`
2. Lookup first 32B block using index and tag
3. Lookup next 32B block
4. Merge results and sort by PC order

```cpp
std::vector<TickedBTBEntry> lookup(Addr block_pc) {
    std::vector<TickedBTBEntry> res;
    
    if (halfAligned) {
        // Calculate 32B aligned address
        Addr alignedPC = block_pc & ~(blockSize - 1);
        // Lookup first 32B block
        res = lookupSingleBlock(alignedPC);
        // Lookup next 32B block
        auto nextBlockRes = lookupSingleBlock(alignedPC + blockSize);
        // Merge results
        res.insert(res.end(), nextBlockRes.begin(), nextBlockRes.end());
        // Sort entries by PC order
        std::sort(res.begin(), res.end(),
                 [](const TickedBTBEntry &a, const TickedBTBEntry &b) {
                     return a.pc < b.pc;
                 });
    } else {
        res = lookupSingleBlock(block_pc);
    }
    return res;
}
```

### 4.2 Prediction Generation

The prediction process:

1. Lookup BTB entries for the fetch block
2. Process entries (filter, sort)
3. Fill predictions for each pipeline stage
4. Update prediction metadata

```cpp
void putPCHistory(Addr startAddr,
                  const boost::dynamic_bitset<> &history,
                  std::vector<FullBTBPrediction> &stagePreds) {
    // Lookup all matching entries in BTB
    auto find_entries = lookup(startAddr);
    
    // Process BTB entries
    auto processed_entries = processEntries(find_entries, startAddr);
    
    // Fill predictions for each pipeline stage
    fillStagePredictions(processed_entries, stagePreds);
    
    // Update metadata for later stages
    updatePredictionMeta(processed_entries, stagePreds);
}
```

### 4.3 BTB Update

The update process:

1. Process old entries from prediction metadata
2. Check prediction hit status
3. Collect entries to update
4. Update BTB entries with execution results
   - For conditional branches, update counter
   - For indirect branches, update target

```cpp
void update(const FetchTarget &stream) {
    // 1. Process old entries
    auto old_entries = processOldEntries(stream);
    
    // 2. Check prediction hit status, for stats recording
    checkPredictionHit(stream,
        std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]).get());

    // 3. Collect entries to update
    auto entries_to_update = collectEntriesToUpdate(old_entries, stream);
    
    // 4. Update BTB entries - each entry uses its own PC to calculate index and tag
    for (auto &entry : entries_to_update) {
        // Calculate 32-byte aligned address for this entry
        Addr entryPC = entry.pc & ~(blockSize - 1);
        updateBTBEntry(entryPC, entry, stream);
    }
}
```

### 4.4 MRU Replacement Policy

The BTB uses a heap-based MRU (Most Recently Used) replacement policy:

1. Each BTB set has an associated MRU heap
2. The heap maintains BTB entry iterators ordered by their timestamp
3. The oldest entry (top of heap) is replaced when needed
4. Timestamps are updated on BTB accesses

```cpp
// Replace oldest entry in the set
std::pop_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
const auto& entry_in_btb_now = mruList[btb_idx].back();
*entry_in_btb_now = ticked_entry;
std::make_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
```

## 5. Prediction Flow

The BTB operation follows a specific prediction-update pattern:

### 5.1 Prediction Phase
```cpp
// Make prediction
btb->putPCHistory(startPC, history, stagePreds);

// Get prediction metadata
auto meta = btb->getPredictionMeta();
```

### 5.2 Update Phase
```cpp
// Setup update stream
FetchTarget stream;
stream.startPC = pc;
stream.predMetas[0] = meta;

// Optional: Get and set new BTB entry (only for L1 BTB)
if (isL1BTB) {
    btb->getAndSetNewBTBEntry(stream);
}

// Update BTB
btb->update(stream);
```

## 6. Key Configuration Parameters

- **numEntries**: Total number of BTB entries
- **numWays**: Number of ways per set (associativity)
- **tagBits**: Number of bits for each tag
- **numDelay**: Number of delay cycles (0 for L0 BTB)
- **halfAligned**: When true, BTB looks up two consecutive 32B blocks

## 7. Interaction with Other Predictors

The BTB works in conjunction with other branch predictors:

- **Direction Predictor**: The BTB identifies branches and provides target addresses, while direction predictors (like TAGE) predict whether conditional branches are taken.
- **Return Address Stack (RAS)**: For return instructions, the RAS typically provides higher accuracy predictions than the BTB.
- **Indirect Branch Predictor**: For indirect jumps, specialized indirect predictors may override BTB predictions.

## 8. Statistics

The BTB implementation collects various statistics to evaluate performance:

- Hit/miss rates for different branch types
- Update success/failure rates
- Branch type distribution
- Prediction accuracy metrics

These statistics help in tuning the BTB design and understanding its behavior.
