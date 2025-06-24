# MGSC (Multiple GEHL Statistical Corrector) Predictor Documentation

## Overview

The MGSC (Multiple GEHL Statistical Corrector) predictor is a sophisticated statistical corrector component that works alongside TAGE predictors to improve branch prediction accuracy. It implements a multi-component perceptron-based approach using various history patterns to correct TAGE predictions when confidence is low. MGSC is part of the TAGE-SC-L family of predictors, which have won multiple Championship Branch Prediction contests.

## Core Architecture

### Main Class: BTBMGSC

The `BTBMGSC` class extends `TimedBaseBTBPredictor` and implements a statistical corrector with multiple GEHL (GEometric History Length) tables. It operates on the principle that different history patterns can provide valuable information for branch prediction correction, following the research lineage from OGEHL to TAGE to TAGE-SC-L predictors.

## Key Data Structures

### 1. MgscEntry
```cpp
struct MgscEntry {
    bool valid;           // Entry validity flag
    short counter;        // Prediction counter (-32 to 31), 6 bits
    Addr pc;             // Branch PC for tag matching
    unsigned lruCounter; // LRU replacement counter
}
```
- **Purpose**: Basic entry in MGSC prediction tables
- **Counter Range**: -32 to 31 (6-bit signed counter)
- **Usage**: Stores branch bias information for specific PC and history combinations

### 2. MgscWeightEntry
```cpp
struct MgscWeightEntry {
    bool valid;           // Entry validity flag
    Addr pc;             // Branch PC for tag matching  
    short counter;        // Weight counter (-32 to 31), 6 bits
    unsigned lruCounter; // LRU replacement counter
}
```
- **Purpose**: Controls the relative importance of each predictor component
- **Weight Range**: -32 to 31, scaled to [0, 2] multiplier
- **Usage**: Adaptive weighting system to tune predictor importance

### 3. MgscThresEntry
```cpp
struct MgscThresEntry {
    bool valid;           // Entry validity flag
    Addr pc;             // Branch PC for tag matching
    unsigned counter;     // Threshold counter (0 to 255), 8 bits
    unsigned lruCounter; // LRU replacement counter
}
```
- **Purpose**: Stores confidence thresholds for prediction decisions
- **Usage**: Determines when to override TAGE predictions with SC predictions

### 4. MgscPrediction
```cpp
struct MgscPrediction {
    Addr btb_pc;                    // BTB entry PC
    int lsum;                       // Total weighted sum
    bool use_mgsc;                  // Whether to use MGSC prediction
    bool taken;                     // Final prediction = (use sc pred) ? (lsum >= 0) : tage prediction
    bool taken_before_sc;           // Tage prediction (before SC)
    unsigned total_thres;           // Combined threshold
    std::vector<Addr> bwIndex;      // BW table indices
    std::vector<Addr> lIndex;       // L table indices
    std::vector<Addr> iIndex;       // I table indices
    std::vector<Addr> gIndex;       // G table indices
    std::vector<Addr> pIndex;       // P table indices
    std::vector<Addr> biasIndex;    // Bias table indices
    // Weight scale difference flags and percsum values
    bool bw_weight_scale_diff;
    // ... (similar for other components)
    int bw_percsum, l_percsum, i_percsum, g_percsum, p_percsum, bias_percsum;
}
```
- **Purpose**: Complete prediction result containing all component contributions
- **Usage**: Stores intermediate calculations and final prediction decision

## Predictor Components

### 1. Global Backward Branch History Tables (BW)
- **Tables**: `bwTable[bwnb][2^logBwnb][numWays]` (bwnb tables, each with 2^logBwnb sets)
- **Weight Table**: `bwWeightTable[2^logWeightnb][numWays]` (2^logWeightnb sets)
- **History Lengths**: Configurable via `bwm` parameter
- **Purpose**: Captures patterns in backward branch history, exploiting the correlation that backward branches often exhibit predictable patterns

### 2. First Local History Tables (L)
- **Tables**: `lTable[lnb][2^logLnb][numWays]` (lnb tables, each with 2^logLnb sets)
- **Weight Table**: `lWeightTable[2^logWeightnb][numWays]` (2^logWeightnb sets)
- **History Lengths**: Configurable via `lm` parameter
- **Local Histories**: `numEntriesFirstLocalHistories` separate history registers
- **Purpose**: Captures local branch patterns specific to program regions, maintaining per-PC local history

### 3. IMLI (Inner Most Loop Iteration) Tables (I)
- **Tables**: `iTable[inb][2^logInb][numWays]` (inb tables, each with 2^logInb sets)
- **Weight Table**: `iWeightTable[2^logWeightnb][numWays]` (2^logWeightnb sets)
- **History Lengths**: Configurable via `im` parameter
- **Purpose**: Exploits correlation between branch outcomes in multidimensional loops. For branches encapsulated in nested loops, their outcomes correlate with the same branch in neighbor iterations from previous outer loop iterations. This is particularly effective for scientific and multimedia applications with nested loop structures.

### 4. Global History Tables (G)
- **Tables**: `gTable[gnb][2^logGnb][numWays]` (gnb tables, each with 2^logGnb sets)
- **Weight Table**: `gWeightTable[2^logWeightnb][numWays]` (2^logWeightnb sets)
- **History Lengths**: Configurable via `gm` parameter (following geometric series)
- **Purpose**: Captures global branch history patterns using very long histories (hundreds of bits), following the GEHL principle of geometric history lengths

### 5. Path History Tables (P)
- **Tables**: `pTable[pnb][2^logPnb][numWays]` (pnb tables, each with 2^logPnb sets)
- **Weight Table**: `pWeightTable[2^logWeightnb][numWays]` (2^logWeightnb sets)
- **History Lengths**: Configurable via `pm` parameter
- **Purpose**: Captures path-based patterns using encoded branch address information. When branches are taken, specific PC bits (1,3,5,7) are XORed and 2 bits are inserted into the path history, creating a compressed representation of execution paths

### 6. Bias Tables
- **Tables**: `biasTable[biasnb][2^logBiasnb][numWays]` (biasnb tables, each with 2^logBiasnb sets)
- **Weight Table**: `biasWeightTable[2^logWeightnb][numWays]` (2^logWeightnb sets)
- **Purpose**: Provides baseline bias correction based on TAGE confidence levels and alternative prediction differences

### 7. Threshold Tables
- **PC-indexed**: `pUpdateThreshold[2^thresholdTablelogSize][numWays]` (2^thresholdTablelogSize sets)
- **Global**: `updateThreshold[numWays]` (single table with numWays entries)
- **Purpose**: Dynamic threshold adjustment for prediction confidence, adapting when to use SC over TAGE

## Prediction Algorithm

### 1. Index Calculation
```cpp
// Example for backward branch history
for (unsigned int i = 0; i < bwnb; ++i) {
    bwIndex[i] = getHistIndex(startPC, logBwnb, indexBwFoldedHist[i].get());
}
```
- Uses folded history XOR with PC bits for efficient indexing
- Different components use different history types and lengths following GEHL principles

### 2. Perceptron Sum Calculation
```cpp
int calculatePercsum(const std::vector<std::vector<std::vector<MgscEntry>>> &table,
                    const std::vector<Addr> &tableIndices,
                    unsigned numTables, Addr pc) {
    int percsum = 0;
    for (unsigned int i = 0; i < numTables; ++i) {
        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = table[i][tableIndices[i]][way];
            if (tagMatch(pc, entry.pc, 5) && entry.valid) {
                percsum += (2 * entry.counter + 1); // Always positive
                break;
            }
        }
    }
    return percsum;
}
```
- Converts signed counters to positive perceptron values
- Formula: `2 * counter + 1` ensures positive contribution for perceptron computation

### 3. Weight Application
```cpp
int calculateScaledPercsum(int weight, int percsum) {
    return (double)((double)(weight + 32)/32.0) * percsum;
}
```
- Weight range [-32, 31] mapped to multiplier [0, 2]
- Allows adaptive scaling of component importance

### 4. Final Prediction Decision
```cpp
int lsum = bw_scale_percsum + l_scale_percsum + i_scale_percsum +
           g_scale_percsum + p_scale_percsum + bias_scale_percsum;

bool use_sc_pred = false;
if (tage_info.tage_pred_conf_high) {
    if (abs(lsum) > total_thres/2) use_sc_pred = true;
} else if (tage_info.tage_pred_conf_mid) {
    if (abs(lsum) > total_thres/4) use_sc_pred = true;
} else if (tage_info.tage_pred_conf_low) {
    if (abs(lsum) > total_thres/8) use_sc_pred = true;
}

bool taken = (use_sc_pred && enableMGSC) ? (lsum >= 0) : tage_info.tage_pred_taken;
```
- Combines all weighted perceptron sums
- Uses adaptive thresholds based on TAGE confidence levels
- Only overrides TAGE when SC confidence exceeds the dynamic threshold

## Update Algorithm

### 1. Prediction Table Updates
```cpp
void updateAndAllocatePredTable(std::vector<std::vector<std::vector<MgscEntry>>> &table,
                               const std::vector<Addr> &tableIndices,
                               unsigned numTables, Addr pc, bool actual_taken) {
    for (unsigned int i = 0; i < numTables; ++i) {
        bool found_entry = false;
        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = table[i][tableIndices[i]][way];
            if (tagMatch(pc, entry.pc, 5) && entry.valid) {
                updateCounter(actual_taken, scCountersWidth, entry.counter);
                found_entry = true;
                updateLRU(table[i], tableIndices[i], way);
                break;
            }
        }
        if (!found_entry) {
            unsigned alloc_way = getLRUVictim(table[i], tableIndices[i]);
            auto &entry_to_alloc = table[i][tableIndices[i]][alloc_way];
            short newCounter = actual_taken ? 0 : -1;
            entry_to_alloc = MgscEntry(true, newCounter, pc, 0);
        }
    }
}
```
- Updates existing entries or allocates new ones using LRU replacement
- Saturating counter updates based on actual branch outcome
- Initial counter values: 0 for taken, -1 for not-taken

### 2. Weight Table Updates
```cpp
void updateAndAllocateWeightTable(std::vector<std::vector<MgscWeightEntry>> &weightTable,
                                 Addr tableIndex, Addr pc,
                                 bool weight_scale_diff, bool percsum_matches_actual) {
    // Only update if weight scaling affects prediction
    if (weight_scale_diff) {
        updateCounter(percsum_matches_actual, extraWeightsWidth, entry.counter);
    }
}
```
- Updates weights only when they could affect the prediction outcome
- Increases weight if component was correct, decreases if incorrect
- Enables adaptive tuning of component importance

### 3. Threshold Updates
```cpp
void updatePCThresholdTable(Addr tableIndex, Addr pc,
                           bool update_condition, bool update_direction) {
    if (update_condition) { // TAGE and SC disagree
        updateCounter(update_direction, pUpdateThresholdWidth, entry.counter);
    }
}
```
- Updates thresholds when TAGE and SC predictions disagree
- Adjusts confidence requirements based on which predictor was correct

## History Management

### Folded Histories
- **Purpose**: Efficient history indexing using XOR folding technique
- **Types**: Separate folded histories for each component type
- **Update**: Speculative updates during prediction, recovery on misprediction

### History Types
1. **Global History**: Overall branch outcome sequence for capturing general patterns
2. **Global Backward History**: Specifically targets backward branch patterns
3. **Local History**: Per-PC region branch patterns maintained in separate registers
4. **Path History**: Branch target address sequences for path-sensitive prediction
5. **IMLI History**: Inner most loop iteration patterns for multidimensional loops

## Configuration Parameters

### Table Sizes
- `bwnb`, `lnb`, `inb`, `gnb`, `pnb`: Number of tables per component
- `logBwnb`, `logLnb`, etc.: Log2 of table sizes (actual table size = 2^logBwnb entries)
- `logWeightnb`: Log2 of weight table size (actual size = 2^logWeightnb entries)
- `thresholdTablelogSize`: Log2 of threshold table size (actual size = 2^thresholdTablelogSize entries)
- `numWays`: Set associativity (typically 8)

### History Lengths
- `bwm`, `lm`, `im`, `gm`, `pm`: History lengths for each table
- Follows geometric progression principle from GEHL research

### Counter Widths
- `scCountersWidth`: Prediction counter width (6 bits)
- `extraWeightsWidth`: Weight counter width (6 bits)
- `updateThresholdWidth`: Global threshold width (12 bits)
- `pUpdateThresholdWidth`: PC threshold width (8 bits)

## Performance Characteristics

### Advantages
- **High Accuracy**: Multiple complementary predictors capture diverse patterns, building on proven TAGE-SC-L success
- **Adaptive**: Weight and threshold learning adapts to program behavior
- **Research-Proven**: Based on championship-winning predictors (TAGE-SC-L won CBP contests in 2014 and 2016)
- **Multidimensional Loop Support**: IMLI component specifically targets nested loop patterns

### Overhead
- **Storage**: Multiple large tables with set-associative design
- **Latency**: Multi-cycle operation due to complexity (numDelay = 3)
- **Power**: Multiple table accesses and arithmetic operations

## Integration with TAGE

The MGSC predictor works as a statistical corrector to TAGE predictions, following the successful TAGE-SC-L architecture:
1. **Confidence-based**: Only activates when TAGE confidence is questionable
2. **Threshold-adaptive**: Learns appropriate confidence thresholds dynamically
3. **Complementary**: Captures patterns TAGE might miss using multiple history components
4. **Research Heritage**: Builds on the evolution from OGEHL → TAGE → TAGE-SC-L

## Research Background

This implementation is based on André Seznec's branch prediction research at INRIA, particularly the TAGE-SC-L predictor family that achieved state-of-the-art results in Championship Branch Prediction contests. The MGSC component incorporates lessons learned from:

- **GEHL Predictors**: Geometric history length principle for multiple tables
- **TAGE Architecture**: Tagged geometric history length with partial tag matching
- **Statistical Correctors**: Perceptron-like computation for improving TAGE accuracy
- **IMLI Research**: Exploitation of inner most loop iteration patterns for multidimensional loops

This design achieves high accuracy while maintaining the efficiency of the base TAGE predictor for high-confidence predictions.
