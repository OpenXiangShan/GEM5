#include "cpu/pred/decoupled_bpred.hh"

namespace gem5
{
namespace branch_prediction
{

DecoupledBPU::DecoupledBPU(const DecoupledBPUParams &p)
    : BPredUnit(p), fetchTargetQueue(p.ftq_size), streamUBTB(p.stream_ubtb)
{
    assert(streamUBTB);

    // TODO: remove this
    fetchStreamQueueSize = 64;
    s0StreamPC = 0x80000000;

    s0History.resize(historyBits, 0);
    fetchTargetQueue.setName(name());

    commitHistory.resize(historyBits, 0);
    squashing = true;
}

void
DecoupledBPU::tick()
{
    if (!squashing) {
        DPRINTF(DecoupleBP, "DecoupledBPU::tick()\n");
        s0UbtbPred = streamUBTB->getStream();
        tryEnqFetchTarget();
        tryEnqFetchStream();

    } else {
        DPRINTF(DecoupleBP, "Squashing, skip this cycle\n");
    }

    streamUBTB->tickStart();
    if (!streamQueueFull()) {
        streamUBTB->putPCHistory(s0StreamPC, s0History);
    }
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

    assert(pc.instAddr() < end && pc.instAddr() >= start);
    bool taken = pc.instAddr() == taken_pc && target_to_fetch.taken;

    bool run_out_of_this_entry = false;

    if (taken) {
        auto &rtarget = target->as<GenericISA::PCStateWithNext>();
        rtarget.pc(target_to_fetch.target);
        // TODO???
        // rtarget.npc(target_to_fetch.target + 4);
        rtarget.uReset();
        DPRINTF(DecoupleBP,
                "Predicted pc: %#lx, upc: %#lx, npc(meaningless): %#lx\n",
                target->instAddr(), rtarget.upc(), rtarget.npc());
        set(pc, *target);
        run_out_of_this_entry = true;
    } else {
        // npc could be end + 2
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
            target_id, stream_id, control_pc.instAddr(),
            corr_target.instAddr(), is_conditional, is_indirect,
            actually_taken, seq);

    squashing = true;

    s0StreamPC = corr_target.instAddr();

    dumpFsq("Before control squash");

    // check sanity
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());
    auto &stream = it->second;

    FetchTargetId ftq_demand_stream_id;
    stream.resolved = true;

    if (actually_taken) {
        stream.exeEnded = true;
        stream.exeBranchAddr = control_pc.instAddr();
        stream.exeBranchType = 0;
        stream.exeTarget = corr_target.instAddr();
        stream.branchSeq = seq;
        stream.exeStreamEnd = stream.exeBranchAddr + control_inst_size;


        streamUBTB->update(stream_id, stream.streamStart,
                           control_pc.instAddr(), corr_target.instAddr(),
                           is_conditional, is_indirect, control_inst_size,
                           actually_taken, stream.history);

        // clear younger fsq entries
        auto erase_it = fetchStreamQueue.upper_bound(stream_id);
        while (erase_it != fetchStreamQueue.end()) {
            DPRINTF(DecoupleBP, "erasing entry %lu\n", erase_it->first);
            printStream(erase_it->second);
            fetchStreamQueue.erase(erase_it++);
        }

        std::string buf1, buf2;
        boost::to_string(s0History, buf1);
        boost::to_string(stream.history, buf2);
        DPRINTF(DecoupleBP, "Recover history %s\nto %s\n", buf1.c_str(),
                buf2.c_str());
        s0History = stream.history;
        auto hashed_path =
            computePathHash(control_pc.instAddr(), corr_target.instAddr());
        histShiftIn(hashed_path, s0History);
        boost::to_string(s0History, buf1);
        DPRINTF(DecoupleBP, "Shift in history %s\n", buf1.c_str());

        // inc stream id because current stream ends
        ftq_demand_stream_id = stream_id + 1;

        // todo update stream head id here
        fsqId = stream_id + 1;

        DPRINTF(DecoupleBP,
                "a %s flow was redirected by taken branch, "
                "new fsq entry is:\n",
                stream.streamEnded ? "pred-longer" : "miss");
        printStream(stream);

    } else {
        stream.exeEnded = false;     // its end has not be found yet
        stream.streamEnded = false;  // convert it to missing stream
        stream.predStreamEnd = 0;
        stream.predBranchAddr = 0;
        stream.resolved = false;

        // keep stream id because still in the same stream
        ftq_demand_stream_id = stream_id;
        // todo update stream head id here
        fsqId = stream_id;

        // clear younger fsq entries
        auto erase_it = fetchStreamQueue.upper_bound(stream_id);
        while (erase_it != fetchStreamQueue.end()) {
            DPRINTF(DecoupleBP, "erasing entry %lu\n", erase_it->first);
            printStream(erase_it->second);
            fetchStreamQueue.erase(erase_it++);
        }
        DPRINTF(DecoupleBP,
                "a taken flow was redirected by NOT taken branch, new fsq "
                "entry is:\n");
        printStream(stream);
    }
    dumpFsq("After control squash");

    s0UbtbPred.valid = false;

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            corr_target.instAddr());

    fetchTargetQueue.dump("After control squash");

    DPRINTF(DecoupleBP,
            "After squash, FSQ head Id=%lu, demand stream Id=%lu, Fetch "
            "demanded target Id=%lu\n",
            fsqId, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void
DecoupledBPU::nonControlSquash(unsigned target_id, unsigned stream_id,
                               const PCStateBase &inst_pc,
                               const InstSeqNum seq, ThreadID tid)
{
    DPRINTF(DecoupleBP,
            "non control squash: target id: %lu, stream id: %lu, inst_pc: %x, "
            "seq: %lu\n",
            target_id, stream_id, inst_pc.instAddr(), seq);
    squashing = true;

    dumpFsq("before non-control squash");

    // make sure the stream is in FSQ
    auto it = fetchStreamQueue.find(stream_id);
    assert(it != fetchStreamQueue.end());

    auto start = it->second.streamStart;
    auto end = it->second.resolved ? it->second.exeBranchAddr
                                   : it->second.predBranchAddr;
    auto target =
        it->second.resolved ? it->second.exeTarget : it->second.predTarget;

    auto ftq_demand_stream_id = stream_id;

    DPRINTF(DecoupleBP, "non control squash: start: %x, end: %x, target: %x\n",
            start, end, target);

    if (end) {
        if (start <= inst_pc.instAddr() && inst_pc.instAddr() <= end) {
            // this pc is in the stream
        } else if (inst_pc.instAddr() == target) {
            // this pc is in the next stream!
            DPRINTF(DecoupleBP,
                    "Redirected PC is the target of the stream, "
                    "indicating that the stream is ended, switch to next "
                    "stream\n");
            ++it;
            ftq_demand_stream_id = stream_id + 1;
        } else {
            panic("this pc is in the last stream ?!");
        }
    }


    // Clear potential resolved results and set to predicted result
    auto &stream = it->second;
    // stream.setDefaultResolve();
    // stream.branchSeq = -1;


    // Fetching from the original predicted fsq entry, since this is not a
    // mispredict. We allocate a new target id to avoid alias
    auto pc = inst_pc.instAddr();
    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id, pc);

    // we should set s0StreamPC to be the predTarget of the fsq entry
    // in case that the stream is the newest entry in fsq,
    // That's because, if an inst of this stream had been squashed due to a
    // mispredict, and then a non-control squash was made on an inst oldder
    // than this inst, the s0StreamPC would be set incorrectly by the previous
    // squash
    target = stream.resolved ? stream.exeTarget
                             : stream.predTarget;  // target might be updated
    bool this_entry_ended =
        stream.resolved ? stream.exeStreamEnd : stream.streamEnded;
    if (++it == fetchStreamQueue.end() && this_entry_ended) {
        s0StreamPC = target;
        fsqId = (--it)->first + 1;
    }
    dumpFsq("after non-control squash");
    DPRINTF(DecoupleBP,
            "After squash, FSQ head Id=%lu, s0pc=%#lx, demand stream Id=%lu, "
            "Fetch demanded target Id=%lu\n",
            fsqId, s0StreamPC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void
DecoupledBPU::trapSquash(unsigned target_id, unsigned stream_id,
                         const PCStateBase &inst_pc, ThreadID tid)
{
    DPRINTF(DecoupleBP,
            "trap squash: target id: %lu, stream id: %lu, inst_pc: %#lx\n",
            target_id, stream_id, inst_pc.instAddr());
    squashing = true;

    dumpFsq("before trap squash");

    auto erase_it = fetchStreamQueue.upper_bound(stream_id);
    while (erase_it != fetchStreamQueue.end()) {
        DPRINTF(DecoupleBP, "erasing entry %lu\n", erase_it->first);
        printStream(erase_it->second);
        fetchStreamQueue.erase(erase_it++);
    }

    // inc stream id because current stream is disturbed
    auto ftq_demand_stream_id = stream_id + 1;
    // todo update stream head id here
    fsqId = stream_id + 1;

    fetchTargetQueue.squash(target_id + 1, ftq_demand_stream_id,
                            inst_pc.instAddr());

    s0StreamPC = inst_pc.instAddr();

    DPRINTF(DecoupleBP,
            "After squash, FSQ head Id=%lu, s0pc=%#lx, demand stream Id=%lu, "
            "Fetch demanded target Id=%lu\n",
            fsqId, s0StreamPC, fetchTargetQueue.getEnqState().streamId,
            fetchTargetQueue.getSupplyingTargetId());
}

void DecoupledBPU::update(unsigned stream_id, ThreadID tid)
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
        DPRINTF(DecoupleBP, "dequeueing stream id: %lu, entry below:\n",
                it->first);
        it = fetchStreamQueue.erase(it);
    }
    DPRINTF(DecoupleBP, "after commit stream, fetchStreamQueue size: %lu\n",
            fetchStreamQueue.size());
    printStream(it->second);
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
    if (!streamQueueFull()) {
        // if queue empty, should make predictions
        if (fetchStreamQueue.empty()) {
            DPRINTF(DecoupleBP, "FSQ is empty\n");
            makeNewPredictionAndInsertFsq();
        } else {
            auto it = fetchStreamQueue.end();
            it--;
            auto &back = it->second;
            if (back.streamEnded || back.exeEnded) {
                DPRINTF(DecoupleBP,
                        "FSQ is not empty, and the latest stream %lu (%#lx) "
                        "has ended\n",
                        it->first, it->second.streamStart);
                // make new predictions
                makeNewPredictionAndInsertFsq();
            } else {
                // check if current stream hits after miss
                DPRINTF(DecoupleBP,
                        "FSQ is not empty, but stream %lu has not ended:\n",
                        it->first);
                printStream(back);
                bool hit = false;  // TODO: use prediction result
                // if hit, do something
                if (hit) {
                    // TODO: use prediction result
                    back.streamEnded = true;
                    back.predStreamEnd = 0;
                    back.predTarget = 0;
                    back.predBranchAddr = 0;
                    back.predBranchType = 0;
                    back.hasEnteredFtq = false;
                    s0StreamPC = back.predTarget;
                    DPRINTF(DecoupleBP, "fsq entry of id %lu modified to:\n",
                            it->first);
                    // pred ended, inc fsqID
                    fsqId++;
                    printStream(back);
                } else {
                    // streamMiss = true;
                    s0StreamPC += 0x40;
                    DPRINTF(DecoupleBP, "s0StreamPC update to %#lx\n",
                            s0StreamPC);
                }
            }
        }
    } else {
        DPRINTF(DecoupleBP, "FSQ is full: %lu\n", fetchStreamQueue.size());
    }
    DPRINTF(DecoupleBP, "fsqId=%lu\n", fsqId);
}

void
DecoupledBPU::tryEnqFetchTarget()
{
    DPRINTF(DecoupleBP, "Try to enq fetch target\n");
    if (!fetchTargetQueue.full()) {
        // ftq can accept new cache lines,
        // get cache lines from fetchStreamQueue
        if (!fetchStreamQueue.empty()) {
            // find current stream with ftqEnqfsqID in fetchStreamQueue
            auto &ftq_enq_state = fetchTargetQueue.getEnqState();
            auto it = fetchStreamQueue.find(ftq_enq_state.streamId);
            if (it != fetchStreamQueue.end()) {
                auto stream_to_enq = it->second;
                Addr end = stream_to_enq.resolved
                               ? stream_to_enq.exeStreamEnd
                               : stream_to_enq.predStreamEnd;
                Addr baddr = stream_to_enq.resolved
                                 ? stream_to_enq.exeBranchAddr
                                 : stream_to_enq.predBranchAddr;
                bool ended = stream_to_enq.resolved
                                 ? stream_to_enq.exeEnded
                                 : stream_to_enq.streamEnded;
                DPRINTF(DecoupleBP, "Try to enque with stream: ID=%i ",
                        it->first);
                printStream(it->second);
                // DPRINTF(DecoupleBP, "tryToEnqFtq: enq stream %d,
                // pc:0x%lx\n", ftqEnqFsqID, s0StreamPC);
                DPRINTF(DecoupleBP, "Serve enq PC: %#lx with stream:\n",
                        ftq_enq_state.pc);
                printStream(stream_to_enq);
                // dumpFsq("for debugging steam enq");
                DPRINTF(DecoupleBP, "ftqEnqPC < StreamEnd: %d\n",
                        ftq_enq_state.pc < end);
                // assert((ftqEnqPC <= s0StreamPC &&
                // !stream_to_enq.streamEnded)
                if (ended) {
                    assert(ftq_enq_state.pc < end);
                }
                auto ftq_entry = FtqEntry();
                if (!stream_to_enq.hasEnteredFtq) {
                    ftq_entry.startPC = stream_to_enq.streamStart;
                    stream_to_enq.hasEnteredFtq = true;
                    it->second = stream_to_enq;
                } else {
                    ftq_entry.startPC = ftq_enq_state.pc;
                }
                ftq_enq_state.pc = alignToCacheLine(ftq_entry.startPC + 0x40);
                DPRINTF(DecoupleBP, "Update ftqEnqPC from %#lx to %#lx\n",
                        ftq_entry.startPC, ftq_enq_state.pc);

                // check if this is the last cache line of the stream
                bool end_in_line =
                    (end - alignToCacheLine(ftq_entry.startPC)) <= 0x40;
                // whether the first byte of the branch instruction lies in
                // this cache line
                bool branch_in_line =
                    (baddr - alignToCacheLine(ftq_entry.startPC)) < 0x40;

                DPRINTF(DecoupleBP,
                        "end in line: %d, branch in line: %d, pred end: %d, "
                        "exe end: %d\n",
                        end_in_line, branch_in_line, stream_to_enq.streamEnded,
                        stream_to_enq.exeEnded);
                ftq_entry.fsqID = ftq_enq_state.streamId;
                if ((end_in_line || branch_in_line) && ended) {
                    ftq_entry.taken = true;
                    if (!stream_to_enq.resolved) {
                        ftq_entry.endPC = stream_to_enq.predStreamEnd;
                        ftq_entry.takenPC = stream_to_enq.predBranchAddr;
                        ftq_entry.target = stream_to_enq.predTarget;
                    } else {
                        ftq_entry.endPC = stream_to_enq.exeStreamEnd;
                        ftq_entry.takenPC = stream_to_enq.exeBranchAddr;
                        ftq_entry.target = stream_to_enq.exeTarget;
                    }
                    // done with this stream
                    // dumpFsq("Before update ftqEnqFsqID");
                    // DPRINTF(DecoupleBP,
                    //         "Stream %lu entered ftq %lu\n",
                    //         ftq_enq_state.streamId,
                    //         ftq_enq_state.desireTargetId);

                    DPRINTF(DecoupleBP, "Enqueue FTQ with ended stream %lu\n",
                            ftq_enq_state.streamId);
                    printStream(stream_to_enq);

                    ftq_enq_state.streamId++;
                    ftq_enq_state.pc = ftq_entry.target;
                    DPRINTF(DecoupleBP,
                            "Update ftqEnqPC to %#lx, FTQ demand stream ID to "
                            "%u, because stream ends\n",
                            ftq_enq_state.pc, ftq_enq_state.streamId);
                } else {
                    // align to the end of current cache line
                    ftq_entry.endPC =
                        alignToCacheLine(ftq_entry.startPC + 0x40);

                    ftq_entry.taken = false;
                    ftq_entry.takenPC = 0;
                    DPRINTF(DecoupleBP,
                            "Enqueue FTQ with continuous stream %lu\n",
                            ftq_enq_state.streamId);
                    printStream(stream_to_enq);
                }
                assert(ftq_enq_state.streamId <= fsqId + 1);
                fetchTargetQueue.enqueue(ftq_entry);
                ftq_enq_state.desireTargetId++;
                DPRINTF(DecoupleBP, "a %s stream inc ftqID: %lu -> %lu\n",
                        ended ? "ended" : "miss",
                        ftq_enq_state.desireTargetId - 1,
                        ftq_enq_state.desireTargetId);
                printFetchTarget(ftq_entry, "Insert to FTQ");
                fetchTargetQueue.dump("After insert new entry");
            } else {
                DPRINTF(DecoupleBP, "FTQ enq Stream ID %u is not found\n",
                        ftq_enq_state.streamId);
            }
        } else {
            // no stream that have not entered ftq
            DPRINTF(DecoupleBP,
                    "No stream to enter ftq in fetchStreamQueue\n");
        }
    } else {
        DPRINTF(DecoupleBP, "FTQ is full\n");
    }
}

Addr
DecoupledBPU::computePathHash(Addr br, Addr target)
{
    Addr ret = 0;
    for (unsigned i = 0; i < numFoldingTokens(); i++) {
        ret ^= (br >> (i * historyTokenBits)) & foldingTokenMask();
    }
    for (unsigned i = 0; i < numFoldingTokens(); i++) {
        ret ^= (target >> (i * historyTokenBits)) & foldingTokenMask();
    }
    return ret;
}

void
DecoupledBPU::histShiftIn(Addr hash, boost::dynamic_bitset<> &history)
{
    std::string buf;
    boost::to_string(history, buf);
    // DPRINTF(DecoupleBP, "Hist before shiftin: %s, hist len: %u, hash:
    // %#lx\n", buf.c_str(), history.size(), hash); DPRINTF(DecoupleBP, "Reach
    // x\n");
    history <<= 2;
    boost::dynamic_bitset<> temp_hash_bits(historyBits, hash);
    boost::to_string(temp_hash_bits, buf);
    // DPRINTF(DecoupleBP, "hash to shiftin: %s\n", buf.c_str());
    history ^= temp_hash_bits;
    boost::to_string(history, buf);
    DPRINTF(DecoupleBP, "Hist after shiftin: %s\n", buf.c_str());
}

void
DecoupledBPU::makeNewPredictionAndInsertFsq()
{
    DPRINTF(DecoupleBP, "Try to make new prediction and insert Fsq\n");
    FetchStream entry;
    // TODO: this may be wrong, need to check if we should use the last
    // s0StreamPC
    entry.streamStart = s0StreamPC;
    if (s0UbtbPred.valid) {
        entry.streamEnded = true;
        entry.predStreamEnd = s0UbtbPred.controlAddr + s0UbtbPred.controlSize;
        entry.predBranchAddr = s0UbtbPred.controlAddr;
        entry.predTarget = s0UbtbPred.nextStream;
        s0StreamPC = s0UbtbPred.nextStream;
        entry.history = s0UbtbPred.history;
        auto hashed_path = computePathHash(s0UbtbPred.controlAddr,
                                            s0UbtbPred.nextStream);
        std::string buf1, buf2;
        boost::to_string(s0History, buf1);
        histShiftIn(hashed_path, s0History);
        boost::to_string(s0History, buf2);
        DPRINTF(DecoupleBP, "Update s0History form %s to %s\n", buf1.c_str(),
                buf2.c_str());
        DPRINTF(DecoupleBP, "Hashed path: %#lx\n", hashed_path);
        DPRINTF(DecoupleBP, "Valid s0UbtbPred: %#lx-[%#lx, %#lx) --> %#lx\n",
                entry.streamStart, entry.predBranchAddr, entry.predStreamEnd,
                entry.predTarget);
    } else {
        DPRINTF(DecoupleBP,
                "No valid prediction, gen missing stream: %#lx -> ...\n",
                s0StreamPC);
        entry.streamEnded = false;
        entry.history.resize(historyBits);
        DPRINTF(DecoupleBP, "entry hist size: %lu, ubtb hist size: %lu\n",
                entry.history.size(), s0UbtbPred.history.size());
        entry.history = s0UbtbPred.history;
        // TODO: when hit, the remaining signals should be the prediction
        // result
    }
    std::string buf;
    boost::to_string(entry.history, buf);
    DPRINTF(DecoupleBP, "New prediction history: %s\n", buf.c_str());
    entry.setDefaultResolve();
    auto [insert_it, inserted] = fetchStreamQueue.emplace(fsqId, entry);
    assert(inserted);

    dumpFsq("after insert new stream");
    DPRINTF(DecoupleBP, "Insert fetch stream %lu\n", fsqId);

    // only if the stream is predicted to be ended can we inc fsqID
    if (entry.streamEnded) {
        fsqId++;
        DPRINTF(DecoupleBP, "Inc fetch stream to %lu, because stream ends\n",
                fsqId);
    }
    printStream(entry);
}

}  // namespace branch_prediction

}  // namespace gem5
