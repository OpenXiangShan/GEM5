#include "cpu/pred/btb/btb_mgsc.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/MGSC.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred{

// Constructor: Initialize MGSC predictor with given parameters
BTBMGSC::BTBMGSC(const Params& p):
TimedBaseBTBPredictor(p),
bwnb(p.bwnb),
logBwnb(p.logBwnb),
bwm(p.bwm),
bwWeightInitValue(p.bwWeightInitValue),
numEntriesFirstLocalHistories(p.numEntriesFirstLocalHistories),
lnb(p.lnb),
logLnb(p.logLnb),
lm(p.lm),
lWeightInitValue(p.lWeightInitValue),
inb(p.inb),
logInb(p.logInb),
im(p.im),
iWeightInitValue(p.iWeightInitValue),
gnb(p.gnb),
logGnb(p.logGnb),
gm(p.gm),
gWeightInitValue(p.gWeightInitValue),
pnb(p.pnb),
logPnb(p.logPnb),
pm(p.pm),
pWeightInitValue(p.pWeightInitValue),
biasnb(p.biasnb),
logBiasnb(p.logBiasnb),
scCountersWidth(p.scCountersWidth),
thresholdTablelogSize(p.thresholdTablelogSize),
updateThresholdWidth(p.updateThresholdWidth),
pUpdateThresholdWidth(p.pUpdateThresholdWidth),
initialUpdateThresholdValue(p.initialUpdateThresholdValue),
extraWeightsWidth(p.extraWeightsWidth),
logWeightnb(p.logWeightnb),
numWays(p.numWays),
enableMGSC(p.enableMGSC),
mgscStats(this)
{
    DPRINTF(MGSC, "BTBMGSC constructor\n");
    this->needMoreHistories = p.needMoreHistories;
    bwTable.resize(bwnb);
    for (unsigned int i = 0; i < bwnb; ++i) {
        assert(bwTable.size() >= bwnb);
        bwTable[i].resize(std::pow(2,logBwnb));
        for (unsigned int j = 0; j < (std::pow(2,logBwnb)); ++j) {
            bwTable[i][j].resize(numWays);
        }
        indexBwFoldedHist.push_back(FoldedHist(bwm[i], logBwnb, 16, HistoryType::GLOBALBW));
    }
    bwIndex.resize(bwnb);

    lTable.resize(lnb);
    indexLFoldedHist.resize(numEntriesFirstLocalHistories);
    for (unsigned int i = 0; i < lnb; ++i) {
        assert(lTable.size() >= lnb);
        lTable[i].resize(std::pow(2,logLnb));
        for (unsigned int j = 0; j < (std::pow(2,logLnb)); ++j) {
            lTable[i][j].resize(numWays);
        }
        for (unsigned int k = 0; k < numEntriesFirstLocalHistories; ++k) {
            indexLFoldedHist[k].push_back(FoldedHist(lm[i], logLnb, 16, HistoryType::LOCAL));
        }
    }
    lIndex.resize(lnb);

    iTable.resize(inb);
    for (unsigned int i = 0; i < inb; ++i) {
        assert(iTable.size() >= inb);
        iTable[i].resize(std::pow(2,logInb));
        for (unsigned int j = 0; j < (std::pow(2,logInb)); ++j) {
            iTable[i][j].resize(numWays);
        }
        indexIFoldedHist.push_back(FoldedHist(im[i], logInb, 16, HistoryType::IMLI));
    }
    iIndex.resize(inb);

    gTable.resize(gnb);
    for (unsigned int i = 0; i < gnb; ++i) {
        assert(gTable.size() >= gnb);
        gTable[i].resize(std::pow(2,logGnb));
        for (unsigned int j = 0; j < (std::pow(2,logGnb)); ++j) {
            gTable[i][j].resize(numWays);
        }
        indexGFoldedHist.push_back(FoldedHist(gm[i], logGnb, 16, HistoryType::GLOBAL));
    }
    gIndex.resize(gnb);

    pTable.resize(pnb);
    for (unsigned int i = 0; i < pnb; ++i) {
        assert(pTable.size() >= pnb);
        pTable[i].resize(std::pow(2,logPnb));
        for (unsigned int j = 0; j < (std::pow(2,logPnb)); ++j) {
            pTable[i][j].resize(numWays);
        }
        indexPFoldedHist.push_back(FoldedHist(pm[i], logPnb, 16, HistoryType::PATH));
    }
    pIndex.resize(pnb);

    biasTable.resize(biasnb);
    for (unsigned int i = 0; i < biasnb; ++i) {
        assert(biasTable.size() >= biasnb);
        biasTable[i].resize(std::pow(2,logBiasnb));
        for (unsigned int j = 0; j < (std::pow(2,logBiasnb)); ++j) {
            biasTable[i][j].resize(numWays);
        }
    }
    biasIndex.resize(biasnb);

    bwWeightTable.resize(std::pow(2,logWeightnb));
    for (unsigned int j = 0; j < (std::pow(2,logWeightnb)); ++j) {
        bwWeightTable[j].resize(numWays);
    }

    lWeightTable.resize(std::pow(2,logWeightnb));
    for (unsigned int j = 0; j < (std::pow(2,logWeightnb)); ++j) {
        lWeightTable[j].resize(numWays);
    }

    iWeightTable.resize(std::pow(2,logWeightnb));
    for (unsigned int j = 0; j < (std::pow(2,logWeightnb)); ++j) {
        iWeightTable[j].resize(numWays);
    }

    gWeightTable.resize(std::pow(2,logWeightnb));
    for (unsigned int j = 0; j < (std::pow(2,logWeightnb)); ++j) {
        gWeightTable[j].resize(numWays);
    }

    pWeightTable.resize(std::pow(2,logWeightnb));
    for (unsigned int j = 0; j < (std::pow(2,logWeightnb)); ++j) {
        pWeightTable[j].resize(numWays);
    }

    biasWeightTable.resize(std::pow(2,logWeightnb));
    for (unsigned int j = 0; j < (std::pow(2,logWeightnb)); ++j) {
        biasWeightTable[j].resize(numWays);
    }

    pUpdateThreshold.resize(std::pow(2,thresholdTablelogSize));
    for (unsigned int j = 0; j < (std::pow(2,thresholdTablelogSize)); ++j) {
        pUpdateThreshold[j].resize(numWays);
    }

    updateThreshold.resize(numWays);

}

BTBMGSC::~BTBMGSC()
{
}

// Set up tracing for debugging
void
BTBMGSC::setTrace()
{
}

void
BTBMGSC::tick() {}

void
BTBMGSC::tickStart() {}

bool
BTBMGSC::tagMatch(Addr pc_a, Addr pc_b, unsigned matchBits) {
    // Use bitwise operations directly to avoid overhead of creating bitset objects
    // Create mask: when matchBits=5, mask=0x1F (binary 11111)
    Addr mask = (1ULL << matchBits) - 1;
    return ((pc_a >> instShiftAmt) & mask) == ((pc_b >> instShiftAmt) & mask);
}

/**
 * Calculate perceptron sum from a table for a given PC
 * perceptron sum is the sum of the (2*counter + 1) of the matching entries
 * @param table The table to search in
 * @param tableIndices Indices to use for each table component
 * @param numTables Number of tables to search
 * @param pc PC to match against
 * @return Calculated percsum value
 */
int
BTBMGSC::calculatePercsum(const std::vector<std::vector<std::vector<MgscEntry>>> &table,
                         const std::vector<Addr> &tableIndices,
                         unsigned numTables,
                         Addr pc) {
    int percsum = 0;
    for (unsigned int i = 0; i < numTables; ++i) {
        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = table[i][tableIndices[i]][way];
            if (tagMatch(pc, entry.pc, 5) && entry.valid) {
                percsum += (2 * entry.counter + 1); // 2*counter + 1 is always >= 0
                break;
            }
        }
    }
    return percsum;
}

/**
 * Find weight in a weight table for a given PC
 * @param weightTable The weight table to search
 * @param tableIndex Index to use for the table
 * @param pc PC to match against
 * @return Found weight or 0 if not found
 */
int
BTBMGSC::findWeight(const std::vector<std::vector<MgscWeightEntry>> &weightTable,
                   Addr tableIndex,
                   Addr pc) {
    for (unsigned way = 0; way < numWays; way++) {
        auto &entry = weightTable[tableIndex][way];
        if (tagMatch(pc, entry.pc, 5) && entry.valid) {
            return entry.counter;
        }
    }
    return 0;
}

/**
 * Calculate scaled percsum using weight
 * weight range is [-32, 31], return value range is [0, 2 * percsum]
 * @param weight Weight value
 * @param percsum Original percsum value
 * @return Scaled percsum value
 */
int
BTBMGSC::calculateScaledPercsum(int weight, int percsum) {
    return (double)((double)(weight + 32)/32.0) * percsum;
}

/**
 * Find threshold in a threshold table for a given PC
 * @param thresholdTable The threshold table to search
 * @param tableIndex Index to use for the table
 * @param pc PC to match against
 * @param defaultValue Default value to return if not found
 * @return Found threshold or default value if not found
 */
int
BTBMGSC::findThreshold(const std::vector<std::vector<MgscThresEntry>> &thresholdTable,
                      Addr tableIndex,
                      Addr pc,
                      int defaultValue) {
    for (unsigned way = 0; way < numWays; way++) {
        auto &entry = thresholdTable[tableIndex][way];
        if (tagMatch(pc, entry.pc, 5) && entry.valid) {
            return entry.counter;
        }
    }
    return defaultValue;
}

/**
 * Calculate if weight scale causes prediction difference
 * @param lsum Total weighted sum
 * @param scale_percsum Component's scaled percsum
 * @param percsum Component's raw percsum
 * @return True if weight scale causes prediction to change
 */
bool
BTBMGSC::calculateWeightScaleDiff(int lsum, int scale_percsum, int percsum) {
    // First check if removing this table's contribution keeps the sum positive (predict taken)
    // Then check if doubling this table's contribution keeps the sum positive
    // If one is true and the other is false, the table's weight is crucial for prediction
    return ((lsum - scale_percsum) >= 0) != ((lsum - scale_percsum + 2*percsum) >= 0);
}

/**
 * @brief Generate prediction for a single BTB entry by searching MGSC tables
 *
 * @param btb_entry The BTB entry to generate prediction for
 * @param startPC The starting PC address for calculating indices and tags
 * @return TagePrediction containing main and alternative predictions
 */
BTBMGSC::MgscPrediction
BTBMGSC::generateSinglePrediction(const BTBEntry &btb_entry,
                                 const Addr &startPC,
                                 const TageInfoForMGSC &tage_info) {
    DPRINTF(MGSC, "generateSinglePrediction for btbEntry: %#lx, always taken %d\n",
        btb_entry.pc, btb_entry.alwaysTaken);

    // Calculate indices for all tables
    for (unsigned int i = 0; i < bwnb; ++i) {
        bwIndex[i] = getHistIndex(startPC, logBwnb, indexBwFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < lnb; ++i) {
        lIndex[i] = getHistIndex(startPC, logLnb,
                  indexLFoldedHist[getPcIndex(startPC, log2(numEntriesFirstLocalHistories))][i].get());
    }

    for (unsigned int i = 0; i < inb; ++i) {
        iIndex[i] = getHistIndex(startPC, logInb, indexIFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < gnb; ++i) {
        gIndex[i] = getHistIndex(startPC, logGnb, indexGFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < pnb; ++i) {
        pIndex[i] = getHistIndex(startPC, logPnb, indexPFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < biasnb; ++i) {
        biasIndex[i] = getBiasIndex(startPC, logBiasnb, tage_info.tage_pred_taken,
                    tage_info.tage_pred_conf_low && tage_info.tage_pred_alt_diff);
    }

    // Calculate percsums and weights for all tables
    Addr tableIndex = getPcIndex(startPC, logWeightnb);

    int bw_percsum = calculatePercsum(bwTable, bwIndex, bwnb, btb_entry.pc);
    int bw_weight = findWeight(bwWeightTable, tableIndex, btb_entry.pc);
    int bw_scale_percsum = calculateScaledPercsum(bw_weight, bw_percsum);

    int l_percsum = calculatePercsum(lTable, lIndex, lnb, btb_entry.pc);
    int l_weight = findWeight(lWeightTable, tableIndex, btb_entry.pc);
    int l_scale_percsum = calculateScaledPercsum(l_weight, l_percsum);

    int i_percsum = calculatePercsum(iTable, iIndex, inb, btb_entry.pc);
    int i_weight = findWeight(iWeightTable, tableIndex, btb_entry.pc);
    int i_scale_percsum = calculateScaledPercsum(i_weight, i_percsum);

    int g_percsum = calculatePercsum(gTable, gIndex, gnb, btb_entry.pc);
    int g_weight = findWeight(gWeightTable, tableIndex, btb_entry.pc);
    int g_scale_percsum = calculateScaledPercsum(g_weight, g_percsum);

    int p_percsum = calculatePercsum(pTable, pIndex, pnb, btb_entry.pc);
    int p_weight = findWeight(pWeightTable, tableIndex, btb_entry.pc);
    int p_scale_percsum = calculateScaledPercsum(p_weight, p_percsum);

    int bias_percsum = calculatePercsum(biasTable, biasIndex, biasnb, btb_entry.pc);
    int bias_weight = findWeight(biasWeightTable, tableIndex, btb_entry.pc);
    int bias_scale_percsum = calculateScaledPercsum(bias_weight, bias_percsum);

    // Calculate total sum of all weighted percsums
    int lsum = bw_scale_percsum + l_scale_percsum + i_scale_percsum +
               g_scale_percsum + p_scale_percsum + bias_scale_percsum;

    // Find thresholds
    // pc-indexed threshold table, default value = initialUpdateThresholdValue = 0
    int p_update_thres = findThreshold(pUpdateThreshold,
                                     getPcIndex(startPC, thresholdTablelogSize),
                                     btb_entry.pc,
                                     initialUpdateThresholdValue);

    // global threshold table
    int update_thres = 35 << 3;     // default value = 35 << 3 ?
    for (unsigned way = 0; way < numWays; way++) {
        auto &entry = updateThreshold[way];
        if (tagMatch(btb_entry.pc, entry.pc, 5) && entry.valid) {
            update_thres = entry.counter;
            break;
        }
    }

    int total_thres = (update_thres >> 3) + p_update_thres;  // total_thres = global_thres + pc_thres

    // Determine whether to use SC prediction based on confidence levels
    bool use_sc_pred = false;
    if (tage_info.tage_pred_conf_high) {
        if (abs(lsum) > total_thres/2) {
            use_sc_pred = true;
        }
    } else if (tage_info.tage_pred_conf_mid) {
        if (abs(lsum) > total_thres/4) {
            use_sc_pred = true;
        }
    } else if (tage_info.tage_pred_conf_low) {
        if (abs(lsum) > total_thres/8) {
            use_sc_pred = true;
        }
    }

    // Final prediction
    bool taken = (use_sc_pred && enableMGSC) ? (lsum >= 0) : tage_info.tage_pred_taken;  // lsum >= 0 means taken if use_sc_pred

    // Calculate weight scale differences
    bool bw_weight_scale_diff = calculateWeightScaleDiff(lsum, bw_scale_percsum, bw_percsum);
    bool l_weight_scale_diff = calculateWeightScaleDiff(lsum, l_scale_percsum, l_percsum);
    bool i_weight_scale_diff = calculateWeightScaleDiff(lsum, i_scale_percsum, i_percsum);
    bool g_weight_scale_diff = calculateWeightScaleDiff(lsum, g_scale_percsum, g_percsum);
    bool p_weight_scale_diff = calculateWeightScaleDiff(lsum, p_scale_percsum, p_percsum);
    bool bias_weight_scale_diff = calculateWeightScaleDiff(lsum, bias_scale_percsum, bias_percsum);

    DPRINTF(MGSC, "sc predict %#lx taken %d\n", btb_entry.pc, taken);

    return MgscPrediction(btb_entry.pc, lsum, use_sc_pred, taken,
                        tage_info.tage_pred_taken, total_thres,
                        bwIndex, lIndex, iIndex, gIndex, pIndex, biasIndex,
                        bw_weight_scale_diff, l_weight_scale_diff, i_weight_scale_diff,
                        g_weight_scale_diff, p_weight_scale_diff, bias_weight_scale_diff,
                        bw_percsum, l_percsum, i_percsum, g_percsum, p_percsum, bias_percsum);
}

/**
 * @brief Look up predictions in MGSC tables for a stream of instructions
 *
 * @param startPC The starting PC address for the instruction stream
 * @param btbEntries Vector of BTB entries to make predictions for
 * @return Map of branch PC addresses to their predicted outcomes
 */
void
BTBMGSC::lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
                      const std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs, CondTakens& results)
{
    DPRINTF(MGSC, "lookupHelper startAddr: %#lx\n", startPC);

    // Process each BTB entry to make predictions
    for (auto &btb_entry : btbEntries) {
        // Only predict for valid conditional branches
        if (btb_entry.isCond && btb_entry.valid) {
            auto tage_info = tageInfoForMgscs.find(btb_entry.pc);
            if(tage_info != tageInfoForMgscs.end()){
                auto pred = generateSinglePrediction(btb_entry, startPC, tage_info->second);
                meta->preds[btb_entry.pc] = pred;
                results.push_back({btb_entry.pc, pred.taken || btb_entry.alwaysTaken});
            } else {
                assert(false);
            }
        }
    }
}

/**
 * @brief Makes predictions for a stream of instructions using TAGE predictor
 *
 * This function is called during the prediction stage and:
 * 1. Uses lookupHelper to get predictions for all BTB entries
 * 2. Stores predictions in the stage prediction structure
 * 3. Handles multiple prediction stages with different delays
 *
 * @param stream_start Starting PC of the instruction stream
 * @param history Current branch history
 * @param stagePreds Vector of predictions for different pipeline stages
 */
void
BTBMGSC::putPCHistory(Addr stream_start, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) {
    DPRINTF(MGSC, "putPCHistory startAddr: %#lx\n", stream_start);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it

    // Clear old prediction metadata and save current history state
    meta = std::make_shared<MgscMeta>();
    meta->indexBwFoldedHist = indexBwFoldedHist;
    meta->indexLFoldedHist = indexLFoldedHist;
    meta->indexIFoldedHist = indexIFoldedHist;
    meta->indexGFoldedHist = indexGFoldedHist;
    meta->indexPFoldedHist = indexPFoldedHist;

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        stage_pred.condTakens.clear();
        lookupHelper(stream_start, stage_pred.btbEntries, stage_pred.tageInfoForMgscs, stage_pred.condTakens);
    }

}

std::shared_ptr<void>
BTBMGSC::getPredictionMeta() {
    return meta;
}

/**
 * @brief Prepare BTB entries for update by filtering and processing
 *
 * @param stream The fetch stream containing update information
 * @return Vector of BTB entries that need to be updated
 */
std::vector<BTBEntry>
BTBMGSC::prepareUpdateEntries(const FetchStream &stream) {
    auto all_entries = stream.updateBTBEntries;

    // Filter out non-conditional and always-taken branches
    auto remove_it = std::remove_if(all_entries.begin(), all_entries.end(),
        [](const BTBEntry &e) { return !e.isCond && !e.alwaysTaken; });
    all_entries.erase(remove_it, all_entries.end());

    // Handle potential new BTB entry
    auto &potential_new_entry = stream.updateNewBTBEntry;
    if (!stream.updateIsOldEntry && potential_new_entry.isCond &&
        !potential_new_entry.alwaysTaken) {
        all_entries.push_back(potential_new_entry);
    }

    return all_entries;
}

/**
 * Update a prediction table and allocate new entry if needed
 *
 * This function handles the main perceptron tables (bwTable, lTable, iTable, gTable, pTable, biasTable)
 * which store counter values that contribute to the final prediction. These tables:
 * - Are organized as [numTables][tableIndices][numWays]
 * - Store signed counters (-32 to 31) representing branch bias
 * - Are updated for each branch outcome
 * - Start with 0 for taken branches and -1 for not-taken branches when newly allocated
 *
 * @param table The table to update (one of the six main prediction tables)
 * @param tableIndices Indices for each component of the table, derived from history hashing
 * @param numTables Number of tables in this category (e.g., bwnb, lnb, etc.)
 * @param pc PC to match against for finding the right entry
 * @param actual_taken Actual branch outcome (true=taken, false=not taken)
 */
void
BTBMGSC::updateAndAllocatePredTable(std::vector<std::vector<std::vector<MgscEntry>>> &table,
                                  const std::vector<Addr> &tableIndices,
                                  unsigned numTables,
                                  Addr pc,
                                  bool actual_taken) {
    for (unsigned int i = 0; i < numTables; ++i) {
        bool found_entry = false;
        // Search all ways in the set for a matching entry
        for (unsigned way = 0; way < numWays; way++) {
            auto &entry = table[i][tableIndices[i]][way];
            if (tagMatch(pc, entry.pc, 5) && entry.valid) {
                // Entry found - update its counter based on branch outcome
                updateCounter(actual_taken, scCountersWidth, entry.counter);
                found_entry = true;
                updateLRU(table[i], tableIndices[i], way);
                break;
            }
        }
        // Allocate if not found - use LRU replacement policy
        if (!found_entry) {
            unsigned alloc_way = getLRUVictim(table[i], tableIndices[i]);
            auto &entry_to_alloc = table[i][tableIndices[i]][alloc_way];
            // Initialize counter based on branch outcome
            short newCounter = actual_taken ? 0 : -1;
            entry_to_alloc = MgscEntry(true, newCounter, pc, 0);
        }
    }
}

/**
 * Update a weight table and allocate new entry if needed
 *
 * This function handles the weight tables (bwWeightTable, lWeightTable, etc.) which
 * determine the relative importance of each predictor type. These tables:
 * - Are organized as [tableIndex][numWays]
 * - Store weights that scale the importance of each predictor component
 * - Are only updated when the weight could have affected the outcome (weight_scale_diff)
 * - Are initialized to 0 when newly allocated
 * - Allow adaptive tuning of the prediction mechanism
 *
 * @param weightTable The weight table to update
 * @param tableIndex Index to use for the table (typically derived from PC)
 * @param pc PC to match against for finding the right entry
 * @param weight_scale_diff Whether weight scaling affects prediction outcome
 * @param percsum_matches_actual Whether the raw percsum correctly predicted the outcome
 */
void
BTBMGSC::updateAndAllocateWeightTable(std::vector<std::vector<MgscWeightEntry>> &weightTable,
                                    Addr tableIndex,
                                    Addr pc,
                                    bool weight_scale_diff,
                                    bool percsum_matches_actual) {
    bool found_entry = false;
    // Search all ways in the set for a matching entry
    for (unsigned way = 0; way < numWays; way++) {
        auto &entry = weightTable[tableIndex][way];
        if (tagMatch(pc, entry.pc, 5) && entry.valid) {
            // Only update if weight scale could affect prediction
            if (weight_scale_diff) {
                // Increase weight if percsum was correct, decrease if incorrect
                updateCounter(percsum_matches_actual, extraWeightsWidth, entry.counter);
            }
            found_entry = true;
            updateLRU(weightTable, tableIndex, way);
            break;
        }
    }

    // Allocate if not found - use LRU replacement policy
    if (!found_entry) {
        unsigned alloc_way = getLRUVictim(weightTable, tableIndex);
        auto &entry_to_alloc = weightTable[tableIndex][alloc_way];
        // Initialize weight to neutral value (0)
        entry_to_alloc = MgscWeightEntry(true, pc, 0, 0);
    }
}

/**
 * Update a threshold table and allocate new entry if needed
 *
 * This function handles threshold tables (pUpdateThreshold) which determine
 * when to use statistical correction over TAGE. These tables:
 * - Are organized as [tableIndex][numWays]
 * - Store unsigned threshold values
 * - Are only updated when there's a disagreement between TAGE and SC predictions
 * - Control the confidence level required to override TAGE prediction
 * - Are initialized to a default value when newly allocated
 *
 * @param tableIndex Index to use for the table (typically derived from PC)
 * @param pc PC to match against for finding the right entry
 * @param update_condition Whether to update the counter (typically when TAGE and SC disagree)
 * @param update_direction Direction to update (true=increment, false=decrement)
 */
void
BTBMGSC::updatePCThresholdTable(Addr tableIndex,
                                Addr pc,
                                bool update_condition,
                                bool update_direction) {
    bool found_entry = false;
    // Search all ways in the set for a matching entry
    for (unsigned way = 0; way < numWays; way++) {
        auto &entry = pUpdateThreshold[tableIndex][way];
        if (tagMatch(pc, entry.pc, 5) && entry.valid) {
            // Only update if the update condition is met (TAGE and SC disagree)
            if (update_condition) {
                // Adjust threshold based on which prediction was correct
                updateCounter(update_direction, pUpdateThresholdWidth, entry.counter);
            }
            found_entry = true;
            updateLRU(pUpdateThreshold, tableIndex, way);
            break;
        }
    }

    // Allocate if not found - use LRU replacement policy
    if (!found_entry) {
        unsigned alloc_way = getLRUVictim(pUpdateThreshold, tableIndex);
        auto &entry_to_alloc = pUpdateThreshold[tableIndex][alloc_way];
        // Initialize with default value from class member
        entry_to_alloc = MgscThresEntry(true, pc, initialUpdateThresholdValue, 0);
    }
}

/**
 * Update the global threshold table and allocate new entry if needed
 *
 * This function handles the global threshold table (updateThreshold) which is
 * structured differently than other threshold tables:
 * - It's a one-dimensional array of entries
 * - It stores a global threshold value that applies across many branches
 * - It's updated when TAGE and SC predictions disagree
 *
 * @param pc PC to match against for finding the right entry
 * @param update_condition Whether to update the counter (typically when TAGE and SC disagree)
 * @param update_direction Direction to update (true=increment, false=decrement)
 */
void
BTBMGSC::updateGlobalThreshold(Addr pc,
                             bool update_condition,
                             bool update_direction) {
    bool found_entry = false;
    // Search all ways for a matching entry
    for (unsigned way = 0; way < numWays; way++) {
        auto &entry = updateThreshold[way];
        if (tagMatch(pc, entry.pc, 5) && entry.valid) {
            // Only update if the update condition is met
            if (update_condition) {
                updateCounter(update_direction, updateThresholdWidth, entry.counter);
            }
            found_entry = true;
            updateLRU(updateThreshold, way);
            break;
        }
    }

    // Allocate if not found - use LRU replacement policy
    if (!found_entry) {
        unsigned alloc_way = getLRUVictim(updateThreshold);
        auto &entry_to_alloc = updateThreshold[alloc_way];
        // Initialize with default hard-coded value (35 << 3)
        entry_to_alloc = MgscThresEntry(true, pc, 35 << 3, 0);
    }
}

/**
 * @brief Update predictor for a single entry and allocate new entries if needed
 *
 * This function updates the MGSC predictor state based on the actual branch outcome
 * and allocates new entries in various tables if they don't already exist.
 *
 * @param entry The BTB entry being updated
 * @param actual_taken The actual outcome of the branch
 * @param pred The prediction made for this entry
 * @param stream The fetch stream containing update information
 */
void
BTBMGSC::updateAndAllocateSinglePredictor(const BTBEntry &entry,
                             bool actual_taken,
                             const MgscPrediction &pred,
                             const FetchStream &stream) {
    // Extract prediction information
    auto lsum = pred.lsum;
    auto use_mgsc = pred.use_mgsc;
    auto total_thres = pred.total_thres;
    auto sc_pred_taken = lsum >= 0;
    auto tage_pred_taken = pred.taken_before_sc;    // tage predictions

    // Update statistics
    if (use_mgsc) {
        mgscStats.scUsed++;
        if (sc_pred_taken == actual_taken && tage_pred_taken != actual_taken) {
            mgscStats.scCorrectTageWrong++;
        } else if (sc_pred_taken != actual_taken && tage_pred_taken == actual_taken) {
            mgscStats.scWrongTageCorrect++;
        } else if (sc_pred_taken == actual_taken && tage_pred_taken == actual_taken) {
            mgscStats.scCorrectTageCorrect++;
        } else if (sc_pred_taken != actual_taken && tage_pred_taken != actual_taken) {
            mgscStats.scWrongTageWrong++;
        }
    } else {
        mgscStats.scNotUsed++;  // sc confidence is low
    }

    // Only update tables if prediction was wrong or confidence was low
    if (sc_pred_taken != actual_taken || abs(lsum) < total_thres) {
        // PC index for weight tables
        Addr pcIndex = getPcIndex(stream.startPC, logWeightnb);

        // Update BW tables
        updateAndAllocatePredTable(bwTable, pred.bwIndex, bwnb, entry.pc, actual_taken);
        updateAndAllocateWeightTable(
            bwWeightTable, pcIndex, entry.pc, pred.bw_weight_scale_diff,
            (pred.bw_percsum >= 0) == actual_taken);

        // Update L tables
        updateAndAllocatePredTable(
            lTable, pred.lIndex, lnb, entry.pc, actual_taken);
        updateAndAllocateWeightTable(
            lWeightTable, pcIndex, entry.pc, pred.l_weight_scale_diff,
            (pred.l_percsum >= 0) == actual_taken);

        // Update I tables
        updateAndAllocatePredTable(
            iTable, pred.iIndex, inb, entry.pc, actual_taken);
        updateAndAllocateWeightTable(
            iWeightTable, pcIndex, entry.pc, pred.i_weight_scale_diff,
            (pred.i_percsum >= 0) == actual_taken);

        // Update G tables
        updateAndAllocatePredTable(
            gTable, pred.gIndex, gnb, entry.pc, actual_taken);
        updateAndAllocateWeightTable(
            gWeightTable, pcIndex, entry.pc, pred.g_weight_scale_diff,
            (pred.g_percsum >= 0) == actual_taken);

        // Update P tables
        updateAndAllocatePredTable(
            pTable, pred.pIndex, pnb, entry.pc, actual_taken);
        updateAndAllocateWeightTable(
            pWeightTable, pcIndex, entry.pc, pred.p_weight_scale_diff,
            (pred.p_percsum >= 0) == actual_taken);

        // Update bias tables
        updateAndAllocatePredTable(
            biasTable, pred.biasIndex, biasnb, entry.pc, actual_taken);
        updateAndAllocateWeightTable(
            biasWeightTable, pcIndex, entry.pc, pred.bias_weight_scale_diff,
            (pred.bias_percsum >= 0) == actual_taken);

        // Update PC-indexed threshold table
        updatePCThresholdTable(getPcIndex(stream.startPC, thresholdTablelogSize),
            entry.pc, tage_pred_taken != sc_pred_taken, sc_pred_taken != actual_taken);

        // Update global threshold table
        updateGlobalThreshold(entry.pc, tage_pred_taken != sc_pred_taken,
                             sc_pred_taken != actual_taken);
    }
}

void
BTBMGSC::update(const FetchStream &stream) {
    Addr startAddr = stream.getRealStartPC();
    DPRINTF(MGSC, "update startAddr: %#lx\n", startAddr);

    // Prepare BTB entries to update
    auto entries_to_update = prepareUpdateEntries(stream);

    // Get prediction metadata
    auto meta = std::static_pointer_cast<MgscMeta>(stream.predMetas[getComponentIdx()]);
    auto &preds = meta->preds;

    // Process each BTB entry
    for (auto &btb_entry : entries_to_update) {
        bool actual_taken = stream.exeTaken && stream.exeBranchInfo == btb_entry;
        auto pred_it = preds.find(btb_entry.pc);

        if (pred_it == preds.end()) {
            continue;
        }

        // Update predictor state and check if need to allocate new entry
        updateAndAllocateSinglePredictor(btb_entry, actual_taken, pred_it->second, stream);
    }

    DPRINTF(MGSC, "end update\n");
}

// Update signed counter with saturation
void
BTBMGSC::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width-1)) - 1;
    int min = -(1 << (width-1));
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

// Update unsigned counter with saturation
void
BTBMGSC::updateCounter(bool taken, unsigned width, unsigned &counter) {
    int max = (1 << width) - 1;
    int min = 0;
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}


Addr
BTBMGSC::getHistIndex(Addr pc, unsigned tableIndexBits, bitset &foldedHist)
{
    // Create mask to limit result size to tableIndexBits
    Addr mask = (1ULL << tableIndexBits) - 1;

    // Extract lower bits of PC and XOR with folded history directly
    Addr pcBits = (pc >> floorLog2(blockSize)) & mask;
    Addr foldedBits = foldedHist.to_ulong() & mask;

    return pcBits ^ foldedBits;
}

Addr
BTBMGSC::getBiasIndex(Addr pc, unsigned tableIndexBits, bool lowbit0, bool lowbit1)
{
    // Create mask for tableIndexBits-2 to extract PC bits
    Addr mask = (1ULL << (tableIndexBits - 2)) - 1;

    // Extract lower bits of PC directly and combine with low bits
    Addr pcBits = (pc >> floorLog2(blockSize)) & mask;
    unsigned index = (pcBits << 2) + (lowbit1 << 1) + lowbit0;
    return index;
}

Addr
BTBMGSC::getPcIndex(Addr pc, unsigned tableIndexBits)
{
    // Create mask to extract tableIndexBits from PC
    Addr mask = (1ULL << tableIndexBits) - 1;

    // Extract lower bits of PC directly without bitset
    return (pc >> floorLog2(blockSize)) & mask;
}

bool
BTBMGSC::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
BTBMGSC::satIncrement(int max, unsigned &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
BTBMGSC::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

bool
BTBMGSC::satDecrement(int min, unsigned &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

/**
 * @brief Updates branch history for speculative execution
 *
 * This function updates three types of folded histories:
 * - Tag folded history: Used for tag computation
 * - Alternative tag folded history: Used for alternative tag computation
 * - Index folded history: Used for table index computation
 *
 * @param history The current branch history
 * @param shamt The number of bits to shift
 * @param taken Whether the branch was taken
 */
void
BTBMGSC::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt,
                        bool taken, std::vector<FoldedHist> &foldedHist, Addr pc)
{
    if (debugFlagOn) {
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(MGSC, "in doUpdateHist, shamt %d, taken %d, history %s\n",
                shamt, taken, buf.c_str());
    }
    if (shamt == 0) {
        DPRINTF(MGSC, "shamt is 0, returning\n");
        return;
    }

    for (int t = 0; t < foldedHist.size(); t++) {
        foldedHist[t].update(history, shamt, taken, pc);
    }
}


/**
 * @brief Updates branch history for speculative execution
 *
 * This function updates the branch history for speculative execution
 * based on the provided history and prediction information.
 *
 * It first retrieves the history information from the prediction metadata
 * and then calls the doUpdateHist function to update the folded histories.
 *
 * @param history The current branch history
 * @param pred The prediction metadata containing history information
 */
void
BTBMGSC::specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getHistInfo();
    doUpdateHist(history, shamt, cond_taken, indexGFoldedHist); // use global history to update G folded history
}

/**
 * @brief Updates branch history for speculative execution
 *
 * This function updates the branch history for speculative execution
 * based on the provided history and prediction information.
 *
 * It first retrieves the history information from the prediction metadata
 * and then calls the doUpdateHist function to update the folded histories.
 *
 * @param history The current branch history
 * @param pred The prediction metadata containing history information
 */
void
BTBMGSC::specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    Addr pc;
    bool cond_taken;
    std::tie(pc, cond_taken) = pred.getPHistInfo();
    doUpdateHist(history, 1, cond_taken, indexPFoldedHist, pc); // only path history needs pc!
}


/**
 * @brief Updates global backward branch history for speculative execution
 *
 * This function updates the branch history for speculative execution
 * based on the provided history and prediction information.
 *
 * It first retrieves the history information from the prediction metadata
 * and then calls the doUpdateHist function to update the folded histories.
 *
 * @param history The current global backward branch history
 * @param pred The prediction metadata containing history information
 */
void
BTBMGSC::specUpdateBwHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getBwHistInfo();
    doUpdateHist(history, shamt, cond_taken, indexBwFoldedHist);
}

/**
 * @brief Updates IMLI branch history for speculative execution
 *
 * This function updates the branch history for speculative execution
 * based on the provided history and prediction information.
 *
 * It first retrieves the history information from the prediction metadata
 * and then calls the doUpdateHist function to update the folded histories.
 *
 * @param history The current imli branch history
 * @param pred The prediction metadata containing history information
 */
void
BTBMGSC::specUpdateIHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getBwHistInfo();
    doUpdateHist(history, shamt, cond_taken, indexIFoldedHist);
}

/**
 * @brief Updates local branch history for speculative execution
 *
 * This function updates the branch history for speculative execution
 * based on the provided history and prediction information.
 *
 * It first retrieves the history information from the prediction metadata
 * and then calls the doUpdateHist function to update the folded histories.
 *
 * @param history The current local branch history
 * @param pred The prediction metadata containing history information
 */
void
BTBMGSC::specUpdateLHist(const std::vector<boost::dynamic_bitset<>> &history, FullBTBPrediction &pred)
{
    int shamt;
    bool cond_taken;
    std::tie(shamt, cond_taken) = pred.getHistInfo();
    doUpdateHist(history[getPcIndex(pred.bbStart, log2(numEntriesFirstLocalHistories))],
                 shamt, cond_taken,
                 indexLFoldedHist[getPcIndex(pred.bbStart, log2(numEntriesFirstLocalHistories))]);
}

/**
 * @brief Recovers branch global history state after a misprediction
 *
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 *
 * @param history The branch history to recover to
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBMGSC::recoverHist(const boost::dynamic_bitset<> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < gnb; i++) {
        indexGFoldedHist[i].recover(predMeta->indexGFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken, indexGFoldedHist);
}

/**
 * @brief Recovers branch path history state after a misprediction
 *
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 *
 * @param history The branch history to recover to
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBMGSC::recoverPHist(const boost::dynamic_bitset<> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < pnb; i++) {
        indexPFoldedHist[i].recover(predMeta->indexPFoldedHist[i]);
    }
    doUpdateHist(history, 1, cond_taken, indexPFoldedHist, entry.getControlPC());
}

/**
 * @brief Recovers branch global backward history state after a misprediction
 *
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 *
 * @param history The branch history to recover to
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBMGSC::recoverBwHist(const boost::dynamic_bitset<> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < bwnb; i++) {
        indexBwFoldedHist[i].recover(predMeta->indexBwFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken, indexBwFoldedHist);
}

/**
 * @brief Recovers branch imli history state after a misprediction
 *
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 *
 * @param history The branch history to recover to
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBMGSC::recoverIHist(const boost::dynamic_bitset<> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < inb; i++) {
        indexIFoldedHist[i].recover(predMeta->indexIFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken, indexIFoldedHist);
}

/**
 * @brief Recovers branch local history state after a misprediction
 *
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 *
 * @param history The branch history to recover to
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBMGSC::recoverLHist(const std::vector<boost::dynamic_bitset<>> &history,
    const FetchStream &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (unsigned int k = 0; k < numEntriesFirstLocalHistories; ++k) {
        for (int i = 0; i < lnb; i++) {
            indexLFoldedHist[k][i].recover(predMeta->indexLFoldedHist[k][i]);
        }
    }
    doUpdateHist(history[getPcIndex(entry.startPC, log2(numEntriesFirstLocalHistories))], shamt, cond_taken,
                indexLFoldedHist[getPcIndex(entry.startPC, log2(numEntriesFirstLocalHistories))]);
}

// Constructor for TAGE statistics
BTBMGSC::MgscStats::MgscStats(statistics::Group* parent):
    statistics::Group(parent),
     ADD_STAT(scCorrectTageWrong, statistics::units::Count::get(), "number of sc predict correct and tage predict wrong"),
     ADD_STAT(scWrongTageCorrect, statistics::units::Count::get(), "number of sc predict wrong and tage predict correct"),
     ADD_STAT(scCorrectTageCorrect, statistics::units::Count::get(), "number of sc predict correct and tage predict correct"),
     ADD_STAT(scWrongTageWrong, statistics::units::Count::get(), "number of sc predict wrong and tage predict wrong"),
     ADD_STAT(scUsed, statistics::units::Count::get(), "number of sc used"),
     ADD_STAT(scNotUsed, statistics::units::Count::get(), "number of sc not used")
{
}

// Update LRU counters for a set
template <typename T>
void
BTBMGSC::updateLRU(std::vector<std::vector<T>> &table, Addr index, unsigned way)
{
    // Increment LRU counters for all entries in the set
    for (unsigned i = 0; i < numWays; i++) {
        if (i != way && table[index][i].valid) {
            table[index][i].lruCounter++;
        }
    }
    // Reset LRU counter for the accessed entry
    table[index][way].lruCounter = 0;
}

template <typename T>
void
BTBMGSC::updateLRU(std::vector<T> &table, unsigned way)
{
    // Increment LRU counters for all entries in the set
    for (unsigned i = 0; i < numWays; i++) {
        if (i != way && table[i].valid) {
            table[i].lruCounter++;
        }
    }
    // Reset LRU counter for the accessed entry
    table[way].lruCounter = 0;
}

// Find the LRU victim in a set
template <typename T>
unsigned
BTBMGSC::getLRUVictim(std::vector<std::vector<T>> &table, Addr index)
{
    unsigned victim = 0;
    unsigned maxLRU = 0;

    // Find the entry with the highest LRU counter
    for (unsigned i = 0; i < numWays; i++) {
        if (!table[index][i].valid) {
            return i; // Use invalid entry if available
        }
        if (table[index][i].lruCounter > maxLRU) {
            maxLRU = table[index][i].lruCounter;
            victim = i;
        }
    }
    return victim;
}

template <typename T>
unsigned
BTBMGSC::getLRUVictim(std::vector<T> &table)
{
    unsigned victim = 0;
    unsigned maxLRU = 0;

    // Find the entry with the highest LRU counter
    for (unsigned i = 0; i < numWays; i++) {
        if (!table[i].valid) {
            return i; // Use invalid entry if available
        }
        if (table[i].lruCounter > maxLRU) {
            maxLRU = table[i].lruCounter;
            victim = i;
        }
    }
    return victim;
}

void
BTBMGSC::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
}

} // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
