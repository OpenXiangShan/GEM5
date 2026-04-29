#include "cpu/pred/btb/decoupled_bpred.hh"

#include <algorithm>
#include <array>

#include "arch/riscv/regs/misc.hh"
#include "base/debug_helper.hh"
#include "base/output.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/folded_hist.hh"
#include "cpu/thread_context.hh"
#include "debug/BTB.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/Override.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

uint8_t
DecoupledBPUWithBTB::getThreadAsidHash(ThreadID tid) const
{
    if (!cpu) {
        return 0;
    }

    const RegVal satp =
        cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_SATP, tid);
    const uint16_t asid = (satp >> 44) & mask(16);
    return foldAsidHash16To4(asid);
}

namespace
{

BTBEntry
buildPairBlockEntry(const PairTAGE::PairBlockInfo &block, int pairComponentIdx)
{
    BTBEntry entry;
    entry.valid = block.valid && !block.isFallThrough();
    entry.pc = block.branchPC;
    entry.target = block.targetPC;
    entry.size = block.isFallThrough() ? 0 : 4;
    entry.isCond = !block.isFallThrough();
    entry.isDirect = !block.isFallThrough();
    entry.alwaysTaken = false;
    entry.ctr = block.taken ? 0 : -1;
    entry.source = pairComponentIdx;
    return entry;
}

FullBTBPrediction
buildPredictionFromPairBlock(ThreadID tid,
    const PairTAGE::PairBlockInfo &block,
    Addr blockStartPC,
    const FullBTBPrediction &basePred,
    int pairComponentIdx)
{
    FullBTBPrediction pred;
    pred.tid = tid;
    pred.bbStart = blockStartPC;
    pred.predSource = basePred.predSource;
    pred.overrideReason = OverrideReason::NO_OVERRIDE;
    pred.predTick = basePred.predTick;
    pred.s1Source = pairComponentIdx;
    pred.s3Source = pairComponentIdx;

    auto entry = buildPairBlockEntry(block, pairComponentIdx);
    pred.btbEntries.push_back(entry);
    if (!block.isFallThrough()) {
        pred.condTakens.push_back({entry.pc, block.taken});
    }
    return pred;
}

bool
predictionHasUsableEntry(const FullBTBPrediction &pred)
{
    for (const auto &entry : pred.btbEntries) {
        if (entry.valid) {
            return true;
        }
    }
    return false;
}

PairTAGE::PairBlockInfo
buildTrainingPairBlockFromPrediction(const FullBTBPrediction &pred,
                                     Addr predictWidth)
{
    auto predCopy = pred;
    const BTBEntry *trainEntry = nullptr;

    if (predCopy.isTaken()) {
        auto takenEntry = predCopy.getTakenEntry();
        if (takenEntry.valid) {
            for (const auto &btbEntry : predCopy.btbEntries) {
                if (btbEntry.valid && btbEntry.pc == takenEntry.pc) {
                    trainEntry = &btbEntry;
                    break;
                }
            }
        }
    } else {
        for (auto it = predCopy.btbEntries.rbegin();
             it != predCopy.btbEntries.rend(); ++it) {
            if (it->valid && it->isCond && it->isDirect &&
                !it->isIndirect && !it->isCall && !it->isReturn) {
                trainEntry = &*it;
                break;
            }
        }
    }

    if (!trainEntry || !trainEntry->valid || !trainEntry->isCond ||
        !trainEntry->isDirect || trainEntry->isIndirect ||
        trainEntry->isCall || trainEntry->isReturn) {
        if (!predCopy.isTaken() && predCopy.btbEntries.empty()) {
            return PairTAGE::PairBlockInfo(
                false, predCopy.bbStart, predCopy.getFallThrough(predictWidth),
                true);
        }
        if (!predCopy.isTaken() && predCopy.btbEntries.size() == 1) {
            const auto &marker = predCopy.btbEntries.front();
            if (!marker.valid && marker.pc == predCopy.bbStart) {
                return PairTAGE::PairBlockInfo(
                    false, marker.pc, marker.target, true);
            }
        }
        return PairTAGE::PairBlockInfo{};
    }

    return PairTAGE::PairBlockInfo(
        predCopy.isTaken(), trainEntry->pc, trainEntry->target);
}

bool
pairBlocksMatch(const PairTAGE::PairBlockInfo &lhs,
                const PairTAGE::PairBlockInfo &rhs)
{
    if (lhs.valid != rhs.valid) {
        return false;
    }

    if (!lhs.valid) {
        return true;
    }

    return lhs.taken == rhs.taken &&
           lhs.fallThrough == rhs.fallThrough &&
           lhs.branchPC == rhs.branchPC &&
           lhs.targetPC == rhs.targetPC;
}

PairPhase
flippedPairPhase(PairPhase phase)
{
    return phase == PairPhase::Even ? PairPhase::Odd : PairPhase::Even;
}

void
advancePairPhase(PairPhase &phase)
{
    phase = flippedPairPhase(phase);
}

} // namespace
void
DecoupledBPUWithBTB::consumeFetchTarget(unsigned fetched_inst_num, ThreadID tid)
{
    auto &target = ftq.fetching(tid);
    target.fetchInstNum = fetched_inst_num;
    if (target.pairtageUsed) {
        if (target.pairtageSecondBlock) {
            dbpBtbStats.pairtageSecondBlockFetched++;
            dbpBtbStats.pairtageSecondBlockFetchedInsts += fetched_inst_num;
            dbpBtbStats.pairtageSecondBlockFetchedInstsDist.sample(
                fetched_inst_num, 1);
        } else {
            dbpBtbStats.pairtageFirstBlockFetched++;
            dbpBtbStats.pairtageFirstBlockFetchedInsts += fetched_inst_num;
        }
    }
    ftq.finishTarget(tid);
}

DecoupledBPUWithBTB::DecoupledBPUWithBTB(const DecoupledBPUWithBTBParams &p)
    : BPredUnit(p),

      predictWidth(p.predictWidth),
      maxInstsNum(p.predictWidth / 2),
      historyBits(p.maxHistLen),
      ubtb(p.ubtb),
      abtb(p.abtb),
      mbtb(p.mbtb),
      microtage(p.microtage),
      pairtage(p.pairtage),
      tage(p.tage),
      ittage(p.ittage),
      mgsc(p.mgsc),
      ras(p.ras),
      // uras(p.uras),
      bpDBSwitches(p.bpDBSwitches),
      numStages(p.numStages),
      ftqEntries(p.ftq_size),
      ftqMode(p.smtFTQMode),
      ftqPolicy(p.smtFTQPolicy),
      smtFTQThreshold(p.smtFTQThreshold),
      ftq(p.numThreads, p.ftq_size),
      resolveBlockThreshold(p.resolveBlockThreshold),
      dbpBtbStats(this, p.numStages, p.fsq_size, maxInstsNum)
{
    panic_if(ftqMode == SMTFTQMode::Shared &&
             ftqPolicy == SMTFTQPolicy::Threshold &&
             smtFTQThreshold > ftqEntries,
             "SMT FTQ threshold (%u) exceeds total FTQ entries (%u)",
             smtFTQThreshold, ftqEntries);

    if (bpDBSwitches.size() > 0) {
        initDB();
    }
    bpType = DecoupledBTBType;
    // Only add enabled components to the list
    if (ubtb->isEnabled()) components.push_back(ubtb);
    if (abtb->isEnabled()) components.push_back(abtb);
    if (microtage->isEnabled()) components.push_back(microtage);
    if (pairtage->isEnabled()) components.push_back(pairtage);
    if (mbtb->isEnabled()) components.push_back(mbtb);
    if (tage->isEnabled()) components.push_back(tage);
    if (ras->isEnabled()) components.push_back(ras);
    if (ittage->isEnabled()) components.push_back(ittage);
    if (mgsc->isEnabled()) components.push_back(mgsc);
    numComponents = components.size();
    for (int i = 0; i < numComponents; i++) {
        components[i]->setComponentIdx(i);
        if (components[i]->hasDB) {
            bool enableDB = checkGivenSwitch(bpDBSwitches, components[i]->dbName);
            if (enableDB) {
                components[i]->enableDB = true;
                components[i]->setDB(&bpdb);
                components[i]->setTrace();
                removeGivenSwitch(bpDBSwitches, components[i]->dbName);
                someDBenabled = true;
            }
        }
    }
    if (bpDBSwitches.size() > 0) {
        warn("bpDBSwitches contains unknown switches\n");
        printf("unknown switches: ");
        for (auto it = bpDBSwitches.begin(); it != bpDBSwitches.end(); it++) {
            printf("%s ", it->c_str());
        }
        printf("\n");
    }

    historyManagers.reserve(numThreads);
    resolveDequeueFailCounters.assign(numThreads, 0);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        historyManagers.emplace_back(16);
    }

    for (int tid=0;tid<numThreads; tid++) {
        auto& thread = threads[tid];

        thread.s0PC = 0x80000000;
        thread.predsOfEachStage.resize(numStages);
        for (unsigned i = 0; i < numStages; i++) {
            thread.predsOfEachStage[i].predSource = i;
        }
        thread.s0History.resize(historyBits, 0);
        thread.s0PHistory.resize(historyBits, 0);
        thread.s0BwHistory.resize(historyBits, 0);
        thread.s0LHistory.resize(mgsc->getNumEntriesFirstLocalHistories());
        for (unsigned int i = 0; i < mgsc->getNumEntriesFirstLocalHistories(); ++i) {
            thread.s0LHistory[i].resize(historyBits, 0);
        }
        thread.commitHistory.resize(historyBits, 0);
        thread.s0PairPhase = PairPhase::Even;
        thread.squashing = true;
    }

    commitFsqEntryHasInstsVector.resize(maxInstsNum+1, 0);
    lastPhaseFsqEntryNumCommittedInstDist.resize(maxInstsNum+1, 0);
    commitFsqEntryFetchedInstsVector.resize(maxInstsNum+1, 0);
    lastPhaseFsqEntryNumFetchedInstDist.resize(maxInstsNum+1, 0);

    registerExitCallback([this]() {
        this->dumpStats();
    });
}

bool
DecoupledBPUWithBTB::sharedFTQMode() const
{
    return ftqMode == SMTFTQMode::Shared;
}

unsigned
DecoupledBPUWithBTB::activeFTQThreads() const
{
    if (!sharedFTQMode()) {
        return 1;
    }

    if (!cpu) {
        return std::max(1u, numThreads);
    }

    return std::max(1, cpu->numActiveThreads());
}

unsigned
DecoupledBPUWithBTB::totalFTQEntries() const
{
    unsigned total = 0;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        total += ftq.size(tid);
    }
    return total;
}

unsigned
DecoupledBPUWithBTB::sharedFTQAllocation(unsigned entries) const
{
    const unsigned active_threads = activeFTQThreads();

    switch (ftqPolicy) {
      case SMTFTQPolicy::Dynamic:
        return entries;
      case SMTFTQPolicy::Partitioned:
        return entries / active_threads;
      case SMTFTQPolicy::Threshold:
        return active_threads == 1 ? entries : std::min(entries, smtFTQThreshold);
      default:
        panic("Invalid SMT FTQ sharing policy");
    }
}

unsigned
DecoupledBPUWithBTB::logicalMaxFTQEntries(ThreadID tid) const
{
    if (!sharedFTQMode()) {
        return ftqEntries;
    }

    return sharedFTQAllocation(ftqEntries);
}

unsigned
DecoupledBPUWithBTB::logicalFreeFTQEntries(ThreadID tid) const
{
    const unsigned local_max = logicalMaxFTQEntries(tid);
    const unsigned local_used = ftq.size(tid);
    const unsigned local_free = local_used >= local_max ? 0 : local_max - local_used;

    if (!sharedFTQMode()) {
        return local_free;
    }

    const unsigned total_used = totalFTQEntries();
    const unsigned shared_free = total_used >= ftqEntries ? 0 : ftqEntries - total_used;
    return std::min(local_free, shared_free);
}

bool
DecoupledBPUWithBTB::ftqFull(ThreadID tid) const
{
    return logicalFreeFTQEntries(tid) == 0;
}

bool
DecoupledBPUWithBTB::isThreadActive(ThreadID tid) const
{
    if (!cpu) {
        return true;
    }

    auto *tc = cpu->getContext(tid);
    return tc && tc->status() == gem5::ThreadContext::Active;
}

bool
DecoupledBPUWithBTB::canStartPrediction(ThreadID tid) const
{
    const auto &thread = threads[tid];
    return isThreadActive(tid) &&
           !thread.squashing &&
           !thread.redirectPending &&
           !thread.validprediction &&
           !ftqFull(tid);
}

ThreadID
DecoupledBPUWithBTB::scheduleThread()
{
    for (ThreadID offset = 0; offset < numThreads; ++offset) {
        const ThreadID tid = (nextPredictTid + offset) % numThreads;

        if (!isThreadActive(tid)) {
            continue;
        }

        if (!canStartPrediction(tid)) {
            dbpBtbStats.scheduleIneligibleThreadSkips++;
            if (threads[tid].redirectPending) {
                dbpBtbStats.redirectPendingPredictionSkips++;
            }
            continue;
        }

        nextPredictTid = (tid + 1) % numThreads;
        return tid;
    }

    dbpBtbStats.scheduleNoEligibleThread++;
    return InvalidThreadID;
}


void
DecoupledBPUWithBTB::tick()
{
    DPRINTF(Override, "DecoupledBPUWithBTB::tick()\n");

    ThreadID curTid = scheduleThread();
    bool anyActiveThread = false;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (isThreadActive(tid)) {
            anyActiveThread = true;
            break;
        }
    }
    if (!anyActiveThread) {
        return;
    }

    // On squash, reset state if there was a valid prediction.
    bool squashOccurred = false;
    for (int tid = 0; tid < numThreads; tid++) {
        if (threads[tid].squashing) {
            if (tid == curTid) {
                squashOccurred = true;
            }
            threads[tid].validprediction = false;
            threads[tid].numOverrideBubbles = 0;
            threads[tid].nextPredictionAfterSquash = true;
            threads[tid].pendingSecondBlockValid = false;
            threads[tid].secondBlockTrainPredReady = false;
            threads[tid].firstBlockProcessedThisTick = false;
            threads[tid].secondBlockTrainPred = FullBTBPrediction();
            threads[tid].pendingSecondBlockEntry = FetchTarget();
            tage->dryRunCycle(threads[tid].s0PC);
            DPRINTF(Override, "Squashing, BPU state updated.\n");
            threads[tid].squashing = false;
        }
    }

    if (squashOccurred) {
        DPRINTF(Override, "Squash occurred for current thread, skip predict.\n");
        return;
    }

    if (curTid != InvalidThreadID) {
        for (int tid = 0; tid < numThreads; tid++) {
            threads[tid].firstBlockProcessedThisTick = false;
        }
        if (threads[curTid].blockPredictionPending) {
            DPRINTF(Override, "Prediction blocked to prioritize resolve update\n");
            dbpBtbStats.predictionBlockedForUpdate++;
            threads[curTid].blockPredictionPending = false;
        } else {
            requestNewPrediction(curTid);
        }
    }

    for (int tid = 0; tid < numThreads; tid++) {
        processNewPrediction(tid);

        // Decrement override bubbles counter
        auto& numOverrideBubbles = threads[tid].numOverrideBubbles;
        if (numOverrideBubbles > 0) {
            numOverrideBubbles--;
            dbpBtbStats.overrideBubbleNum++;
            DPRINTF(Override, "Consuming override bubble, %d remaining\n", numOverrideBubbles);
        }
    }

    for (int tid = 0; tid < numThreads; tid++) {
        FetchTargetId firstBlockTargetId = 0;
        if (threads[tid].firstBlockProcessedThisTick && !ftq.empty(tid)) {
            firstBlockTargetId = ftq.backId(tid);
        }

        prepareSecondBlockTrainingPrediction(tid);
        processSecondBlock(tid);

        if (firstBlockTargetId != 0 && pairtage && pairtage->isEnabled() &&
            ftq.hasTarget(firstBlockTargetId, tid)) {
            const auto *secondPred = threads[tid].secondBlockTrainPredReady ?
                &threads[tid].secondBlockTrainPred : nullptr;
            pairtage->trainFromActualPred(ftq.get(firstBlockTargetId, tid),
                                          secondPred);
        }
    }

    DPRINTF(Override, "Prediction cycle complete\n");
}

/**
 * @brief Requests new predictions from predictor components
 *
 * If no prediction is in progress and FSQ has space, requests new predictions
 * from each predictor component by sending the current PC and history
 */
void
DecoupledBPUWithBTB::requestNewPrediction(ThreadID tid)
{
    auto& thread = threads[tid];
    auto& predsOfEachStage = threads[tid].predsOfEachStage;
    const uint8_t asid_hash = getThreadAsidHash(tid);

    DPRINTF(Override, "Requesting new prediction for PC %#lx\n", thread.s0PC);

    // Reset all stage-local prediction fields before components fill them.
    clearPreds(tid);
    for (int i = 0; i < numStages; i++) {
        predsOfEachStage[i].tid = tid;
        predsOfEachStage[i].asidHash = asid_hash;
        predsOfEachStage[i].bbStart = thread.s0PC;
        predsOfEachStage[i].predSource = i;
    }

    if (pairtage && pairtage->isEnabled()) {
        pairtage->setPredictionPhase(thread.s0PairPhase);
    }

    // Query each predictor component with current PC and history
    for (int i = 0; i < numComponents; i++) {
        components[i]->putPCHistory(thread.s0PC, thread.s0History, predsOfEachStage);  //s0History not used
    }

    generateFinalPredAndCreateBubbles(tid);

    DPRINTF(Override, "Generating final prediction for PC %#lx\n", thread.s0PC);

    threads[tid].validprediction = true;
}

// this function collects predictions from all stages and generate bubbles
// when loop buffer is active, predictions are from saved target
void
DecoupledBPUWithBTB::generateFinalPredAndCreateBubbles(ThreadID tid)
{
    DPRINTF(Override, "In generateFinalPredAndCreateBubbles().\n");

    auto& predsOfEachStage = threads[tid].predsOfEachStage;
    auto& finalPred = threads[tid].finalPred;

    // 1. Debug output: dump predictions from all stages
    for (int i = 0; i < numStages; i++) {
        printFullBTBPrediction(predsOfEachStage[i]);
    }

    // 2. Select the most accurate prediction (prioritize later stages)
    // Initially assume stage 0 (UBTB) prediction
    FullBTBPrediction *chosenPrediction = &predsOfEachStage[0];

    // Search from last stage to first for valid predictions
    for (int i = (int)numStages - 1; i >= 0; i--) {
        if (predictionHasUsableEntry(predsOfEachStage[i])) {
            chosenPrediction = &predsOfEachStage[i];
            DPRINTF(Override, "Selected prediction from stage %d\n", i);
            break;
        }
    }

    // Store the chosen prediction as our final prediction
    finalPred = *chosenPrediction;

    finalPred.s1Source = -1;//meaning fallthrough
    finalPred.s3Source = -1;

    if (predsOfEachStage[0].btbEntries.size() != 0) {
        for (auto entry : predsOfEachStage[0].btbEntries){
            if (entry.isIndirect || entry.isDirect || entry.ctr >= 0 ||entry.alwaysTaken){
                finalPred.s1Source = entry.source;
                break;
            }
        }
    }

    bool found_s3_taken = false;
    bool na_s3_taken_but_have_cond = false;

    for (BTBEntry entry : predsOfEachStage[2].btbEntries) {
        if (entry.isDirect || entry.isIndirect || entry.ctr >= 0 || entry.alwaysTaken) {
            found_s3_taken = true;
        }else if (entry.isCond){
            //only use when there's no taken prediction in s3
            na_s3_taken_but_have_cond = true;
        }
    }

    if (found_s3_taken) {
        auto pred_taken_entry = finalPred.getTakenEntry();
        if (pred_taken_entry.valid) {
            if (pred_taken_entry.isReturn) {
                finalPred.s3Source = ras->getComponentIdx();
            } else if (pred_taken_entry.isIndirect && ittage->tageHit()) {
                finalPred.s3Source = ittage->getComponentIdx();
            }else if (pred_taken_entry.isCond) {
                finalPred.s3Source = tage->getComponentIdx();
            } else {
                finalPred.s3Source = mbtb->getComponentIdx();
            }
        }else {
            if (na_s3_taken_but_have_cond) {
                finalPred.s3Source = tage->getComponentIdx();
            }else {
                finalPred.s3Source = -1;
            }
        }
    }



    // 3. Calculate override bubbles needed for pipeline consistency
    // Override bubbles are needed when earlier stages predict differently from later stages
    unsigned first_hit_stage = 0;
    OverrideReason overrideReason = OverrideReason::NO_OVERRIDE;

    // Find first stage that matches the chosen prediction
    while (first_hit_stage < numStages - 1) {
        auto [matches, reason] = predsOfEachStage[first_hit_stage].match(*chosenPrediction, predictWidth);
        if (matches) {
            break;
        }
        first_hit_stage++;
        overrideReason = reason;
    }

    // update ubtb/abtb using final S3 prediction
    if (predictionHasUsableEntry(predsOfEachStage[numStages - 1])) {
        if (ubtb->isEnabled()) {
            ubtb->updateUsingS3Pred(predsOfEachStage[numStages - 1]);
        }
        if (abtb->isEnabled() && !ftq.empty(tid)) {
            auto previous_block_startpc = ftq.back(tid).startPC;
            abtb->updateUsingS3Pred(predsOfEachStage[numStages - 1], previous_block_startpc);
        } else if (abtb->isEnabled()) {
            abtb->updateUsingS3Pred(predsOfEachStage[numStages - 1], 0);
        }
        if (microtage->isEnabled()) {
            microtage->updateUsingS3Pred(predsOfEachStage[numStages - 1]);
        }
    }

    // 4. Record override bubbles and update statistics
    if (first_hit_stage > 0) {
        dbpBtbStats.overrideCount++;
        if (finalPred.s1Source == ubtb->getComponentIdx()) {
            ubtb->recordS1OverrideDetail(overrideReason,
                                         abtb->lastPredHasEntries(tid),
                                         threads[tid].nextPredictionAfterSquash);
        }
    }

    // 5. Finalize prediction process
    finalPred.predSource = first_hit_stage;
    finalPred.overrideReason = overrideReason;

    // Debug output for final prediction
    printFullBTBPrediction(finalPred);
    dbpBtbStats.predsOfEachStage[first_hit_stage]++;

    // Clear stage predictions for next cycle
    clearPreds(tid);

    DPRINTF(Override, "Prediction complete: override bubbles=%d\n", first_hit_stage);
    threads[tid].numOverrideBubbles = first_hit_stage;
}

// this function enqueues fsq and update s0PC and s0History
void
DecoupledBPUWithBTB::processNewPrediction(ThreadID tid)
{

    // Check if a prediction is available to enqueue
    if (!threads[tid].validprediction) {
        DPRINTF(Override, "No prediction available to enqueue into FSQ\n");
        return;
    }

    // Check for override bubbles
    // When higher stages override lower stages, bubbles are needed for pipeline consistency
    if (threads[tid].numOverrideBubbles > 0) {
        DPRINTF(Override, "Waiting for %u override bubbles before enqueuing\n", threads[tid].numOverrideBubbles);
        return;
    }

    // Monitor FSQ size for statistics
    dbpBtbStats.fsqEntryDist.sample(ftq.size(tid), 1);
    if (ftqFull(tid)) {
        dbpBtbStats.fsqFullCannotEnq++;
        DPRINTF(Override, "FSQ is full (%lu entries)\n", ftq.size(tid));
        return;
    }

    auto& s0PC = threads[tid].s0PC;

    // Validate PC value
    if (s0PC == MaxAddr) {
        DPRINTF(DecoupleBP, "Invalid PC value %#lx, cannot make prediction\n", s0PC);
        return;
    }

    DPRINTF(DecoupleBP, "Creating new prediction for PC %#lx\n", s0PC);

    // 1. Create a new fetch target entry with prediction information
    FetchTarget entry = createFetchTargetEntry(tid);
    if (pairtage && pairtage->isEnabled()) {
        auto pairMeta = std::static_pointer_cast<PairTAGE::TageMeta>(
            pairtage->getPredictionMeta());
        if (pairMeta && pairMeta->predictedFirstBlock.valid) {
            dbpBtbStats.pairtageFirstBlockCandidates++;
            if (entry.pairtageUsed) {
                dbpBtbStats.pairtageFirstBlockSelected++;
            } else {
                dbpBtbStats.pairtageFirstBlockOverridden++;
            }
        }
    }

    // 2. Update global PC state to target or fall-through
    s0PC = threads[tid].finalPred.getTarget(predictWidth);;

    // 3. Update history information
    updateHistoryForPrediction(entry, threads[tid].finalPred);

    // 4. Fill ahead pipeline
    fillAheadPipeline(entry);

    if (enablePredFSQTrace) {
        predTraceManager->write_record(PredictionTrace(ftq.backId(tid), entry));
    }

    // 5. Add entry to fetch target queue
    ftq.insert(entry);
    threads[tid].nextPredictionAfterSquash = false;
    advancePairPhase(threads[tid].s0PairPhase);
    threads[tid].validprediction = false;
    threads[tid].firstBlockProcessedThisTick = true;

    // 6. Debug output and update statistics
    dumpFsq("after insert new target");
    DPRINTF(DecoupleBP, "Inserted fetch target %lu starting at PC %#lx\n",
            ftq.backId(tid), entry.startPC);

    // 7. Increment statistics
    printTarget(entry);
    dbpBtbStats.fsqEntryEnqueued++;
}

void
DecoupledBPUWithBTB::processSecondBlock(ThreadID tid)
{
    auto &thread = threads[tid];

    thread.pendingSecondBlockValid = false;
    thread.pendingSecondBlockEntry = FetchTarget();

    if (!thread.firstBlockProcessedThisTick) {
        return;
    }

    if (!pairtage || !pairtage->isEnabled()) {
        return;
    }

    dbpBtbStats.pairtageSecondBlockAttempted++;

    if (!pairtage->secondBlockEnabled()) {
        dbpBtbStats.pairtageSecondBlockSkippedDisabled++;
        return;
    }

    if (!currentFirstBlockHasEvenPairPhase(tid)) {
        dbpBtbStats.pairtageSecondBlockSkippedOddPhase++;
        DPRINTF(DecoupleBP,
                "Skip PairTAGE second block for thread %u because first block phase is Odd\n",
                tid);
        return;
    }

    auto firstBlockStatus = pairtageFirstBlockStatusForSecondBlock(tid);
    if (firstBlockStatus != PairtageFirstBlockSecondBlockStatus::Match) {
        dbpBtbStats.pairtageSecondBlockSkippedFirstBlockOverridden++;
        switch (firstBlockStatus) {
          case PairtageFirstBlockSecondBlockStatus::NoCandidateLookupMiss:
            dbpBtbStats.pairtageSecondBlockNoFirstBlockCandidate++;
            dbpBtbStats.pairtageSecondBlockNoFirstBlockLookupMiss++;
            break;
          case PairtageFirstBlockSecondBlockStatus::NoCandidateUntrainable:
            dbpBtbStats.pairtageSecondBlockNoFirstBlockCandidate++;
            dbpBtbStats.pairtageSecondBlockNoFirstBlockUntrainable++;
            break;
          case PairtageFirstBlockSecondBlockStatus::FallThruMismatch:
            dbpBtbStats.pairtageSecondBlockFirstBlockMismatchFallThru++;
            break;
          case PairtageFirstBlockSecondBlockStatus::ControlAddrMismatch:
            dbpBtbStats.pairtageSecondBlockFirstBlockMismatchControlAddr++;
            break;
          case PairtageFirstBlockSecondBlockStatus::TargetMismatch:
            dbpBtbStats.pairtageSecondBlockFirstBlockMismatchTarget++;
            break;
          case PairtageFirstBlockSecondBlockStatus::Match:
            break;
        }
        DPRINTF(DecoupleBP,
                "Skip PairTAGE second block for thread %u because first block was overridden by final prediction\n",
                tid);
        return;
    }

    if (ftq.full(tid)) {
        dbpBtbStats.pairtageSecondBlockSkippedFtqFull++;
        DPRINTF(DecoupleBP,
                "Skip PairTAGE second block enqueue for thread %u because FTQ is full\n",
                tid);
        return;
    }

    auto secondBlock = pairtage->getSecondPredBlock();
    if (!secondBlock.valid) {
        dbpBtbStats.pairtageSecondBlockNoCandidate++;
        DPRINTF(DecoupleBP,
                "No pending PairTAGE second block for thread %u after first block\n",
                tid);
        return;
    }

    if (thread.secondBlockTrainPredReady) {
        auto trainedSecondBlock =
            buildTrainingPairBlockFromPrediction(thread.secondBlockTrainPred,
                                                 predictWidth);
        if (!pairBlocksMatch(secondBlock, trainedSecondBlock)) {
            dbpBtbStats.pairtageSecondBlockTeacherDisagree++;
            DPRINTF(DecoupleBP,
                    "Skip PairTAGE second block enqueue for thread %u because training prediction disagrees: "
                    "pairtage(valid=%d pc=%#lx target=%#lx taken=%d) vs "
                    "teacher(valid=%d pc=%#lx target=%#lx taken=%d)\n",
                    tid,
                    secondBlock.valid, secondBlock.branchPC,
                    secondBlock.targetPC, secondBlock.taken,
                    trainedSecondBlock.valid, trainedSecondBlock.branchPC,
                    trainedSecondBlock.targetPC, trainedSecondBlock.taken);
            return;
        }
        dbpBtbStats.pairtageSecondBlockTeacherAgree++;
    } else {
        dbpBtbStats.pairtageSecondBlockNoTeacher++;
    }

    auto secondPred = buildPredictionFromPairBlock(
        tid, secondBlock, thread.s0PC, thread.finalPred, pairtage->getComponentIdx());
    refreshSecondBlockPredictionMetas(tid, secondPred);
    auto entry = createFetchTargetEntry(tid, thread.s0PC, secondPred);
    entry.pairtageUsed = true;
    entry.pairtageSecondBlock = true;

    thread.s0PC = secondPred.getTarget(predictWidth);
    updateHistoryForPrediction(entry, secondPred);
    fillAheadPipeline(entry);
    ftq.insert(entry);
    advancePairPhase(thread.s0PairPhase);

    thread.pendingSecondBlockEntry = entry;
    thread.pendingSecondBlockValid = true;
    dbpBtbStats.pairtageSecondBlockEnqueued++;
    dbpBtbStats.pairtageSecondBlockPredBytes += entry.predEndPC - entry.startPC;
    if (entry.predTaken) {
        dbpBtbStats.pairtageSecondBlockPredTaken++;
    } else {
        dbpBtbStats.pairtageSecondBlockPredNotTaken++;
    }

    DPRINTF(DecoupleBP,
            "Inserted PairTAGE second block %lu for thread %u: startPC %#lx, branchPC %#lx, target %#lx, taken %d\n",
            ftq.backId(tid), tid, entry.startPC, secondBlock.branchPC,
            secondBlock.targetPC, secondBlock.taken);

    printTarget(entry);
    dbpBtbStats.fsqEntryEnqueued++;
}

void
DecoupledBPUWithBTB::refreshSecondBlockPredictionMetas(
    ThreadID tid, FullBTBPrediction &pred)
{
    auto &thread = threads[tid];

    if (pairtage && pairtage->isEnabled()) {
        pairtage->setPredictionPhase(thread.s0PairPhase);
    }

    pred.tageInfoForMgscs.clear();
    for (int i = 0; i < numComponents; ++i) {
        components[i]->refreshPredictionMeta(thread.s0PC, thread.s0History, pred);
    }
}

void
DecoupledBPUWithBTB::prepareSecondBlockTrainingPrediction(ThreadID tid)
{
    auto &thread = threads[tid];
    thread.secondBlockTrainPredReady = false;
    thread.secondBlockTrainPred = FullBTBPrediction();

    if (!thread.firstBlockProcessedThisTick) {
        return;
    }

    if (!pairtage || !pairtage->isEnabled() || !mbtb || !mbtb->isEnabled()) {
        return;
    }

    if (!pairtage->secondBlockEnabled()) {
        return;
    }

    if (!currentFirstBlockHasEvenPairPhase(tid)) {
        return;
    }

    if (pairtageFirstBlockStatusForSecondBlock(tid) !=
        PairtageFirstBlockSecondBlockStatus::Match) {
        DPRINTF(DecoupleBP,
                "Skip PairTAGE second-block training prediction for thread %u because first block was overridden\n",
                tid);
        return;
    }

    auto &secondPred = thread.secondBlockTrainPred;
    secondPred.tid = tid;
    secondPred.bbStart = thread.s0PC;
    secondPred.predSource = thread.finalPred.predSource;
    secondPred.overrideReason = OverrideReason::NO_OVERRIDE;
    secondPred.predTick = thread.finalPred.predTick;
    secondPred.s1Source = mbtb->getComponentIdx();
    secondPred.s3Source = mbtb->getComponentIdx();

    secondPred.btbEntries = mbtb->getPredictedEntriesNoSideEffect(thread.s0PC);
    secondPred.condTakens.clear();
    secondPred.indirectTargets.clear();
    secondPred.tageInfoForMgscs.clear();
    secondPred.returnTarget = 0;

    if (tage && tage->isEnabled()) {
        tage->lookupNoSideEffect(thread.s0PC, secondPred.btbEntries,
                                 secondPred.condTakens);
        secondPred.s3Source = tage->getComponentIdx();
    } else {
        for (const auto &entry : secondPred.btbEntries) {
            if (entry.valid && entry.isCond) {
                secondPred.condTakens.push_back(
                    {entry.pc, entry.alwaysTaken || (entry.ctr >= 0)});
            }
        }
    }

    thread.secondBlockTrainPredReady = true;
    dbpBtbStats.pairtageSecondBlockTrainPrepared++;

    DPRINTF(DecoupleBP,
            "Prepared PairTAGE second-block training prediction for thread %u: startPC %#lx, %zu BTB entries, %zu "
            "cond takens\n",
            tid, secondPred.bbStart, secondPred.btbEntries.size(), secondPred.condTakens.size());
}

bool
DecoupledBPUWithBTB::currentFirstBlockHasEvenPairPhase(ThreadID tid) const
{
    return threads[tid].firstBlockProcessedThisTick &&
           !ftq.empty(tid) &&
           ftq.back(tid).pairPhase == PairPhase::Even;
}

DecoupledBPUWithBTB::PairtageFirstBlockSecondBlockStatus
DecoupledBPUWithBTB::pairtageFirstBlockStatusForSecondBlock(ThreadID tid) const
{
    if (!pairtage || !pairtage->isEnabled()) {
        return PairtageFirstBlockSecondBlockStatus::NoCandidateLookupMiss;
    }

    if (!currentFirstBlockHasEvenPairPhase(tid)) {
        return PairtageFirstBlockSecondBlockStatus::NoCandidateLookupMiss;
    }

    auto &thread = threads[tid];
    auto pairMeta = std::static_pointer_cast<PairTAGE::TageMeta>(
        pairtage->getPredictionMeta());
    if (!pairMeta || !pairMeta->firstBlockValid ||
        !pairMeta->predictedFirstBlock.valid) {
        return buildTrainingPairBlockFromPrediction(thread.finalPred,
                                                    predictWidth).valid ?
            PairtageFirstBlockSecondBlockStatus::NoCandidateLookupMiss :
            PairtageFirstBlockSecondBlockStatus::NoCandidateUntrainable;
    }

    auto actualFirstBlock = buildTrainingPairBlockFromPrediction(
        thread.finalPred, predictWidth);
    if (!actualFirstBlock.valid) {
        return PairtageFirstBlockSecondBlockStatus::NoCandidateUntrainable;
    }

    if (pairBlocksMatch(actualFirstBlock, pairMeta->predictedFirstBlock)) {
        return PairtageFirstBlockSecondBlockStatus::Match;
    }

    if (actualFirstBlock.taken != pairMeta->predictedFirstBlock.taken) {
        return PairtageFirstBlockSecondBlockStatus::FallThruMismatch;
    }
    if (actualFirstBlock.branchPC != pairMeta->predictedFirstBlock.branchPC) {
        return PairtageFirstBlockSecondBlockStatus::ControlAddrMismatch;
    }
    if (actualFirstBlock.targetPC != pairMeta->predictedFirstBlock.targetPC ||
        actualFirstBlock.fallThrough !=
            pairMeta->predictedFirstBlock.fallThrough) {
        return PairtageFirstBlockSecondBlockStatus::TargetMismatch;
    }

    return PairtageFirstBlockSecondBlockStatus::TargetMismatch;
}

bool
DecoupledBPUWithBTB::predictionMatchesPairtageFirstBlock(
    const FullBTBPrediction &pred) const
{
    if (!pairtage || !pairtage->isEnabled()) {
        return false;
    }

    auto pairMeta = std::static_pointer_cast<PairTAGE::TageMeta>(
        pairtage->getPredictionMeta());
    if (!pairMeta || !pairMeta->firstBlockValid ||
        !pairMeta->predictedFirstBlock.valid) {
        return false;
    }

    return pairBlocksMatch(buildTrainingPairBlockFromPrediction(pred, predictWidth),
                           pairMeta->predictedFirstBlock);
}

/**
 * @brief Common logic for handling squash events
 *
 * This function encapsulates the shared logic between different types of squashes:
 * - Setting squashing state
 * - Finding and updating the target
 * - Recovering history information
 * - Clearing predictions
 * - Updating FTQ and FSQ state
 *
 * @param target_id ID of the target being squashed
 * @param squash_type Type of squash (CTRL/OTHER/TRAP)
 * @param squash_pc PC where the squash occurred
 * @param redirect_pc PC to redirect to after squash
 * @param is_conditional Whether the squash is caused by a conditional branch
 * @param actually_taken Whether the branch was actually taken (for conditional branches)
 * @param static_inst Static instruction pointer (for control squash)
 * @param control_inst_size Size of the control instruction (for control squash)
 */
void
DecoupledBPUWithBTB::handleSquash(ThreadID tid, unsigned target_id,
                                 SquashType squash_type,
                                 const PCStateBase &squash_pc,
                                 Addr redirect_pc,
                                 bool is_conditional,
                                 bool actually_taken,
                                 const StaticInstPtr &static_inst,
                                 unsigned control_inst_size)
{
    // Set squashing state
    threads[tid].squashing = true;
    threads[tid].redirectPending = false;

    // Find the target being squashed
    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP,
                "Ignore squash for tid %u on missing FTQ target %u; "
                "recovering predictor state from redirect PC %#lx\n",
                tid, target_id, redirect_pc);
        ftq.clear(tid);
        clearPreds(tid);
        threads[tid].validprediction = false;
        threads[tid].s0PC = redirect_pc;
        return;
    }

    // Get reference to the target
    auto &target = ftq.get(target_id, tid);

    // Update target state
    target.resolved = true;
    target.exeTaken = actually_taken;
    target.squashPC = squash_pc.instAddr();
    target.squashType = squash_type;

    // Special handling for control squash - create branch info
    if (squash_type == SQUASH_CTRL && static_inst) {
        // Use full branch info with static_inst if available
        target.exeBranchInfo = BranchInfo(squash_pc.instAddr(), redirect_pc, static_inst, control_inst_size);
        dumpFsq("Before control squash");
    }

    // Remove targets after the squashed one
    ftq.squashAfter(target_id, tid);

    // Recover history using the extracted function
    recoverHistoryForSquash(target, target_id, squash_pc, is_conditional, actually_taken, squash_type, redirect_pc);

    // Clear predictions for next cycle
    clearPreds(tid);

    // Update PC and target ID
    threads[tid].s0PC = redirect_pc;

    DPRINTF(DecoupleBP,
            "After squash, fsqId(next alloc)=%lu, fetchHeadFsqId=%lu, s0pc=%#lx\n",
            ftq.backId(tid) + 1, ftq.frontId(tid), redirect_pc);
}

void
DecoupledBPUWithBTB::controlSquash(unsigned target_id,
                            const PCStateBase &control_pc,
                            const PCStateBase &corr_target,
                            const StaticInstPtr &static_inst,
                            unsigned control_inst_size, bool actually_taken,
                            const InstSeqNum &seq, ThreadID tid,
                            const unsigned &currentLoopIter, const bool fromCommit)
{
    if (fromCommit) {
        dbpBtbStats.controlSquashFromCommit++;
        auto branchClass = classifyBranch(static_inst);
        addControlSquashCommitStat(branchClass);
    } else {
        dbpBtbStats.controlSquashFromDecode++;
    }

    // Get branch type information
    bool is_conditional = static_inst->isCondCtrl();
    bool is_indirect = static_inst->isIndirectCtrl();

    if (!ftq.hasTarget(target_id, tid)) {
        threads[tid].redirectPending = false;
        DPRINTF(DecoupleBP, "The squashing target is insane, ignore squash on it");
        return;
    }
    auto &target = ftq.get(target_id, tid);
    // Get target address
    Addr real_target = corr_target.instAddr();
    if (!fromCommit && static_inst->isReturn() && !static_inst->isNonSpeculative()) {
        // get ret addr from ras meta
        real_target = ras->getTopAddrFromMetas(target);
        // TODO: set real target to dynamic inst
    }

    // Detailed debugging for control squash
    DPRINTF(DecoupleBP,
            "Control squash: ftq_id=%d,"
            " control_pc=%#lx, real_target=%#lx, is_conditional=%u, "
            "is_indirect=%u, actually_taken=%u, branch seq: %lu\n",
            target_id, control_pc.instAddr(),
            real_target, is_conditional, is_indirect,
            actually_taken, seq);

    // Call shared squash handling logic
    handleSquash(tid, target_id, SQUASH_CTRL, control_pc,
                real_target, is_conditional, actually_taken, static_inst, control_inst_size);
}

void
DecoupledBPUWithBTB::nonControlSquash(unsigned target_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.nonControlSquash++;
    DPRINTF(DecoupleBP,
            "non control squash: target id: %d, inst_pc: %#lx, "
            "seq: %lu\n",
            target_id, inst_pc.instAddr(), seq);

    // Call shared squash handling logic
    handleSquash(tid, target_id, SQUASH_OTHER, inst_pc, inst_pc.instAddr());
}

void
DecoupledBPUWithBTB::trapSquash(unsigned target_id,
                         Addr last_committed_pc, const PCStateBase &inst_pc,
                         ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.trapSquash++;
    DPRINTF(DecoupleBP,
            "Trap squash: target id: %d, inst_pc: %#lx\n",
            target_id, inst_pc.instAddr());

    // Call shared squash handling logic
    handleSquash(tid, target_id, SQUASH_TRAP, inst_pc, inst_pc.instAddr());
}

void
DecoupledBPUWithBTB::commit(unsigned target_id, ThreadID tid)
{
    // No need to dequeue when queue is empty
    if (ftq.empty(tid)) {
        return;
    }

    // Process all targets that have been committed (target_id >= head target id).
    while (!ftq.empty(tid) && target_id >= ftq.frontId(tid)) {
        auto &target = ftq.front(tid);

        DPRINTF(DecoupleBP,
                "Commit target start %#lx, which is predicted, "
                "final br addr: %#lx, final target: %#lx, pred br addr: %#lx, "
                "pred target: %#lx\n",
                target.startPC, target.exeBranchInfo.pc, target.exeBranchInfo.target, target.predBranchInfo.pc,
                target.predBranchInfo.target);

        // Update statistics
        updateStatistics(target);

        // Update predictor components
        updatePredictorComponents(target);

        ftq.commitTarget(tid);
        dbpBtbStats.fsqEntryCommitted++;
    }

    DPRINTF(DecoupleBP, "after commit target, fetchTargetQueue size: %lu\n", ftq.size(tid));

    if (!ftq.empty(tid))
        printTarget(ftq.front(tid));

    historyManagers[tid].commit(target_id);
}

bool
DecoupledBPUWithBTB::resolveUpdate(unsigned &target_id, ThreadID tid)
{
    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP, "Target id %u not found in fetchTargetQueue, cannot update predictors\n", target_id);
        return true;
    }

    auto &target = ftq.get(target_id, tid);

    // Update predictor components only if the target is hit or taken
    if (!(target.isHit || target.exeTaken)) {
        return true;
    }

    // Phase 1: probe all resolved-update components to ensure no blocker
    for (int i = 0; i < numComponents; ++i) {
        if (components[i]->getResolvedUpdate()) {
            if (!components[i]->canResolveUpdate(target)) {
                return false;
            }
        }
    }

    // Phase 2: all clear, perform updates once
    for (int i = 0; i < numComponents; ++i) {
        if (components[i]->getResolvedUpdate()) {
            components[i]->doResolveUpdate(target);
        }
    }

    return true;
}

void
DecoupledBPUWithBTB::notifyResolveSuccess(ThreadID tid)
{
    resolveDequeueFailCounters[tid] = 0;
}

void
DecoupledBPUWithBTB::notifyResolveFailure(ThreadID tid)
{
    auto &failCounter = resolveDequeueFailCounters[tid];
    failCounter++;
    if (failCounter >= resolveBlockThreshold) {
        blockPredictionOnce(tid);
        failCounter = 0;
    }
}

void
DecoupledBPUWithBTB::blockPredictionOnce(ThreadID tid)
{
    threads[tid].blockPredictionPending = true;
}

void
DecoupledBPUWithBTB::setRedirectPending(ThreadID tid, bool pending)
{
    threads[tid].redirectPending = pending;
}

void
DecoupledBPUWithBTB::prepareResolveUpdateEntries(unsigned &target_id, ThreadID tid)
{
    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP, "Target id %u not found in fetchTargetQueue, cannot update predictors\n", target_id);
        return;
    }
    auto &target = ftq.get(target_id, tid);

    if (target.isHit || target.exeTaken) {
        // Prepare target for update
        target.setUpdateInstEndPC(predictWidth);
        target.setUpdateBTBEntries();

        // only mbtb can generate new entry
        if (mbtb->isEnabled()) {
            mbtb->getAndSetNewBTBEntry(target);
        }
    }
}

void
DecoupledBPUWithBTB::markCFIResolved(unsigned &target_id, uint64_t resolvedInstPC, ThreadID tid)
{

    if (!ftq.hasTarget(target_id, tid)) {
        DPRINTF(DecoupleBP, "Target id %u not found in fetchTargetQueue, cannot update predictors\n", target_id);
        return;
    }
    auto &target = ftq.get(target_id, tid);

    if (target.updateNewBTBEntry.pc == resolvedInstPC) {
        target.updateNewBTBEntry.resolved = true;
    }

    target.markBTBEntryResolved(resolvedInstPC);
}

void
DecoupledBPUWithBTB::updatePredictorComponents(FetchTarget &target)
{
    // Update predictor components only if the target is hit or taken
    if (target.isHit || target.exeTaken) {
        // Prepare target for update
        target.setUpdateInstEndPC(predictWidth);
        target.setUpdateBTBEntries();

        // only mbtb can generate new entry
        if (mbtb->isEnabled()) {
            mbtb->getAndSetNewBTBEntry(target);
        }

        // Update predictor components
        for (int i = 0; i < numComponents; ++i) {
            if (!components[i]->getResolvedUpdate()) {
                components[i]->update(target);
            }
        }
    }
}


void
DecoupledBPUWithBTB::histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history)
{
    if (shamt == 0) {
        return;
    }
    history <<= shamt;
    history[0] = taken;
}

void
DecoupledBPUWithBTB::pHistShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history, Addr pc, Addr target)
{
    if (shamt == 0) {
        return;
    }
    if(taken){
        // Calculate path hash
        uint64_t hash = pathHash(pc, target);

        history <<= shamt;
        for (auto i = 0; i < pathHashLength && i < history.size(); i++) {
            history[i] = (hash & 1) ^ history[i];
            hash >>= 1;
        }
    }
}

/**
 * @brief Creates a new FetchTarget entry with prediction information
 *
 * @return FetchTarget The created fetch target
 */
FetchTarget
DecoupledBPUWithBTB::createFetchTargetEntry(ThreadID tid)
{
    return createFetchTargetEntry(tid, threads[tid].s0PC, threads[tid].finalPred);
}

FetchTarget
DecoupledBPUWithBTB::createFetchTargetEntry(
    ThreadID tid, Addr startPC, FullBTBPrediction &pred)
{
    auto& s0History = threads[tid].s0History;
    auto& s0PHistory = threads[tid].s0PHistory;
    auto& s0BwHistory = threads[tid].s0BwHistory;
    auto& s0LHistory = threads[tid].s0LHistory;

    // Create a new fetch target entry
    FetchTarget entry;
    entry.tid = tid;
    entry.asidHash = pred.asidHash;
    entry.startPC = startPC;

    // Extract branch prediction information
    bool taken = pred.isTaken();
    Addr fallThroughAddr = pred.getFallThrough(predictWidth);
    Addr nextPC = pred.getTarget(predictWidth);

    // Configure target entry with prediction details
    panic_if(numComponents > entry.predMetas.size(),
             "Too many BTB predictor components (%u) for FetchTarget meta slots (%zu)",
             numComponents, entry.predMetas.size());

    auto pairMeta = pairtage ? std::static_pointer_cast<PairTAGE::TageMeta>(
        pairtage->getPredictionMeta()) : nullptr;
    const bool pairtageFallThroughHit = pairMeta &&
        pairMeta->firstBlockValid &&
        pairMeta->predictedFirstBlock.valid &&
        pairMeta->predictedFirstBlock.isFallThrough();

    entry.isHit = !pred.btbEntries.empty() || pairtageFallThroughHit;
    entry.falseHit = false;
    entry.predBTBEntries = pred.btbEntries;
    if (pairtageFallThroughHit && entry.predBTBEntries.empty()) {
        entry.predBTBEntries.push_back(buildPairBlockEntry(
            pairMeta->predictedFirstBlock, pairtage->getComponentIdx()));
    }
    entry.predTaken = taken;
    entry.predEndPC = fallThroughAddr;

    // Set branch info for taken predictions
    if (taken) {
        entry.predBranchInfo = pred.getTakenEntry().getBranchInfo();
        entry.predBranchInfo.target = nextPC; // Use final target (may not be from BTB)
    }

    // Record current history and prediction metadata
    entry.history = s0History;
    entry.phistory = s0PHistory;
    entry.bwhistory = s0BwHistory;
    entry.lhistory = s0LHistory;
    entry.pairPhase = threads[tid].s0PairPhase;
    entry.predTick = pred.predTick;
    entry.predSource = pred.predSource;
    entry.overrideReason = pred.overrideReason;
    entry.pairtageUsed = predictionMatchesPairtageFirstBlock(pred);
    entry.pairtageSecondBlock = false;

    entry.s1Source = pred.s1Source;
    entry.s3Source = pred.s3Source;

    // Save predictors' metadata
    for (int i = 0; i < numComponents; i++) {
        entry.predMetas[i] = components[i]->getPredictionMeta(tid);
    }

    // Initialize default resolution state
    entry.setDefaultResolve();

    return entry;
}

/**
 * @brief fill ahead pipeline entry.previousPCs
 */
void
DecoupledBPUWithBTB::fillAheadPipeline(FetchTarget &entry)
{
    ThreadID tid = entry.tid;
    // Handle ahead pipelined predictors
    unsigned max_ahead_pipeline_stages = 0;
    for (int i = 0; i < numComponents; i++) {
        max_ahead_pipeline_stages = std::max(max_ahead_pipeline_stages, components[i]->aheadPipelinedStages);
    }

    // Get previous PCs from fetchTargetQueue if needed
    if (max_ahead_pipeline_stages > 0) {
        for (int i = 0; i < max_ahead_pipeline_stages; i++) {
            auto id = ftq.backId(tid) + 1 - max_ahead_pipeline_stages + i;
            if (ftq.hasTarget(id, tid)) {
                // FIXME: it may not work well with jump ahead predictor
                entry.previousPCs.push(ftq.get(id, tid).getRealStartPC());
            }
        }
    }
}

void
DecoupledBPUWithBTB::checkHistories(const boost::dynamic_bitset<> &history,
                                    const boost::dynamic_bitset<> &phistory,
                                    ThreadID tid)
{
    DPRINTF(DecoupleBP, "Checking GHR/PHR speculative history replay\n");
    assert(historyManagers[tid].checkGHist(history, historyBits));
    assert(historyManagers[tid].checkPHist(phistory, historyBits));
}

void
DecoupledBPUWithBTB::resetPC(Addr new_pc)
{
    for (int i = 0; i < numThreads; i++) {
        threads[i].s0PC = new_pc;
        threads[i].redirectPending = false;
    }
}

void
DecoupledBPUWithBTB::resetPC(ThreadID tid, Addr new_pc)
{
    threads[tid].s0PC = new_pc;
    threads[tid].redirectPending = false;
}

Addr
DecoupledBPUWithBTB::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    DPRINTF(DecoupleBP, "acquiring reutrn address for inst pc %#lx from decode\n", dynInst->pcState().instAddr());
    auto ftqid = dynInst->getFtqId();
    auto retAddr = ras->getTopAddrFromMetas(ftq.get(ftqid, dynInst->threadNumber));
    DPRINTF(DecoupleBP, "get ret addr %#lx\n", retAddr);
    return retAddr;
}

/**
 * @brief Updates global history based on prediction results
 *
 * @param entry The fetch target entry to update history for
 */
void
DecoupledBPUWithBTB::updateHistoryForPrediction(FetchTarget &entry,
                                                FullBTBPrediction &pred)
{
    ThreadID tid = entry.tid;
    auto& s0History = threads[tid].s0History;
    auto& s0PHistory = threads[tid].s0PHistory;
    auto& s0BwHistory = threads[tid].s0BwHistory;
    auto& s0LHistory = threads[tid].s0LHistory;

    const auto ghist_update = finalPred.getGHistUpdate();
    const auto bwhist_update = finalPred.getBwHistUpdate();
    const auto phist_update = finalPred.getPHistUpdate();

    // RAS updates its speculative stack, not folded history.
    if (ras->isEnabled()) {
        ras->specUpdateState(finalPred);
    }

    // Update component-local folded histories.
    for (int i = 0; i < numComponents; i++) {
        // use old histories to update predictor-local folded histories
        components[i]->specUpdateGHist(s0History, finalPred, ghist_update);
        components[i]->specUpdatePHist(s0PHistory, finalPred, phist_update);
    }
    if (mgsc->isEnabled()) {
        mgsc->specUpdateBwHist(s0BwHistory, finalPred, bwhist_update);
        mgsc->specUpdateIHist(finalPred, bwhist_update);
        mgsc->specUpdateLHist(s0LHistory, finalPred, ghist_update);
    }

    // Update global history
    histShiftIn(ghist_update.shamt, ghist_update.taken, s0History);

    // Update history manager and verify TAGE folded history
    historyManagers[tid].addSpeculativeHist(
        entry.startPC, entry.history, entry.phistory, ghist_update,
        phist_update, entry.predBranchInfo, ftq.backId(tid) + 1);

    // Update global backward history
    histShiftIn(bwhist_update.shamt, bwhist_update.taken, s0BwHistory);

    // Update path history
    pHistShiftIn(phist_update.shamt, phist_update.taken, s0PHistory,
                 phist_update.pc, phist_update.target);

    // Update local history
    const Addr localHistoryIndex =
        mgsc->getPcIndex(pred.bbStart,
                         log2(mgsc->getNumEntriesFirstLocalHistories()),
                         pred.asidHash);
    histShiftIn(ghist_update.shamt, ghist_update.taken,
        s0LHistory[localHistoryIndex]);

#ifndef NDEBUG
    if (tage->isEnabled()) {
        tage->checkFoldedHist(
            tage->usesPathHistory() ? s0PHistory : s0History, tid,
            "speculative update");
    }
    if (ittage->isEnabled()) {
        ittage->checkFoldedHist(s0PHistory, tid, "speculative update");
    }
    if (microtage->isEnabled()) {
        microtage->checkFoldedHist(s0PHistory, tid, "speculative update");
    }
    if (mgsc->isEnabled()) {
        mgsc->checkFoldedHist(s0History, s0PHistory, s0LHistory, tid,
                              "speculative update");
    }
#endif
}

/**
 * @brief Recovers branch history during a squash event
 *
 * @param target The target being squashed
 * @param target_id ID of the target being squashed
 * @param squash_pc PC where the squash occurred
 * @param is_conditional Whether the branch is conditional
 * @param actually_taken Whether the branch was actually taken
 * @param squash_type Type of squash (CTRL/OTHER/TRAP)
 */
void
DecoupledBPUWithBTB::recoverHistoryForSquash(
    FetchTarget &target,
    unsigned target_id,
    const PCStateBase &squash_pc,
    bool is_conditional,
    bool actually_taken,
    SquashType squash_type,
    Addr redirect_pc)
{
    ThreadID tid = target.tid;
    auto& s0History = threads[tid].s0History;
    auto& s0PHistory = threads[tid].s0PHistory;
    auto& s0BwHistory = threads[tid].s0BwHistory;
    auto& s0LHistory = threads[tid].s0LHistory;

    //printf("recover target_id: %u\n", target_id);
    // Restore history from the target
    s0History = target.history;
    s0PHistory = target.phistory;
    s0BwHistory = target.bwhistory;
    s0LHistory = target.lhistory;
    threads[tid].s0PairPhase = target.pairPhase;

    // Get actual history update information.
    const auto ghist_update = target.getGHistUpdateDuringSquash(
        squash_pc.instAddr(), is_conditional, actually_taken);
    const auto bwhist_update = target.getBwHistUpdateDuringSquash(
        squash_pc.instAddr(), is_conditional, actually_taken, redirect_pc);
    const auto phist_update = target.getPHistUpdateDuringSquash(
        squash_pc.instAddr(), actually_taken, redirect_pc);

    // RAS recovers its speculative stack, not folded history.
    if (ras->isEnabled()) {
        ras->recoverState(target);
    }
    if (abtb->isEnabled()) {
        abtb->recoverState(target);
    }

    // Recover component-local folded histories.
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, target, ghist_update.shamt,
                                   ghist_update.taken);
        components[i]->recoverPHist(s0PHistory, target, phist_update);
    }
    if (mgsc->isEnabled()) {
        mgsc->recoverBwHist(s0BwHistory, target, bwhist_update.shamt,
                            bwhist_update.taken);
        mgsc->recoverIHist(target, bwhist_update.shamt,
                           bwhist_update.taken);
        mgsc->recoverLHist(s0LHistory, target, ghist_update.shamt,
                           ghist_update.taken);
    }

    // Update global history with actual outcome
    histShiftIn(ghist_update.shamt, ghist_update.taken, s0History);

    // Update path history with actual outcome
    pHistShiftIn(phist_update.shamt, phist_update.taken, s0PHistory,
                 phist_update.pc, phist_update.target);

    // Update global backward history with actual outcome
    histShiftIn(bwhist_update.shamt, bwhist_update.taken, s0BwHistory);

    // Update local history with actual outcome
    const Addr localHistoryIndex =
        mgsc->getPcIndex(target.startPC,
                         log2(mgsc->getNumEntriesFirstLocalHistories()),
                         target.asidHash);
    histShiftIn(ghist_update.shamt, ghist_update.taken,
                s0LHistory[localHistoryIndex]);

    advancePairPhase(threads[tid].s0PairPhase);

    // Update history manager with appropriate branch info
    if (squash_type == SQUASH_CTRL) {
        historyManagers[tid].squash(target_id, ghist_update,
                                    phist_update,
                                    target.exeBranchInfo);
    } else {
        historyManagers[tid].squash(target_id, ghist_update,
                                    phist_update, BranchInfo());
    }

    // Perform history consistency checks when not a fast build variant
#ifndef NDEBUG
    checkHistories(s0History, s0PHistory, tid);
    if (tage->isEnabled()) {
        tage->checkFoldedHist(
            tage->usesPathHistory() ? s0PHistory : s0History, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (ittage->isEnabled()) {
        ittage->checkFoldedHist(s0PHistory, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (microtage->isEnabled()) {
        microtage->checkFoldedHist(s0PHistory, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (mgsc->isEnabled()) {
        mgsc->checkFoldedHist(s0History, s0PHistory, s0LHistory, tid,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
#endif
}


}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
