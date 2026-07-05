#include "cpu/pred/btb/btb_mgsc.hh"

#include "base/intmath.hh"
#include "base/logging.hh"

#ifdef UNIT_TEST
#include "cpu/pred/btb/test/test_dprintf.hh"

// Define debug flags for unit testing
namespace gem5 {
namespace debug {
    bool MGSC = true;
}
}
#else
#include "cpu/o3/dyn_inst.hh"
#include "debug/MGSC.hh"

#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <type_traits>
#include <vector>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test
{
#endif

void
BTBMGSC::initStorage()
{
    auto pow2 = [](unsigned width) -> uint64_t {
        assert(width < 63);
        return 1ULL << width;
    };
    auto allocPredTable = [&](std::vector<std::vector<std::vector<int16_t>>> &table, unsigned numTables,
                              unsigned idxWidth) -> uint64_t {
        table.resize(numTables);
        auto tableSize = pow2(idxWidth);
        assert(tableSize > numCtrsPerLine);
        for (unsigned int i = 0; i < numTables; ++i) {
            table[i].resize(tableSize / numCtrsPerLine, std::vector<int16_t>(numCtrsPerLine, 0));
        }
        return tableSize;
    };

    assert(isPowerOf2(numCtrsPerLine));
    numCtrsPerLineBits = log2i(numCtrsPerLine);

    threadHistory.resize(MaxThreads);
    threadMeta.resize(MaxThreads);

    auto bwTableSize = allocPredTable(bwTable, bwTableNum, bwTableIdxWidth);
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        auto &state = threadHistory[tid];
        for (unsigned int i = 0; i < bwTableNum; ++i) {
            state.indexBwFoldedHist.emplace_back(
                bwHistLen[i], bwTableIdxWidth - numCtrsPerLineBits, 16);
        }
    }
    bwIndex.resize(bwTableNum);

    auto lTableSize = allocPredTable(lTable, lTableNum, lTableIdxWidth);
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        auto &state = threadHistory[tid];
        state.indexLFoldedHist.resize(numEntriesFirstLocalHistories);
        for (unsigned int i = 0; i < lTableNum; ++i) {
            for (unsigned int k = 0; k < numEntriesFirstLocalHistories; ++k) {
                state.indexLFoldedHist[k].push_back(LocalFoldedHist(
                    lHistLen[i], lTableIdxWidth - numCtrsPerLineBits, 16));
            }
        }
    }
    lIndex.resize(lTableNum);

    auto iTableSize = allocPredTable(iTable, iTableNum, iTableIdxWidth);
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        auto &state = threadHistory[tid];
        for (unsigned int i = 0; i < iTableNum; ++i) {
            assert(iHistLen[i] >= 0);
            assert(static_cast<unsigned>(iHistLen[i]) < 63);
            assert(pow2(static_cast<unsigned>(iHistLen[i])) <= iTableSize);
            state.indexIFoldedHist.emplace_back(
                iHistLen[i], iTableIdxWidth - numCtrsPerLineBits, 16);
        }
    }
    iIndex.resize(iTableNum);

    auto gTableSize = allocPredTable(gTable, gTableNum, gTableIdxWidth);
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        auto &state = threadHistory[tid];
        for (unsigned int i = 0; i < gTableNum; ++i) {
            assert(gTable.size() >= gTableNum);
            state.indexGFoldedHist.emplace_back(
                gHistLen[i], gTableIdxWidth - numCtrsPerLineBits, 16);
        }
    }
    gIndex.resize(gTableNum);

    auto pTableSize = allocPredTable(pTable, pTableNum, pTableIdxWidth);
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        auto &state = threadHistory[tid];
        for (unsigned int i = 0; i < pTableNum; ++i) {
            assert(pTable.size() >= pTableNum);
            state.indexPFoldedHist.emplace_back(
                pHistLen[i], pTableIdxWidth - numCtrsPerLineBits, 2);
        }
    }
    pIndex.resize(pTableNum);

    allocPredTable(biasTable, biasTableNum, biasTableIdxWidth);
    biasIndex.resize(biasTableNum);

    auto weightTableSize = pow2(weightTableIdxWidth);
    bwWeightTable.resize(weightTableSize);
    lWeightTable.resize(weightTableSize);
    iWeightTable.resize(weightTableSize);
    gWeightTable.resize(weightTableSize);
    pWeightTable.resize(weightTableSize);
    biasWeightTable.resize(weightTableSize);

    pUpdateThreshold.resize(pow2(thresholdTablelogSize));
}

#ifdef UNIT_TEST
BTBMGSC::BTBMGSC()
    : TimedBaseBTBPredictor(),
      bwTableNum(1),
      // Use a slightly larger idx width so foldedLen is not too small (helps pattern-learning tests).
      bwTableIdxWidth(6),
      bwHistLen({4}),
      numEntriesFirstLocalHistories(4),
      lTableNum(1),
      // Use a slightly larger idx width so foldedLen is not too small (helps pattern-learning tests).
      lTableIdxWidth(6),
      lHistLen({4}),
      iTableNum(1),
      iTableIdxWidth(5),
      // `ImliFoldedHist` requires foldedLen >= histLen. With `numCtrsPerLine=8` and `iTableIdxWidth=5`,
      // foldedLen is small (5 - log2(8) = 2), so keep histLen=1 for unit tests.
      // Also keep it >= 2 so we can build loop-trip-count tests on IMLI.
      iHistLen({2}),
      gTableNum(1),
      // Use a slightly larger idx width so foldedLen is not too small (helps pattern-learning tests).
      gTableIdxWidth(6),
      gHistLen({4}),
      pTableNum(1),
      // Use a slightly larger idx width so foldedLen is not too small (helps pattern-learning tests).
      pTableIdxWidth(6),
      pHistLen({4}),
      biasTableNum(1),
      biasTableIdxWidth(5),
      scCountersWidth(6),
      thresholdTablelogSize(4),
      updateThresholdWidth(12),
      pUpdateThresholdWidth(8),
      extraWeightsWidth(6),
      weightTableIdxWidth(4),
      // Keep consistent with `src/cpu/pred/BranchPredictor.py` default (8 counters per SRAM line).
      // This models "read a whole SRAM line, then pick a lane" behavior in `posHash()`.
      numCtrsPerLine(8),
      forceUseSC(false),
      allowMissingTageInfo(false),
      enableBwTable(true),
      enableLTable(true),
      enableITable(true),
      enableGTable(true),
      enablePTable(true),
      enableBiasTable(true),
      enablePCThreshold(false),
      focusBranchPC(0),
      mgscStats()
{
    // Test-only small config: keep tables tiny and deterministic for fast unit tests.
    initStorage();
    updateThreshold = 35 * 8;
}
#else
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
      forceUseSC(p.forceUseSC),
      allowMissingTageInfo(p.allowMissingTageInfo),
      enableBwTable(p.enableBwTable),
      enableLTable(p.enableLTable),
      enableITable(p.enableITable),
      enableGTable(p.enableGTable),
      enablePTable(p.enablePTable),
      enableBiasTable(p.enableBiasTable),
      enablePCThreshold(p.enablePCThreshold),
      focusBranchPC(p.focusBranchPC),
      mgscStats(this)
{
    DPRINTF(MGSC, "BTBMGSC constructor\n");
    initStorage();
    updateThreshold = 35 * 8;

    hasDB = true;
    dbName = std::string("mgsc");
}
#endif
BTBMGSC::~BTBMGSC() {}

ThreadID
BTBMGSC::predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const
{
    assert(!stagePreds.empty());
    return stagePreds.front().tid;
}

BTBMGSC::ThreadHistoryState &
BTBMGSC::historyState(ThreadID tid)
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

const BTBMGSC::ThreadHistoryState &
BTBMGSC::historyState(ThreadID tid) const
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

// Set up tracing for debugging
void
BTBMGSC::setTrace()
{
#ifndef UNIT_TEST
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("branchPC", UINT64),
            std::make_pair("bbStart", UINT64),
            std::make_pair("branchOffset", UINT64),
            std::make_pair("tagePred", UINT64),
            std::make_pair("tageConfHigh", UINT64),
            std::make_pair("tageConfMid", UINT64),
            std::make_pair("tageConfLow", UINT64),
            std::make_pair("bwPercsum", UINT64),
            std::make_pair("lPercsum", UINT64),
            std::make_pair("iPercsum", UINT64),
            std::make_pair("gPercsum", UINT64),
            std::make_pair("pPercsum", UINT64),
            std::make_pair("biasPercsum", UINT64),
            std::make_pair("totalSum", UINT64),
            std::make_pair("totalThres", UINT64),
            std::make_pair("effectiveGate", UINT64),
            std::make_pair("margin", UINT64),
            std::make_pair("bwIndex0", UINT64),
            std::make_pair("bwIndex1", UINT64),
            std::make_pair("lIndex0", UINT64),
            std::make_pair("lIndex1", UINT64),
            std::make_pair("iIndex0", UINT64),
            std::make_pair("gIndex0", UINT64),
            std::make_pair("gIndex1", UINT64),
            std::make_pair("pIndex0", UINT64),
            std::make_pair("pIndex1", UINT64),
            std::make_pair("biasIndex0", UINT64),
            std::make_pair("useSc", UINT64),
            std::make_pair("scPred", UINT64),
            std::make_pair("scWrong", UINT64),
            std::make_pair("actualTaken", UINT64),
        };
        mgscMissTrace = _db->addAndGetTrace("MGSCTRACE", fields_vec);
        mgscMissTrace->init_table();
    }
#endif
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
 * Counter range: [-2^(w-1), 2^(w-1)-1], e.g., [-32, 31] for w=6
 * Percsum = sum of (2*counter + 1), transforms to odd numbers, e.g., [-63, 63] per entry
 * @param table The table to search in
 * @param tableIndices Indices to use for each table component
 * @param numTables Number of tables to search
 * @param pc PC to match against
 * @return Calculated percsum value (positive=taken bias, negative=not-taken bias)
 */
int
BTBMGSC::calculatePercsum(const std::vector<std::vector<std::vector<int16_t>>> &table,
                          const std::vector<unsigned> &tableIndices, unsigned numTables, Addr pc)
{
    int percsum = 0;
    for (unsigned int i = 0; i < numTables; ++i) {
        auto [idx1, idx2] = posHash(pc, tableIndices[i]);
        auto &entry = table[i][idx1][idx2];
        percsum += (2 * entry + 1);  // transform to odd numbers, avoid zero
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
BTBMGSC::findWeight(const std::vector<int16_t> &weightTable, Addr pc,
                    uint8_t asidHash)
{
    auto mask = (1 << weightTableIdxWidth) - 1;
    auto pcHash = ((pc >> instShiftAmt) ^ ((pc >> instShiftAmt) >> 2)) & mask;
    pcHash = xorAsidHashIntoIndex(pcHash, weightTableIdxWidth, asidHash);
    auto &entry = weightTable[pcHash];
    return entry;
}


int
BTBMGSC::calculateScaledPercsum(int weight, int percsum)
{
    return percsum; // disable weight scaling for test
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
BTBMGSC::findThreshold(const std::vector<int16_t> &thresholdTable, Addr pc,
                       uint8_t asidHash)
{
    auto mask = (1 << thresholdTablelogSize) - 1;
    auto pcHash = ((pc >> instShiftAmt) ^ ((pc >> instShiftAmt) >> 2)) & mask;
    pcHash = xorAsidHashIntoIndex(pcHash, thresholdTablelogSize, asidHash);
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
 * @brief Generate prediction for a single branch by searching MGSC tables
 *
 * @param branchPC The branch PC to generate prediction for
 * @param startPC The starting PC address for calculating indices and tags
 * @return TagePrediction containing main and alternative predictions
 */
BTBMGSC::MgscPrediction
BTBMGSC::generateSinglePrediction(Addr branchPC, const Addr &startPC,
                                  const TageInfoForMGSC &tage_info,
                                  ThreadID tid, uint8_t asidHash)
{
    DPRINTF(MGSC, "generateSinglePrediction for pc: %#lx\n", branchPC);
    const auto &state = historyState(tid);

    // Calculate indices for all tables
    for (unsigned int i = 0; i < bwTableNum; ++i) {
        bwIndex[i] = getHistIndex(startPC, bwTableIdxWidth - numCtrsPerLineBits,
                                  state.indexBwFoldedHist[i].get(), asidHash);
    }

    const Addr localHistoryIndex =
        getPcIndex(startPC, log2(numEntriesFirstLocalHistories), asidHash);
    for (unsigned int i = 0; i < lTableNum; ++i) {
        lIndex[i] = getHistIndex(startPC, lTableIdxWidth - numCtrsPerLineBits,
                                 state.indexLFoldedHist[localHistoryIndex][i].get(),
                                 asidHash);
    }
    // std::string buf;
    // boost::to_string(indexLFoldedHist[getPcIndex(startPC, log2(numEntriesFirstLocalHistories))][0].getAsBitset(), buf);
    // DPRINTF(MGSC, "startPC: %#lx, local index: %d, local_folded_hist: %s\n", startPC, lIndex[0], buf.c_str());

    for (unsigned int i = 0; i < iTableNum; ++i) {
        iIndex[i] = getHistIndex(startPC, iTableIdxWidth - numCtrsPerLineBits,
                                 state.indexIFoldedHist[i].get(), asidHash);
    }

    for (unsigned int i = 0; i < gTableNum; ++i) {
        gIndex[i] = getHistIndex(startPC, gTableIdxWidth - numCtrsPerLineBits,
                                 state.indexGFoldedHist[i].get(), asidHash);
    }

    for (unsigned int i = 0; i < pTableNum; ++i) {
        pIndex[i] = getHistIndex(startPC, pTableIdxWidth - numCtrsPerLineBits,
                                 state.indexPFoldedHist[i].get(), asidHash);
    }

    for (unsigned int i = 0; i < biasTableNum; ++i) {
        biasIndex[i] = getBiasIndex(startPC, biasTableIdxWidth - numCtrsPerLineBits, tage_info.tage_main_taken,
                                    tage_info.tage_pred_conf_low, asidHash);
    }

    int bw_percsum = enableBwTable ? calculatePercsum(bwTable, bwIndex, bwTableNum, branchPC) : 0;
    int bw_weight = findWeight(bwWeightTable, branchPC, asidHash);
    int bw_scaled_percsum = calculateScaledPercsum(bw_weight, bw_percsum);

    int l_percsum = enableLTable ? calculatePercsum(lTable, lIndex, lTableNum, branchPC) : 0;
    int l_weight = findWeight(lWeightTable, branchPC, asidHash);
    int l_scaled_percsum = calculateScaledPercsum(l_weight, l_percsum);

    int i_percsum = enableITable ? calculatePercsum(iTable, iIndex, iTableNum, branchPC) : 0;
    int i_weight = findWeight(iWeightTable, branchPC, asidHash);
    int i_scaled_percsum = calculateScaledPercsum(i_weight, i_percsum);

    int g_percsum = enableGTable ? calculatePercsum(gTable, gIndex, gTableNum, branchPC) : 0;
    int g_weight = findWeight(gWeightTable, branchPC, asidHash);
    int g_scaled_percsum = calculateScaledPercsum(g_weight, g_percsum);

    int p_percsum = enablePTable ? calculatePercsum(pTable, pIndex, pTableNum, branchPC) : 0;
    int p_weight = findWeight(pWeightTable, branchPC, asidHash);
    int p_scaled_percsum = calculateScaledPercsum(p_weight, p_percsum);

    int bias_percsum = enableBiasTable ? calculatePercsum(biasTable, biasIndex, biasTableNum, branchPC) : 0;
    int bias_weight = findWeight(biasWeightTable, branchPC, asidHash);
    int bias_scaled_percsum = calculateScaledPercsum(bias_weight, bias_percsum);

    // Calculate total sum of all weighted percsums
    int total_sum = bw_scaled_percsum + l_scaled_percsum + i_scaled_percsum + g_scaled_percsum + p_scaled_percsum +
                    bias_scaled_percsum;

    // Find thresholds
    // pc-indexed threshold table (only if enabled)
    int p_update_thres =
        enablePCThreshold ? findThreshold(pUpdateThreshold, branchPC, asidHash) : 0;

    int total_thres = (updateThreshold / 8) + p_update_thres;
    // Threshold is used as a confidence gate; avoid negative values which
    // effectively disable the gate (abs(sum) > negative is almost always true).
    total_thres = std::max(total_thres, 0);

    bool use_sc_pred = forceUseSC;  // Force use SC if configured
    if (!use_sc_pred) {
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
    }
    // Final prediction, total_sum >= 0 means taken if use_sc_pred
    bool taken = use_sc_pred ? (total_sum >= 0) : tage_info.tage_pred_taken;

    // DPRINTF(MGSC, "global tag_index: %d, global_percsum: %d, total_sum: %d\n", gIndex[0], g_percsum, total_sum);
    // DPRINTF(MGSC, "local tag_index: %d, local_percsum: %d, total_sum: %d\n", lIndex[0], l_percsum, total_sum);
    // DPRINTF(MGSC, "path tag_index: %d, path_percsum: %d, total_sum: %d\n", pIndex[0], p_percsum, total_sum);

    // Calculate weight scale differences
    bool bw_weight_scale_diff = calculateWeightScaleDiff(total_sum, bw_scaled_percsum, bw_percsum);
    bool l_weight_scale_diff = calculateWeightScaleDiff(total_sum, l_scaled_percsum, l_percsum);
    bool i_weight_scale_diff = calculateWeightScaleDiff(total_sum, i_scaled_percsum, i_percsum);
    bool g_weight_scale_diff = calculateWeightScaleDiff(total_sum, g_scaled_percsum, g_percsum);
    bool p_weight_scale_diff = calculateWeightScaleDiff(total_sum, p_scaled_percsum, p_percsum);
    bool bias_weight_scale_diff = calculateWeightScaleDiff(total_sum, bias_scaled_percsum, bias_percsum);

    DPRINTF(MGSC, "sc predict %#lx taken %d\n", branchPC, taken);

    return MgscPrediction(branchPC, total_sum, use_sc_pred, taken, tage_info.tage_pred_taken,
                          tage_info.tage_pred_conf_high, tage_info.tage_pred_conf_mid, tage_info.tage_pred_conf_low,
                          total_thres, bwIndex, lIndex, iIndex, gIndex, pIndex, biasIndex, bw_weight_scale_diff,
                          l_weight_scale_diff, i_weight_scale_diff, g_weight_scale_diff, p_weight_scale_diff,
                          bias_weight_scale_diff, bw_percsum, l_percsum, i_percsum, g_percsum, p_percsum, bias_percsum);
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
                      const std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs,
                      CondTakens &results, ThreadID tid, uint8_t asidHash)
{
    DPRINTF(MGSC, "lookupHelper startAddr: %#lx\n", startPC);

    // Process each BTB entry to make predictions
    for (auto &btb_entry : btbEntries) {
        // Only predict for valid conditional branches
        if (btb_entry.isCond && btb_entry.valid) {
            auto tage_info = tageInfoForMgscs.find(btb_entry.pc);
            panic_if(tage_info == tageInfoForMgscs.end() && !allowMissingTageInfo,
                     "MGSC missing TAGE info for conditional branch pc %#lx "
                     "startPC %#lx tid %u asidHash %#x",
                     btb_entry.pc, startPC, static_cast<unsigned>(tid),
                     static_cast<unsigned>(asidHash));

            const TageInfoForMGSC missing_tage_info;
            const auto &info =
                tage_info != tageInfoForMgscs.end() ? tage_info->second : missing_tage_info;
            auto pred = generateSinglePrediction(btb_entry.pc, startPC, info, tid, asidHash);
            threadMeta[tid]->preds[btb_entry.pc] = pred;
            results.push_back({btb_entry.pc, pred.taken});
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
    const ThreadID tid = predictorTid(stagePreds);
    const auto &state = historyState(tid);
    const uint8_t asidHash = stagePreds.empty() ? 0 : stagePreds.front().asidHash;
    DPRINTF(MGSC, "putPCHistory startAddr: %#lx\n", stream_start);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it

    if (!isEnabled()) {
        return;  // Just return if MGSC is disabled
    }

    // Clear old prediction metadata and save current history state
    threadMeta[tid] = std::make_shared<MgscMeta>();
    threadMeta[tid]->indexBwFoldedHist = state.indexBwFoldedHist;
    threadMeta[tid]->indexLFoldedHist = state.indexLFoldedHist;
    threadMeta[tid]->indexIFoldedHist = state.indexIFoldedHist;
    threadMeta[tid]->indexGFoldedHist = state.indexGFoldedHist;
    threadMeta[tid]->indexPFoldedHist = state.indexPFoldedHist;

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        stage_pred.condTakens.clear();
        lookupHelper(stream_start, stage_pred.btbEntries,
                     stage_pred.tageInfoForMgscs, stage_pred.condTakens, tid,
                     asidHash);
    }
}

std::shared_ptr<void>
BTBMGSC::getPredictionMeta(ThreadID tid)
{
    if (tid >= threadMeta.size()) {
        return nullptr;
    }
    return threadMeta[tid];
}

/**
 * @brief Prepare BTB entries for update by filtering and processing
 *
 * @param stream The fetch stream containing update information
 * @return Vector of BTB entries that need to be updated
 */
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
    auto pcHash = ((pc >> instShiftAmt) ^ ((pc >> instShiftAmt) >> 2)) & mask;
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
BTBMGSC::updatePCThresholdTable(Addr pc, uint8_t asidHash, bool update_direction)
{
    auto mask = (1 << thresholdTablelogSize) - 1;
    auto pcHash = ((pc >> instShiftAmt) ^ ((pc >> instShiftAmt) >> 2)) & mask;
    pcHash = xorAsidHashIntoIndex(pcHash, thresholdTablelogSize, asidHash);
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
    // Keep global threshold non-negative; negative thresholds make SC gating
    // degenerate and can cause overuse of SC.
    if (updateThreshold < 0) {
        updateThreshold = 0;
    }
}

void
BTBMGSC::recordPredictionStats(const MgscPrediction &pred, bool actual_taken, bool sc_pred_taken,
                               bool tage_pred_taken)
{
    auto tage_conf_high = pred.tage_conf_high;
    auto tage_conf_mid = pred.tage_conf_mid;
    auto tage_conf_low = pred.tage_conf_low;

    // SC vs TAGE outcomes
    if (pred.use_mgsc) {
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

    // Record raw percsum correctness and weight criticality for each table
    auto recordPercsum = [&](int percsum, auto &correct, auto &wrong) {
        if ((percsum >= 0) == actual_taken) {
            correct++;
        } else {
            wrong++;
        }
    };
    if (pred.bw_weight_scale_diff) {
        mgscStats.bwWeightScaleDiff++;
    }
    recordPercsum(pred.bw_percsum, mgscStats.bwPercsumCorrect, mgscStats.bwPercsumWrong);

    if (pred.l_weight_scale_diff) {
        mgscStats.lWeightScaleDiff++;
    }
    recordPercsum(pred.l_percsum, mgscStats.lPercsumCorrect, mgscStats.lPercsumWrong);

    if (pred.i_weight_scale_diff) {
        mgscStats.iWeightScaleDiff++;
    }
    recordPercsum(pred.i_percsum, mgscStats.iPercsumCorrect, mgscStats.iPercsumWrong);

    if (pred.g_weight_scale_diff) {
        mgscStats.gWeightScaleDiff++;
    }
    recordPercsum(pred.g_percsum, mgscStats.gPercsumCorrect, mgscStats.gPercsumWrong);

    if (pred.p_weight_scale_diff) {
        mgscStats.pWeightScaleDiff++;
    }
    recordPercsum(pred.p_percsum, mgscStats.pPercsumCorrect, mgscStats.pPercsumWrong);

    if (pred.bias_weight_scale_diff) {
        mgscStats.biasWeightScaleDiff++;
    }
    recordPercsum(pred.bias_percsum, mgscStats.biasPercsumCorrect, mgscStats.biasPercsumWrong);

    // SC usage under TAGE confidence buckets
    auto recordConfOutcome = [&](bool conf_high, bool conf_mid, bool conf_low, bool use, bool correct) {
        if (conf_high) {
            if (use) {
                correct ? mgscStats.scHighUseCorrect++ : mgscStats.scHighUseWrong++;
            } else {
                mgscStats.scHighBypass++;
            }
        } else if (conf_mid) {
            if (use) {
                correct ? mgscStats.scMidUseCorrect++ : mgscStats.scMidUseWrong++;
            } else {
                mgscStats.scMidBypass++;
            }
        } else if (conf_low) {
            if (use) {
                correct ? mgscStats.scLowUseCorrect++ : mgscStats.scLowUseWrong++;
            } else {
                mgscStats.scLowBypass++;
            }
        }
    };
    recordConfOutcome(tage_conf_high, tage_conf_mid, tage_conf_low, pred.use_mgsc, sc_pred_taken == actual_taken);
}

/**
 * @brief Update predictor for a single entry and allocate new entries if needed
 *
 * This function updates the MGSC predictor state based on the actual branch outcome
 * and allocates new entries in various tables if they don't already exist.
 *
 * @param branchPC The branch PC being updated
 * @param actual_taken The actual outcome of the branch
 * @param pred The prediction made for this entry
 * @param ctx Stream-level context needed by direction update
 */
void
BTBMGSC::updateSinglePredictor(Addr branchPC, bool actual_taken, const MgscPrediction &pred,
                               const BranchUpdateContext &ctx)
{
    // Extract prediction information
    auto total_sum = pred.total_sum;
    auto use_mgsc = pred.use_mgsc;
    auto total_thres = pred.total_thres;
    auto sc_pred_taken = total_sum >= 0;
    auto tage_pred_taken = pred.taken_before_sc;  // tage predictions

    recordPredictionStats(pred, actual_taken, sc_pred_taken, tage_pred_taken);

#ifndef UNIT_TEST
    // Write trace record
    if (enableDB && (focusBranchPC == 0 || branchPC == focusBranchPC)) {
        auto effective_gate = pred.tage_conf_high ? (total_thres / 2)
            : (pred.tage_conf_mid ? (total_thres / 4) : (total_thres / 8));
        auto margin = std::abs(total_sum) - effective_gate;
        auto indexAt = [](const std::vector<unsigned> &indices, size_t idx) -> uint64_t {
            return idx < indices.size() ? indices[idx] : 0;
        };
        MgscTrace t;
        t.set(branchPC,
            ctx.startPC, getOffset(branchPC),
            tage_pred_taken, pred.tage_conf_high, pred.tage_conf_mid, pred.tage_conf_low,
            pred.bw_percsum, pred.l_percsum, pred.i_percsum,
            pred.g_percsum, pred.p_percsum, pred.bias_percsum,
            total_sum, total_thres, effective_gate, margin,
            indexAt(pred.bwIndex, 0), indexAt(pred.bwIndex, 1),
            indexAt(pred.lIndex, 0), indexAt(pred.lIndex, 1),
            indexAt(pred.iIndex, 0),
            indexAt(pred.gIndex, 0), indexAt(pred.gIndex, 1),
            indexAt(pred.pIndex, 0), indexAt(pred.pIndex, 1),
            indexAt(pred.biasIndex, 0),
            use_mgsc, sc_pred_taken, sc_pred_taken != actual_taken,
            actual_taken);
        mgscMissTrace->write_record(t);
    }
#endif

        // Only update tables if prediction was wrong or confidence was low
    if (sc_pred_taken != actual_taken || abs(total_sum) < (total_thres / 2)) {
        // get weight table index from startPC
        Addr weightTableIdx = getPcIndex(ctx.startPC, weightTableIdxWidth,
                                         ctx.asidHash);
        bool threshold_inc = (sc_pred_taken != actual_taken);
        if (threshold_inc) {
            mgscStats.pcThresholdInc++;
            mgscStats.globalThresholdInc++;
        } else {
            mgscStats.pcThresholdDec++;
            mgscStats.globalThresholdDec++;
        }

        // Update BW tables
        updatePredTable(bwTable, pred.bwIndex, bwTableNum, branchPC, actual_taken);
        updateWeightTable(bwWeightTable, weightTableIdx, branchPC, pred.bw_weight_scale_diff,
                          (pred.bw_percsum >= 0) == actual_taken);

        // Update L tables
        updatePredTable(lTable, pred.lIndex, lTableNum, branchPC, actual_taken);
        updateWeightTable(lWeightTable, weightTableIdx, branchPC, pred.l_weight_scale_diff,
                          (pred.l_percsum >= 0) == actual_taken);

        // Update I tables
        updatePredTable(iTable, pred.iIndex, iTableNum, branchPC, actual_taken);
        updateWeightTable(iWeightTable, weightTableIdx, branchPC, pred.i_weight_scale_diff,
                          (pred.i_percsum >= 0) == actual_taken);

        // Update G tables
        updatePredTable(gTable, pred.gIndex, gTableNum, branchPC, actual_taken);
        updateWeightTable(gWeightTable, weightTableIdx, branchPC, pred.g_weight_scale_diff,
                          (pred.g_percsum >= 0) == actual_taken);

        // Update P tables
        updatePredTable(pTable, pred.pIndex, pTableNum, branchPC, actual_taken);
        updateWeightTable(pWeightTable, weightTableIdx, branchPC, pred.p_weight_scale_diff,
                          (pred.p_percsum >= 0) == actual_taken);

        // Update bias tables
        updatePredTable(biasTable, pred.biasIndex, biasTableNum, branchPC, actual_taken);
        updateWeightTable(biasWeightTable, weightTableIdx, branchPC, pred.bias_weight_scale_diff,
                          (pred.bias_percsum >= 0) == actual_taken);

        // Update PC-indexed threshold table (only if enabled)
        if (enablePCThreshold) {
            updatePCThresholdTable(branchPC, ctx.asidHash,
                                   sc_pred_taken != actual_taken);
        }

        // Update global threshold table
        updateGlobalThreshold(branchPC, sc_pred_taken != actual_taken);
    }
}

void
BTBMGSC::updateWithDirectionEntries(
    const std::vector<DirectionUpdateEntry> &entries,
    const BranchUpdateContext &ctx,
    const std::shared_ptr<void> &prediction_meta,
    const boost::dynamic_bitset<> &)
{
    if (!isEnabled()) {
        return;  // No update if disabled
    }
    DPRINTF(MGSC, "update startAddr: %#lx\n", ctx.startPC);
    // Get prediction metadata
    auto meta = std::static_pointer_cast<MgscMeta>(prediction_meta);
    updateWithEntries(entries, ctx, *meta);
}

void
BTBMGSC::updateWithEntries(const std::vector<DirectionUpdateEntry> &entries,
                           const BranchUpdateContext &ctx,
                           const MgscMeta &meta)
{
    // Process each branch entry
    for (const auto &update_entry : entries) {
        const auto &actual_branch = update_entry.actualBranch;
        const Addr branch_pc = actual_branch.pc;
        const bool actual_taken = actual_branch.taken;
        auto pred_it = meta.preds.find(branch_pc);

        if (pred_it == meta.preds.end()) {
            continue;
        }

        // Update predictor state and check if need to allocate new entry
        updateSinglePredictor(branch_pc, actual_taken, pred_it->second,
                              ctx);
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
        T min = static_cast<T>(-(1LL << (width - 1)));
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
BTBMGSC::getHistIndex(Addr pc, unsigned tableIndexBits, uint64_t foldedHist,
                      uint8_t asidHash)
{
    // Create mask to limit result size to tableIndexBits
    Addr mask = (1ULL << tableIndexBits) - 1;

    // Extract lower bits of PC and XOR with folded history directly
    Addr pcBits = (pc >> floorLog2(blockSize)) & mask;
    Addr foldedBits = foldedHist & mask;

    return xorAsidHashIntoIndex(pcBits ^ foldedBits, tableIndexBits, asidHash);
}

Addr
BTBMGSC::getBiasIndex(Addr pc, unsigned tableIndexBits, bool lowbit0,
                      bool lowbit1, uint8_t asidHash)
{
    // Create mask for tableIndexBits-2 to extract PC bits
    Addr mask = (1ULL << (tableIndexBits - 2)) - 1;

    // Extract lower bits of PC directly and combine with low bits
    Addr pcBits = (pc >> floorLog2(blockSize)) & mask;
    unsigned index = (pcBits << 2) + (lowbit1 << 1) + lowbit0;
    return xorAsidHashIntoIndex(index, tableIndexBits, asidHash);
}

Addr
BTBMGSC::getPcIndex(Addr pc, unsigned tableIndexBits, uint8_t asidHash)
{
    // Create mask to extract tableIndexBits from PC
    Addr mask = (1ULL << tableIndexBits) - 1;

    // Extract lower bits of PC directly without bitset
    Addr baseIndex = (pc >> floorLog2(blockSize)) & mask;
    return xorAsidHashIntoIndex(baseIndex, tableIndexBits, asidHash);
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
template<typename T>
void
BTBMGSC::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt, bool taken, std::vector<T> &foldedHist,
                      Addr pc, Addr target)
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
 * @brief Speculatively updates global folded histories.
 */
void
BTBMGSC::specUpdateGHist(const boost::dynamic_bitset<> &history,
                        FullBTBPrediction &pred,
                        const DirectionHistoryUpdate &update)
{
    auto &state = historyState(pred.tid);
    doUpdateHist(history, update.shamt, update.taken,
                 state.indexGFoldedHist);  // use global history to update G folded history
}

/**
 * @brief Speculatively updates path folded histories.
 */
void
BTBMGSC::specUpdatePHist(const boost::dynamic_bitset<> &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update)
{
    auto &state = historyState(pred.tid);
    doUpdateHist(history, update.shamt, update.taken, state.indexPFoldedHist,
                 update.pc, update.target);  // only path history needs pc!
}


/**
 * @brief Speculatively updates global backward folded histories.
 */
void
BTBMGSC::specUpdateBwHist(const boost::dynamic_bitset<> &history,
                          FullBTBPrediction &pred,
                          const DirectionHistoryUpdate &update)
{
    auto &state = historyState(pred.tid);
    doUpdateHist(history, update.shamt, update.taken, state.indexBwFoldedHist);
}

/**
 * @brief Speculatively updates IMLI folded histories.
 */
void
BTBMGSC::specUpdateIHist(FullBTBPrediction &pred,
                         const DirectionHistoryUpdate &update)
{
    auto &state = historyState(pred.tid);
    // IMLI uses counter only, pass empty bitset (not used by ImliFoldedHist::update)
    boost::dynamic_bitset<> dummy;
    doUpdateHist(dummy, update.shamt, update.taken, state.indexIFoldedHist);
}

/**
 * @brief Speculatively updates local folded histories.
 */
void
BTBMGSC::specUpdateLHist(const std::vector<boost::dynamic_bitset<>> &history,
                         FullBTBPrediction &pred,
                         const DirectionHistoryUpdate &update)
{
    auto &state = historyState(pred.tid);
    const Addr localHistoryIndex =
        getPcIndex(pred.bbStart, log2(numEntriesFirstLocalHistories), pred.asidHash);
    doUpdateHist(history[localHistoryIndex], update.shamt, update.taken,
                 state.indexLFoldedHist[localHistoryIndex]);
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
BTBMGSC::recoverHist(const boost::dynamic_bitset<> &history, const FetchTarget &entry, int shamt, bool cond_taken)
{
    if (!isEnabled()) {
        return;  // No recover when disabled
    }
    auto &state = historyState(entry.tid);
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < gTableNum; i++) {
        state.indexGFoldedHist[i].recover(predMeta->indexGFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken, state.indexGFoldedHist);
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
                      const FetchTarget &entry,
                      const PathHistoryUpdate &update)
{
    if (!isEnabled()) {
        return;  // No recover when disabled
    }
    auto &state = historyState(entry.tid);
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < pTableNum; i++) {
        state.indexPFoldedHist[i].recover(predMeta->indexPFoldedHist[i]);
    }
    doUpdateHist(history, update.shamt, update.taken, state.indexPFoldedHist,
                 update.pc, update.target);
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
BTBMGSC::recoverBwHist(const boost::dynamic_bitset<> &history, const FetchTarget &entry, int shamt, bool cond_taken)
{
    if (!isEnabled()) {
        return;  // No recover when disabled
    }
    auto &state = historyState(entry.tid);
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < bwTableNum; i++) {
        state.indexBwFoldedHist[i].recover(predMeta->indexBwFoldedHist[i]);
    }
    doUpdateHist(history, shamt, cond_taken, state.indexBwFoldedHist);
}

/**
 * @brief Recovers branch imli history state after a misprediction
 *
 * This function:
 * 1. Restores the folded histories from the saved metadata
 * 2. Updates the histories with the correct branch outcome
 * 3. Ensures predictor state is consistent after recovery
 * Note: IMLI only uses counter, not history bits.
 *
 * @param entry The fetch stream entry containing recovery information
 * @param shamt Number of bits to shift in history update
 * @param cond_taken The actual branch outcome
 */
void
BTBMGSC::recoverIHist(const FetchTarget &entry, int shamt, bool cond_taken)
{
    if (!isEnabled()) {
        return;  // No recover when disabled
    }
    auto &state = historyState(entry.tid);
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < iTableNum; i++) {
        state.indexIFoldedHist[i].recover(predMeta->indexIFoldedHist[i]);
    }
    // IMLI uses counter only, pass empty bitset (not used by ImliFoldedHist::update)
    boost::dynamic_bitset<> dummy;
    doUpdateHist(dummy, shamt, cond_taken, state.indexIFoldedHist);
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
BTBMGSC::recoverLHist(const std::vector<boost::dynamic_bitset<>> &history, const FetchTarget &entry, int shamt,
                      bool cond_taken)
{
    if (!isEnabled()) {
        return;  // No recover when disabled
    }
    auto &state = historyState(entry.tid);
    std::shared_ptr<MgscMeta> predMeta = std::static_pointer_cast<MgscMeta>(entry.predMetas[getComponentIdx()]);
    for (unsigned int k = 0; k < numEntriesFirstLocalHistories; ++k) {
        for (int i = 0; i < lTableNum; i++) {
            state.indexLFoldedHist[k][i].recover(predMeta->indexLFoldedHist[k][i]);
        }
    }
    const Addr localHistoryIndex =
        getPcIndex(entry.startPC, log2(numEntriesFirstLocalHistories), entry.asidHash);
    doUpdateHist(history[localHistoryIndex], shamt, cond_taken,
                 state.indexLFoldedHist[localHistoryIndex]);
}

#ifndef UNIT_TEST
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
      ADD_STAT(scNotUsed, statistics::units::Count::get(), "number of sc not used"),

      ADD_STAT(predHit, statistics::units::Count::get(), "number of sc prediction hit"),
      ADD_STAT(predMiss, statistics::units::Count::get(), "number of sc prediction miss"),
      ADD_STAT(scPredCorrect, statistics::units::Count::get(), "number of sc prediction correct"),
      ADD_STAT(scPredWrong, statistics::units::Count::get(), "number of sc prediction wrong"),
      ADD_STAT(scPredMissTaken, statistics::units::Count::get(), "number of sc prediction miss taken"),
      ADD_STAT(scPredMissNotTaken, statistics::units::Count::get(), "number of sc prediction miss not taken"),
      ADD_STAT(scPredCorrectTageWrong, statistics::units::Count::get(),"number of sc prediction correct and tage wrong"),
      ADD_STAT(scPredWrongTageCorrect, statistics::units::Count::get(),"number of sc prediction wrong and tage correct"),

      ADD_STAT(bwWeightScaleDiff, statistics::units::Count::get(), "bw table weight scaling decisive"),
      ADD_STAT(lWeightScaleDiff, statistics::units::Count::get(), "l table weight scaling decisive"),
      ADD_STAT(iWeightScaleDiff, statistics::units::Count::get(), "i table weight scaling decisive"),
      ADD_STAT(gWeightScaleDiff, statistics::units::Count::get(), "g table weight scaling decisive"),
      ADD_STAT(pWeightScaleDiff, statistics::units::Count::get(), "p table weight scaling decisive"),
      ADD_STAT(biasWeightScaleDiff, statistics::units::Count::get(), "bias table weight scaling decisive"),

      ADD_STAT(bwPercsumCorrect, statistics::units::Count::get(), "bw table raw percsum sign correct"),
      ADD_STAT(bwPercsumWrong, statistics::units::Count::get(), "bw table raw percsum sign wrong"),
      ADD_STAT(lPercsumCorrect, statistics::units::Count::get(), "l table raw percsum sign correct"),
      ADD_STAT(lPercsumWrong, statistics::units::Count::get(), "l table raw percsum sign wrong"),
      ADD_STAT(iPercsumCorrect, statistics::units::Count::get(), "i table raw percsum sign correct"),
      ADD_STAT(iPercsumWrong, statistics::units::Count::get(), "i table raw percsum sign wrong"),
      ADD_STAT(gPercsumCorrect, statistics::units::Count::get(), "g table raw percsum sign correct"),
      ADD_STAT(gPercsumWrong, statistics::units::Count::get(), "g table raw percsum sign wrong"),
      ADD_STAT(pPercsumCorrect, statistics::units::Count::get(), "p table raw percsum sign correct"),
      ADD_STAT(pPercsumWrong, statistics::units::Count::get(), "p table raw percsum sign wrong"),
      ADD_STAT(biasPercsumCorrect, statistics::units::Count::get(), "bias table raw percsum sign correct"),
      ADD_STAT(biasPercsumWrong, statistics::units::Count::get(), "bias table raw percsum sign wrong"),

      ADD_STAT(pcThresholdInc, statistics::units::Count::get(), "pc threshold increment"),
      ADD_STAT(pcThresholdDec, statistics::units::Count::get(), "pc threshold decrement"),
      ADD_STAT(globalThresholdInc, statistics::units::Count::get(), "global threshold increment"),
      ADD_STAT(globalThresholdDec, statistics::units::Count::get(), "global threshold decrement"),

      ADD_STAT(scHighUseCorrect, statistics::units::Count::get(), "tage high conf, sc used, correct"),
      ADD_STAT(scHighUseWrong, statistics::units::Count::get(), "tage high conf, sc used, wrong"),
      ADD_STAT(scMidUseCorrect, statistics::units::Count::get(), "tage mid conf, sc used, correct"),
      ADD_STAT(scMidUseWrong, statistics::units::Count::get(), "tage mid conf, sc used, wrong"),
      ADD_STAT(scLowUseCorrect, statistics::units::Count::get(), "tage low conf, sc used, correct"),
      ADD_STAT(scLowUseWrong, statistics::units::Count::get(), "tage low conf, sc used, wrong"),
      ADD_STAT(scHighBypass, statistics::units::Count::get(), "tage high conf, sc not used"),
      ADD_STAT(scMidBypass, statistics::units::Count::get(), "tage mid conf, sc not used"),
      ADD_STAT(scLowBypass, statistics::units::Count::get(), "tage low conf, sc not used")
{
}
#endif

#ifndef UNIT_TEST
void
BTBMGSC::recordCommittedBranchStats(
    const ResolvedBranch &branch,
    const std::shared_ptr<void> &prediction_meta)
{
    if (!branch.isCond) {
        // tage olnly deals with conditional branches
        return;
    }
    auto meta = std::static_pointer_cast<MgscMeta>(prediction_meta);
    auto pc = branch.pc;
    auto pred_it = meta->preds.find(pc);
    bool pred_hit = false;
    bool sc_taken = false;
    bool tage_taken = false;
    if (pred_it != meta->preds.end()) {
        sc_taken =pred_it->second.taken;
        tage_taken = pred_it->second.taken_before_sc;
        pred_hit = true;
    }
    if (pred_hit) {
        mgscStats.predHit++;
        if (sc_taken == branch.taken) {
            mgscStats.scPredCorrect++;
            if (sc_taken != tage_taken) {
                mgscStats.scPredCorrectTageWrong++;
            }
        } else {
            mgscStats.scPredWrong++;
            if (tage_taken == branch.taken) {
                mgscStats.scPredWrongTageCorrect++;
            }
        }
    }else {
        mgscStats.predMiss++;
        if (branch.taken) {
            mgscStats.scPredMissTaken++;
            mgscStats.scPredWrong++;
            if (branch.taken == tage_taken) {
                mgscStats.scWrongTageCorrect++;
            }
        } else {
            mgscStats.scPredMissNotTaken++;
            mgscStats.scPredCorrect++;
            if (sc_taken != tage_taken) {
                mgscStats.scPredCorrectTageWrong++;
            }
        }
    }

}
#endif

void
BTBMGSC::checkFoldedHist(const boost::dynamic_bitset<> &Ghistory, const boost::dynamic_bitset<> &PHistory,
                         const std::vector<boost::dynamic_bitset<>> &LHistory, const char *when)
{
    checkFoldedHist(Ghistory, PHistory, LHistory, 0, when);
}

void
BTBMGSC::checkFoldedHist(const boost::dynamic_bitset<> &Ghistory, const boost::dynamic_bitset<> &PHistory,
                         const std::vector<boost::dynamic_bitset<>> &LHistory,
                         ThreadID tid, const char *when)
{
    auto &state = historyState(tid);
    DPRINTF(MGSC, "checking folded history when %s\n", when);
    if (debug::MGSC) {
        std::string hist_str;
        boost::to_string(Ghistory, hist_str);
        DPRINTF(MGSC, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < gTableNum; t++) {
        auto &foldedHist = state.indexGFoldedHist[t];
        foldedHist.check(Ghistory);
    }
    for (int t = 0; t < pTableNum; t++) {
        auto &foldedHist = state.indexPFoldedHist[t];
        foldedHist.check(PHistory);
    }
    for (int t = 0; t < lTableNum; t++) {
        assert(LHistory.size() == state.indexLFoldedHist.size());
        for (int i = 0; i < LHistory.size(); i++) {
            auto &foldedHist = state.indexLFoldedHist[i][t];
            foldedHist.check(LHistory[i]);
        }
    }
}

#ifdef UNIT_TEST
}  // namespace test
#endif

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
