#include "cpu/pred/ftb/decoupled_bpred.hh"

#include "base/output.hh"
#include "base/debug_helper.hh"
#include "cpu/pred/ftb/stream_common.hh"
#include "debug/DecoupleBPVerbose.hh"
#include "debug/DecoupleBPHist.hh"
#include "debug/Override.hh"
#include "sim/core.hh"

namespace gem5
{
namespace branch_prediction
{
namespace ftb_pred
{

DecoupledBPUWithFTB::DecoupledBPUWithFTB(const DecoupledBPUWithFTBParams &p)
    : BPredUnit(p),
      fetchTargetQueue(p.ftq_size),
      historyBits(p.maxHistLen),
      ftb(p.ftb)
    //   streamTAGE(p.stream_tage),
    //   streamUBTB(p.stream_ubtb),
    //   dumpLoopPred(p.dump_loop_pred),
    //   loopDetector(p.loop_detector),
    //   streamLoopPredictor(p.stream_loop_predictor)
{
    // assert(streamTAGE);
    // assert(streamUBTB);
    bpType = DecoupledFTBType;
    numComponents = 1;
    numStages = 3;
    components[0] = ftb;
    // components[0] = streamUBTB;
    // components[1] = streamTAGE;

    // TODO: remove this
    fetchStreamQueueSize = 64;
    s0PC = 0x80000000;
    // s0StreamStartPC = s0PC;

    s0History.resize(historyBits, 0);
    fetchTargetQueue.setName(name());

    commitHistory.resize(historyBits, 0);
    squashing = true;

    // loopDetector->setStreamLoopPredictor(streamLoopPredictor);

    // streamTAGE->setStreamLoopPredictor(streamLoopPredictor);

    // registerExitCallback([this]() {
    //     auto out_handle = simout.create("topMisPredicts.txt", false, true);
    //     *out_handle->stream() << "startPC" << " " << "control pc" << " " << "count" << std::endl;
    //     std::vector<std::pair<std::pair<Addr, Addr>, int>> topMisPredPC;
    //     for (auto &it : topMispredicts) {
    //         topMisPredPC.push_back(it);
    //     }
    //     std::sort(topMisPredPC.begin(), topMisPredPC.end(), [](const std::pair<std::pair<Addr, Addr>, int> &a, const std::pair<std::pair<Addr, Addr>, int> &b) {
    //         return a.second > b.second;
    //     });
    //     for (auto& it : topMisPredPC) {
    //         *out_handle->stream() << std::hex << it.first.first << " " << it.first.second << " " << std::dec << it.second << std::endl;
    //     }
    //     simout.close(out_handle);

    //     out_handle = simout.create("topMisPredictHist.txt", false, true);
    //     *out_handle->stream() << "use loop but invalid: " << useLoopButInvalid 
    //                           << " use loop and valid: " << useLoopAndValid 
    //                           << " not use loop: " << notUseLoop << std::endl;
    //     *out_handle->stream() << "Hist" << " " << "count" << std::endl;
    //     std::vector<std::pair<uint64_t, uint64_t>> topMisPredHistVec;
    //     for (const auto &entry: topMispredHist) {
    //         topMisPredHistVec.push_back(entry);
    //     }
    //     std::sort(topMisPredHistVec.begin(), topMisPredHistVec.end(),
    //               [](const std::pair<uint64_t, uint64_t> &a,
    //                  const std::pair<uint64_t, uint64_t> &b) {
    //                   return a.second > b.second;
    //               });
    //     for (const auto &entry: topMisPredHistVec) {
    //         *out_handle->stream() << std::hex << entry.first << " " << std::dec << entry.second << std::endl;
    //     }

    //     if (dumpLoopPred) {
    //         out_handle = simout.create("misPredTripCount.txt", false, true);
    //         *out_handle->stream() << missCount << std::endl;
    //         for (const auto &entry : misPredTripCount) {
    //             *out_handle->stream()
    //                 << entry.first << " " << entry.second << std::endl;
    //         }

    //         out_handle = simout.create("loopInfo.txt", false, true);
    //         for (const auto &entry : storedLoopStreams) {
    //             bool misPred = entry.second.squashType == SQUASH_CTRL;
    //             *out_handle->stream()
    //                 << std::dec << "miss: " << misPred << " " << entry.first << " "
    //                 << std::hex << entry.second.startPC << ", "
    //                 << (misPred ? entry.second.exeBranchPC
    //                             : entry.second.predBranchPC)
    //                 << "--->"
    //                 << (misPred ? entry.second.exeTarget : entry.second.predTarget)
    //                 << std::dec
    //                 << " useLoopPred: " << entry.second.useLoopPrediction
    //                 << " tripCount: " << entry.second.tripCount << std::endl;
    //         }
    //     }

    //     out_handle = simout.create("targets.txt", false, true);
    //     for (const auto it : storeTargets) {
    //         *out_handle->stream() << std::hex << it << std::endl;
    //     }

    //     simout.close(out_handle);
    // });
}

// bool
// DecoupledBPUWithFTB::useStreamRAS(FetchStreamId stream_id)
// {
//     if (finalPred.isCall()) {
//         pushRAS(stream_id, "speculative update", finalPred.getFallThruPC());
//         dumpRAS();
//         return true;
//     } else if (finalPred.isReturn()) {
//         popRAS(stream_id, "speculative update");
//         dumpRAS();
//         return true;
//     }
//     return false;
// }

void
DecoupledBPUWithFTB::tick()
{
    if (!receivedPred && numOverrideBubbles == 0 && sentPCHist) {
        generateFinalPredAndCreateBubbles();
    }
    if (!squashing) {
        DPRINTF(DecoupleBP, "DecoupledBPUWithFTB::tick()\n");
        DPRINTF(Override, "DecoupledBPUWithFTB::tick()\n");
        tryEnqFetchTarget();
        tryEnqFetchStream();
    } else {
        receivedPred = false;
        DPRINTF(DecoupleBP, "Squashing, skip this cycle, receivedPred is %d.\n", receivedPred);
        DPRINTF(Override, "Squashing, skip this cycle, receivedPred is %d.\n", receivedPred);
    }

    if (numOverrideBubbles > 0) {
        numOverrideBubbles--;
    }

    sentPCHist = false;

    // streamTAGE->tickStart();
    // streamUBTB->tickStart();

    // TODO: ftq
    if (!receivedPred && !streamQueueFull()) {
        if (s0PC == ObservingPC) {
            DPRINTFV(true, "Predicting block %#lx, id: %lu\n", s0PC, fsqId);
        }
        DPRINTF(DecoupleBP, "Requesting prediction for stream start=%#lx\n", s0PC);
        DPRINTF(Override, "Requesting prediction for stream start=%#lx\n", s0PC);
        // put startAddr in preds
        for (int i = 0; i < numStages; i++) {
            predsOfEachStage[i].bbStart = s0PC;
        }
        for (int i = 0; i < numComponents; i++) {
            components[i]->putPCHistory(s0PC, s0History, predsOfEachStage);
        }
        sentPCHist = true;
    }
    
    if (streamQueueFull()) {
        DPRINTF(DecoupleBP, "Stream queue is full, don't request prediction\n");
        DPRINTF(Override, "Stream queue is full, don't request prediction\n");
    }
    squashing = false;
}

// this function collects predictions from all stages and generate bubbles
void
DecoupledBPUWithFTB::generateFinalPredAndCreateBubbles()
{
    DPRINTF(Override, "In generateFinalPredAndCreateBubbles().\n");
    // predsOfEachStage should be ready now
    // for (unsigned i = 0; i < numComponents; i++) {
    //     components[i]->makePrediction(predsOfEachStage[components[i]->getDelay()]);
    // }
    // TODO: print each stage predictions
    // DPRINTF(Override, "componentPreds[0]: valid: %d, (%#lx,%#lx)(%#lx)->%#lx, taken: %d, toBeCont: %d;\n"
    //         "componentPreds[1]: valid: %d, (%#lx,%#lx)(%#lx)->%#lx, taken: %d, toBeCont: %d.\n",
    //         componentPreds[0].valid, componentPreds[0].bbStart, componentPreds[0].controlAddr, componentPreds[0].getFallThruPC(),
    //         componentPreds[0].nextStream, componentPreds[0].isTaken(), componentPreds[0].toBeCont(),
    //         componentPreds[1].valid, componentPreds[1].bbStart, componentPreds[1].controlAddr, componentPreds[1].getFallThruPC(),
    //         componentPreds[1].nextStream, componentPreds[1].isTaken(), componentPreds[1].toBeCont());
    // choose the most accurate prediction
    FullFTBPrediction *chosen = &predsOfEachStage[0];
    // int j = (int) numComponents - 1;
    // DPRINTF(Override, "(int) numComponents - 1 : %d, numComponents: %d.\n", j, numComponents);

    // TODO: numStages
    for (int i = (int) numStages - 1; i >= 0; i--) {
        // const auto &e =predsOfEachStage[i];
        // DPRINTF(Override,
        //         "stage %u prediction: %#lx-[%#lx, %#lx) -> %#lx\n", i,
        //         e.bbStart, e.controlAddr, e.getFallThruPC(), e.nextStream);
        if (predsOfEachStage[i].valid) {
            chosen = &predsOfEachStage[i];
            DPRINTF(Override, "choose stage %d.\n", i);
            break;
        }
    }
    finalPred = *chosen;  // chosen could be invalid
    // streamTAGE->recordFoldedHist(finalPred);  // store the folded history
    // calculate bubbles
    unsigned first_hit_stage = 0;
    while (first_hit_stage < numStages-1) {
        if (predsOfEachStage[first_hit_stage].match(*chosen)) {
            break;
        }
        first_hit_stage++;
    }
    // generate bubbles
    /**
     * @brief We don't need to generate bubbles because ubtb does not work yet
     * @todo fix it with numOverrideBubbles = first_hit_stage;
     */
    numOverrideBubbles = first_hit_stage;
    receivedPred = true;
    // TODO: fix this
    // DPRINTF(Override, "chosen is valid: %d, (%#lx,%#lx)(%#lx)->%#lx, taken: %d, toBeCont: %d;\n", 
    //         finalPred.valid, finalPred.bbStart, finalPred.controlAddr, finalPred.getFallThruPC(),
    //         finalPred.nextStream, finalPred.isTaken(), finalPred.toBeCont());
    // DPRINTF(Override, "Ends generateFinalPredAndCreateBubbles(), numOverrideBubbles is %d, receivedPred is set true.\n", numOverrideBubbles);
}

bool
DecoupledBPUWithFTB::trySupplyFetchWithTarget(Addr fetch_demand_pc)
{
    return fetchTargetQueue.trySupplyFetchWithTarget(fetch_demand_pc);
}

// TODO: use fsq results generated by ftb
std::pair<bool, bool>
DecoupledBPUWithFTB::decoupledPredict(const StaticInstPtr &inst,
                               const InstSeqNum &seqNum, PCStateBase &pc,
                               ThreadID tid)
{
    std::unique_ptr<PCStateBase> target(pc.clone());

    DPRINTF(DecoupleBP, "looking up pc %#lx\n", pc.instAddr());
    auto target_avail = fetchTargetQueue.fetchTargetAvailable();

    DPRINTF(DecoupleBP, "Supplying fetch with target ID %lu\n",
            fetchTargetQueue.getSupplyingTargetId());

    if (!target_avail) {
        DPRINTF(DecoupleBP,
                "No ftq entry to fetch, return dummy prediction\n");
        // todo pass these with reference
        // TODO: do we need to update PC if not taken?
        return std::make_pair(false, true);
    }

    const auto &target_to_fetch = fetchTargetQueue.getTarget();
    // found corresponding entry
    auto start = target_to_fetch.startPC;
    auto end = target_to_fetch.endPC;
    auto taken_pc = target_to_fetch.takenPC;
    DPRINTF(DecoupleBP, "Responsing fetch with");
    printFetchTarget(target_to_fetch, "");

    // supplying ftq entry might be taken before pc
    // because it might just be updated last cycle
    // but last cycle ftq tells fetch that this is a miss stream
    assert(pc.instAddr() < end && pc.instAddr() >= start);
    bool taken = pc.instAddr() == taken_pc && target_to_fetch.taken;

    bool run_out_of_this_entry = false;

    if (taken) {
        auto &rtarget = target->as<GenericISA::PCStateWithNext>();
        rtarget.pc(target_to_fetch.target);
        // TODO: how about compressed?
        rtarget.npc(target_to_fetch.target + 4);
        rtarget.uReset();
        DPRINTF(DecoupleBP,
                "Predicted pc: %#lx, upc: %#lx, npc(meaningless): %#lx\n",
                target->instAddr(), rtarget.upc(), rtarget.npc());
        set(pc, *target);
        run_out_of_this_entry = true;
    } else {
        inst->advancePC(*target);
        if (target->instAddr() >= end) {
            run_out_of_this_entry = true;
        }
    }
    DPRINTF(DecoupleBP, "Predict it %staken to %#lx\n", taken ? "" : "not ",
            target->instAddr());

    if (run_out_of_this_entry) {
        // dequeue the entry
        DPRINTF(DecoupleBP, "running out of ftq entry %lu\n",
                fetchTargetQueue.getSupplyingTargetId());
        fetchTargetQueue.finishCurrentFetchTarget();
    }

    return std::make_pair(taken, run_out_of_this_entry);
}

void
DecoupledBPUWithFTB::controlSquash(unsigned target_id, unsigned stream_id,
                            const PCStateBase &control_pc,
                            const PCStateBase &corr_target,
                            const StaticInstPtr &static_inst,
                            unsigned control_inst_size, bool actually_taken,
                            const InstSeqNum &seq, ThreadID tid)
{
    // bool is_conditional = static_inst->isCondCtrl();
    // bool is_indirect = static_inst->isIndirectCtrl();
    // bool is_call = static_inst->isCall() && !static_inst->isNonSpeculative();
    // bool is_return = static_inst->isReturn() && !static_inst->isNonSpeculative();




    squashing = true;

    // check sanity
    auto squashing_stream_it = fetchStreamQueue.find(stream_id);

    if (squashing_stream_it == fetchStreamQueue.end()) {
        assert(!fetchStreamQueue.empty());
        // assert(fetchStreamQueue.rbegin()->second.getNextStreamStart() == MaxAddr);
        DPRINTF(
            DecoupleBP || debugFlagOn,
            "The squashing stream is insane, ignore squash on it");
        return;
    }

    s0PC = corr_target.instAddr();
    // s0StreamStartPC = s0PC;

    // get corresponding stream entry
    auto &stream = squashing_stream_it->second;

    auto pc = stream.startPC;
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (pc == ObservingPC) {
        debugFlagOn = true;
    }
    if (control_pc.instAddr() == ObservingPC || control_pc.instAddr() == ObservingPC2) {
        debugFlagOn = true;
    }

    DPRINTF(DecoupleBPHist,
            "stream start=%#lx, predict on hist: %s\n", stream.startPC,
            stream.history);

    // DPRINTF(DecoupleBP || debugFlagOn,
    //         "Control squash: ftq_id=%lu, fsq_id=%lu,"
    //         " control_pc=%#lx, corr_target=%#lx, is_conditional=%u, "
    //         "is_indirect=%u, actually_taken=%u, branch seq: %lu\n",
    //         target_id, stream_id, control_pc.instAddr(),
    //         corr_target.instAddr(), is_conditional, is_indirect,
    //         actually_taken, seq);

    dumpFsq("Before control squash");

    // streamLoopPredictor->restoreLoopTable(stream.mruLoop);
    // streamLoopPredictor->controlSquash(stream_id, stream, control_pc.instAddr(), corr_target.instAddr());

    stream.squashType = SQUASH_CTRL;

    FetchTargetId ftq_demand_stream_id;

    // TODO: restore ras
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBPRAS,
             "dump ras before control squash\n");
    // dumpRAS();

    // restore ras by traversing the stream younger than the squashing stream
    // for (auto iter = --(fetchStreamQueue.end());iter != squashing_stream_it;--iter) {
    //     auto restore = iter->second;
    //     if (restore.isCall()) {
    //         popRAS(iter->first, "control squash (on mispred path)");
    //     } else if (restore.isReturn()) {
    //         pushRAS(iter->first, "control squash (on mispred path)", restore.getTakenTarget());
    //     }
    // }
    // restore RAS to the state before speculative update
    // We use **old** speculations (on whether is call or ret)
    // if (stream.isCall()) {
    //     popRAS(stream_id, "control squash");
    // } else if (stream.isReturn()) {
    //     pushRAS(stream_id, "control squash", stream.getTakenTarget());
    // }

    // TODO: update exeBranchInfo
    stream.exeBranchInfo = BranchInfo(control_pc, corr_target, static_inst, control_inst_size);
    stream.exeTaken = actually_taken;
    // We should update endtype after restoring speculatively updated states!!
    // if (is_return) {
    //     ++stats.RASIncorrect;
    //     stream.endType = END_RET;
    // } else if (is_call) {
    //     stream.endType = END_CALL;
    // } else if (actually_taken) {
    //     stream.endType = END_OTHER_TAKEN;
    // } else {
    //     stream.endType = END_NOT_TAKEN;
    // }
    // DPRINTF(DecoupleBP || debugFlagOn, "stream end type: %d\n", stream.endType);
    // Since here, we are using updated infomation

    squashStreamAfter(stream_id);

    stream.resolved = true;
    // stream.exeEnded = true;
    // stream.exeBranchPC = control_pc.instAddr();
    // stream.exeTarget = corr_target.instAddr();
    // stream.exeEndPC = stream.exeBranchPC + control_inst_size;

    // recover history to the moment doing prediction
    DPRINTF(DecoupleBPHist,
             "Recover history %s\nto %s\n", s0History, stream.history);
    s0History = stream.history;
    // streamTAGE->recoverFoldedHist(s0History);
    
    // TODO: update history again with correct branch info in all FTB entry
    // TODO: wrap into a function
    // int shamt;
    // histShiftIn();

    DPRINTF(DecoupleBPHist,
                "Shift in history %s\n", s0History);

    if (actually_taken) {

        stream.exeTaken = true;
        // stream.endType =
        //     is_call ? END_CALL : (is_return ? END_RET : END_OTHER_TAKEN);
        // DPRINTF(DecoupleBP || debugFlagOn,
        //         "Mark stream %lu is call: %u, is return: %u\n", stream_id,
        //         is_call, is_return);

        // update ras
        // if (stream.isCall()) {
        //     pushRAS(stream_id, "speculative update (in squash)", stream.getFallThruPC());
        // } else if (stream.isReturn()) {
        //     popRAS(stream_id, "speculative update (in squash)");
        // }

        // DPRINTFV(
        //     this->debugFlagOn || ::gem5::debug::DecoupleBP,
        //     "a %s flow was redirected by taken branch, "
        //     "predicted: %#lx->%#lx, correct: %#lx->%#lx, new fsq entry is:\n",
        //     stream.predEnded ? "pred-longer" : "miss", stream.predBranchPC,
        //     stream.predTarget, control_pc.instAddr(), corr_target.instAddr());
        
        printStream(stream);

    } else {
        // stream.endType = END_NOT_TAKEN;
        // DPRINTFV(
        //     this->debugFlagOn || ::gem5::debug::DecoupleBP,
        //     "a taken flow was redirected by NOT taken branch,"
        //     "predicted: %#lx->%#lx, correct: %#lx->%#lx, new fsq entry is:\n",
        //     stream.predBranchPC, stream.predTarget, control_pc.instAddr(),
        //     corr_target.instAddr());
        printStream(stream);
    }

    
    // inc stream id because current stream ends
    // now stream always ends
    ftq_demand_stream_id = stream_id + 1;
    fsqId = stream_id + 1;

    dumpFsq("After control squash");

    DPRINTF(DecoupleBPRAS, "dump ras after control squash\n");
    // dumpRAS();

    finalPred.valid = false;

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            corr_target.instAddr());

    fetchTargetQueue.dump("After control squash");

    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "After squash, FSQ head Id=%lu, demand stream Id=%lu, Fetch "
            "demanded target Id=%lu\n",
            fsqId, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());

    // historyManager.squash(stream_id, actually_taken, control_pc.instAddr(),
    //                       corr_target.instAddr());
    // checkHistory(s0History);
}

void
DecoupledBPUWithFTB::nonControlSquash(unsigned target_id, unsigned stream_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid)
{
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "non control squash: target id: %lu, stream id: %lu, inst_pc: %x, "
            "seq: %lu\n",
            target_id, stream_id, inst_pc.instAddr(), seq);
    squashing = true;

    dumpFsq("before non-control squash");

    // make sure the stream is in FSQ
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());

    // auto start = it->second.startPC;
    // auto end_pc = it->second.getEndPC();
    // auto next_stream_start = it->second.getNextStreamStart();

    auto ftq_demand_stream_id = stream_id;

    squashStreamAfter(stream_id);
    // why not recover history here?
    

    // DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
    //          "non control squash: start: %x, end: %x, target: %x\n", start,
    //          end_pc, next_stream_start);

    // if (end_pc) {
    //     if (start <= inst_pc.instAddr() && inst_pc.instAddr() < end_pc) {
    //         // this pc is in the stream
    //     } else if (inst_pc.instAddr() == next_stream_start) {
    //         // why would this happen?
    //         // this pc is in the next stream!
    //         DPRINTF(DecoupleBP,
    //                 "Redirected PC is the target of the stream, "
    //                 "indicating that the stream is ended, switch to next "
    //                 "stream\n");
    //         ++it;
    //         ftq_demand_stream_id = stream_id + 1;
    //     } else {
    //         // why would this happen?
    //         ++it;
    //         // find the containing stream
    //         DPRINTF(DecoupleBP,
    //                 "Redirected PC is not following stream, find it\n");
    //         while (it != fetchStreamQueue.end()) {
    //             if (it->second.startPC <= inst_pc.instAddr() &&
    //                 inst_pc.instAddr() < it->second.getEndPC()) {
    //                 DPRINTF(DecoupleBP, "Found it in stream %lu\n", it->first);
    //                 ftq_demand_stream_id = it->first;
    //                 break;
    //             }
    //             ++it;
    //         }
    //         assert(it != fetchStreamQueue.end());
    //     }
    // }

    // TODO: update history again with correct branch info in all FTB entry
    // TODO: wrap into a function
    // auto &stream = it->second;
    // fetching from a new fsq entry
    auto pc = inst_pc.instAddr();
    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id + 1, pc);

    // we should set s0PC to be the predTarget of the fsq entry
    // in case that the stream is the newest entry in fsq,
    // That's because, if an inst of this stream had been squashed due to a
    // mispredict, and then a non-control squash was made on an inst oldder
    // than this inst, the s0PC would be set incorrectly by the previous
    // squash

    // next_stream_start = stream.getNextStreamStart();
    // stream ptr might have been updated, so we need to update next stream start

    s0PC = pc;
    fsqId = stream_id + 1;
    // if (++it == fetchStreamQueue.end() && stream.getEnded()) {
    //     s0PC = next_stream_start;
    //     // s0StreamStartPC = s0PC;
    //     fsqId = (--it)->first + 1;
    // }

    if (pc == ObservingPC) dumpFsq("after non-control squash");
    DPRINTFV(this->debugFlagOn || ::gem5::debug::DecoupleBP,
            "After squash, FSQ head Id=%lu, s0pc=%#lx, demand stream Id=%lu, "
            "Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void
DecoupledBPUWithFTB::trapSquash(unsigned target_id, unsigned stream_id,
                         Addr last_committed_pc, const PCStateBase &inst_pc,
                         ThreadID tid)
{
    DPRINTF(DecoupleBP || debugFlagOn,
            "Trap squash: target id: %lu, stream id: %lu, inst_pc: %#lx\n",
            target_id, stream_id, inst_pc.instAddr());
    squashing = true;

    auto pc = inst_pc.instAddr();

    if (pc == ObservingPC) dumpFsq("before trap squash");

    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());
    auto &stream = it->second;

    // TODO: restore ras
    DPRINTF(DecoupleBPRAS, "dump ras before trap squash\n");
    // dumpRAS();
    // for (auto iter = --(fetchStreamQueue.end());iter != it;--iter) {
    //     auto restore = iter->second;
    //     if (restore.isCall()) {
    //         DPRINTF(DecoupleBPRAS, "erase call in trap squash, fsqid: %lu\n", iter->first);
    //         if (!streamRAS.empty()) {
    //             streamRAS.pop();
    //         }
    //     } else if (restore.isReturn()) {
    //         DPRINTF(DecoupleBPRAS, "erase return in trap squash, fsqid: %lu\n", iter->first);
    //         streamRAS.push(restore.predTarget);
    //     }
    // }
    // if (stream.isCall()) {
    //     DPRINTF(DecoupleBPRAS, "erase call in trap squash, fsqid: %lu\n", it->first);
    //     if (!streamRAS.empty()) {
    //         streamRAS.pop();
    //     }
    // } else if (stream.isReturn()) {
    //     DPRINTF(DecoupleBPRAS, "erase return in trap squash, fsqid: %lu\n", it->first);
    //     streamRAS.push(stream.predTarget);
    // }

    stream.resolved = true;
    // stream.exeEnded = true;
    // stream.exeBranchPC = last_committed_pc;
    // // stream.exeBranchType = 1;
    // stream.exeTarget = inst_pc.instAddr();
    // stream.exeEndPC = stream.exeBranchPC + 4;
    // stream.endType = END_NOT_TAKEN;
    stream.squashType = SQUASH_TRAP;

    // TODO: update history again with correct branch info in all FTB entry
    // TODO: wrap into a function
    // historyManager.squash(stream_id, true, last_committed_pc, inst_pc.instAddr());

    // boost::to_string(s0History, buf1);
    // boost::to_string(stream.history, buf2);
    // DPRINTF(DecoupleBP, "Recover history %s\nto %s\n", buf1.c_str(),
    //         buf2.c_str());
    // s0History = stream.history;
    // streamTAGE->recoverFoldedHist(s0History);
    // // streamTAGE->checkFoldedHist(s0History);
    // auto hashed_path =
    //     computePathHash(last_committed_pc, inst_pc.instAddr());
    // histShiftIn(hashed_path, s0History);
    // boost::to_string(s0History, buf1);
    // DPRINTF(DecoupleBP, "Shift in history %s\n", buf1.c_str());

    // checkHistory(s0History);

    squashStreamAfter(stream_id);

    // inc stream id because current stream is disturbed
    auto ftq_demand_stream_id = stream_id + 1;
    // todo update stream head id here
    fsqId = stream_id + 1;

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            inst_pc.instAddr());

    s0PC = inst_pc.instAddr();
    // s0StreamStartPC = s0PC;

    DPRINTF(DecoupleBP,
            "After trap squash, FSQ head Id=%lu, s0pc=%#lx, demand stream "
            "Id=%lu, Fetch demanded target Id=%lu\n",
            fsqId, s0PC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());

    // restore ras
    DPRINTF(DecoupleBPRAS, "dump ras after trap squash\n");
    // dumpRAS();
}

// TODO: update with fsq id
void DecoupledBPUWithFTB::update(unsigned stream_id, ThreadID tid)
{
    // aka, commit stream
    // commit controls in local prediction history buffer to committedSeq
    // mark all committed control instructions as correct
    // do not need to dequeue when empty
    if (fetchStreamQueue.empty())
        return;
    auto it = fetchStreamQueue.begin();
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    while (it != fetchStreamQueue.end() && stream_id >= it->first) {
        auto &stream = it->second;
        // storeLoopInfo(it->first, stream);
        // if (stream.isLoop && stream.tageTarget != stream.loopTarget) {
        //     Addr target = stream.squashType == SQUASH_CTRL ? stream.exeTarget : stream.predTarget;
        //     streamLoopPredictor->updateLoopUseCount(stream.loopTarget == target);
        // }
        // dequeue
        DPRINTF(DecoupleBP, "dequeueing stream id: %lu, entry below:\n",
                it->first);
        bool miss_predicted = stream.squashType == SQUASH_CTRL;
        // if (stream.startPC == ObservingPC) {
        //     debugFlagOn = true;
        // }
        // if (stream.exeBranchPC == ObservingPC2) {
        //     debugFlagOn = true;
        // }
        DPRINTF(DecoupleBP || debugFlagOn,
                "Commit stream start %#lx, which is %s predicted, "
                "final br addr: %#lx, final target: %#lx, pred br addr: %#lx, "
                "pred target: %#lx\n",
                stream.startPC, miss_predicted ? "miss" : "correctly",
                stream.exeBranchInfo.pc, stream.exeBranchInfo.target,
                stream.predBranchInfo.pc, stream.predBranchInfo.target);

        ftb->update(stream, tid);
        // if (stream.squashType == SQUASH_NONE || stream.squashType == SQUASH_CTRL) {
        //     DPRINTF(DecoupleBP || debugFlagOn,
        //             "Update mispred stream %lu, start=%#lx, hist=%s, "
        //             "taken=%i, control=%#lx, target=%#lx\n",
        //             it->first, stream.startPC, stream.history,
        //             stream.getTaken(), stream.getControlPC(),
        //             stream.getTakenTarget());
        //     auto cur_chunk = stream.startPC;
        //     auto last_chunk = computeLastChunkStart(stream.getControlPC(), stream.startPC);
        //     while (cur_chunk < last_chunk) {
        //         auto next_chunk = cur_chunk + streamChunkSize;
        //         DPRINTF(DecoupleBP || debugFlagOn,
        //                 "Update fall-thru chunk %#lx\n", cur_chunk);
        //         streamTAGE->update(
        //             cur_chunk, stream.startPC, next_chunk - 2, next_chunk,
        //             2, false /* not taken*/, stream.history, END_CONT,
		//             stream.indexFoldedHist, stream.tagFoldedHist);
        //         streamUBTB->update(
        //             cur_chunk, stream.startPC, next_chunk - 2, next_chunk,
        //             2, false /* not taken*/, stream.history, END_CONT);
        //         cur_chunk = next_chunk;
        //     }
        //     updateTAGE(stream);
        //     streamUBTB->update(last_chunk, stream.startPC,
        //                        stream.getControlPC(),
        //                        stream.getNextStreamStart(),
        //                        stream.getFallThruPC() - stream.getControlPC(),
        //                        stream.getTaken(), stream.history,
        //                        static_cast<EndType>(stream.endType));

        //     if (stream.squashType == SQUASH_CTRL) {
        //         if (stream.exeBranchPC == ObservingPC2) {
        //             storeTargets.push_back(stream.exeTarget);
        //         }

        //         if (stream.predSource == LoopButInvalid) {
        //             useLoopButInvalid++;
        //         } else if (stream.predSource == LoopAndValid)
        //             useLoopAndValid++;
        //         else
        //             notUseLoop++;

        //         auto find_it = topMispredicts.find(std::make_pair(stream.startPC, stream.exeBranchPC));
        //         if (find_it == topMispredicts.end()) {
        //             topMispredicts[std::make_pair(stream.startPC, stream.exeBranchPC)] = 1;
        //         } else {
        //             find_it->second++;
        //         }

        //         if (stream.isMiss && stream.exeBranchPC == ObservingPC) {
        //             missCount++;
        //         }

        //         if (stream.exeBranchPC == ObservingPC) {
        //             debugFlagOn = true;
        //             auto misTripCount = misPredTripCount.find(stream.tripCount);
        //             if (misTripCount == misPredTripCount.end()) {
        //                 misPredTripCount[stream.tripCount] = 1;
        //             } else {
        //                 misPredTripCount[stream.tripCount]++;
        //             }
        //             DPRINTF(DecoupleBP || debugFlagOn, "commit mispredicted stream %lu\n", it->first);
        //         }
        //     } else {
        //         if (stream.predBranchPC == ObservingPC2) {
        //             storeTargets.push_back(stream.predTarget);
        //         }
        //     }

        //     if (stream.startPC == ObservingPC && stream.squashType == SQUASH_CTRL) {
        //         auto hist(stream.history);
        //         hist.resize(18);
        //         uint64_t pattern = hist.to_ulong();
        //         auto find_it = topMispredHist.find(pattern);
        //         if (find_it == topMispredHist.end()) {
        //             topMispredHist[pattern] = 1;
        //         } else {
        //             find_it->second++;
        //         }
        //     }
        // } else {
        //     DPRINTF(DecoupleBP || debugFlagOn, "Update trap stream %lu\n", it->first);
        //     assert(stream.squashType == SQUASH_TRAP);
        //     // For trap squash, we expect them to always incorrectly predict
        //     // Because correct prediction will cause strange behaviors
        //     streamTAGE->update(computeLastChunkStart(stream.getControlPC(),
        //                                              stream.startPC),
        //                        stream.startPC, stream.getControlPC(),
        //                        stream.getFallThruPC(), 4, false /* not taken*/,
        //                        stream.history, END_NOT_TAKEN,
		// 	                   stream.indexFoldedHist, stream.tagFoldedHist);

        //     streamUBTB->update(computeLastChunkStart(stream.getControlPC(),
        //                                              stream.startPC),
        //                        stream.startPC, stream.getControlPC(),
        //                        stream.getFallThruPC(), 4, false /* not taken*/,
        //                        stream.history, END_NOT_TAKEN);
        // }

        it = fetchStreamQueue.erase(it);
    }
    DPRINTF(DecoupleBP, "after commit stream, fetchStreamQueue size: %lu\n",
            fetchStreamQueue.size());
    printStream(it->second);

    historyManager.commit(stream_id);
}

// TODO: squash ftq entry after
void
DecoupledBPUWithFTB::squashStreamAfter(unsigned squash_stream_id)
{
    auto erase_it = fetchStreamQueue.upper_bound(squash_stream_id);
    while (erase_it != fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP || debugFlagOn || erase_it->second.startPC == ObservingPC,
                "Erasing stream %lu when squashing %lu\n", erase_it->first,
                squash_stream_id);
        printStream(erase_it->second);
        fetchStreamQueue.erase(erase_it++);
    }
}

void
DecoupledBPUWithFTB::dumpFsq(const char *when)
{
    DPRINTF(DecoupleBPProbe, "dumping fsq entries %s...\n", when);
    for (auto it = fetchStreamQueue.begin(); it != fetchStreamQueue.end();
         it++) {
        DPRINTFR(DecoupleBPProbe, "StreamID %lu, ", it->first);
        printStream(it->second);
    }
}

// this funtion use finalPred to enq fsq(ftq) and update s0PC
void
DecoupledBPUWithFTB::tryEnqFetchStream()
{
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (s0PC == ObservingPC) {
        debugFlagOn = true;
    }
    if (!receivedPred) {
        DPRINTF(DecoupleBP, "No received prediction, cannot enq fsq\n");
        DPRINTF(Override, "In tryEnqFetchStream(), received is false.\n");
        return;
    } else {
        DPRINTF(Override, "In tryEnqFetchStream(), received is true.\n");
    }
    if (s0PC == MaxAddr) {
        DPRINTF(DecoupleBP, "s0PC %#lx is insane, cannot make prediction\n", s0PC);
        return;
    }
    // prediction valid, but not ready to enq because of bubbles
    if (numOverrideBubbles > 0) {
        DPRINTF(DecoupleBP, "Waiting for bubble caused by overriding, bubbles rest: %u\n", numOverrideBubbles);
        DPRINTF(Override, "Waiting for bubble caused by overriding, bubbles rest: %u\n", numOverrideBubbles);
        return;
    }
    assert(!streamQueueFull());
    if (true) {
        bool should_create_new_stream = true;
        // if (!fetchStreamQueue.empty()) {
        //     // check last entry state
        //     auto &back = fetchStreamQueue.rbegin()->second;
        //     if (back.getEnded()) {
        //         should_create_new_stream = true;
        //         DPRINTF(DecoupleBP || debugFlagOn,
        //                 "FSQ: the latest stream has ended\n");
        //     } else if (back.getEndPC() >= MaxAddr) {
        //         DPRINTF(DecoupleBP,
        //                 "stream end %#lx is insane, cannot make prediction\n",
        //                 back.getEndPC());
        //         return;
        //     }
        // }
        // if (fetchStreamQueue.empty()) {
        //     should_create_new_stream = true;
        //     DPRINTF(DecoupleBP || debugFlagOn,
        //             "FSQ: is empty\n");
        // }

        // if (!should_create_new_stream) {
        //     DPRINTF(DecoupleBP || debugFlagOn,
        //             "FSQ is not empty and the latest stream hasn't ended\n");
        // }
        makeNewPrediction(should_create_new_stream);

        // const auto &back = fetchStreamQueue.rbegin()->second;
        // if (!back.getEnded()) {
        //     // streamMiss = true;
        //     DPRINTF(DecoupleBP || debugFlagOn, "s0PC update to %#lx\n", s0PC);
        // } else {
        //     DPRINTF(DecoupleBP || debugFlagOn,
        //             "stream %lu has ended, s0PC update to %#lx\n",
        //             fetchStreamQueue.rend()->first, s0PC);
        // }

    } else {
        DPRINTF(DecoupleBP || debugFlagOn, "FSQ is full: %lu\n",
                fetchStreamQueue.size());
    }
    receivedPred = false;
    DPRINTF(Override, "In tryFetchEnqStream(), receivedPred reset to false.\n");
    DPRINTF(DecoupleBP || debugFlagOn, "fsqId=%lu\n", fsqId);
}

void
DecoupledBPUWithFTB::setTakenEntryWithStream(const FetchStream &stream_entry, FtqEntry &ftq_entry)
{
    ftq_entry.taken = true;
    ftq_entry.takenPC = stream_entry.getControlPC();
    ftq_entry.endPC = stream_entry.predEndPC;
    ftq_entry.target = stream_entry.getTakenTarget();
}

void
DecoupledBPUWithFTB::setNTEntryWithStream(FtqEntry &ftq_entry, Addr end_pc)
{
    ftq_entry.taken = false;
    ftq_entry.takenPC = 0;
    ftq_entry.target = 0;
    ftq_entry.endPC = end_pc;
}

// previous: get ftq entry with fsq entry
// TODO: replace tryEnqFetchStream, and rename it to this
void
DecoupledBPUWithFTB::tryEnqFetchTarget()
{
    DPRINTF(DecoupleBP, "Try to enq fetch target\n");
    if (fetchTargetQueue.full()) {
        DPRINTF(DecoupleBP, "FTQ is full\n");
        return;
    }
    if (fetchStreamQueue.empty()) {
        // no stream that have not entered ftq
        DPRINTF(DecoupleBP, "No stream to enter ftq in fetchStreamQueue\n");
        return;
    }
    // ftq can accept new cache lines,
    // try to get cache lines from fetchStreamQueue
    // find current stream with ftqEnqfsqID in fetchStreamQueue
    auto &ftq_enq_state = fetchTargetQueue.getEnqState();
    auto it = fetchStreamQueue.find(ftq_enq_state.streamId);
    if (it == fetchStreamQueue.end()) {
        // desired stream not found in fsq
        DPRINTF(DecoupleBP, "FTQ enq desired Stream ID %u is not found\n",
                ftq_enq_state.streamId);
        return;
    }

    auto &stream_to_enq = it->second;
    Addr end = stream_to_enq.predEndPC;
    DPRINTF(DecoupleBP, "Serve enq PC: %#lx with stream %lu:\n",
            ftq_enq_state.pc, it->first);
    printStream(stream_to_enq);
    

    // We does let ftq to goes beyond fsq now
    assert(ftq_enq_state.pc <= end);

    // deal with an incompleted target entry
    // would not happen now
    // if (fetchTargetQueue.lastEntryIncomplete()) {
    //     DPRINTF(DecoupleBP || debugFlagOn,
    //             "Try filling up incompleted ftq entry\n");
    //     fetchTargetQueue.dump("Before update last entry");
    //     auto &last = fetchTargetQueue.getLastInsertedEntry();
    //     bool used_up_stream = false;
    //     if (stream_to_enq.getTaken()) {
    //         DPRINTF(DecoupleBP || debugFlagOn,
    //                 "mark the last target as taken\n");
    //         setTakenEntryWithStream(stream_to_enq, last);
    //         ftq_enq_state.pc = last.target;
    //         used_up_stream = true;
    //     } else {
    //         // still a missing target, set endPC to the predicted end
    //         // decouplePredict is only allow to predicted untial this end
    //         Addr predicted_until = stream_to_enq.getEndPC();
    //         if (predicted_until > ftq_enq_state.pc) {
    //             last.endPC =
    //                 std::min(predicted_until,
    //                          alignToCacheLine(last.endPC + fetchTargetSize));
    //             ftq_enq_state.pc = last.endPC;
    //         } else {
    //             DPRINTF(
    //                 DecoupleBP || debugFlagOn,
    //                 "FSQ falls behind FTQ, cannot update last FTQ entry\n");
    //         }
    //         used_up_stream = stream_to_enq.getEnded() &&
    //                          ftq_enq_state.pc == stream_to_enq.getFallThruPC();
    //         DPRINTF(
    //             DecoupleBP || debugFlagOn,
    //             "the last target is still not taken, can predict to %#lx\n",
    //             ftq_enq_state.pc);
    //     }
    //     if (used_up_stream) {
    //         ftq_enq_state.streamId++;
    //     }
    //     DPRINTF(DecoupleBP || debugFlagOn,
    //             "After fill up last entry, Update ftqEnqPC to %#lx, "
    //             "FTQ demand stream ID to %lu\n",
    //             ftq_enq_state.pc, ftq_enq_state.streamId);
    //     printFetchTarget(last, "Updated in FTQ");
    //     fetchTargetQueue.dump("After update last entry");
    //     return;
    // }
    // create a new target entry
    FtqEntry ftq_entry;
    ftq_entry.startPC = ftq_enq_state.pc;
    ftq_entry.fsqID = ftq_enq_state.streamId;

    Addr baddr = stream_to_enq.getControlPC();
    // Addr line_end = alignToCacheLine(ftq_entry.startPC + fetchTargetSize);
    // bool branch_in_line = baddr < line_end;
    // bool end_in_line = end <= line_end;

    bool taken = stream_to_enq.predTaken;
    ftq_entry.endPC = end;
    ftq_entry.takenPC = taken ? baddr : 0;
    ftq_entry.taken = taken;
    ftq_entry.target = taken ? stream_to_enq.predBranchInfo.target : 0;


    // DPRINTF(DecoupleBP,
    //         "end in line: %d, branch in line: %d, stream ended: %d\n",
    //         end_in_line, branch_in_line, stream_to_enq.getEnded());


    // TODO: add at most two cachelines at a time
    // set ftq_enq_state, and ftq_entry

    // first ftq entry
    // bool used_up_stream = false;
    // at the latter half of a cache line
    // bool cross_two_cachelines = crossCacheLine(ftq_entry.startPC);
    // if (stream_to_enq.getTaken() && branch_in_line) {
    //     DPRINTF(DecoupleBP, "Enqueue FTQ with ended stream %lu\n",
    //             ftq_enq_state.streamId);
    //     printStream(stream_to_enq);
        

    //     setTakenEntryWithStream(stream_to_enq, ftq_entry);
    //     ftq_enq_state.pc = ftq_entry.target;
    //     used_up_stream = true;

    // } else if (stream_to_enq.predEndPC <= ftq_enq_state.pc) {
    //     DPRINTF(DecoupleBP || debugFlagOn,
    //             "FSQ falls behind FTQ, cannot update last FTQ entry\n");

    //     DPRINTF(DecoupleBP || debugFlagOn,
    //             "(Unchanged) Update ftqEnqPC = %#lx, FTQ demand stream ID = %lu\n",
    //             ftq_enq_state.pc, ftq_enq_state.streamId);
    //     return;
    // } else {  // not taken:  A. missing or B. ended but not taken
    //     Addr predict_until;
    //     // assert(!stream_to_enq.getEnded() ||
    //     //        (stream_to_enq.getEnded() && !stream_to_enq.getTaken())||
    //     //        (stream_to_enq.getTaken() && !branch_in_line));
    //     used_up_stream = stream_to_enq.getEnded() && end_in_line;
    //     if (end_in_line) {
    //         predict_until = end;
    //     } else {
    //         // Taken but br not in line
    //         // Or, miss and predicted_until goes beyond this cache line
    //         predict_until = line_end;
    //     }
    //     setNTEntryWithStream(ftq_entry, predict_until);
    //     ftq_enq_state.pc = predict_until;
    //     DPRINTF(DecoupleBP,
    //             "Enqueue FTQ with continuous stream %lu that can predicted "
    //             "until %#lx. Update ftqEnqPC to %#lx\n",
    //             ftq_enq_state.streamId, predict_until, ftq_enq_state.pc);
    //     printStream(stream_to_enq);
    // }

    // if (used_up_stream) {
    ftq_enq_state.pc = taken ? stream_to_enq.predBranchInfo.target : end;
    ftq_enq_state.streamId++;
    // }
    DPRINTF(DecoupleBP,
            "Update ftqEnqPC to %#lx, FTQ demand stream ID to %lu\n",
            ftq_enq_state.pc, ftq_enq_state.streamId);

    fetchTargetQueue.enqueue(ftq_entry);

    assert(ftq_enq_state.streamId <= fsqId + 1);

    // DPRINTF(DecoupleBP, "a%s stream, next enqueue target: %lu\n",
    //         stream_to_enq.getEnded() ? "n ended" : " miss", ftq_enq_state.nextEnqTargetId);
    printFetchTarget(ftq_entry, "Insert to FTQ");
    fetchTargetQueue.dump("After insert new entry");
}

void
DecoupledBPUWithFTB::histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history)
{
    history <<= shamt;
    history[0] = taken;
}

// this function enqueues fsq and update s0PC and s0History
void
DecoupledBPUWithFTB::makeNewPrediction(bool create_new_stream)
{
    DPRINTF(DecoupleBP, "Try to make new prediction\n");
    FetchStream entry_new;
    // TODO: this may be wrong, need to check if we should use the last
    // s0PC
    // auto back = fetchStreamQueue.rbegin();
    auto &entry = entry_new;
    entry.startPC = s0PC;
    defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
    if (s0PC == ObservingPC) {
        debugFlagOn = true;
    }
    if (finalPred.controlAddr() == ObservingPC || finalPred.controlAddr() == ObservingPC2) {
        debugFlagOn = true;
    }
    DPRINTF(DecoupleBP || debugFlagOn, "Make pred with %s, pred valid: %i, taken: %i\n",
             create_new_stream ? "new stream" : "last missing stream",
             finalPred.valid, finalPred.isTaken());

    Addr fallThroughAddr = finalPred.getFallThrough();
    bool taken = finalPred.isTaken();
    entry.isHit = finalPred.valid;
    entry.predFTBEntry = finalPred.ftbEntry;
    entry.predTaken = taken;
    entry.predEndPC = fallThroughAddr;
    // TODO: if taken, set branch info with corresponding ftb slot
    if (taken) {
        entry.predBranchInfo = finalPred.getTakenSlot().getBranchInfo();
    }

    // update s0PC
    Addr nextPC = finalPred.getTarget();

    s0PC = nextPC;

    entry.history = s0History;
    // entry.predStreamId = fsqId;


    // TODO: history manager
    {}


    // update histories for components
    for (int i = 0; i < numComponents; i++) {
        components[i]->specUpdateHist(s0History, finalPred);
    }
    // update ghr
    int shamt;
    std::tie(shamt, taken) = finalPred.getHistInfo();
    boost::to_string(s0History, buf1);
    histShiftIn(shamt, taken, s0History);
    boost::to_string(s0History, buf2);

    // // make new stream entry with final pred
    // if (finalPred.valid && finalPred.isTaken()) {
    //     DPRINTF(DecoupleBP, "TAGE predicted target: %#lx\n", finalPred.nextStream);

    //     entry.predEnded = true;
    //     entry.predTaken = true;
    //     entry.predEndPC = finalPred.controlAddr + finalPred.controlSize;
    //     entry.predBranchPC = finalPred.controlAddr;
    //     entry.predTarget = finalPred.nextStream;
    //     entry.history = finalPred.history;
    //     entry.endType = finalPred.endType;
    //     s0PC = finalPred.nextStream;
    //     s0StreamStartPC = s0PC;


    //     DPRINTF(DecoupleBP, "Update s0PC to %#lx\n", s0PC);
    //     DPRINTF(DecoupleBP, "Update s0History from %s to %s\n", buf1.c_str(),
    //             buf2.c_str());
    //     DPRINTF(DecoupleBP, "hist info: shamt->%d, taken->%d\n", shamt, taken);

    //     if (create_new_stream) {
    //         historyManager.addSpeculativeHist(finalPred.controlAddr,
    //                                           finalPred.nextStream, fsqId, false);
    //     } else {
    //         historyManager.updateSpeculativeHist(finalPred.controlAddr,
    //                                              finalPred.nextStream, fsqId);
    //     }

    //     DPRINTF(DecoupleBP || debugFlagOn,
    //             "Build stream %lu with Valid finalPred: %#lx-[%#lx, %#lx) "
    //             "--> %#lx, is call: %i, is return: %i\n",
    //             fsqId, entry.startPC, entry.predBranchPC,
    //             entry.predEndPC, entry.predTarget, entry.isCall(), entry.isReturn());
    // } else {
    //     DPRINTF(DecoupleBP || debugFlagOn,
    //             "No valid prediction or pred fall thru, gen missing stream:"
    //             " %#lx -> ... is call: %i, is return: %i\n",
    //             s0PC, entry.isCall(), entry.isReturn());
    //     entry.isMiss = true;

    //     if (finalPred.valid && !finalPred.isTaken() && !finalPred.toBeCont()) {
    //         // The prediction only tell us not taken until endPC
    //         // The generated stream cannot serve since endPC
    //         s0PC = finalPred.getFallThruPC();
    //         s0StreamStartPC = s0PC;
    //         entry.predEnded = true;
    //         entry.predBranchPC = finalPred.controlAddr;
    //         entry.predTarget = finalPred.nextStream;
    //     } else {
    //         if (M5_UNLIKELY(s0PC + streamChunkSize < s0PC)) {
    //             // wrap around is insane, we stop predicting
    //             s0PC = MaxAddr;
    //         } else {
    //             s0PC += streamChunkSize;
    //         }
    //         entry.predEnded = false;
    //     }
    //     entry.predEndPC = s0PC;
    //     entry.history.resize(historyBits);

    //     // when pred is invalid, history from finalPred is outdated
    //     // entry.history = s0History is right for back-to-back predictor
    //     // but not true for backing predictor taking multi-cycles to predict.
    //     entry.history = s0History; 

    //     if (create_new_stream) {
    //         // A missing history will not participate into history checking
    //         historyManager.addSpeculativeHist(0, 0, fsqId, true);
    //     }
    // }
    // DPRINTF(DecoupleBP || debugFlagOn,
    //         "After pred, s0StreamStartPC=%#lx, s0PC=%#lx\n", s0StreamStartPC,
    //         s0PC);
    // boost::to_string(entry.history, buf1);
    // DPRINTF(DecoupleBP, "New prediction history: %s\n", buf1.c_str());

    // std::tie(entry.isLoop, entry.loopTarget) = streamLoopPredictor->makeLoopPrediction(finalPred.controlAddr);
    // if (finalPred.useLoopPrediction) {
    //     streamLoopPredictor->updateTripCount(fsqId, finalPred.controlAddr);
    //     entry.useLoopPrediction = true;
    // }
    // entry.tripCount = finalPred.useLoopPrediction ? streamLoopPredictor->getTripCount(finalPred.controlAddr) : entry.tripCount;
    // entry.predSource = finalPred.predSource;
    // entry.mruLoop = streamLoopPredictor->getMRULoop();
    // entry.indexFoldedHist = finalPred.indexFoldedHist;
    // entry.tagFoldedHist = finalPred.tagFoldedHist;

    
    // if (create_new_stream) {
        entry.setDefaultResolve();
        auto [insert_it, inserted] = fetchStreamQueue.emplace(fsqId, entry);
        assert(inserted);

    //     DPRINTF(DecoupleBP || debugFlagOn, "use loop prediction: %d at %#lx-->%#lx\n",
    //             entry.useLoopPrediction, entry.predBranchPC, entry.predTarget);
        dumpFsq("after insert new stream");
        DPRINTF(DecoupleBP || debugFlagOn, "Insert fetch stream %lu\n", fsqId);
    // } else {
    //     DPRINTF(DecoupleBP || debugFlagOn, "use loop prediction: %d at %#lx-->%#lx\n",
    //             finalPred.useLoopPrediction, entry.predBranchPC, entry.predTarget);
        // DPRINTF(DecoupleBP || debugFlagOn, "Update fetch stream %lu\n", fsqId);
    // }

    fsqId++;
    // // only if the stream is predicted to be ended can we inc fsqID
    // if (entry.getEnded()) {
    //     fsqId++;
    //     DPRINTF(DecoupleBP, "Inc fetch stream to %lu, because stream ends\n",
    //             fsqId);
    // }
    // printStream(entry);
    // checkHistory(s0History);
}

// void
// DecoupledBPUWithFTB::checkHistory(const boost::dynamic_bitset<> &history)
// {
//     unsigned ideal_size = 0;
//     boost::dynamic_bitset<> ideal_hash_hist(historyBits, 0);
//     for (const auto entry: historyManager.getSpeculativeHist()) {
//         if (entry.miss) {
//             continue;
//         }
//         ideal_size += 2;
//         Addr signature = computePathHash(entry.pc, entry.target);
//         DPRINTF(DecoupleBPVerbose, "%#lx->%#lx, signature: %#lx\n", entry.pc,
//                 entry.target, signature);
//         ideal_hash_hist <<= 2;
//         for (unsigned i = 0; i < historyTokenBits; i++) {
//             ideal_hash_hist[i] ^= (signature >> i) & 1;
//         }
//     }
//     unsigned comparable_size = std::min(ideal_size, historyBits);
//     boost::dynamic_bitset<> sized_real_hist(history);
//     ideal_hash_hist.resize(comparable_size);
//     sized_real_hist.resize(comparable_size);

//     boost::to_string(ideal_hash_hist, buf1);
//     boost::to_string(sized_real_hist, buf2);
//     DPRINTF(DecoupleBP,
//             "Ideal size:\t%u, real history size:\t%u, comparable size:\t%u\n",
//             ideal_size, historyBits, comparable_size);
//     DPRINTF(DecoupleBP, "Ideal history:\t%s\nreal history:\t%s\n",
//             buf1.c_str(), buf2.c_str());
//     assert(ideal_hash_hist == sized_real_hist);
// }

// bool
// DecoupledBPUWithFTB::popRAS(FetchStreamId stream_id, const char *when)
// {
//     if (streamRAS.empty()) {
//         DPRINTF(DecoupleBPRAS || debugFlagOn,
//                 "RAS is empty, cannot pop when %s\n", when);
//         return false;
//     }
//     DPRINTF(DecoupleBPRAS || debugFlagOn,
//             "Pop RA: %#lx when %s, stream id: %lu, ", streamRAS.top(), when,
//             stream_id);
//     finalPred.nextStream = streamRAS.top();
//     streamRAS.pop();
//     DPRINTFR(DecoupleBPRAS || debugFlagOn, "RAS size: %lu after action\n", streamRAS.size());
//     return true;
// }

// void
// DecoupledBPUWithFTB::pushRAS(FetchStreamId stream_id, const char *when, Addr ra)
// {
//     DPRINTF(DecoupleBPRAS || debugFlagOn,
//             "Push RA: %#lx when %s, stream id: %lu, ", ra, when, stream_id);
//     streamRAS.push(ra);
//     DPRINTFR(DecoupleBPRAS || debugFlagOn, "RAS size: %lu after action\n", streamRAS.size());
// }

// void
// DecoupledBPUWithFTB::updateTAGE(FetchStream &stream)
// {
//     defer _(nullptr, std::bind([this]{ debugFlagOn = false; }));
//     if (stream.startPC == ObservingPC) {
//         debugFlagOn = true;
//     }
//     auto res = streamLoopPredictor->updateTAGE(stream.startPC, stream.exeBranchPC, stream.exeTarget);
//     if (res.first) {
//         // loop up the tage and allocate for each divided stream
//         for (auto &it : res.second) {
//             auto last_chunk = computeLastChunkStart(it.branch, it.start);
//             DPRINTF(DecoupleBP || debugFlagOn, "Update divided chunk %#lx\n", last_chunk);
//             streamTAGE->update(last_chunk, it.start,
//                                it.branch,
//                                it.next,
//                                it.fallThruPC - it.branch,
//                                it.taken, stream.history,
//                                static_cast<EndType>(it.taken ? EndType::END_OTHER_TAKEN : EndType::END_NOT_TAKEN),
// 			                   stream.indexFoldedHist, stream.tagFoldedHist);
//         }
//     } else {
//         auto last_chunk = computeLastChunkStart(stream.getControlPC(), stream.startPC);
//         DPRINTF(DecoupleBP || debugFlagOn, "Update taken chunk %#lx\n", last_chunk);
//         streamTAGE->update(last_chunk, stream.startPC,
//                            stream.getControlPC(),
//                            stream.getNextStreamStart(),
//                            stream.getFallThruPC() - stream.getControlPC(),
//                            stream.getTaken(), stream.history,
//                            static_cast<EndType>(stream.endType),
// 			               stream.indexFoldedHist, stream.tagFoldedHist);
//     }
// }

// void
// DecoupledBPUWithFTB::storeLoopInfo(unsigned int fsqId, FetchStream stream)
// {
//     Addr branchPC = stream.squashType == SQUASH_CTRL ? stream.exeBranchPC : stream.predBranchPC;
//     if (dumpLoopPred && loopDetector->findLoop(branchPC)) {
//         storedLoopStreams.push_back(std::make_pair(fsqId, stream));
//     }
// }

void
DecoupledBPUWithFTB::resetPC(Addr new_pc)
{
    s0PC = new_pc;
    fetchTargetQueue.resetPC(new_pc);
}

}  // namespace ftb_pred

}  // namespace branch_prediction

}  // namespace gem5
