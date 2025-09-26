#include "cpu/pred/btb/btb_mgsc.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <type_traits>

#include "debug/MGSC.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

// Constructor: Initialize MGSC predictor with given parameters
BTBMGSC::BTBMGSC(const Params &p)
    : TimedBaseBTBPredictor(p),
      bwTableNum(p.bwTableNum),
      bwTableIdxWidth(p.bwTableIdxWidth),
      bwHistLen(p.bwHistLen),
      numEntriesFirstLocalHistories(p.numEntriesFirstLocalHistories),
      lTableNum(p.lTableNum),
      lTableIdxWidth(p.lTableIdxWidth),
      lHistLen(p.lHistLen),
      iTableNum(p.iTableNum),
      iTableIdxWidth(p.iTableIdxWidth),
      iHistLen(p.iHistLen),
      gTableNum(p.gTableNum),
      gTableIdxWidth(p.gTableIdxWidth),
      gHistLen(p.gHistLen),
      pTableNum(p.pTableNum),
      pTableIdxWidth(p.pTableIdxWidth),
      pHistLen(p.pHistLen),
      biasTableNum(p.biasTableNum),
      biasTableIdxWidth(p.biasTableIdxWidth),
      scCountersWidth(p.scCountersWidth),
      thresholdTablelogSize(p.thresholdTablelogSize),
      updateThresholdWidth(p.updateThresholdWidth),
      pUpdateThresholdWidth(p.pUpdateThresholdWidth),
      extraWeightsWidth(p.extraWeightsWidth),
      weightTableIdxWidth(p.weightTableIdxWidth),
      numCtrsPerLine(p.numCtrsPerLine),
      enableMGSC(p.enableMGSC),
      mgscStats(this)
{
    DPRINTF(MGSC, "BTBMGSC constructor\n");
    this->needMoreHistories = p.needMoreHistories;

    assert(isPowerOf2(numCtrsPerLine));
    numCtrsPerLineBits = log2i(numCtrsPerLine);

    bwTable.resize(bwTableNum);
    auto bwTableSize = std::pow(2, bwTableIdxWidth);
    assert(bwTableSize > numCtrsPerLine);
    for (unsigned int i = 0; i < bwTableNum; ++i) {
        bwTable[i].resize(bwTableSize / numCtrsPerLine, std::vector<int16_t>(numCtrsPerLine, 0));
        indexBwFoldedHist.push_back(FoldedHist(bwHistLen[i], bwTableIdxWidth, 16, HistoryType::GLOBALBW));
    }
    bwIndex.resize(bwTableNum);

    lTable.resize(lTableNum);
    indexLFoldedHist.resize(numEntriesFirstLocalHistories);
    auto lTableSize = std::pow(2, lTableIdxWidth);
    assert(lTableSize > numCtrsPerLine);
    for (unsigned int i = 0; i < lTableNum; ++i) {
        lTable[i].resize(lTableSize / numCtrsPerLine, std::vector<int16_t>(numCtrsPerLine, 0));
        for (unsigned int k = 0; k < numEntriesFirstLocalHistories; ++k) {
            indexLFoldedHist[k].push_back(FoldedHist(lHistLen[i], lTableIdxWidth, 16, HistoryType::LOCAL));
        }
    }
    lIndex.resize(lTableNum);

    iTable.resize(iTableNum);
    auto iTableSize = std::pow(2, iTableIdxWidth);
    assert(iTableSize > numCtrsPerLine);
    for (unsigned int i = 0; i < iTableNum; ++i) {
        assert(std::pow(2, iHistLen[i]) <= iTableSize);
        iTable[i].resize(iTableSize / numCtrsPerLine, std::vector<int16_t>(numCtrsPerLine, 0));
        indexIFoldedHist.push_back(FoldedHist(iHistLen[i], iTableIdxWidth, 16, HistoryType::IMLI));
    }
    iIndex.resize(iTableNum);

    gTable.resize(gTableNum);
    auto gTableSize = std::pow(2, gTableIdxWidth);
    assert(gTableSize > numCtrsPerLine);
    for (unsigned int i = 0; i < gTableNum; ++i) {
        assert(gTable.size() >= gTableNum);
        gTable[i].resize(gTableSize / numCtrsPerLine, std::vector<int16_t>(numCtrsPerLine, 0));
        indexGFoldedHist.push_back(FoldedHist(gHistLen[i], gTableIdxWidth, 16, HistoryType::GLOBAL));
    }
    gIndex.resize(gTableNum);

    pTable.resize(pTableNum);
    auto pTableSize = std::pow(2, pTableIdxWidth);
    assert(pTableSize > numCtrsPerLine);
    for (unsigned int i = 0; i < pTableNum; ++i) {
        assert(pTable.size() >= pTableNum);
        pTable[i].resize(pTableSize / numCtrsPerLine, std::vector<int16_t>(numCtrsPerLine, 0));
        indexPFoldedHist.push_back(FoldedHist(pHistLen[i], pTableIdxWidth, 2, HistoryType::PATH));
    }
    pIndex.resize(pTableNum);

    biasTable.resize(biasTableNum);
    auto biasTableSize = std::pow(2, biasTableIdxWidth);
    assert(biasTableSize > numCtrsPerLine);
    for (unsigned int i = 0; i < biasTableNum; ++i) {
        biasTable[i].resize(biasTableSize / numCtrsPerLine, std::vector<int16_t>(numCtrsPerLine, 0));
    }
    biasIndex.resize(biasTableNum);

    bwWeightTable.resize(std::pow(2, weightTableIdxWidth));
    lWeightTable.resize(std::pow(2, weightTableIdxWidth));
    iWeightTable.resize(std::pow(2, weightTableIdxWidth));
    gWeightTable.resize(std::pow(2, weightTableIdxWidth));
    pWeightTable.resize(std::pow(2, weightTableIdxWidth));
    biasWeightTable.resize(std::pow(2, weightTableIdxWidth));
    pUpdateThreshold.resize(std::pow(2, thresholdTablelogSize));

    updateThreshold = 35 * 8;
}

BTBMGSC::~BTBMGSC() {}

// Set up tracing for debugging
void
BTBMGSC::setTrace()
{
}

void
BTBMGSC::tick()
{
}

void
BTBMGSC::tickStart()
{
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
BTBMGSC::calculatePercsum(const std::vector<std::vector<std::vector<int16_t>>> &table,
                          const std::vector<unsigned> &tableIndices, unsigned numTables, Addr pc)
{
    int percsum = 0;
    for (unsigned int i = 0; i < numTables; ++i) {
        auto [idx1, idx2] = posHash(pc, tableIndices[i]);
        auto &entry = table[i][idx1][idx2];
        percsum += (2 * entry + 1);  // align to zero center
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
BTBMGSC::findWeight(const std::vector<int16_t> &weightTable, Addr pc)
{
    auto mask = (1 << weightTableIdxWidth) - 1;
    auto pcHash = ((pc >> instShiftAmt) ^ (pc >> instShiftAmt >> 2)) & mask;
    auto &entry = weightTable[pcHash];
    return entry;
}

/**
 * Calculate scaled percsum using weight
 * weight range is [-32, 31], return value range is percsum or 2x percsum
 * @param weight Weight value
 * @param percsum Original percsum value
 * @return Scaled percsum value
 */
int
BTBMGSC::calculateScaledPercsum(int weight, int percsum)
{
    return ((weight + 64) / 32) * percsum;
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
BTBMGSC::findThreshold(const std::vector<int16_t> &thresholdTable, Addr pc)
{
    auto mask = (1 << thresholdTablelogSize) - 1;
    auto pcHash = ((pc >> instShiftAmt) ^ (pc >> instShiftAmt >> 2)) & mask;
    auto &entry = thresholdTable[pcHash];
    return entry;
}

/**
 * Calculate if weight scale causes prediction difference
 * @param total_sum Total weighted sum
 * @param scale_percsum Component's scaled percsum
 * @param percsum Component's raw percsum
 * @return True if weight scale causes prediction to change
 */
bool
BTBMGSC::calculateWeightScaleDiff(int total_sum, int scale_percsum, int percsum)
{
    // First check if removing this table's contribution keeps the sum positive (predict taken)
    // Then check if doubling this table's contribution keeps the sum positive
    // If one is true and the other is false, the table's weight is crucial for prediction
    return ((total_sum - scale_percsum) >= 0) != ((total_sum - scale_percsum + 2 * percsum) >= 0);
}

/**
 * @brief Generate prediction for a single BTB entry by searching MGSC tables
 *
 * @param btb_entry The BTB entry to generate prediction for
 * @param startPC The starting PC address for calculating indices and tags
 * @return TagePrediction containing main and alternative predictions
 */
BTBMGSC::MgscPrediction
BTBMGSC::generateSinglePrediction(const BTBEntry &btb_entry, const Addr &startPC, const TageInfoForMGSC &tage_info)
{
    DPRINTF(MGSC, "generateSinglePrediction for btbEntry: %#lx, always taken %d\n", btb_entry.pc,
            btb_entry.alwaysTaken);

    // Calculate indices for all tables
    for (unsigned int i = 0; i < bwTableNum; ++i) {
        bwIndex[i] = getHistIndex(startPC, bwTableIdxWidth, indexBwFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < lTableNum; ++i) {
        lIndex[i] = getHistIndex(startPC, lTableIdxWidth,
                                 indexLFoldedHist[getPcIndex(startPC, log2(numEntriesFirstLocalHistories))][i].get());
    }

    for (unsigned int i = 0; i < iTableNum; ++i) {
        iIndex[i] = getHistIndex(startPC, iTableIdxWidth, indexIFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < gTableNum; ++i) {
        gIndex[i] = getHistIndex(startPC, gTableIdxWidth, indexGFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < pTableNum; ++i) {
        pIndex[i] = getHistIndex(startPC, pTableIdxWidth, indexPFoldedHist[i].get());
    }

    for (unsigned int i = 0; i < biasTableNum; ++i) {
        biasIndex[i] = getBiasIndex(startPC, biasTableIdxWidth, tage_info.tage_pred_taken,
                                    tage_info.tage_pred_conf_low && tage_info.tage_pred_alt_diff);
    }

    int bw_percsum = calculatePercsum(bwTable, bwIndex, bwTableNum, btb_entry.pc);
    int bw_weight = findWeight(bwWeightTable, btb_entry.pc);
    int bw_scaled_percsum = calculateScaledPercsum(bw_weight, bw_percsum);

    int l_percsum = calculatePercsum(lTable, lIndex, lTableNum, btb_entry.pc);
    int l_weight = findWeight(lWeightTable, btb_entry.pc);
    int l_scaled_percsum = calculateScaledPercsum(l_weight, l_percsum);

    int i_percsum = calculatePercsum(iTable, iIndex, iTableNum, btb_entry.pc);
    int i_weight = findWeight(iWeightTable, btb_entry.pc);
    int i_scaled_percsum = calculateScaledPercsum(i_weight, i_percsum);

    int g_percsum = calculatePercsum(gTable, gIndex, gTableNum, btb_entry.pc);
    int g_weight = findWeight(gWeightTable, btb_entry.pc);
    int g_scaled_percsum = calculateScaledPercsum(g_weight, g_percsum);

    int p_percsum = calculatePercsum(pTable, pIndex, pTableNum, btb_entry.pc);
    int p_weight = findWeight(pWeightTable, btb_entry.pc);
    int p_scaled_percsum = calculateScaledPercsum(p_weight, p_percsum);

    int bias_percsum = calculatePercsum(biasTable, biasIndex, biasTableNum, btb_entry.pc);
    int bias_weight = findWeight(biasWeightTable, btb_entry.pc);
    int bias_scaled_percsum = calculateScaledPercsum(bias_weight, bias_percsum);

    // Calculate total sum of all weighted percsums
    int total_sum = bw_scaled_percsum + l_scaled_percsum + i_scaled_percsum + g_scaled_percsum + p_scaled_percsum +
                    bias_scaled_percsum;

    // Find thresholds
    // pc-indexed threshold table
    int p_update_thres = findThreshold(pUpdateThreshold, btb_entry.pc);

    int total_thres = (updateThreshold / 8) + p_update_thres;

    bool use_sc_pred = false;
    if (tage_info.tage_pred_conf_high) {
        if (abs(total_sum) > total_thres / 2) {
            use_sc_pred = true;
        }
    } else if (tage_info.tage_pred_conf_mid) {
        if (abs(total_sum) > total_thres / 4) {
            use_sc_pred = true;
        }
    } else if (tage_info.tage_pred_conf_low) {
        if (abs(total_sum) > total_thres / 8) {
            use_sc_pred = true;
        }
    }
    // Final prediction, total_sum >= 0 means taken if use_sc_pred
    bool taken = use_sc_pred ? (total_sum >= 0) : tage_info.tage_pred_taken;

    // Calculate weight scale differences
    bool bw_weight_scale_diff = calculateWeightScaleDiff(total_sum, bw_scaled_percsum, bw_percsum);
    bool l_weight_scale_diff = calculateWeightScaleDiff(total_sum, l_scaled_percsum, l_percsum);
    bool i_weight_scale_diff = calculateWeightScaleDiff(total_sum, i_scaled_percsum, i_percsum);
    bool g_weight_scale_diff = calculateWeightScaleDiff(total_sum, g_scaled_percsum, g_percsum);
    bool p_weight_scale_diff = calculateWeightScaleDiff(total_sum, p_scaled_percsum, p_percsum);
    bool bias_weight_scale_diff = calculateWeightScaleDiff(total_sum, bias_scaled_percsum, bias_percsum);

    DPRINTF(MGSC, "sc predict %#lx taken %d\n", btb_entry.pc, taken);

    return MgscPrediction(btb_entry.pc, total_sum, use_sc_pred, taken, tage_info.tage_pred_taken, total_thres, bwIndex,
                          lIndex, iIndex, gIndex, pIndex, biasIndex, bw_weight_scale_diff, l_weight_scale_diff,
                          i_weight_scale_diff, g_weight_scale_diff, p_weight_scale_diff, bias_weight_scale_diff,
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
                      const std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs, CondTakens &results)
{
    DPRINTF(MGSC, "lookupHelper startAddr: %#lx\n", startPC);

    // Process each BTB entry to make predictions
    for (auto &btb_entry : btbEntries) {
        // Only predict for valid conditional branches
        if (btb_entry.isCond && btb_entry.valid) {
            auto tage_info = tageInfoForMgscs.find(btb_entry.pc);
            if (tage_info != tageInfoForMgscs.end()) {
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
BTBMGSC::putPCHistory(Addr stream_start, const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds)
{
    DPRINTF(MGSC, "putPCHistory startAddr: %#lx\n", stream_start);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it

    if (!enableMGSC) {
        return;  // Just return if MGSC is disabled
    }

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
BTBMGSC::getPredictionMeta()
{
    return meta;
}

/**
 * @brief Prepare BTB entries for update by filtering and processing
 *
 * @param stream The fetch stream containing update information
 * @return Vector of BTB entries that need to be updated
 */
std::vector<BTBEntry>
BTBMGSC::prepareUpdateEntries(const FetchStream &stream)
{
    auto all_entries = stream.updateBTBEntries;

    // Filter out non-conditional and always-taken branches
    auto remove_it = std::remove_if(all_entries.begin(), all_entries.end(),
                                    [](const BTBEntry &e) { return !e.isCond && !e.alwaysTaken; });
    all_entries.erase(remove_it, all_entries.end());

    // Handle potential new BTB entry
    auto &potential_new_entry = stream.updateNewBTBEntry;
    if (!stream.updateIsOldEntry && potential_new_entry.isCond && !potential_new_entry.alwaysTaken) {
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
BTBMGSC::updatePredTable(std::vector<std::vector<std::vector<int16_t>>> &table,
                         const std::vector<unsigned> &tableIndices, unsigned numTables, Addr pc, bool actual_taken)
{
    for (unsigned int i = 0; i < numTables; ++i) {
        auto [idx1, idx2] = posHash(pc, tableIndices[i]);
        auto &entry = table[i][idx1][idx2];
        updateCounter(actual_taken, scCountersWidth, entry);
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
BTBMGSC::updateWeightTable(std::vector<int16_t> &weightTable, Addr tableIndex, Addr pc, bool weight_scale_diff,
                           bool percsum_matches_actual)
{
    auto mask = (1 << weightTableIdxWidth) - 1;
    auto pcHash = ((pc >> instShiftAmt) ^ (pc >> instShiftAmt >> 2)) & mask;
    auto &entry = weightTable[pcHash];
    // Only update if weight scale could affect prediction
    if (weight_scale_diff) {
        // Increase weight if percsum was correct, decrease if incorrect
        updateCounter(percsum_matches_actual, extraWeightsWidth, entry);
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
BTBMGSC::updatePCThresholdTable(Addr pc, bool update_direction)
{
    auto mask = (1 << thresholdTablelogSize) - 1;
    auto pcHash = ((pc >> instShiftAmt) ^ (pc >> instShiftAmt >> 2)) & mask;
    auto &entry = pUpdateThreshold[pcHash];
    updateCounter(update_direction, pUpdateThresholdWidth, entry);
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
BTBMGSC::updateGlobalThreshold(Addr pc, bool update_direction)
{
    updateCounter(update_direction, updateThresholdWidth, updateThreshold);
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
BTBMGSC::updateSinglePredictor(const BTBEntry &entry, bool actual_taken, const MgscPrediction &pred,
                               const FetchStream &stream)
{
    // Extract prediction information
    auto total_sum = pred.total_sum;
    auto use_mgsc = pred.use_mgsc;
    auto total_thres = pred.total_thres;
    auto sc_pred_taken = total_sum >= 0;
    auto tage_pred_taken = pred.taken_before_sc;  // tage predictions

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
    if (sc_pred_taken != actual_taken || abs(total_sum) < total_thres) {
        // get weight table index from startPC
        Addr weightTableIdx = getPcIndex(stream.startPC, weightTableIdxWidth);

        // Update BW tables
        updatePredTable(bwTable, pred.bwIndex, bwTableNum, entry.pc, actual_taken);
        updateWeightTable(bwWeightTable, weightTableIdx, entry.pc, pred.bw_weight_scale_diff,
                          (pred.bw_percsum >= 0) == actual_taken);

        // Update L tables
        updatePredTable(lTable, pred.lIndex, lTableNum, entry.pc, actual_taken);
        updateWeightTable(lWeightTable, weightTableIdx, entry.pc, pred.l_weight_scale_diff,
                          (pred.l_percsum >= 0) == actual_taken);

        // Update I tables
        updatePredTable(iTable, pred.iIndex, iTableNum, entry.pc, actual_taken);
        updateWeightTable(iWeightTable, weightTableIdx, entry.pc, pred.i_weight_scale_diff,
                          (pred.i_percsum >= 0) == actual_taken);

        // Update G tables
        updatePredTable(gTable, pred.gIndex, gTableNum, entry.pc, actual_taken);
        updateWeightTable(gWeightTable, weightTableIdx, entry.pc, pred.g_weight_scale_diff,
                          (pred.g_percsum >= 0) == actual_taken);

        // Update P tables
        updatePredTable(pTable, pred.pIndex, pTableNum, entry.pc, actual_taken);
        updateWeightTable(pWeightTable, weightTableIdx, entry.pc, pred.p_weight_scale_diff,
                          (pred.p_percsum >= 0) == actual_taken);

        // Update bias tables
        updatePredTable(biasTable, pred.biasIndex, biasTableNum, entry.pc, actual_taken);
        updateWeightTable(biasWeightTable, weightTableIdx, entry.pc, pred.bias_weight_scale_diff,
                          (pred.bias_percsum >= 0) == actual_taken);

        // Update PC-indexed threshold table
        updatePCThresholdTable(entry.pc, sc_pred_taken != actual_taken);

        // Update global threshold table
        updateGlobalThreshold(entry.pc, sc_pred_taken != actual_taken);
    }
}

void
BTBMGSC::update(const FetchStream &stream)
{
    if (!enableMGSC) {
        return;  // No update if disabled
    }
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
        updateSinglePredictor(btb_entry, actual_taken, pred_it->second, stream);
    }

    DPRINTF(MGSC, "end update\n");
}

// Update counter with saturation (template for all integer types)
template<typename T>
void
BTBMGSC::updateCounter(bool taken, unsigned width, T &counter)
{
    static_assert(std::is_integral<T>::value, "Counter type must be integral");

    if constexpr (std::is_signed<T>::value) {
        T max = static_cast<T>((1LL << (width - 1)) - 1);
        T min = static_cast<T>(-((1LL << (width - 1)) - 1));
        if (taken) {
            satIncrement(max, counter);
        } else {
            satDecrement(min, counter);
        }
    } else {
        T max = static_cast<T>((1LL << width) - 1);
        T min = static_cast<T>(0);
        if (taken) {
            satIncrement(max, counter);
        } else {
            satDecrement(min, counter);
        }
    }
}

// Explicit instantiations for commonly used types
template void
BTBMGSC::updateCounter<int8_t>(bool taken, unsigned width, int8_t &counter);
template void
BTBMGSC::updateCounter<int16_t>(bool taken, unsigned width, int16_t &counter);
template void
BTBMGSC::updateCounter<int32_t>(bool taken, unsigned width, int32_t &counter);
template void
BTBMGSC::updateCounter<int64_t>(bool taken, unsigned width, int64_t &counter);
template void
BTBMGSC::updateCounter<uint8_t>(bool taken, unsigned width, uint8_t &counter);
template void
BTBMGSC::updateCounter<uint16_t>(bool taken, unsigned width, uint16_t &counter);
template void
BTBMGSC::updateCounter<uint32_t>(bool taken, unsigned width, uint32_t &counter);
template void
BTBMGSC::updateCounter<uint64_t>(bool taken, unsigned width, uint64_t &counter);


Addr
BTBMGSC::getHistIndex(Addr pc, unsigned tableIndexBits, uint64_t foldedHist)
{
    // Create mask to limit result size to tableIndexBits
    Addr mask = (1ULL << tableIndexBits) - 1;

    // Extract lower bits of PC and XOR with folded history directly
    Addr pcBits = (pc >> floorLog2(blockSize)) & mask;
    Addr foldedBits = foldedHist & mask;

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

template<typename T>
bool
BTBMGSC::satIncrement(T max, T &counter)
{
    static_assert(std::is_integral<T>::value, "Counter type must be integral");
    if (counter < max) {
        ++counter;
    } else {
        counter = max;
    }
    return counter == max;
}

// Explicit instantiations for commonly used types
template bool
BTBMGSC::satIncrement<int8_t>(int8_t max, int8_t &counter);
template bool
BTBMGSC::satIncrement<int16_t>(int16_t max, int16_t &counter);
template bool
BTBMGSC::satIncrement<int32_t>(int32_t max, int32_t &counter);
template bool
BTBMGSC::satIncrement<int64_t>(int64_t max, int64_t &counter);
template bool
BTBMGSC::satIncrement<uint8_t>(uint8_t max, uint8_t &counter);
template bool
BTBMGSC::satIncrement<uint16_t>(uint16_t max, uint16_t &counter);
template bool
BTBMGSC::satIncrement<uint32_t>(uint32_t max, uint32_t &counter);
template bool
BTBMGSC::satIncrement<uint64_t>(uint64_t max, uint64_t &counter);

template<typename T>
bool
BTBMGSC::satDecrement(T min, T &counter)
{
    static_assert(std::is_integral<T>::value, "Counter type must be integral");
    if (counter > min) {
        --counter;
    } else {
        counter = min;
    }
    return counter == min;
}

// Explicit instantiations for commonly used types
template bool
BTBMGSC::satDecrement<int8_t>(int8_t min, int8_t &counter);
template bool
BTBMGSC::satDecrement<int16_t>(int16_t min, int16_t &counter);
template bool
BTBMGSC::satDecrement<int32_t>(int32_t min, int32_t &counter);
template bool
BTBMGSC::satDecrement<int64_t>(int64_t min, int64_t &counter);
template bool
BTBMGSC::satDecrement<uint8_t>(uint8_t min, uint8_t &counter);
template bool
BTBMGSC::satDecrement<uint16_t>(uint16_t min, uint16_t &counter);
template bool
BTBMGSC::satDecrement<uint32_t>(uint32_t min, uint32_t &counter);
template bool
BTBMGSC::satDecrement<uint64_t>(uint64_t min, uint64_t &counter);

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
BTBMGSC::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool taken,
                      std::vector<FoldedHist> &foldedHist, Addr pc, Addr target)
{
    if (debug::MGSC) {
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(MGSC, "in doUpdateHist, shamt %d, taken %d, history %s\n", shamt, taken, buf.c_str());
    }
    if (shamt == 0) {
        DPRINTF(MGSC, "shamt is 0, returning\n");
        return;
    }

    for (int t = 0; t < foldedHist.size(); t++) {
        foldedHist[t].update(history, shamt, taken, pc, target);
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
    doUpdateHist(history, shamt, cond_taken, indexGFoldedHist);  // use global history to update G folded history
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
    auto [pc, target, taken] = pred.getPHistInfo();
    doUpdateHist(history, 2, taken, indexPFoldedHist, pc, target);  // only path history needs pc!
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
    doUpdateHist(history[getPcIndex(pred.bbStart, log2(numEntriesFirstLocalHistories))], shamt, cond_taken,
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
BTBMGSC::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    if (!enableMGSC) {
        return; // No recover when disabled
    }
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < gTableNum; i++) {
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
BTBMGSC::recoverPHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    if (!enableMGSC) {
        return; // No recover when disabled
    }
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < pTableNum; i++) {
        indexPFoldedHist[i].recover(predMeta->indexPFoldedHist[i]);
    }
    doUpdateHist(history, 2, cond_taken, indexPFoldedHist, entry.getControlPC(), entry.getTakenTarget());
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
BTBMGSC::recoverBwHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    if (!enableMGSC) {
        return; // No recover when disabled
    }
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < bwTableNum; i++) {
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
BTBMGSC::recoverIHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    if (!enableMGSC) {
        return; // No recover when disabled
    }
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < iTableNum; i++) {
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
BTBMGSC::recoverLHist(const std::vector<boost::dynamic_bitset<>> &history, const FetchStream &entry, int shamt,
                      bool cond_taken)
{
    if (!enableMGSC) {
        return; // No recover when disabled
    }
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (unsigned int k = 0; k < numEntriesFirstLocalHistories; ++k) {
        for (int i = 0; i < lTableNum; i++) {
            indexLFoldedHist[k][i].recover(predMeta->indexLFoldedHist[k][i]);
        }
    }
    doUpdateHist(history[getPcIndex(entry.startPC, log2(numEntriesFirstLocalHistories))], shamt, cond_taken,
                 indexLFoldedHist[getPcIndex(entry.startPC, log2(numEntriesFirstLocalHistories))]);
}

// Constructor for TAGE statistics
BTBMGSC::MgscStats::MgscStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(scCorrectTageWrong, statistics::units::Count::get(),
               "number of sc predict correct and tage predict wrong"),
      ADD_STAT(scWrongTageCorrect, statistics::units::Count::get(),
               "number of sc predict wrong and tage predict correct"),
      ADD_STAT(scCorrectTageCorrect, statistics::units::Count::get(),
               "number of sc predict correct and tage predict correct"),
      ADD_STAT(scWrongTageWrong, statistics::units::Count::get(), "number of sc predict wrong and tage predict wrong"),
      ADD_STAT(scUsed, statistics::units::Count::get(), "number of sc used"),
      ADD_STAT(scNotUsed, statistics::units::Count::get(), "number of sc not used")
{
}

void
BTBMGSC::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
}

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
