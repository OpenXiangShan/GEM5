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

#ifdef UNIT_TEST
namespace test {
#endif

#ifdef UNIT_TEST
// Test constructor for unit testing mode
BTBTAGE::BTBTAGE(unsigned numPredictors, unsigned numWays, unsigned tableSize, unsigned numBanks)
    : TimedBaseBTBPredictor(),
      numPredictors(numPredictors),
      numWays(numWays),
      baseTableSize(2048),
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
maxHistLen(p.maxHistLen),
numWays(p.numWays),
baseTableSize(p.baseTableSize),
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
    this->needMoreHistories = p.needMoreHistories;

    // Warn if updateOnRead is disabled (bank simulation works better with it enabled)
    if (!p.updateOnRead) {
        warn("BTBTAGE: Bank simulation works better with updateOnRead=true");
    }
#endif
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    // Initialize base table for fallback predictions
    baseTable.resize(baseTableSize);
    for (unsigned i = 0; i < baseTable.size(); ++i) {
        baseTable[i].resize(maxBranchPositions, 0);  // Initialize counters to 0 (weakly taken)
    }

    for (unsigned int i = 0; i < numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);
        for (unsigned int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numWays);
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
            std::make_pair("altFound", UINT64),
            std::make_pair("altCounter", UINT64),
            std::make_pair("altUseful", UINT64),
            std::make_pair("altTable", UINT64),
            std::make_pair("altIndex", UINT64),
            std::make_pair("useAlt", UINT64),
            std::make_pair("predTaken", UINT64),
            std::make_pair("actualTaken", UINT64),
            std::make_pair("allocSuccess", UINT64),
            std::make_pair("allocTable", UINT64),
            std::make_pair("allocIndex", UINT64),
            std::make_pair("allocWay", UINT64),
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
        for (unsigned way = 0; way < numWays; way++) {
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
    Addr base_idx = getBaseTableIndex(startPC);
    unsigned branch_idx = getBranchIndexInBlock(btb_entry.pc, startPC);
    bool base_taken = getDelay() != 0 ? baseTable[base_idx][branch_idx] >= 0 : btb_entry.ctr >= 0;
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

    DPRINTF(TAGE, "tage predict %#lx taken %d\n", btb_entry.pc, taken);
    DPRINTF(TAGE, "tage use_alt %d ? (alt_provided %d ? alt_taken %d : base_taken %d) : main_taken %d\n",
        use_alt, alt_provided, alt_taken, base_taken, main_taken);

    return TagePrediction(btb_entry.pc, main_info, alt_info, use_alt, taken, alt_pred);
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
BTBTAGE::prepareUpdateEntries(const FetchStream &stream) {
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
                             const FetchStream &stream) {
    tageStats.updateStatsWithTagePrediction(pred, false);

    auto &main_info = pred.mainInfo;
    auto &alt_info = pred.altInfo;
    bool used_alt = pred.useAlt;
    // Use base table instead of entry.ctr for fallback prediction
    Addr startPC = stream.getRealStartPC();
    Addr base_idx = getBaseTableIndex(startPC);
    unsigned branch_idx = getBranchIndexInBlock(entry.pc, startPC);
    bool base_taken = baseTable[base_idx][branch_idx] >= 0;
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
    if (main_info.found) {
        DPRINTF(TAGE, "prediction provided by table %d, idx %lu, way %u, updating corresponding entry\n",
            main_info.table, main_info.index, main_info.way);

        auto &way = tageTable[main_info.table][main_info.index][main_info.way];

        // Update prediction counter
        updateCounter(actual_taken, 3, way.counter);

        // Update useful bit based on several conditions
        bool main_is_correct = main_info.taken() == actual_taken;
        bool alt_is_correct_and_strong = alt_info.found &&
                                     (alt_info.taken() == actual_taken) &&
                                     (abs(2 * alt_info.entry.counter + 1) == 7);

        // a. Special reset (humility mechanism)
        if (alt_is_correct_and_strong && main_is_correct) {
            way.useful = 0;
            DPRINTF(TAGEUseful, "useful bit reset to 0 due to humility rule\n");
        } else if (main_info.taken() != alt_taken) {
            // b. Original logic to set useful bit high
            if (main_is_correct) {
                way.useful = 1;
            }
        }

        // c. Reset u on counter sign flip (becomes weak)
        if (way.counter == 0 || way.counter == -1) {
            way.useful = 0;
            DPRINTF(TAGEUseful, "useful bit reset to 0 due to weak counter\n");
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

    // Update base table counter if used as fallback
    if (used_alt && !alt_info.found) {
        DPRINTF(TAGE, "prediction provided by base table idx %lu, branch %u, updating corresponding entry\n",
                base_idx, branch_idx);
        updateCounter(actual_taken, 2, baseTable[base_idx][branch_idx]);
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
                                 uint64_t &allocated_way) {
    // Simple set-associative allocation (no LFSR, no per-way table gating):
    // - For each table from start_table upward, check the set at computed index.
    // - Prefer invalid ways; else choose any way with useful==0 and weak counter.
    // - If none, apply a one-step age penalty to a strong, not-useful way (no allocation).

    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(entry.pc, startPC);

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        Addr newIndex = getTageIndex(startPC, ti, meta->indexFoldedHist[ti].get());
        Addr newTag = getTageTag(startPC, ti,
            meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get(), position);

        auto &set = tageTable[ti][newIndex];

        // Allocate into invalid way or not-useful and weak way
        for (unsigned way = 0; way < numWays; ++way) {
            auto &cand = set[way];
            const bool weakish = std::abs(cand.counter * 2 + 1) <= 3; // -3,-2,-1,0,1,2
            if (!cand.valid || (!cand.useful && weakish)) {
                short newCounter = actual_taken ? 0 : -1;
                DPRINTF(TAGE, "allocating entry in table %d[%lu][%u], tag %lu (with pos %u), counter %d, pc %#lx\n",
                        ti, newIndex, way, newTag, position, newCounter, entry.pc);
                cand = TageEntry(newTag, newCounter, entry.pc); // u = 0 default
                tageStats.updateAllocSuccess++;
                allocated_table = ti;
                allocated_index = newIndex;
                allocated_way = way;
                usefulResetCnt = usefulResetCnt <= 0 ? 0 : usefulResetCnt - 1;
                return true;
            }
        }

        // 3) Apply age penalty to one strong, not-useful way to make it replacable later
        for (unsigned way = 0; way < numWays; ++way) {
            auto &cand = set[way];
            const bool weakish = std::abs(cand.counter * 2 + 1) <= 3;
            if (!cand.useful && !weakish) {
                if (cand.counter > 0) cand.counter--; else cand.counter++;
                DPRINTF(TAGE, "age penalty applied on table %d[%lu][%u], new ctr %d\n",
                        ti, newIndex, way, cand.counter);
                break; // one penalty per table per update
            }
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

/**
 * @brief Probe resolved update for bank conflicts without mutating state.
 * Returns false if the update cannot proceed due to a bank conflict.
 */
bool
BTBTAGE::canResolveUpdate(const FetchStream &stream) {
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
BTBTAGE::doResolveUpdate(const FetchStream &stream) {
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
BTBTAGE::update(const FetchStream &stream) {
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
    for (auto &btb_entry : entries_to_update) {
        bool actual_taken = stream.exeTaken && stream.exeBranchInfo == btb_entry;
        TagePrediction recomputed;
        if (updateOnRead) { // if update on read is enabled, re-read providers using snapshot
            // Re-read providers using snapshot (do not rely on prediction-time main/alt)
            recomputed = generateSinglePrediction(btb_entry, startAddr, predMeta);
        } else { // otherwise, use the prediction from the prediction-time main/alt
            recomputed = predMeta->preds[btb_entry.pc];
        }

        // Update predictor state and check if need to allocate new entry
        bool need_allocate = updatePredictorStateAndCheckAllocation(btb_entry, actual_taken, recomputed, stream);

        // Handle new entry allocation if needed
        bool alloc_success = false;
        uint64_t allocated_table = 0;
        uint64_t allocated_index = 0;
        uint64_t allocated_way = 0;
        if (need_allocate) {

            // Handle allocation of new entries
            uint start_table = 0;
            auto &main_info = recomputed.mainInfo;
            if (main_info.found) {
                start_table = main_info.table + 1; // start from the table after the main prediction table
            }
            alloc_success = handleNewEntryAllocation(startAddr, btb_entry, actual_taken,
                                   start_table, predMeta, allocated_table, allocated_index, allocated_way);
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
            auto main_info = recomputed.mainInfo;
            auto alt_info = recomputed.altInfo;
            t.set(startAddr, btb_entry.pc, main_info.way,
                main_info.found, main_info.entry.counter, main_info.entry.useful,
                main_info.table, main_info.index,
                alt_info.found, alt_info.entry.counter, alt_info.entry.useful,
                alt_info.table, alt_info.index,
                recomputed.useAlt, recomputed.taken, actual_taken, alloc_success,
                allocated_table, allocated_index, allocated_way,
                history_str, predMeta->indexFoldedHist[main_info.table].get());
            tageMissTrace->write_record(t);
        }
#endif
    }
    if (getDelay() <2){
        checkUtageUpdateMisspred(stream);
    }
    DPRINTF(TAGE, "end update\n");
}

void
BTBTAGE::checkUtageUpdateMisspred(const FetchStream &stream) {
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

    // Extract lower bits of PC directly (remove instruction alignment bits)
    Addr pcBits = (pc >> bankBaseShift) & mask;

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

    return pcBits ^ foldedBits;
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
BTBTAGE::getUseAltIdx(Addr pc) {
    Addr shiftedPc = pc >> instShiftAmt;
    return shiftedPc & (useAltOnNaSize - 1);
}

Addr
BTBTAGE::getBaseTableIndex(Addr pc) {
    // Use blockSize-aligned address as index; block offset bits captured by blockWidth
    return ((pc >> blockWidth) & (baseTableSize - 1));
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
    const FetchStream &entry, int shamt, bool cond_taken)
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
    ADD_STAT(updateBankConflict, statistics::units::Count::get(), "number of bank conflicts detected"),
    ADD_STAT(updateDeferredDueToConflict, statistics::units::Count::get(), "number of updates deferred due to bank conflict (retried later)"),
    ADD_STAT(updateBankConflictPerBank, statistics::units::Count::get(), "bank conflicts per bank"),
    ADD_STAT(updateAccessPerBank, statistics::units::Count::get(), "update accesses per bank"),
    ADD_STAT(predAccessPerBank, statistics::units::Count::get(), "prediction accesses per bank"),
    ADD_STAT(predTableHits, statistics::units::Count::get(), "hit of each tage table on prediction"),
    ADD_STAT(updateTableHits, statistics::units::Count::get(), "hit of each tage table on update"),
    ADD_STAT(updateTableMispreds, statistics::units::Count::get(), "mispreds of each table when update"),

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
    for (unsigned i = 0; i < numWays; i++) {
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

#ifndef UNIT_TEST
void
BTBTAGE::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
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
