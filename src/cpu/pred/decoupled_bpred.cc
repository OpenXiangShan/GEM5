#include "cpu/pred/decoupled_bpred.hh"

namespace gem5
{
namespace branch_prediction
{

DecoupledBPU::DecoupledBPU(const DecoupledBPUParams &p)
    : BPredUnit(p), streamUBTB(p.stream_ubtb)
{
}

void
DecoupledBPU::tick()
{
    if (!squashing) {
        s0UbtbPred = streamUBTB->getStream();
        tryEnqFetchTarget();
        tryEnqFetchStream();

    } else {
        DPRINTF(DecoupleBP, "Squashing, skip this cycle\n");
    }

    streamUBTB->putPCHistory(s0StreamPC, s0History);
    squashing = false;
}

bool
DecoupledBPU::trySupplyFetchWithTarget()
{
    return fetchTargetQueue.trySupplyFetchWithTarget();
}


std::pair<bool, bool>
DecoupledBPU::decoupledPredict(const StaticInstPtr &inst,
                               const InstSeqNum &seqNum, PCStateBase &pc,
                               ThreadID tid)
{
    std::unique_ptr<PCStateBase> target(pc.clone());

    DPRINTF(DecoupleBP, "looking up pc %#lx\n", pc.instAddr());
    auto target_avail = fetchTargetQueue.fetchTargetAvailable();

    DPRINTF(DecoupleBP,
            "Supplying fetch with target ID %lu\n",
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

    assert(pc.instAddr() < end && pc.instAddr() >= start);
    bool taken = pc.instAddr() == taken_pc && target_to_fetch.taken;

    // npc could be end + 2
    auto &rpc = pc.as<GenericISA::PCStateWithNext>();
    bool run_out_of_this_entry = rpc.npc() >= end;

    if (taken) {
        auto &rtarget = target->as<GenericISA::PCStateWithNext>();
        rtarget.pc(target_to_fetch.target);
    } else {
        inst->advancePC(*target);
        if (target->instAddr() >= end) {
            run_out_of_this_entry = true;
        }
    }
    DPRINTF(DecoupleBP,
            "Predict PC %#lx %staken to %#lx\n",
            pc.instAddr(),
            taken ? "" : "not ",
            target->instAddr());
    set(pc, *target);

    if (run_out_of_this_entry) {
        // dequeue the entry
        DPRINTF(DecoupleBP,
                "running out of ftq entry %lu\n",
                fetchTargetQueue.getSupplyingTargetId());
        fetchTargetQueue.finishCurrentFetchTarget();
    }

    return std::make_pair(taken, run_out_of_this_entry);
}

void
DecoupledBPU::controlSquash(unsigned target_id, unsigned stream_id,
                            const PCStateBase &control_pc,
                            const PCStateBase &corr_target,
                            const StaticInstPtr &static_inst,
                            unsigned control_inst_size, bool actually_taken,
                            const InstSeqNum &seq, ThreadID tid)
{
    bool is_conditional = static_inst->isCondCtrl();
    bool is_indirect = static_inst->isIndirectCtrl();
    // bool is_call = static_inst->isCall();
    // bool is_return = static_inst->isReturn();
    DPRINTF(DecoupleBP,
            "Control squash: ftq_id=%lu, fsq_id=%lu, control_pc=0x%lx, "
            "corr_target=0x%lx, is_conditional=%u, "
            "is_indirect=%u, actually_taken=%u, branch seq: %lu\n",
            target_id,
            stream_id,
            control_pc.instAddr(),
            corr_target.instAddr(),
            is_conditional,
            is_indirect,
            actually_taken,
            seq);

    squashing = true;

    s0StreamPC = corr_target.instAddr();

    dumpFsq("Before control squash");

    // check sanity
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());
    auto &stream = it->second;

    FetchTargetId ftq_demand_stream_id;

    if (actually_taken) {
        DPRINTF(
            DecoupleBP,
            "a miss flow was redirected by taken branch, new fsq entry is:\n");
        printStream(stream);

        stream.exeEnded = true;
        stream.exeBranchAddr = control_pc.instAddr();
        stream.exeBranchType = 0;
        stream.exeTarget = corr_target.instAddr();
        stream.branchSeq = seq;
        stream.exeStreamEnd = stream.exeBranchAddr + control_inst_size;


        streamUBTB->update(stream_id,
                           stream.streamStart,
                           control_pc.instAddr(),
                           corr_target.instAddr(),
                           is_conditional,
                           is_indirect,
                           control_inst_size,
                           actually_taken,
                           stream.history);

        // clear younger fsq entries
        auto erase_it = fetchStreamQueue.upper_bound(stream_id);
        while (erase_it != fetchStreamQueue.end()) {
            DPRINTF(DecoupleBP, "erasing entry %lu\n", erase_it->first);
            printStream(erase_it->second);
            fetchStreamQueue.erase(erase_it++);
        }

        // inc stream id because current stream ends
        ftq_demand_stream_id = stream_id + 1;
        // todo update stream head id here
        // fsqID = stream_id + 1;

    } else {
        DPRINTF(DecoupleBP,
                "a taken flow was redirected by NOT taken branch, new fsq "
                "entry is:\n");
        stream.exeEnded = false;     // its ned has not be found yet
        stream.streamEnded = false;  // convert it to missing stream
        stream.predStreamEnd = 0;
        stream.predBranchAddr = 0;

        // keep stream id because still in the same stream
        ftq_demand_stream_id = stream_id;
        // todo update stream head id here
        // fsqID = stream_id;

        printStream(stream);

        // clear younger fsq entries
        auto erase_it = fetchStreamQueue.upper_bound(stream_id);
        while (erase_it != fetchStreamQueue.end()) {
            DPRINTF(DecoupleBP, "erasing entry %lu\n", erase_it->first);
            printStream(erase_it->second);
            fetchStreamQueue.erase(erase_it++);
        }
    }
    dumpFsq("After control squash");

    s0UbtbPred.valid = false;

    fetchTargetQueue.squash(
        target_id + 1, ftq_demand_stream_id, corr_target.instAddr());
}

void
DecoupledBPU::nonControlSquash(unsigned target_id, unsigned stream_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid)
{
    DPRINTF(DecoupleBP,
            "non control squash: target id: %lu, stream id: %lu, inst_pc: %x, "
            "seq: %lu\n",
            target_id,
            stream_id,
            inst_pc.instAddr(),
            seq);
    squashing = true;

    dumpFsq("before non-control squash");

    // make sure the stream is in FSQ
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());

    // Clear potential resolved results and set to predicted result
    auto &stream = it->second;
    stream.setDefaultResolve();
    stream.branchSeq = -1;

    // Fetching from the original predicted fsq entry, since this is not a
    // mispredict. We allocate a new target id to avoid alias
    auto pc = inst_pc.instAddr();
    fetchTargetQueue.squash(target_id + 1, stream_id + 1, pc);

    // we should set s0StreamPC to be the predTarget of the fsq entry
    // in case that the stream is the newest entry in fsq,
    // That's because, if an inst of this stream had been squashed due to a
    // mispredict, and then a non-control squash was made on an inst oldder
    // than this inst, the s0StreamPC would be set incorrectly by the previous
    // squash
    Addr target = stream.predTarget;
    bool this_entry_ended = stream.streamEnded;
    if (++it == fetchStreamQueue.end() && this_entry_ended) {
        s0StreamPC = target;
        fsqId = stream_id + 1;
    }
}

void
DecoupledBPU::update(unsigned stream_id, ThreadID tid)
{
    // aka, commit stream
    // commit controls in local prediction history buffer to committedSeq
    // mark all committed control instructions as correct
    // do not need to dequeue when empty
    if (fetchStreamQueue.empty())
        return;
    auto it = fetchStreamQueue.begin();
    while (it != fetchStreamQueue.end() && stream_id >= it->first) {
        // TODO: do update here

        // dequeue
        DPRINTF(DecoupleBP,
                "dequeueing stream id: %lu, entry below:\n",
                it->first);
        it = fetchStreamQueue.erase(it);
    }
    DPRINTF(DecoupleBP,
            "after commit stream, fetchStreamQueue size: %lu\n",
            fetchStreamQueue.size());
    printStream(it->second);
}

unsigned
DecoupledBPU::getSupplyingTargetId(ThreadID tid)
{
    panic("not implemented\n");
}

unsigned
DecoupledBPU::getSupplyingStreamId(ThreadID tid)
{
    panic("not implemented\n");
}

void
DecoupledBPU::dumpFsq(const char *when)
{
    DPRINTF(DecoupleBP, "dumping fsq entries %s...\n", when);
    for (auto it = fetchStreamQueue.begin(); it != fetchStreamQueue.end();
         it++) {
        DPRINTFR(DecoupleBP, "StreamID %lu, ", it->first);
        printStream(it->second);
    }
}

void
DecoupledBPU::tryEnqFetchStream()
{
}

void
DecoupledBPU::tryEnqFetchTarget()
{
}

void
DecoupledBPU::makeNewPredictionAndInsertFsq()
{
}

}  // namespace branch_prediction

}  // namespace gem5
