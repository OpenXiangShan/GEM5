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
      enableContextAllocFilter(false),
      contextAllocEntries(0),
      contextAllocHistoryBits(0),
      contextAllocThreshold(0),
      contextAllocExplorePeriod(0),
      contextAllocColdAccept(false),
      contextAllocMaxInstability(3),
      contextAllocInstabilityStableDecimation(1),
      contextAllocProtectBudget(0),
      contextAllocProtectTables(0),
      contextAllocProtectProviderHit(false),
      contextAllocMinTable(0),
      contextAllocUsePcInstability(false),
      contextAllocPcEntries(0),
      contextAllocPcThreshold(0),
      contextAllocPcStableDecimation(1),
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
enableContextAllocFilter(p.enableContextAllocFilter),
contextAllocEntries(p.contextAllocEntries),
contextAllocHistoryBits(p.contextAllocHistoryBits),
contextAllocThreshold(p.contextAllocThreshold),
contextAllocExplorePeriod(p.contextAllocExplorePeriod),
contextAllocColdAccept(p.contextAllocColdAccept),
contextAllocMaxInstability(p.contextAllocMaxInstability),
contextAllocInstabilityStableDecimation(p.contextAllocInstabilityStableDecimation),
contextAllocProtectBudget(p.contextAllocProtectBudget),
contextAllocProtectTables(p.contextAllocProtectTables),
contextAllocProtectProviderHit(p.contextAllocProtectProviderHit),
contextAllocMinTable(p.contextAllocMinTable),
contextAllocUsePcInstability(p.contextAllocUsePcInstability),
contextAllocPcEntries(p.contextAllocPcEntries),
contextAllocPcThreshold(p.contextAllocPcThreshold),
contextAllocPcStableDecimation(p.contextAllocPcStableDecimation),
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
    if (enableContextAllocFilter) {
        contextAllocTable.resize(contextAllocEntries);
        if (contextAllocUsePcInstability) {
            contextAllocPcTable.resize(contextAllocPcEntries);
        }
    }
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);

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
        tagFoldedHist.emplace_back((int)histLengths[i], (int)tableTagBits[i],
                                   16, historyType);
        altTagFoldedHist.emplace_back((int)histLengths[i],
                                      (int)tableTagBits[i] - 1, 16,
                                      historyType);
        indexFoldedHist.emplace_back((int)histLengths[i],
                                     (int)tableIndexBits[i], 16,
                                     historyType);
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
                                 std::shared_ptr<TageMeta> predMeta) {
    DPRINTF(TAGE, "generateSinglePrediction for btbEntry: %#lx\n", btb_entry.pc);

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
        Addr index = predMeta ? getTageIndex(startPC, i, predMeta->indexFoldedHist[i].get())
                          : getTageIndex(startPC, i);
        Addr tag = predMeta ? getTageTag(startPC, i,
                            predMeta->tagFoldedHist[i].get(), predMeta->altTagFoldedHist[i].get(), position)
                        : getTageTag(startPC, i, position);

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
                      std::unordered_map<Addr, TageInfoForMGSC> &tageInfoForMgscs, CondTakens& results)
{
    DPRINTF(TAGE, "lookupHelper startAddr: %#lx\n", startPC);

    // Process each BTB entry to make predictions
    for (auto &btb_entry : btbEntries) {
        // Only predict for valid conditional branches
        if (btb_entry.isCond && btb_entry.valid) {
            auto pred = generateSinglePrediction(btb_entry, startPC);
            meta->preds[btb_entry.pc] = pred;
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
    // Record prediction bank for next tick's conflict detection
    lastPredBankId = getBankId(startPC);
    predBankValid = true;

#ifndef UNIT_TEST
    // Record prediction access per bank
    tageStats.predAccessPerBank[lastPredBankId]++;
#endif

    DPRINTF(TAGE, "putPCHistory startAddr: %#lx, bank: %u\n",
            startPC, lastPredBankId);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it

    // Clear old prediction metadata and save current history state
    meta = std::make_shared<TageMeta>();
    meta->tagFoldedHist = tagFoldedHist;
    meta->altTagFoldedHist = altTagFoldedHist;
    meta->indexFoldedHist = indexFoldedHist;
    meta->history = history;

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        stage_pred.condTakens.clear();
        lookupHelper(startPC, stage_pred.btbEntries, stage_pred.tageInfoForMgscs, stage_pred.condTakens);
    }

}

std::shared_ptr<void>
BTBTAGE::getPredictionMeta() {
    return meta;
}

/**
 * @brief Prepare BTB entries for update by filtering and processing
 * 
 * @param stream The fetch stream containing update information
 * @return Vector of BTB entries that need to be updated
 */
std::vector<BTBEntry>
BTBTAGE::prepareUpdateEntries(const FetchTarget &stream) {
    auto all_entries = stream.updateBTBEntries;

    // Add potential new BTB entry if it's a btb miss during prediction
    if (!stream.updateIsOldEntry) {
        BTBEntry potential_new_entry = stream.updateNewBTBEntry;
        bool new_entry_taken = stream.exeTaken && stream.getControlPC() == potential_new_entry.pc;
        if (!new_entry_taken) {
            potential_new_entry.alwaysTaken = false;
        }
        all_entries.push_back(potential_new_entry);
    }

    // Filter: only keep conditional branches that are not always taken
    if (getResolvedUpdate()) {
        auto remove_it = std::remove_if(all_entries.begin(), all_entries.end(),
            [](const BTBEntry &e) { return !(e.isCond && !e.alwaysTaken && e.resolved); });
        all_entries.erase(remove_it, all_entries.end());
    } else {
        auto remove_it = std::remove_if(all_entries.begin(), all_entries.end(),
            [](const BTBEntry &e) { return !(e.isCond && !e.alwaysTaken); });
        all_entries.erase(remove_it, all_entries.end());
    }

    return all_entries;
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
                             const FetchTarget &stream) {
    tageStats.updateStatsWithTagePrediction(pred, false);

    auto &main_info = pred.mainInfo;
    auto &alt_info = pred.altInfo;
    bool used_alt = pred.useAlt;
    // Use base table instead of entry.ctr for fallback prediction
    Addr startPC = stream.getRealStartPC();
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
#ifndef UNIT_TEST
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
#endif

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
        if (contextAllocProtectProviderHit && main_is_correct &&
            main_info.table < contextAllocProtectTables &&
            contextAllocProtectBudget > 0 &&
            contextAllocContextIsProven(entry, actual_taken, stream,
                                        nullptr)) {
            way.allocProtect = std::max(way.allocProtect,
                                        contextAllocProtectBudget);
            tageStats.contextAllocProviderHitProtected++;
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
    bool this_fb_mispred = stream.squashType == SquashType::SQUASH_CTRL &&
                               stream.squashPC == entry.pc;
    if (this_fb_mispred) {
        tageStats.mispredictBranchHasProvider += main_info.found;
        tageStats.mispredictBranchUseProvider += use_provider;
        tageStats.mispredictBranchHasAlt += alt_info.found;
        tageStats.mispredictBranchUseAltTable += use_alt_table;
        tageStats.mispredictBranchUseBaseTable += use_base_table;
#ifndef UNIT_TEST
        if (use_provider) {
            tageStats.mispredictUseProviderTable[main_info.table]++;
        }
        if (use_alt_table) {
            tageStats.mispredictUseAltTable[alt_info.table]++;
        }
#endif
    }
    if (getDelay() == 2){
        if (this_fb_mispred) {
            tageStats.updateMispred++;
            if (!used_alt && main_info.found) {
#ifndef UNIT_TEST
                tageStats.updateTableMispreds[main_info.table]++;
#endif
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
 * @return true if allocation is successful
 */
bool
BTBTAGE::handleNewEntryAllocation(const Addr &startPC,
                                 const BTBEntry &entry,
                                 bool actual_taken,
                                 unsigned start_table,
                                 std::shared_ptr<TageMeta> meta,
                                 const FetchTarget &stream,
                                 AllocationTraceInfo &allocInfo) {
    // Match RTL victim priority:
    // 1) invalid way
    // 2) weak and not-useful way
    // 3) any not-useful way

    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(entry.pc, startPC);

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        Addr newIndex = getTageIndex(startPC, ti, meta->indexFoldedHist[ti].get());
        Addr newTag = getTageTag(startPC, ti,
            meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get(), position);

        auto &set = tageTable[ti][newIndex];

        const unsigned ways = getNumWays(ti);
        std::vector<bool> protectedThisAttempt(ways, false);
        auto shouldSkipProtectedVictim = [&](unsigned way) {
            if (!enableContextAllocFilter || ti >= contextAllocProtectTables) {
                return false;
            }
            auto &cand = set[way];
            if (protectedThisAttempt[way]) {
                return true;
            }
            if (cand.allocProtect == 0) {
                return false;
            }
            cand.allocProtect--;
            protectedThisAttempt[way] = true;
            tageStats.contextAllocProtectSkips++;
            return true;
        };

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
                if (!cand.useful && weakish &&
                    !shouldSkipProtectedVictim(way)) {
                    selected_way = way;
                    break;
                }
            }
        }

        if (selected_way == -1) {
            for (unsigned way = 0; way < ways; ++way) {
                if (!set[way].useful &&
                    !shouldSkipProtectedVictim(way)) {
                    selected_way = way;
                    break;
                }
            }
        }

        if (selected_way != -1) {
            unsigned protect_budget = 0;
            if (!shouldAllocateByContextFilter(entry, actual_taken, stream, meta,
                                               set[selected_way].valid, ti,
                                               protect_budget)) {
                continue;
            }
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
            set[selected_way].allocProtect = protect_budget;
            tageStats.updateAllocSuccess++;
            usefulResetCnt = usefulResetCnt <= 0 ? 0 : usefulResetCnt - 1;
            return true;
        }
        tageStats.updateAllocFailure++;
        usefulResetCnt++;
    }

    if (usefulResetCnt >= 256) {
        usefulResetCnt = 0;
        tageStats.updateResetU++;
        DPRINTF(TAGE, "reset useful bit of all entries\n");
        for (auto &table : tageTable) {
            for (auto &set : table) {
                for (auto &way : set) {
                    way.useful = false;
                }
            }
        }
    }

    DPRINTF(TAGE, "no eligible way found for allocation starting from table %d\n", start_table);
    tageStats.updateAllocFailureNoValidTable++;
    return false;
}

Addr
BTBTAGE::getContextAllocKey(const BTBEntry &entry,
                            const FetchTarget &stream,
                            std::shared_ptr<TageMeta> meta) const
{
    Addr histSig = 0;
    const auto &hist = stream.phistory.empty() && meta ? meta->history
                                                       : stream.phistory;
    const unsigned maxBits = sizeof(Addr) * 8;
    const unsigned bits = std::min(contextAllocHistoryBits, maxBits);
    const unsigned limit = std::min<unsigned>(bits, hist.size());
    for (unsigned i = 0; i < limit; ++i) {
        if (hist[i]) {
            histSig |= (Addr(1) << i);
        }
    }

    Addr key = entry.pc ^ (entry.pc >> 3) ^ (histSig << 1);
    key ^= (histSig >> 7) ^ (histSig << 17);
    return key;
}

bool
BTBTAGE::contextAllocContextIsProven(const BTBEntry &entry,
                                     bool actual_taken,
                                     const FetchTarget &stream,
                                     std::shared_ptr<TageMeta> meta) const
{
    if (!enableContextAllocFilter || contextAllocTable.empty()) {
        return false;
    }

    const Addr key = getContextAllocKey(entry, stream, meta);
    const auto &slot = contextAllocTable[key % contextAllocTable.size()];
    return slot.valid && slot.tag == key &&
           slot.lastTaken == actual_taken &&
           slot.confidence >= contextAllocThreshold &&
           slot.instability <= contextAllocMaxInstability;
}

bool
BTBTAGE::shouldAllocateByContextFilter(const BTBEntry &entry,
                                       bool actual_taken,
                                       const FetchTarget &stream,
                                       std::shared_ptr<TageMeta> meta,
                                       bool replacing_valid,
                                       unsigned table,
                                       unsigned &protect_budget)
{
    protect_budget = 0;
    if (!enableContextAllocFilter) {
        return true;
    }
    if (!replacing_valid || contextAllocTable.empty()) {
        tageStats.contextAllocAccepted++;
        return true;
    }

    const Addr key = getContextAllocKey(entry, stream, meta);
    auto &slot = contextAllocTable[key % contextAllocTable.size()];
    contextAllocProbeCount++;
    const bool explore = contextAllocExplorePeriod > 0 &&
                         contextAllocProbeCount % contextAllocExplorePeriod == 0;

    auto accept = [&]() {
        if (table < contextAllocProtectTables &&
            contextAllocProtectBudget > 0) {
            protect_budget = contextAllocProtectBudget;
            tageStats.contextAllocProtected++;
        }
        tageStats.contextAllocAccepted++;
        return true;
    };

    auto contextIsProven = [&]() {
        return contextAllocContextIsProven(entry, actual_taken, stream, meta);
    };

    if (table < contextAllocMinTable) {
        if (contextIsProven()) {
            return accept();
        }
        tageStats.contextAllocAccepted++;
        tageStats.contextAllocBypassedStablePc++;
        return true;
    }

    if (contextAllocUsePcInstability && !contextAllocPcTable.empty()) {
        const Addr pcKey = entry.pc ^ (entry.pc >> 2);
        auto &pcSlot = contextAllocPcTable[pcKey % contextAllocPcTable.size()];
        const bool pcIsUnstable =
            pcSlot.valid && pcSlot.tag == pcKey &&
            pcSlot.instability >= contextAllocPcThreshold;
        if (!pcIsUnstable) {
            if (contextIsProven()) {
                return accept();
            }
            tageStats.contextAllocAccepted++;
            tageStats.contextAllocBypassedStablePc++;
            return true;
        }
    }

    if (!slot.valid || slot.tag != key) {
        if (contextAllocColdAccept || explore) {
            if (explore && !contextAllocColdAccept) {
                tageStats.contextAllocExplored++;
            }
            return accept();
        }
        tageStats.contextAllocRejectedCold++;
        return false;
    }

    if (slot.lastTaken != actual_taken) {
        if (explore) {
            tageStats.contextAllocExplored++;
            return accept();
        }
        tageStats.contextAllocRejectedMismatch++;
        return false;
    }

    if (slot.confidence < contextAllocThreshold) {
        if (explore) {
            tageStats.contextAllocExplored++;
            return accept();
        }
        tageStats.contextAllocRejectedWeak++;
        return false;
    }

    if (slot.instability > contextAllocMaxInstability) {
        if (explore) {
            tageStats.contextAllocExplored++;
            return accept();
        }
        tageStats.contextAllocRejectedUnstable++;
        return false;
    }

    return accept();
}

void
BTBTAGE::updateContextAllocFilter(const BTBEntry &entry,
                                  bool actual_taken,
                                  const FetchTarget &stream,
                                  std::shared_ptr<TageMeta> meta)
{
    if (!enableContextAllocFilter || contextAllocTable.empty()) {
        return;
    }

    const Addr key = getContextAllocKey(entry, stream, meta);
    auto &slot = contextAllocTable[key % contextAllocTable.size()];
    const bool hadSameContext = slot.valid && slot.tag == key;
    const bool wasConsistent = hadSameContext && slot.lastTaken == actual_taken;
    const bool wasConfident = wasConsistent &&
                              slot.confidence >= contextAllocThreshold;
    const bool wasInconsistent = hadSameContext &&
                                 slot.lastTaken != actual_taken;
    if (!slot.valid || slot.tag != key) {
        slot.valid = true;
        slot.tag = key;
        slot.lastTaken = actual_taken;
        slot.confidence = 0;
        slot.instability = 0;
        slot.stableUpdates = 0;
    } else if (slot.lastTaken == actual_taken) {
        slot.confidence = std::min(slot.confidence + 1, 3U);
        if (slot.confidence >= contextAllocThreshold &&
            slot.instability > 0) {
            const unsigned decimation =
                std::max(contextAllocInstabilityStableDecimation, 1U);
            slot.stableUpdates++;
            if (slot.stableUpdates >= decimation) {
                slot.stableUpdates = 0;
                slot.instability--;
                tageStats.contextAllocInstabilityDecays++;
            }
        }
        tageStats.contextAllocConsistentUpdates++;
    } else {
        tageStats.contextAllocInconsistentUpdates++;
        slot.instability = std::min(slot.instability + 1, 3U);
        slot.stableUpdates = 0;
        if (slot.confidence > 0) {
            slot.confidence--;
        } else {
            slot.lastTaken = actual_taken;
        }
    }

    if (contextAllocUsePcInstability && !contextAllocPcTable.empty()) {
        const Addr pcKey = entry.pc ^ (entry.pc >> 2);
        auto &pcSlot = contextAllocPcTable[pcKey % contextAllocPcTable.size()];
        if (!pcSlot.valid || pcSlot.tag != pcKey) {
            pcSlot.valid = true;
            pcSlot.tag = pcKey;
            pcSlot.instability = 0;
            pcSlot.stableUpdates = 0;
        }

        if (wasInconsistent) {
            pcSlot.instability = std::min(pcSlot.instability + 1, 3U);
            pcSlot.stableUpdates = 0;
            tageStats.contextAllocPcThrottleUpdates++;
        } else if (wasConfident && pcSlot.instability > 0) {
            const unsigned decimation =
                std::max(contextAllocPcStableDecimation, 1U);
            pcSlot.stableUpdates++;
            if (pcSlot.stableUpdates >= decimation) {
                pcSlot.stableUpdates = 0;
                pcSlot.instability--;
                tageStats.contextAllocPcStableUpdates++;
            }
        }
    }
}

/**
 * @brief Probe resolved update for bank conflicts without mutating state.
 * Returns false if the update cannot proceed due to a bank conflict.
 */
bool
BTBTAGE::canResolveUpdate(const FetchTarget &stream) {
    Addr startAddr = stream.getRealStartPC();
    unsigned updateBank = getBankId(startAddr);

#ifndef UNIT_TEST
    // Record attempted update access per bank (even if it conflicts)
    tageStats.updateAccessPerBank[updateBank]++;
#endif

    if (enableBankConflict && predBankValid && updateBank == lastPredBankId) {
        tageStats.updateBankConflict++;
        tageStats.updateDeferredDueToConflict++;
#ifndef UNIT_TEST
        tageStats.updateBankConflictPerBank[updateBank]++;
#endif
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
BTBTAGE::doResolveUpdate(const FetchTarget &stream) {
    if (enableBankConflict && predBankValid) {
        // Prediction consumed; clear bank tag for next cycle
        predBankValid = false;
    }
    update(stream);
}

/**
 * @brief Updates the TAGE predictor state based on actual branch execution results
 * 
 * @param stream The fetch stream containing branch execution information
 */
void
BTBTAGE::update(const FetchTarget &stream) {
    Addr startAddr = stream.getRealStartPC();
    unsigned updateBank = getBankId(startAddr);

    DPRINTF(TAGE, "update startAddr: %#lx, bank: %u\n", startAddr, updateBank);

    // ========== Normal Update Logic ==========
    // Prepare BTB entries to update
    auto entries_to_update = prepareUpdateEntries(stream);
    
    // Get prediction metadata snapshot and bind to member for helpers
    auto predMeta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    if (!predMeta) {
        DPRINTF(TAGE, "update: no prediction meta, skip\n");
        return;
    }

    // Process each BTB entry
    bool hasRecomputedVsActualDiff = false;
    bool hasRecomputedVsOriginalDiff = false;
    for (auto &btb_entry : entries_to_update) {
        bool actual_taken = stream.exeTaken && stream.exeBranchInfo == btb_entry;
        const bool is_new_entry = !stream.updateIsOldEntry &&btb_entry.pc == stream.updateNewBTBEntry.pc;
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

#ifndef UNIT_TEST
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
#endif

        TagePrediction recomputed;
        if (updateOnRead || !has_original_pred) {
            // Reconstruct providers when update-on-read is enabled or when a new
            // BTB entry lacks prediction-time metadata.
            recomputed = generateSinglePrediction(btb_entry, startAddr, predMeta);
            if (has_original_pred && recomputed.taken != original_pred.taken) {
                hasRecomputedVsOriginalDiff = true;
            }
        } else { // otherwise, use the prediction from the prediction-time main/alt
            recomputed = original_pred;
        }
        if (recomputed.taken != actual_taken) {
            hasRecomputedVsActualDiff = true;
        }

        // Update predictor state and check if need to allocate new entry
        bool need_allocate = updatePredictorStateAndCheckAllocation(btb_entry, actual_taken, recomputed, stream);

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
                                     start_table, predMeta, stream, allocInfo);
        }
        updateContextAllocFilter(btb_entry, actual_taken, stream, predMeta);

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
        checkUtageUpdateMisspred(stream);
    }
    DPRINTF(TAGE, "end update\n");
}

void
BTBTAGE::checkUtageUpdateMisspred(const FetchTarget &stream) {
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
    bool fallthrough_mispred = (first_taken_pc == 0 && stream.exeTaken) ||
                                (first_taken_pc != 0 && !stream.exeTaken);
    bool branch_mispred = stream.exeTaken && first_taken_pc != stream.exeBranchInfo.pc;
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
BTBTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist, Addr position)
{
    // Create mask for tableTagBits[t] to limit result size
    Addr mask = (1ULL << tableTagBits[t]) - 1;

    unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    pcShift += tableIndexBits[t] - 1;   // since tableIndexBits = log(2048) = 11, RTL is 10
    Addr pcBits = (pc >> pcShift) & mask;

    // Extract and prepare folded history bits
    Addr foldedBits = foldedHist & mask;

    // Extract alt tag bits and shift left by 1
    Addr altTagBits = (altFoldedHist << 1) & mask;

    // XOR all components together, including position (like RTL)
    return pcBits ^ foldedBits ^ altTagBits ^ position;
}

Addr
BTBTAGE::getTageTag(Addr pc, int t, Addr position)
{
    return getTageTag(pc, t, tagFoldedHist[t].get(), altTagFoldedHist[t].get(), position);
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist)
{
    // Create mask for tableIndexBits[t] to limit result size
    Addr mask = (1ULL << tableIndexBits[t]) - 1;

    const unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    Addr pcBits = (pc >> pcShift) & mask;
    Addr foldedBits = foldedHist & mask;

    // Support non-power-of-two table sizes when tuning capacities.
    return (pcBits ^ foldedBits) % tableSizes[t];
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t)
{
    return getTageIndex(pc, t, indexFoldedHist[t].get());
}

bool
BTBTAGE::matchTag(Addr expected, Addr found)
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
BTBTAGE::getBranchIndexInBlock(Addr branchPC, Addr startPC) {
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
                      bool taken, Addr pc, Addr target)
{
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
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            foldedHist.update(history, shamt, taken, pc, target);
            DPRINTF(TAGEHistory, "t: %d, type: %d, foldedHist _folded 0x%lx\n", t, type, foldedHist.get());
        }
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
BTBTAGE::specUpdateHist(const boost::dynamic_bitset<> &history,
                        FullBTBPrediction &pred)
{
    if (usePathHistory) {
        return;
    }

    auto [shamt, taken] = pred.getHistInfo();
    doUpdateHist(history, shamt, taken, 0, 0);
}

void
BTBTAGE::specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    if (!usePathHistory) {
        return;
    }

    auto [pc, target, taken] = pred.getPHistInfo();
    doUpdateHist(history, 2, taken, pc, target);
}

void
BTBTAGE::recoverFoldedHist(const FetchTarget &entry)
{
    auto predMeta =
        std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
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
    doUpdateHist(history, shamt, cond_taken, 0, 0);
}

void
BTBTAGE::recoverPHist(const boost::dynamic_bitset<> &history,
    const FetchTarget &entry, int shamt, bool cond_taken)
{
    if (!usePathHistory) {
        return;
    }

    recoverFoldedHist(entry);
    doUpdateHist(history, 2, cond_taken, entry.getControlPC(),
                 entry.getTakenTarget());
}

// Check folded history after speculative update and recovery
void
BTBTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    DPRINTF(TAGE, "checking folded history when %s\n", when);
    if (debug::TAGEHistory) {
        std::string hist_str;
        boost::to_string(hist, hist_str);
        DPRINTF(TAGEHistory, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            std::string buf2, buf3;
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
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
    ADD_STAT(contextAllocAccepted, statistics::units::Count::get(),
        "allocation candidates accepted by PC+PHR context filter"),
    ADD_STAT(contextAllocRejectedCold, statistics::units::Count::get(),
        "valid-victim allocations rejected on cold context-filter entries"),
    ADD_STAT(contextAllocRejectedWeak, statistics::units::Count::get(),
        "valid-victim allocations rejected on weak context confidence"),
    ADD_STAT(contextAllocRejectedMismatch, statistics::units::Count::get(),
        "valid-victim allocations rejected on context outcome mismatch"),
    ADD_STAT(contextAllocRejectedUnstable, statistics::units::Count::get(),
        "valid-victim allocations rejected on recently unstable context outcomes"),
    ADD_STAT(contextAllocExplored, statistics::units::Count::get(),
        "context-filtered allocations accepted by periodic exploration"),
    ADD_STAT(contextAllocProtected, statistics::units::Count::get(),
        "context-filtered allocations granted replacement protection"),
    ADD_STAT(contextAllocProtectSkips, statistics::units::Count::get(),
        "replacement candidates skipped by context allocation protection"),
    ADD_STAT(contextAllocProviderHitProtected, statistics::units::Count::get(),
        "correct provider hits granted context allocation protection"),
    ADD_STAT(contextAllocConsistentUpdates, statistics::units::Count::get(),
        "context-filter entries updated with the same outcome"),
    ADD_STAT(contextAllocInconsistentUpdates, statistics::units::Count::get(),
        "context-filter entries updated with a changed outcome"),
    ADD_STAT(contextAllocInstabilityDecays, statistics::units::Count::get(),
        "context-filter instability decrements from confident stable contexts"),
    ADD_STAT(contextAllocBypassedStablePc, statistics::units::Count::get(),
        "context-filter candidates accepted before PC instability throttling"),
    ADD_STAT(contextAllocPcThrottleUpdates, statistics::units::Count::get(),
        "PC instability increments from context outcome changes"),
    ADD_STAT(contextAllocPcStableUpdates, statistics::units::Count::get(),
        "PC instability decrements from confident stable contexts"),
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
    predTableHits.init(0, numPredictors-1, 1);
    updateTableHits.init(0, numPredictors-1, 1);
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

    // Initialize per-bank statistics vectors
    updateBankConflictPerBank.init(numBanks);
    updateAccessPerBank.init(numBanks);
    predAccessPerBank.init(numBanks);
}
#endif

// Update statistics based on TAGE prediction
void
BTBTAGE::TageStats::updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred)
{
    bool hit = pred.mainInfo.found;
    unsigned hit_table = pred.mainInfo.table;
    bool useAlt = pred.useAlt;
    if (when_pred) {
        if (hit) {
#ifndef UNIT_TEST
            predTableHits.sample(hit_table, 1);
#endif
        } else {
            predNoHitUseBim++;
        }
        if (!hit || useAlt) {
            predUseAlt++;
        }
#ifndef UNIT_TEST
        if (pred.finalProviderTable >= 0) {
            predFinalSourceTable[pred.finalProviderTable]++;
        } else {
            predFinalSourceBase++;
        }
#endif
    } else {
        if (hit) {
#ifndef UNIT_TEST
            updateTableHits.sample(hit_table, 1);
#endif
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
