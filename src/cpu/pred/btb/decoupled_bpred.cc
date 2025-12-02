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
#include "debug/JumpAheadPredictor.hh"
#include "debug/Override.hh"
#include "debug/Profiling.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

DecoupledBPUWithBTB::DecoupledBPUWithBTB(const DecoupledBPUWithBTBParams &p)
    : BPredUnit(p),
      enableLoopBuffer(p.enableLoopBuffer),
      enableLoopPredictor(p.enableLoopPredictor),
      enableJumpAheadPredictor(p.enableJumpAheadPredictor),
      fetchTargetQueue(p.ftq_size),
      fetchStreamQueueSize(p.fsq_size),
      predictWidth(p.predictWidth),
      maxInstsNum(p.predictWidth / 2),
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
      historyManager(16), // TODO: fix this
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

    predsOfEachStage.resize(numStages);
    for (unsigned i = 0; i < numStages; i++) {
        predsOfEachStage[i].predSource = i;
        clearPreds();
    }

    s0PC = 0x80000000;

    s0History.resize(historyBits, 0);
    s0PHistory.resize(historyBits, 0);
    s0BwHistory.resize(historyBits, 0);
    s0IHistory.resize(historyBits, 0);
    s0LHistory.resize(mgsc->getNumEntriesFirstLocalHistories());
    for (unsigned int i = 0; i < mgsc->getNumEntriesFirstLocalHistories(); ++i) {
        s0LHistory[i].resize(historyBits, 0);
    }
    fetchTargetQueue.setName(name());

    commitHistory.resize(historyBits, 0);
    squashing = true;
    bpuState = BpuState::IDLE;

    lp = LoopPredictor(16, 4, enableLoopDB);
    lb.setLp(&lp);

    jap = JumpAheadPredictor(16, 4);

    if (!enableLoopPredictor && enableLoopBuffer) {
        fatal("loop buffer cannot be enabled without loop predictor\n");
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

    // On squash, reset state if there was a valid prediction.
    if (squashing) {
        bpuState = BpuState::IDLE;
        numOverrideBubbles = 0;
        DPRINTF(Override, "Squashing, BPU state updated.\n");
        squashing = false;
        return;
    }

    // 1. Request new prediction if FSQ not full and we are idle
    if (bpuState == BpuState::IDLE && !streamQueueFull()) {
        requestNewPrediction();
        bpuState = BpuState::PREDICTOR_DONE;
    }

    // 2. Handle pending prediction if available
    if (bpuState == BpuState::PREDICTOR_DONE) {
        DPRINTF(Override, "Generating final prediction for PC %#lx\n", s0PC);
        numOverrideBubbles = generateFinalPredAndCreateBubbles();
        bpuState = BpuState::PREDICTION_OUTSTANDING;

        // Clear each predictor's output
        for (int i = 0; i < numStages; i++) {
            predsOfEachStage[i].btbEntries.clear();
        }
    }

    // 3. Process enqueue operations and bubble counter
    tryEnqFetchTarget();

    // check if:
    // 1. FSQ has space
    // 2. there's no bubble
    // 3. PREDICTION_OUTSTANDING
    if (validateFSQEnqueue()) {
        // Create new FSQ entry with the current prediction
        processNewPrediction(true);

        DPRINTF(Override, "FSQ entry enqueued, prediction state reset\n");
        bpuState = BpuState::IDLE;
    }


    // Decrement override bubbles counter
    if (numOverrideBubbles > 0) {
        numOverrideBubbles--;
        dbpBtbStats.overrideBubbleNum++;
        DPRINTF(Override, "Consuming override bubble, %d remaining\n", numOverrideBubbles);
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
DecoupledBPUWithBTB::requestNewPrediction()
{

        DPRINTF(Override, "Requesting new prediction for PC %#lx\n", s0PC);

        // Initialize prediction state for each stage
        for (int i = 0; i < numStages; i++) {
            predsOfEachStage[i].bbStart = s0PC;
        }

        // Query each predictor component with current PC and history
        for (int i = 0; i < numComponents; i++) {
            components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);  //s0History not used
        }

}

// this function collects predictions from all stages and generate bubbles
// when loop buffer is active, predictions are from saved stream
unsigned
DecoupledBPUWithBTB::generateFinalPredAndCreateBubbles()
{
    DPRINTF(Override, "In generateFinalPredAndCreateBubbles().\n");

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

    // update ubtb using mbtb prediction
    if (predsOfEachStage[numStages - 1].btbEntries.size() > 0) {
        ubtb->updateUsingS3Pred(predsOfEachStage[numStages - 1]);
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
    clearPreds();

    DPRINTF(Override, "Prediction complete: override bubbles=%d\n", first_hit_stage);
    return first_hit_stage;
}

bool
DecoupledBPUWithBTB::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &fetch_target_in_loop)
{
    return fetchTargetQueue.trySupplyFetchWithTarget(fetch_demand_pc, fetch_target_in_loop);
}

/**
 * @brief Interface between fetch stage and branch predictor for instruction prediction
 *
 * This function is called by the fetch stage to get branch prediction information
 * for the current fetch address. It checks the Fetch Target Queue (FTQ) for available
 * prediction entries and returns the prediction result for the current PC.
 *
 * Key operations:
 * 1. Check if a prediction is available in the FTQ
 * 2. Compare current PC against the prediction entry's PC range
 * 3. Determine if current instruction is a predicted branch
 * 4. Update PC for taken branches or advance to next instruction for not-taken
 * 5. Handle entry dequeuing when prediction stream is exhausted
 *
 * @param inst The static instruction being fetched
 * @param seqNum The sequence number of the instruction
 * @param pc Current program counter (updated with prediction target if taken)
 * @param tid Thread ID
 * @param currentLoopIter Reference to current loop iteration counter
 *
 * @return A pair containing:
 *   - First value: Whether the branch is predicted taken
 *   - Second value: Whether we've exhausted the current FTQ entry
 */
std::pair<bool, bool>
DecoupledBPUWithBTB::decoupledPredict(const StaticInstPtr &inst,
                               const InstSeqNum &seqNum, PCStateBase &pc,
                               ThreadID tid, unsigned &currentLoopIter)
{
    DPRINTF(DecoupleBP, "looking up pc %#lx, Supplying target ID %lu\n",
        pc.instAddr(), fetchTargetQueue.getSupplyingTargetId());

    // Check if fetch target queue has prediction available
    auto target_avail = fetchTargetQueue.fetchTargetAvailable();
    if (!target_avail) {
        dbpBtbStats.ftqNotValid++;
        DPRINTF(DecoupleBP,
                "No ftq entry to fetch, return dummy prediction\n");
        // Return (not taken, exhausted entry) to indicate no valid prediction
        return std::make_pair(false, true);
    }

    // Get current prediction entry from FTQ
    const auto &target_to_fetch = fetchTargetQueue.getTarget();
    DPRINTF(DecoupleBP, "Responsing fetch with");
    printFetchTarget(target_to_fetch, "");
    // Extract prediction entry information
    auto start = target_to_fetch.startPC;    // Start of basic block
    auto end = target_to_fetch.endPC;        // End of basic block
    auto taken_pc = target_to_fetch.takenPC; // Branch instruction address if taken
    // Verify current PC is within the predicted entry range
    assert(start <= pc.instAddr() && pc.instAddr() < end);

    // Check if current PC matches predicted branch address and is taken
    bool taken = pc.instAddr() == taken_pc && target_to_fetch.taken;
    bool run_out_of_this_entry = false;
    // Clone PC for potential updates
    std::unique_ptr<PCStateBase> target(pc.clone());

    // Handle taken prediction by updating PC to target address
    if (taken) {
        auto &rtarget = target->as<GenericISA::PCStateWithNext>();
        rtarget.pc(target_to_fetch.target);   // Set new PC to predicted target

        // Set next PC (NPC) for pipeline logic
        rtarget.npc(target_to_fetch.target + 4);
        rtarget.uReset();

        DPRINTF(DecoupleBP,
                "Predicted pc: %#lx, upc: %u, npc(meaningless): %#lx, instSeqNum: %lu\n",
                target->instAddr(), rtarget.upc(), rtarget.npc(), seqNum);

        // Update passed-in PC reference with prediction
        set(pc, *target);

        run_out_of_this_entry = true;
    } else {
        // For not-taken branches or non-branches, advance to next instruction
        inst->advancePC(*target);

        // Check if we've reached the end of the current basic block
        if (target->instAddr() >= end) {
            run_out_of_this_entry = true;
        }
    }

    // Increment instruction counter for current FTQ entry
    currentFtqEntryInstNum++;
    if (run_out_of_this_entry) {
        processFetchTargetCompletion(target_to_fetch);
    }

    DPRINTF(DecoupleBP, "Predict it %staken to %#lx\n", taken ? "" : "not ",
            target->instAddr());

    return std::make_pair(taken, run_out_of_this_entry);
}

/**
 * @brief Process the completion of a fetch target queue entry
 *
 * This function handles the logic when a fetch target queue entry is exhausted:
 * - Dequeues the entry from FTQ
 * - Updates instruction count statistics in the corresponding FSQ entry
 * - Resets instruction counter for the next FTQ entry
 *
 * @param target_to_fetch The FTQ entry being completed
 */
void
DecoupledBPUWithBTB::processFetchTargetCompletion(const FtqEntry &target_to_fetch)
{
    DPRINTF(DecoupleBP, "running out of ftq entry %lu with %d insts\n",
            fetchTargetQueue.getSupplyingTargetId(), currentFtqEntryInstNum);

    // Get stream ID for the current fetch target before removing from FTQ
    const auto fsqId = target_to_fetch.fsqID;

    // Remove the current entry from FTQ
    fetchTargetQueue.finishCurrentFetchTarget();
    // Update instruction count in the fetch stream entry
    auto it = fetchStreamQueue.find(fsqId);
    assert(it != fetchStreamQueue.end());
    it->second.fetchInstNum = currentFtqEntryInstNum;

    // Reset instruction counter for next FTQ entry
    currentFtqEntryInstNum = 0;
}

/**
 * @brief Common logic for handling squash events
 *
 * This function encapsulates the shared logic between different types of squashes:
 * - Setting squashing state
 * - Finding and updating the stream
 * - Recovering history information
 * - Clearing predictions
 * - Updating FTQ and FSQ state
 *
 * @param target_id ID of the target being squashed
 * @param stream_id ID of the stream being squashed
 * @param squash_type Type of squash (CTRL/OTHER/TRAP)
 * @param squash_pc PC where the squash occurred
 * @param redirect_pc PC to redirect to after squash
 * @param is_conditional Whether the squash is caused by a conditional branch
 * @param actually_taken Whether the branch was actually taken (for conditional branches)
 * @param static_inst Static instruction pointer (for control squash)
 * @param control_inst_size Size of the control instruction (for control squash)
 */
void
DecoupledBPUWithBTB::handleSquash(unsigned target_id,
                                 unsigned stream_id,
                                 SquashType squash_type,
                                 const PCStateBase &squash_pc,
                                 Addr redirect_pc,
                                 bool is_conditional,
                                 bool actually_taken,
                                 const StaticInstPtr &static_inst,
                                 unsigned control_inst_size)
{
    // Set squashing state
    squashing = true;

    // Find the stream being squashed
    auto stream_it = fetchStreamQueue.find(stream_id);
    if (stream_it == fetchStreamQueue.end()) {
        assert(!fetchStreamQueue.empty());
        DPRINTF(DecoupleBP, "The squashing stream is insane, ignore squash on it");
        return;
    }

    // Get reference to the stream
    auto &stream = stream_it->second;

    // Update stream state
    stream.resolved = true;
    stream.exeTaken = actually_taken;
    stream.squashPC = squash_pc.instAddr();
    stream.squashType = squash_type;

    // Special handling for control squash - create branch info
    if (squash_type == SQUASH_CTRL && static_inst) {
        // Use full branch info with static_inst if available
        stream.exeBranchInfo = BranchInfo(squash_pc.instAddr(), redirect_pc, static_inst, control_inst_size);
        dumpFsq("Before control squash");
    }

    // Remove streams after the squashed one
    squashStreamAfter(stream_id);

    // Recover history using the extracted function
    recoverHistoryForSquash(stream, stream_id, squash_pc, is_conditional, actually_taken, squash_type, redirect_pc);

    // Clear predictions for next cycle
    clearPreds();

    // Update PC and stream ID
    s0PC = redirect_pc;
    fsqId = stream_id + 1;

    // Squash fetch target queue and redirect to new PC
    fetchTargetQueue.squash(target_id + 1, fsqId, redirect_pc);

    // Additional debugging for control squash
    if (squash_type == SQUASH_CTRL) {
        fetchTargetQueue.dump("After control squash");
    }

    DPRINTF(DecoupleBP,
            "After squash, FSQ head Id=%lu, s0pc=%#lx, demand stream Id=%lu, "
            "Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void
DecoupledBPUWithBTB::controlSquash(unsigned target_id, unsigned stream_id,
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

    auto stream_it = fetchStreamQueue.find(stream_id);
    if (stream_it == fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP, "The squashing stream is insane, ignore squash on it");
        return;
    }
    auto &stream = stream_it->second;
    // Get target address
    Addr real_target = corr_target.instAddr();
    if (!fromCommit && static_inst->isReturn() && !static_inst->isNonSpeculative()) {
        // get ret addr from ras meta
        real_target = ras->getTopAddrFromMetas(stream);
        // TODO: set real target to dynamic inst
    }

    // Detailed debugging for control squash
    DPRINTF(DecoupleBP,
            "Control squash: ftq_id=%d, fsq_id=%d,"
            " control_pc=%#lx, real_target=%#lx, is_conditional=%u, "
            "is_indirect=%u, actually_taken=%u, branch seq: %lu\n",
            target_id, stream_id, control_pc.instAddr(),
            real_target, is_conditional, is_indirect,
            actually_taken, seq);

    // Call shared squash handling logic
    handleSquash(target_id, stream_id, SQUASH_CTRL, control_pc,
                real_target, is_conditional, actually_taken, static_inst, control_inst_size);
}

void
DecoupledBPUWithBTB::nonControlSquash(unsigned target_id, unsigned stream_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.nonControlSquash++;
    DPRINTF(DecoupleBP,
            "non control squash: target id: %d, stream id: %d, inst_pc: %#lx, "
            "seq: %lu\n",
            target_id, stream_id, inst_pc.instAddr(), seq);

    // Call shared squash handling logic
    handleSquash(target_id, stream_id, SQUASH_OTHER, inst_pc, inst_pc.instAddr());
}

void
DecoupledBPUWithBTB::trapSquash(unsigned target_id, unsigned stream_id,
                         Addr last_committed_pc, const PCStateBase &inst_pc,
                         ThreadID tid, const unsigned &currentLoopIter)
{
    dbpBtbStats.trapSquash++;
    DPRINTF(DecoupleBP,
            "Trap squash: target id: %d, stream id: %d, inst_pc: %#lx\n",
            target_id, stream_id, inst_pc.instAddr());

    // Call shared squash handling logic
    handleSquash(target_id, stream_id, SQUASH_TRAP, inst_pc, inst_pc.instAddr());
}

void
DecoupledBPUWithBTB::update(unsigned stream_id, ThreadID tid)
{
    // No need to dequeue when queue is empty
    if (fetchStreamQueue.empty())
        return;

    auto it = fetchStreamQueue.begin();

    // Process all streams that have been committed (stream_id >= stream's id)
    while (it != fetchStreamQueue.end() && stream_id >= it->first) {
        auto &stream = it->second;

        DPRINTF(DecoupleBP,
                "Commit stream start %#lx, which is predicted, "
                "final br addr: %#lx, final target: %#lx, pred br addr: %#lx, "
                "pred target: %#lx\n",
                stream.startPC, stream.exeBranchInfo.pc, stream.exeBranchInfo.target, stream.predBranchInfo.pc,
                stream.predBranchInfo.target);

        // Update statistics
        updateStatistics(stream);

        // Update predictor components
        updatePredictorComponents(stream);

        it = fetchStreamQueue.erase(it);
        dbpBtbStats.fsqEntryCommitted++;
    }

    DPRINTF(DecoupleBP, "after commit stream, fetchStreamQueue size: %lu\n", fetchStreamQueue.size());

    if (it != fetchStreamQueue.end()) {
        printStream(it->second);
    }

    historyManager.commit(stream_id);
}

void
DecoupledBPUWithBTB::resolveUpdate(unsigned &stream_id)
{
    auto stream_it = fetchStreamQueue.find(stream_id);
    if (stream_it == fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP, "Stream id %u not found in fetchStreamQueue, cannot update predictors\n", stream_id);
        return;
    }

    auto &stream = stream_it->second;

    // Update predictor components only if the stream is hit or taken
    if (stream.isHit || stream.exeTaken) {
        // Update predictor components
        for (int i = 0; i < numComponents; ++i) {
            if (components[i]->getResolvedUpdate()) {
                components[i]->update(stream);
            }
        }
    }
}

void
DecoupledBPUWithBTB::prepareResolveUpdateEntries(unsigned &stream_id)
{
    auto stream_it = fetchStreamQueue.find(stream_id);
    if (stream_it == fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP, "Stream id %u not found in fetchStreamQueue, cannot update predictors\n", stream_id);
        return;
    }
    auto &stream = stream_it->second;

    if (stream.isHit || stream.exeTaken) {
        // Prepare stream for update
        stream.setUpdateInstEndPC(predictWidth);
        stream.setUpdateBTBEntries();

        // only mbtb can generate new entry
        if (mbtb->isEnabled()) {
            mbtb->getAndSetNewBTBEntry(stream);
        }
    }
}

void
DecoupledBPUWithBTB::markCFIResolved(unsigned &stream_id, uint64_t resolvedInstPC)
{

    auto stream_it = fetchStreamQueue.find(stream_id);
    if (stream_it == fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP, "Stream id %u not found in fetchStreamQueue, cannot update predictors\n", stream_id);
        return;
    }
    auto &stream = stream_it->second;

    stream.markBTBEntryResolved(resolvedInstPC);
}

void
DecoupledBPUWithBTB::updatePredictorComponents(FetchStream &stream)
{
    // Update predictor components only if the stream is hit or taken
    if (stream.isHit || stream.exeTaken) {
        // Prepare stream for update
        stream.setUpdateInstEndPC(predictWidth);
        stream.setUpdateBTBEntries();

        // only mbtb can generate new entry
        if (mbtb->isEnabled()) {
            mbtb->getAndSetNewBTBEntry(stream);
        }

        // Update predictor components
        for (int i = 0; i < numComponents; ++i) {
            if (!components[i]->getResolvedUpdate()) {
                components[i]->update(stream);
            }
        }
    }
}


void
DecoupledBPUWithBTB::squashStreamAfter(unsigned squash_stream_id)
{
    // Erase all streams after the squashed one
    // upper_bound returns the first element greater than squash_stream_id
    auto erase_it = fetchStreamQueue.upper_bound(squash_stream_id);
    while (erase_it != fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP || erase_it->second.startPC == ObservingPC,
                "Erasing stream %lu when squashing %d\n", erase_it->first,
                squash_stream_id);
        printStream(erase_it->second);
        fetchStreamQueue.erase(erase_it++);
    }
}

bool
DecoupledBPUWithBTB::validateFSQEnqueue()
{
    // Monitor FSQ size for statistics
    dbpBtbStats.fsqEntryDist.sample(fetchStreamQueue.size(), 1);
    if (streamQueueFull()) {
        dbpBtbStats.fsqFullCannotEnq++;
        DPRINTF(Override, "FSQ is full (%lu entries)\n", fetchStreamQueue.size());
        return false;
    }

    // 1. Check if a prediction is available to enqueue
    if (bpuState != BpuState::PREDICTION_OUTSTANDING) {
        DPRINTF(Override, "No prediction available to enqueue into FSQ\n");
        return false;
    }

    // 2. Validate PC value
    if (s0PC == MaxAddr) {
        DPRINTF(DecoupleBP, "Invalid PC value %#lx, cannot make prediction\n", s0PC);
        return false;
    }

    // 3. Check for override bubbles
    // When higher stages override lower stages, bubbles are needed for pipeline consistency
    if (numOverrideBubbles > 0) {
        DPRINTF(Override, "Waiting for %u override bubbles before enqueuing\n", numOverrideBubbles);
        return false;
    }

    // Ensure FSQ has space for the new entry
    assert(!streamQueueFull());
    return true;
}


void
DecoupledBPUWithBTB::setTakenEntryWithStream(FtqEntry &ftq_entry, const FetchStream &stream_entry)
{
    ftq_entry.taken = true;
    ftq_entry.takenPC = stream_entry.getControlPC();
    ftq_entry.target = stream_entry.getTakenTarget();
    ftq_entry.endPC = stream_entry.predEndPC;
}

void
DecoupledBPUWithBTB::setNTEntryWithStream(FtqEntry &ftq_entry, Addr end_pc)
{
    ftq_entry.taken = false;
    ftq_entry.takenPC = 0;
    ftq_entry.target = 0;
    ftq_entry.endPC = end_pc;
}
/**
 * @brief Validate FTQ and FSQ state before enqueueing a fetch target
 *
 * This function checks:
 * 1. If FTQ has space for new entries
 * 2. If FSQ has valid entries
 * 3. If the requested stream exists in the FSQ
 *
 * @return true if validation passes, false otherwise
 */
bool
DecoupledBPUWithBTB::validateFTQEnqueue()
{
    // 1. Check if FTQ can accept new entries
    if (fetchTargetQueue.full()) {
        DPRINTF(DecoupleBP, "Cannot enqueue - FTQ is full\n");
        dbpBtbStats.ftqFullCannotEnq++;
        return false;
    }

    // 2. Check if FSQ has valid entries
    if (fetchStreamQueue.empty()) {
        dbpBtbStats.fsqEmpty++;
        DPRINTF(DecoupleBP, "Cannot enqueue - FSQ is empty\n");
        return false;
    }

    // 3. Get FTQ enqueue state and find corresponding stream
    auto &ftq_enq_state = fetchTargetQueue.getEnqState();
    auto streamIt = fetchStreamQueue.find(ftq_enq_state.streamId);

    if (streamIt == fetchStreamQueue.end()) {
        dbpBtbStats.fsqNotValid++;
        DPRINTF(DecoupleBP, "Cannot enqueue - Stream ID %lu not found in FSQ\n",
                ftq_enq_state.streamId);
        if (streamQueueFull()) {
            // fetch hungry, but fsq is full, cannot enqueue to ftq
            dbpBtbStats.fsqFullFetchHungry++;
        }
        return false;
    }

    // Validation check - warn if FTQ enqueue PC is beyond FSQ end
    if (ftq_enq_state.pc > streamIt->second.predEndPC) {
        warn("Warning: FTQ enqueue PC %#lx is beyond FSQ end %#lx\n",
             ftq_enq_state.pc, streamIt->second.predEndPC);
    }

    return true;
}

/**
 * @brief Creates a FTQ entry from a stream entry at specific PC
 *
 * @param stream The fetch stream to use as source
 * @param ftq_enq_state The fetch target enqueue state to use
 * @return FtqEntry The created fetch target queue entry
 */
FtqEntry
DecoupledBPUWithBTB::createFtqEntryFromStream(
    const FetchStream &stream, const FetchTargetEnqState &ftq_enq_state)
{
    FtqEntry ftq_entry;
    ftq_entry.startPC = ftq_enq_state.pc;
    ftq_entry.fsqID = ftq_enq_state.streamId;

    // Configure based on taken/not-taken
    if (stream.getTaken()) {
        setTakenEntryWithStream(ftq_entry, stream);
    } else {
        setNTEntryWithStream(ftq_entry, stream.predEndPC);
    }

    return ftq_entry;
}

void
DecoupledBPUWithBTB::tryEnqFetchTarget()
{
    DPRINTF(DecoupleBP, "Attempting to enqueue fetch target into FTQ\n");

    // 1. Validate FTQ and FSQ state before proceeding
    if (!validateFTQEnqueue()) {
        return; // Validation failed, cannot proceed
    }

    // 2. Get FTQ enqueue state and find corresponding stream
    auto &ftq_enq_state = fetchTargetQueue.getEnqState();
    auto streamIt = fetchStreamQueue.find(ftq_enq_state.streamId);
    assert(streamIt != fetchStreamQueue.end()); // This should never fail since we validated

    // 3. Get fetch stream and process it
    auto &stream_to_enq = streamIt->second;

    DPRINTF(DecoupleBP, "Processing stream %lu (PC: %#lx)\n",
            streamIt->first, ftq_enq_state.pc);
    printStream(stream_to_enq);

    // 4. Create FTQ entry from stream
    FtqEntry ftq_entry = createFtqEntryFromStream(stream_to_enq, ftq_enq_state);

    // 5. Update FTQ enqueue state for next entry
    ftq_enq_state.pc = ftq_entry.taken ? stream_to_enq.getBranchInfo().target : ftq_entry.endPC;
    ftq_enq_state.streamId++;

    DPRINTF(DecoupleBP, "Updated FTQ state: PC=%#lx, next stream ID=%lu\n",
            ftq_enq_state.pc, ftq_enq_state.streamId);

    // 6. Enqueue the entry and verify state
    fetchTargetQueue.enqueue(ftq_entry);
    assert(ftq_enq_state.streamId <= fsqId + 1);

    // 7. Trace the entry
    if (enablePredFTQTrace) {
        ftqTraceManager->write_record(FtqTrace(ftq_enq_state.nextEnqTargetId-1, ftq_entry.fsqID, ftq_entry));
    }

    // 8. Debug output
    printFetchTarget(ftq_entry, "Insert to FTQ");
    fetchTargetQueue.dump("After insert new entry");
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
 * @brief Creates a new FetchStream entry with prediction information
 *
 * @return FetchStream The created fetch stream
 */
FetchStream
DecoupledBPUWithBTB::createFetchStreamEntry()
{
    // Create a new fetch stream entry
    FetchStream entry;
    entry.startPC = s0PC;

    // Extract branch prediction information
    bool taken = finalPred.isTaken();
    Addr fallThroughAddr = finalPred.getFallThrough(predictWidth);
    Addr nextPC = finalPred.getTarget(predictWidth);

    // Configure stream entry with prediction details
    entry.isHit = !finalPred.btbEntries.empty();
    entry.falseHit = false;
    entry.predBTBEntries = finalPred.btbEntries;
    entry.predTaken = taken;
    entry.predEndPC = fallThroughAddr;

    // Set branch info for taken predictions
    if (taken) {
        entry.predBranchInfo = finalPred.getTakenEntry().getBranchInfo();
        entry.predBranchInfo.target = nextPC; // Use final target (may not be from BTB)
    }

    // Record current history and prediction metadata
    entry.history = s0History;
    entry.phistory = s0PHistory;
    entry.bwhistory = s0BwHistory;
    entry.ihistory = s0IHistory;
    entry.lhistory = s0LHistory;
    entry.predTick = finalPred.predTick;
    entry.predSource = finalPred.predSource;
    entry.overrideReason = finalPred.overrideReason;

    // Save predictors' metadata
    for (int i = 0; i < numComponents; i++) {
        entry.predMetas[i] = components[i]->getPredictionMeta();
    }

    // Initialize default resolution state
    entry.setDefaultResolve();

    return entry;
}

/**
 * @brief fill ahead pipeline entry.previousPCs
 */
void
DecoupledBPUWithBTB::fillAheadPipeline(FetchStream &entry)
{
    // Handle ahead pipelined predictors
    unsigned max_ahead_pipeline_stages = 0;
    for (int i = 0; i < numComponents; i++) {
        max_ahead_pipeline_stages = std::max(max_ahead_pipeline_stages, components[i]->aheadPipelinedStages);
    }

    // Get previous PCs from fetchStreamQueue if needed
    if (max_ahead_pipeline_stages > 0) {
        for (int i = 0; i < max_ahead_pipeline_stages; i++) {
            auto it = fetchStreamQueue.find(fsqId - max_ahead_pipeline_stages + i);
            if (it != fetchStreamQueue.end()) {
                // FIXME: it may not work well with jump ahead predictor
                entry.previousPCs.push(it->second.getRealStartPC());
            }
        }
    }
}

// this function enqueues fsq and update s0PC and s0History
void
DecoupledBPUWithBTB::processNewPrediction(bool create_new_stream)
{
    DPRINTF(DecoupleBP, "Creating new prediction for PC %#lx\n", s0PC);

    // 1. Create a new fetch stream entry with prediction information
    FetchStream entry = createFetchStreamEntry();

    // 2. Update global PC state to target or fall-through
    s0PC = finalPred.getTarget(predictWidth);;

    // 3. Update history information
    updateHistoryForPrediction(entry);

    // 4. Fill ahead pipeline
    fillAheadPipeline(entry);

    // 5. Add entry to fetch stream queue
    auto [insertIt, inserted] = fetchStreamQueue.emplace(fsqId, entry);
    assert(inserted);
    //printf("curr tick: %lu\n", entry.predTick);
    //printf("curr fsqId: %lu\n", fsqId);

    // 6. Record prediction to database if enabled
    if (enablePredFSQTrace) {
        predTraceManager->write_record(PredictionTrace(fsqId, entry));
    }

    // 7. Debug output and update statistics
    dumpFsq("after insert new stream");
    DPRINTF(DecoupleBP, "Inserted fetch stream %lu starting at PC %#lx\n",
            fsqId, entry.startPC);

    // 8. Update FSQ ID and increment statistics
    fsqId++;
    printStream(entry);
    dbpBtbStats.fsqEntryEnqueued++;

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
    s0PC = new_pc;
    fetchTargetQueue.resetPC(new_pc);
}

Addr
DecoupledBPUWithBTB::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    DPRINTF(DecoupleBP, "acquiring reutrn address for inst pc %#lx from decode\n", dynInst->pcState().instAddr());
    auto fsqid = dynInst->getFsqId();
    auto it = fetchStreamQueue.find(fsqid);
    auto retAddr = ras->getTopAddrFromMetas(it->second);
    DPRINTF(DecoupleBP, "get ret addr %#lx\n", retAddr);
    return retAddr;
}

/**
 * @brief Updates global history based on prediction results
 *
 * @param entry The fetch stream entry to update history for
 */
void
DecoupledBPUWithBTB::updateHistoryForPrediction(FetchStream &entry)
{
    // Update component-specific history, for TAGE/ITTAGE/MGSC
    for (int i = 0; i < numComponents; i++) {
        // use old s0History to update folded history, then use finalPred to update folded history
        components[i]->specUpdateHist(s0History, finalPred);
        if(components[i]->needMoreHistories){
            components[i]->specUpdatePHist(s0PHistory, finalPred);
            components[i]->specUpdateBwHist(s0BwHistory, finalPred);
            components[i]->specUpdateIHist(s0IHistory, finalPred);
            components[i]->specUpdateLHist(s0LHistory, finalPred);
        }
    }

    // Get prediction information for history updates
    int shamt;
    bool taken;
    std::tie(shamt, taken) = finalPred.getHistInfo();

    // Update global history
    histShiftIn(shamt, taken, s0History);

    // Update history manager and verify TAGE folded history
    historyManager.addSpeculativeHist(
        entry.startPC, shamt, taken, entry.predBranchInfo, fsqId);

    // Get prediction information for global backward history updates
    int bw_shamt;
    bool bw_taken;
    std::tie(bw_shamt, bw_taken) = finalPred.getBwHistInfo();

    // Get prediction information for path history updates
    auto [p_pc, p_target, p_taken]= finalPred.getPHistInfo(); // p_taken = taken

    // Update global backward history
    histShiftIn(bw_shamt, bw_taken, s0BwHistory);

    // Update path history
    pHistShiftIn(2, p_taken, s0PHistory, p_pc, p_target);

    // Update imli history
    histShiftIn(bw_shamt, bw_taken, s0IHistory);  //s0IHistory is not used

    // Update local history
    histShiftIn(shamt, taken,
        s0LHistory[mgsc->getPcIndex(finalPred.bbStart, log2(mgsc->getNumEntriesFirstLocalHistories()))]);

#ifndef NDEBUG
    if (tage->isEnabled()) {
        tage->checkFoldedHist(s0PHistory, "speculative update");
    }
    if (ittage->isEnabled()) {
        ittage->checkFoldedHist(s0PHistory, "speculative update");
    }
    if (microtage->isEnabled()) {
        microtage->checkFoldedHist(s0PHistory, "speculative update");
    }
    if (mgsc->isEnabled()) {
        mgsc->checkFoldedHist(s0History, s0PHistory, s0LHistory, "speculative update");
    }
#endif
}

/**
 * @brief Recovers branch history during a squash event
 *
 * @param stream The stream being squashed
 * @param stream_id ID of the stream being squashed
 * @param squash_pc PC where the squash occurred
 * @param is_conditional Whether the branch is conditional
 * @param actually_taken Whether the branch was actually taken
 * @param squash_type Type of squash (CTRL/OTHER/TRAP)
 */
void
DecoupledBPUWithBTB::recoverHistoryForSquash(
    FetchStream &stream,
    unsigned stream_id,
    const PCStateBase &squash_pc,
    bool is_conditional,
    bool actually_taken,
    SquashType squash_type,
    Addr redirect_pc)
{
    //printf("recover stream_id: %u\n", stream_id);
    // Restore history from the stream
    s0History = stream.history;
    s0PHistory = stream.phistory;
    s0BwHistory = stream.bwhistory;
    s0IHistory = stream.ihistory;
    s0LHistory = stream.lhistory;

    // Get actual history shift information
    int real_shamt;
    bool real_taken;
    std::tie(real_shamt, real_taken) = stream.getHistInfoDuringSquash(
        squash_pc.instAddr(), is_conditional, actually_taken);

    // Get actual history shift information
    int real_bw_shamt;
    bool real_bw_taken;
    std::tie(real_bw_shamt, real_bw_taken) = stream.getBwHistInfoDuringSquash(
    squash_pc.instAddr(), is_conditional, actually_taken, redirect_pc);

    // Recover component-specific history
    for (int i = 0; i < numComponents; ++i) {
        components[i]->recoverHist(s0History, stream, real_shamt, real_taken);
        if(components[i]->needMoreHistories){
            components[i]->recoverPHist(s0PHistory, stream, real_shamt, real_taken);
            components[i]->recoverBwHist(s0BwHistory, stream, real_bw_shamt, real_bw_taken);
            components[i]->recoverIHist(s0IHistory, stream, real_bw_shamt, real_bw_taken); //s0IHistory is not used
            components[i]->recoverLHist(s0LHistory, stream, real_shamt, real_taken);
        }
    }

    // Update global history with actual outcome
    histShiftIn(real_shamt, real_taken, s0History);

    // Update path history with actual outcome
    pHistShiftIn(2, real_taken, s0PHistory, squash_pc.instAddr(), redirect_pc);

    // Update global backward history with actual outcome
    histShiftIn(real_bw_shamt, real_bw_taken, s0BwHistory);

    // Update imli history with actual outcome
    histShiftIn(real_bw_shamt, real_bw_taken, s0IHistory);  //s0IHistory is not used

    // Update local history with actual outcome
    histShiftIn(real_shamt, real_taken,
                s0LHistory[mgsc->getPcIndex(stream.startPC, log2(mgsc->getNumEntriesFirstLocalHistories()))]);

    // Update history manager with appropriate branch info
    if (squash_type == SQUASH_CTRL) {
        historyManager.squash(stream_id, real_shamt, real_taken, stream.exeBranchInfo);
    } else {
        historyManager.squash(stream_id, real_shamt, real_taken, BranchInfo());
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
