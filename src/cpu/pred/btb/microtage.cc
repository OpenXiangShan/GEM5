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
      usingS3Pred(false),
      numBanks(numBanks),
      bankIdWidth(ceilLog2(numBanks)),
      bankBaseShift(instShiftAmt),
      indexShift(bankBaseShift + ceilLog2(numBanks)),
      enableBankConflict(false),
      lastPredBankId(0),
      predBankValid(false),
      tageStats()
{
    setNumDelay(0);

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
MicroTAGE::MicroTAGE(const Params& p)
    : TimedBaseBTBPredictor(p),
      numPredictors(p.numPredictors),
      tableSizes(p.tableSizes),
      tableTagBits(p.TTagBitSizes),
      tablePcShifts(p.TTagPcShifts),
      histLengths(p.histLengths),
      maxHistLen(p.maxHistLen),
      numWays(p.numWays),
      maxBranchPositions(p.maxBranchPositions),
      updateOnRead(p.updateOnRead),
      usingS3Pred(p.usingS3Pred),
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
                (int)histLengths[i],
                (int)partitionIndexBits(tableIndexBits[i]), 16);
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
 * @brief Generate prediction for a single BTB entry by searching TAGE tables
 *
 * @param btb_entry The BTB entry to generate prediction for
 * @param startPC The starting PC address for calculating indices and tags
 * @param predMeta Optional prediction metadata; if provided, use snapshot for index/tag
 *             calculation (update path); if nullptr, use current folded history (prediction path)
 * @return TagePrediction containing main and alternative predictions
 */
MicroTAGE::TagePrediction
MicroTAGE::generateSinglePrediction(const BTBEntry &btb_entry,
                                 const Addr &startPC,
                                 const std::shared_ptr<TageMeta>& predMeta,
                                 ThreadID tid,
                                 uint8_t asidHash) {
    DPRINTF(UTAGE, "generateSinglePrediction for btbEntry: %#lx\n", btb_entry.pc);
    const auto &state = historyState(tid);

    bool provided = false;
    TageTableInfo main_info;

    // Search from highest to lowest table for matches
    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(btb_entry.pc, startPC);

    for (int i = numPredictors - 1; i >= 0; --i) {
        // Calculate index and tag: use snapshot if provided, otherwise use current folded history
        // Tag includes position XOR (like RTL: tag = tempTag ^ cfiPosition)
        Addr index = predMeta ? getTageIndex(
            startPC, i, predMeta->indexFoldedHist[i], asidHash, tid)
                              : getTageIndex(
            startPC, i, state.indexFoldedHist[i].get(), asidHash, tid);
        Addr tag = predMeta ? getTageTag(startPC, i,
                            predMeta->tagFoldedHist[i],predMeta->altTagFoldedHist[i],
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

                DPRINTF(UTAGE, "hit  table %d[%lu][%u]: valid %d, tag %lu, ctr %d, useful %d, btb_pc %#lx, pos %u\n",
                    i, index, way, entry.valid, entry.tag, entry.counter, entry.useful, btb_entry.pc, position);
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
            DPRINTF(UTAGE, "miss table %d[%lu] for tag %lu (with pos %u), btb_pc %#lx\n",
                i, index, tag, position, btb_entry.pc);
        }
    }

    // Generate final prediction
    bool main_taken = main_info.taken();
    bool base_pred = btb_entry.ctr >= 0;

    bool taken = provided ? main_taken : base_pred;

    DPRINTF(UTAGE, "tage predict %#lx taken %d\n", btb_entry.pc, taken);
    DPRINTF(UTAGE, "tage main provided %d ? main_taken %d : base_taken %d\n",
            provided, main_taken, base_pred);

    return TagePrediction(btb_entry.pc, main_info, provided, taken, base_pred);
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
            auto pred = generateSinglePrediction(btb_entry, startPC, nullptr,
                                                 tid, asidHash);
            threadMeta[tid]->preds[btb_entry.pc] = pred;
            tageStats.updateStatsWithTagePrediction(pred, true);
            results.push_back({btb_entry.pc, pred.taken || btb_entry.alwaysTaken});
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

    // Clear old prediction metadata and save current history state. Non-ahead
    // folded histories are stored as values (see TageMeta); recovery restores
    // them via recoverValue().
    threadMeta[tid] = std::make_shared<TageMeta>();
    auto snapshotFoldedValues = [](const auto &src, std::vector<uint64_t> &dst) {
        dst.resize(src.size());
        for (size_t i = 0; i < src.size(); ++i) {
            dst[i] = src[i].get();
        }
    };
    snapshotFoldedValues(state.tagFoldedHist, threadMeta[tid]->tagFoldedHist);
    snapshotFoldedValues(state.altTagFoldedHist, threadMeta[tid]->altTagFoldedHist);
    snapshotFoldedValues(state.indexFoldedHist, threadMeta[tid]->indexFoldedHist);
    threadMeta[tid]->aheadIndexFoldedHistValid =
        !state.aheadIndexFoldedHist.empty();
    if (threadMeta[tid]->aheadIndexFoldedHistValid) {
        threadMeta[tid]->aheadIndexFoldedHist =
            state.aheadIndexFoldedHist.front();
    } else {
        threadMeta[tid]->aheadIndexFoldedHist.clear();
    }
    threadMeta[tid]->history = history;

    if (getDelay() < stagePreds.size()) {
        threadMeta[tid]->abtbEntries =
            getAbtbConditionalEntries(stagePreds[getDelay()].btbEntries);
    }

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        // TODO: only lookup once for one btb entry in different stages
        auto &stage_pred = stagePreds[s];
        auto abtb_entries = getAbtbConditionalEntries(stage_pred.btbEntries);
        if (abtb_entries.empty()) {
            continue;
        }
        for (const auto &entry : abtb_entries) {
            stage_pred.condTakens.erase(
                std::remove_if(stage_pred.condTakens.begin(),
                               stage_pred.condTakens.end(),
                               [&entry](const auto &taken) {
                                   return taken.first == entry.pc;
                               }),
                stage_pred.condTakens.end());
        }
        lookupHelper(startPC, abtb_entries, stage_pred.condTakens,
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
std::vector<BTBEntry>
MicroTAGE::prepareUpdateEntries(const FetchTarget &stream) {
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

std::vector<BTBEntry>
MicroTAGE::prepareS3UpdateEntries(const FullBTBPrediction &s3Pred)
{
    std::vector<BTBEntry> entries;
    for (const auto &entry : s3Pred.btbEntries) {
        if (!entry.valid) {
            continue;
        }

        if (!entry.isCond) {
            if (entry.isDirect || entry.isIndirect || entry.isReturn || entry.isCall ||
                entry.isUncond()) {
                break;
            }
            continue;
        }

        Addr branch_pc = entry.pc;
        auto teacher_it = CondTakens_find(s3Pred.condTakens, branch_pc);
        // Stop at the first control transfer the S3 teacher says is taken.
        bool teacher_taken = entry.alwaysTaken ||
            (teacher_it != s3Pred.condTakens.end() && teacher_it->second);

        if (!entry.alwaysTaken) {
            entries.push_back(entry);
        }

        if (teacher_taken) {
            break;
        }
    }
    return entries;
}

bool
MicroTAGE::isAbtbEntry(const BTBEntry &entry) const
{
#ifdef UNIT_TEST
    if (abtbComponentIdx < 0) {
        return true;
    }
#endif
    return abtbComponentIdx >= 0 && entry.source == abtbComponentIdx;
}

std::vector<BTBEntry>
MicroTAGE::getAbtbConditionalEntries(const std::vector<BTBEntry> &btbEntries) const
{
    std::vector<BTBEntry> entries;
    for (const auto &entry : btbEntries) {
        if (entry.valid && entry.isCond && isAbtbEntry(entry)) {
            entries.push_back(entry);
        }
    }
    return entries;
}

std::vector<BTBEntry>
MicroTAGE::prepareS3UpdateEntriesFromAbtbMeta(
    const std::vector<BTBEntry> &abtbEntries,
    FullBTBPrediction &s3Pred,
    CondTakens &teacherCondTakens)
{
    std::vector<BTBEntry> entries;
    auto taken_entry = s3Pred.getTakenEntry();

    for (const auto &entry : abtbEntries) {
        if (!entry.valid || !entry.isCond) {
            continue;
        }

        if (taken_entry.valid && entry.pc > taken_entry.pc) {
            break;
        }

        const bool actual_taken =
            taken_entry.valid && taken_entry.isCond &&
            entry.pc == taken_entry.pc;
        entries.push_back(entry);
        teacherCondTakens.push_back({entry.pc, actual_taken});

        if (taken_entry.valid && entry.pc == taken_entry.pc) {
            break;
        }
    }

    return entries;
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
MicroTAGE::updatePredictorStateAndCheckAllocation(const BTBEntry &entry,
                             bool actual_taken,
                             const TagePrediction &pred,
                             const FetchTarget &stream) {
    tageStats.updateStatsWithTagePrediction(pred, false);

    auto &main_info = pred.mainInfo;
    bool used_base = !pred.mainprovided;
    // Use base table instead of entry.ctr for fallback prediction
    bool base_taken = entry.ctr >= 0;

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
        bool base_is_correct_and_strong =
                                     (base_taken == actual_taken) &&
                                     (abs(2 * entry.ctr + 1) == 5);

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
    bool this_fb_mispred = stream.squashType == SquashType::SQUASH_CTRL &&
                               stream.squashPC == entry.pc;
    // No allocation if no misprediction
    if (!this_fb_mispred) {
        return false;
    }

    // All other cases: allocate longer history table
    return true;
}

bool
MicroTAGE::updatePredictorStateAndCheckAllocationS3(const BTBEntry &entry,
                             bool actual_taken,
                             const TagePrediction &pred) {
    // Mirror the normal update path, but interpret mismatch against the S3 teacher
    // instead of a resolved squash/commit outcome.
    auto &main_info = pred.mainInfo;
    bool used_base = !pred.mainprovided;
    if (!main_info.found) {
        tageStats.s3UpdateNoHitUseBim++;
    }
    if (!main_info.found || used_base) {
        tageStats.s3UpdateUseAlt++;
    }
    bool base_taken = entry.ctr >= 0;

    if (main_info.found) {
        bool main_weak = (main_info.entry.counter == 0 || main_info.entry.counter == -1);
        if (main_weak) {
            tageStats.s3UpdateProviderNa++;
            bool base_correct = (base_taken == actual_taken);
            tageStats.s3UpdateUseAltOnNaUpdated++;
            if (base_correct) {
                tageStats.s3UpdateUseAltOnNaCorrect++;
            } else {
                tageStats.s3UpdateUseAltOnNaWrong++;
            }
        }
    }

    if (main_info.found) {
        DPRINTF(UTAGE, "S3 teacher-update provided by table %d, idx %lu, way %u\n",
            main_info.table, main_info.index, main_info.way);

        auto &way = tageTable[main_info.table][main_info.index][main_info.way];
        updateCounter(actual_taken, 3, way.counter);

        bool main_is_correct = main_info.taken() == actual_taken;
        bool base_is_correct_and_strong =
                                     (base_taken == actual_taken) &&
                                     (abs(2 * entry.ctr + 1) == 5);

        if (base_is_correct_and_strong && main_is_correct) {
            way.useful = 0;
            DPRINTF(TAGEUseful, "useful bit reset to 0 due to humility rule\n");
        } else if (main_info.taken() != base_taken) {
            if (main_is_correct) {
                way.useful = 1;
            }
        }

        if (way.counter == 0 || way.counter == -1) {
            way.useful = 0;
            DPRINTF(TAGEUseful, "useful bit reset to 0 due to weak counter\n");
        }
        DPRINTF(UTAGE, "useful bit is now %d\n", way.useful);

        if (!main_is_correct) {
            tageStats.s3UpdateUtageHitWrong++;
        }
    }

    if (used_base) {
        bool base_correct = base_taken == actual_taken;
        if (base_correct) {
            tageStats.s3UpdateUseAltCorrect++;
        } else {
            tageStats.s3UpdateUseAltWrong++;
        }
        if (main_info.found && main_info.taken() != base_taken) {
            tageStats.s3UpdateAltDiffers++;
        }
    }

    bool teacher_mismatch = pred.taken != actual_taken;
    if (!teacher_mismatch) {
        return false;
    }

    tageStats.s3UpdateMispred++;

    if (main_info.found && main_info.table == numPredictors - 1) {
        return false;
    }

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
MicroTAGE::handleNewEntryAllocation(const Addr &startPC,
                                 const BTBEntry &entry,
                                 bool actual_taken,
                                 unsigned start_table,
                                 const std::shared_ptr<TageMeta>& meta,
                                 uint8_t asidHash,
                                 TrainingMode mode,
                                 uint64_t &allocated_table,
                                 uint64_t &allocated_index,
                                 uint64_t &allocated_way,
                                 ThreadID tid) {
    int &resetCnt = usesTidPartitionedStorage() ?
        usefulResetCntByThread[tid] : usefulResetCnt;
    // Simple set-associative allocation (no LFSR, no per-way table gating):
    // - For each table from start_table upward, check the set at computed index.
    // - Prefer invalid ways; else choose any way with useful==0 and weak counter.
    // - If none, apply a one-step age penalty to a strong, not-useful way (no allocation).
    const bool isS3Update = mode == TrainingMode::S3Update;
    auto count_alloc_success = [&]() {
        if (isS3Update) {
            tageStats.s3UpdateAllocSuccess++;
        } else {
            tageStats.updateAllocSuccess++;
        }
    };
    auto count_alloc_failure = [&]() {
        if (isS3Update) {
            tageStats.s3UpdateAllocFailure++;
        } else {
            tageStats.updateAllocFailure++;
        }
    };
    auto count_reset_u = [&]() {
        if (isS3Update) {
            tageStats.s3UpdateResetU++;
        } else {
            tageStats.updateResetU++;
        }
    };
    auto count_no_valid_table = [&]() {
        if (isS3Update) {
            tageStats.s3UpdateAllocFailureNoValidTable++;
        } else {
            tageStats.updateAllocFailureNoValidTable++;
        }
    };

    // Calculate branch position within the block (like RTL's cfiPosition)
    unsigned position = getBranchIndexInBlock(entry.pc, startPC);

    for (unsigned ti = start_table; ti < numPredictors; ++ti) {
        Addr newIndex = getTageIndex(
            startPC, ti, meta->indexFoldedHist[ti], asidHash, tid);
        Addr newTag = getTageTag(startPC, ti,
            meta->tagFoldedHist[ti], meta->altTagFoldedHist[ti],
            position, asidHash);

        auto &set = tageTable[ti][newIndex];

        // Allocate into invalid way or not-useful and weak way
        for (unsigned way = 0; way < numWays; ++way) {
            auto &cand = set[way];
            const bool weakish = std::abs(cand.counter * 2 + 1) <= 3; // -3,-2,-1,0,1,2
            if (!cand.valid || (!cand.useful && weakish)) {
                short newCounter = actual_taken ? 0 : -1;
                DPRINTF(UTAGE, "allocating entry in table %d[%lu][%u], tag %lu (with pos %u), counter %d, pc %#lx\n",
                        ti, newIndex, way, newTag, position, newCounter, entry.pc);
                cand = TageEntry(newTag, newCounter, entry.pc); // u = 0 default
                count_alloc_success();
                allocated_table = ti;
                allocated_index = newIndex;
                allocated_way = way;
                resetCnt = resetCnt <= 0 ? 0 : resetCnt - 1;
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

        count_alloc_failure();
        resetCnt++;
    }

    if (resetCnt >= 256) {
        resetCnt = 0;
        count_reset_u();
        DPRINTF(UTAGE, "reset useful bit of all entries\n");
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

    DPRINTF(UTAGE, "no eligible way found for allocation starting from table %d\n", start_table);
    count_no_valid_table();
    return false;
}

/**
 * @brief Probe whether the resolved-update path may proceed this cycle.
 *
 * In S3 teacher-update mode, MicroTAGE no longer participates in resolved
 * update backpressure and always lets the caller proceed.
 */
bool
MicroTAGE::canResolveUpdate(const FetchTarget &stream) {
    if (usingS3Pred) {
        return true;
    }

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
        DPRINTF(UTAGE, "Bank conflict detected: update bank %u conflicts with prediction bank %u, "
                      "deferring this update (will retry after blocking prediction)\n",
                      updateBank, lastPredBankId);
        predBankValid = false;
        return false;
    }

    return true;
}

/**
 * @brief Perform resolved update after probe success.
 *
 * In S3 teacher-update mode this callback becomes a no-op because functional
 * predictor state is updated by updateUsingS3Pred().
 */
void
MicroTAGE::doResolveUpdate(const FetchTarget &stream) {
    if (usingS3Pred) {
        return;
    }
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
MicroTAGE::update(const FetchTarget &stream) {
    if (usingS3Pred) {
        DPRINTF(UTAGE, "update bypassed because usingS3Pred is enabled\n");
        return;
    }

    Addr startAddr = stream.getRealStartPC();
    unsigned updateBank = getBankId(startAddr);

    DPRINTF(UTAGE, "update startAddr: %#lx, bank: %u\n", startAddr, updateBank);

    // ========== Normal Update Logic ==========
    // Prepare BTB entries to update
    auto entries_to_update = prepareUpdateEntries(stream);

    // Get prediction metadata snapshot and bind to member for helpers
    auto predMeta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    if (!predMeta) {
        DPRINTF(UTAGE, "update: no prediction meta, skip\n");
        return;
    }

    trainEntries(entries_to_update, predMeta, startAddr, stream.tid, stream.asidHash,
                 TrainingMode::Resolved, &stream, nullptr);
    checkUtageUpdateMisspred(stream);
    DPRINTF(UTAGE, "end update\n");
}

void
MicroTAGE::updateUsingS3Pred(FullBTBPrediction &s3Pred)
{
    if (!usingS3Pred) {
        return;
    }

    const ThreadID tid = s3Pred.tid;
    if (tid >= threadMeta.size()) {
        DPRINTF(UTAGE, "S3 teacher-update: invalid tid %u\n", tid);
        return;
    }

    auto predMeta = threadMeta[tid];
    if (!predMeta) {
        DPRINTF(UTAGE, "S3 teacher-update: no prediction meta for tid %u, skip\n", tid);
        tageStats.s3UpdateNoMeta++;
        return;
    }

    const Addr startAddr = s3Pred.bbStart;
    // Only train the conditional prefix that remains reachable under the
    // final-stage teacher prediction for this fetch block.
    CondTakens teacher_cond_takens;
    auto entries_to_update = prepareS3UpdateEntriesFromAbtbMeta(
        predMeta->abtbEntries, s3Pred, teacher_cond_takens);
    trainEntries(entries_to_update, predMeta, startAddr, tid, s3Pred.asidHash,
                 TrainingMode::S3Update, nullptr, &teacher_cond_takens);
}

void
MicroTAGE::trainEntries(const std::vector<BTBEntry> &entries_to_update,
                        const std::shared_ptr<TageMeta> &predMeta,
                        const Addr &startPC,
                        ThreadID tid,
                        uint8_t asidHash,
                        TrainingMode mode,
                        const FetchTarget *stream,
                        const CondTakens *teacherCondTakens)
{
    const bool isS3Update = mode == TrainingMode::S3Update;
    bool utage_hit = false;
    const char *context = isS3Update ? "S3 teacher-update" : "update";
    auto get_prediction_for_training =
        [&](const BTBEntry &btb_entry) -> TagePrediction {
            if (updateOnRead) {
                return generateSinglePrediction(
                    btb_entry, startPC, predMeta, tid, asidHash);
            }

            auto pred_it = predMeta->preds.find(btb_entry.pc);
            if (pred_it != predMeta->preds.end()) {
                return pred_it->second;
            }

            DPRINTF(UTAGE,
                    "%s: missing predMeta entry for pc %#lx, recompute with snapshot\n",
                    context, btb_entry.pc);
            return generateSinglePrediction(
                btb_entry, startPC, predMeta, tid, asidHash);
        };

    for (const auto &btb_entry : entries_to_update) {
        if (isS3Update) {
            tageStats.s3UpdateEntries++;
        }

        bool actual_taken = false;
        if (isS3Update) {
            assert(teacherCondTakens != nullptr);
            const auto &teacher_cond_takens = *teacherCondTakens;
            Addr branch_pc = btb_entry.pc;
            auto teacher_it = CondTakens_find(teacher_cond_takens, branch_pc);
            if (teacher_it != teacher_cond_takens.end()) {
                actual_taken = teacher_it->second;
            }
        } else {
            assert(stream != nullptr);
            actual_taken = stream->exeTaken && stream->exeBranchInfo == btb_entry;
        }

        auto recomputed = get_prediction_for_training(btb_entry);

        if (recomputed.mainprovided) {
            utage_hit = true;
        }

        bool need_allocate = isS3Update
            ? updatePredictorStateAndCheckAllocationS3(btb_entry, actual_taken, recomputed)
            : updatePredictorStateAndCheckAllocation(btb_entry, actual_taken, recomputed, *stream);

        if (!need_allocate) {
            continue;
        }

        uint64_t allocated_table = 0;
        uint64_t allocated_index = 0;
        uint64_t allocated_way = 0;
        unsigned start_table = 0;
        auto &main_info = recomputed.mainInfo;
        if (main_info.found) {
            start_table = main_info.table + 1;
        }

        handleNewEntryAllocation(startPC, btb_entry, actual_taken,
                                 start_table, predMeta, asidHash, mode,
                                 allocated_table, allocated_index,
                                 allocated_way, tid);

#ifndef UNIT_TEST
        // Optional per-entry miss tracing is only kept for the resolved update path.
        // The S3 teacher-update path shares the same provider/allocation mechanics,
        // but does not currently feed the miss-trace database.
        // if (!isS3Update && enableDB) {
        //     TageMissTrace t;
        //     std::string history_str;
        //     boost::dynamic_bitset<> history_low50 = predMeta->history;
        //     if (history_low50.size() > 50) {
        //         history_low50.resize(50);  // get the lower 50 bits of history
        //     }
        //     boost::to_string(history_low50, history_str);
        //     auto main_info = recomputed.mainInfo;
        //     t.set(startPC, btb_entry.pc, main_info.way,
        //         main_info.found, main_info.entry.counter, main_info.entry.useful,
        //         main_info.table, main_info.index,
        //         recomputed.useAlt, recomputed.taken, actual_taken, alloc_success,
        //         allocated_table, allocated_index, allocated_way,
        //         history_str, predMeta->indexFoldedHist[main_info.table].get());
        //     tageMissTrace->write_record(t);
        // }
#endif
    }

    if (utage_hit) {
        if (isS3Update) {
            tageStats.s3UpdateUtageHit++;
        } else {
            tageStats.updateUtageHit++;
        }
    }
}

void
MicroTAGE::checkUtageUpdateMisspred(const FetchTarget &stream) {
    auto predMeta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    if (!predMeta) {
        DPRINTF(UTAGE, "checkUtageUpdateMisspred: no prediction meta, skip\n");
        return;
    }

    // used for MicroTAGE update misprediction counting
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
    bool has_taken_pred = false;
    Addr first_taken_pc = 0;
    for (auto &entry_info : lastPreds) {
        if (entry_info.second.taken) {
            has_taken_pred = true;
            first_taken_pc = entry_info.first;
            break;
        }
    }
    bool fallthrough_mispred = (!has_taken_pred && stream.exeTaken) ||
                                (has_taken_pred && !stream.exeTaken);
    bool branch_mispred = stream.exeTaken && has_taken_pred &&
                          first_taken_pc != stream.exeBranchInfo.pc;
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
MicroTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist,
                        uint8_t asidHash, ThreadID tid)
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
MicroTAGE::getTageIndex(Addr pc, int t, uint8_t asidHash, ThreadID tid)
{
    return getTageIndex(pc, t, historyState(tid).indexFoldedHist[t].get(),
                        asidHash, tid);
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
        state.indexFoldedHist[i].recoverValue(predMeta->indexFoldedHist[i]);
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
        state.altTagFoldedHist[i].recoverValue(predMeta->altTagFoldedHist[i]);
        state.tagFoldedHist[i].recoverValue(predMeta->tagFoldedHist[i]);
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

    ADD_STAT(s3UpdateEntries, statistics::units::Count::get(),
             "number of conditional entries trained by S3 teacher update"),
    ADD_STAT(s3UpdateNoMeta, statistics::units::Count::get(),
             "number of S3 teacher updates skipped due to missing prediction metadata"),
    ADD_STAT(s3UpdateNoHitUseBim, statistics::units::Count::get(),
             "use bimodal when no hit on S3 teacher update"),
    ADD_STAT(s3UpdateUseAlt, statistics::units::Count::get(),
             "use alt on S3 teacher update"),
    ADD_STAT(s3UpdateUseAltCorrect, statistics::units::Count::get(),
             "use alt on S3 teacher update and correct"),
    ADD_STAT(s3UpdateUseAltWrong, statistics::units::Count::get(),
             "use alt on S3 teacher update and wrong"),
    ADD_STAT(s3UpdateAltDiffers, statistics::units::Count::get(),
             "alt differs on S3 teacher update"),
    ADD_STAT(s3UpdateUseAltOnNaUpdated, statistics::units::Count::get(),
             "use alt on na ctr updated during S3 teacher update"),
    ADD_STAT(s3UpdateProviderNa, statistics::units::Count::get(),
             "provider weak during S3 teacher update"),
    ADD_STAT(s3UpdateUseAltOnNaCorrect, statistics::units::Count::get(),
             "use alt on na correct during S3 teacher update"),
    ADD_STAT(s3UpdateUseAltOnNaWrong, statistics::units::Count::get(),
             "use alt on na wrong during S3 teacher update"),
    ADD_STAT(s3UpdateAllocFailure, statistics::units::Count::get(),
             "alloc failure during S3 teacher update"),
    ADD_STAT(s3UpdateAllocFailureNoValidTable, statistics::units::Count::get(),
             "alloc failure with no valid table during S3 teacher update"),
    ADD_STAT(s3UpdateAllocSuccess, statistics::units::Count::get(),
             "alloc success during S3 teacher update"),
    ADD_STAT(s3UpdateMispred, statistics::units::Count::get(),
             "teacher mismatch during S3 teacher update"),
    ADD_STAT(s3UpdateResetU, statistics::units::Count::get(),
             "reset u during S3 teacher update"),
    ADD_STAT(s3UpdateUtageHit, statistics::units::Count::get(),
             "number of S3 teacher updates where utage provided the main prediction"),
    ADD_STAT(s3UpdateUtageHitWrong, statistics::units::Count::get(),
             "number of S3 teacher updates where utage prediction disagreed with the S3 teacher"),

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
MicroTAGE::commitBranch(const FetchTarget &stream, const DynInstPtr &inst)
{
    if (!inst->isCondCtrl()) {
        // tage only deals with conditional branches
        return;
    }
    auto meta = std::static_pointer_cast<TageMeta>(stream.predMetas[getComponentIdx()]);
    if (!meta) {
        DPRINTF(UTAGE, "commitBranch: no prediction meta, skip\n");
        return;
    }
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
