#include <cstring>
#include <list>

#include "arch/riscv/decoder.hh"
#include "base/types.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fetch/fetch.hh"
#include "debug/Activity.hh"
#include "debug/DecoupleBPProbe.hh"
#include "debug/Fetch.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

bool
Fetch::checkSignalsAndUpdate(ThreadID tid)
{
    // Update the per thread stall statuses.
    if (fromDecode->decodeBlock[tid]) {
        stalls[tid].decode = true;
    }

    if (fromDecode->decodeUnblock[tid]) {
        assert(stalls[tid].decode);
        assert(!fromDecode->decodeBlock[tid]);
        stalls[tid].decode = false;
    }

    // Check squash signals from commit.
    if (handleCommitSignals(tid)) {
        return true;
    }

    if (handleDecodeSquash(tid)) {
        return true;
    }

    if (checkStall(tid) && !hasPendingCacheRequests(tid)) {
        DPRINTF(Fetch, "[tid:%i] Setting to blocked\n",tid);

        setThreadStatus(tid, Blocked);

        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        DPRINTF(Fetch, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        setThreadStatus(tid, Running);

        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return false;
}

bool
Fetch::handleCommitSignals(ThreadID tid)
{
    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from commit.\n",tid);
        // In any case, squash.
        squash(*fromCommit->commitInfo[tid].pc,
               fromCommit->commitInfo[tid].doneSeqNum,
               fromCommit->commitInfo[tid].squashInst, tid);

        localSquashVer.update(fromCommit->commitInfo[tid].squashVersion.getVersion());
        DPRINTF(Fetch, "Updating squash version to %u\n",
                localSquashVer.getVersion());

        // If it was a branch mispredict on a control instruction, update the
        // branch predictor with that instruction, otherwise just kill the
        // invalid state we generated in after sequence number
        if (!isDecoupledFrontend()) {
            if (fromCommit->commitInfo[tid].mispredictInst &&
                fromCommit->commitInfo[tid].mispredictInst->isControl()) {
                branchPred->squash(fromCommit->commitInfo[tid].doneSeqNum,
                        *fromCommit->commitInfo[tid].pc,
                        fromCommit->commitInfo[tid].branchTaken, tid);
            } else {
                branchPred->squash(fromCommit->commitInfo[tid].doneSeqNum,
                                tid);
            }
        } else {
            auto mispred_inst = fromCommit->commitInfo[tid].mispredictInst;
            // TODO: write dbpftb conditions
            if (mispred_inst) {
                DPRINTF(Fetch, "Use mispred inst to redirect, treating as control squash\n");
                if (isStreamPred()) {
                    dbsp->controlSquash(
                        mispred_inst->getFtqId(), mispred_inst->getFsqId(),
                        mispred_inst->pcState(), *fromCommit->commitInfo[tid].pc,
                        mispred_inst->staticInst, mispred_inst->getInstBytes(),
                        fromCommit->commitInfo[tid].branchTaken,
                        mispred_inst->seqNum, tid);
                } else if (isFTBPred()) {
                    dbpftb->controlSquash(
                        mispred_inst->getFtqId(), mispred_inst->getFsqId(),
                        mispred_inst->pcState(), *fromCommit->commitInfo[tid].pc,
                        mispred_inst->staticInst, mispred_inst->getInstBytes(),
                        fromCommit->commitInfo[tid].branchTaken,
                        mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(),
                        true);
                } else if (isBTBPred()) {
                    dbpbtb->controlSquash(
                        mispred_inst->getFtqId(), mispred_inst->getFsqId(),
                        mispred_inst->pcState(), *fromCommit->commitInfo[tid].pc,
                        mispred_inst->staticInst, mispred_inst->getInstBytes(),
                        fromCommit->commitInfo[tid].branchTaken,
                        mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(),
                        true);
                }
            } else if (fromCommit->commitInfo[tid].isTrapSquash) {
                DPRINTF(Fetch, "Treating as trap squash\n",tid);
                if (isStreamPred()) {
                    dbsp->trapSquash(
                        fromCommit->commitInfo[tid].squashedTargetId,
                        fromCommit->commitInfo[tid].squashedStreamId,
                        fromCommit->commitInfo[tid].committedPC,
                        *fromCommit->commitInfo[tid].pc, tid);
                } else if (isFTBPred()) {
                    dbpftb->trapSquash(
                        fromCommit->commitInfo[tid].squashedTargetId,
                        fromCommit->commitInfo[tid].squashedStreamId,
                        fromCommit->commitInfo[tid].committedPC,
                        *fromCommit->commitInfo[tid].pc, tid, fromCommit->commitInfo[tid].squashedLoopIter);
                } else if (isBTBPred()) {
                    dbpbtb->trapSquash(
                        fromCommit->commitInfo[tid].squashedTargetId,
                        fromCommit->commitInfo[tid].squashedStreamId,
                        fromCommit->commitInfo[tid].committedPC,
                        *fromCommit->commitInfo[tid].pc, tid, fromCommit->commitInfo[tid].squashedLoopIter);
                }


            } else {
                if (fromCommit->commitInfo[tid].pc &&
                    fromCommit->commitInfo[tid].squashedStreamId != 0) {
                    DPRINTF(Fetch,
                            "Squash with stream id and target id from IEW\n");
                    if (isStreamPred()) {
                        dbsp->nonControlSquash(
                            fromCommit->commitInfo[tid].squashedTargetId,
                            fromCommit->commitInfo[tid].squashedStreamId,
                            *fromCommit->commitInfo[tid].pc, 0, tid);
                    } else if (isFTBPred()) {
                        dbpftb->nonControlSquash(
                            fromCommit->commitInfo[tid].squashedTargetId,
                            fromCommit->commitInfo[tid].squashedStreamId,
                            *fromCommit->commitInfo[tid].pc, 0, tid, fromCommit->commitInfo[tid].squashedLoopIter);
                    } else if (isBTBPred()) {
                        dbpbtb->nonControlSquash(
                            fromCommit->commitInfo[tid].squashedTargetId,
                            fromCommit->commitInfo[tid].squashedStreamId,
                            *fromCommit->commitInfo[tid].pc, 0, tid, fromCommit->commitInfo[tid].squashedLoopIter);
                    }
                } else {
                    DPRINTF(
                        Fetch,
                        "Dont squash dbq because no meaningful stream\n");
                }
            }
        }

        return true;
    } else if (fromCommit->commitInfo[tid].doneSeqNum) {
        // Update the branch predictor if it wasn't a squashed instruction
        // that was broadcasted.
        if (!isDecoupledFrontend()) {
            branchPred->update(fromCommit->commitInfo[tid].doneSeqNum, tid);
        } else {
            DPRINTF(DecoupleBP, "Commit stream Id: %lu\n",
                    fromCommit->commitInfo[tid].doneFsqId);
            if (isStreamPred()) {
                assert(dbsp);
                dbsp->update(fromCommit->commitInfo[tid].doneFsqId, tid);
            } else if (isFTBPred()) {
                assert(dbpftb);
                dbpftb->update(fromCommit->commitInfo[tid].doneFsqId, tid);
            } else if (isBTBPred()) {
                assert(dbpbtb);
                dbpbtb->update(fromCommit->commitInfo[tid].doneFsqId, tid);
            }
        }
    }

    return false;
}

bool
Fetch::handleDecodeSquash(ThreadID tid)
{
    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from decode.\n",tid);

        // Update the branch predictor.
        if (!isDecoupledFrontend()) {
            if (fromDecode->decodeInfo[tid].branchMispredict) {
                branchPred->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                                   *fromDecode->decodeInfo[tid].nextPC,
                                   fromDecode->decodeInfo[tid].branchTaken,
                                   tid);
            } else {
                branchPred->squash(fromDecode->decodeInfo[tid].doneSeqNum,
                                   tid);
            }
        // TODO: write dbpftb conditions
        } else {
            auto mispred_inst = fromDecode->decodeInfo[tid].mispredictInst;
            if (fromDecode->decodeInfo[tid].branchMispredict) {
                if (isStreamPred()) {
                    dbsp->controlSquash(
                        mispred_inst->getFtqId(), mispred_inst->getFsqId(),
                        mispred_inst->pcState(),
                        *fromDecode->decodeInfo[tid].nextPC,
                        mispred_inst->staticInst, mispred_inst->getInstBytes(),
                        fromDecode->decodeInfo[tid].branchTaken,
                        mispred_inst->seqNum, tid);
                } else if (isFTBPred()) {
                    dbpftb->controlSquash(
                        mispred_inst->getFtqId(), mispred_inst->getFsqId(),
                        mispred_inst->pcState(),
                        *fromDecode->decodeInfo[tid].nextPC,
                        mispred_inst->staticInst, mispred_inst->getInstBytes(),
                        fromDecode->decodeInfo[tid].branchTaken,
                        mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(),
                        false);
                } else if (isBTBPred()) {
                    dbpbtb->controlSquash(
                        mispred_inst->getFtqId(), mispred_inst->getFsqId(),
                        mispred_inst->pcState(),
                        *fromDecode->decodeInfo[tid].nextPC,
                        mispred_inst->staticInst, mispred_inst->getInstBytes(),
                        fromDecode->decodeInfo[tid].branchTaken,
                        mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(),
                        false);
                }
            } else {
                warn("Unexpected non-control squash from decode.\n");
            }
        }

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from decode with PC = %s\n",
                *fromDecode->decodeInfo[tid].nextPC);
            // Squash unless we're already squashing
            squashFromDecode(*fromDecode->decodeInfo[tid].nextPC,
                             fromDecode->decodeInfo[tid].squashInst,
                             fromDecode->decodeInfo[tid].doneSeqNum,
                             tid, 0);

            return true;
        }
    }

    return false;
}

void
Fetch::squash(PCStateBase &new_pc, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid, unsigned ftqIndex)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid, ftqIndex);
    assert(new_pc.instAddr() % 2 == 0 && "squash PC should be 2-byte aligned");

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}

void
Fetch::doSquash(PCStateBase &new_pc, const DynInstPtr squashInst, const InstSeqNum seqNum,
        ThreadID tid, unsigned ftqIndex)
{

    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s.\n",
            tid, new_pc);

    // restore vtype
    uint8_t restored_vtype = cpu->readMiscReg(RiscvISA::MISCREG_VTYPE, tid);
    for (auto& it : cpu->instList) {
        if (!it->isSquashed() &&
            it->seqNum <= seqNum &&
            it->staticInst->isVectorConfig()) {
            auto vset = static_cast<RiscvISA::VConfOp*>(it->staticInst.get());
            if (vset->vtypeIsImm) {
                restored_vtype = vset->earlyVtype;
            }
        }
    }
    decoder[tid]->as<RiscvISA::Decoder>().setVtype(restored_vtype);

    // align PC to 2 bytes
    // This handles cases where PC might be odd due to speculative execution,
    // but no need to throw INST_ADDR_MISALIGNED fault here
    if (new_pc.instAddr() % 2 != 0) {
        // Modify new_pc directly to make it 2-byte aligned
        auto& riscv_pc = new_pc.as<RiscvISA::PCState>();
        riscv_pc.set(new_pc.instAddr() & ~1);
        set(pc[tid], new_pc);
        DPRINTF(Fetch, "[tid:%i] pc is misaligned, aligned to %#lx\n", tid, new_pc.instAddr());
    } else {
        set(pc[tid], new_pc);
    }
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    DPRINTF(Fetch, "[tid:%i] Squash: clear cacheReq, current fetchStatus[tid]=%d\n", tid, fetchStatus[tid]);

    // Cancel all active cache requests in new status system
    CacheRequest& cacheReqRef = getCacheReq(tid, ftqIndex);
    cacheReqRef.cancelAllRequests();
    DPRINTF(Fetch, "[tid:%i] Squash: cancelled all cache requests, status: %s\n",
            tid, cacheReqRef.getStatusSummary().c_str());

    // Reset the cache request after cancelling
    cacheReqRef.reset();
    // Reset fetch2Coord state for dual FTQ support
    fetch2Coord[tid].reset();
    DPRINTF(Fetch, "[tid:%i] Squash: reset fetch2Coord state for dual FTQ support\n", tid);

    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == tid) {
        assert(cacheBlocked);
        for (auto it : retryPkt) {
            delete it;
        }
        retryPkt.clear();
        retryTid = InvalidThreadID;
        cacheBlocked = false;   // clear cache blocked
    }

    if (squashInst && !squashInst->isControl()) {
        // csrrw satp need to flush all fetch targets
        FetchBuffer& bufferRef = getFetchBuffer(tid, ftqIndex);
        bufferRef.valid = false;
    }

    setThreadStatus(tid, Squashing);
    setAllFetchStalls(StallReason::BpStall); // may caused by other stages like load and store

    // Empty fetch queue
    fetchQueue[tid].clear();

    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    // Set usedUpFetchTargets to force getting new FTQ entry after squash
    usedUpFetchTargets = true;
    FetchBuffer& bufferRef = getFetchBuffer(tid, ftqIndex);
    bufferRef.valid = false;  // clear fetch buffer valid

    DPRINTF(Fetch, "[tid:%i] Squash: set usedUpFetchTargets=true, will need new FTQ entry\n", tid);

    ++fetchStats.squashCycles;
}

void
Fetch::squashFromDecode(PCStateBase &new_pc, const DynInstPtr squashInst,
        const InstSeqNum seq_num, ThreadID tid, unsigned ftqIndex)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid, ftqIndex);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}

} // namespace o3
} // namespace gem5
