#include "cpu/pred/btb/btb_ittage.hh"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "base/debug_helper.hh"
#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPUseful.hh"
#include "debug/ITTAGE.hh"
#include "debug/ITTAGEHistory.hh"

namespace gem5 {

namespace branch_prediction {

namespace btb_pred{

BTBITTAGE::BTBITTAGE(const Params& p):
TimedBaseBTBPredictor(p),
numPredictors(p.numPredictors),
tableSizes(p.tableSizes),
tableTagBits(p.TTagBitSizes),
tablePcShifts(p.TTagPcShifts),
histLengths(p.histLengths),
maxHistLen(p.maxHistLen),
numTablesToAlloc(p.numTablesToAlloc),
ittageStats(this, p.numPredictors)
{
    DPRINTF(ITTAGE, "BTBITTAGE constructor numBr=%d\n", numBr);
    tageTable.resize(numPredictors);
    tableIndexBits.resize(numPredictors);
    tableIndexMasks.resize(numPredictors);
    tableTagBits.resize(numPredictors);
    tableTagMasks.resize(numPredictors);
    threadHistory.resize(MaxThreads);
    threadMeta.resize(MaxThreads);
    for (unsigned int i = 0; i < p.numPredictors; ++i) {
        //initialize ittage predictor
        assert(tableSizes.size() >= numPredictors);
        tageTable[i].resize(tableSizes[i]);

        tableIndexBits[i] = ceilLog2(tableSizes[i]);
        tableIndexMasks[i].resize(tableIndexBits[i], true);

        assert(histLengths.size() >= numPredictors);

        assert(tableTagBits.size() >= numPredictors);
        tableTagMasks[i].resize(tableTagBits[i], true);

        assert(tablePcShifts.size() >= numPredictors);

        for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
            auto &state = threadHistory[tid];
            state.tagFoldedHist.emplace_back(
                (int)histLengths[i], (int)tableTagBits[i], (int)16);
            state.altTagFoldedHist.emplace_back(
                (int)histLengths[i], (int)tableTagBits[i] - 1, (int)16);
            state.indexFoldedHist.emplace_back(
                (int)histLengths[i], (int)tableIndexBits[i], (int)16);
        }
    }
    // useAlt.resize(128);
    // for (unsigned i = 0; i < useAlt.size(); ++i) {
    //     useAlt[i].resize(1, 0);
    // }
    usefulResetCnt = 0;
}

ThreadID
BTBITTAGE::predictorTid(const std::vector<FullBTBPrediction> &stagePreds) const
{
    assert(!stagePreds.empty());
    return stagePreds.front().tid;
}

BTBITTAGE::ThreadHistoryState &
BTBITTAGE::historyState(ThreadID tid)
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

const BTBITTAGE::ThreadHistoryState &
BTBITTAGE::historyState(ThreadID tid) const
{
    assert(tid < threadHistory.size());
    return threadHistory[tid];
}

void
BTBITTAGE::tickStart()
{
}

void
BTBITTAGE::tick() {}

void
BTBITTAGE::lookupHelper(Addr startAddr, const std::vector<BTBEntry> &btbEntries,
                        IndirectTargets& results, ThreadID tid, uint8_t asidHash)
{
    (void)asidHash;
    DPRINTF(ITTAGE, "lookupHelper startAddr: %#lx\n", startAddr);
    std::vector<TagePrediction> preds;
    for (auto &btb_entry : btbEntries) {
        if (btb_entry.isIndirect && !btb_entry.isReturn && btb_entry.valid) {
            DPRINTF(ITTAGE, "lookupHelper btbEntry: %#lx\n", btb_entry.pc);
            bool provided = false;
            bool alt_provided = false;

            TageTableInfo main_info, alt_info;

            for (int i = numPredictors - 1; i >= 0; --i) {
                auto &way = lookupEntries[i];
                // TODO: count alias hit (offset match but pc differs)
                bool match = way.valid && lookupTags[i] == way.tag && btb_entry.pc == way.pc;
                DPRINTF(ITTAGE, "hit %d, table %d, index %d, lookup tag %d, tag %d, useful %d, btb_pc %#lx, entry_pc %#lx\n",
                    match, i, lookupIndices[i], lookupTags[i], way.tag, way.useful, btb_entry.pc, way.pc);

                if (match) {
                    if (!provided) {
                        main_info = TageTableInfo(true, way, i, lookupIndices[i], lookupTags[i]);
                        provided = true;
                    } else if (!alt_provided) {
                        alt_info = TageTableInfo(true, way, i, lookupIndices[i], lookupTags[i]);
                        alt_provided = true;
                        break;
                    }
                }
            }

            Addr main_target = main_info.entry.target;
            Addr alt_target = alt_info.entry.target;

            Addr base_target = btb_entry.target;
            bool base_as_alt = false;


            Addr alt_pred = alt_provided ? alt_target : base_target;
            bool taken = false;
            Addr target;

            // TODO: dynamic control whether to use alt prediction
            bool main_weak = main_info.entry.counter == 0;
            bool use_alt_provider = main_weak && alt_provided;
            bool use_base = !provided || (provided && main_weak && !alt_provided);
            bool use_alt = use_alt_provider || use_base;
            if (provided && !main_weak) {
                taken = main_info.taken();
                target = main_target;
            } else if (alt_provided && use_alt_provider) {
                taken = alt_info.taken();
                target = alt_target;
            } else if (use_base) {
                taken = true;
                target = base_target;
            } else {
                target = base_target;
                warn("no target found\n");
            }
            if (taken) {
                results.push_back({btb_entry.pc, target});
            }
            DPRINTF(ITTAGE, "tage predict %#lx target %#lx\n", btb_entry.pc, target);

            // Update prediction statistics
            if (!provided) {
                ittageStats.predNoHitUseBTB++;
            }
            if (use_alt) {
                ittageStats.predUseAlt++;
            }
            if (provided) {
                ittageStats.predTableHits.sample(main_info.table, 1);
            }
            // Note: predTargetHit will be updated in the update phase when we know the actual target
            TagePrediction pred(btb_entry.pc, main_info, alt_info, use_alt, main_target);
            threadMeta[tid]->preds[btb_entry.pc] = pred;
        }
    }
}

void
BTBITTAGE::dryRunCycle(Addr startPC) {
  return;
}

void
BTBITTAGE::putPCHistory(Addr stream_start, const bitset &history, std::vector<FullBTBPrediction> &stagePreds) {
    const ThreadID tid = predictorTid(stagePreds);
    const uint8_t asidHash = stagePreds.empty() ? 0 : stagePreds.front().asidHash;
    const auto &state = historyState(tid);
    if (debugPC == stream_start) {
        debugFlag = true;
    }
    DPRINTF(ITTAGE, "putPCHistory startAddr: %#lx\n", stream_start);

    // clear old metas
    threadMeta[tid] = std::make_shared<TageMeta>();
    // assign history for meta
    threadMeta[tid]->tagFoldedHist = state.tagFoldedHist;
    threadMeta[tid]->altTagFoldedHist = state.altTagFoldedHist;
    threadMeta[tid]->indexFoldedHist = state.indexFoldedHist;

    lookupEntries.clear();
    lookupIndices.clear();
    lookupTags.clear();
    bitset useful_mask(numPredictors, false);
    // all btb entries should use the same lookup result
    // but each btb entry can use prediction from different tables
    for (int i = 0; i < numPredictors; ++i) {
        Addr index = getTageIndex(stream_start, i, state.indexFoldedHist[i].get(), asidHash);
        Addr tag = getTageTag(stream_start, i, state.tagFoldedHist[i].get(),
                              state.altTagFoldedHist[i].get(), asidHash);
        auto &entry = tageTable[i][index];
        lookupEntries.push_back(entry);
        lookupIndices.push_back(index);
        lookupTags.push_back(tag);
        useful_mask[i] = entry.useful;
        DPRINTF(ITTAGE, "lookup table %d[%d]: valid %d, tag %d, ctr %d, useful %d\n",
            i, index, entry.valid, entry.tag, entry.counter, entry.useful);
    }
    threadMeta[tid]->usefulMask = std::move(useful_mask);

    for (int s = getDelay(); s < stagePreds.size(); s++) {
        auto &stage_pred = stagePreds[s];
        stage_pred.indirectTargets.clear();
        lookupHelper(stream_start, stage_pred.btbEntries,
                     stage_pred.indirectTargets, tid, asidHash);
    }
    DPRINTF(ITTAGE, "putPCHistory end\n");
    debugFlag = false;
}

std::shared_ptr<void>
BTBITTAGE::getPredictionMeta(ThreadID tid) {
    if (tid >= threadMeta.size()) {
        return nullptr;
    }
    return threadMeta[tid];
}

void
BTBITTAGE::updateWithBranchUpdateContext(
    const BranchUpdateContext &ctx,
    const std::vector<ResolvedBranch> &update_branches,
    const std::shared_ptr<void> &prediction_meta)
{
    // get tage predictions from meta
    auto meta = std::static_pointer_cast<TageMeta>(prediction_meta);
    if (!meta) {
        DPRINTF(ITTAGE, "update: no prediction meta, skip\n");
        return;
    }
    std::vector<ResolvedBranch> branches;
    branches.reserve(update_branches.size());
    for (const auto &branch : update_branches) {
        if (branch.taken && branch.isIndirect && !branch.isReturn) {
            branches.push_back(branch);
        }
    }
    if (branches.empty()) {
        return;
    }
    updateWithResolvedBranches(branches, ctx, *meta);
}

void
BTBITTAGE::updateWithResolvedBranches(
    const std::vector<ResolvedBranch> &branches,
    const BranchUpdateContext &ctx,
    const TageMeta &meta)
{
    Addr startAddr = ctx.startPC;
    DPRINTF(ITTAGE, "update startAddr: %#lx\n", startAddr);
    const auto &preds = meta.preds;
    const auto &updateTagFoldedHist = meta.tagFoldedHist;
    const auto &updateAltTagFoldedHist = meta.altTagFoldedHist;
    const auto &updateIndexFoldedHist = meta.indexFoldedHist;

    // update each branch
    for (const auto &branch : branches) {
        auto pred_it = preds.find(branch.pc);
        TagePrediction pred;
        if (pred_it != preds.end()) {
            pred = pred_it->second;
        }
        bool mispred = branch.mispred;
        Addr exe_target = branch.target;
        auto &main_info = pred.mainInfo;

        // Update misprediction statistics
        if (mispred) {
            ittageStats.updateMispred++;
        }
        bool &main_found = main_info.found;
        auto &main_counter = main_info.entry.counter;
        bool main_taken = main_info.taken();
        bool main_weak  = main_counter == 0;
        Addr main_target = main_info.entry.target;

        bool &used_alt = pred.useAlt;
        auto &alt_info = pred.altInfo;
        // update provider
        if (main_found) {
            DPRINTF(ITTAGE, "prediction provided by table %d, idx %d, updating corresponding entry\n",
                main_info.table, main_info.index);
            auto &way = tageTable[main_info.table][main_info.index];
            updateCounter(exe_target == main_target, 2, way.counter); // need modify
            if (way.counter == 0) {
                way.target = exe_target;
            }
            bool alt_taken = (alt_info.found && alt_info.taken()) || !pred.altInfo.found;
            bool alt_diff = alt_taken != main_taken;
            if (alt_diff) {
                way.useful = exe_target == main_target;
            }

            // Update target hit statistics
            if (main_target == exe_target) {
                ittageStats.predTargetHit++;
            }
            ittageStats.updateTableHits.sample(main_info.table, 1);

            if (used_alt && mispred) {
                auto &alt_way = tageTable[pred.altInfo.table][pred.altInfo.index];
                updateCounter(false, 2, alt_way.counter);
                if (alt_way.counter == 0) {
                    alt_way.target = exe_target;
                }
            } else if (used_alt && alt_info.found && alt_info.entry.target == exe_target) {
                // Update alternative prediction correct statistics
                ittageStats.updateUseAltCorrect++;
            }
            DPRINTF(ITTAGE, "useful bit set to %d\n", way.useful);
        }

        bool use_alt_on_main_found_correct = used_alt && main_found && main_target == exe_target;
        bool needToAllocate = mispred && !use_alt_on_main_found_correct;
        DPRINTF(ITTAGE, "mispred %d, use_alt_on_main_found_correct %d, needToAllocate %d\n",
            mispred, use_alt_on_main_found_correct, needToAllocate);

        auto useful_mask = meta.usefulMask;
        int alloc_table_num = numPredictors - (main_info.found ? main_info.table + 1 : 0);
        if (main_found) {
            useful_mask >>= main_info.table + 1;
            useful_mask.resize(alloc_table_num);
        }
        int num_tables_can_allocate = (~useful_mask).count();
        bool canAllocate = num_tables_can_allocate > 0;
        if (needToAllocate) {
            if (canAllocate) {
                usefulResetCnt -= 1;
                if (usefulResetCnt <= 0) {
                    usefulResetCnt = 0;
                }
                DPRINTF(ITTAGE, "can allocate, usefulResetCnt %d\n", usefulResetCnt);
            } else {
                usefulResetCnt += 1;
                if (usefulResetCnt >= 256) {
                    usefulResetCnt = 256;
                }
                DPRINTF(ITTAGE, "can not allocate, usefulResetCnt %d\n", usefulResetCnt);
            }
            if (usefulResetCnt == 256) {
                DPRINTF(ITTAGE, "reset useful bit of all entries\n");
                for (auto &table : tageTable) {
                    for (auto &entry : table) {
                        entry.useful = 0;
                    }
                }
                ittageStats.updateResetU++;
                usefulResetCnt = 0;
            }
        }

        if (needToAllocate) {
            // allocate new entry
            unsigned maskMaxNum = std::pow(2, alloc_table_num);
            unsigned mask = allocLFSR.get() % maskMaxNum;
            bitset allocateLFSR(alloc_table_num, mask);

            auto flipped_usefulMask = useful_mask.flip();
            bitset masked = allocateLFSR & flipped_usefulMask;
            bitset allocate = masked.any() ? masked : flipped_usefulMask;
            if (debugFlag) {
                std::string buf;
                boost::to_string(allocateLFSR, buf);
                DPRINTF(ITTAGE, "allocateLFSR %s, size %d\n", buf, allocateLFSR.size());
                boost::to_string(flipped_usefulMask, buf);
                DPRINTF(ITTAGE, "pred usefulmask %s, size %d\n", buf, useful_mask.size());
                boost::to_string(masked, buf);
                DPRINTF(ITTAGE, "masked %s, size %d\n", buf, masked.size());
                boost::to_string(allocate, buf);
                DPRINTF(ITTAGE, "allocate %s, size %d\n", buf, allocate.size());
            }

            bool allocateValid = flipped_usefulMask.any();
            if (needToAllocate && allocateValid) {
                DPRINTF(ITTAGE, "allocate new entry\n");
                unsigned startTable = main_found ? main_info.table + 1 : 0;

                for (int ti = startTable; ti < numPredictors; ti++) {
                    Addr newIndex = getTageIndex(startAddr, ti,
                                                 updateIndexFoldedHist[ti].get(),
                                                 ctx.asidHash);
                    Addr newTag = getTageTag(startAddr, ti, updateTagFoldedHist[ti].get(),
                                             updateAltTagFoldedHist[ti].get(),
                                             ctx.asidHash);
                    assert(newIndex < tageTable[ti].size());
                    auto &newEntry = tageTable[ti][newIndex];

                    if (allocate[ti - startTable]) {
                        DPRINTF(ITTAGE, "found allocatable entry, table %d, index %d, tag %d, counter %d\n",
                            ti, newIndex, newTag, 2);
                        newEntry = TageEntry(
                            newTag, exe_target, 2, branch.pc);
                        ittageStats.updateAllocSuccess++;
                        break; // allocate only 1 entry
                    }
                }
            } else if (needToAllocate && !allocateValid) {
                ittageStats.updateAllocFailure++;
            }
        }
    }

    DPRINTF(ITTAGE, "end update\n");
    debugFlag = false;
}

void
BTBITTAGE::updateCounter(bool taken, unsigned width, short &counter) {
    int max = (1 << (width)) - 1;
    int min = 0;
    if (taken) {
        satIncrement(max, counter);
    } else {
        satDecrement(min, counter);
    }
}

Addr
BTBITTAGE::getTageTag(Addr pc, int t, uint64_t foldedHist, uint64_t altFoldedHist,
                      uint8_t asidHash)
{
    // Create mask for tableTagBits[t]
    uint64_t mask = ((1ULL << tableTagBits[t]) - 1);

    // Extract lower bits of PC
    uint64_t pcBits = (pc >> floorLog2(blockSize));

    // Prepare alt tag: shift left by 1 and mask
    uint64_t altTagBits = (altFoldedHist << 1);

    // XOR all components
    return injectAsidHashIntoTag((pcBits ^ foldedHist ^ altTagBits) & mask,
                                 tableTagBits[t], asidHash);
}

Addr
BTBITTAGE::getTageTag(Addr pc, int t, uint8_t asidHash)
{
    const auto &state = historyState(0);
    return getTageTag(pc, t, state.tagFoldedHist[t].get(),
                      state.altTagFoldedHist[t].get(), asidHash);
}

Addr
BTBITTAGE::getTageIndex(Addr pc, int t, uint64_t foldedHist, uint8_t asidHash)
{
    // Create mask for tableIndexBits[t]
    uint64_t mask = ((1ULL << tableIndexBits[t]) - 1);

    // Extract lower bits of PC and XOR with folded history
    uint64_t pcBits = (pc >> floorLog2(blockSize));
    return xorAsidHashIntoIndex((pcBits ^ foldedHist) & mask, tableIndexBits[t], asidHash);
}

Addr
BTBITTAGE::getTageIndex(Addr pc, int t, uint8_t asidHash)
{
    return getTageIndex(pc, t, historyState(0).indexFoldedHist[t].get(), asidHash);
}

bool
BTBITTAGE::matchTag(Addr expected, Addr found)
{
    return expected == found;
}

bool
BTBITTAGE::satIncrement(int max, short &counter)
{
    if (counter < max) {
        ++counter;
    }
    return counter == max;
}

bool
BTBITTAGE::satDecrement(int min, short &counter)
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
 * @param taken Whether the branch was taken
 * @param pc The program counter of the branch
 * @param target The target address of the branch
 */
void
BTBITTAGE::doUpdateHist(const boost::dynamic_bitset<> &history, bool taken,
                        Addr pc, Addr target, ThreadID tid)
{
    auto &state = historyState(tid);
    if (debug::ITTAGEHistory) {  // if debug flag is off, do not use to_string since it's too slow
        std::string buf;
        boost::to_string(history, buf);
        DPRINTF(ITTAGEHistory, "in doUpdateHist, taken %d, pc %#lx, history %s\n", taken, pc, buf.c_str());
    }
    if (!taken) {
        DPRINTF(ITTAGEHistory, "not updating folded history, since FB not taken\n");
        return;
    }

    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 3; type++) {
            auto &foldedHist = type == 0 ? state.indexFoldedHist[t]
                                         : type == 1 ? state.tagFoldedHist[t]
                                                     : state.altTagFoldedHist[t];
            // since we have folded path history, we can put arbitrary shamt here, and it wouldn't make a difference
            foldedHist.update(history, 2, taken, pc, target);
            DPRINTF(ITTAGEHistory, "t: %d, type: %d, foldedHist _folded 0x%lx\n", t, type, foldedHist.get());
        }
    }
}

bool
BTBITTAGE::tageHit()
{
    auto meta = getPredictionMeta(0);
    auto preds = std::static_pointer_cast<TageMeta>(meta)->preds;
    bool hit = false;
    for (auto & [pc, pred] : preds) {
        if (pred.mainInfo.found) {
            hit = true;
            break;
        }
    }
    return hit;
}

/**
 * @brief Speculatively updates path folded histories.
 */
void
BTBITTAGE::specUpdatePHist(const boost::dynamic_bitset<> &history,
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
BTBITTAGE::recoverPHist(const boost::dynamic_bitset<> &history,
                        const FetchTarget &entry,
                        const PathHistoryUpdate &update)
{
    auto &state = historyState(entry.tid);
    std::shared_ptr<TageMeta> predMeta = std::static_pointer_cast<TageMeta>(entry.predMetas[getComponentIdx()]);
    for (int i = 0; i < numPredictors; i++) {
        state.tagFoldedHist[i].recover(predMeta->tagFoldedHist[i]);
        state.altTagFoldedHist[i].recover(predMeta->altTagFoldedHist[i]);
        state.indexFoldedHist[i].recover(predMeta->indexFoldedHist[i]);
    }
    doUpdateHist(history, update.taken, update.pc, update.target, entry.tid);
}

void
BTBITTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, const char * when)
{
    checkFoldedHist(hist, 0, when);
}

void
BTBITTAGE::checkFoldedHist(const boost::dynamic_bitset<> &hist, ThreadID tid,
                           const char * when)
{
    auto &state = historyState(tid);
    if (debugFlag) {
        DPRINTF(ITTAGE, "checking folded history when %s\n", when);
        std::string hist_str;
        boost::to_string(hist, hist_str);
        DPRINTF(ITTAGE, "history:\t%s\n", hist_str.c_str());
    }
    for (int t = 0; t < numPredictors; t++) {
        for (int type = 0; type < 2; type++) {
            DPRINTF(ITTAGE, "t: %d, type: %d\n", t, type);
            std::string buf2, buf3;
            auto &foldedHist = type == 0 ? state.indexFoldedHist[t]
                                         : type == 1 ? state.tagFoldedHist[t]
                                                     : state.altTagFoldedHist[t];
            foldedHist.check(hist);
        }
    }
}

void
BTBITTAGE::recordCommittedBranchStats(
    const ResolvedBranch &branch,
    const std::shared_ptr<void> &prediction_meta)
{
    if (!(branch.isIndirect && !branch.isReturn)) {
        // ittage only cares about indirect non-return branches
        return;
    }
    auto meta = std::static_pointer_cast<TageMeta>(prediction_meta);
    if (!meta) {
        return;
    }
    auto pc = branch.pc;
    auto npc = branch.target;
    auto pred_it = meta->preds.find(pc);
    bool this_branch_hit = false;
    Addr pred_npc;
    if (pred_it != meta->preds.end()) {
        this_branch_hit = true;
        pred_npc = (pred_it->second).target;
    }
    bool iscalled = branch.isCall;

     // Update commit statistics
    if (this_branch_hit) {
        ittageStats.commitHits++;
        if (pred_npc == npc) {
            ittageStats.commitPredCorrect++;
            if (iscalled) {
                ittageStats.callPredCorrect++;
            }else {
            ittageStats.otherPredCorrect++;
            }
        } else {
            ittageStats.commitPredWrong++;
            if (iscalled) {
                ittageStats.callPredWrong++;
            }else {
                ittageStats.otherPredWrong++;
            }
        }
        if (iscalled) {
            ittageStats.callHits++;
        }else {
            ittageStats.otherHits++;
        }
    }else {
        ittageStats.commitMisses++;
        ittageStats.commitPredWrong++;
        if (iscalled) {
            ittageStats.callMisses++;
            ittageStats.callPredWrong++;
        }else {
            ittageStats.otherMisses++;
            ittageStats.otherPredWrong++;
        }
    }


}

#ifndef UNIT_TEST
// Constructor for ITTAGE statistics
BTBITTAGE::IttageStats::IttageStats(statistics::Group* parent, int numPredictors) :
    statistics::Group(parent),
    ADD_STAT(predNoHitUseBTB, statistics::units::Count::get(), "use BTB target when no TAGE hit on prediction"),
    ADD_STAT(predUseAlt, statistics::units::Count::get(), "use alternative prediction on prediction"),
    ADD_STAT(predTargetHit, statistics::units::Count::get(), "TAGE target matches actual target (verified during update)"),
    ADD_STAT(updateMispred, statistics::units::Count::get(), "target misprediction on update"),
    ADD_STAT(updateAllocSuccess, statistics::units::Count::get(), "successful allocation when update"),
    ADD_STAT(updateAllocFailure, statistics::units::Count::get(), "allocation failure when update"),
    ADD_STAT(updateResetU, statistics::units::Count::get(), "reset useful bits when update"),
    ADD_STAT(updateUseAltCorrect, statistics::units::Count::get(), "use alternative prediction and correct on update"),
    ADD_STAT(predTableHits, statistics::units::Count::get(), "hit of each tage table on prediction"),
    ADD_STAT(updateTableHits, statistics::units::Count::get(), "hit of each tage table on update"),

    ADD_STAT(commitHits, statistics::units::Count::get(), "number of indirect branch commits that hit in ITTAGE"),
    ADD_STAT(callHits, statistics::units::Count::get(), "number of call commits that hit in ITTAGE"),
    ADD_STAT(otherHits, statistics::units::Count::get(), "number of other (excpet call) commits that hit in ITTAGE"),
    ADD_STAT(commitMisses, statistics::units::Count::get(), "number of indirect branch commits that missed in ITTAGE"),
    ADD_STAT(callMisses, statistics::units::Count::get(), "number of call commits that missed in ITTAGE"),
    ADD_STAT(otherMisses,statistics::units::Count::get(), "number of other (excpet call) commits that missed in ITTAGE"),
    ADD_STAT(commitPredCorrect, statistics::units::Count::get(), "number of indirect branch commits with correct predictions in ITTAGE"),
    ADD_STAT(commitPredWrong, statistics::units::Count::get(), "number of indirect branch commits with wrong predictions in ITTAGE"),
    ADD_STAT(callPredCorrect, statistics::units::Count::get(), "number of call commits with correct predictions in ITTAGE"),
    ADD_STAT(otherPredCorrect, statistics::units::Count::get(), "number of other (except call) commits with correct predictions in ITTAGE"),
    ADD_STAT(callPredWrong, statistics::units::Count::get(), "number of call commits with wrong predictions in ITTAGE"),
    ADD_STAT(otherPredWrong, statistics::units::Count::get(), "number of other (except call) commits with wrong predictions in ITTAGE")
{
    predTableHits.init(0, numPredictors-1, 1);
    updateTableHits.init(0, numPredictors-1, 1);
}
#endif

} // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
