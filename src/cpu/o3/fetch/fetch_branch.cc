#include "base/types.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fetch/fetch.hh"
#include "debug/DecoupleBPProbe.hh"
#include "debug/Fetch.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"

namespace gem5
{

namespace o3
{

bool
Fetch::lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc, unsigned ftqIndex)
{
    // Do branch prediction check here.
    // A bit of a misnomer...next_PC is actually the current PC until
    // this function updates it.
    bool predict_taken;

    //  BP  =>  FSQ  =>  FTB  => Fetch
    ThreadID tid = inst->threadNumber;
    if (isDecoupledFrontend()) {
        if (isStreamPred()) {
            std::tie(predict_taken, usedUpFetchTargets) =
                dbsp->decoupledPredict(
                    inst->staticInst, inst->seqNum, next_pc, tid);
            if (usedUpFetchTargets) {
                DPRINTF(DecoupleBP, "Used up fetch targets.\n");
                fetchBuffer[tid][ftqIndex].valid = false;  // Invalidate fetch buffer when FTQ entry exhausted
            }
        }
        else  {
            if (isFTBPred()) {
                std::tie(predict_taken, usedUpFetchTargets) =
                    dbpftb->decoupledPredict(
                        inst->staticInst, inst->seqNum, next_pc, tid, currentLoopIter);
            } else if (isBTBPred()) {
                std::tie(predict_taken, usedUpFetchTargets) =
                    dbpbtb->decoupledPredict(
                        inst->staticInst, inst->seqNum, next_pc, tid, currentLoopIter);
            }
            if (usedUpFetchTargets) {
                DPRINTF(DecoupleBP, "Used up fetch targets.\n");
                fetchBuffer[tid][ftqIndex].valid = false;  // Invalidate fetch buffer when FTQ entry exhausted
            }
            inst->setLoopIteration(currentLoopIter);
        }
    }

    // For decoupled frontend, the instruction type is predicted with BTB
    if ((isDecoupledFrontend() && !predict_taken) ||
        (!isDecoupledFrontend() && !inst->isControl())) {
        inst->staticInst->advancePC(next_pc);
        inst->setPredTarg(next_pc);
        inst->setPredTaken(false);
        return false;
    }

    if (!isDecoupledFrontend()) {
        predict_taken = branchPred->predict(inst->staticInst, inst->seqNum,
                                            next_pc, tid);
    }

    if (predict_taken) {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be taken to %s\n",
                tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    } else {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be not taken\n",
                tid, inst->seqNum, inst->pcState().instAddr());
    }

    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
            "predicted to go to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    inst->setPredTarg(next_pc);
    inst->setPredTaken(predict_taken);

    ++fetchStats.branches;

    if (predict_taken) {
        ++fetchStats.predictedBranches;
    }

    return predict_taken;
}

void
Fetch::updateBranchPredictors()
{
    if (isStreamPred()) {
        assert(dbsp);
        dbsp->tick();
        usedUpFetchTargets = !dbsp->trySupplyFetchWithTarget(pc[0]->instAddr());
    } else if (isFTBPred()) {
        assert(dbpftb);
        // TODO: remove ideal_tick()
        if (dbpftb->enableTwoTaken){
            dbpftb->ideal_tick();
        } else {
            dbpftb->tick();
        }
        usedUpFetchTargets = !dbpftb->trySupplyFetchWithTarget(pc[0]->instAddr(), currentFetchTargetInLoop);
    } else if (isBTBPred()) {
        assert(dbpbtb);
        dbpbtb->tick();
        usedUpFetchTargets = !dbpbtb->trySupplyFetchWithTarget(pc[0]->instAddr(), currentFetchTargetInLoop);
    }
}

// ============================================
// checkDecoupledFrontend
// ============================================

bool
Fetch::checkDecoupledFrontend(ThreadID tid)
{
    if (!isDecoupledFrontend()) {
        return true; // No decoupled frontend to check
    }

    if (isStreamPred()) {
        if (!dbsp->fetchTargetAvailable()) {
            DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
            setAllFetchStalls(StallReason::FTQBubble);
            return false;
        }
    } else if (isFTBPred()) {
        if (!dbpftb->fetchTargetAvailable()) {
            dbpftb->addFtqNotValid();
            DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
            setAllFetchStalls(StallReason::FTQBubble);
            return false;
        }
    } else if (isBTBPred()) {
        if (!dbpbtb->fetchTargetAvailable()) {
            dbpbtb->addFtqNotValid();
            DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
            return false;
        }
    }

    return true;
}

bool
Fetch::needNewFTQEntry(ThreadID tid, unsigned ftqIndex)
{
    // Check if we need a new FTQ entry based on:
    // 1. Used up current FTQ targets (decoupled frontend)
    // 2. Invalid fetch buffer (cache miss or initial state)
    bool need_new = usedUpFetchTargets || !fetchBuffer[tid][ftqIndex].valid;

    // Assert consistency: if usedUpFetchTargets=true, fetchBuffer should be invalid
    if (isDecoupledFrontend() && usedUpFetchTargets) {
        assert(!fetchBuffer[tid][ftqIndex].valid &&
               "fetchBuffer should be invalid when FTQ entry is exhausted");
    }

    DPRINTF(Fetch, "[tid:%i][ftq:%d] needNewFTQEntry: usedUpFetchTargets=%d, "
            "fetchBufferValid=%d, result=%d\n",
            tid, ftqIndex, usedUpFetchTargets, fetchBuffer[tid][ftqIndex].valid, need_new);

    return need_new;
}

Addr
Fetch::getNextFTQStartPC(ThreadID tid, unsigned ftqIndex)
{
    assert(isDecoupledFrontend());

    // When we need a new FTQ entry, try to supply fetch with the next target immediately
    if (usedUpFetchTargets) {
        DPRINTF(Fetch, "[tid:%i] usedUpFetchTargets=true, trying to get next FTQ entry\n", tid);

        bool in_loop = false;
        bool got_target = false;

        if (isBTBPred()) {
            got_target = dbpbtb->trySupplyFetchWithTarget(pc[tid]->instAddr(), in_loop);
        } else if (isFTBPred()) {
            got_target = dbpftb->trySupplyFetchWithTarget(pc[tid]->instAddr(), in_loop);
        } else if (isStreamPred()) {
            got_target = dbsp->trySupplyFetchWithTarget(pc[tid]->instAddr());
        }

        if (got_target) {
            DPRINTF(Fetch, "[tid:%i] Successfully got next FTQ entry, resetting usedUpFetchTargets\n", tid);
            usedUpFetchTargets = false;  // Reset flag since we got a new FTQ entry
            // Note: fetchBufferValid[tid] will be set to true later when cache line is fetched
        } else {
            DPRINTF(Fetch, "[tid:%i] Failed to get next FTQ entry, should stall fetch until FTQ available\n", tid);
            // Don't fallback to old address, return 0 to indicate stall needed
            return 0;  // Signal that fetch should stall
        }
    }

    // Now get the current supplying FTQ entry
    if (isBTBPred()) {
        assert(dbpbtb);
        auto& ftq_entry = dbpbtb->getSupplyingFetchTarget();
        Addr start_pc = ftq_entry.startPC;

        // Update fetchBufferPC to align with FTQ entry
        fetchBuffer[tid][ftqIndex].startPC = start_pc;

        DPRINTF(Fetch, "[tid:%i][ftq:%d] getNextFTQStartPC: FTQ entry startPC=%#x, "
                "endPC=%#x, fetchBufferPC updated to %#x\n",
                tid, ftqIndex, start_pc, ftq_entry.endPC, fetchBuffer[tid][ftqIndex].startPC);

        return start_pc;
    } else if (isFTBPred()) {
        assert(dbpftb);
        auto& ftq_entry = dbpftb->getSupplyingFetchTarget();
        Addr start_pc = ftq_entry.startPC;
        fetchBuffer[tid][ftqIndex].startPC = start_pc;

        DPRINTF(Fetch, "[tid:%i][ftq:%d] getNextFTQStartPC: FTB entry startPC=%#x, "
                "endPC=%#x, fetchBufferPC updated to %#x\n",
                tid, ftqIndex, start_pc, ftq_entry.endPC, fetchBuffer[tid][ftqIndex].startPC);

        return start_pc;
    } else if (isStreamPred()) {
        // For stream predictor, fall back to current fetchBufferPC
        DPRINTF(Fetch, "[tid:%i][ftq:%d] getNextFTQStartPC: Stream predictor fallback, "
                "using fetchBufferPC=%#x\n", tid, ftqIndex, fetchBuffer[tid][ftqIndex].startPC);
        return fetchBuffer[tid][ftqIndex].startPC;
    }

    panic("getNextFTQStartPC called with unsupported predictor type");
    return 0;
}

void
Fetch::finishCurrentFetchTarget()
{
    // Process the completion of a fetch target queue entry
    if (isBTBPred() && dbpbtb) {
        dbpbtb->finishCurrentFetchTarget();
    }
}

Addr
Fetch::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    if (isFTBPred()) {
        return dbpftb->getPreservedReturnAddr(dynInst);
    } else if (isBTBPred()) {
        return dbpbtb->getPreservedReturnAddr(dynInst);
    } else {
        panic("getPreservedReturnAddr not implemented for this bpu");
        return 0;
    }
}

} // namespace o3
} // namespace gem5
