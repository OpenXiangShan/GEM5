/*
 * Copyright (c) 2012, 2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include <queue>

#include "cpu/o3/decode.hh"

#include "arch/generic/pcstate.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/Decode.hh"
#include "debug/DecoupleBP.hh"
#include "debug/O3PipeView.hh"
#include "debug/Counters.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

namespace gem5
{

namespace o3
{

Decode::Decode(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      renameToDecodeDelay(params.renameToDecodeDelay),
      iewToDecodeDelay(params.iewToDecodeDelay),
      commitToDecodeDelay(params.commitToDecodeDelay),
      fetchToDecodeDelay(params.fetchToDecodeDelay),
      decodeWidth(params.decodeWidth),
      numThreads(params.numThreads),
      stats(_cpu)
{
    if (decodeWidth > MaxWidth)
        fatal("decodeWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             decodeWidth, static_cast<int>(MaxWidth));

    // @todo: Make into a parameter
    skidBufferMax = (fetchToDecodeDelay + 1) *  params.fetchWidth;
    for (int tid = 0; tid < MaxThreads; tid++) {
        stalls[tid] = {false};
        decodeStatus[tid] = Idle;
        bdelayDoneSeqNum[tid] = 0;
        squashInst[tid] = nullptr;
        squashAfterDelaySlot[tid] = 0;
    }

    decodeStalls.resize(decodeWidth, StallReason::NoStall);
}

void
Decode::startupStage()
{
    resetStage();
}

void
Decode::clearStates(ThreadID tid)
{
    decodeStatus[tid] = Idle;
    stalls[tid].rename = false;
}

void
Decode::resetStage()
{
    _status = Inactive;

    // Setup status, make sure stall signals are clear.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        decodeStatus[tid] = Idle;

        stalls[tid].rename = false;
    }
}

std::string
Decode::name() const
{
    return cpu->name() + ".decode";
}

Decode::DecodeStats::DecodeStats(CPU *cpu)
    : statistics::Group(cpu, "decode"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is idle"),
      ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is blocked"),
      ADD_STAT(runCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is running"),
      ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is unblocking"),
      ADD_STAT(squashCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is squashing"),
      ADD_STAT(branchResolved, statistics::units::Count::get(),
               "Number of times decode resolved a branch"),
      ADD_STAT(branchMispred, statistics::units::Count::get(),
               "Number of times decode detected a branch misprediction"),
      ADD_STAT(controlMispred, statistics::units::Count::get(),
               "Number of times decode detected an instruction incorrectly "
               "predicted as a control"),
      ADD_STAT(decodedInsts, statistics::units::Count::get(),
               "Number of instructions handled by decode"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions handled by decode"),
      ADD_STAT(mispredictedByPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to pc"),
      ADD_STAT(mispredictedByNPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to npc")
{
    idleCycles.prereq(idleCycles);
    blockedCycles.prereq(blockedCycles);
    runCycles.prereq(runCycles);
    unblockCycles.prereq(unblockCycles);
    squashCycles.prereq(squashCycles);
    branchResolved.prereq(branchResolved);
    branchMispred.prereq(branchMispred);
    controlMispred.prereq(controlMispred);
    decodedInsts.prereq(decodedInsts);
    squashedInsts.prereq(squashedInsts);
    mispredictedByPC.flags(statistics::total);
    mispredictedByNPC.flags(statistics::total);
}

void
Decode::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to write information back to fetch.
    toFetch = timeBuffer->getWire(0);

    // Create wires to get information from proper places in time buffer.
    fromRename = timeBuffer->getWire(-renameToDecodeDelay);
    fromIEW = timeBuffer->getWire(-iewToDecodeDelay);
    fromCommit = timeBuffer->getWire(-commitToDecodeDelay);
}

void
Decode::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // Setup wire to write information to proper place in decode queue.
    toRename = decodeQueue->getWire(0);
}

void
Decode::setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)
{
    fetchQueue = fq_ptr;

    // Setup wire to read information from fetch queue.
    fromFetch = fetchQueue->getWire(-fetchToDecodeDelay);
}

void
Decode::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Decode::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        assert(insts[tid].empty());
        assert(skidBuffer[tid].empty());
    }
}

bool
Decode::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!insts[tid].empty() || !skidBuffer[tid].empty() ||
                (decodeStatus[tid] != Running && decodeStatus[tid] != Idle))
            return false;
    }
    return true;
}

bool
Decode::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].rename) {
        DPRINTF(Decode,"[tid:%i] Stall fom Rename stage detected.\n", tid);
        ret_val = true;
    }

    return ret_val;
}

bool
Decode::fetchInstsValid()
{
    return fromFetch->size > 0;
}

bool
Decode::block(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] Blocking.\n", tid);

    // Add the current inputs to the skid buffer so they can be
    // reprocessed when this stage unblocks.
    skidInsert(tid);

    // If the decode status is blocked or unblocking then decode has not yet
    // signalled fetch to unblock. In that case, there is no need to tell
    // fetch to block.
    if (decodeStatus[tid] != Blocked) {
        // Set the status to Blocked.
        decodeStatus[tid] = Blocked;

        if (toFetch->decodeUnblock[tid]) {
            toFetch->decodeUnblock[tid] = false;
        } else {
            toFetch->decodeBlock[tid] = true;  // 如果decode block，则设置fetch block
            wroteToTimeBuffer = true;
        }

        return true;
    }

    return false;
}

bool
Decode::unblock(ThreadID tid)
{
    // Decode is done unblocking only if the skid buffer is empty.
    if (skidBuffer[tid].empty()) {
        DPRINTF(Decode, "[tid:%i] Done unblocking.\n", tid);
        toFetch->decodeUnblock[tid] = true;
        wroteToTimeBuffer = true;

        decodeStatus[tid] = Running;
        return true;
    }

    DPRINTF(Decode, "[tid:%i] Currently unblocking.\n", tid);

    return false;
}

void
Decode::squash(const DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] [sn:%llu] Squashing due to incorrect branch "
            "prediction detected at decode.\n", tid, inst->seqNum);

    // Send back mispredict information.
    toFetch->decodeInfo[tid].branchMispredict = true;
    toFetch->decodeInfo[tid].predIncorrect = true;
    toFetch->decodeInfo[tid].mispredictInst = inst;
    toFetch->decodeInfo[tid].squash = true;
    toFetch->decodeInfo[tid].doneSeqNum = inst->seqNum;
    if (inst->isControl()) {
        if (!inst->isReturn()) {
            set(toFetch->decodeInfo[tid].nextPC, *inst->branchTarget());
        } else {
            // if it is return, the target must have already been set in pred target now
            std::unique_ptr<PCStateBase> tgt_ptr(inst->readPredTarg().clone());
            set(toFetch->decodeInfo[tid].nextPC, *tgt_ptr);
        }
    } else {
        std::unique_ptr<PCStateBase> npc_ptr(inst->pcState().clone());
        npc_ptr->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
        set(toFetch->decodeInfo[tid].nextPC, *npc_ptr);
    }

    // Looking at inst->pcState().branching()
    // may yield unexpected results if the branch
    // was predicted taken but aliased in the BTB
    // with a branch jumping to the next instruction (mistarget)
    // Using PCState::branching()  will send execution on the
    // fallthrough and this will not be caught at execution (since
    // branch was correctly predicted taken)
    toFetch->decodeInfo[tid].branchTaken = inst->readPredTaken() ||
                                           inst->isUncondCtrl();

    toFetch->decodeInfo[tid].squashInst = inst;

    InstSeqNum squash_seq_num = inst->seqNum;

    // Might have to tell fetch to unblock.
    if (decodeStatus[tid] == Blocked ||
        decodeStatus[tid] == Unblocking) {
        toFetch->decodeUnblock[tid] = 1;
    }

    // Set status to squashing.
    decodeStatus[tid] = Squashing;

    for (int i=0; i<fromFetch->size; i++) {
        if (fromFetch->insts[i]->threadNumber == tid &&
            fromFetch->insts[i]->seqNum > squash_seq_num) {
            fromFetch->insts[i]->setSquashed();
        }
    }

    // Clear the instruction list and skid buffer in case they have any
    // insts in them.
    while (!insts[tid].empty()) {
        insts[tid].pop_front();
    }

    while (!skidBuffer[tid].empty()) {
        skidBuffer[tid].pop_front();
    }

    // Squash instructions up until this one
    cpu->removeInstsUntil(squash_seq_num, tid);
}

unsigned
Decode::squash(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] Squashing.\n",tid);

    if (decodeStatus[tid] == Blocked ||
        decodeStatus[tid] == Unblocking) {
        if (FullSystem) {
            toFetch->decodeUnblock[tid] = 1;
        } else {
            // In syscall emulation, we can have both a block and a squash due
            // to a syscall in the same cycle.  This would cause both signals
            // to be high.  This shouldn't happen in full system.
            // @todo: Determine if this still happens.
            if (toFetch->decodeBlock[tid])
                toFetch->decodeBlock[tid] = 0;
            else
                toFetch->decodeUnblock[tid] = 1;
        }
    }

    // Set status to squashing.
    decodeStatus[tid] = Squashing;

    // Go through incoming instructions from fetch and squash them.
    unsigned squash_count = 0;

    for (int i=0; i<fromFetch->size; i++) {
        if (fromFetch->insts[i]->threadNumber == tid) {
            fromFetch->insts[i]->setSquashed();
            squash_count++;
        }
    }

    // Clear the instruction list and skid buffer in case they have any
    // insts in them.
    while (!insts[tid].empty()) {
        insts[tid].pop_front();
    }

    while (!skidBuffer[tid].empty()) {
        skidBuffer[tid].pop_front();
    }

    return squash_count;
}

void
Decode::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop_front();

        assert(tid == inst->threadNumber);

        skidBuffer[tid].push_back(inst);

        DPRINTF(Decode, "Inserting [tid:%d][sn:%lli] PC: %s into decode "
                "skidBuffer %i\n", inst->threadNumber, inst->seqNum,
                inst->pcState(), skidBuffer[tid].size());
    }

    // @todo: Eventually need to enforce this by not letting a thread
    // fetch past its skidbuffer
    assert(skidBuffer[tid].size() <= skidBufferMax);
}

bool
Decode::skidsEmpty()
{
    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        if (!skidBuffer[tid].empty())
            return false;
    }

    return true;
}

void
Decode::updateStatus()
{
    bool any_unblocking = false;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (decodeStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

    // Decode will have activity if it's unblocking.
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::DecodeIdx);
        }
    } else {
        // If it's not unblocking, then decode will not have any internal
        // activity.  Switch it to inactive.
        if (_status == Active) {
            _status = Inactive;
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::DecodeIdx);
        }
    }
}

void
Decode::sortInsts()
{
    int insts_from_fetch = fromFetch->size;
    for (int i = 0; i < insts_from_fetch; ++i) {
        const DynInstPtr &inst = fromFetch->insts[i];
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();
        }
        insts[inst->threadNumber].push_back(inst);
    }
}

void
Decode::readStallSignals(ThreadID tid)
{
    if (fromRename->renameBlock[tid]) {
        stalls[tid].rename = true;
    }

    if (fromRename->renameUnblock[tid]) {
        assert(stalls[tid].rename);
        stalls[tid].rename = false;
    }
}

bool
Decode::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is.
    // Check stall signals, block if necessary.
    // If status was blocked
    //     Check if stall conditions have passed
    //         if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    // Update the per thread stall statuses.
    readStallSignals(tid);

    // Check squash signals from commit.
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Decode, "[tid:%i] Squashing instructions due to squash "
                "from commit.\n", tid);

        squash(tid);

        localSquashVer.update(fromCommit->commitInfo[tid].squashVersion.getVersion());
        DPRINTF(Decode, "Updating squash version to %u\n",
                localSquashVer.getVersion());

        return true;
    }

    if (checkStall(tid)) {  // rename stall导致decode stall,传递给fetch
        blockReason = fromRename->renameInfo[tid].blockReason;
        return block(tid);
    }

    if (decodeStatus[tid] == Blocked) {
        DPRINTF(Decode, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        decodeStatus[tid] = Unblocking;

        unblock(tid);

        return true;
    }

    if (decodeStatus[tid] == Squashing) {
        // Switch status to running if decode isn't being told to block or
        // squash this cycle.
        DPRINTF(Decode, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        decodeStatus[tid] = Running;

        return false;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause decode to change its status.  Decode remains the same as before.
    return false;
}

void
Decode::tick()
{
    toRename->fetchStallReason = fromFetch->fetchStallReason;

    wroteToTimeBuffer = false;

    bool status_change = false;

    toRenameIndex = 0;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    sortInsts();

    //Check stall and squash signals.
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(Decode,"Processing [tid:%i]\n",tid);
        status_change =  checkSignalsAndUpdate(tid) || status_change;

        decode(status_change, tid);

        toFetch->decodeInfo[tid].blockReason = blockReason;
    }

    if (status_change) {
        updateStatus();
    }

    toRename->decodeStallReason = decodeStalls;     // 将decode stall原因传递给rename

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");

        cpu->activityThisCycle();
    }
}

void
Decode::decode(bool &status_change, ThreadID tid)
{
    // If status is Running or idle,
    //     call decodeInsts()
    // If status is Unblocking,
    //     buffer any instructions coming from fetch
    //     continue trying to empty skid buffer
    //     check if stall conditions have passed

    if (decodeStatus[tid] == Blocked) {
        ++stats.blockedCycles;
        setAllStalls(blockReason);
    } else if (decodeStatus[tid] == Squashing) {
        ++stats.squashCycles;
        setAllStalls(StallReason::SquashStall);
    }

    // Decode should try to decode as many instructions as its bandwidth
    // will allow, as long as it is not currently blocked.
    if (decodeStatus[tid] == Running ||
        decodeStatus[tid] == Idle) {
        DPRINTF(Decode, "[tid:%i] Not blocked, so attempting to run "
                "stage.\n",tid);

        decodeInsts(tid);
    } else if (decodeStatus[tid] == Unblocking) {
        // Make sure that the skid buffer has something in it if the
        // status is unblocking.
        assert(!skidsEmpty());

        // If the status was unblocking, then instructions from the skid
        // buffer were used.  Remove those instructions and handle
        // the rest of unblocking.
        decodeInsts(tid);

        if (fetchInstsValid()) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);
        }

        status_change = unblock(tid) || status_change;
    }
}

void
Decode::decodeInsts(ThreadID tid)
{
    // Instructions can come either from the skid buffer or the list of
    // instructions coming from fetch, depending on decode's status.
    int insts_available = decodeStatus[tid] == Unblocking ?
        skidBuffer[tid].size() : insts[tid].size();

    std::queue<StallReason> decode_stalls;  // 存放当前inst的stall原因，队列

    StallReason breakDecode = StallReason::NoStall;  // 当前拍decode break stall原因

    if (insts_available == 0) {     // 当前拍fetch 没有传递指令过来，所以全盘接受fetchStallReason
        DPRINTF(Decode, "[tid:%i] Nothing to do, breaking out"
                " early.\n",tid);
        // Should I change the status to idle?
        ++stats.idleCycles;

        StallReason stall = StallReason::NoStall;
        for (auto iter : fromFetch->fetchStallReason) {
            if (iter != StallReason::NoStall) {
                stall = iter;       // 找到第一个stall原因
                break;
            }
        }
        setAllStalls(stall);    // 把fetchStallReason赋值给decodeStallReason！
        return;
    } else if (decodeStatus[tid] == Unblocking) {
        DPRINTF(Decode, "[tid:%i] Unblocking, removing insts from skid "
                "buffer.\n",tid);
        ++stats.unblockCycles;
    } else if (decodeStatus[tid] == Running) {
        ++stats.runCycles;
    }

    std::deque<DynInstPtr>
        &insts_to_decode = decodeStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];   // 从skidBuffer或insts中取指令，这一拍要尽可能译码完

    DPRINTF(Decode, "[tid:%i] Sending instruction to rename.\n",tid);

    int decode_width = decodeWidth;
    int count_ = 0;
    for (auto it : insts_to_decode) {
        count_++;
        if (it->opClass() == FMAAccOp) {    // 对融合指令额外处理，width++
            decode_width++;
        }
        if (count_ >= decodeWidth ||
            decode_width >= decodeWidth * 2) {
            break;
        }
    }

    bool vec_decode_limit = false;

    if (!insts_to_decode.front()->isVector()) {
        vec_decode_limit = true;
    }

    while (insts_available > 0 && toRenameIndex < decode_width) {   // 遍历每条要译码指令
        assert(!insts_to_decode.empty());
        if (vec_decode_limit && insts_to_decode.front()->isVector()) {
            break;
        }

        DynInstPtr inst = std::move(insts_to_decode.front());

        insts_to_decode.pop_front();

        DPRINTF(Decode, "[tid:%i] Processing instruction [sn:%lli] with "
                "PC %s\n", tid, inst->seqNum, inst->pcState());

        if (inst->isSquashed()) {
            DPRINTF(Decode, "[tid:%i] Instruction %i with PC %s is "
                    "squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;

            --insts_available;

            decode_stalls.push(StallReason::InstSquashed);

            continue;
        }

        // Also check if instructions have no source registers.  Mark
        // them as ready to issue at any time.  Not sure if this check
        // should exist here or at a later stage; however it doesn't matter
        // too much for function correctness.
        if (inst->numSrcRegs() == 0) {
            inst->setCanIssue();
        }

        // This current instruction is valid, so add it into the decode
        // queue.  The next instruction may not be valid, so check to
        // see if branches were predicted correctly.
        toRename->insts[toRenameIndex] = inst;  // 当条指令有效，下一条指令不一定，检查分支预测

        ++(toRename->size);
        ++toRenameIndex;
        ++stats.decodedInsts;
        --insts_available;

#if TRACING_ON
        if (debug::O3PipeView) {
            inst->decodeTick = curTick() - inst->fetchTick;
            // DPRINTF(O3PipeView, "Record decode for inst sn:%lu\n",
            //         inst->seqNum);
        }
#endif

        // Ensure that if it was predicted as a branch, it really is a
        // branch. 是否被预测为branch
        if (inst->readPredTaken() && !inst->isControl()) {    // 预测taken，且不是control指令
            // panic("Instruction predicted as a branch!");

            ++stats.controlMispred;

            // Might want to set some sort of boolean and just do
            // a check at the end
            squash(inst, inst->threadNumber);

            decode_stalls.push(StallReason::InstMisPred);  // 预测错误，指令都不对
            breakDecode = StallReason::InstMisPred;

            break;
        }

        // Go ahead and compute any PC-relative branches.
        // This includes direct unconditional control and
        // direct conditional control that is predicted taken.
        // 处理直接跳转指令的目标地址计算和预测检查
        // 包括无条件直接跳转和预测taken的条件直接跳转
        if (inst->isDirectCtrl() &&
           (inst->isUncondCtrl() || inst->readPredTaken()))
        {
            ++stats.branchResolved;  // 增加已解析分支计数

            // 计算实际的分支目标地址
            std::unique_ptr<PCStateBase> target = inst->branchTarget();
            auto &t = target->as<RiscvISA::PCState>();  // 实际目标
            auto &pred = inst->readPredTarg().as<RiscvISA::PCState>();  // 预测目标

            // 如果预测的PC正确但nPC不正确,更新预测目标
            if (t.start_equals(pred) && !t.equals(pred)) {
                DPRINTF(
                    DecoupleBP,
                    "Override useless npc, from %#lx->%#lx to %#lx->%#lx\n",
                    pred.pc(), pred.npc(), t.pc(), t.npc());
                inst->setPredTarg(t);
            }

            // 检查预测目标是否正确
            if (*target != inst->readPredTarg()) {   // 预测目标和实际目标不一致
                ++stats.branchMispred;  // 增加分支预测错误计数

                // 克隆目标PC和预测PC用于详细分析
                RiscvISA::PCState cpTarget = target->clone()->as<RiscvISA::PCState>();
                RiscvISA::PCState cpPredTarget = inst->readPredTarg().clone()->as<RiscvISA::PCState>();

                // 分析预测错误的类型:
                // 1. PC预测错误但nPC正确
                if (cpTarget.instAddr() != cpPredTarget.instAddr() && cpTarget.npc() == cpPredTarget.npc()) {
                    ++stats.mispredictedByPC;
                } 
                // 2. PC预测正确但nPC错误
                else if (cpTarget.instAddr() == cpPredTarget.instAddr() && cpTarget.npc() != cpPredTarget.npc()) {
                    ++stats.mispredictedByNPC;
                }

                // 预测错误,需要squash流水线
                squash(inst, inst->threadNumber);

                decode_stalls.push(StallReason::InstMisPred);   // 记录stall原因
                breakDecode = StallReason::InstMisPred;

                DPRINTF(Decode,
                        "[tid:%i] [sn:%llu] Updating predictions:"
                        " Wrong predicted target: %s PredPC: %s\n",
                        tid, inst->seqNum, inst->readPredTarg(), *target);
                //指令级分支后的微PC应该为0
                inst->setPredTarg(*target);  // 更新为正确的目标
                break;
            }
        }
        // 处理未预测到的返回指令 - 可以利用RAS(Return Address Stack)结果来提前重定向
        if (inst->isReturn() && !inst->isNonSpeculative() && !inst->readPredTaken()) {
            // 增加分支预测错误计数
            ++stats.branchMispred;
            
            // 由于return预测错误,需要flush流水线并stall
            decode_stalls.push(StallReason::InstMisPred);   
            breakDecode = StallReason::InstMisPred;
            
            // 由于return是间接跳转,无法在decode阶段计算目标地址
            // 需要查询分支预测单元(BPU)获取返回地址
            auto return_addr = fetch_ptr->getPreservedReturnAddr(inst);
            auto target = std::make_unique<RiscvISA::PCState>(return_addr);
            
            // 打印调试信息 - 显示预测更新详情
            DPRINTF(Decode, "[tid:%i] [sn:%llu] Updating predictions:"
                    " Return not identified by bp: predTaken %d, PredPC: %s Now PC %s\n",
                    tid, inst->seqNum, inst->readPredTaken(), inst->readPredTarg(), *target);
            
            // 更新预测信息
            inst->setPredTaken(true);  // 设置为taken
            inst->setPredTarg(*target);  // 设置正确的目标地址
            
            // 必须在设置指令实际目标后再squash,因为无法从静态指令计算目标
            squash(inst, inst->threadNumber);
            break;
        }

        // 处理非推测性指令
        if (inst->isNonSpeculative() && inst->readPredTaken()) {
            // 对于非推测性指令,需要重定向到顺序执行的下一条指令
            std::unique_ptr<PCStateBase> npc(inst->pcState().clone());
            npc->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
            
            // 更新预测信息为not taken
            inst->setPredTaken(false);
            inst->setPredTarg(*npc);
        }
    }

    for (int i = 0;i < decodeWidth;i++) {
        if (i < toRenameIndex) {    // 如果当前inst已经rename，则不stall
            decodeStalls.at(i) = StallReason::NoStall;
        } else {    // toRenameIndex < i < decodeWidth
            if (!decode_stalls.empty()) {   // 如果当前inst未rename，则根据decode_stalls队列赋值
                decodeStalls.at(i) = decode_stalls.front();
                decode_stalls.pop();
            } else if (breakDecode != StallReason::NoStall) {    // 如果当前inst未rename，且decode_stalls队列空，则赋值breakDecode
                decodeStalls.at(i) = breakDecode;
            } else {
                decodeStalls.at(i) = StallReason::NoStall;
            }
        }
    }

    // If we didn't process all instructions, then we will need to block
    // and put all those instructions into the skid buffer.
    if (!insts_to_decode.empty()) { // 如果没有处理完所有inst，则decode stall，把指令放入skidBuffer
        blockReason = breakDecode;
        block(tid);
    }

    // Record that decode has written to the time buffer for activity
    // tracking.
    if (toRenameIndex) {
        wroteToTimeBuffer = true;
    }
}

void
Decode::setAllStalls(StallReason decodeStall)
{
    for (int i = 0;i < decodeStalls.size();i++) {
        decodeStalls.at(i) = decodeStall;
    }
}

} // namespace o3
} // namespace gem5
