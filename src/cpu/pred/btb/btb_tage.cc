#include "cpu/pred/btb/btb_tage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#ifdef UNIT_TEST
// Define debug flags for unit testing
namespace gem5 {
namespace debug {
    bool TAGEUseful = true;
    bool TAGEHistory = true;
}
}
#endif

#ifndef UNIT_TEST
#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/TAGE.hh"
#endif
namespace gem5 {

namespace branch_prediction {

namespace btb_pred{

namespace
{

#ifndef UNIT_TEST
inline uint64_t
mixTraceHash(uint64_t value)
{
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

uint64_t
hashBitset(const boost::dynamic_bitset<> &bits)
{
    uint64_t seed = mixTraceHash(bits.size());
    for (size_t pos = bits.find_first();
         pos != boost::dynamic_bitset<>::npos;
         pos = bits.find_next(pos)) {
        seed ^= mixTraceHash(static_cast<uint64_t>(pos) + 0x9e3779b97f4a7c15ULL +
                             (seed << 6) + (seed >> 2));
    }
    return seed;
}

uint64_t
hashFoldedHistVec(const std::vector<TageFoldedHist> &folded)
{
    uint64_t seed = mixTraceHash(folded.size());
    for (size_t i = 0; i < folded.size(); ++i) {
        uint64_t value = folded[i].get();
        value ^= static_cast<uint64_t>(folded[i].getHistoryType()) << 56;
        seed ^= mixTraceHash(value + static_cast<uint64_t>(i) * 0x9e3779b97f4a7c15ULL +
                             (seed << 6) + (seed >> 2));
    }
    return seed;
}
#endif

} // anonymous namespace

#ifdef UNIT_TEST
namespace test {
#endif

#ifdef UNIT_TEST
// Test constructor for unit testing mode
BTBTAGE::BTBTAGE(unsigned numPredictors, unsigned numWaysPerTable,
                 unsigned tableSize, unsigned numBanks, bool usePathHistory)
    : TimedBaseBTBPredictor(),
      numPredictors(numPredictors),
      usePathHistory(usePathHistory),
      maxHistLen(0),
      numWays(numPredictors, numWaysPerTable),
      maxBranchPositions(32),
      useAltOnNaSize(1024),
      useAltOnNaWidth(7),
      updateOnRead(false),
      numBanks(numBanks),
      bankIdWidth(ceilLog2(numBanks)),
      blockWidth(floorLog2(blockSize)),
      bankBaseShift(instShiftAmt),
      indexShift(bankBaseShift + ceilLog2(numBanks)),
      enableBankConflict(false),
      lastPredBankId(0),
      predBankValid(false)
{
    setNumDelay(1);

    // Initialize with default parameters for testing
    tableSizes.resize(numPredictors, tableSize);
    tableTagBits.resize(numPredictors, 8);
    tablePcShifts.resize(numPredictors, 1);
    histLengths.resize(numPredictors);
    for (unsigned i = 0; i < numPredictors; ++i) {
        histLengths[i] = (i + 1) * 4;
    }
    maxHistLen = histLengths[numPredictors-1];
    numTablesToAlloc = 1;
    enableSC = false;
    tageStats.init(numPredictors, numBanks);
#else
// Constructor: Initialize TAGE predictor with given parameters
BTBTAGE::BTBTAGE(const Params& p):
TimedBaseBTBPredictor(p),
numPredictors(p.numPredictors),
tableSizes(p.tableSizes),
tableTagBits(p.TTagBitSizes),
tablePcShifts(p.TTagPcShifts),
histLengths(p.histLengths),
usePathHistory(p.usePathHistory),
maxHistLen(p.maxHistLen),
numWays(p.numWays),
maxBranchPositions(p.maxBranchPositions),
useAltOnNaSize(p.useAltOnNaSize),
useAltOnNaWidth(p.useAltOnNaWidth),
numTablesToAlloc(p.numTablesToAlloc),
enableSC(p.enableSC),
updateOnRead(p.updateOnRead),
numBanks(p.numBanks),
bankIdWidth(ceilLog2(p.numBanks)),
blockWidth(floorLog2(blockSize)),
bankBaseShift(instShiftAmt), // strip instruction alignment bits before indexing
indexShift(bankBaseShift + ceilLog2(p.numBanks)),
enableBankConflict(p.enableBankConflict),
lastPredBankId(0),
predBankValid(false),
tageStats(this, p.numPredictors, p.numBanks)
{
    // Warn if updateOnRead is disabled (bank simulation works better with it enabled)
    if (!p.updateOnRead) {
        warn("BTBTAGE: Bank simulation works better with updateOnRead=true");
    }
#endif
    if (numWays.size() == 1 && numPredictors > 1) {
        numWays.resize(numPredictors, numWays.front());
    }

    assert(numWays.size() >= numPredictors);
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);

    threadHistory.resize(MaxThreads);
    threadMeta.resize(MaxThreads);

    for (unsigned int i = 0; i < numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);
        const unsigned ways = getNumWays(i);
        for (unsigned int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(ways);
        }

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(tableIndexBits[i], true);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(tableTagBits[i], true);

        assert(tablePcShifts.size() >= numPredictors);
        const auto historyType =
            usePathHistory ? HistoryType::PATH : HistoryType::GLOBAL;
        for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
            auto &state = threadHistory[tid];
            state.tagFoldedHist.emplace_back((int)histLengths[i], (int)tableTagBits[i], 16, historyType);
            state.altTagFoldedHist.emplace_back((int)histLengths[i], (int)tableTagBits[i] - 1, 16, historyType);
            state.indexFoldedHist.emplace_back(
                (int)histLengths[i],
                (int)partitionIndexBits(tableIndexBits[i]), 16,
                historyType);
        }
    }
    usefulResetCnt = 0;

    // initialize use_alt_on_na table
    useAlt.resize(useAltOnNaSize, 0);
#ifndef UNIT_TEST
    hasDB = true;
    switch (getDelay()) {
        case 0: dbName = std::string("microtage"); break;
        default: dbName = std::string("tage"); break;
    }
#endif
}

BTBTAGE::~BTBTAGE()
{
}

ThreadID
BTBTAGE::predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const
{
    assert(!stagePreds.empty());
    return stagePreds.front().tid;
}

BTBTAGE::ThreadHistoryState &
BTBTAGE::historyState(ThreadID tid)
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

const BTBTAGE::ThreadHistoryState &
BTBTAGE::historyState(ThreadID tid) const
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

// Set up tracing for debugging
void
BTBTAGE::setTrace()
{
#ifndef UNIT_TEST
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("startPC", UINT64),
            std::make_pair("branchPC", UINT64),
            std::make_pair("wayIdx", UINT64),
            std::make_pair("mainFound", UINT64),
            std::make_pair("mainCounter", UINT64),
            std::make_pair("mainUseful", UINT64),
            std::make_pair("mainTable", UINT64),
            std::make_pair("mainIndex", UINT64),
            std::make_pair("mainTag", UINT64),
            std::make_pair("altFound", UINT64),
            std::make_pair("altCounter", UINT64),
            std::make_pair("altUseful", UINT64),
            std::make_pair("altTable", UINT64),
            std::make_pair("altIndex", UINT64),
            std::make_pair("altTag", UINT64),
            std::make_pair("useAlt", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("actualTaken", UINT64),
            std::make_pair("allocSuccess", UINT64),
            std::make_pair("allocTable", UINT64),
            std::make_pair("allocIndex", UINT64),
            std::make_pair("allocWay", UINT64),
            std::make_pair("allocTag", UINT64),
            std::make_pair("victimValid", UINT64),
            std::make_pair("victimTag", UINT64),
            std::make_pair("victimCounter", UINT64),
            std::make_pair("victimUseful", UINT64),
            std::make_pair("victimPC", UINT64),
            std::make_pair("history", TEXT),
            std::make_pair("indexFoldedHist", UINT64),
            std::make_pair("phistory", TEXT),
            std::make_pair("useAltIdx", UINT64),
            std::make_pair("useAltCtr", UINT64),
            std::make_pair("hitTableMask", UINT64),
            std::make_pair("finalProviderTable", UINT64),
            std::make_pair("finalProviderIsAlt", UINT64),
            std::make_pair("historyHash", UINT64),
            std::make_pair("phistoryHash", UINT64),
            std::make_pair("indexFoldedHistHash", UINT64),
            std::make_pair("tagFoldedHistHash", UINT64),
            std::make_pair("altTagFoldedHistHash", UINT64),
        };
        tageMissTrace = _db->addAndGetTrace("TAGEMISSTRACE", fields_vec);
        tageMissTrace->init_table();
    }
#endif
}

void
BTBTAGE::tick() {}

void
BTBTAGE::tickStart() {}

/**
 * @brief Generate prediction for a single BTB entry by searching TAGE tables
 *
 * @param btb_entry The BTB entry to generate prediction for
 * @param startPC The starting PC address for calculating indices and tags
 * @param predMeta Optional prediction metadata; if provided, use snapshot for index/tag
 *             calculation (update path); if nullptr, use current folded history (prediction path)
 * @return TagePrediction containing main and alternative predictions
 */
BTBTAGE::TagePrediction
BTBTAGE::generateSinglePrediction(const BTBEntry &btb_entry,
                                 const Addr &startPC,
                                 std::shared_ptr<TageMeta> predMeta,
                                 ThreadID tid,
                                 uint8_t asidHash) const
{
    DPRINTF(TAGE, "generateSinglePrediction for btbEntry: %#lx\n", btb_entry.pc);
    const auto &state = historyState(tid);

    // Find main and alternative predictions
    bool provided = false;
    bool alt_provided = false;
    TageTableInfo main_info, alt_info;
    uint64_t hit_table_mask = 0;

    // Search from highest to lowest table for matches
    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(btb_entry.pc, startPC);

    for (int i = numPredictors - 1; i >= 0; --i) {
        // Calculate index and tag: use snapshot if provided, otherwise use current folded history
        // Tag includes position XOR (like RTL: tag = tempTag ^ cfiPosition)
        Addr index = predMeta ? getTageIndex(
            startPC, i, predMeta->indexFoldedHist[i].get(), asidHash, tid)
                              : getTageIndex(
            startPC, i, state.indexFoldedHist[i].get(), asidHash, tid);
        Addr tag = predMeta ? getTageTag(startPC, i,
                            predMeta->tagFoldedHist[i].get(), predMeta->altTagFoldedHist[i].get(),
                            position, asidHash)
                        : getTageTag(startPC, i, state.tagFoldedHist[i].get(),
                                     state.altTagFoldedHist[i].get(), position, asidHash);

        bool match = false; // for each table, only one way can be matched
        TageEntry matching_entry;
        unsigned matching_way = 0;

        // Search all ways for a matching entry
        const unsigned ways = getNumWays(i);
        for (unsigned way = 0; way < ways; way++) {
            auto &entry = tageTable[i][index][way];
            // entry valid, tag match (position already encoded in tag, no need to check pc)
            if (entry.valid && tag == entry.tag) {
                matching_entry = entry;
                matching_way = way;
                match = true;

                // Do not use LRU; keep logic simple and align with CBP-style replacement

                DPRINTF(TAGE, "hit  table %d[%lu][%u]: valid %d, tag %lu, ctr %d, useful %d, btb_pc %#lx, pos %u\n",
                    i, index, way, entry.valid, entry.tag, entry.counter, entry.useful, btb_entry.pc, position);
                break;  // only one way can be matched, aviod multi hit, TODO: RTL how to do this?
            }
        }

        if (match) {
            if (i < 64) {
                hit_table_mask |= (1ULL << i);
            }
            if (!provided) {
                // First match becomes main prediction
                main_info = TageTableInfo(true, matching_entry, i, index, tag, matching_way);
                provided = true;
            } else if (!alt_provided) {
                // Second match becomes alternative prediction
                alt_info = TageTableInfo(true, matching_entry, i, index, tag, matching_way);
                alt_provided = true;
                break;
            }
        } else {
            DPRINTF(TAGE, "miss table %d[%lu] for tag %lu (with pos %u), btb_pc %#lx\n",
                i, index, tag, position, btb_entry.pc);
        }
    }

    // Generate final prediction
    bool main_taken = main_info.taken();
    bool alt_taken = alt_info.taken();
    // Use base table instead of btb_entry.ctr
    bool base_taken = btb_entry.ctr >= 0;
    //bool base_taken = btb_entry.ctr >= 0;
    bool alt_pred = alt_provided ? alt_taken : base_taken; // if alt provided, use alt prediction, otherwise use base
    Addr use_alt_idx = getUseAltIdx(btb_entry.pc);
    short use_alt_ctr = useAlt[use_alt_idx];

    // use_alt_on_na gating: when provider weak, consult per-PC counter
    bool use_alt = false;
    if (!provided) {
        use_alt = true;
    } else {
        bool main_weak = (main_info.entry.counter == 0 || main_info.entry.counter == -1);
        if (main_weak) {
            use_alt = (use_alt_ctr >= 0);
        } else {
            use_alt = false;
        }
    }
    bool taken = use_alt ? alt_pred : main_taken;
    int final_provider_table = -1;
    bool final_provider_is_alt = false;
    if (!use_alt && provided) {
        final_provider_table = main_info.table;
    } else if (use_alt && alt_provided) {
        final_provider_table = alt_info.table;
        final_provider_is_alt = true;
    }

    DPRINTF(TAGE, "tage predict %#lx taken %d\n", btb_entry.pc, taken);
    DPRINTF(TAGE, "tage use_alt %d ? (alt_provided %d ? alt_taken %d : base_taken %d) : main_taken %d\n",
        use_alt, alt_provided, alt_taken, base_taken, main_taken);
    DPRINTF(TAGE, "tage final source %#lx table %d alt %d\n",
        btb_entry.pc, final_provider_table, final_provider_is_alt);

    return TagePrediction(btb_entry.pc, main_info, alt_info, use_alt, taken,
        alt_pred, final_provider_table, final_provider_is_alt, use_alt_idx,
        use_alt_ctr, hit_table_mask);
}

/**
 * @brief Look up predictions in TAGE tables for a stream of instructions
 * 
 * @param startPC The starting PC address for the instruction stream
 * @param btbEntries Vector of BTB entries to make predictions for
 * @return Map of branch PC addresses to their predicted outcomes
 */
void
BTBTAGE::lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
                      std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs,
                      CondTakens& results, ThreadID tid, uint8_t asidHash)
{
    DPRINTF(TAGE, "lookupHelper startAddr: %#lx\n", startPC);

    // Process each BTB entry to make predictions
    for (auto &btb_entry : btbEntries) {
        // Only predict for valid conditional branches
        if (btb_entry.isCond && btb_entry.valid) {
            auto pred = generateSinglePrediction(btb_entry, startPC, nullptr, tid, asidHash);
            threadMeta[tid]->preds[btb_entry.pc] = pred;
            tageStats.updateStatsWithTagePrediction(pred, true);
            results.push_back({btb_entry.pc, pred.taken || btb_entry.alwaysTaken});
            tageInfoForMgscs[btb_entry.pc].tage_pred_taken = pred.taken;
            tageInfoForMgscs[btb_entry.pc].tage_main_taken = pred.mainInfo.found ? pred.mainInfo.taken() : false;
            tageInfoForMgscs[btb_entry.pc].tage_pred_conf_high = pred.mainInfo.found &&
                                         abs(pred.mainInfo.entry.counter*2 + 1) == 7; // counter saturated, -4 or 3
            tageInfoForMgscs[btb_entry.pc].tage_pred_conf_mid = pred.mainInfo.found &&
                                         (abs(pred.mainInfo.entry.counter*2 + 1) < 7 &&
                                         abs(pred.mainInfo.entry.counter*2 + 1) > 1); // counter not saturated, -3, -2, 1, 2
            tageInfoForMgscs[btb_entry.pc].tage_pred_conf_low = !pred.mainInfo.found ||
                                         (abs(pred.mainInfo.entry.counter*2 + 1) <= 1); // counter initialized, -1 or 0
            // main predict is different from alt predict/base predict
            tageInfoForMgscs[btb_entry.pc].tage_pred_alt_diff = pred.mainInfo.found && pred.mainInfo.taken() != pred.altPred;
        }
    }
}

void
BTBTAGE::lookupNoSideEffect(const Addr &startPC,
                            const std::vector<BTBEntry> &btbEntries,
                            CondTakens &results,
                            ThreadID tid,
                            uint8_t asidHash) const
{
    for (const auto &btb_entry : btbEntries) {
        if (btb_entry.isCond && btb_entry.valid) {
            auto pred = generateSinglePrediction(
                btb_entry, startPC, nullptr, tid, asidHash);
            results.push_back({btb_entry.pc, pred.taken || btb_entry.alwaysTaken});
        }
    }
}

void
BTBTAGE::dryRunCycle(Addr startPC) {
    // No operation in dry run cycle for BTBTAGE
    // Record prediction bank for next tick's conflict detection
    lastPredBankId = getBankId(startPC);
    predBankValid = true;

    return;
}

/**
 * @brief Makes predictions for a stream of instructions using TAGE predictor
 * 
 * This function is called during the prediction stage and:
 * 1. Uses lookupHelper to get predictions for all BTB entries
 * 2. Stores predictions in the stage prediction structure
 * 3. Handles multiple prediction stages with different delays
 * 
 * @param startPC Starting PC of the instruction stream
 * @param history Current branch history
 * @param stagePreds Vector of predictions for different pipeline stages
 */
void
BTBTAGE::putPCHistory(Addr startPC, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) {
    const ThreadID tid = predictorTid(stagePreds);
    const uint8_t asidHash = stagePreds.empty() ? 0 : stagePreds.front().asidHash;
    const auto &state = historyState(tid);
    // Record prediction bank for next tick's conflict detection
    lastPredBankId = getBankId(startPC);
    predBankValid = true;

    // Record prediction access per bank
    tageStats.predAccessPerBank[lastPredBankId]++;

    DPRINTF(TAGE, "putPCHistory startAddr: %#lx, bank: %u\n",
            startPC, lastPredBankId);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it

    // Clear old prediction metadata and save current history state
    threadMeta[tid] = std::make_shared<TageMeta>();
    threadMeta[tid]->tagFoldedHist = state.tagFoldedHist;
    threadMeta[tid]->altTagFoldedHist = state.altTagFoldedHist;
    threadMeta[tid]->indexFoldedHist = state.indexFoldedHist;
    threadMeta[tid]->history = history;

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        stage_pred.condTakens.clear();
        lookupHelper(startPC, stage_pred.btbEntries, stage_pred.tageInfoForMgscs,
                     stage_pred.condTakens, tid, asidHash);
    }

}

std::shared_ptr<void>
BTBTAGE::getPredictionMeta(ThreadID tid) {
    if (tid >= threadMeta.size()) {
        return nullptr;
    }
    return threadMeta[tid];
}

void
BTBTAGE::refreshPredictionMeta(Addr startPC,
                               const bitset &history,
                               FullBTBPrediction &pred)
{
    auto &state = historyState(pred.tid);
    threadMeta[pred.tid] = std::make_shared<TageMeta>();
    auto &meta = threadMeta[pred.tid];
    meta->tagFoldedHist = state.tagFoldedHist;
    meta->altTagFoldedHist = state.altTagFoldedHist;
    meta->indexFoldedHist = state.indexFoldedHist;
    meta->history = history;

    pred.tageInfoForMgscs.clear();
    for (const auto &btb_entry : pred.btbEntries) {
        if (!(btb_entry.isCond && btb_entry.valid)) {
            continue;
        }

        auto tage_pred = generateSinglePrediction(
            btb_entry, startPC, nullptr, pred.tid, pred.asidHash);
        meta->preds[btb_entry.pc] = tage_pred;

        auto &tage_info = pred.tageInfoForMgscs[btb_entry.pc];
        tage_info.tage_pred_taken = tage_pred.taken;
        tage_info.tage_main_taken =
            tage_pred.mainInfo.found ? tage_pred.mainInfo.taken() : false;
        tage_info.tage_pred_conf_high = tage_pred.mainInfo.found &&
            abs(tage_pred.mainInfo.entry.counter * 2 + 1) == 7;
        tage_info.tage_pred_conf_mid = tage_pred.mainInfo.found &&
            (abs(tage_pred.mainInfo.entry.counter * 2 + 1) < 7 &&
             abs(tage_pred.mainInfo.entry.counter * 2 + 1) > 1);
        tage_info.tage_pred_conf_low = !tage_pred.mainInfo.found ||
            (abs(tage_pred.mainInfo.entry.counter * 2 + 1) <= 1);
        tage_info.tage_pred_alt_diff = tage_pred.mainInfo.found &&
            tage_pred.mainInfo.taken() != tage_pred.altPred;
    }
}

/**
 * @brief Update predictor state for a single entry
 * 
 * @param entry The BTB entry being updated
 * @param actual_taken The actual outcome of the branch
 * @param pred The prediction made for this entry
 * @param stream The fetch stream containing update information
 * @return true if need to allocate new entry
 */
bool
BTBTAGE::updatePredictorStateAndCheckAllocation(const BTBEntry &entry,
                             bool actual_taken,
                             const TagePrediction &pred,
                             bool control_mispred) {
    tageStats.updateStatsWithTagePrediction(pred, false);

    auto &main_info = pred.mainInfo;
    auto &alt_info = pred.altInfo;
    bool used_alt = pred.useAlt;
    // Use base table instead of entry.ctr for fallback prediction
    bool base_taken = entry.ctr >= 0;
    bool alt_taken = alt_info.found ? alt_info.taken() : base_taken;
    bool use_provider = main_info.found && !used_alt;
    bool use_alt_table = used_alt && alt_info.found;
    bool use_base_table = !use_provider && !use_alt_table;

    tageStats.resolveBranchHasProvider += main_info.found;
    tageStats.resolveBranchUseProvider += use_provider;
    tageStats.resolveBranchHasAlt += alt_info.found;
    tageStats.resolveBranchUseAltTable += use_alt_table;
    tageStats.resolveBranchUseBaseTable += use_base_table;
    if (main_info.found) {
        tageStats.resolveProviderTable[main_info.table]++;
    }
    if (alt_info.found) {
        tageStats.resolveAltTable[alt_info.table]++;
    }
    if (use_provider) {
        tageStats.resolveUseProviderTable[main_info.table]++;
    }
    if (use_alt_table) {
        tageStats.resolveUseAltTable[alt_info.table]++;
    }

    // Update use_alt_on_na when provider is weak (0 or -1)
    if (main_info.found) {
        bool main_weak = (main_info.entry.counter == 0 || main_info.entry.counter == -1);
        if (main_weak) {
            tageStats.updateProviderNa++;
            Addr uidx = getUseAltIdx(entry.pc);
            bool alt_correct = (alt_taken == actual_taken);
            updateCounter(alt_correct, useAltOnNaWidth, useAlt[uidx]);
            tageStats.updateUseAltOnNaUpdated++;
            if (alt_correct) {
                tageStats.updateUseAltOnNaCorrect++;
            } else {
                tageStats.updateUseAltOnNaWrong++;
            }
        }
    }

    // Update main prediction provider
    if (main_info.found) {
        DPRINTF(TAGE, "prediction provided by table %d, idx %lu, way %u, updating corresponding entry\n",
            main_info.table, main_info.index, main_info.way);

        auto &way = tageTable[main_info.table][main_info.index][main_info.way];

        // Update prediction counter
        updateCounter(actual_taken, 3, way.counter);

        // Match RTL behavior: useful only increases when the provider proves
        // itself against the alternative prediction. There is no local
        // decrement/reset path tied to weak counters or "humility" cases.
        bool main_is_correct = main_info.taken() == actual_taken;
        if (main_info.taken() != alt_taken && main_is_correct) {
            way.useful = 1;
        }
        DPRINTF(TAGE, "useful bit is now %d\n", way.useful);

        // No LRU maintenance
    }

    // Update alternative prediction provider
    if (used_alt && alt_info.found) {
        auto &way = tageTable[alt_info.table][alt_info.index][alt_info.way];
        updateCounter(actual_taken, 3, way.counter);
        // No LRU maintenance
    }

    // Update statistics
    if (used_alt) {
        bool alt_correct = alt_taken == actual_taken;
        if (alt_correct) {
            tageStats.updateUseAltCorrect++;
        } else {
            tageStats.updateUseAltWrong++;
        }
        if (main_info.found && main_info.taken() != alt_taken) {
            tageStats.updateAltDiffers++;
        }
    }

    // Check if misprediction occurred
    bool this_fb_mispred = control_mispred;
    if (this_fb_mispred) {
        tageStats.mispredictBranchHasProvider += main_info.found;
        tageStats.mispredictBranchUseProvider += use_provider;
        tageStats.mispredictBranchHasAlt += alt_info.found;
        tageStats.mispredictBranchUseAltTable += use_alt_table;
        tageStats.mispredictBranchUseBaseTable += use_base_table;
        if (use_provider) {
            tageStats.mispredictUseProviderTable[main_info.table]++;
        }
        if (use_alt_table) {
            tageStats.mispredictUseAltTable[alt_info.table]++;
        }
    }
    if (getDelay() == 2){
        if (this_fb_mispred) {
            tageStats.updateMispred++;
            if (!used_alt && main_info.found) {
                tageStats.updateTableMispreds[main_info.table]++;
            }
        }
    }

    // No allocation if no misprediction
    if (!this_fb_mispred) {
        return false;
    }

    // Match RTL: a provider from the highest history table should not trigger
    // longer-history allocation.
    if (main_info.found && main_info.table == numPredictors - 1) {
        return false;
    }

    // Classic TAGE would also stop here when the provider is weak but its
    // direction matches the resolved outcome while the final prediction came
    // from alt/base. That rule assumes the provider only needs more training.
    //
    // For BTBTAGE with path history, h264ref's 0x588d6/0x58962 loop-phase
    // pattern shows a corner case where two opposite local contexts collide in
    // the same short-history entry, keeping the provider counter weak forever.
    // If we keep the classic gate here, the pattern never gets a chance to
    // allocate into a longer-history table and stays locked in the short table.
    //
    // Therefore we intentionally allow allocation to proceed even when:
    //   used_alt && main_info.found && main_info.taken() == actual_taken
    // so the minority pattern can escape to a longer-history entry.
    //
    // if (used_alt && main_info.found && main_info.taken() == actual_taken) {
    //     return false;
    // }

    // All other cases: allocate longer history table
    return true;
}

/**
 * @brief Handle allocation of new entries
 * 
 * @param startPC The starting PC address
 * @param entry The BTB entry being updated
 * @param actual_taken The actual outcome of the branch
 * @param start_table The starting table for allocation
 * @param meta The metadata of the predictor
 * @param asidHash The ASID hash used in TAGE index/tag calculation
 * @return true if allocation is successful
 */
bool
BTBTAGE::handleNewEntryAllocation(const Addr &startPC,
                                 const BTBEntry &entry,
                                 bool actual_taken,
                                 unsigned start_table,
                                 std::shared_ptr<TageMeta> meta,
                                 uint8_t asidHash,
                                 ThreadID tid,
                                 AllocationTraceInfo &allocInfo) {
    int &resetCnt = usesTidPartitionedStorage() ?
        usefulResetCntByThread[tid] : usefulResetCnt;
    // Match RTL victim priority:
    // 1) invalid way
    // 2) weak and not-useful way
    // 3) any not-useful way

    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(entry.pc, startPC);

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        Addr newIndex = getTageIndex(
            startPC, ti, meta->indexFoldedHist[ti].get(), asidHash, tid);
        Addr newTag = getTageTag(startPC, ti,
            meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get(), position, asidHash);

        auto &set = tageTable[ti][newIndex];

        const unsigned ways = getNumWays(ti);

        int selected_way = -1;
        for (unsigned way = 0; way < ways; ++way) {
            if (!set[way].valid) {
                selected_way = way;
                break;
            }
        }

        if (selected_way == -1) {
            for (unsigned way = 0; way < ways; ++way) {
                auto &cand = set[way];
                const bool weakish = std::abs(cand.counter * 2 + 1) <= 3;
                if (!cand.useful && weakish) {
                    selected_way = way;
                    break;
                }
            }
        }

        if (selected_way == -1) {
            for (unsigned way = 0; way < ways; ++way) {
                if (!set[way].useful) {
                    selected_way = way;
                    break;
                }
            }
        }

        if (selected_way != -1) {
            short newCounter = actual_taken ? 0 : -1;
            auto &victim = set[selected_way];
            DPRINTF(TAGE, "allocating entry in table %d[%lu][%u], tag %lu (with pos %u), counter %d, pc %#lx\n",
                    ti, newIndex, selected_way, newTag, position, newCounter, entry.pc);
            allocInfo.success = true;
            allocInfo.table = ti;
            allocInfo.index = newIndex;
            allocInfo.way = selected_way;
            allocInfo.tag = newTag;
            allocInfo.victimValid = victim.valid;
            allocInfo.victimTag = victim.tag;
            allocInfo.victimCounter = victim.counter;
            allocInfo.victimUseful = victim.useful;
            allocInfo.victimPC = victim.pc;
            set[selected_way] = TageEntry(newTag, newCounter, entry.pc); // u = 0 default
            tageStats.updateAllocSuccess++;
            resetCnt = resetCnt <= 0 ? 0 : resetCnt - 1;
            return true;
        }
        tageStats.updateAllocFailure++;
        resetCnt++;
    }

    if (resetCnt >= 256) {
        resetCnt = 0;
        tageStats.updateResetU++;
        DPRINTF(TAGE, "reset useful bit of all entries\n");
        for (auto &table : tageTable) {
            const unsigned begin = partitionBegin(table.size(), tid);
            const unsigned end = partitionEnd(table.size(), tid);
            for (unsigned index = begin; index < end; ++index) {
                for (auto &way : table[index]) {
                    way.useful = false;
                }
            }
        }
    }

    DPRINTF(TAGE, "no eligible way found for allocation starting from table %d\n", start_table);
    tageStats.updateAllocFailureNoValidTable++;
    return false;
}

/**
 * @brief Probe resolved update for bank conflicts without mutating state.
 * Returns false if the update cannot proceed due to a bank conflict.
 */
bool
BTBTAGE::canResolveUpdate(
    const FetchTarget &stream, const PreparedUpdate &update)
{
    Addr startAddr = stream.getRealStartPC();
    unsigned updateBank = getBankId(startAddr);

    // Record attempted update access per bank (even if it conflicts)
    tageStats.updateAccessPerBank[updateBank]++;

    if (enableBankConflict && predBankValid && updateBank == lastPredBankId) {
        tageStats.updateBankConflict++;
        tageStats.updateDeferredDueToConflict++;
        tageStats.updateBankConflictPerBank[updateBank]++;
        DPRINTF(TAGE, "Bank conflict detected: update bank %u conflicts with prediction bank %u, "
                      "deferring this update (will retry after blocking prediction)\n",
                      updateBank, lastPredBankId);
        predBankValid = false;
        return false;
    }

    return true;
}

/**
 * @brief Perform resolved update after probe success.
 */
void
BTBTAGE::doResolveUpdate(
    const FetchTarget &stream, const PreparedUpdate &update)
{
    if (enableBankConflict && predBankValid) {
        // Prediction consumed; clear bank tag for next cycle
        predBankValid = false;
    }
    this->update(stream, update);
}

/**
 * @brief Updates the TAGE predictor state based on actual branch execution results
 * 
 * @param stream The fetch stream containing branch execution information
 */
void
BTBTAGE::update(const FetchTarget &stream, const PreparedUpdate &update) {
    Addr startAddr = stream.getRealStartPC();
    unsigned updateBank = getBankId(startAddr);

    DPRINTF(TAGE, "update startAddr: %#lx, bank: %u\n", startAddr, updateBank);

    // Get prediction metadata snapshot and bind to member for helpers
    auto predMeta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    if (!predMeta) {
        DPRINTF(TAGE, "update: no prediction meta, skip\n");
        return;
    }

    // Process each BTB entry
    bool hasRecomputedVsActualDiff = false;
    bool hasRecomputedVsOriginalDiff = false;
    for (const auto &branch : update.branches) {
        const auto &btb_entry = branch.entry;
        if (!(btb_entry.isCond && !btb_entry.alwaysTaken) ||
            (getResolvedUpdate() && !branch.resolvedThisAttempt)) {
            continue;
        }
        const bool actual_taken = branch.actualTaken;
        const bool is_new_entry = branch.matchesMbtbMissCandidate;
        auto orig_it = predMeta->preds.find(btb_entry.pc);
        const bool has_original_pred = orig_it != predMeta->preds.end();
        TagePrediction original_pred;
        if (has_original_pred) {
            original_pred = orig_it->second;
        } else if (!is_new_entry) {
            DPRINTF(TAGE, "update: missing original prediction for old entry pc %#lx, skip\n",
                    btb_entry.pc);
            continue;
        } else {
            DPRINTF(TAGE, "update: reconstruct prediction for new entry pc %#lx from snapshot\n",
                    btb_entry.pc);
        }

        if (has_original_pred && original_pred.finalProviderTable >= 0) {
            if (original_pred.taken == actual_taken) {
                tageStats.updateFinalSourceTableCorrect[original_pred.finalProviderTable]++;
            } else {
                tageStats.updateFinalSourceTableWrong[original_pred.finalProviderTable]++;
            }
        } else if (has_original_pred && original_pred.taken == actual_taken) {
            tageStats.updateFinalSourceBaseCorrect++;
        } else if (has_original_pred) {
            tageStats.updateFinalSourceBaseWrong++;
        }

        TagePrediction recomputed;
        if (updateOnRead || !has_original_pred) {
            // Re-read providers using snapshot (do not rely on prediction-time main/alt)
            recomputed = generateSinglePrediction(btb_entry, startAddr, predMeta,
                                                 stream.tid, stream.asidHash);
            // Track differences for statistics
            auto it = predMeta->preds.find(btb_entry.pc);
            if (has_original_pred && it != predMeta->preds.end() && recomputed.taken != original_pred.taken) {
                hasRecomputedVsOriginalDiff = true;
            }
        } else { // otherwise, use the prediction from the prediction-time main/alt
            recomputed = original_pred;
        }
        if (recomputed.taken != actual_taken) {
            hasRecomputedVsActualDiff = true;
        }

        // Update predictor state and check if need to allocate new entry
        bool need_allocate = updatePredictorStateAndCheckAllocation(
            btb_entry, actual_taken, recomputed, branch.controlMispred);

        // Handle new entry allocation if needed
        AllocationTraceInfo allocInfo;
        if (need_allocate) {

            // Handle allocation of new entries
            uint start_table = 0;
            auto &main_info = recomputed.mainInfo;
            if (main_info.found) {
                start_table = main_info.table + 1; // start from the table after the main prediction table
            }
            handleNewEntryAllocation(startAddr, btb_entry, actual_taken,
                                     start_table, predMeta, stream.asidHash,
                                     stream.tid,
                                     allocInfo);
        }

#ifndef UNIT_TEST
        if (enableDB) {
            TageMissTrace t;
            std::string history_str;
            std::string phistory_str;
            boost::dynamic_bitset<> history_low50 = predMeta->history;
            boost::dynamic_bitset<> phistory_low50 = stream.phistory;
            if (history_low50.size() > 50) {
                history_low50.resize(50);  // get the lower 50 bits of history
            }
            if (phistory_low50.size() > 50) {
                phistory_low50.resize(50);  // get the lower 50 bits of path history
            }
            boost::to_string(history_low50, history_str);
            boost::to_string(phistory_low50, phistory_str);
            TagePrediction trace_pred = predMeta->preds[btb_entry.pc];
            auto main_info = trace_pred.mainInfo;
            auto alt_info = trace_pred.altInfo;
            const uint64_t history_hash = hashBitset(predMeta->history);
            const uint64_t phistory_hash = hashBitset(stream.phistory);
            const uint64_t index_folded_hist_hash =
                hashFoldedHistVec(predMeta->indexFoldedHist);
            const uint64_t tag_folded_hist_hash =
                hashFoldedHistVec(predMeta->tagFoldedHist);
            const uint64_t alt_tag_folded_hist_hash =
                hashFoldedHistVec(predMeta->altTagFoldedHist);
            t.set(startAddr, btb_entry.pc, main_info.way,
                main_info.found, main_info.entry.counter, main_info.entry.useful,
                main_info.table, main_info.index, main_info.entry.tag,
                alt_info.found, alt_info.entry.counter, alt_info.entry.useful,
                alt_info.table, alt_info.index, alt_info.entry.tag,
                trace_pred.useAlt, trace_pred.taken, actual_taken, allocInfo.success,
                allocInfo.table, allocInfo.index, allocInfo.way, allocInfo.tag,
                allocInfo.victimValid, allocInfo.victimTag,
                allocInfo.victimCounter, allocInfo.victimUseful,
                allocInfo.victimPC,
                history_str, phistory_str,
                predMeta->indexFoldedHist[main_info.table].get(),
                trace_pred.useAltIdx, trace_pred.useAltCtr,
                trace_pred.hitTableMask, trace_pred.finalProviderTable,
                trace_pred.finalProviderIsAlt, history_hash, phistory_hash,
                index_folded_hist_hash, tag_folded_hist_hash,
                alt_tag_folded_hist_hash);
            tageMissTrace->write_record(t);
        }
#endif
    }
    // Update recomputed difference statistics (per fetchBlock)
    if (hasRecomputedVsActualDiff) {
        tageStats.recomputedVsActualDiff++;
    }
    if (hasRecomputedVsOriginalDiff) {
        tageStats.recomputedVsOriginalDiff++;
    }
    if (getDelay() <2){
        checkUtageUpdateMisspred(stream, update);
    }
    DPRINTF(TAGE, "end update\n");
}

void
BTBTAGE::checkUtageUpdateMisspred(
    const FetchTarget &stream, const PreparedUpdate &update) {
    auto predMeta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    // use for microtage updatemispred counting
    // sort microtage predictions by pc to find the first taken branch
    std::vector<std::pair<Addr, TagePrediction>> lastPreds;
    lastPreds.reserve(predMeta->preds.size());
    for (auto &kv : predMeta->preds) {
        lastPreds.emplace_back(kv.first, kv.second);
    }
    std::sort(lastPreds.begin(), lastPreds.end(),
            [](const std::pair<Addr, TagePrediction> &a,
                const std::pair<Addr, TagePrediction> &b) {
                return a.first < b.first;
            });
    Addr first_taken_pc = 0;
    for (auto &entry_info : lastPreds) {
        if (entry_info.second.taken) {
            first_taken_pc = entry_info.first;
            break;
        }
    }
    bool fallthrough_mispred =
        (first_taken_pc == 0 && update.outcome.taken) ||
        (first_taken_pc != 0 && !update.outcome.taken);
    bool branch_mispred = update.outcome.taken &&
        first_taken_pc != update.outcome.branch.pc;
    if (fallthrough_mispred || branch_mispred) {
        tageStats.updateMispred++;
    }
}

// Update prediction counter with saturation
void
BTBTAGE::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width-1)) - 1;
    int min = -(1 << (width-1));
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

// Calculate TAGE tag with folded history - optimized version using bitwise operations
Addr
BTBTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist,
                    Addr position, uint8_t asidHash) const
{
    // Create mask for tableTagBits[t] to limit result size
    Addr mask = (1ULL << tableTagBits[t]) - 1;

    unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    pcShift += partitionIndexBits(tableIndexBits[t]) - 1;
    Addr pcBits = (pc >> pcShift) & mask;

    // Extract and prepare folded history bits
    Addr foldedBits = foldedHist & mask;

    // Extract alt tag bits and shift left by 1
    Addr altTagBits = (altFoldedHist << 1) & mask;

    // XOR all components together, including position (like RTL)
    return injectAsidHashIntoTag(pcBits ^ foldedBits ^ altTagBits ^ position,
                                 tableTagBits[t], asidHash);
}

Addr
BTBTAGE::getTageTag(Addr pc, int t, Addr position, uint8_t asidHash) const
{
    const auto &state = historyState(0);
    return getTageTag(pc, t, state.tagFoldedHist[t].get(),
                      state.altTagFoldedHist[t].get(), position, asidHash);
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist,
                      uint8_t asidHash, ThreadID tid) const
{
    const unsigned localIndexBits = partitionIndexBits(tableIndexBits[t]);
    Addr mask = (1ULL << localIndexBits) - 1;

    const unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    Addr pcBits = (pc >> pcShift) & mask;
    Addr foldedBits = foldedHist & mask;

    Addr localIndex = xorAsidHashIntoIndex(
        pcBits ^ foldedBits, localIndexBits, asidHash);
    return partitionIndex(localIndex, tableSizes[t], tid);
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t, uint8_t asidHash, ThreadID tid) const
{
    return getTageIndex(pc, t, historyState(tid).indexFoldedHist[t].get(),
                        asidHash, tid);
}

bool
BTBTAGE::matchTag(Addr expected, Addr found) const
{
    return expected == found;
}

bool
BTBTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
BTBTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

Addr
BTBTAGE::getUseAltIdx(Addr pc) const {
    Addr shiftedPc = pc >> instShiftAmt;
    return shiftedPc & (useAltOnNaSize - 1);
}

unsigned
BTBTAGE::getBranchIndexInBlock(Addr branchPC, Addr startPC) const {
    // Calculate branch position within the fetch block (0 .. maxBranchPositions-1)
    Addr alignedPC = startPC & ~(blockSize - 1);
    Addr offset = (branchPC - alignedPC) >> instShiftAmt;
    assert(offset < maxBranchPositions);
    return offset;
}

unsigned
BTBTAGE::getBankId(Addr pc) const
{
    // Extract bank ID bits after removing instruction alignment
    return (pc >> bankBaseShift) & ((1 << bankIdWidth) - 1);
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
BTBTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, int shamt,
                      bool taken, Addr pc, Addr target, ThreadID tid)
{
    auto &state = historyState(tid);
    if (debug::TAGEHistory) {   // if debug flag is off, do not use to_string since it's too slow
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(TAGEHistory, "in doUpdateHist, shamt %d, taken %d, pc %#lx, target %#lx, history %s\n",
                shamt, taken, pc, target, buf.c_str());
    }

    if (usePathHistory) {
        if (!taken) {
            DPRINTF(TAGEHistory,
                    "not updating path folded history, since FB not taken\n");
            return;
        }
        shamt = 2;
    } else if (shamt == 0) {
        DPRINTF(TAGEHistory,
                "not updating direction folded history, shamt is 0\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            auto &foldedHist = type == 0 ? state.indexFoldedHist[t]
                                         : type == 1 ? state.tagFoldedHist[t]
                                                     : state.altTagFoldedHist[t];
            // since we have folded path history, we can put arbitrary shamt here, and it wouldn't make a difference
            foldedHist.update(history, shamt, taken, pc, target);
            DPRINTF(TAGEHistory, "t: %d, type: %d, foldedHist _folded 0x%lx\n", t, type, foldedHist.get());
        }
    }
}

/**
 * @brief Speculatively updates direction folded histories.
 */
void
BTBTAGE::specUpdateGHist(const boost::dynamic_bitset<> &history,
                        FullBTBPrediction &pred,
                        const DirectionHistoryUpdate &update)
{
    if (usePathHistory) {
        return;
    }

    doUpdateHist(history, update.shamt, update.taken, 0, 0, pred.tid);
}

void
BTBTAGE::specUpdatePHist(const boost::dynamic_bitset<> &history,
                         FullBTBPrediction &pred,
                         const PathHistoryUpdate &update)
{
    if (!usePathHistory) {
        return;
    }

    doUpdateHist(history, update.shamt, update.taken, update.pc,
                 update.target, pred.tid);
}

void
BTBTAGE::recoverFoldedHist(const FetchTarget &entry)
{
    auto predMeta =
        std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        threadHistory[entry.tid].tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        threadHistory[entry.tid].altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        threadHistory[entry.tid].indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }
}

/**
 * @brief Recovers branch history state after a misprediction
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
BTBTAGE::recoverHist(const boost::dynamic_bitset<> &history,
    const FetchTarget &entry, int shamt, bool cond_taken)
{
    if (usePathHistory) {
        return;
    }

    recoverFoldedHist(entry);
    doUpdateHist(history, shamt, cond_taken, 0, 0, entry.tid);
}

void
BTBTAGE::recoverPHist(const boost::dynamic_bitset<> &history,
    const FetchTarget &entry, const PathHistoryUpdate &update)
{
    if (!usePathHistory) {
        return;
    }

    recoverFoldedHist(entry);
    doUpdateHist(history, update.shamt, update.taken, update.pc,
                 update.target, entry.tid);
}

// Check folded history after speculative update and recovery
void
BTBTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    checkFoldedHist(hist, 0, when);
}

void
BTBTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, ThreadID tid,
                         const char * when)
{
    auto &state = historyState(tid);
    DPRINTF(TAGE, "checking folded history when %s\n", when);
    if (debug::TAGEHistory) {
        std::string hist_str;
        boost::to_string(hist, hist_str);
        DPRINTF(TAGEHistory, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            std::string buf2, buf3;
            auto &foldedHist = type == 0 ? state.indexFoldedHist[t]
                                         : type == 1 ? state.tagFoldedHist[t]
                                                     : state.altTagFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

#ifndef UNIT_TEST
// Constructor for TAGE statistics
BTBTAGE::TageStats::TageStats(statistics::Group* parent, int numPredictors, int numBanks):
    statistics::Group(parent),
    ADD_STAT(predNoHitUseBim, statistics::units::Count::get(), "use bimodal when no hit on prediction"),
    ADD_STAT(predUseAlt, statistics::units::Count::get(), "use alt on prediction"),
    ADD_STAT(updateNoHitUseBim, statistics::units::Count::get(), "use bimodal when no hit on update"),
    ADD_STAT(updateUseAlt, statistics::units::Count::get(), "use alt on update"),
    ADD_STAT(updateUseAltCorrect, statistics::units::Count::get(), "use alt on update and correct"),
    ADD_STAT(updateUseAltWrong, statistics::units::Count::get(), "use alt on update and wrong"),
    ADD_STAT(updateAltDiffers, statistics::units::Count::get(), "alt differs on update"),
    ADD_STAT(updateUseAltOnNaUpdated, statistics::units::Count::get(), "use alt on na ctr updated when update"),
    ADD_STAT(updateProviderNa, statistics::units::Count::get(), "provider weak when update"),
    ADD_STAT(updateUseNaCorrect, statistics::units::Count::get(), "use na on update and correct"),
    ADD_STAT(updateUseNaWrong, statistics::units::Count::get(), "use na on update and wrong"),
    ADD_STAT(updateUseAltOnNaCorrect, statistics::units::Count::get(), "use alt on na correct when update"),
    ADD_STAT(updateUseAltOnNaWrong, statistics::units::Count::get(), "use alt on na wrong when update"),
    ADD_STAT(updateAllocFailure, statistics::units::Count::get(), "alloc failure when update"),
    ADD_STAT(updateAllocFailureNoValidTable, statistics::units::Count::get(), "alloc failure no valid table when update"),
    ADD_STAT(updateAllocSuccess, statistics::units::Count::get(), "alloc success when update"),
    ADD_STAT(updateMispred, statistics::units::Count::get(), "mispred when update"),
    ADD_STAT(updateResetU, statistics::units::Count::get(), "reset u when update"),
    ADD_STAT(resolveBranchHasProvider, statistics::units::Count::get(),
        "resolved conditional branches whose recomputed TAGE state has a provider"),
    ADD_STAT(resolveBranchUseProvider, statistics::units::Count::get(),
        "resolved conditional branches that use the provider table"),
    ADD_STAT(resolveBranchHasAlt, statistics::units::Count::get(),
        "resolved conditional branches whose recomputed TAGE state has an alt table"),
    ADD_STAT(resolveBranchUseAltTable, statistics::units::Count::get(),
        "resolved conditional branches that use the alt table as final prediction"),
    ADD_STAT(resolveBranchUseBaseTable, statistics::units::Count::get(),
        "resolved conditional branches that fall back to base prediction"),
    ADD_STAT(mispredictBranchHasProvider, statistics::units::Count::get(),
        "mispredicted branches whose recomputed TAGE state has a provider"),
    ADD_STAT(mispredictBranchUseProvider, statistics::units::Count::get(),
        "mispredicted branches that use the provider table"),
    ADD_STAT(mispredictBranchHasAlt, statistics::units::Count::get(),
        "mispredicted branches whose recomputed TAGE state has an alt table"),
    ADD_STAT(mispredictBranchUseAltTable, statistics::units::Count::get(),
        "mispredicted branches that use the alt table as final prediction"),
    ADD_STAT(mispredictBranchUseBaseTable, statistics::units::Count::get(),
        "mispredicted branches that fall back to base prediction"),
    ADD_STAT(predFinalSourceBase, statistics::units::Count::get(), "predictions whose final source is base BTB"),
    ADD_STAT(updateFinalSourceBaseCorrect, statistics::units::Count::get(), "base BTB final-source predictions that are correct"),
    ADD_STAT(updateFinalSourceBaseWrong, statistics::units::Count::get(), "base BTB final-source predictions that are wrong"),
    ADD_STAT(recomputedVsActualDiff, statistics::units::Count::get(), "fetchBlocks where recomputed.taken != actual_taken"),
    ADD_STAT(recomputedVsOriginalDiff, statistics::units::Count::get(), "fetchBlocks where recomputed.taken != original pred.taken"),
    ADD_STAT(updateBankConflict, statistics::units::Count::get(), "number of bank conflicts detected"),
    ADD_STAT(updateDeferredDueToConflict, statistics::units::Count::get(), "number of updates deferred due to bank conflict (retried later)"),
    ADD_STAT(updateBankConflictPerBank, statistics::units::Count::get(), "bank conflicts per bank"),
    ADD_STAT(updateAccessPerBank, statistics::units::Count::get(), "update accesses per bank"),
    ADD_STAT(predAccessPerBank, statistics::units::Count::get(), "prediction accesses per bank"),
    ADD_STAT(resolveProviderTable, statistics::units::Count::get(),
        "resolved conditional branches grouped by provider table"),
    ADD_STAT(resolveAltTable, statistics::units::Count::get(),
        "resolved conditional branches grouped by alt table"),
    ADD_STAT(resolveUseProviderTable, statistics::units::Count::get(),
        "resolved conditional branches that use the provider table, grouped by table"),
    ADD_STAT(resolveUseAltTable, statistics::units::Count::get(),
        "resolved conditional branches that use the alt table, grouped by table"),
    ADD_STAT(mispredictUseProviderTable, statistics::units::Count::get(),
        "mispredicted branches that use the provider table, grouped by table"),
    ADD_STAT(mispredictUseAltTable, statistics::units::Count::get(),
        "mispredicted branches that use the alt table, grouped by table"),
    ADD_STAT(predTableHits, statistics::units::Count::get(), "hit of each tage table on prediction"),
    ADD_STAT(updateTableHits, statistics::units::Count::get(), "hit of each tage table on update"),
    ADD_STAT(updateTableMispreds, statistics::units::Count::get(), "mispreds of each table when update"),
    ADD_STAT(predFinalSourceTable, statistics::units::Count::get(), "predictions whose final source is a TAGE table"),
    ADD_STAT(updateFinalSourceTableCorrect, statistics::units::Count::get(), "correct predictions grouped by final-source table"),
    ADD_STAT(updateFinalSourceTableWrong, statistics::units::Count::get(), "wrong predictions grouped by final-source table"),

    ADD_STAT(condPredwrong, statistics::units::Count::get(), "number of conditional branch mispredictions committed"),
    ADD_STAT(condMissTakens, statistics::units::Count::get(), "number of conditional branch mispredictions committed with no prediction"),
    ADD_STAT(condCorrect, statistics::units::Count::get(), "number of conditional branch correct predictions committed"),
    ADD_STAT(condMissNoTakens, statistics::units::Count::get(), "number of conditional branch correct predictions committed with no prediction"),
    ADD_STAT(predHit, statistics::units::Count::get(), "number of conditional branch predictions that hit"),
    ADD_STAT(predMiss, statistics::units::Count::get(), "number of conditional branch predictions that miss")
{
    init(numPredictors, numBanks);
}
#endif

void
BTBTAGE::TageStats::init(int predictors, int banks)
{
    numPredictors = predictors;
    numBanks = banks;
    bankIdx = 0;

    predTableHits.init(0, numPredictors - 1, 1);
    updateTableHits.init(0, numPredictors - 1, 1);
    updateTableMispreds.init(numPredictors);
    predFinalSourceTable.init(numPredictors);
    updateFinalSourceTableCorrect.init(numPredictors);
    updateFinalSourceTableWrong.init(numPredictors);
    resolveProviderTable.init(numPredictors);
    resolveAltTable.init(numPredictors);
    resolveUseProviderTable.init(numPredictors);
    resolveUseAltTable.init(numPredictors);
    mispredictUseProviderTable.init(numPredictors);
    mispredictUseAltTable.init(numPredictors);

    updateBankConflictPerBank.init(numBanks);
    updateAccessPerBank.init(numBanks);
    predAccessPerBank.init(numBanks);
}

// Update statistics based on TAGE prediction
void
BTBTAGE::TageStats::updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred)
{
    bool hit = pred.mainInfo.found;
    unsigned hit_table = pred.mainInfo.table;
    bool useAlt = pred.useAlt;
    if (when_pred) {
        if (hit) {
            predTableHits.sample(hit_table, 1);
        } else {
            predNoHitUseBim++;
        }
        if (!hit || useAlt) {
            predUseAlt++;
        }
        if (pred.finalProviderTable >= 0) {
            predFinalSourceTable[pred.finalProviderTable]++;
        } else {
            predFinalSourceBase++;
        }
    } else {
        if (hit) {
            updateTableHits.sample(hit_table, 1);
        } else {
            updateNoHitUseBim++;
        }
        if (!hit || useAlt) {
            updateUseAlt++;
        }
    }
}

// Update LRU counters for a set
void
BTBTAGE::updateLRU(int table, Addr index, unsigned way)
{
    // Increment LRU counters for all entries in the set
    const unsigned ways = getNumWays(table);
    for (unsigned i = 0; i < ways; i++) {
        if (i != way && tageTable[table][index][i].valid) {
            tageTable[table][index][i].lruCounter++;
        }
    }
    // Reset LRU counter for the accessed entry
    tageTable[table][index][way].lruCounter = 0;
}

// Find the LRU victim in a set
unsigned
BTBTAGE::getLRUVictim(int table, Addr index)
{
    unsigned victim = 0;
    unsigned maxLRU = 0;
    const unsigned ways = getNumWays(table);

    // Find the entry with the highest LRU counter
    for (unsigned i = 0; i < ways; i++) {
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

unsigned
BTBTAGE::getNumWays(unsigned table) const
{
    assert(table < numWays.size());
    return numWays[table];
}

#ifndef UNIT_TEST

void
BTBTAGE::commitBranch(const FetchTarget &stream, const DynInstPtr &inst)
{
    if (!inst->isCondCtrl()) {
        // tage olnly deals with conditional branches
        return;
    }
    auto meta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    auto pc = inst->pcState().instAddr();
    auto it = meta->preds.find(pc);
    bool pred_taken = false;
    bool pred_hit = false;
    if (it != meta->preds.end()) {
        pred_taken = it->second.taken;
        pred_hit = true;
    }
    bool this_cond_taken = stream.exeTaken && stream.exeBranchInfo.pc == pc;
    bool predcorrect = (pred_taken == this_cond_taken);
    if (!predcorrect) {
        tageStats.condPredwrong++;
        if (!pred_hit) {
            tageStats.condMissTakens++;
        }
    }else{
        tageStats.condCorrect++;
        if (!pred_hit) {
            tageStats.condMissNoTakens++;
        }
    }

    if (pred_hit) {
        tageStats.predHit++;
    } else {
        tageStats.predMiss++;
    }
}
#endif

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
