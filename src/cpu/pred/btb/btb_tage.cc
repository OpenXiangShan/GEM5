#include "cpu/pred/btb/btb_tage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <sstream>

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

#ifdef UNIT_TEST
namespace test {
#endif

#ifdef UNIT_TEST
// Test constructor for unit testing mode
BTBTAGE::BTBTAGE(unsigned numPredictors, unsigned numWaysPerTable,
                 unsigned tableSize, unsigned numBanks)
    : TimedBaseBTBPredictor(),
      numPredictors(numPredictors),
      numWays(numPredictors, numWaysPerTable),
      enableShareTable(false),
      shareTableSize(2048),
      shareTableWays(2),
      shareAllocWindow(1400),
      shareAllocConsecutive(2),
      shareBound(false),
      shareTargetTable(-1),
      shareCurrentWinner(-1),
      shareCurrentWinnerStreak(0),
      shareWindowAllocCount(0),
      shareEpoch(0),
      maxBranchPositions(32),
      useAltOnNaSize(1024),
      useAltOnNaWidth(7),
      useV2PHistory(false),
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
maxHistLen(p.maxHistLen),
numWays(p.numWays),
enableShareTable(p.enableShareTable),
shareTableSize(p.shareTableSize),
shareTableWays(p.shareTableWays),
shareAllocWindow(p.shareAllocWindow),
shareAllocConsecutive(p.shareAllocConsecutive),
shareBound(false),
shareTargetTable(-1),
shareCurrentWinner(-1),
shareCurrentWinnerStreak(0),
shareWindowAllocCount(0),
shareEpoch(0),
maxBranchPositions(p.maxBranchPositions),
useAltOnNaSize(p.useAltOnNaSize),
useAltOnNaWidth(p.useAltOnNaWidth),
numTablesToAlloc(p.numTablesToAlloc),
enableSC(p.enableSC),
useV2PHistory(p.useV2PHistory),
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
    this->needMoreHistories = p.needMoreHistories;

    // Warn if updateOnRead is disabled (bank simulation works better with it enabled)
    if (!p.updateOnRead) {
        warn("BTBTAGE: Bank simulation works better with updateOnRead=true");
    }
#endif
    if (numWays.size() == 1 && numPredictors > 1) {
        numWays.resize(numPredictors, numWays.front());
    }

    assert(numWays.size() >= numPredictors);
    assert(numTablesToAlloc == 1);
    if (enableShareTable) {
        fatal_if(updateOnRead,
            "BTBTAGE share table V1 does not support updateOnRead=true");
        fatal_if(shareTableSize != 2048,
            "BTBTAGE share table V1 only supports shareTableSize=2048");
        fatal_if(shareTableWays != 2,
            "BTBTAGE share table V2 only supports shareTableWays=2");
    }
    tageTable.resize(numPredictors);
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

        tagFoldedHist.push_back(PathFoldedHist((int)histLengths[i], (int)tableTagBits[i], 16));
        altTagFoldedHist.push_back(PathFoldedHist((int)histLengths[i], (int)tableTagBits[i]-1, 16));
        indexFoldedHist.push_back(PathFoldedHist((int)histLengths[i], (int)tableIndexBits[i], 16));
        indexFoldedHist4k.push_back(PathFoldedHist((int)histLengths[i], 12, 16));
    }
    if (enableShareTable) {
        shareAllocCounters.assign(numPredictors, 0);
        shareTable.resize(shareTableSize);
        for (unsigned i = 0; i < shareTableSize; ++i) {
            shareTable[i].resize(shareTableWays);
        }
        bool has_compatible_target = false;
        for (unsigned i = 0; i < numPredictors; ++i) {
            if (tableSizes[i] == shareTableSize) {
                has_compatible_target = true;
                break;
            }
        }
        fatal_if(!has_compatible_target,
            "BTBTAGE share table V1 requires at least one predictor table with size 2048");
    }
    usefulResetCnt = 0;

    // initialize use_alt_on_na table
    useAlt.resize(useAltOnNaSize, 0);
    if (useV2PHistory) {
        // Length is not the goal in this experiment. Reuse the current
        // history framework capacity and only change update semantics.
        v2PHistory.resize(std::max<unsigned>(maxHistLen, 10), 0);
    }
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

bool
BTBTAGE::canUseShareForTable(unsigned table) const
{
    return enableShareTable && shareBound && shareTargetTable == (int)table &&
           table < tableSizes.size() && tableSizes[table] == shareTableSize;
}

bool
BTBTAGE::isExpandedShareTarget(unsigned table) const
{
    return canUseShareForTable(table);
}

unsigned
BTBTAGE::getExpandedIndexBits() const
{
    return ceilLog2(shareTableSize * 2);
}

unsigned
BTBTAGE::getLogicalTableSize(unsigned table) const
{
    return isExpandedShareTarget(table) ? (shareTableSize * 2) : tableSizes[table];
}

Addr
BTBTAGE::getExpandedTageIndex(Addr pc, int table, uint64_t foldedHist) const
{
    const unsigned indexBits = getExpandedIndexBits();
    const Addr mask = (1ULL << indexBits) - 1;
    const unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    Addr pcBits = (pc >> pcShift) & mask;
    Addr foldedBits = foldedHist & mask;
    return (pcBits ^ foldedBits) % (shareTableSize * 2);
}

bool
BTBTAGE::mapLogicalIndexToStorage(unsigned table, Addr logicalIndex,
                                  Addr &physicalIndex, bool &fromShare) const
{
    if (!isExpandedShareTarget(table)) {
        physicalIndex = logicalIndex;
        fromShare = false;
        return false;
    }
    fromShare = logicalIndex >= shareTableSize;
    physicalIndex = logicalIndex & (shareTableSize - 1);
    return true;
}

std::vector<BTBTAGE::TageEntry> &
BTBTAGE::selectTargetSet(unsigned table, Addr physicalIndex, bool fromShare)
{
    if (fromShare) {
        return shareTable[physicalIndex];
    }
    return tageTable[table][physicalIndex];
}

const std::vector<BTBTAGE::TageEntry> &
BTBTAGE::selectTargetSet(unsigned table, Addr physicalIndex, bool fromShare) const
{
    if (fromShare) {
        return shareTable[physicalIndex];
    }
    return tageTable[table][physicalIndex];
}

void
BTBTAGE::clearTargetTable(unsigned table)
{
    if (table >= tageTable.size()) {
        return;
    }
    for (auto &set : tageTable[table]) {
        for (auto &way : set) {
            way = TageEntry();
        }
    }
}

void
BTBTAGE::clearShareTable()
{
    if (!enableShareTable) {
        return;
    }
    for (auto &set : shareTable) {
        for (auto &way : set) {
            way = TageEntry();
        }
    }
}

BTBTAGE::TageEntry &
BTBTAGE::resolveProviderEntry(const TageTableInfo &info)
{
    assert(info.found);
    if (info.fromShareTable) {
        assert(canUseShareForTable(info.table));
        return shareTable[info.index][info.way];
    }
    return tageTable[info.table][info.index][info.way];
}

bool
BTBTAGE::updateShareBindingOnAlloc(unsigned allocatedTable)
{
    if (!enableShareTable) {
        return false;
    }
    assert(allocatedTable < shareAllocCounters.size());
    shareAllocCounters[allocatedTable]++;
    shareWindowAllocCount++;
    if (shareWindowAllocCount < shareAllocWindow) {
        return false;
    }

    unsigned winner = 0;
    unsigned winnerCount = shareAllocCounters[0];
    for (unsigned i = 1; i < numPredictors; ++i) {
        if (tableSizes[i] != shareTableSize) {
            continue;
        }
        if (shareAllocCounters[i] > winnerCount) {
            winner = i;
            winnerCount = shareAllocCounters[i];
        }
    }

    if ((int)winner == shareCurrentWinner) {
        shareCurrentWinnerStreak++;
    } else {
        shareCurrentWinner = (int)winner;
        shareCurrentWinnerStreak = 1;
    }

    bool bound_now_to_allocated = false;
    if (!shareBound && shareCurrentWinnerStreak >= shareAllocConsecutive) {
        shareBound = true;
        shareTargetTable = winner;
        shareEpoch++;
        clearTargetTable(winner);
        clearShareTable();
        tageStats.shareBindCount++;
        bound_now_to_allocated = (winner == allocatedTable);
    }

    std::fill(shareAllocCounters.begin(), shareAllocCounters.end(), 0);
    shareWindowAllocCount = 0;
    return bound_now_to_allocated;
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
            std::make_pair("branchPos", UINT64),
            std::make_pair("mainFound", UINT64),
            std::make_pair("mainCounter", UINT64),
            std::make_pair("mainUseful", UINT64),
            std::make_pair("mainTable", UINT64),
            std::make_pair("mainIndex", UINT64),
            std::make_pair("mainStoredPC", UINT64),
            std::make_pair("mainStoredTag", UINT64),
            std::make_pair("altFound", UINT64),
            std::make_pair("altCounter", UINT64),
            std::make_pair("altUseful", UINT64),
            std::make_pair("altTable", UINT64),
            std::make_pair("altIndex", UINT64),
            std::make_pair("altWay", UINT64),
            std::make_pair("altStoredPC", UINT64),
            std::make_pair("altStoredTag", UINT64),
            std::make_pair("mainFromShare", UINT64),
            std::make_pair("altFromShare", UINT64),
            std::make_pair("useAlt", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("actualTaken", UINT64),
            std::make_pair("allocSuccess", UINT64),
            std::make_pair("allocTable", UINT64),
            std::make_pair("allocIndex", UINT64),
            std::make_pair("allocWay", UINT64),
            std::make_pair("allocTag", UINT64),
            std::make_pair("allocToShare", UINT64),
            std::make_pair("shareBound", UINT64),
            std::make_pair("shareTargetTable", UINT64),
            std::make_pair("victimOldValid", UINT64),
            std::make_pair("victimOldPC", UINT64),
            std::make_pair("victimOldTag", UINT64),
            std::make_pair("victimOldCounter", UINT64),
            std::make_pair("victimOldUseful", UINT64),
            std::make_pair("history", TEXT),
            std::make_pair("indexFoldedHist", UINT64),
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

    // Search from highest to lowest table for matches
    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(btb_entry.pc, startPC);

    for (int i = numPredictors - 1; i >= 0; --i) {
        const bool expanded = isExpandedShareTarget(i);
        Addr logicalIndex = 0;
        if (predMeta) {
            logicalIndex = expanded ?
                getExpandedTageIndex(startPC, i, predMeta->indexFoldedHist4k[i].get()) :
                getTageIndex(startPC, i, predMeta->indexFoldedHist[i].get());
        } else {
            logicalIndex = expanded ?
                getExpandedTageIndex(startPC, i, indexFoldedHist4k[i].get()) :
                getTageIndex(startPC, i);
        }

        Addr index = 0;
        bool matching_from_share = false;
        mapLogicalIndexToStorage(i, logicalIndex, index, matching_from_share);

        Addr tag = predMeta ? getTageTag(startPC, i,
                            predMeta->tagFoldedHist[i].get(), predMeta->altTagFoldedHist[i].get(), position)
                        : getTageTag(startPC, i, position);

        bool match = false; // for each logical table layer, only one match is kept
        TageEntry matching_entry;
        unsigned matching_way = 0;

        const auto &set = selectTargetSet(i, index, matching_from_share);
        const unsigned ways = matching_from_share ? shareTableWays : getNumWays(i);
        for (unsigned way = 0; way < ways; way++) {
            const auto &entry = set[way];
            if (entry.valid && tag == entry.tag) {
                matching_entry = entry;
                matching_way = way;
                match = true;
                DPRINTF(TAGE, "hit  table %d[%lu][%u]%s: valid %d, tag %lu, ctr %d, useful %d, btb_pc %#lx, pos %u\n",
                    i, index, way, matching_from_share ? " [share]" : "", entry.valid, entry.tag,
                    entry.counter, entry.useful, btb_entry.pc, position);
                break;
            }
        }

        if (matching_from_share) {
            if (match) {
                tageStats.shareLookupHit++;
            } else {
                tageStats.shareLookupMiss++;
            }
        }

        if (match) {
            if (!provided) {
                // First match becomes main prediction
                main_info = TageTableInfo(true, matching_entry, i, index, tag,
                                          matching_way, matching_from_share);
                provided = true;
            } else if (!alt_provided) {
                // Second match becomes alternative prediction
                alt_info = TageTableInfo(true, matching_entry, i, index, tag,
                                         matching_way, matching_from_share);
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

    // use_alt_on_na gating: when provider weak, consult per-PC counter
    bool use_alt = false;
    if (!provided) {
        use_alt = true;
    } else {
        bool main_weak = (main_info.entry.counter == 0 || main_info.entry.counter == -1);
        if (main_weak) {
            Addr uidx = getUseAltIdx(btb_entry.pc);
            use_alt = (useAlt[uidx] >= 0);
        } else {
            use_alt = false;
        }
    }
    bool taken = use_alt ? alt_pred : main_taken;
    int final_provider_table = -1;
    bool final_provider_is_alt = false;
    bool final_provider_from_share = false;
    if (!use_alt && provided) {
        final_provider_table = main_info.table;
        final_provider_from_share = main_info.fromShareTable;
    } else if (use_alt && alt_provided) {
        final_provider_table = alt_info.table;
        final_provider_is_alt = true;
        final_provider_from_share = alt_info.fromShareTable;
    }

    DPRINTF(TAGE, "tage predict %#lx taken %d\n", btb_entry.pc, taken);
    DPRINTF(TAGE, "tage use_alt %d ? (alt_provided %d ? alt_taken %d : base_taken %d) : main_taken %d\n",
        use_alt, alt_provided, alt_taken, base_taken, main_taken);
    DPRINTF(TAGE, "tage final source %#lx table %d alt %d\n",
        btb_entry.pc, final_provider_table, final_provider_is_alt);

    return TagePrediction(btb_entry.pc, main_info, alt_info, use_alt, taken, alt_pred,
        final_provider_table, final_provider_is_alt, final_provider_from_share);
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
    meta->indexFoldedHist4k = indexFoldedHist4k;
    meta->predictEpoch = shareEpoch;
    meta->history = history;
    if (useV2PHistory) {
        meta->localV2PHistory = v2PHistory;
    }

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
                             const FetchTarget &stream,
                             bool staleMainProvider,
                             bool staleAltProvider) {
    tageStats.updateStatsWithTagePrediction(pred, false);

    auto &main_info = pred.mainInfo;
    auto &alt_info = pred.altInfo;
    bool used_alt = pred.useAlt;
    // Use base table instead of entry.ctr for fallback prediction
    Addr startPC = stream.getRealStartPC();
    bool base_taken = entry.ctr >= 0;
    bool alt_taken = alt_info.found ? alt_info.taken() : base_taken;

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
    if (main_info.found && !staleMainProvider) {
        DPRINTF(TAGE, "prediction provided by table %d, idx %lu, way %u, updating corresponding entry\n",
            main_info.table, main_info.index, main_info.way);

        auto &way = resolveProviderEntry(main_info);

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
    } else if (staleMainProvider) {
        tageStats.shareStalePredDropProviderUpdate++;
    }

    // Update alternative prediction provider
    if (used_alt && alt_info.found && !staleAltProvider) {
        auto &way = resolveProviderEntry(alt_info);
        updateCounter(actual_taken, 3, way.counter);
        // No LRU maintenance
    } else if (used_alt && staleAltProvider) {
        tageStats.shareStalePredDropProviderUpdate++;
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

    // Special case: provider is weak but direction is correct
    // In this case, provider just needs more training, not a longer history table
    // This avoids wasteful allocation and prevents ping-pong effects
    if (used_alt && main_info.found && main_info.taken() == actual_taken) {
        return false;
    }

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
                                 uint64_t &allocated_table,
                                 uint64_t &allocated_index,
                                 uint64_t &allocated_way,
                                 uint64_t &allocated_tag,
                                 bool &allocated_to_share,
                                 uint64_t &victim_old_valid,
                                 uint64_t &victim_old_pc,
                                 uint64_t &victim_old_tag,
                                 uint64_t &victim_old_counter,
                                 uint64_t &victim_old_useful) {
    // Match RTL victim priority:
    // 1) invalid way
    // 2) weak and not-useful way
    // 3) any not-useful way

    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(entry.pc, startPC);

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        bool expanded = isExpandedShareTarget(ti);
        Addr logicalIndex = expanded ?
            getExpandedTageIndex(startPC, ti, meta->indexFoldedHist4k[ti].get()) :
            getTageIndex(startPC, ti, meta->indexFoldedHist[ti].get());
        Addr newTag = getTageTag(startPC, ti,
            meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get(), position);

        Addr newIndex = 0;
        bool selected_from_share = false;
        mapLogicalIndexToStorage(ti, logicalIndex, newIndex, selected_from_share);

        auto select_victim = [&](std::vector<TageEntry> &candidate_set,
                                 unsigned candidate_ways) -> int {
            for (unsigned way = 0; way < candidate_ways; ++way) {
                if (!candidate_set[way].valid) {
                    return way;
                }
            }
            for (unsigned way = 0; way < candidate_ways; ++way) {
                auto &cand = candidate_set[way];
                const bool weakish = std::abs(cand.counter * 2 + 1) <= 3;
                if (!cand.useful && weakish) {
                    return way;
                }
            }
            for (unsigned way = 0; way < candidate_ways; ++way) {
                if (!candidate_set[way].useful) {
                    return way;
                }
            }
            return -1;
        };

        auto *selected_set_ptr = &selectTargetSet(ti, newIndex, selected_from_share);
        const unsigned ways = selected_from_share ? shareTableWays : getNumWays(ti);
        int selected_way = select_victim(*selected_set_ptr, ways);

        if (selected_way != -1) {
            const bool rebound_current_table = updateShareBindingOnAlloc(ti);
            if (rebound_current_table) {
                expanded = isExpandedShareTarget(ti);
                logicalIndex = expanded ?
                    getExpandedTageIndex(startPC, ti, meta->indexFoldedHist4k[ti].get()) :
                    getTageIndex(startPC, ti, meta->indexFoldedHist[ti].get());
                newTag = getTageTag(startPC, ti,
                    meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get(), position);
                mapLogicalIndexToStorage(ti, logicalIndex, newIndex, selected_from_share);
                selected_set_ptr = &selectTargetSet(ti, newIndex, selected_from_share);
                selected_way = select_victim(*selected_set_ptr,
                    selected_from_share ? shareTableWays : getNumWays(ti));
                assert(selected_way != -1);
            }

            short newCounter = actual_taken ? 0 : -1;
            auto &selected_set = *selected_set_ptr;
            const auto old_entry = selected_set[selected_way];
            DPRINTF(TAGE,
                    "allocating entry in table %d[%lu][%u]%s, logicalIndex "
                    "%lu, tag %lu (with pos %u), counter %d, pc %#lx\n",
                    ti, newIndex, selected_way,
                    selected_from_share ? " [share]" : "", logicalIndex,
                    newTag, position, newCounter, entry.pc);
            victim_old_valid = old_entry.valid;
            victim_old_pc = old_entry.pc;
            victim_old_tag = old_entry.tag;
            victim_old_counter = old_entry.counter;
            victim_old_useful = old_entry.useful;
            selected_set[selected_way] = TageEntry(newTag, newCounter, entry.pc);
            tageStats.updateAllocSuccess++;
            if (selected_from_share) {
                tageStats.shareAllocSuccess++;
            }
            allocated_table = ti;
            allocated_index = newIndex;
            allocated_way = selected_way;
            allocated_tag = newTag;
            allocated_to_share = selected_from_share;
            usefulResetCnt = usefulResetCnt <= 0 ? 0 : usefulResetCnt - 1;
            return true;
        }

        tageStats.updateAllocFailure++;
        if (selected_from_share) {
            tageStats.shareAllocFailure++;
        }
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
        if (enableShareTable) {
            for (auto &set : shareTable) {
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
        const bool stale_pred = predMeta->predictEpoch != shareEpoch;
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
                if (original_pred.finalProviderFromShare) {
                    tageStats.shareFinalSourceCorrect++;
                }
            } else {
                tageStats.updateFinalSourceTableWrong[original_pred.finalProviderTable]++;
                if (original_pred.finalProviderFromShare) {
                    tageStats.shareFinalSourceWrong++;
                }
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

        const bool stale_main_provider = stale_pred && recomputed.mainInfo.found &&
            shareBound && recomputed.mainInfo.table == (unsigned)shareTargetTable;
        const bool stale_alt_provider = stale_pred && recomputed.altInfo.found &&
            shareBound && recomputed.altInfo.table == (unsigned)shareTargetTable;

        // Update predictor state and check if need to allocate new entry
        bool need_allocate = updatePredictorStateAndCheckAllocation(
            btb_entry, actual_taken, recomputed, stream,
            stale_main_provider, stale_alt_provider);

        // Handle new entry allocation if needed
        bool alloc_success = false;
        bool alloc_to_share = false;
        uint64_t allocated_table = 0;
        uint64_t allocated_index = 0;
        uint64_t allocated_way = 0;
        uint64_t allocated_tag = 0;
        uint64_t victim_old_valid = 0;
        uint64_t victim_old_pc = 0;
        uint64_t victim_old_tag = 0;
        uint64_t victim_old_counter = 0;
        uint64_t victim_old_useful = 0;
        if (need_allocate) {

            // Handle allocation of new entries
            uint start_table = 0;
            auto &main_info = recomputed.mainInfo;
            if (main_info.found) {
                if (stale_main_provider) {
                    start_table = main_info.table;
                } else {
                    start_table = main_info.table + 1;
                }
            }
            alloc_success = handleNewEntryAllocation(startAddr, btb_entry, actual_taken,
                                   start_table, predMeta, allocated_table, allocated_index, allocated_way,
                                   allocated_tag, alloc_to_share, victim_old_valid, victim_old_pc, victim_old_tag,
                                   victim_old_counter, victim_old_useful);
        }

#ifndef UNIT_TEST
        if (enableDB) {
            TageMissTrace t;
            std::string history_str;
            boost::dynamic_bitset<> history_low50 = predMeta->history;
            if (history_low50.size() > 50) {
                history_low50.resize(50);  // get the lower 50 bits of history
            }
            boost::to_string(history_low50, history_str);
            TagePrediction trace_pred = predMeta->preds[btb_entry.pc];
            auto main_info = trace_pred.mainInfo;
            auto alt_info = trace_pred.altInfo;
            const auto branch_pos = getBranchIndexInBlock(btb_entry.pc, startAddr);
            t.set(startAddr, btb_entry.pc, main_info.way,
                branch_pos,
                main_info.found, main_info.entry.counter, main_info.entry.useful,
                main_info.table, main_info.index,
                main_info.entry.pc, main_info.entry.tag,
                alt_info.found, alt_info.entry.counter, alt_info.entry.useful,
                alt_info.table, alt_info.index,
                alt_info.way, alt_info.entry.pc, alt_info.entry.tag,
                main_info.fromShareTable, alt_info.fromShareTable,
                trace_pred.useAlt, trace_pred.taken, actual_taken, alloc_success,
                allocated_table, allocated_index, allocated_way, allocated_tag, alloc_to_share,
                shareBound, shareTargetTable < 0 ? 0 : (uint64_t)shareTargetTable,
                victim_old_valid, victim_old_pc, victim_old_tag,
                victim_old_counter, victim_old_useful,
                history_str,
                (main_info.found && isExpandedShareTarget(main_info.table)) ?
                    predMeta->indexFoldedHist4k[main_info.table].get() :
                    predMeta->indexFoldedHist[main_info.table].get());
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
    const unsigned indexBits =
        isExpandedShareTarget(t) ? getExpandedIndexBits() : tableIndexBits[t];
    return getTageTagWithIndexBits(pc, t, foldedHist, altFoldedHist,
                                   position, indexBits);
}

Addr
BTBTAGE::getTageTagWithIndexBits(Addr pc, int t, uint64_t foldedHist,
                                 uint64_t altFoldedHist, Addr position,
                                 unsigned indexBits) const
{
    // Create mask for tableTagBits[t] to limit result size
    Addr mask = (1ULL << tableTagBits[t]) - 1;

    unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    pcShift += indexBits - 1;
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
BTBTAGE::updateFoldedHistoriesFromHistory(const boost::dynamic_bitset<> &history,
                                          bool taken, Addr pc, Addr target)
{
    if (debug::TAGEHistory) {   // if debug flag is off, do not use to_string since it's too slow
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(TAGEHistory, "in doUpdateHist, taken %d, pc %#lx, history %s\n", taken, pc, buf.c_str());
    }
    if (!taken) {
        DPRINTF(TAGEHistory, "not updating folded history, since FB not taken\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            auto &foldedHist = type == 0 ? indexFoldedHist[t] : type == 1 ? tagFoldedHist[t] : altTagFoldedHist[t];
            // since we have folded path history, we can put arbitrary shamt here, and it wouldn't make a difference
            foldedHist.update(history, 2, taken, pc, target);
            DPRINTF(TAGEHistory, "t: %d, type: %d, foldedHist _folded 0x%lx\n", t, type, foldedHist.get());
        }
        indexFoldedHist4k[t].update(history, 2, taken, pc, target);
    }
}

uint16_t
BTBTAGE::getV2Footprint(Addr branchPC, Addr targetPC) const
{
    auto b = [branchPC](unsigned bit) -> uint16_t {
        return (branchPC >> bit) & 1;
    };
    auto t = [targetPC](unsigned bit) -> uint16_t {
        return (targetPC >> bit) & 1;
    };

    uint16_t footprint = 0;
    footprint |= ((b(2)  ^ t(7))  << 0);
    footprint |= ((b(3)  ^ t(8))  << 1);
    footprint |= ((b(4)  ^ t(9))  << 2);
    footprint |= ((b(5)  ^ t(10)) << 3);
    footprint |= ((b(6)  ^ b(12) ^ t(11)) << 4);
    footprint |= ((b(7)  ^ b(13) ^ t(2))  << 5);
    footprint |= ((b(8)  ^ b(14) ^ t(3))  << 6);
    footprint |= ((b(9)  ^ b(15) ^ t(4))  << 7);
    footprint |= ((b(10) ^ b(16) ^ t(5))  << 8);
    footprint |= ((b(11) ^ b(17) ^ t(6))  << 9);
    return footprint;
}

void
BTBTAGE::doUpdateHistLegacy(const boost::dynamic_bitset<> &history, bool taken,
                            Addr pc, Addr target)
{
    updateFoldedHistoriesFromHistory(history, taken, pc, target);
}

void
BTBTAGE::doUpdateHistV2(bool taken, Addr pc, Addr target)
{
    if (debug::TAGEHistory) {
        std::string buf;
        boost::to_string(v2PHistory, buf);
        DPRINTF(TAGEHistory,
                "in doUpdateHistV2, taken %d, pc %#lx, target %#lx, history %s\n",
                taken, pc, target, buf.c_str());
    }
    if (!taken) {
        DPRINTF(TAGEHistory, "not updating folded history, since FB not taken\n");
        return;
    }

    const uint16_t footprint = getV2Footprint(pc, target);
    v2PHistory <<= 2;
    for (std::size_t i = 0; i < 10 && i < v2PHistory.size(); ++i) {
        const bool old_bit = v2PHistory[i];
        const bool fp_bit = (footprint >> i) & 1;
        v2PHistory[i] = old_bit ^ fp_bit;
    }
    updateFoldedHistoriesFromHistory(v2PHistory, taken, pc, target);
}

void
BTBTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, bool taken, Addr pc, Addr target)
{
    if (useV2PHistory) {
        doUpdateHistV2(taken, pc, target);
        return;
    }
    doUpdateHistLegacy(history, taken, pc, target);
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
BTBTAGE::specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    auto [pc, target, taken] = pred.getPHistInfo();
    doUpdateHist(history, taken, pc, target);
    if (meta && useV2PHistory) {
        meta->localV2PHistory = v2PHistory;
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
BTBTAGE::recoverPHist(const boost::dynamic_bitset<> &history,
    const FetchTarget &entry, int shamt, bool cond_taken)
{
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
        indexFoldedHist4k[i].recover(predMeta->indexFoldedHist4k[i]);
    }
    if (useV2PHistory) {
        v2PHistory = predMeta->localV2PHistory;
    }
    doUpdateHist(history, cond_taken, entry.getControlPC(), entry.getTakenTarget());
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
        indexFoldedHist4k[t].check(hist);
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
    ADD_STAT(predFinalSourceBase, statistics::units::Count::get(),
        "predictions whose final source is base BTB"),
    ADD_STAT(updateFinalSourceBaseCorrect, statistics::units::Count::get(),
        "base BTB final-source predictions that are correct"),
    ADD_STAT(updateFinalSourceBaseWrong, statistics::units::Count::get(),
        "base BTB final-source predictions that are wrong"),
    ADD_STAT(shareBindCount, statistics::units::Count::get(), "number of times the share table becomes bound"),
    ADD_STAT(shareLookupHit, statistics::units::Count::get(), "number of share table lookup hits"),
    ADD_STAT(shareLookupMiss, statistics::units::Count::get(), "number of share table lookup misses"),
    ADD_STAT(shareAllocSuccess, statistics::units::Count::get(), "number of successful share table allocations"),
    ADD_STAT(shareAllocFailure, statistics::units::Count::get(), "number of failed share table allocations"),
    ADD_STAT(shareFinalSourceCorrect, statistics::units::Count::get(),
        "share table final-source predictions that are correct"),
    ADD_STAT(shareFinalSourceWrong, statistics::units::Count::get(),
        "share table final-source predictions that are wrong"),
    ADD_STAT(shareStalePredDropProviderUpdate, statistics::units::Count::get(),
        "provider writebacks dropped because prediction epoch is stale for expanded share target"),
    ADD_STAT(recomputedVsActualDiff, statistics::units::Count::get(),
        "fetchBlocks where recomputed.taken != actual_taken"),
    ADD_STAT(recomputedVsOriginalDiff, statistics::units::Count::get(),
        "fetchBlocks where recomputed.taken != original pred.taken"),
    ADD_STAT(updateBankConflict, statistics::units::Count::get(),
        "number of bank conflicts detected"),
    ADD_STAT(updateDeferredDueToConflict, statistics::units::Count::get(),
        "number of updates deferred due to bank conflict (retried later)"),
    ADD_STAT(updateBankConflictPerBank, statistics::units::Count::get(),
        "bank conflicts per bank"),
    ADD_STAT(updateAccessPerBank, statistics::units::Count::get(), "update accesses per bank"),
    ADD_STAT(predAccessPerBank, statistics::units::Count::get(), "prediction accesses per bank"),
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
