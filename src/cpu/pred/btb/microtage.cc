#include "cpu/pred/btb/microtage.hh"

#include <algorithm>
#include <cmath>

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
#include "debug/UTAGE.hh"

#endif
namespace gem5 {

namespace branch_prediction {

namespace btb_pred{

#ifdef UNIT_TEST
namespace test {
#endif

#ifdef UNIT_TEST
// Test constructor for unit testing mode
MicroTAGE::MicroTAGE(unsigned numPredictors, unsigned numWays, unsigned tableSize, unsigned numBanks)
    : TimedBaseBTBPredictor(),
      numPredictors(numPredictors),
      numWays(numWays),
      maxBranchPositions(32),
      updateOnRead(false),
      numBanks(numBanks),
      bankIdWidth(ceilLog2(numBanks)),
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
#else
// Constructor: Initialize TAGE predictor with given parameters
MicroTAGE::MicroTAGE(const Params& p):
TimedBaseBTBPredictor(p),
numPredictors(p.numPredictors),
tableSizes(p.tableSizes),
tableTagBits(p.TTagBitSizes),
tablePcShifts(p.TTagPcShifts),
histLengths(p.histLengths),
maxHistLen(p.maxHistLen),
numWays(p.numWays),
maxBranchPositions(p.maxBranchPositions),
updateOnRead(p.updateOnRead),
numBanks(p.numBanks),
bankIdWidth(ceilLog2(p.numBanks)),
bankBaseShift(instShiftAmt), // strip instruction alignment bits before indexing
indexShift(bankBaseShift + ceilLog2(p.numBanks)),
enableBankConflict(p.enableBankConflict),
lastPredBankId(0),
predBankValid(false),
tageStats(this, p.numPredictors, p.numBanks)
{
    // Warn if updateOnRead is disabled (bank simulation works better with it enabled)
    if (!p.updateOnRead) {
        warn("MicroTAGE: Bank simulation works better with updateOnRead=true");
    }
#endif
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    // Ensure PC shift vector has entries for all predictors (fallback default = 1)
    if (tablePcShifts.size() < numPredictors) {
        tablePcShifts.resize(numPredictors, 1);
    }

    // Initialize base table for fallback predictions
    threadHistory.resize(MaxThreads);
    threadMeta.resize(MaxThreads);

    for (unsigned int i = 0; i < numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);
        for (unsigned int j = 0; j < tableSizes[i]; ++j) {
            tageTable[i][j].resize(numWays);
        }

        tableIndexBits[i] = ceilLog2(tableSizes[i]);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);

        assert(tablePcShifts.size() >= numPredictors);

        for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
            auto &state = threadHistory[tid];
            state.tagFoldedHist.emplace_back(
                (int)histLengths[i], (int)tableTagBits[i], 16);
            state.altTagFoldedHist.emplace_back(
                (int)histLengths[i], (int)tableTagBits[i] - 1, 16);
            state.indexFoldedHist.emplace_back(
                (int)histLengths[i], (int)tableIndexBits[i], 16);
        }
    }
    usefulResetCnt = 0;

#ifndef UNIT_TEST
    hasDB = true;
    dbName = std::string("microtage");
#endif
}

MicroTAGE::~MicroTAGE()
{
}

ThreadID
MicroTAGE::predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const
{
    assert(!stagePreds.empty());
    return stagePreds.front().tid;
}

MicroTAGE::ThreadHistoryState &
MicroTAGE::historyState(ThreadID tid)
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

const MicroTAGE::ThreadHistoryState &
MicroTAGE::historyState(ThreadID tid) const
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

// Set up tracing for debugging
void
MicroTAGE::setTrace()
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
MicroTAGE::tick() {}

void
MicroTAGE::tickStart() {}

/**
 * @brief Generate prediction for a single branch by searching TAGE tables
 *
 * @param branchPC The branch PC to generate prediction for
 * @param baseTaken The base-table direction for this branch
 * @param startPC The starting PC address for calculating indices and tags
 * @param predMeta Optional prediction metadata; if provided, use snapshot for index/tag
 *             calculation (update path); if nullptr, use current folded history (prediction path)
 * @return TagePrediction containing main and alternative predictions
 */
MicroTAGE::TagePrediction
MicroTAGE::generateSinglePrediction(Addr branchPC,
                                 bool baseTaken,
                                 const Addr &startPC,
                                 std::shared_ptr<TageMeta> predMeta,
                                 ThreadID tid,
                                 uint8_t asidHash) {
    DPRINTF(UTAGE, "generateSinglePrediction for pc: %#lx\n", branchPC);
    const auto &state = historyState(tid);

    bool provided = false;
    TageTableInfo main_info;

    // Search from highest to lowest table for matches
    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(branchPC, startPC);

    for (int i = numPredictors - 1; i >= 0; --i) {
        // Calculate index and tag: use snapshot if provided, otherwise use current folded history
        // Tag includes position XOR (like RTL: tag = tempTag ^ cfiPosition)
        Addr index = predMeta ? getTageIndex(startPC, i,
                            predMeta->indexFoldedHist[i].get(), asidHash)
                          : getTageIndex(startPC, i, state.indexFoldedHist[i].get(), asidHash);
        Addr tag = predMeta ? getTageTag(startPC, i,
                            predMeta->tagFoldedHist[i].get(),predMeta->altTagFoldedHist[i].get(),
                            position, asidHash)
                        : getTageTag(startPC, i, state.tagFoldedHist[i].get(),
                                     state.altTagFoldedHist[i].get(), position, asidHash);

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

                DPRINTF(UTAGE, "hit  table %d[%lu][%u]: valid %d, tag %lu, ctr %d, useful %d, pc %#lx, pos %u\n",
                    i, index, way, entry.valid, entry.tag, entry.counter, entry.useful, branchPC, position);
                break;  // only one way can be matched, avoid multi-hit, TODO: RTL behavior?
            }
        }

        if (match) {
            if (!provided) {
                // First match becomes main prediction
                main_info = TageTableInfo(true, matching_entry, i, index, tag, matching_way);
                provided = true;
            }
        } else {
            DPRINTF(UTAGE, "miss table %d[%lu] for tag %lu (with pos %u), pc %#lx\n",
                i, index, tag, position, branchPC);
        }
    }

    // Generate final prediction
    bool main_taken = main_info.taken();
    bool base_pred = baseTaken;

    bool taken = provided ? main_taken : base_pred;

    DPRINTF(UTAGE, "tage predict %#lx taken %d\n", branchPC, taken);
    DPRINTF(UTAGE, "tage main provided %d ? main_taken %d : base_taken %d\n",
            provided, main_taken, base_pred);

    return TagePrediction(branchPC, main_info, provided, taken, base_pred);
}

/**
 * @brief Look up predictions in TAGE tables for a stream of instructions
 *
 * @param startPC The starting PC address for the instruction stream
 * @param btbEntries Vector of BTB entries to make predictions for
 * @return Map of branch PC addresses to their predicted outcomes
 */
void
MicroTAGE::lookupHelper(const Addr &startPC, const std::vector<BTBEntry> &btbEntries,
                        CondTakens& results, ThreadID tid, uint8_t asidHash)
{
    DPRINTF(UTAGE, "lookupHelper startAddr: %#lx\n", startPC);

    // Process each BTB entry to make predictions
    for (auto &btb_entry : btbEntries) {
        // Only predict for valid conditional branches
        if (btb_entry.isCond && btb_entry.valid) {
            auto pred = generateSinglePrediction(btb_entry.pc, btb_entry.ctr >= 0,
                                                 startPC, nullptr, tid,
                                                 asidHash);
            threadMeta[tid]->preds[btb_entry.pc] = pred;
            tageStats.updateStatsWithTagePrediction(pred, true);
            results.push_back({btb_entry.pc, pred.taken});
        }
    }
}

void
MicroTAGE::dryRunCycle(Addr startPC) {
    // No operation in dry run cycle for MicroTAGE
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
MicroTAGE::putPCHistory(Addr startPC, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) {
    const ThreadID tid = predictorTid(stagePreds);
    const uint8_t asidHash = stagePreds.empty() ? 0 : stagePreds.front().asidHash;
    const auto &state = historyState(tid);
    // Record prediction bank for next tick's conflict detection
    lastPredBankId = getBankId(startPC);
    predBankValid = true;

#ifndef UNIT_TEST
    // Record prediction access per bank
    tageStats.predAccessPerBank[lastPredBankId]++;
#endif

    DPRINTF(UTAGE, "putPCHistory startAddr: %#lx, bank: %u\n",
            startPC, lastPredBankId);

    // IMPORTANT: when this function is called,
    // btb entries should already be in stagePreds
    // get prediction and save it

    // Clear old prediction metadata and save current history state
    threadMeta[tid] = std::make_shared<TageMeta>();
    threadMeta[tid]->tagFoldedHist = state.tagFoldedHist;
    threadMeta[tid]->altTagFoldedHist = state.altTagFoldedHist;
    threadMeta[tid]->indexFoldedHist = state.indexFoldedHist;
    threadMeta[tid]->aheadIndexFoldedHistValid =
        !state.aheadIndexFoldedHist.empty();
    if (threadMeta[tid]->aheadIndexFoldedHistValid) {
        threadMeta[tid]->aheadIndexFoldedHist =
            state.aheadIndexFoldedHist.front();
    } else {
        threadMeta[tid]->aheadIndexFoldedHist.clear();
    }
    threadMeta[tid]->history = history;

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        stage_pred.condTakens.clear();
        lookupHelper(startPC, stage_pred.btbEntries, stage_pred.condTakens,
                     tid, asidHash);
    }

}

std::shared_ptr<void>
MicroTAGE::getPredictionMeta(ThreadID tid) {
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
 * @brief Update predictor state for a single entry
 *
 * @param branchPC The branch PC being updated
 * @param baseTaken The base-table direction for this branch
 * @param actual_taken The actual outcome of the branch
 * @param pred The prediction made for this entry
 * @param stream The fetch stream containing update information
 * @return true if need to allocate new entry
 */
bool
MicroTAGE::updatePredictorStateAndCheckAllocation(Addr branchPC,
                             bool baseTaken,
                             bool actual_taken,
                             const TagePrediction &pred,
                             bool actual_mispred) {
    tageStats.updateStatsWithTagePrediction(pred, false);

    auto &main_info = pred.mainInfo;
    bool used_base = !pred.mainprovided;
    const bool base_taken = baseTaken;

    // Update use_alt_on_na when provider is weak (0 or -1)
    if (main_info.found) {
        bool main_weak = (main_info.entry.counter == 0 || main_info.entry.counter == -1);
        if (main_weak) {
            tageStats.updateProviderNa++;
            bool base_correct = (base_taken == actual_taken);
            //updateCounter(base_correct, useAltOnNaWidth, useAlt[uidx]);
            tageStats.updateUseAltOnNaUpdated++;
            if (base_correct) {
                tageStats.updateUseAltOnNaCorrect++;
            } else {
                tageStats.updateUseAltOnNaWrong++;
            }
        }
    }

    // Update main prediction provider
    if (main_info.found) {
        DPRINTF(UTAGE, "prediction provided by table %d, idx %lu, way %u, updating corresponding entry\n",
            main_info.table, main_info.index, main_info.way);

        auto &way = tageTable[main_info.table][main_info.index][main_info.way];

        // Update prediction counter
        updateCounter(actual_taken, 3, way.counter);

        // Update useful bit based on several conditions
        bool main_is_correct = main_info.taken() == actual_taken;
        const bool base_is_correct_and_strong = false;

        // a. Special reset (humility mechanism)
        if (base_is_correct_and_strong && main_is_correct) {
            way.useful = 0;
            DPRINTF(TAGEUseful, "useful bit reset to 0 due to humility rule\n");
        } else if (main_info.taken() != base_taken) {
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
        DPRINTF(UTAGE, "useful bit is now %d\n", way.useful);

        // No LRU maintenance

        if (!main_is_correct) {
            tageStats.updateUtageHitWrong++;
        }
    }


    // Update statistics
    if (used_base) {
        bool base_correct = base_taken == actual_taken;
        if (base_correct) {
            tageStats.updateUseAltCorrect++;
        } else {
            tageStats.updateUseAltWrong++;

        }
        if (main_info.found && main_info.taken() != base_taken) {
            tageStats.updateAltDiffers++;
        }
    }

    // Check if misprediction occurred
    bool this_fb_mispred = actual_mispred;
    // No allocation if no misprediction
    if (!this_fb_mispred) {
        return false;
    }

    // All other cases: allocate longer history table
    return true;
}

/**
 * @brief Handle allocation of new entries
 *
 * @param startPC The starting PC address
 * @param branchPC The branch PC being updated
 * @param actual_taken The actual outcome of the branch
 * @param start_table The starting table for allocation
 * @param meta The metadata of the predictor
 * @return true if allocation is successful
 */
bool
MicroTAGE::handleNewEntryAllocation(const Addr &startPC,
                                 Addr branchPC,
                                 bool actual_taken,
                                 unsigned start_table,
                                 std::shared_ptr<TageMeta> meta,
                                 uint8_t asidHash,
                                 uint64_t &allocated_table,
                                 uint64_t &allocated_index,
                                 uint64_t &allocated_way) {
    // Simple set-associative allocation (no LFSR, no per-way table gating):
    // - For each table from start_table upward, check the set at computed index.
    // - Prefer invalid ways; else choose any way with useful==0 and weak counter.
    // - If none, apply a one-step age penalty to a strong, not-useful way (no allocation).

    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(branchPC, startPC);

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        Addr newIndex = getTageIndex(startPC, ti,
            meta->indexFoldedHist[ti].get(), asidHash);
        Addr newTag = getTageTag(startPC, ti,
            meta->tagFoldedHist[ti].get(), meta->altTagFoldedHist[ti].get(),
            position, asidHash);

        auto &set = tageTable[ti][newIndex];

        // Allocate into invalid way or not-useful and weak way
        for (unsigned way = 0; way < numWays; ++way) {
            auto &cand = set[way];
            const bool weakish = std::abs(cand.counter * 2 + 1) <= 3; // -3,-2,-1,0,1,2
            if (!cand.valid || (!cand.useful && weakish)) {
                short newCounter = actual_taken ? 0 : -1;
                DPRINTF(UTAGE, "allocating entry in table %d[%lu][%u], tag %lu (with pos %u), counter %d, pc %#lx\n",
                        ti, newIndex, way, newTag, position, newCounter, branchPC);
                cand = TageEntry(newTag, newCounter, branchPC); // u = 0 default
                tageStats.updateAllocSuccess++;
                allocated_table = ti;
                allocated_index = newIndex;
                allocated_way = way;
                usefulResetCnt = usefulResetCnt <= 0 ? 0 : usefulResetCnt - 1;
                return true;
            }
        }

        // 3) Apply age penalty to one strong, not-useful way to make it replaceable later
        for (unsigned way = 0; way < numWays; ++way) {
            auto &cand = set[way];
            const bool weakish = std::abs(cand.counter * 2 + 1) <= 3;
            if (!cand.useful && !weakish) {
                if (cand.counter > 0) cand.counter--; else cand.counter++;
                DPRINTF(UTAGE, "age penalty applied on table %d[%lu][%u], new ctr %d\n",
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
        DPRINTF(UTAGE, "reset useful bit of all entries\n");
        for (auto &table : tageTable) {
            for (auto &set : table) {
                for (auto &way : set) {
                    way.useful = false;
                }
            }
        }
    }

    DPRINTF(UTAGE, "no eligible way found for allocation starting from table %d\n", start_table);
    tageStats.updateAllocFailureNoValidTable++;
    return false;
}

/**
 * @brief Probe resolved update for bank conflicts without mutating state.
 * Returns false if the update cannot proceed due to a bank conflict.
 */
bool
MicroTAGE::canResolveUpdate(Addr update_start_pc) {
    unsigned updateBank = getBankId(update_start_pc);

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
        DPRINTF(UTAGE, "Bank conflict detected: update bank %u conflicts with prediction bank %u, "
                      "deferring this update (will retry after blocking prediction)\n",
                      updateBank, lastPredBankId);
        predBankValid = false;
        return false;
    }

    return true;
}

/**
 * @brief Retire prediction bank state after a resolved update probe succeeds.
 */
void
MicroTAGE::noteResolveUpdateAccepted(Addr) {
    if (enableBankConflict && predBankValid) {
        // Prediction consumed; clear bank tag for next cycle
        predBankValid = false;
    }
}

void
MicroTAGE::updateWithDirectionEntries(
    const std::vector<DirectionUpdateEntry> &entries,
    const BranchUpdateContext &ctx,
    const std::shared_ptr<void> &prediction_meta,
    const boost::dynamic_bitset<> &)
{
    // Get prediction metadata snapshot and bind to member for helpers
    auto predMeta = std::static_pointer_cast<TageMeta>(prediction_meta);
    if (!predMeta) {
        DPRINTF(UTAGE, "update: no prediction meta, skip\n");
        return;
    }

    updateWithEntries(entries, ctx, predMeta);
}

void
MicroTAGE::updateWithEntries(const std::vector<DirectionUpdateEntry> &entries,
                             const BranchUpdateContext &ctx,
                             const std::shared_ptr<TageMeta> &predMeta)
{
    Addr startAddr = ctx.startPC;
    unsigned updateBank = getBankId(startAddr);

    DPRINTF(UTAGE, "update startAddr: %#lx, bank: %u\n", startAddr, updateBank);

    bool utage_hit = false;
    // Process each branch entry
    for (const auto &update_entry : entries) {
        const auto &actual_branch = update_entry.actualBranch;
        const Addr branch_pc = actual_branch.pc;
        const bool base_taken = update_entry.baseTaken;
        if (!isBranchInPredictionBlock(branch_pc, startAddr)) {
            DPRINTF(UTAGE,
                    "update: skip pc %#lx outside prediction block start %#lx\n",
                    branch_pc, startAddr);
            continue;
        }
        const bool actual_taken = actual_branch.taken;
        TagePrediction recomputed;
        if (updateOnRead) { // if update on read is enabled, re-read providers using snapshot
            // Re-read providers using snapshot (do not rely on prediction-time main/alt)
            recomputed = generateSinglePrediction(branch_pc, base_taken,
                                                 startAddr, predMeta,
                                                 ctx.tid,
                                                 ctx.asidHash);
        } else { // otherwise, use the prediction from the prediction-time main/alt
            auto pred_it = predMeta->preds.find(branch_pc);
            if (pred_it != predMeta->preds.end()) {
                recomputed = pred_it->second;
            } else {
                DPRINTF(UTAGE, "update: missing predMeta entry for pc %#lx, recompute with snapshot\n",
                        branch_pc);
                recomputed = generateSinglePrediction(branch_pc, base_taken,
                                                     startAddr, predMeta,
                                                     ctx.tid,
                                                     ctx.asidHash);
            }
        }
        if (recomputed.mainprovided) {
            utage_hit = true;
        }
        // Update predictor state and check if need to allocate new entry
        bool need_allocate = updatePredictorStateAndCheckAllocation(
            branch_pc, base_taken, actual_taken, recomputed,
            actual_branch.mispred);

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
            alloc_success = handleNewEntryAllocation(startAddr, branch_pc, actual_taken,
                                   start_table, predMeta, ctx.asidHash,
                                   allocated_table, allocated_index, allocated_way);
        }

#ifndef UNIT_TEST
        // if (enableDB) {
        //     TageMissTrace t;
        //     std::string history_str;
        //     boost::dynamic_bitset<> history_low50 = predMeta->history;
        //     if (history_low50.size() > 50) {
        //         history_low50.resize(50);  // get the lower 50 bits of history
        //     }
        //     boost::to_string(history_low50, history_str);
        //     auto main_info = recomputed.mainInfo;
        //     t.set(startAddr, btb_entry.pc, main_info.way,
        //         main_info.found, main_info.entry.counter, main_info.entry.useful,
        //         main_info.table, main_info.index,
        //         recomputed.useAlt, recomputed.taken, actual_taken, alloc_success,
        //         allocated_table, allocated_index, allocated_way,
        //         history_str, predMeta->indexFoldedHist[main_info.table].get());
        //     tageMissTrace->write_record(t);
        // }
#endif
    }
    if (utage_hit){
        tageStats.updateUtageHit++;//for RTL align pred Accuracy
    }
    checkUtageUpdateMisspred(predMeta->preds, entries);
    DPRINTF(UTAGE, "end update\n");
}

void
MicroTAGE::checkUtageUpdateMisspred(
    const std::unordered_map<Addr, TagePrediction> &preds,
    const std::vector<DirectionUpdateEntry> &entries) {
    // used for MicroTAGE update misprediction counting
    // sort microtage predictions by pc to find the first taken branch
    std::vector<std::pair<Addr, TagePrediction>> lastPreds;
    lastPreds.reserve(preds.size());
    for (auto &kv : preds) {
        lastPreds.emplace_back(kv.first, kv.second);
    }
    std::sort(lastPreds.begin(), lastPreds.end(),
            [](const std::pair<Addr, TagePrediction> &a,
                const std::pair<Addr, TagePrediction> &b) {
                return a.first < b.first;
            });
    bool has_taken_pred = false;
    Addr first_taken_pc = 0;
    for (auto &entry_info : lastPreds) {
        if (entry_info.second.taken) {
            has_taken_pred = true;
            first_taken_pc = entry_info.first;
            break;
        }
    }
    const auto *first_actual_taken =
        findFirstTakenDirectionUpdateEntry(entries);
    const bool actual_taken = first_actual_taken != nullptr;
    const Addr first_actual_taken_pc =
        actual_taken ? first_actual_taken->actualBranch.pc : 0;
    bool fallthrough_mispred = (!has_taken_pred && actual_taken) ||
                                (has_taken_pred && !actual_taken);
    bool branch_mispred = actual_taken && has_taken_pred &&
                          first_taken_pc != first_actual_taken_pc;
    if (fallthrough_mispred || branch_mispred) {
        tageStats.updateMispred++;
    }
}

// Update prediction counter with saturation
void
MicroTAGE::updateCounter(bool taken, unsigned width, short &counter) {
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
MicroTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist,
                      Addr position, uint8_t asidHash)
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
    return injectAsidHashIntoTag(pcBits ^ foldedBits ^ position ^ altTagBits,
                                 tableTagBits[t], asidHash);
}

Addr
MicroTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist, uint8_t asidHash)
{
    // Create mask for tableIndexBits[t] to limit result size
    Addr mask = (1ULL << tableIndexBits[t]) - 1;

    const unsigned pcShift = enableBankConflict ? indexShift : bankBaseShift;
    Addr pcBits = (pc >> pcShift) & mask;
    Addr foldedBits = foldedHist & mask;

    return xorAsidHashIntoIndex(pcBits ^ foldedBits, tableIndexBits[t], asidHash);
}

Addr
MicroTAGE::getTageIndex(Addr pc, int t, uint8_t asidHash)
{
    return getTageIndex(pc, t, historyState(0).indexFoldedHist[t].get(), asidHash);
}

bool
MicroTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
MicroTAGE::satDecrement(int min, short &counter)
{
    if (counter > min) {
        --counter;
    }
    return counter == min;
}

bool
MicroTAGE::isBranchInPredictionBlock(Addr branchPC, Addr startPC) const
{
    if (blockSize == 0) {
        return false;
    }

    Addr alignedPC = startPC & ~(blockSize - 1);
    if (branchPC < alignedPC) {
        return false;
    }

    Addr offset = (branchPC - alignedPC) >> instShiftAmt;
    return offset < maxBranchPositions;
}

unsigned
MicroTAGE::getBranchIndexInBlock(Addr branchPC, Addr startPC) {
    // Calculate branch position within the fetch block (0 .. maxBranchPositions-1)
    const Addr alignedPC = startPC & ~(blockSize - 1);

    unsigned position = 0;
    if (branchPC >= alignedPC) {
        const Addr byteOffset = branchPC - alignedPC;
        position = byteOffset >> instShiftAmt;
    } else {
        warn_once("MicroTAGE: branch %#lx precedes block start %#lx; treating as offset 0",
                  branchPC, startPC);
    }

    if (position >= maxBranchPositions) {
        warn_once("MicroTAGE: branch %#lx exceeds block [%#lx, %#lx) (blockSize=%lu, instShift=%u, maxPositions=%u); clamping index",
                  branchPC, alignedPC,
                  alignedPC + blockSize,
                  static_cast<unsigned long>(blockSize), instShiftAmt, maxBranchPositions);
        position %= maxBranchPositions;
    }

    return position;
}

unsigned
MicroTAGE::getBankId(Addr pc) const
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
MicroTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, bool taken,
                        Addr pc, Addr target, ThreadID tid)
{
    auto &state = historyState(tid);
    if (debug::TAGEHistory) {   // if debug flag is off, do not use to_string since it's too slow
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(TAGEHistory, "in doUpdateHist, taken %d, pc %#lx, history %s\n", taken, pc, buf.c_str());
    }

    if (!state.aheadIndexFoldedHist.empty()) {
        state.indexFoldedHist = state.aheadIndexFoldedHist.front();
    }

    if (!taken) {
        if (debug::TAGEHistory && !state.aheadIndexFoldedHist.empty()) {
            bool mismatch = false;
            for (int t = 0; t < numPredictors; t++) {
                if (state.indexFoldedHist[t].get() !=
                    state.aheadIndexFoldedHist.front()[t].get()) {
                    mismatch = true;
                    break;
                }
            }
            if (mismatch) {
                DPRINTF(TAGEHistory,
                        "doUpdateHist: not taken, indexFoldedHist stale vs ahead queue\n");
            }
        }
        DPRINTF(TAGEHistory, "not updating folded history, since FB not taken\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        // Update tag folded history immediately so tag calculation always sees current history.
        state.tagFoldedHist[t].update(history, 2, taken, pc, target);
        state.altTagFoldedHist[t].update(history, 2, taken, pc, target);
        DPRINTF(TAGEHistory, "t: %d, tag 0x%lx, altTag 0x%lx\n",
                t, state.tagFoldedHist[t].get(),
                state.altTagFoldedHist[t].get());
    }

    // Prepare next-cycle index folded history and delay its visibility by one cycle.
    auto nextIndexFoldedHist = state.indexFoldedHist;
    for (int t = 0; t < numPredictors; t++) {
        nextIndexFoldedHist[t].update(history, 2, taken, pc, target);
        DPRINTF(TAGEHistory, "t: %d, index foldedHist(next) _folded 0x%lx\n",
                t, nextIndexFoldedHist[t].get());
    }
    state.aheadIndexFoldedHist.push(nextIndexFoldedHist);
    if (state.aheadIndexFoldedHist.size() > 1) {
        state.aheadIndexFoldedHist.pop();
    }
}

/**
 * @brief Speculatively updates path folded histories.
 */
void
MicroTAGE::specUpdatePHist(const boost::dynamic_bitset<> &history,
                           FullBTBPrediction &pred,
                           const PathHistoryUpdate &update)
{
    doUpdateHist(history, update.taken, update.pc, update.target, pred.tid);
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
MicroTAGE::recoverPHist(const boost::dynamic_bitset<> &history,
    const FetchTarget &entry, const PathHistoryUpdate &update)
{
    auto &state = historyState(entry.tid);
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    if (!predMeta) {
        DPRINTF(UTAGE, "recoverPHist: no prediction metadata, cannot recover\n");
        return;
    }
    // Restore current folded index history exactly to prediction-time state.
    for (int i = 0; i < numPredictors; i++) {
        state.indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }

    // Restore delayed index folded history slot exactly to prediction-time state.
    while (!state.aheadIndexFoldedHist.empty()) {
        state.aheadIndexFoldedHist.pop();
    }
    if (predMeta->aheadIndexFoldedHistValid) {
        assert(predMeta->aheadIndexFoldedHist.size() == numPredictors);
        state.aheadIndexFoldedHist.push(predMeta->aheadIndexFoldedHist);
    }

    if (debug::TAGEHistory) {
        bool queue_valid_mismatch =
            (predMeta->aheadIndexFoldedHistValid !=
             !state.aheadIndexFoldedHist.empty());
        if (queue_valid_mismatch) {
            DPRINTF(TAGEHistory,
                    "recoverPHist: ahead queue valid mismatch after restore, path_taken %d\n",
                    update.taken);
        }
    }

    for (int i = 0; i < numPredictors; i++) {
        state.altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        state.tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
    }
    doUpdateHist(history, update.taken, update.pc, update.target, entry.tid);
}

// Check folded history after speculative update and recovery
void
MicroTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    checkFoldedHist(hist, 0, when);
}

void
MicroTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, ThreadID tid,
                           const char * when)
{
    auto &state = historyState(tid);
    DPRINTF(UTAGE, "checking folded history when %s\n", when);
    if (debug::TAGEHistory) {
        std::string hist_str;
        boost::to_string(hist, hist_str);
        DPRINTF(TAGEHistory, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < numPredictors; t++) {
        // indexFoldedHist is intentionally delayed by one cycle via
        // aheadindexFoldedHist in doUpdateHist(). During consistency checks
        // right after speculative/recovery updates, compare against the staged
        // next-cycle value when available.
        if (!state.aheadIndexFoldedHist.empty()) {
            state.aheadIndexFoldedHist.front()[t].check(hist);
        } else {
            state.indexFoldedHist[t].check(hist);
        }
        state.tagFoldedHist[t].check(hist);
        state.altTagFoldedHist[t].check(hist);
    }
}

#ifndef UNIT_TEST
// Constructor for TAGE statistics
MicroTAGE::TageStats::TageStats(statistics::Group* parent, int numPredictors, int numBanks):
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
    ADD_STAT(updateUseAltOnNaCorrect, statistics::units::Count::get(), "use alt on na correct when update"),
    ADD_STAT(updateUseAltOnNaWrong, statistics::units::Count::get(), "use alt on na wrong when update"),
    ADD_STAT(updateAllocFailure, statistics::units::Count::get(), "alloc failure when update"),
    ADD_STAT(updateAllocFailureNoValidTable, statistics::units::Count::get(), "alloc failure no valid table when update"),
    ADD_STAT(updateAllocSuccess, statistics::units::Count::get(), "alloc success when update"),
    ADD_STAT(updateMispred, statistics::units::Count::get(), "mispred when update"),
    ADD_STAT(updateResetU, statistics::units::Count::get(), "reset u when update"),

    ADD_STAT(updateUtageHit, statistics::units::Count::get(), "number of updates where utage provided the main prediction"),
    ADD_STAT(updateUtageHitWrong, statistics::units::Count::get(), "number of updates where utage prediction was wrong"),

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
MicroTAGE::TageStats::updateStatsWithTagePrediction(const TagePrediction &pred, bool when_pred)
{
    bool hit = pred.mainInfo.found;
    unsigned hit_table = pred.mainInfo.table;
    bool useAlt = pred.mainprovided ? false : true;
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

#ifndef UNIT_TEST
void
MicroTAGE::recordCommittedBranchStats(
    const ResolvedBranch &branch,
    const std::shared_ptr<void> &prediction_meta)
{
    if (!branch.isCond) {
        // tage only deals with conditional branches
        return;
    }
    auto meta = std::static_pointer_cast<TageMeta>(prediction_meta);
    if (!meta) {
        DPRINTF(UTAGE, "commitBranch: no prediction meta, skip\n");
        return;
    }
    auto pc = branch.pc;
    auto it = meta->preds.find(pc);
    bool pred_taken = false;
    bool pred_hit = false;
    if (it != meta->preds.end()) {
        pred_taken = it->second.taken;
        pred_hit = true;
    }
    bool this_cond_taken = branch.taken;
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
