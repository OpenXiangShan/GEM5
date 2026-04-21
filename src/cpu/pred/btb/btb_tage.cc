#include "cpu/pred/btb/btb_tage.hh"

#include <algorithm>
#include <cassert>
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
      maxBranchPositions(32),
      useAltOnNaSize(1024),
      useAltOnNaWidth(7),
      indexHistMixMode(IndexHistMixMode::Full),
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
maxBranchPositions(p.maxBranchPositions),
useAltOnNaSize(p.useAltOnNaSize),
useAltOnNaWidth(p.useAltOnNaWidth),
numTablesToAlloc(p.numTablesToAlloc),
indexHistMixMode(IndexHistMixMode::Full),
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
    this->needMoreHistories = p.needMoreHistories;

    // Warn if updateOnRead is disabled (bank simulation works better with it enabled)
    if (!p.updateOnRead) {
        warn("BTBTAGE: Bank simulation works better with updateOnRead=true");
    }
    setIndexHistMixMode(p.indexHistMixMode);
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

void
BTBTAGE::setIndexHistMixMode(const std::string &mode)
{
    if (mode == "low") {
        indexHistMixMode = IndexHistMixMode::Low;
        return;
    }
    if (mode == "high") {
        indexHistMixMode = IndexHistMixMode::High;
        return;
    }
    if (mode != "full") {
#ifndef UNIT_TEST
        warn("BTBTAGE: invalid indexHistMixMode '%s', fallback to 'full'",
             mode);
#endif
    }
    indexHistMixMode = IndexHistMixMode::Full;
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
            std::make_pair("mainWay", UINT64),
            std::make_pair("mainSlot", UINT64),
            std::make_pair("altFound", UINT64),
            std::make_pair("altCounter", UINT64),
            std::make_pair("altUseful", UINT64),
            std::make_pair("altTable", UINT64),
            std::make_pair("altIndex", UINT64),
            std::make_pair("altWay", UINT64),
            std::make_pair("altSlot", UINT64),
            std::make_pair("useAlt", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("actualTaken", UINT64),
            std::make_pair("allocSuccess", UINT64),
            std::make_pair("allocTable", UINT64),
            std::make_pair("allocIndex", UINT64),
            std::make_pair("allocWay", UINT64),
            std::make_pair("allocSlot", UINT64),
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

    // Search from highest to lowest table for matches.
    const Addr blockBase = startPC & ~(blockSize - 1);
    unsigned position = getBranchIndexInBlock(btb_entry.pc, startPC);

    for (int i = numPredictors - 1; i >= 0; --i) {
        // Compute block-level index/tag.
        Addr index = predMeta ? getTageIndex(blockBase, i, predMeta->indexFoldedHist[i].get())
                          : getTageIndex(blockBase, i);
        Addr tag = predMeta ? getTageTag(blockBase, i,
                            predMeta->tagFoldedHist[i].get(), predMeta->altTagFoldedHist[i].get())
                        : getTageTag(blockBase, i);

        const unsigned ways = getNumWays(i);
        int same_tag_way = -1;
        for (unsigned way = 0; way < ways; way++) {
            auto &entry = tageTable[i][index][way];
            if (!entry.valid || tag != entry.tag) {
                continue;
            }
            assert(same_tag_way < 0 &&
                   "Duplicate same-tag entries detected in BTBTAGE lookup path");
            same_tag_way = static_cast<int>(way);
        }

        if (same_tag_way >= 0) {
            auto &entry = tageTable[i][index][same_tag_way];
            int slot = findSlotByPosition(entry, position);
            if (slot >= 0) {
                const auto &slot_entry = entry.slots[slot];
                DPRINTF(TAGE,
                    "hit table %d[%lu][%u] slot %d: tag %lu, ctr %d, useful %d, btb_pc %#lx, pos %u\n",
                    i, index, same_tag_way, slot, tag, slot_entry.counter,
                    slot_entry.useful, btb_entry.pc, position);
                if (!provided) {
                    main_info = TageTableInfo(true, entry, i, index, tag,
                                              same_tag_way, slot, slot_entry);
                    provided = true;
                } else if (!alt_provided) {
                    alt_info = TageTableInfo(true, entry, i, index, tag,
                                             same_tag_way, slot, slot_entry);
                    alt_provided = true;
                }
            } else {
                tageStats.predTagHitSlotMiss++;
#ifndef UNIT_TEST
                tageStats.predTagHitSlotMissByTable[i]++;
#endif
                DPRINTF(TAGE,
                        "tag hit but slot miss table %d[%lu], tag %lu, btb_pc %#lx, pos %u\n",
                        i, index, tag, btb_entry.pc, position);
            }
        } else {
            DPRINTF(TAGE, "miss table %d[%lu] for tag %lu, btb_pc %#lx, pos %u\n",
                i, index, tag, btb_entry.pc, position);
        }

        if (alt_provided) {
            break;
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
        bool main_weak = (main_info.slotInfo.counter == 0 || main_info.slotInfo.counter == -1);
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

    return TagePrediction(btb_entry.pc, main_info, alt_info, use_alt, taken, alt_pred,
        final_provider_table, final_provider_is_alt);
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
                                         abs(pred.mainInfo.slotInfo.counter*2 + 1) == 7; // counter saturated, -4 or 3
            tageInfoForMgscs[btb_entry.pc].tage_pred_conf_mid = pred.mainInfo.found &&
                                         (abs(pred.mainInfo.slotInfo.counter*2 + 1) < 7 &&
                                         abs(pred.mainInfo.slotInfo.counter*2 + 1) > 1); // counter not saturated, -3, -2, 1, 2
            tageInfoForMgscs[btb_entry.pc].tage_pred_conf_low = !pred.mainInfo.found ||
                                         (abs(pred.mainInfo.slotInfo.counter*2 + 1) <= 1); // counter initialized, -1 or 0
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

int
BTBTAGE::findSlotByPosition(const TageEntry &entry, unsigned position) const
{
    for (unsigned i = 0; i < entry.slots.size(); ++i) {
        if (entry.slots[i].valid && entry.slots[i].position == position) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool
BTBTAGE::findLiveSameTagSlot(unsigned table, Addr index, Addr tag,
                             unsigned position, unsigned preferred_way,
                             unsigned &resolved_way,
                             unsigned &resolved_slot) const
{
    const auto &set = tageTable[table][index];
    const unsigned ways = getNumWays(table);
    bool found = false;
    bool saw_same_tag_entry = false;

    auto inspectWay = [&](unsigned way) {
        if (way >= ways) {
            return;
        }
        const auto &entry = set[way];
        if (!entry.valid || entry.tag != tag) {
            return;
        }

        assert(!saw_same_tag_entry &&
               "Duplicate same-tag entries detected in BTBTAGE fallback path");
        saw_same_tag_entry = true;

        int slot = findSlotByPosition(entry, position);
        if (slot >= 0) {
            resolved_way = way;
            resolved_slot = static_cast<unsigned>(slot);
            found = true;
        }
    };

    inspectWay(preferred_way);
    for (unsigned way = 0; way < ways; ++way) {
        if (way == preferred_way) {
            continue;
        }
        inspectWay(way);
    }

    return found;
}

bool
BTBTAGE::isWeakishCounter(short counter) const
{
    return std::abs(counter * 2 + 1) <= 3;
}

bool
BTBTAGE::isSlotUnprotected(const TageSlot &slot) const
{
    return !slot.valid || (!slot.useful && isWeakishCounter(slot.counter));
}

bool
BTBTAGE::isEntryWholeEvictable(const TageEntry &entry) const
{
    if (!entry.valid) {
        return true;
    }

    for (const auto &slot : entry.slots) {
        if (!isSlotUnprotected(slot)) {
            return false;
        }
    }
    return true;
}

void
BTBTAGE::sortEntrySlotsByPosition(TageEntry &entry)
{
    std::stable_sort(entry.slots.begin(), entry.slots.end(),
        [](const TageSlot &lhs, const TageSlot &rhs) {
            if (lhs.valid != rhs.valid) {
                return lhs.valid > rhs.valid;
            }
            if (!lhs.valid) {
                return false;
            }
            return lhs.position < rhs.position;
        });
}

void
BTBTAGE::noteAllocationFailure()
{
    tageStats.updateAllocFailure++;
    usefulResetCnt++;

    if (usefulResetCnt >= 256) {
        usefulResetCnt = 0;
        tageStats.updateResetU++;
        DPRINTF(TAGE, "reset useful bit of all entries\n");
        for (auto &table : tageTable) {
            for (auto &set : table) {
                for (auto &way : set) {
                    resetEntryUsefulBits(way);
                }
            }
        }
    }
}

void
BTBTAGE::resetEntryUsefulBits(TageEntry &entry)
{
    for (auto &slot : entry.slots) {
        if (slot.valid) {
            slot.useful = false;
        }
    }
    syncEntryLegacyMirror(entry);
}

void
BTBTAGE::syncEntryLegacyMirror(TageEntry &entry)
{
    if (entry.slots[0].valid) {
        entry.counter = entry.slots[0].counter;
        entry.useful = entry.slots[0].useful;
    } else {
        entry.counter = 0;
        entry.useful = false;
    }
}

bool
BTBTAGE::weakenFirstNonUsefulStrongSlot(unsigned table, Addr index)
{
    auto &set = tageTable[table][index];
    const unsigned ways = getNumWays(table);

    for (unsigned way = 0; way < ways; ++way) {
        auto &entry = set[way];
        for (unsigned slot = 0; slot < entry.slots.size(); ++slot) {
            auto &cand_slot = entry.slots[slot];
            if (!cand_slot.valid || cand_slot.useful ||
                isWeakishCounter(cand_slot.counter)) {
                continue;
            }

            if (cand_slot.counter > 0) {
                cand_slot.counter--;
            } else {
                cand_slot.counter++;
            }
            syncEntryLegacyMirror(entry);
            DPRINTF(TAGE,
                    "counter weakening by one step toward zero on table %d[%lu][%u] slot %u, new ctr %d\n",
                    table, index, way, slot, cand_slot.counter);
            return true;
        }
    }

    return false;
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
    bool base_taken = entry.ctr >= 0;
    bool alt_taken = alt_info.found ? alt_info.taken() : base_taken;

    // Update use_alt_on_na when provider is weak (0 or -1)
    if (main_info.found) {
        bool main_weak = (main_info.slotInfo.counter == 0 || main_info.slotInfo.counter == -1);
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
        DPRINTF(TAGE,
            "prediction provided by table %d, idx %lu, way %u, slot %u, updating corresponding slot\n",
            main_info.table, main_info.index, main_info.way, main_info.slot);

        unsigned main_way = main_info.way;
        unsigned main_slot_idx = main_info.slot;
        if (!findLiveSameTagSlot(main_info.table, main_info.index, main_info.tag,
                                 main_info.slotInfo.position, main_info.way,
                                 main_way, main_slot_idx)) {
            DPRINTF(TAGE,
                "main provider slot vanished for table %d[%lu], tag %lu, pos %u\n",
                main_info.table, main_info.index, main_info.tag,
                main_info.slotInfo.position);
        } else {
            auto &entry_ref = tageTable[main_info.table][main_info.index][main_way];
            auto &main_slot = entry_ref.slots[main_slot_idx];

            // Update prediction counter for provider slot.
            updateCounter(actual_taken, 3, main_slot.counter);

            // Update useful bit based on several conditions
            bool main_is_correct = main_info.taken() == actual_taken;
            bool alt_is_correct_and_strong = alt_info.found &&
                                         (alt_info.taken() == actual_taken) &&
                                         (abs(2 * alt_info.slotInfo.counter + 1) == 7);

            // a. Special reset (humility mechanism)
            if (alt_is_correct_and_strong && main_is_correct) {
                main_slot.useful = 0;
                DPRINTF(TAGEUseful, "useful bit reset to 0 due to humility rule\n");
            } else if (main_info.taken() != alt_taken) {
                // b. Original logic to set useful bit high
                if (main_is_correct) {
                    main_slot.useful = 1;
                }
            }

            // c. Reset u on counter sign flip (becomes weak)
            if (main_slot.counter == 0 || main_slot.counter == -1) {
                main_slot.useful = 0;
                DPRINTF(TAGEUseful, "useful bit reset to 0 due to weak counter\n");
            }
            syncEntryLegacyMirror(entry_ref);
            DPRINTF(TAGE, "useful bit is now %d\n", main_slot.useful);

            // No LRU maintenance
        }
    }

    // Update alternative prediction provider
    if (used_alt && alt_info.found) {
        unsigned alt_way = alt_info.way;
        unsigned alt_slot_idx = alt_info.slot;
        if (findLiveSameTagSlot(alt_info.table, alt_info.index, alt_info.tag,
                                alt_info.slotInfo.position, alt_info.way,
                                alt_way, alt_slot_idx)) {
            auto &entry_ref = tageTable[alt_info.table][alt_info.index][alt_way];
            updateCounter(actual_taken, 3, entry_ref.slots[alt_slot_idx].counter);
            syncEntryLegacyMirror(entry_ref);
        } else {
            DPRINTF(TAGE,
                "alt provider slot vanished for table %d[%lu], tag %lu, pos %u\n",
                alt_info.table, alt_info.index, alt_info.tag,
                alt_info.slotInfo.position);
        }
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
                                 uint64_t &allocated_slot) {
    const Addr blockBase = startPC & ~(blockSize - 1);
    const unsigned position = getBranchIndexInBlock(entry.pc, startPC);
    const short initCounter = actual_taken ? 0 : -1;

    // Unique same-tag set-associative allocation:
    // - each table/set may contain at most one entry for a given tag.
    // - same-tag requests try position hit, then empty-slot fill, then
    //   weakish-slot replacement inside that unique container.
    // - different-tag requests may only evict a whole entry when all slots are
    //   unprotected.
    // - if a same-tag request cannot hit/fill/replace, weaken one non-useful
    //   strong slot by one step toward zero and report allocation failure.

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        Addr newIndex = getTageIndex(blockBase, ti, meta->indexFoldedHist[ti].get());
        Addr newTag = getTageTag(blockBase, ti,
            meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get());

        auto &set = tageTable[ti][newIndex];
        const unsigned ways = getNumWays(ti);
        int same_tag_way = -1;
        int first_invalid_way = -1;
        bool has_existing_different_tag_entry = false;
        for (unsigned way = 0; way < ways; ++way) {
            auto &cand = set[way];
            if (!cand.valid) {
                if (first_invalid_way < 0) {
                    first_invalid_way = static_cast<int>(way);
                }
                continue;
            }
            if (cand.tag == newTag) {
                assert(same_tag_way < 0 &&
                       "Duplicate same-tag entries detected in BTBTAGE allocation path");
                same_tag_way = static_cast<int>(way);
            } else {
                has_existing_different_tag_entry = true;
            }
        }

        if (same_tag_way >= 0) {
            if (first_invalid_way >= 0) {
                tageStats.allocSameTagWhileInvalidWayExists++;
#ifndef UNIT_TEST
                tageStats.allocSameTagWhileInvalidWayExistsByTable[ti]++;
#endif
            }

            auto classifyReusedSameTagEntry = [&](unsigned way) {
                if (set[way].ownerBlockBase == blockBase) {
                    tageStats.allocSameTagTrueBlockReuse++;
#ifndef UNIT_TEST
                    tageStats.allocSameTagTrueBlockReuseByTable[ti]++;
#endif
                } else {
                    tageStats.allocSameTagAliasCollision++;
#ifndef UNIT_TEST
                    tageStats.allocSameTagAliasCollisionByTable[ti]++;
#endif
                }
            };

            int fill_slot = -1;
            int replace_slot = -1;
            auto &same_entry = set[same_tag_way];
            int hit_slot = findSlotByPosition(same_entry, position);
            if (hit_slot >= 0) {
                // same-tag + position hit: slot training is handled in provider
                // update path.
                classifyReusedSameTagEntry(same_tag_way);
                return false;
            }

            for (unsigned slot = 0; slot < same_entry.slots.size(); ++slot) {
                if (!same_entry.slots[slot].valid) {
                    fill_slot = static_cast<int>(slot);
                    break;
                }
            }

            if (fill_slot >= 0) {
                // same-tag + position miss + empty slot: insert.
                classifyReusedSameTagEntry(same_tag_way);
                tageStats.allocSameTagFillEmptySlot++;
#ifndef UNIT_TEST
                tageStats.allocSameTagFillEmptySlotByTable[ti]++;
#endif
                same_entry.slots[fill_slot] = TageSlot(true, position, initCounter, false);
                sortEntrySlotsByPosition(same_entry);
                syncEntryLegacyMirror(same_entry);
                tageStats.updateAllocSuccess++;
                allocated_table = ti;
                allocated_index = newIndex;
                allocated_way = same_tag_way;
                allocated_slot = findSlotByPosition(same_entry, position);
                return true;
            }

            for (unsigned slot = 0; slot < same_entry.slots.size(); ++slot) {
                const auto &cand_slot = same_entry.slots[slot];
                if (!cand_slot.useful && isWeakishCounter(cand_slot.counter)) {
                    replace_slot = static_cast<int>(slot);
                    break;
                }
            }

            // same-tag + the unique existing entry cannot absorb this position
            // via position hit or empty-slot fill.
            tageStats.allocSameTagFullBlocked++;
#ifndef UNIT_TEST
            tageStats.allocSameTagFullBlockedByTable[ti]++;
#endif
            if (first_invalid_way >= 0) {
                tageStats.allocSameTagFullBlockedWhileInvalidWayExists++;
#ifndef UNIT_TEST
                tageStats.allocSameTagFullBlockedWhileInvalidWayExistsByTable[ti]++;
#endif
            }

            if (replace_slot >= 0) {
                classifyReusedSameTagEntry(same_tag_way);
                tageStats.allocSameTagReplaceWeakishSlot++;
#ifndef UNIT_TEST
                tageStats.allocSameTagReplaceWeakishSlotByTable[ti]++;
#endif
                same_entry.slots[replace_slot] = TageSlot(true, position, initCounter, false);
                sortEntrySlotsByPosition(same_entry);
                syncEntryLegacyMirror(same_entry);
                tageStats.updateAllocSuccess++;
                allocated_table = ti;
                allocated_index = newIndex;
                allocated_way = same_tag_way;
                allocated_slot = findSlotByPosition(same_entry, position);
                return true;
            }

            weakenFirstNonUsefulStrongSlot(ti, newIndex);
            noteAllocationFailure();
            return false;
        }

        // different-tag path: allocate into an invalid way, or evict a whole
        // entry only when every slot is unprotected.
        for (unsigned way = 0; way < ways; ++way) {
            auto &cand = set[way];
            if (!cand.valid || isEntryWholeEvictable(cand)) {
                if (!cand.valid && has_existing_different_tag_entry) {
                    tageStats.allocDifferentTagUseInvalidWay++;
#ifndef UNIT_TEST
                    tageStats.allocDifferentTagUseInvalidWayByTable[ti]++;
#endif
                } else if (cand.valid) {
                    tageStats.allocDifferentTagWholeEvict++;
#ifndef UNIT_TEST
                    tageStats.allocDifferentTagWholeEvictByTable[ti]++;
#endif
                }
                TageEntry new_entry;
                new_entry.valid = true;
                new_entry.tag = newTag;
                new_entry.lruCounter = 0;
                new_entry.slots[0] = TageSlot(true, position, initCounter, false);
                for (unsigned slot = 1; slot < new_entry.slots.size(); ++slot) {
                    new_entry.slots[slot] = TageSlot();
                }
                new_entry.pc = entry.pc;
                new_entry.ownerBlockBase = blockBase;
                syncEntryLegacyMirror(new_entry);

                DPRINTF(TAGE,
                        "%s whole entry in table %d[%lu][%u], tag %lu, initial slot pos %u, ctr %d, pc %#lx\n",
                        cand.valid ? "evicting" : "allocating",
                        ti, newIndex, way, newTag, position, initCounter, entry.pc);
                cand = new_entry;
                tageStats.updateAllocSuccess++;
                allocated_table = ti;
                allocated_index = newIndex;
                allocated_way = way;
                allocated_slot = 0;
                usefulResetCnt = usefulResetCnt <= 0 ? 0 : usefulResetCnt - 1;
                return true;
            }
        }

        // No whole-entry victim: weaken one non-useful strong slot by one step
        // toward zero to make a later replacement possible.
        tageStats.allocDifferentTagBlockedProtected++;
#ifndef UNIT_TEST
        tageStats.allocDifferentTagBlockedProtectedByTable[ti]++;
#endif
        if (weakenFirstNonUsefulStrongSlot(ti, newIndex)) {
            tageStats.allocDifferentTagWeakenOnly++;
#ifndef UNIT_TEST
            tageStats.allocDifferentTagWeakenOnlyByTable[ti]++;
#endif
        }

        noteAllocationFailure();
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
        } else {
            recomputed = original_pred;
        }
        if (recomputed.taken != actual_taken) {
            hasRecomputedVsActualDiff = true;
        }

        // Update predictor state and check if need to allocate new entry
        bool need_allocate = updatePredictorStateAndCheckAllocation(btb_entry, actual_taken, recomputed, stream);

        // Handle new entry allocation if needed
        bool alloc_success = false;
        uint64_t allocated_table = 0;
        uint64_t allocated_index = 0;
        uint64_t allocated_way = 0;
        uint64_t allocated_slot = 0;
        if (need_allocate) {

            // Handle allocation of new entries
            uint start_table = 0;
            auto &main_info = recomputed.mainInfo;
            if (main_info.found) {
                start_table = main_info.table + 1; // start from the table after the main prediction table
            }
            alloc_success = handleNewEntryAllocation(startAddr, btb_entry, actual_taken,
                                   start_table, predMeta, allocated_table, allocated_index,
                                   allocated_way, allocated_slot);
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
            t.set(startAddr, btb_entry.pc, main_info.way,
                main_info.found, main_info.slotInfo.counter, main_info.slotInfo.useful,
                main_info.table, main_info.index, main_info.way, main_info.slot,
                alt_info.found, alt_info.slotInfo.counter, alt_info.slotInfo.useful,
                alt_info.table, alt_info.index, alt_info.way, alt_info.slot,
                trace_pred.useAlt, trace_pred.taken, actual_taken, alloc_success,
                allocated_table, allocated_index, allocated_way, allocated_slot,
                history_str, predMeta->indexFoldedHist[main_info.table].get());
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
BTBTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist)
{
    // Create mask for tableTagBits[t] to limit result size
    Addr mask = (1ULL << tableTagBits[t]) - 1;

    Addr blockBase = pc & ~(blockSize - 1);
    unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    pcShift += tableIndexBits[t] - 1;   // since tableIndexBits = log(2048) = 11, RTL is 10
    Addr pcBits = (blockBase >> pcShift) & mask;

    // Extract and prepare folded history bits
    Addr foldedBits = foldedHist & mask;

    // Extract alt tag bits and shift left by 1
    Addr altTagBits = (altFoldedHist << 1) & mask;

    // XOR block-level components (position is no longer part of tag in stage-1).
    return pcBits ^ foldedBits ^ altTagBits;
}

Addr
BTBTAGE::getTageTag(Addr pc, int t)
{
    return getTageTag(pc, t, tagFoldedHist[t].get(), altTagFoldedHist[t].get());
}

Addr
BTBTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist)
{
    const unsigned width = tableIndexBits[t];
    // Create mask for tableIndexBits[t] to limit result size.
    Addr mask = (1ULL << width) - 1;
    const unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    Addr pcBits = (pc >> pcShift) & mask;
    Addr foldedBits = foldedHist & mask;
    Addr mixedBits = pcBits ^ foldedBits;

    if (indexHistMixMode != IndexHistMixMode::Full && width > 0) {
        const unsigned lowBits = width / 2;
        const unsigned highBits = width - lowBits;
        const Addr lowMask = lowBits == 0 ? 0 : ((Addr(1) << lowBits) - 1);
        const Addr highMask = mask & ~lowMask;

        if (indexHistMixMode == IndexHistMixMode::Low) {
            // Low mode: only the low half of the original pcBits window
            // participates in XOR; the high half comes directly from history.
            mixedBits = (foldedBits & highMask) |
                        ((pcBits ^ foldedBits) & lowMask);
        } else {
            assert(indexHistMixMode == IndexHistMixMode::High);
            // High mode: only the high half of the original pcBits window
            // participates in XOR; the low half comes directly from history.
            mixedBits = (foldedBits & lowMask) |
                        ((pcBits ^ foldedBits) & highMask);
        }
    }

    // Support non-power-of-two table sizes when tuning capacities.
    return mixedBits % tableSizes[t];
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
BTBTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, bool taken, Addr pc, Addr target)
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
BTBTAGE::specUpdatePHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred)
{
    auto [pc, target, taken] = pred.getPHistInfo();
    doUpdateHist(history, taken, pc, target);
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
    ADD_STAT(predTagHitSlotMiss,
             statistics::units::Count::get(),
             "prediction-time tag hits whose unique same-tag entry has no "
             "matching slot"),
    ADD_STAT(allocSameTagTrueBlockReuse, statistics::units::Count::get(),
             "same-tag cases that ultimately reuse an existing same-tag "
             "entry owned by the same block"),
    ADD_STAT(allocSameTagAliasCollision, statistics::units::Count::get(),
             "same-tag cases that ultimately reuse an existing same-tag "
             "entry owned by a different block"),
    ADD_STAT(allocSameTagWhileInvalidWayExists,
             statistics::units::Count::get(),
             "same-tag cases observed while the set still had an invalid "
             "way"),
    ADD_STAT(allocSameTagFillEmptySlot, statistics::units::Count::get(),
             "same-tag position misses that fill an empty slot"),
    ADD_STAT(allocSameTagReplaceWeakishSlot, statistics::units::Count::get(),
             "same-tag position misses that replace a weakish non-useful "
             "slot inside the unique same-tag entry"),
    ADD_STAT(allocSameTagFullBlocked, statistics::units::Count::get(),
             "same-tag position misses that no existing same-tag entry can "
             "absorb via hit or empty-slot fill"),
    ADD_STAT(allocSameTagFullBlockedWhileInvalidWayExists,
             statistics::units::Count::get(),
             "same-tag absorption failures observed while the set still had "
             "an invalid way"),
    ADD_STAT(allocSameTagSpillUseInvalidWay, statistics::units::Count::get(),
             "reserved sanity counter: unique same-tag policy should never "
             "spill into an invalid way"),
    ADD_STAT(allocSameTagSpillWholeEvict, statistics::units::Count::get(),
             "reserved sanity counter: unique same-tag policy should never "
             "whole-entry spill into another way"),
    ADD_STAT(allocSameTagSpillWholeEvictSameTagVictim,
             statistics::units::Count::get(),
             "reserved sanity counter: strict same-tag spill policy should "
             "not whole-evict a same-tag victim"),
    ADD_STAT(allocSameTagSpillWholeEvictDifferentTagVictim,
             statistics::units::Count::get(),
             "reserved sanity counter for forbidden same-tag whole-entry "
             "spills against a different-tag victim"),
    ADD_STAT(allocDifferentTagUseInvalidWay, statistics::units::Count::get(),
             "different-tag allocations that use an invalid way while the "
             "set already contains another valid different-tag entry"),
    ADD_STAT(allocDifferentTagWholeEvict, statistics::units::Count::get(),
             "different-tag allocations that succeed by whole-entry "
             "eviction"),
    ADD_STAT(allocDifferentTagBlockedProtected,
             statistics::units::Count::get(),
             "different-tag allocations blocked because no whole-entry "
             "victim was unprotected"),
    ADD_STAT(allocDifferentTagWeakenOnly, statistics::units::Count::get(),
             "different-tag blocked cases that only weaken one slot "
             "counter"),
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
    ADD_STAT(predTableHits, statistics::units::Count::get(), "hit of each tage table on prediction"),
    ADD_STAT(updateTableHits, statistics::units::Count::get(), "hit of each tage table on update"),
    ADD_STAT(updateTableMispreds, statistics::units::Count::get(), "mispreds of each table when update"),
    ADD_STAT(predFinalSourceTable, statistics::units::Count::get(), "predictions whose final source is a TAGE table"),
    ADD_STAT(updateFinalSourceTableCorrect, statistics::units::Count::get(), "correct predictions grouped by final-source table"),
    ADD_STAT(updateFinalSourceTableWrong, statistics::units::Count::get(), "wrong predictions grouped by final-source table"),
    ADD_STAT(predTagHitSlotMissByTable,
             statistics::units::Count::get(),
             "prediction-time tag hits whose unique same-tag entry has slot "
             "miss, grouped by table"),
    ADD_STAT(allocSameTagTrueBlockReuseByTable,
             statistics::units::Count::get(),
             "same-tag reuse of a same-block existing entry, grouped by "
             "table"),
    ADD_STAT(allocSameTagAliasCollisionByTable,
             statistics::units::Count::get(),
             "same-tag reuse of a different-block existing entry, grouped "
             "by table"),
    ADD_STAT(allocSameTagWhileInvalidWayExistsByTable,
             statistics::units::Count::get(),
             "same-tag cases while an invalid way still existed, grouped by "
             "table"),
    ADD_STAT(allocSameTagFillEmptySlotByTable,
             statistics::units::Count::get(),
             "same-tag fills into empty slots, grouped by table"),
    ADD_STAT(allocSameTagReplaceWeakishSlotByTable,
             statistics::units::Count::get(),
             "same-tag weakish-slot replacements inside the unique entry, "
             "grouped by table"),
    ADD_STAT(allocSameTagFullBlockedByTable,
             statistics::units::Count::get(),
             "same-tag hit/fill absorption failures, grouped by table"),
    ADD_STAT(allocSameTagFullBlockedWhileInvalidWayExistsByTable,
             statistics::units::Count::get(),
             "same-tag absorption failures while an invalid way existed, "
             "grouped by table"),
    ADD_STAT(allocSameTagSpillUseInvalidWayByTable,
             statistics::units::Count::get(),
             "reserved sanity counter for unique same-tag policy spilling "
             "via invalid way, grouped by table"),
    ADD_STAT(allocSameTagSpillWholeEvictByTable,
             statistics::units::Count::get(),
             "reserved sanity counter for unique same-tag policy whole-entry "
             "spills, grouped by table"),
    ADD_STAT(allocSameTagSpillWholeEvictSameTagVictimByTable,
             statistics::units::Count::get(),
             "reserved sanity counter for same-tag whole-entry victims, "
             "grouped by table"),
    ADD_STAT(allocSameTagSpillWholeEvictDifferentTagVictimByTable,
             statistics::units::Count::get(),
             "reserved sanity counter for forbidden same-tag whole-entry "
             "spills against a different-tag victim, grouped by table"),
    ADD_STAT(allocDifferentTagUseInvalidWayByTable,
             statistics::units::Count::get(),
             "different-tag allocations via invalid ways while the set "
             "already contains another valid different-tag entry, grouped "
             "by table"),
    ADD_STAT(allocDifferentTagWholeEvictByTable,
             statistics::units::Count::get(),
             "different-tag allocations via whole-entry eviction, grouped "
             "by table"),
    ADD_STAT(allocDifferentTagBlockedProtectedByTable,
             statistics::units::Count::get(),
             "different-tag blocked-protected cases, grouped by table"),
    ADD_STAT(allocDifferentTagWeakenOnlyByTable,
             statistics::units::Count::get(),
             "different-tag weaken-only cases, grouped by table"),

    ADD_STAT(condPredwrong, statistics::units::Count::get(), "number of conditional branch mispredictions committed"),
    ADD_STAT(condMissTakens, statistics::units::Count::get(),
             "number of conditional branch mispredictions committed with no "
             "prediction"),
    ADD_STAT(condCorrect, statistics::units::Count::get(),
             "number of conditional branch correct predictions committed"),
    ADD_STAT(condMissNoTakens, statistics::units::Count::get(),
             "number of conditional branch correct predictions committed "
             "with no prediction"),
    ADD_STAT(predHit, statistics::units::Count::get(), "number of conditional branch predictions that hit"),
    ADD_STAT(predMiss, statistics::units::Count::get(), "number of conditional branch predictions that miss")
{
    predTableHits.init(0, numPredictors-1, 1);
    updateTableHits.init(0, numPredictors-1, 1);
    updateTableMispreds.init(numPredictors);
    predFinalSourceTable.init(numPredictors);
    updateFinalSourceTableCorrect.init(numPredictors);
    updateFinalSourceTableWrong.init(numPredictors);
    predTagHitSlotMissByTable.init(numPredictors);
    allocSameTagTrueBlockReuseByTable.init(numPredictors);
    allocSameTagAliasCollisionByTable.init(numPredictors);
    allocSameTagWhileInvalidWayExistsByTable.init(numPredictors);
    allocSameTagFillEmptySlotByTable.init(numPredictors);
    allocSameTagReplaceWeakishSlotByTable.init(numPredictors);
    allocSameTagFullBlockedByTable.init(numPredictors);
    allocSameTagFullBlockedWhileInvalidWayExistsByTable.init(numPredictors);
    allocSameTagSpillUseInvalidWayByTable.init(numPredictors);
    allocSameTagSpillWholeEvictByTable.init(numPredictors);
    allocSameTagSpillWholeEvictSameTagVictimByTable.init(numPredictors);
    allocSameTagSpillWholeEvictDifferentTagVictimByTable.init(numPredictors);
    allocDifferentTagUseInvalidWayByTable.init(numPredictors);
    allocDifferentTagWholeEvictByTable.init(numPredictors);
    allocDifferentTagBlockedProtectedByTable.init(numPredictors);
    allocDifferentTagWeakenOnlyByTable.init(numPredictors);

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
