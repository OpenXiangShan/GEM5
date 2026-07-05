# BTB-TAGE Predictor Design Document

## 1. Overview

The BTB-TAGE (Branch Target Buffer - Tagged Geometric Length) predictor is a sophisticated branch prediction mechanism implemented in gem5. It combines a BTB (Branch Target Buffer) for target address prediction with a TAGE (Tagged Geometric Length) predictor for direction prediction of conditional branches. The TAGE component uses multiple prediction tables with different history lengths to capture various correlation patterns in branch behavior.

This document describes the design and implementation of the BTBTAGE class, which serves as the direction predictor component in the overall branch prediction system.

## 2. Key Data Structures

### 2.1 TAGE Entry

```cpp
struct TageEntry {
    bool valid;      // Whether this entry is valid
    Addr tag;        // Tag for matching
    short counter;   // Prediction counter (-4 to 3), 3 bits
    bool useful;     // Whether this entry is useful for prediction
    Addr pc;         // Branch PC, for BTB entry PC check
};
```

The `TageEntry` structure represents a single entry in a TAGE prediction table. The `counter` field is a saturating counter that determines the taken/not-taken prediction, with values ≥ 0 indicating taken. The `useful` bit is set when this entry provided a correct prediction different from the alternative prediction.

### 2.2 Table Lookup Result

```cpp
struct TageTableInfo {
    bool found;      // Whether a matching entry was found
    TageEntry entry; // The matching entry
    unsigned table;  // Which table this entry was found in
    Addr index;      // Index in the table
    Addr tag;        // Tag that was matched
};
```

This structure contains information about a TAGE table lookup, including whether an entry was found and the details of the matching entry.

### 2.3 TAGE Prediction

```cpp
struct TagePrediction {
    Addr btb_pc;            // BTB entry PC
    TageTableInfo mainInfo; // Main prediction info
    TageTableInfo altInfo;  // Alternative prediction info
    bool useAlt;            // Whether to use alternative prediction
    bool taken;             // Final prediction
};
```

The `TagePrediction` structure contains the complete prediction result, including both the main and alternative predictions, and whether the alternative prediction should be used.

### 2.4 Prediction Metadata

```cpp
struct TageMeta {
    std::map<Addr, TagePrediction> preds;
    bitset usefulMask;
    std::vector<FoldedHist> tagFoldedHist;
    std::vector<FoldedHist> altTagFoldedHist;
    std::vector<FoldedHist> indexFoldedHist;
};
```

The `TageMeta` structure stores metadata for predictions, which is essential for recovering from mispredictions and updating the predictor state. It contains the predictions for each PC, a mask of useful bits, and the folded history states.

## 3. Key Algorithms

### 3.1 Prediction Generation

The prediction process involves:

1. Looking up entries in all TAGE tables
2. Finding the main and alternative predictions
3. Determining whether to use the alternative prediction
4. Generating the final prediction

```cpp
std::map<Addr, bool> lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries) {
    // Clear old prediction metadata and save current history state
    meta.preds.clear();
    meta.tagFoldedHist = tagFoldedHist;
    meta.altTagFoldedHist = altTagFoldedHist;
    meta.indexFoldedHist = indexFoldedHist;

    // Generate predictions for each BTB entry
    std::map<Addr, bool> cond_takens;
    for (auto &btb_entry : btbEntries) {
        if (btb_entry.isCond && btb_entry.valid) {
            // Look up all TAGE tables and generate predictions
            auto pred = generateSinglePrediction(btb_entry, table_result);
            meta.preds[btb_entry.pc] = pred;
            cond_takens[btb_entry.pc] = pred.taken;
        }
    }
    return cond_takens;
}
```

### 3.2 Table Lookup

The table lookup process involves:

1. Calculating indices and tags for each table
2. Checking for matching entries
3. Collecting the entries and updating useful masks
4. now this function is integrated into generateSinglePrediction

```cpp
std::vector<BTBTAGE::TageEntry> lookupTageTable(const Addr &startPC) {
    std::vector<BTBTAGE::TageEntry> entries(numPredictors);
    meta.usefulMask.resize(numPredictors);
    
    // Look up entries in all TAGE tables
    for (int i = 0; i < numPredictors; ++i) {
        Addr index = getTageIndex(startPC, i);
        Addr tag = getTageTag(startPC, i);
        auto &entry = tageTable[i][index];
        
        result.entries[i] = entry;
        result.indices[i] = index;
        result.tags[i] = tag;
        result.useful_mask[i] = entry.useful;
    }
    return result;
}
```

### 3.3 Predictor Update

The update process involves:

1. Preparing BTB entries for update
2. Updating prediction counters based on actual branch outcomes
3. Managing useful bits
4. Allocating new entries on mispredictions

```cpp
void updateWithDirectionEntries(
    const std::vector<DirectionUpdateEntry> &entries,
    const BranchUpdateContext &update_ctx,
    const FetchTarget &stream) {
    Addr startAddr = stream.startPC;
    
    // Get prediction metadata
    auto meta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    
    // Update each branch entry
    for (const auto &update_entry : entries) {
        Addr branch_pc = update_entry.actualBranch.pc;
        bool actual_taken = update_entry.actualBranch.taken;
        auto pred_it = meta->preds.find(branch_pc);
        
        if (pred_it == meta->preds.end()) {
            continue;
        }
        
        // Update predictor state and check for allocation
        bool need_allocate = updatePredictorStateAndCheckAllocation(
            entry, actual_taken, pred_it->second, stream);
        
        if (need_allocate) {
            // Handle useful bit reset
            handleUsefulBitReset(meta->usefulMask);
            
            // Handle allocation of new entries
            uint start_table = 0;
            auto useful_mask = meta->usefulMask;
            if (pred_it->second.mainInfo.found) {
                start_table = pred_it->second.mainInfo.table + 1;
                useful_mask >>= start_table;
                useful_mask.resize(numPredictors - start_table);
            }
            handleNewEntryAllocation(startAddr, entry, actual_taken, 
                                   useful_mask, start_table, meta);
        }
    }
}
```

### 3.4 History Management

The predictor maintains three types of folded histories:

1. Tag folded history: Used for tag computation
2. Alternative tag folded history: Used for alternative tag computation
3. Index folded history: Used for table index computation

These histories are updated speculatively and recovered on mispredictions.

```cpp
void specUpdateGHist(const boost::dynamic_bitset<> &history,
                    FullBTBPrediction &pred,
                    const DirectionHistoryUpdate &update) {
    doUpdateHist(history, update.shamt, update.taken);
}

void recoverHist(const boost::dynamic_bitset<> &history, const FetchTarget &entry, int shamt, bool cond_taken) {
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(
        entry.predMetas[getComponentIdx()]);
    
    // Recover folded histories
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }
    
    // Update histories with correct information
    doUpdateHist(history, shamt, cond_taken);
}
```

## 4. Interaction with BTB

The BTBTAGE predictor works in conjunction with the BTB to provide complete branch prediction:

1. The BTB identifies branches and provides target addresses
2. The BTBTAGE predictor provides direction predictions for conditional branches
3. The two components communicate through the `BTBEntry` structure and the `FullBTBPrediction` structure

The key interactions include:

- The BTB identifies branches and creates `BTBEntry` structures
- The BTBTAGE predictor takes these entries and produces direction predictions
- The predictions are combined in the `FullBTBPrediction` structure
- The combined predictions guide fetch and execution
- Execution results update both predictors

## 5. Integration with Fetch Stream

The BTBTAGE predictor integrates with the fetch stream mechanism through:

1. `putPCHistory`: Makes predictions for a stream of instructions
2. `specUpdateGHist`: Speculatively updates folded history based on explicit history updates
3. `recoverHist`: Recovers folded history state after mispredictions
4. `update`: Updates predictor state based on actual branch outcomes

The fetch stream mechanism provides a unified interface for all branch predictors and manages the flow of predictions and updates.

## 6. Design Considerations

### 6.1 Multiple Tables with Different History Lengths

The TAGE predictor uses multiple tables with different history lengths to capture different correlation patterns. This allows it to adapt to branches with different correlation behaviors.

### 6.2 Useful Bit Management

The useful bit mechanism helps manage the allocation of new entries and the replacement of existing entries. It ensures that valuable entries are preserved and less valuable ones are replaced.

### 6.3 Main and Alternative Predictions

The predictor provides both main and alternative predictions, and uses a mechanism to decide when to use the alternative prediction. This improves accuracy for branches that are difficult to predict.

### 6.4 Predictor Recovery

The predictor maintains metadata for each prediction, which allows it to recover from mispredictions and ensure consistent state.

## 7. Performance Optimizations

### 7.1 Folded History

The predictor uses folded history to reduce the storage requirements for the history registers while maintaining correlation information.

### 7.2 Allocation Strategy

The allocation strategy uses a combination of LFSR-based random allocation and useful bit guidance to efficiently allocate new entries.

### 7.3 Useful Bit Reset

The periodic reset of useful bits prevents the predictor from becoming stuck with outdated entries.

## 8. Testing and Validation

The BTBTAGE predictor includes comprehensive testing capabilities:

1. Basic prediction testing
2. History update verification
3. Table lookup testing
4. Main/alternative prediction selection testing
5. Useful bit mechanism testing

These tests ensure the correct functionality of the predictor and validate its behavior in various scenarios.

## 9. Conclusion

The BTBTAGE predictor is a sophisticated branch direction prediction mechanism that integrates with the BTB to provide accurate branch prediction. Its multiple table design, useful bit management, and history folding techniques make it highly effective at capturing branch behavior patterns and adapting to different types of branches.

## 10. Testing Framework

### 10.1 TAGE Testing Flow

The TAGE predictor follows a specific prediction-update-recovery flow in its testing, which mirrors its actual operation in the processor pipeline:

#### Prediction Phase
```cpp
// Setup BTB entries and history
std::vector<BTBEntry> btbEntries;
boost::dynamic_bitset<> history(64, 0);  // Global history register

// Make prediction
tage->putPCHistory(startPC, history, stagePreds);

// Get prediction metadata (checkpoint state)
auto meta = tage->getPredictionMeta();

// Speculatively update history (folded histories)
auto hist_update = stagePreds[1].getGHistUpdate();
tage->specUpdateGHist(history, stagePreds[1], hist_update);

// Shift actual history register
history <<= shamt;  // Shift by amount corresponding to instructions
history[0] = taken;  // Set lowest bit based on prediction
```

#### Update Phase (Correct Prediction)
When the branch is resolved and found to be correctly predicted:

```cpp
// Setup update stream with actual outcome
FetchTarget stream;
stream.startPC = pc;
stream.predMetas[0] = meta;  // Must include metadata from prediction phase

// Update predictor state through explicit DirectionUpdateEntry inputs.
ResolvedBranch actual_branch;
actual_branch.pc = branch_info.pc;
actual_branch.target = branch_info.target;
actual_branch.taken = actual_taken;
actual_branch.isCond = branch_info.isCond;
actual_branch.isIndirect = branch_info.isIndirect;
actual_branch.isDirect = branch_info.isDirect;
actual_branch.isCall = branch_info.isCall;
actual_branch.isReturn = branch_info.isReturn;
actual_branch.size = branch_info.size;
auto update_ctx = makeBaseBranchUpdateContext(stream);
const std::vector<DirectionUpdateEntry> entries = {
    {actual_branch, actual_taken, false}
};
tage->updateWithDirectionEntries(entries, update_ctx, meta, stream.phistory);
```

#### Recovery Phase (Misprediction)
When a misprediction is detected, history must be recovered:

```cpp
// Setup recovery stream
FetchTarget recoverStream;
recoverStream.startPC = pc;
recoverStream.addResolvedBranch(actual_branch);
recoverStream.predMetas[0] = meta;  // Must include metadata from prediction


history >>= shamt;  // Undo speculative update
// Recover history to checkpoint state, then update with actual outcome
tage->recoverHist(history, recoverStream, shamt, actual_taken);

history <<= shamt;  // Re-shift
history[0] = actual_taken;  // Set with actual outcome
```


## 11. N-Way Set Associative Extension

### 11.1 Overview

The original BTBTAGE design uses directly-mapped (1-way) tables, where each index maps to exactly one entry. This extension modifies the design to use N-way set associative tables, where each index can map to N different entries. This reduces conflicts and potentially improves prediction accuracy.

### 11.2 Key Modifications

#### 11.2.1 Data Structures

```cpp
// Modified TageEntry structure with LRU information
struct TageEntry {
    bool valid;      // Whether this entry is valid
    Addr tag;        // Tag for matching
    short counter;   // Prediction counter (-4 to 3), 3 bits
    bool useful;     // Whether this entry is useful for prediction
    Addr pc;         // Branch PC, for BTB entry PC check
    unsigned lruCounter; // Counter for LRU replacement policy
};

// Modified TageTableInfo with way information
struct TageTableInfo {
    bool found;      // Whether a matching entry was found
    TageEntry entry; // The matching entry
    unsigned table;  // Which table this entry was found in
    Addr index;      // Index in the table
    Addr tag;        // Tag that was matched
    unsigned way;    // Which way this entry was found in
};
```

The main changes to the data structures include:
- Adding an LRU counter to each table entry
- Adding a way field to the TageTableInfo structure
- Changing the tageTable from a 2D array to a 3D array (table × index × way)

#### 11.2.2 Table Organization

In the N-way set associative design:
- The number of sets in each table remains the same (no reduction in index bits)
- Each set contains N ways (entries), resulting in N times the storage capacity
- The total number of entries in each table increases by a factor of N

#### 11.2.3 LRU Replacement Policy

The Least Recently Used (LRU) replacement policy is used to determine which entry to replace when all ways in a set are occupied. Each entry has an LRU counter that is:
- Incremented for all entries in the set when any entry is accessed
- Reset to 0 for the accessed entry
- Used to identify the least recently used entry (highest counter value) for replacement

#### 11.2.4 Lookup Process

The lookup process changes to:
1. Calculate the index for a given PC and table
2. Check all ways at that index for a matching valid entry
3. If a match is found, update LRU counters and return the entry
4. If no match is found, return an invalid entry

#### 11.2.5 Allocation Process

The allocation process changes to:
1. Calculate the index for a given PC and table
2. Check if any way at that index is invalid (not yet used)
3. If an invalid way exists, use it for the new entry
4. If all ways are valid, use the LRU replacement policy to select a victim
5. Update the selected entry with the new information and reset its LRU counter

### 11.3 Implementation Details

#### 11.3.1 LRU Management Methods

```cpp
// Update LRU counters for a set
void updateLRU(int table, Addr index, unsigned way) {
    // Increment LRU counters for all entries in the set
    for (unsigned i = 0; i < numWays; i++) {
        if (i != way && tageTable[table][index][i].valid) {
            tageTable[table][index][i].lruCounter++;
        }
    }
    // Reset LRU counter for the accessed entry
    tageTable[table][index][way].lruCounter = 0;
}

// Find the LRU victim in a set
unsigned getLRUVictim(int table, Addr index) {
    unsigned victim = 0;
    unsigned maxLRU = 0;
    
    // Find the entry with the highest LRU counter
    for (unsigned i = 0; i < numWays; i++) {
        if (!tageTable[table][index][i].valid) {
            return i; // Use invalid entry if available
        }
        if (tageTable[table][index][i].lruCounter > maxLRU) {
            maxLRU = tageTable[table][index][i].lruCounter;
            victim = i;
        }
    }
    return victim;
}
```

#### 11.3.2 Constructor Changes

The constructor is modified to initialize the 3D tageTable and set the default number of ways:

```cpp
BTBTAGE::BTBTAGE() : numPredictors(4), numWays(2), tageStats(4) {
    // ... existing initialization ...
    
    tageTable.resize(numPredictors);
    for (unsigned int i = 0; i < numPredictors; ++i) {
        tageTable[i].resize(tableSizes[i]);
        for (unsigned int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numWays);
        }
        // ... other initializations ...
    }
    
    // ... rest of constructor ...
}
```

#### 11.3.3 Modified Lookup and Update Methods

The lookup and update methods are modified to handle multiple ways:

```cpp
// Look up entries in all TAGE tables
std::vector<BTBTAGE::TageEntry> lookupTageTable(const Addr &startPC) {
    // ... existing code ...
    
    for (int i = 0; i < numPredictors; ++i) {
        Addr index = getTageIndex(startPC, i);
        Addr tag = getTageTag(startPC, i);
        
        // Check all ways for a match
        bool found = false;
        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = tageTable[i][index][way];
            if (entry.valid && matchTag(tag, entry.tag)) {
                entries[i] = entry;
                meta.usefulMask[i] = entry.useful;
                updateLRU(i, index, way);
                found = true;
                break;
            }
        }
        
        // If no match, return invalid entry
        if (!found) {
            entries[i] = TageEntry();
        }
    }
    return entries;
}
```

### 11.4 Expected Benefits

The N-way set associative design is expected to provide several benefits:

1. **Reduced Conflicts**: Multiple entries at the same index reduce conflicts between different branches
2. **Higher Prediction Accuracy**: Fewer conflicts lead to better prediction accuracy
3. **Better Utilization**: LRU replacement ensures that frequently used entries are retained
4. **Scalability**: The design can be easily scaled to different numbers of ways (2-way, 4-way, etc.)

### 11.5 Trade-offs

The N-way set associative design introduces some trade-offs:

1. **Increased Storage**: The storage requirement increases by a factor of N
2. **Higher Complexity**: The lookup and update logic becomes more complex
3. **Additional State**: LRU counters add extra state bits to each entry
4. **Potential Timing Impact**: The more complex lookup may impact timing in hardware implementations

Despite these trade-offs, the N-way set associative design is expected to provide a net benefit in terms of prediction accuracy and overall performance.
