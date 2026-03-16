#include "cpu/pred/btb/decoupled_bpred.hh"

#include <array>

#include "base/debug_helper.hh"
#include "base/output.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/folded_hist.hh"
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

namespace
{

const FullBTBPrediction &
latestStagePrediction(const std::vector<FullBTBPrediction> &stagePreds)
{
    for (int i = (int)stagePreds.size() - 1; i >= 0; --i) {
        if (!stagePreds[i].btbEntries.empty() || !stagePreds[i].condTakens.empty() ||
            !stagePreds[i].indirectTargets.empty() || stagePreds[i].returnTarget != 0) {
            return stagePreds[i];
        }
    }
    return stagePreds[0];
}

}  // namespace

void
DecoupledBPUWithBTB::consumeFetchTarget(unsigned fetched_inst_num, ThreadID tid)
{
    ftq.fetching(tid).fetchInstNum = fetched_inst_num;
    ftq.finishTarget(tid);
}

DecoupledBPUWithBTB::DecoupledBPUWithBTB(const DecoupledBPUWithBTBParams &p)
    : BPredUnit(p),

      predictWidth(p.predictWidth),
      maxInstsNum(p.predictWidth / 2),
      enableTwoTaken(p.enableTwoTaken),
      dropBlock1OnBlock0Override(p.dropBlock1OnBlock0Override),
      dropBlock1WhenFTQHasOnlyOneSlot(p.dropBlock1WhenFTQHasOnlyOneSlot),
      dropBlock1OnCondWithoutTage(p.dropBlock1OnCondWithoutTage),
      dropBlock1OnIndirectWithoutIttage(p.dropBlock1OnIndirectWithoutIttage),
      dropBlock1OnReturnWithoutRas(p.dropBlock1OnReturnWithoutRas),
      historyBits(p.maxHistLen),
      ubtb(p.ubtb),
      abtb(p.abtb),
      mbtb(p.mbtb),
      microtage(p.microtage),
      tage(p.tage),
      ittage(p.ittage),
      mgsc(p.mgsc),
      ras(p.ras),
      // uras(p.uras),
      bpDBSwitches(p.bpDBSwitches),
      numStages(p.numStages),
      ftq(2, p.ftq_size),
      historyManager(16), // TODO: fix this
      resolveBlockThreshold(p.resolveBlockThreshold),
      dbpBtbStats(this, p.numStages, p.fsq_size, maxInstsNum)
{
    if (bpDBSwitches.size() > 0) {
        initDB();
    }
    bpType = DecoupledBTBType;
    // Only add enabled components to the list
    if (ubtb->isEnabled()) components.push_back(ubtb);
    if (abtb->isEnabled()) components.push_back(abtb);
    if (microtage->isEnabled()) components.push_back(microtage);
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


void
DecoupledBPUWithBTB::tick()
{
    DPRINTF(Override, "DecoupledBPUWithBTB::tick()\n");

    ThreadID curTid = scheduleThread();

    // On squash, reset state if there was a valid prediction.
    bool squashOccurred = false;
    for (int tid = 0; tid < numThreads; tid++) {
        if (threads[tid].squashing) {
            if (tid == curTid) {
                squashOccurred = true;
            }
            threads[tid].validprediction = false;
            threads[tid].numOverrideBubbles = 0;
            tage->dryRunCycle(threads[tid].s0PC);
            DPRINTF(Override, "Squashing, BPU state updated.\n");
            threads[tid].squashing = false;
        }
    }

    if (squashOccurred) {
        DPRINTF(Override, "Squash occurred for current thread, skip predict.\n");
        return;
    }

    // 1. Request new prediction if FSQ not full and we are idle
    if (!threads[curTid].validprediction && !ftq.full(curTid)) {
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

    DPRINTF(Override, "Requesting new prediction for PC %#lx\n", thread.s0PC);


    // Initialize prediction state for each stage
    for (int i = 0; i < numStages; i++) {
        predsOfEachStage[i].tid = tid;
        predsOfEachStage[i].bbStart = thread.s0PC;
    }

    // Query each predictor component with current PC and history
    for (int i = 0; i < numComponents; i++) {
        components[i]->putPCHistory(thread.s0PC, thread.s0History, predsOfEachStage);  //s0History not used
    }

    generateFinalPredAndCreateBubbles(tid);

    buildPredictionBundle(tid);

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
        if (predsOfEachStage[i].btbEntries.size() > 0) {
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
    if (predsOfEachStage[numStages - 1].btbEntries.size() > 0) {
        if (ubtb->isEnabled()) {
            ubtb->updateUsingS3Pred(predsOfEachStage[numStages - 1]);
        }
        if (abtb->isEnabled() && ftq.backId(tid)) {
            auto previous_block_startpc = ftq.back(tid).startPC;
            abtb->updateUsingS3Pred(predsOfEachStage[numStages - 1], previous_block_startpc);
        } else if (abtb->isEnabled()) {
            abtb->updateUsingS3Pred(predsOfEachStage[numStages - 1], 0);
        }
    }

    // 4. Record override bubbles and update statistics
    if (first_hit_stage > 0) {
        dbpBtbStats.overrideCount++;
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
    if (ftq.full(tid)) {
        dbpBtbStats.fsqFullCannotEnq++;
        DPRINTF(Override, "FSQ is full (%lu entries)\n", ftq.size(tid));
        return;
    }

    auto &bundle = threads[tid].bundle;
    auto initialState = makeSpecState(tid);

    // Validate PC value
    if (initialState.pc == MaxAddr) {
        DPRINTF(DecoupleBP, "Invalid PC value %#lx, cannot make prediction\n", initialState.pc);
        return;
    }

    DPRINTF(DecoupleBP, "Creating new prediction for PC %#lx\n", initialState.pc);

    // 1. Create a new fetch target entry with prediction information
    FetchTarget entry = createFetchTargetEntry(tid, initialState, bundle.pred0, bundle.pred0Metas);

    // 4. Fill ahead pipeline
    fillAheadPipeline(entry);

    if (enablePredFSQTrace) {
        predTraceManager->write_record(PredictionTrace(ftq.backId(tid), entry));
    }

    // 5. Add entry to fetch target queue
    ftq.insert(entry);
    dbpBtbStats.fsqEntryEnqueued++;

    if (bundle.hasPred1) {
        FetchTarget entry1 = createFetchTargetEntry(tid, bundle.stateAfter0, bundle.pred1, bundle.pred1Metas);
        fillAheadPipeline(entry1);
        if (enablePredFSQTrace) {
            predTraceManager->write_record(PredictionTrace(ftq.backId(tid), entry1));
        }
        ftq.insert(entry1);
        printTarget(entry1);
        dbpBtbStats.fsqEntryEnqueued++;
    }

    commitSpecState(tid, bundle.stateAfterFinal);
    threads[tid].validprediction = false;

    // 6. Debug output and update statistics
    dumpFsq("after insert new target");
    DPRINTF(DecoupleBP, "Inserted fetch target %lu starting at PC %#lx\n",
            ftq.backId(tid), entry.startPC);

    // 7. Increment statistics
    printTarget(entry);
}

DecoupledBPUWithBTB::SpecState
DecoupledBPUWithBTB::makeSpecState(ThreadID tid) const
{
    SpecState state;
    state.pc = threads[tid].s0PC;
    state.history = threads[tid].s0History;
    state.phistory = threads[tid].s0PHistory;
    state.bwhistory = threads[tid].s0BwHistory;
    state.lhistory = threads[tid].s0LHistory;
    return state;
}

void
DecoupledBPUWithBTB::commitSpecState(ThreadID tid, const SpecState &state)
{
    threads[tid].s0PC = state.pc;
    threads[tid].s0History = state.history;
    threads[tid].s0PHistory = state.phistory;
    threads[tid].s0BwHistory = state.bwhistory;
    threads[tid].s0LHistory = state.lhistory;
}

std::array<std::shared_ptr<void>, 8>
DecoupledBPUWithBTB::capturePredictionMetas() const
{
    std::array<std::shared_ptr<void>, 8> predMetas{};
    predMetas.fill(nullptr);
    for (int i = 0; i < numComponents; i++) {
        predMetas[i] = components[i]->getPredictionMeta();
    }
    return predMetas;
}

void
DecoupledBPUWithBTB::buildPredictionBundle(ThreadID tid)
{
    auto &thread = threads[tid];
    auto &bundle = thread.bundle;
    auto initialState = makeSpecState(tid);

    bundle.pred0 = thread.finalPred;
    bundle.pred0Metas = capturePredictionMetas();
    bundle.hasPred1 = false;
    bundle.pred1 = FullBTBPrediction();
    bundle.pred1Metas.fill(nullptr);
    bundle.pred1DropReason = getInitialBlock1DropReason(
        enableTwoTaken, dropBlock1OnBlock0Override, thread.finalPred.overrideReason != OverrideReason::NO_OVERRIDE,
        dropBlock1WhenFTQHasOnlyOneSlot, ftq.freeSlots(tid), thread.finalPred.getTarget(predictWidth),
        !thread.predsOfEachStage.empty() && !thread.predsOfEachStage[0].btbEntries.empty(), MaxAddr);
    bundle.stateAfter0 = computeNextSpecState(tid, initialState, bundle.pred0, ftq.backId(tid) + 1);
    bundle.stateAfterFinal = bundle.stateAfter0;

    if (enableTwoTaken) {
        dbpBtbStats.block1Attempted++;
    }

    if (bundle.pred1DropReason != Block1DropReason::None) {
        switch (bundle.pred1DropReason) {
            case Block1DropReason::Block0Override:
                dbpBtbStats.block1DroppedByBlock0Override++;
                break;
            case Block1DropReason::FTQNoSpace:
                dbpBtbStats.block1DroppedByFTQFull++;
                break;
            default:
                dbpBtbStats.block1DroppedOther++;
                break;
        }
        return;
    }

    std::vector<FullBTBPrediction> block1StagePreds(numStages);
    for (int i = 0; i < numStages; i++) {
        block1StagePreds[i].tid = tid;
        block1StagePreds[i].bbStart = bundle.stateAfter0.pc;
    }

    FullBTBPrediction lowerPred;
    lowerPred.tid = tid;
    lowerPred.bbStart = bundle.stateAfter0.pc;
    for (int i = 0; i < numComponents; i++) {
        components[i]->putPCHistoryForBlock1(bundle.stateAfter0.pc, bundle.stateAfter0.history,
                                             bundle.stateAfter0.phistory, bundle.stateAfter0.bwhistory,
                                             bundle.stateAfter0.lhistory, block1StagePreds, lowerPred);
        lowerPred = latestStagePrediction(block1StagePreds);
    }

    FullBTBPrediction *chosenPrediction = &block1StagePreds[0];
    for (int i = (int)numStages - 1; i >= 0; i--) {
        if (!block1StagePreds[i].btbEntries.empty()) {
            chosenPrediction = &block1StagePreds[i];
            break;
        }
    }

    bundle.pred1 = *chosenPrediction;
    unsigned first_hit_stage = 0;
    OverrideReason overrideReason = OverrideReason::NO_OVERRIDE;
    while (first_hit_stage < numStages - 1) {
        auto [matches, reason] = block1StagePreds[first_hit_stage].match(*chosenPrediction, predictWidth);
        if (matches) {
            break;
        }
        first_hit_stage++;
        overrideReason = reason;
    }
    bundle.pred1.predSource = first_hit_stage;
    bundle.pred1.overrideReason = overrideReason;

    bool hasCondDirectionSupport = tage->participatesInBlock1() || !bundle.pred1.condTakens.empty();
    bool hasIndirectTargetSupport = ittage->participatesInBlock1() || !bundle.pred1.indirectTargets.empty();
    bool hasReturnTargetSupport = ras->participatesInBlock1() || bundle.pred1.returnTarget != 0;
    bundle.pred1DropReason = getBlock1DropReason(bundle.pred1, dropBlock1OnCondWithoutTage, hasCondDirectionSupport,
                                                 dropBlock1OnIndirectWithoutIttage, hasIndirectTargetSupport,
                                                 dropBlock1OnReturnWithoutRas, hasReturnTargetSupport);
    if (bundle.pred1DropReason != Block1DropReason::None || bundle.pred1.btbEntries.empty()) {
        if (bundle.pred1DropReason == Block1DropReason::None) {
            bundle.pred1DropReason = Block1DropReason::PredictorRejected;
        }
        switch (bundle.pred1DropReason) {
            case Block1DropReason::CondNoSupport:
                dbpBtbStats.block1DroppedByCond++;
                break;
            case Block1DropReason::IndirectNoSupport:
                dbpBtbStats.block1DroppedByIndirect++;
                break;
            case Block1DropReason::ReturnNoSupport:
                dbpBtbStats.block1DroppedByReturn++;
                break;
            default:
                dbpBtbStats.block1DroppedOther++;
                break;
        }
        bundle.pred1 = FullBTBPrediction();
        bundle.stateAfterFinal = bundle.stateAfter0;
        return;
    }

    bundle.pred1Metas = capturePredictionMetas();
    bundle.hasPred1 = true;
    dbpBtbStats.block1Accepted++;
    bundle.stateAfterFinal = computeNextSpecState(tid, bundle.stateAfter0, bundle.pred1, ftq.backId(tid) + 2);
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

    // Find the target being squashed
    if (!ftq.hasTarget(target_id, tid)) {
        assert(!ftq.empty(tid));
        DPRINTF(DecoupleBP, "The squashing target is insane, ignore squash on it");
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

    historyManager.commit(target_id);
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
DecoupledBPUWithBTB::notifyResolveSuccess()
{
    resolveDequeueFailCounter = 0;
}

void
DecoupledBPUWithBTB::notifyResolveFailure()
{
    resolveDequeueFailCounter++;
    if (resolveDequeueFailCounter >= resolveBlockThreshold) {
        blockPredictionOnce();
        resolveDequeueFailCounter = 0;
    }
}

void
DecoupledBPUWithBTB::blockPredictionOnce()
{
    // smtTODO
    threads[0].blockPredictionPending = true;
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
DecoupledBPUWithBTB::SpecState
DecoupledBPUWithBTB::computeNextSpecState(ThreadID tid, const SpecState &state, const FullBTBPrediction &pred,
                                          FetchTargetId targetId)
{
    SpecState next = state;
    auto predCopy = pred;

    for (int i = 0; i < numComponents; i++) {
        components[i]->specUpdateHist(next.history, predCopy);
        if (components[i]->needMoreHistories) {
            components[i]->specUpdatePHist(next.phistory, predCopy);
            components[i]->specUpdateBwHist(next.bwhistory, predCopy);
            components[i]->specUpdateIHist(predCopy);
            components[i]->specUpdateLHist(next.lhistory, predCopy);
        }
    }

    auto [shamt, taken] = predCopy.getHistInfo();
    histShiftIn(shamt, taken, next.history);
    BranchInfo histBranchInfo;
    if (predCopy.isTaken()) {
        histBranchInfo = predCopy.getTakenEntry().getBranchInfo();
    }
    historyManager.addSpeculativeHist(state.pc, shamt, taken, histBranchInfo, targetId);

    auto [bw_shamt, bw_taken] = predCopy.getBwHistInfo();
    auto [p_pc, p_target, p_taken] = predCopy.getPHistInfo();
    histShiftIn(bw_shamt, bw_taken, next.bwhistory);
    pHistShiftIn(2, p_taken, next.phistory, p_pc, p_target);
    histShiftIn(shamt, taken,
                next.lhistory[mgsc->getPcIndex(predCopy.bbStart, log2(mgsc->getNumEntriesFirstLocalHistories()))]);

    next.pc = predCopy.getTarget(predictWidth);
    return next;
}

FetchTarget
DecoupledBPUWithBTB::createFetchTargetEntry(ThreadID tid, const SpecState &state, const FullBTBPrediction &pred,
                                            const std::array<std::shared_ptr<void>, 8> &predMetas)
{
    auto predCopy = pred;
    // Create a new fetch target entry
    FetchTarget entry;
    entry.tid = tid;
    entry.startPC = state.pc;

    // Extract branch prediction information
    bool taken = predCopy.isTaken();
    Addr fallThroughAddr = predCopy.getFallThrough(predictWidth);
    Addr nextPC = predCopy.getTarget(predictWidth);

    // Configure target entry with prediction details
    entry.isHit = !predCopy.btbEntries.empty();
    entry.falseHit = false;
    entry.predBTBEntries = predCopy.btbEntries;
    entry.predTaken = taken;
    entry.predEndPC = fallThroughAddr;

    // Set branch info for taken predictions
    if (taken) {
        entry.predBranchInfo = predCopy.getTakenEntry().getBranchInfo();
        entry.predBranchInfo.target = nextPC;  // Use final target (may not be from BTB)
    }

    // Record current history and prediction metadata
    entry.history = state.history;
    entry.phistory = state.phistory;
    entry.bwhistory = state.bwhistory;
    entry.lhistory = state.lhistory;
    entry.predTick = predCopy.predTick;
    entry.predSource = predCopy.predSource;
    entry.overrideReason = predCopy.overrideReason;

    entry.s1Source = predCopy.s1Source;
    entry.s3Source = predCopy.s3Source;

    // Save predictors' metadata
    entry.predMetas = predMetas;

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
DecoupledBPUWithBTB::checkHistory(const boost::dynamic_bitset<> &history)
{
    // This function performs a crucial validation of branch history consistency
    // It rebuilds the "ideal" history from HistoryManager's records and compares
    // it with the actual history being used by the branch predictor

    // Initialize counter for total history bits and a bitset for rebuilt history
    unsigned ideal_size = 0;
    boost::dynamic_bitset<> ideal_hash_hist(historyBits, 0);

    // Iterate through all speculative history entries stored in HistoryManager
    for (const auto entry: historyManager.getSpeculativeHist()) {
        // Only process entries that have non-zero shift amount (actual branches)
        if (entry.shamt != 0) {
            // Accumulate total history bits
            ideal_size += entry.shamt;
            DPRINTF(DecoupleBPVerbose, "pc: %#lx, shamt: %lu, cond_taken: %d\n", entry.pc,
                    entry.shamt, entry.cond_taken);

            // Rebuild history by shifting and setting bits based on recorded outcomes
            // This emulates how history would be built if all branches were predicted perfectly
            ideal_hash_hist <<= entry.shamt;
            ideal_hash_hist[0] = entry.cond_taken;
        }
    }

    // Determine how many bits to compare (minimum of ideal size and actual history bits)
    unsigned comparable_size = std::min(ideal_size, historyBits);

    // Prepare actual history for comparison by creating a copy
    boost::dynamic_bitset<> sized_real_hist(history);

    // Resize both histories to the comparable size for accurate comparison
    ideal_hash_hist.resize(comparable_size);
    sized_real_hist.resize(comparable_size);

    // boost::to_string(ideal_hash_hist, buf1);
    // boost::to_string(sized_real_hist, buf2);
    DPRINTF(DecoupleBP,
            "Ideal size:\t%u, real history size:\t%u, comparable size:\t%u\n",
            ideal_size, historyBits, comparable_size);
    // DPRINTF(DecoupleBP, "Ideal history:\t%s\nreal history:\t%s\n",
    //         buf1.c_str(), buf2.c_str());

    assert(ideal_hash_hist == sized_real_hist);
}

void
DecoupledBPUWithBTB::resetPC(Addr new_pc)
{
    for (int i = 0; i < numThreads; i++)
        threads[i].s0PC = new_pc;
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

    // Get actual history shift information
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = target.getHistInfoDuringSquash(
        squash_pc.instAddr(), is_conditional, actually_taken);

    // Get actual history shift information
    int real_bw_shamt;
    bool real_bw_taken;
    std::tie(real_bw_shamt, real_bw_taken) = target.getBwHistInfoDuringSquash(
    squash_pc.instAddr(), is_conditional, actually_taken, redirect_pc);

    // Recover component-specific history
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, target, real_shamt, real_taken);
        if (components[i]->needMoreHistories){
            components[i]->recoverPHist(s0PHistory, target, real_shamt, real_taken);
            components[i]->recoverBwHist(s0BwHistory, target, real_bw_shamt, real_bw_taken);
            components[i]->recoverIHist(target, real_bw_shamt, real_bw_taken);
            components[i]->recoverLHist(s0LHistory, target, real_shamt, real_taken);
        }
    }

    // Update global history with actual outcome
    histShiftIn(real_shamt, real_taken, s0History);

    // Update path history with actual outcome
    pHistShiftIn(2, real_taken, s0PHistory, squash_pc.instAddr(), redirect_pc);

    // Update global backward history with actual outcome
    histShiftIn(real_bw_shamt, real_bw_taken, s0BwHistory);

    // Update local history with actual outcome
    histShiftIn(real_shamt, real_taken,
                s0LHistory[mgsc->getPcIndex(target.startPC, log2(mgsc->getNumEntriesFirstLocalHistories()))]);

    // Update history manager with appropriate branch info
    if (squash_type == SQUASH_CTRL) {
        historyManager.squash(target_id, real_shamt, real_taken, target.exeBranchInfo);
    } else {
        historyManager.squash(target_id, real_shamt, real_taken, BranchInfo());
    }

    // Perform history consistency checks when not a fast build variant
#ifndef NDEBUG
    checkHistory(s0History);
    if (tage->isEnabled()) {
        tage->checkFoldedHist(s0PHistory,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (ittage->isEnabled()) {
        ittage->checkFoldedHist(s0PHistory,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (microtage->isEnabled()) {
        microtage->checkFoldedHist(s0PHistory,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
    if (mgsc->isEnabled()) {
        mgsc->checkFoldedHist(s0History, s0PHistory, s0LHistory,
            squash_type == SQUASH_CTRL ? "control squash" :
            squash_type == SQUASH_OTHER ? "non control squash" : "trap squash");
    }
#endif
}


}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
