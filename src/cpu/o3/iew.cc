/*
 * Copyright (c) 2010-2013, 2018-2019 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
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

// @todo: Fix the instantaneous communication among all the stages within
// iew.  There's a clear delay between issue and execute, yet backwards
// communication happens simultaneously.

#include "cpu/o3/iew.hh"

#include <queue>

#include "base/output.hh"
#include "base/stats/info.hh"
#include "config/the_isa.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/timebuf.hh"
#include "debug/Activity.hh"
#include "debug/Counters.hh"
#include "debug/DecoupleBP.hh"
#include "debug/Drain.hh"
#include "debug/IEW.hh"
#include "debug/O3PipeView.hh"
#include "debug/Rename.hh"
#include "params/BaseO3CPU.hh"
#include "sim/core.hh"

namespace gem5
{

namespace o3
{

IEW::IEW(CPU *_cpu, const BaseO3CPUParams &params)
    : dqSize(params.numDQEntries),
      issueToExecQueue(params.backComSize, params.forwardComSize),
      cpu(_cpu),
      scheduler(params.scheduler),
      instQueue(_cpu, this, params),
      ldstQueue(_cpu, this, params),
      dispWidth(params.dispWidth),
      commitToIEWDelay(params.commitToIEWDelay),
      renameToIEWDelay(params.renameToIEWDelay),
      renameWidth(params.renameWidth),
      wbNumInst(0),
      wbCycle(0),
      wbDelay(params.executeToWriteBackDelay),
      wbWidth(params.wbWidth),
      numThreads(params.numThreads),
      iewStats(cpu)
{
    if (wbWidth > MaxWidth)
        fatal("wbWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             wbWidth, static_cast<int>(MaxWidth));

    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

    // Setup wire to read instructions coming from issue.
    fromIssue = issueToExecQueue.getWire(0);

    // Instruction queue needs the queue between issue and execute.
    instQueue.setIssueToExecuteQueue(&issueToExecQueue);

    for (ThreadID tid = 0; tid < MaxThreads; tid++) {
        dispatchStatus[tid] = Running;
        fetchRedirect[tid] = false;
    }

    updateLSQNextCycle = false;

    skidBufferMax = (renameToIEWDelay + 1) * params.renameWidth * 2;

    dispatchStalls.resize(renameWidth, StallReason::NoStall);

}

std::string
IEW::name() const
{
    return cpu->name() + ".iew";
}

void
IEW::regProbePoints()
{
    ppDispatch = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Dispatch");
    ppMispredict = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Mispredict");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction starts to execute.
     */
    ppExecute = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Execute");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction execution completes and it is marked ready to commit.
     */
    ppToCommit = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "ToCommit");
}

IEW::IEWStats::IEWStats(CPU *cpu)
    : statistics::Group(cpu, "iew"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is idle"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is squashing"),
    ADD_STAT(blockCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is blocking"),
    ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is unblocking"),
    ADD_STAT(dispatchedInsts, statistics::units::Count::get(),
             "Number of instructions dispatched to IQ"),
    ADD_STAT(dispSquashedInsts, statistics::units::Count::get(),
             "Number of squashed instructions skipped by dispatch"),
    ADD_STAT(dispLoadInsts, statistics::units::Count::get(),
             "Number of dispatched load instructions"),
    ADD_STAT(dispStoreInsts, statistics::units::Count::get(),
             "Number of dispatched store instructions"),
    ADD_STAT(dispNonSpecInsts, statistics::units::Count::get(),
             "Number of dispatched non-speculative instructions"),
    ADD_STAT(iqFullEvents, statistics::units::Count::get(),
             "Number of times the IQ has become full, causing a stall"),
    ADD_STAT(lsqFullEvents, statistics::units::Count::get(),
             "Number of times the LSQ has become full, causing a stall"),
    ADD_STAT(memOrderViolationEvents, statistics::units::Count::get(),
             "Number of memory order violations"),
    ADD_STAT(predictedTakenIncorrect, statistics::units::Count::get(),
             "Number of branches that were predicted taken incorrectly"),
    ADD_STAT(predictedNotTakenIncorrect, statistics::units::Count::get(),
             "Number of branches that were predicted not taken incorrectly"),
    ADD_STAT(branchMispredicts, statistics::units::Count::get(),
             "Number of branch mispredicts detected at execute",
             predictedTakenIncorrect + predictedNotTakenIncorrect),
    ADD_STAT(dispDist, statistics::units::Count::get(),
             "Number of branch mispredicts detected at execute"),
    executedInstStats(cpu),
    ADD_STAT(instsToCommit, statistics::units::Count::get(),
             "Cumulative count of insts sent to commit"),
    ADD_STAT(writebackCount, statistics::units::Count::get(),
             "Cumulative count of insts written-back"),
    ADD_STAT(producerInst, statistics::units::Count::get(),
             "Number of instructions producing a value"),
    ADD_STAT(consumerInst, statistics::units::Count::get(),
             "Number of instructions consuming a value"),
    ADD_STAT(wbRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Insts written-back per cycle"),
    ADD_STAT(wbFanout, statistics::units::Rate<
                statistics::units::Count, statistics::units::Count>::get(),
             "Average fanout of values written-back"),
    ADD_STAT(stallEvents, statistics::units::Count::get(),
             "Number of events the IEW has stalled"),
    ADD_STAT(fetchStallReason, statistics::units::Count::get(),
             "Number of fetch stall reasons each tick (Total)"),
    ADD_STAT(decodeStallReason, statistics::units::Count::get(),
             "Number of decode stall reasons each tick (Total)"),
    ADD_STAT(renameStallReason, statistics::units::Count::get(),
             "Number of rename stall reasons each tick (Total)"),
    ADD_STAT(dispatchStallReason, statistics::units::Count::get(),
             "Number of dispatch stall reasons each tick (Total)")
{
    instsToCommit
        .init(cpu->numThreads)
        .flags(statistics::total);

    writebackCount
        .init(cpu->numThreads)
        .flags(statistics::total);

    producerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    consumerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    wbRate
        .flags(statistics::total);
    wbRate = writebackCount / cpu->baseStats.numCycles;

    wbFanout
        .flags(statistics::total);
    wbFanout = producerInst / consumerInst;

    stallEvents
        .init(StallEventCount)
        .flags(statistics::total);

    dispDist.init(0,10,1).flags(statistics::nozero);

    std::map < StallEvent, const char* > stall_event_str = {
        { CacheMiss, "CacheMiss" },
        { Translation, "Translation" },
        { ROBWalk, "ROBWalk" },
        { IQFull, "IQFull" },
        { LSQFull, "LSQFull" },
        { DispBWFull, "DispBWFull" }
    };

    for (int i = 0; i < StallEventCount; i++) {
        stallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
    }

    fetchStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    decodeStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    renameStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    dispatchStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    std::map <StallReason, const char*> stallReasonStr = {
        {StallReason::NoStall, "NoStall"},
        {StallReason::IcacheStall, "IcacheStall"},
        {StallReason::ITlbStall, "ITlbStall"},
        {StallReason::DTlbStall, "DTlbStall"},
        {StallReason::BpStall, "BpStall"},
        {StallReason::IntStall, "IntStall"},
        {StallReason::TrapStall, "TrapStall"},
        {StallReason::FetchFragStall, "FetchFragStall"},
        {StallReason::OtherFragStall, "OtherFragStall"},
        {StallReason::SquashStall, "SquashStall"},
        {StallReason::FetchBufferInvalid, "FetchBufferInvalid"},
        {StallReason::InstMisPred, "InstMisPred"},
        {StallReason::InstSquashed, "InstSquashed"},
        {StallReason::SerializeStall, "SerializeStall"},
        {StallReason::VectorLongExecute, "VectorLongExecute"},
        {StallReason::ScalarLongExecute, "ScalarLongExecute"},
        {StallReason::InstNotReady, "InstNotReady"},
        {StallReason::LoadL1Bound, "LoadL1Bound"},
        {StallReason::LoadL2Bound, "LoadL2Bound"},
        {StallReason::LoadL3Bound, "LoadL3Bound"},
        {StallReason::LoadMemBound, "LoadMemBound"},
        {StallReason::StoreL1Bound, "StoreL1Bound"},
        {StallReason::StoreL2Bound, "StoreL2Bound"},
        {StallReason::StoreL3Bound, "StoreL3Bound"},
        {StallReason::StoreMemBound, "StoreMemBound"},
        {StallReason::MemSquashed, "MemSquashed"},
        {StallReason::Atomic,"Atomic"},
        {StallReason::ResumeUnblock, "ResumeUnblock"},
        {StallReason::CommitSquash, "CommitSquash"},
        {StallReason::OtherStall, "OtherStall"},
        {StallReason::OtherFetchStall, "OtherFetchStall"},
        {StallReason::FTQBubble, "FTQBubble"},
        {StallReason::MemDQBandwidth, "MemDQBandwidth"},
        {StallReason::FVDQBandwidth, "FVDQBandwidth"},
        {StallReason::IntDQBandwidth, "IntDQBandwidth"},
        {StallReason::MemNotReady, "MemNotReady"},
        {StallReason::MemCommitRateLimit, "MemCommitRateLimit"},
        {StallReason::OtherMemStall, "OtherMemStall"},
        {StallReason::VectorReadyButNotIssued, "VectorReadyButNotIssued"},
        {StallReason::ScalarReadyButNotIssued, "ScalarReadyButNotIssued"}
    };

    for (int i = 0;i < NumStallReasons;i++) {
        fetchStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        decodeStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        renameStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        dispatchStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
    }
}

IEW::IEWStats::ExecutedInstStats::ExecutedInstStats(CPU *cpu)
    : statistics::Group(cpu),
    ADD_STAT(numInsts, statistics::units::Count::get(),
             "Number of executed instructions"),
    ADD_STAT(numLoadInsts, statistics::units::Count::get(),
             "Number of load instructions executed"),
    ADD_STAT(numSquashedInsts, statistics::units::Count::get(),
             "Number of squashed instructions skipped in execute"),
    ADD_STAT(numSwp, statistics::units::Count::get(),
             "Number of swp insts executed"),
    ADD_STAT(numNop, statistics::units::Count::get(),
             "Number of nop insts executed"),
    ADD_STAT(numRefs, statistics::units::Count::get(),
             "Number of memory reference insts executed"),
    ADD_STAT(numBranches, statistics::units::Count::get(),
             "Number of branches executed"),
    ADD_STAT(numStoreInsts, statistics::units::Count::get(),
             "Number of stores executed"),
    ADD_STAT(numRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Inst execution rate", numInsts / cpu->baseStats.numCycles)
{
    numLoadInsts
        .init(cpu->numThreads)
        .flags(statistics::total);

    numSwp
        .init(cpu->numThreads)
        .flags(statistics::total);

    numNop
        .init(cpu->numThreads)
        .flags(statistics::total);

    numRefs
        .init(cpu->numThreads)
        .flags(statistics::total);

    numBranches
        .init(cpu->numThreads)
        .flags(statistics::total);

    numStoreInsts
        .flags(statistics::total);
    numStoreInsts = numRefs - numLoadInsts;

    numRate
        .flags(statistics::total);
}

void
IEW::startupStage()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        toRename->iewInfo[tid].usedIQ = true;

        toRename->iewInfo[tid].usedLSQ = true;
        toRename->iewInfo[tid].freeLQEntries =
            ldstQueue.numFreeLoadEntries(tid);
        toRename->iewInfo[tid].freeSQEntries =
            ldstQueue.numFreeStoreEntries(tid);
    }

    // Initialize the checker's dcache port here
    if (cpu->checker) {
        cpu->checker->setDcachePort(&ldstQueue.getDataPort());
    }

    cpu->activateStage(CPU::IEWIdx);
}

void
IEW::clearStates(ThreadID tid)
{
    toRename->iewInfo[tid].usedIQ = true;

    toRename->iewInfo[tid].usedLSQ = true;
    toRename->iewInfo[tid].freeLQEntries = ldstQueue.numFreeLoadEntries(tid);
    toRename->iewInfo[tid].freeSQEntries = ldstQueue.numFreeStoreEntries(tid);
}

void
IEW::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to read information from time buffer, from commit.
    fromCommit = timeBuffer->getWire(-commitToIEWDelay);

    // Setup wire to write information back to previous stages.
    toRename = timeBuffer->getWire(0);

    toFetch = timeBuffer->getWire(0);

    // Instruction queue also needs main time buffer.
    instQueue.setTimeBuffer(tb_ptr);
}

void
IEW::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to read information from rename queue.
    fromRename = renameQueue->getWire(-renameToIEWDelay);
}

void
IEW::setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)
{
    iewQueue = iq_ptr;

    // Setup wire to write instructions to commit.
    toCommit = iewQueue->getWire(0);

    execBypass = iewQueue->getWire(0);
    execWB = iewQueue->getWire(-wbDelay);
}

void
IEW::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;

    ldstQueue.setActiveThreads(at_ptr);
    instQueue.setActiveThreads(at_ptr);
}

void
IEW::setScoreboard(Scoreboard *sb_ptr)
{
    scoreboard = sb_ptr;
}

bool
IEW::isDrained() const
{
    bool drained = ldstQueue.isDrained() && instQueue.isDrained();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!insts[tid].empty()) {
            DPRINTF(Drain, "%i: Insts not empty.\n", tid);
            drained = false;
        }
        if (!skidBuffer[tid].empty()) {
            DPRINTF(Drain, "%i: Skid buffer not empty.\n", tid);
            drained = false;
        }
        drained = drained && dispatchStatus[tid] == Running;
    }

    return drained;
}

void
IEW::drainSanityCheck() const
{
    assert(isDrained());

    instQueue.drainSanityCheck();
    ldstQueue.drainSanityCheck();
}

void
IEW::takeOverFrom()
{
    // Reset all state.
    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

    instQueue.takeOverFrom();
    ldstQueue.takeOverFrom();

    startupStage();
    cpu->activityThisCycle();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        dispatchStatus[tid] = Running;
        fetchRedirect[tid] = false;
    }

    updateLSQNextCycle = false;

    for (int i = 0; i < issueToExecQueue.getSize(); ++i) {
        issueToExecQueue.advance();
    }
}

void
IEW::squash(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Squashing all instructions.\n", tid);

    for (auto& dp : dispQue) {
        for (auto& it : dp) {
            if (it->seqNum > fromCommit->commitInfo[tid].doneSeqNum) {
                it->setSquashed();
            }
        }
    }

    // Tell the IQ to start squashing.
    instQueue.squash(tid);

    // Tell the LDSTQ to start squashing.
    ldstQueue.squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
    updatedQueues = true;

    // Clear the skid buffer in case it has any data in it.
    DPRINTF(IEW,
            "Removing skidbuffer instructions until "
            "[sn:%llu] [tid:%i]\n",
            fromCommit->commitInfo[tid].doneSeqNum, tid);

    while (!skidBuffer[tid].empty()) {
        if (skidBuffer[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        if (skidBuffer[tid].front()->isStore() ||
            skidBuffer[tid].front()->isAtomic()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;

        skidBuffer[tid].pop_front();
    }

    emptyRenameInsts(tid);
}

void
IEW::squashDueToBranch(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] [sn:%llu] Squashing from a specific instruction,"
            " PC: %s "
            "\n", tid, inst->seqNum, inst->pcState() );

    if (!execWB->squash[tid] ||
            inst->seqNum < execWB->squashedSeqNum[tid]) {
        execWB->squash[tid] = true;
        execWB->squashedSeqNum[tid] = inst->seqNum;
        execWB->squashedStreamId[tid] = inst->getFsqId();
        execWB->squashedTargetId[tid] = inst->getFtqId();
        execWB->squashedLoopIter[tid] = inst->getLoopIteration();
        execWB->branchTaken[tid] = inst->pcState().branching();

        set(execWB->pc[tid], inst->pcState());
        inst->staticInst->advancePC(*execWB->pc[tid]);

        execWB->mispredictInst[tid] = inst;
        execWB->includeSquashInst[tid] = false;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "Branch misprediction (pc=%#lx) set stream id to %lu, target "
                "id to %lu, loop iter to %u\n",
                execWB->pc[tid]->instAddr(),
                execWB->squashedStreamId[tid],
                execWB->squashedTargetId[tid],
                execWB->squashedLoopIter[tid]);
    }

}

void
IEW::squashDueToMemOrder(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Memory violation, squashing violator and younger "
            "insts, PC: %s [sn:%llu].\n", tid, inst->pcState(), inst->seqNum);
    // Need to include inst->seqNum in the following comparison to cover the
    // corner case when a branch misprediction and a memory violation for the
    // same instruction (e.g. load PC) are detected in the same cycle.  In this
    // case the memory violator should take precedence over the branch
    // misprediction because it requires the violator itself to be included in
    // the squash.
    if (!execWB->squash[tid] ||
            inst->seqNum <= execWB->squashedSeqNum[tid]) {
        execWB->squash[tid] = true;

        execWB->squashedSeqNum[tid] = inst->seqNum;
        execWB->squashedStreamId[tid] = inst->getFsqId();
        execWB->squashedTargetId[tid] = inst->getFtqId();
        execWB->squashedLoopIter[tid] = inst->getLoopIteration();
        set(execWB->pc[tid], inst->pcState());
        execWB->mispredictInst[tid] = NULL;

        // Must include the memory violator in the squash.
        execWB->includeSquashInst[tid] = true;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "Memory violation (pc=%#lx) set stream id to %lu, target id "
                "to %lu, loop iter to %u\n",
                execWB->pc[tid]->instAddr(),
                execWB->squashedStreamId[tid],
                execWB->squashedTargetId[tid],
                execWB->squashedLoopIter[tid]);


    }
}

void
IEW::block(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Blocking.\n", tid);

    if (dispatchStatus[tid] != Blocked &&
        dispatchStatus[tid] != Unblocking) {
        toRename->iewBlock[tid] = true;
        wroteToTimeBuffer = true;
    }

    // Add the current inputs to the skid buffer so they can be
    // reprocessed when this stage unblocks.
    skidInsert(tid);

    dispatchStatus[tid] = Blocked;
}

void
IEW::unblock(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Reading instructions out of the skid "
            "buffer %u.\n",tid, tid);

    // If the skid bufffer is empty, signal back to previous stages to unblock.
    // Also switch status to running.
    if (skidBuffer[tid].empty()) {
        toRename->iewUnblock[tid] = true;
        wroteToTimeBuffer = true;
        DPRINTF(IEW, "[tid:%i] Done unblocking.\n",tid);
        dispatchStatus[tid] = Running;
    }
}

void
IEW::wakeDependents(const DynInstPtr& inst)
{
    instQueue.wakeDependents(inst);
}

void
IEW::rescheduleMemInst(const DynInstPtr& inst)
{
    instQueue.rescheduleMemInst(inst);
}

void
IEW::replayMemInst(const DynInstPtr& inst)
{
    instQueue.replayMemInst(inst);
}

void
IEW::blockMemInst(const DynInstPtr& inst)
{
    instQueue.blockMemInst(inst);
}

void
IEW::cacheUnblocked()
{
    instQueue.cacheUnblocked();
}

void
IEW::instToCommit(const DynInstPtr& inst)
{
    // This function should not be called after writebackInsts in a
    // single cycle.  That will cause problems with an instruction
    // being added to the queue to commit without being processed by
    // writebackInsts prior to being sent to commit.

    // First check the time slot that this instruction will write
    // to.  If there are free write ports at the time, then go ahead
    // and write the instruction to that time.  If there are not,
    // keep looking back to see where's the first time there's a
    // free slot.
    while ((*iewQueue)[wbCycle].insts[wbNumInst]) {
        ++wbNumInst;
        if (wbNumInst == wbWidth) {
            ++wbCycle;
            wbNumInst = 0;
        }
    }

#if TRACING_ON
    if (debug::O3PipeView) {
        inst->completeTick = curTick() - inst->fetchTick;
    }
#endif

    scheduler->bypassWriteback(inst);
    inst->completionTick = curTick();

    DPRINTF(IEW, "Current wb cycle: %i, width: %i, numInst: %i\nwbActual:%i\n",
            wbCycle, wbWidth, wbNumInst, wbCycle * wbWidth + wbNumInst);
    // Add finished instruction to queue to commit.
    (*iewQueue)[wbCycle].insts[wbNumInst] = inst;
    (*iewQueue)[wbCycle].size++;
}

void
IEW::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop_front();

        DPRINTF(IEW,"[tid:%i] Inserting [sn:%lli] PC:%s into "
                "dispatch skidBuffer %i\n",tid, inst->seqNum,
                inst->pcState(),tid);

        skidBuffer[tid].push_back(inst);
    }

    assert(skidBuffer[tid].size() <= skidBufferMax &&
           "Skidbuffer Exceeded Max Size");
}

int
IEW::skidCount()
{
    int max=0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned thread_count = skidBuffer[tid].size();
        if (max < thread_count)
            max = thread_count;
    }

    return max;
}

bool
IEW::skidsEmpty()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!skidBuffer[tid].empty())
            return false;
    }

    return true;
}

void
IEW::updateStatus()
{
    bool any_unblocking = false;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (dispatchStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

    // If there are no ready instructions waiting to be scheduled by the IQ,
    // and there's no stores waiting to write back, and dispatch is not
    // unblocking, then there is no internal activity for the IEW stage.
    instQueue.iqIOStats.intInstQueueReads++;
    if (_status == Active && !instQueue.hasReadyInsts() &&
        !ldstQueue.willWB() && !any_unblocking) {
        DPRINTF(IEW, "IEW switching to idle\n");

        deactivateStage();

        _status = Inactive;
    } else if (_status == Inactive && (instQueue.hasReadyInsts() ||
                                       ldstQueue.willWB() ||
                                       any_unblocking)) {
        // Otherwise there is internal activity.  Set to active.
        DPRINTF(IEW, "IEW switching to active\n");

        activateStage();

        _status = Active;
    }
}

bool
IEW::checkStall(ThreadID tid)
{
    bool ret_val(false);

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW,"[tid:%i] Stall from Commit stage detected.\n",tid);
        ret_val = true;
        blockReason = StallReason::CommitSquash;
    }

    return ret_val;
}

void
IEW::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is
    // Check stall signals, block if there is.
    // If status was Blocked
    //     if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    if (fromCommit->commitInfo[tid].squash) {
        squash(tid);
        localSquashVer.update(fromCommit->commitInfo[tid].squashVersion.getVersion());
        DPRINTF(IEW, "Updating squash version to %u\n",
                localSquashVer.getVersion());

        if (dispatchStatus[tid] == Blocked ||
            dispatchStatus[tid] == Unblocking) {
            toRename->iewUnblock[tid] = true;
            wroteToTimeBuffer = true;
        }

        dispatchStatus[tid] = Squashing;
        fetchRedirect[tid] = false;
        iewStats.stallEvents[ROBWalk]++;
        setAllStalls(StallReason::CommitSquash);
        return;
    }

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW, "[tid:%i] ROB is still squashing.\n", tid);

        dispatchStatus[tid] = Squashing;
        emptyRenameInsts(tid);
        wroteToTimeBuffer = true;
        iewStats.stallEvents[ROBWalk]++;
        setAllStalls(StallReason::CommitSquash);
    }

    if (checkStall(tid)) {
        block(tid);
        dispatchStatus[tid] = Blocked;
        return;
    }

    if (dispatchStatus[tid] == Blocked) {
        // Status from previous cycle was blocked, but there are no more stall
        // conditions.  Switch over to unblocking.
        DPRINTF(IEW, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        dispatchStatus[tid] = Unblocking;

        unblock(tid);

        return;
    }

    if (dispatchStatus[tid] == Squashing) {
        // Switch status to running if rename isn't being told to block or
        // squash this cycle.
        DPRINTF(IEW, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        dispatchStatus[tid] = Running;

        return;
    }
}

void
IEW::sortInsts()
{
    int insts_from_rename = fromRename->size;
#ifdef DEBUG
    for (ThreadID tid = 0; tid < numThreads; tid++)
        assert(insts[tid].empty());
#endif
    for (int i = 0; i < insts_from_rename; ++i) {
        const DynInstPtr &inst = fromRename->insts[i];
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();
        }
        insts[fromRename->insts[i]->threadNumber].push_back(inst);
    }
}

void
IEW::emptyRenameInsts(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Removing incoming rename instructions\n", tid);

    while (!insts[tid].empty()) {

        if (insts[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        if (insts[tid].front()->isStore() ||
            insts[tid].front()->isAtomic()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;

        insts[tid].pop_front();
    }
}

void
IEW::wakeCPU()
{
    cpu->wakeCPU();
}

void
IEW::activityThisCycle()
{
    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();
}

void
IEW::activateStage()
{
    DPRINTF(Activity, "Activating stage.\n");
    cpu->activateStage(CPU::IEWIdx);
}

void
IEW::deactivateStage()
{
    DPRINTF(Activity, "Deactivating stage.\n");
    cpu->deactivateStage(CPU::IEWIdx);
}

void
IEW::dispatch(ThreadID tid)
{
    // If status is Running or idle,
    //     call dispatchInsts()
    // If status is Unblocking,
    //     buffer any instructions coming from rename
    //     continue trying to empty skid buffer
    //     check if stall conditions have passed

    if (dispatchStatus[tid] == Blocked) {
        ++iewStats.blockCycles;
        setAllStalls(blockReason);

    } else if (dispatchStatus[tid] == Squashing) {
        ++iewStats.squashCycles;
        setAllStalls(StallReason::CommitSquash);
    }

    // Dispatch should try to dispatch as many instructions as its bandwidth
    // will allow, as long as it is not currently blocked.
    if (dispatchStatus[tid] == Running ||
        dispatchStatus[tid] == Idle) {
        DPRINTF(IEW, "[tid:%i] Not blocked, so attempting to run "
                "dispatch.\n", tid);

        dispatchInsts(tid);
    } else if (dispatchStatus[tid] == Unblocking) {
        // Make sure that the skid buffer has something in it if the
        // status is unblocking.
        assert(!skidsEmpty());

        // If the status was unblocking, then instructions from the skid
        // buffer were used.  Remove those instructions and handle
        // the rest of unblocking.
        dispatchInsts(tid);

        ++iewStats.unblockCycles;

        if (fromRename->size != 0) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);
        }

        unblock(tid);
    }
}

void
IEW::dispatchInsts(ThreadID tid)
{
    dispatchInstFromDispQue(tid);
    classifyInstToDispQue(tid);
}

void
IEW::classifyInstToDispQue(ThreadID tid)
{
    auto dispClassify = [](const DynInstPtr& inst) -> int{
        if (inst->isMemRef() || inst->isReadBarrier() || inst->isWriteBarrier() || inst->isNonSpeculative()) {
            return MemDQ;
        }
        if (inst->isFloating() || inst->isVector()) {
            return FVDQ;
        }
        return IntDQ;
    };

    std::deque<DynInstPtr> &insts_to_dispatch =
        dispatchStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    bool emptyROB = fromCommit->commitInfo[tid].emptyROB;

    int insts_to_add = insts_to_dispatch.size();
    std::queue<StallReason> dispatch_stalls;
    StallReason breakDispatch = StallReason::NoStall;
    unsigned dispatched = 0;
    while (!insts_to_dispatch.empty()) {
        auto& inst = insts_to_dispatch.front();
        int ins = cpu->cpuStats.committedInsts.total();
        if (cpu->hasHintDownStream() && ins % 10000 == 1) {
            cpu->hintDownStream->notifyIns(ins);
        }
        int id = dispClassify(inst);
        if (dispQue[id].size() < dqSize[id]) {
            if (inst->isSquashed()) {
                ++iewStats.dispSquashedInsts;
                //Tell Rename That An Instruction has been processed
                if (inst->isLoad()) {
                    toRename->iewInfo[tid].dispatchedToLQ++;
                }
                if (inst->isStore() || inst->isAtomic()) {
                    toRename->iewInfo[tid].dispatchedToSQ++;
                }
                toRename->iewInfo[tid].dispatched++;
                insts_to_dispatch.pop_front();

                dispatch_stalls.push(StallReason::InstSquashed);
                continue;
            }

            if ((inst->isSerializeBefore() && !inst->isSerializeHandled()) ? !emptyROB : false) {
                dispatch_stalls.push(StallReason::SerializeStall);
                breakDispatch = StallReason::SerializeStall;
                blockReason = breakDispatch;
                break;
            }

            // hardware transactional memory
            // CPU needs to track transactional state in program order.
            const int numHtmStarts = ldstQueue.numHtmStarts(tid);
            const int numHtmStops = ldstQueue.numHtmStops(tid);
            const int htmDepth = numHtmStarts - numHtmStops;
            if (htmDepth > 0) {
                inst->setHtmTransactionalState(ldstQueue.getLatestHtmUid(tid),
                                                htmDepth);
            } else {
                inst->clearHtmTransactionalState();
            }

            if (inst->isAtomic()) {
                ++iewStats.dispStoreInsts;
                ++iewStats.dispNonSpecInsts;
                toRename->iewInfo[tid].dispatchedToSQ++;
            } else if (inst->isLoad()) {
                ++iewStats.dispLoadInsts;
                toRename->iewInfo[tid].dispatchedToLQ++;
            } else if (inst->isStore()) {
                ++iewStats.dispStoreInsts;
                if (inst->isStoreConditional()) {
                    ++iewStats.dispNonSpecInsts;
                }
                toRename->iewInfo[tid].dispatchedToSQ++;
            }
            toRename->iewInfo[tid].dispatched++;
            ++iewStats.dispatchedInsts;
            dispQue[id].push_back(inst);

            if (!inst->isNop()) {
                scheduler->addProducer(inst);
            }

            inst->enterDQTick = curTick();
            cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDispQue);

            insts_to_dispatch.pop_front();
            dispatched++;
        } else {
            dispatch_stalls.push(checkDispatchStall(tid, id, inst));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }
    }

    if (!dispatch_stalls.empty()) {
        setAllStalls(dispatch_stalls.front());
        dispatch_stalls.pop();
    } else if (breakDispatch != StallReason::NoStall) {
        setAllStalls(breakDispatch);
    } else {
        // no totally stall, pass rename stall
        // assert(dispatched != 0);
        for (int i = 0; i < dispatchStalls.size(); i++) {
            if (i < dispatched) {   // dispatch success, no stall
                dispatchStalls.at(i) = StallReason::NoStall;
            } else {    // dispatch no insts, pass rename stall
                if (fromRename->renameStallReason.size() == 0) {    // initialize, no stall
                    dispatchStalls.at(i) = StallReason::NoStall;
                } else {    // not dispatch initialize, pass rename stall
                    dispatchStalls.at(i) = fromRename->renameStallReason.at(i);
                }
            }
        }
    }


    for (int i = 0;i < dispatchStalls.size();i++) {
        DPRINTF(IEW,"[tid:%i] dispatchStalls[%d]=%d\n", tid, i, dispatchStalls.at(i));
    }

    if (!insts_to_dispatch.empty()) {
        DPRINTF(IEW,"[tid:%i] Dispatch: Bandwidth Full. Blocking.\n", tid);
        block(tid);
        iewStats.stallEvents[DispBWFull]++;
        toRename->iewUnblock[tid] = false;
    }

    if (dispatchStatus[tid] == Idle && insts_to_add) {
        dispatchStatus[tid] = Running;
        updatedQueues = true;
    }
}

void
IEW::dispatchInstFromDispQue(ThreadID tid)
{
    DynInstPtr inst;
    bool add_to_iq = false;
    int dis_num_inst = 0;

    for (int i = 0; i < NumDQ; i++) {
        int dispatched = 0;
        while (!dispQue[i].empty() && dispatched < dispWidth[i]) {
            inst = dispQue[i].front();

            // Check for squashed instructions.
            if (inst->isSquashed()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Squashed instruction encountered, "
                        "not adding to IQ.\n", tid);

                dispQue[i].pop_front();
                continue;
            }

            // Check for ready conditions.(ready: !full && !bwFull )
            if (!instQueue.isReady(inst)) {
                DPRINTF(IEW, "[tid:%i] Dispatch: IQ is full or bwFull.\n", tid);

                iewStats.stallEvents[IQFull]++;
                ++iewStats.iqFullEvents;
                break;
            }

            // Check LSQ if inst is LD/ST
            if ((inst->isAtomic() && ldstQueue.sqFull(tid)) ||
                (inst->isLoad() && ldstQueue.lqFull(tid)) ||
                (inst->isStore() && ldstQueue.sqFull(tid))) {
                DPRINTF(IEW, "[tid:%i] Dispatch: %s has become full.\n",tid,
                        inst->isLoad() ? "LQ" : "SQ");

                iewStats.stallEvents[LSQFull]++;

                ++iewStats.lsqFullEvents;
                break;
            }

            // Otherwise issue the instruction just fine.
            if (inst->isAtomic()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                ldstQueue.insertStore(inst);

                // AMOs need to be set as "canCommit()"
                // so that commit can process them when they reach the
                // head of commit.
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                add_to_iq = false;
            } else if (inst->isLoad()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // Reserve a spot in the load store queue for this
                // memory access.
                ldstQueue.insertLoad(inst);

                add_to_iq = true;

            } else if (inst->isStore()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                ldstQueue.insertStore(inst);

                if (inst->isStoreConditional()) {
                    // Store conditionals need to be set as "canCommit()"
                    // so that commit can process them when they reach the
                    // head of commit.
                    // @todo: This is somewhat specific to Alpha.
                    inst->setCanCommit();
                    instQueue.insertNonSpec(inst);
                    add_to_iq = false;

                } else {
                    add_to_iq = true;
                }
            } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
                // Same as non-speculative stores.
                inst->setCanCommit();
                instQueue.insertBarrier(inst);
                add_to_iq = false;
            } else if (inst->isNop()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Nop instruction encountered, "
                        "skipping.\n", tid);

                inst->setIssued();
                inst->setExecuted();
                inst->setCanCommit();

                iewStats.executedInstStats.numNop[tid]++;

                add_to_iq = false;
            } else {
                assert(!inst->isExecuted());
                add_to_iq = true;
            }

            if (add_to_iq && inst->isNonSpeculative()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Nonspeculative instruction "
                        "encountered, skipping.\n", tid);

                // Same as non-speculative stores.
                inst->setCanCommit();

                // Specifically insert it as nonspeculative.
                instQueue.insertNonSpec(inst);

                add_to_iq = false;
            }

            // If the instruction queue is not full, then add the
            // instruction.
            if (add_to_iq) {
                instQueue.insert(inst);
            }
            ++dis_num_inst;

            inst->exitDQTick = curTick();

    #if TRACING_ON
            inst->dispatchTick = curTick() - inst->fetchTick;
    #endif
            ppDispatch->notify(inst);

            dispQue[i].pop_front();
            dispatched++;
        }
    }
    iewStats.dispDist.sample(dis_num_inst);
}

void
IEW::printAvailableInsts()
{
    int inst = 0;

    std::cout << "Available Instructions: ";

    while (fromIssue->insts[inst]) {

        if (inst%3==0) std::cout << "\n\t";

        std::cout << "PC: " << fromIssue->insts[inst]->pcState()
             << " TN: " << fromIssue->insts[inst]->threadNumber
             << " SN: " << fromIssue->insts[inst]->seqNum << " | ";

        inst++;

    }

    std::cout << "\n";
}

void
IEW::SquashCheckAfterExe(DynInstPtr inst)
{
    ThreadID tid = inst->threadNumber;

    if (!fetchRedirect[tid] ||
        !execWB->squash[tid] ||
        execWB->squashedSeqNum[tid] > inst->seqNum) {

        // Prevent testing for misprediction on load instructions,
        // that have not been executed.
        bool loadNotExecuted = !inst->isExecuted() && inst->isLoad();

        if (inst->mispredicted() && !loadNotExecuted) {
            fetchRedirect[tid] = true;

            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Branch mispredict detected.\n",
                    tid, inst->seqNum);
            DPRINTF(IEW, "[tid:%i] [sn:%llu] "
                    "Predicted target was PC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg());
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Redirecting fetch to PC: %s\n",
                    tid, inst->seqNum, inst->pcState());
            // If incorrect, then signal the ROB that it must be squashed.
            squashDueToBranch(inst, tid);

            ppMispredict->notify(inst);

            if (inst->readPredTaken()) {
                iewStats.predictedTakenIncorrect++;
            } else {
                iewStats.predictedNotTakenIncorrect++;
            }
        } else if (ldstQueue.violation(tid)) {
            assert(inst->isMemRef());
            // If there was an ordering violation, then get the
            // DynInst that caused the violation.  Note that this
            // clears the violation signal.
            DynInstPtr violator;
            violator = ldstQueue.getMemDepViolator(tid);

            DPRINTF(IEW, "LDSTQ detected a violation. Violator PC: %s "
                    "[sn:%lli], inst PC: %s [sn:%lli]. Addr is: %#x.\n",
                    violator->pcState(), violator->seqNum,
                    inst->pcState(), inst->seqNum, inst->physEffAddr);

            fetchRedirect[tid] = true;

            // Tell the instruction queue that a violation has occured.
            instQueue.violation(inst, violator);

            // Squash.
            squashDueToMemOrder(violator, tid);

            ++iewStats.memOrderViolationEvents;
        }
    } else {
        // Reset any state associated with redirects that will not
        // be used.
        if (ldstQueue.violation(tid)) {
            assert(inst->isMemRef());

            DynInstPtr violator = ldstQueue.getMemDepViolator(tid);

            DPRINTF(IEW, "LDSTQ detected a violation.  Violator PC: "
                    "%s, inst PC: %s.  Addr is: %#x.\n",
                    violator->pcState(), inst->pcState(),
                    inst->physEffAddr);
            DPRINTF(IEW, "Violation will not be handled because "
                    "already squashing\n");

            ++iewStats.memOrderViolationEvents;
        }
    }
}

void
IEW::executeInsts()
{
    wbNumInst = 0;
    wbCycle = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        fetchRedirect[tid] = false;
    }

    // Uncomment this if you want to see all available instructions.
    // @todo This doesn't actually work anymore, we should fix it.
//    printAvailableInsts();

    // Execute/writeback any instructions that are available.
    int insts_to_execute = fromIssue->size;
    fromIssue->size = 0;
    int inst_num = 0;
    for (; inst_num < insts_to_execute;
          ++inst_num) {

        DPRINTF(IEW, "Execute: Executing instructions from IQ.\n");

        DynInstPtr inst = instQueue.getInstToExecute();

        DPRINTF(IEW, "Execute: Processing PC %s, [tid:%i] [sn:%llu].\n",
                inst->pcState(), inst->threadNumber,inst->seqNum);

        // Notify potential listeners that this instruction has started
        // executing
        ppExecute->notify(inst);

        // Check if the instruction is squashed; if so then skip it
        if (inst->isSquashed()) {
            DPRINTF(IEW, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                         " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                         inst->seqNum);

            // Consider this instruction executed so that commit can go
            // ahead and retire the instruction.
            inst->setExecuted();

            // Not sure if I should set this here or just let commit try to
            // commit any squashed instructions.  I like the latter a bit more.
            inst->setCanCommit();

            ++iewStats.executedInstStats.numSquashedInsts;

            continue;
        }

        Fault fault = NoFault;

        // Execute instruction.
        // Note that if the instruction faults, it will be handled
        // at the commit stage.
        if (inst->isMemRef()) {
            DPRINTF(IEW, "Execute: Calculating address for memory "
                    "reference.\n");

            // Tell the LDSTQ to execute this instruction (if it is a load).
            if (inst->isAtomic()) {
                // AMOs are treated like store requests
                fault = ldstQueue.executeAmo(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "store.\n");
                    deferMemInst(inst);
                    continue;
                }
            } else if (inst->isLoad()) {
                // add this load inst to loadpipe S0.
                ldstQueue.issueToLoadPipe(inst);
            } else if (inst->isStore()) {
                // add this store inst to storepipe S0.
                ldstQueue.issueToStorePipe(inst);

                // Store conditionals will mark themselves as
                // executed, and their writeback event will add the
                // instruction to the queue to commit.
            } else {
                panic("Unexpected memory type!\n");
            }

        } else {
            // If the instruction has already faulted, then skip executing it.
            // Such case can happen when it faulted during ITLB translation.
            // If we execute the instruction (even if it's a nop) the fault
            // will be replaced and we will lose it.
            if (inst->getFault() == NoFault) {
                inst->execute();
                if (!inst->readPredicate())
                    inst->forwardOldRegs();
            }

            inst->setExecuted();

            instToCommit(inst);
        }

        updateExeInstStats(inst);

        if (debug::IEW) {
            inst->printDisassemblyAndResult(cpu->name());
        }

        // Check if branch prediction was correct, if not then we need
        // to tell commit to squash in flight instructions.  Only
        // handle this if there hasn't already been something that
        // redirects fetch in this group of instructions.

        // This probably needs to prioritize the redirects if a different
        // scheduler is used.  Currently the scheduler schedules the oldest
        // instruction first, so the branch resolution order will be correct.
        if (!(inst->isLoad() || inst->isStore())) {
            // Load/Store will call this in `lsq_unit.cc` after execution
            SquashCheckAfterExe(inst);
        }
    }

    ldstQueue.executePipeSx();

    // Update and record activity if we processed any instructions.
    if (inst_num) {
        if (exeStatus == Idle) {
            exeStatus = Running;
        }

        updatedQueues = true;

        cpu->activityThisCycle();
    }

    // Need to reset this in case a writeback event needs to write into the
    // iew queue.  That way the writeback event will write into the correct
    // spot in the queue.
    wbNumInst = 0;

}

void
IEW::writebackInsts()
{
    // Loop through the head of the time buffer and wake any
    // dependents.  These instructions are about to write back.  Also
    // mark scoreboard that this instruction is finally complete.
    // Either have IEW have direct access to scoreboard, or have this
    // as part of backwards communication.

    int wb_width = wbWidth;
    int count_ = 0;
    while (execWB->insts[count_]) {
        DynInstPtr it = execWB->insts[count_];
        count_++;
        if (it->opClass() == FMAAccOp) {
            wb_width++;
        }
        if (count_ >= wbWidth ||
            wb_width >= wbWidth * 2) {
            break;
        }
    }


    for (int inst_num = 0; inst_num < wb_width &&
             execWB->insts[inst_num]; inst_num++) {
        DynInstPtr inst = execWB->insts[inst_num];
        ThreadID tid = inst->threadNumber;

        if (inst->savedRequest && inst->isLoad()) {
            inst->pf_source = inst->savedRequest->mainReq()->getPFSource();
        }

        DPRINTF(IEW, "Sending instructions to commit, [sn:%lli] PC %s.\n",
                inst->seqNum, inst->pcState());

        iewStats.instsToCommit[tid]++;
        // Notify potential listeners that execution is complete for this
        // instruction.
        ppToCommit->notify(inst);

        // Some instructions will be sent to commit without having
        // executed because they need commit to handle them.
        // E.g. Strictly ordered loads have not actually executed when they
        // are first sent to commit.  Instead commit must tell the LSQ
        // when it's ready to execute the strictly ordered load.
        if (!inst->isSquashed() && inst->isExecuted() &&
                inst->getFault() == NoFault) {

            scheduler->writebackWakeup(inst);
            int dependents = instQueue.wakeDependents(inst);

            for (int i = 0; i < inst->numDestRegs(); i++) {
                // Mark register as ready if not pinned
                if (inst->renamedDestIdx(i)->
                        getNumPinnedWritesToComplete() == 0) {
                    DPRINTF(IEW,"Setting Destination Register %i (%s)\n",
                            inst->renamedDestIdx(i)->index(),
                            inst->renamedDestIdx(i)->className());
                    scoreboard->setReg(inst->renamedDestIdx(i));
                }
            }

            if (dependents) {
                iewStats.producerInst[tid]++;
                iewStats.consumerInst[tid]+= dependents;
            }
            iewStats.writebackCount[tid]++;
        }
    }
}

void
IEW::tick()
{
    for (int i = 0;i < fromRename->fetchStallReason.size();i++) {
        iewStats.fetchStallReason[fromRename->fetchStallReason[i]]++;
    }

    for (int i = 0;i < fromRename->decodeStallReason.size();i++) {
        iewStats.decodeStallReason[fromRename->decodeStallReason[i]]++;
    }

    for (int i = 0;i < fromRename->renameStallReason.size();i++) {
        iewStats.renameStallReason[fromRename->renameStallReason[i]]++;
    }

    wbNumInst = 0;
    wbCycle = 0;

    wroteToTimeBuffer = false;
    updatedQueues = false;

    scheduler->tick();
    ldstQueue.tick();

    sortInsts();

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // Check stall and squash signals, dispatch any instructions.
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(IEW,"Issue: Processing [tid:%i]\n",tid);

        checkSignalsAndUpdate(tid);
        dispatch(tid);

        toRename->iewInfo[tid].robHeadStallReason = checkDispatchStall(tid, NumDQ, nullptr);
        toRename->iewInfo[tid].lqHeadStallReason =
            ldstQueue.lqEmpty() ? StallReason::NoStall : checkLSQStall(tid, true);
        toRename->iewInfo[tid].sqHeadStallReason =
            ldstQueue.sqEmpty() ? StallReason::NoStall : checkLSQStall(tid, false);
        toRename->iewInfo[tid].blockReason = blockReason;
    }
    for (int i = 0;i < dispatchStalls.size();i++) {
        iewStats.dispatchStallReason[dispatchStalls[i]]++;
    }

    if (exeStatus != Squashing) {
        instQueue.scheduleReadyInsts();

        executeInsts();

        writebackInsts();
    }

    scheduler->issueAndSelect();

    bool broadcast_free_entries = false;

    if (updatedQueues || exeStatus == Running || updateLSQNextCycle) {
        exeStatus = Idle;
        updateLSQNextCycle = false;

        broadcast_free_entries = true;
    }

    // Writeback any stores using any leftover bandwidth.
    ldstQueue.writebackStoreBuffer();

    // Check the committed load/store signals to see if there's a load
    // or store to commit.  Also check if it's being told to execute a
    // nonspeculative instruction.
    // This is pretty inefficient...

    threads = activeThreads->begin();
    while (threads != end) {
        ThreadID tid = (*threads++);

        DPRINTF(IEW,"Processing [tid:%i]\n",tid);

        // Update structures based on instructions committed.
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {

            ldstQueue.commitStores(fromCommit->commitInfo[tid].doneSeqNum,tid);

            ldstQueue.commitLoads(fromCommit->commitInfo[tid].doneSeqNum,tid);

            updateLSQNextCycle = true;
            instQueue.commit(fromCommit->commitInfo[tid].doneSeqNum,tid);
        }

        if (fromCommit->commitInfo[tid].nonSpecSeqNum != 0) {

            //DPRINTF(IEW,"NonspecInst from thread %i",tid);
            if (fromCommit->commitInfo[tid].strictlyOrdered) {
                instQueue.replayMemInst(
                    fromCommit->commitInfo[tid].strictlyOrderedLoad);
                fromCommit->commitInfo[tid].strictlyOrderedLoad->setAtCommit();
            } else {
                instQueue.scheduleNonSpec(
                    fromCommit->commitInfo[tid].nonSpecSeqNum);
            }
        }

        if (broadcast_free_entries) {
            toFetch->iewInfo[tid].ldstqCount =
                ldstQueue.getCount(tid);

            toRename->iewInfo[tid].usedIQ = true;
            toRename->iewInfo[tid].usedLSQ = true;

            toRename->iewInfo[tid].freeLQEntries =
                ldstQueue.numFreeLoadEntries(tid);
            toRename->iewInfo[tid].freeSQEntries =
                ldstQueue.numFreeStoreEntries(tid);

            wroteToTimeBuffer = true;
        }

        DPRINTF(IEW, "[tid:%i], Dispatch dispatched %i instructions.\n",
                tid, toRename->iewInfo[tid].dispatched);
    }

    DPRINTF(IEW,"LQ has %i free entries. SQ has %i free entries.\n",
            ldstQueue.numFreeLoadEntries(), ldstQueue.numFreeStoreEntries());

    updateStatus();

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
IEW::updateExeInstStats(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    iewStats.executedInstStats.numInsts++;

    //
    //  Control operations
    //
    if (inst->isControl())
        iewStats.executedInstStats.numBranches[tid]++;

    //
    //  Memory operations
    //
    if (inst->isMemRef()) {
        iewStats.executedInstStats.numRefs[tid]++;

        if (inst->isLoad()) {
            iewStats.executedInstStats.numLoadInsts[tid]++;
        }
    }
}

void
IEW::checkMisprediction(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    if (!fetchRedirect[tid] ||
        !execWB->squash[tid] ||
        execWB->squashedSeqNum[tid] > inst->seqNum) {

        if (inst->mispredicted()) {
            fetchRedirect[tid] = true;

            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Branch mispredict detected.\n",
                    tid, inst->seqNum);
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Predicted target was PC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg());
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Redirecting fetch to PC: %s\n",
                    tid, inst->seqNum, inst->pcState());
            // If incorrect, then signal the ROB that it must be squashed.
            squashDueToBranch(inst, tid);

            if (inst->readPredTaken()) {
                iewStats.predictedTakenIncorrect++;
            } else {
                iewStats.predictedNotTakenIncorrect++;
            }
        }
    }
}

void
IEW::loadCancel(const DynInstPtr &inst)
{
    scheduler->loadCancel(inst);
}

uint32_t
IEW::getIQInsts()
{
    return scheduler->getIQInsts();
}

void
IEW::setAllStalls(StallReason dispatchStall)
{
    for (int i = 0;i < dispatchStalls.size();i++) {
        dispatchStalls.at(i) = dispatchStall;
    }
}

StallReason
IEW::checkLoadStoreInst(DynInstPtr inst)
{
    if (inst->isSquashed()) {
        return StallReason::MemSquashed;
    }
    if (inst->isCommitted()) {
        return StallReason::MemCommitRateLimit;
    }
    if (inst->isAtomic() || inst->isStoreConditional()) {
        return StallReason::Atomic;
    }
    if (!inst->readyToIssue()){
        return StallReason::MemNotReady;
    }
    assert(inst->isLoad() || inst->isStore());

    if (inst->isIssued() && !inst->translationCompleted()) {
        return StallReason::DTlbStall;
    }

    bool inFlight = inst->isIssued() && inst->translationCompleted();
    //Level of the cache hierachy where this request was responded to
    //e.g. 0:in l1, 1:in l2
    int depth=-1;
    if (inFlight) {
        assert(inst->savedRequest);
        depth = inst->savedRequest->mainReq()->depth;
    }
    assert(depth < 5);
    bool in_l1 = depth == 0;
    bool in_l2 = depth == 1;
    bool in_l3 = depth == 2;
    bool other_stall = depth == -1;
    // maybe soc does not have l3cache
    // so we can not use in_mem = depth==3
    bool in_mem = !(in_l1 ||  in_l2 || in_l3 || other_stall);
    if (inFlight && in_l1) {
        return inst->isLoad() ? StallReason::LoadL1Bound : StallReason::StoreL1Bound;
    } else if (inFlight && in_l2) {
        return inst->isLoad() ? StallReason::LoadL2Bound : StallReason::StoreL2Bound;
    } else if (inFlight && in_l3) {
        return inst->isLoad() ? StallReason::LoadL3Bound : StallReason::StoreL3Bound;
    } else if (inFlight && in_mem) {
        return inst->isLoad() ? StallReason::LoadMemBound : StallReason::StoreMemBound;
    }
    return StallReason::OtherMemStall;
}

StallReason
IEW::dqTypeToReason(DQType dq_type)
{
    switch (dq_type) {
        case DQType::IntDQ:
            return StallReason::IntDQBandwidth;
        case DQType::MemDQ:
            return StallReason::MemDQBandwidth;
        case DQType::FVDQ:
            return StallReason::FVDQBandwidth;
        default:
            panic("Unknown DQType");
    }
}

IEW::DQType
IEW::getInstDQType(const DynInstPtr &inst)
{
    if (inst->isMemRef() || inst->isAtomic() || inst->isReadBarrier() || inst->isWriteBarrier() ||
        inst->isNonSpeculative()) {
        return DQType::MemDQ;
    } else if (inst->isFloating() || inst->isVector()) {
        return DQType::FVDQ;
    } else if (inst->isInteger() || inst->isControl() || inst->staticInst->opClass() == enums::No_OpClass) {
        return DQType::IntDQ;
    } else {
        panic("Unknown inst op Class: %i type: %s\n", inst->staticInst->opClass(),
              inst->staticInst->disassemble(inst->pcState().instAddr()));
    }
    return DQType::IntDQ;
}

StallReason
IEW::checkDispatchStall(ThreadID tid, int dq_id, const DynInstPtr &dispatch_inst) {
    DynInstPtr head_inst = rob->readHeadInst(tid);
    if (head_inst == rob->dummyInst) {
        if (dq_id != NumDQ) {  // this call is from dispatch to classify the reason why an instr cannot be dispatched
            return dqTypeToReason(static_cast<DQType>(dq_id));
        } else {  // this call is to tell rename the stall status
            return StallReason::NoStall;
        }
    }

    if (dq_id != NumDQ) {
        // get dq head inst
        assert(dispQue[dq_id].size());
        auto &dq_head = dispQue[dq_id].front();
        if (getInstDQType(dispatch_inst) == getInstDQType(dq_head) && !instQueue.isReady(dq_head)) {
            return dqTypeToReason(static_cast<DQType>(dq_id));
        }

        if (dispatch_inst->isStore() && !ldstQueue.sqFull()) {
            // store cannot be dispatched while sq is not full
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isLoad() && !(ldstQueue.lqFull() || rob->isFull() || instQueue.isFull(dispatch_inst))) {
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isAtCommit() && !(ldstQueue.lqFull() || ldstQueue.sqFull())) {
            return StallReason::MemDQBandwidth;
        }

        if ((dispatch_inst->isFloating() || dispatch_inst->isVector()) &&
            !(rob->isFull() || instQueue.isFull(dispatch_inst))) {
            return StallReason::FVDQBandwidth;
        }

        if (dispatch_inst->isInteger() && !(rob->isFull() || instQueue.isFull(dispatch_inst))) {
            return StallReason::IntDQBandwidth;
        }
    }

    assert(head_inst);

    if (head_inst->readyTick == -1) {
        DPRINTF(Counters, "IEW: [tid:%i] [sn:%llu] "
                "Dispatch: Instruction not ready. nonSpeculative:%d\n",
                tid, head_inst->seqNum, head_inst->isNonSpeculative());
        if (head_inst->isNonSpeculative()) {
            return StallReason::SerializeStall;
        } else if (head_inst->isLoad() && ldstQueue.lqFull(tid)) {
            return checkLSQStall(tid, true);
        } else if ((head_inst->isStore() || head_inst->isAtomic()) && ldstQueue.sqFull(tid)) {
            return checkLSQStall(tid, false);
        } else {
            return StallReason::InstNotReady;
        }
    }

    if (head_inst->isLoad() || head_inst->isStore() || head_inst->isAtomic()) {
        return checkLoadStoreInst(head_inst);
    } else {
        if (head_inst->firstIssue != -1) {
            if (head_inst->isVector()) {
                return StallReason::VectorLongExecute;
            } else {
                return StallReason::ScalarLongExecute;
            }
        } else {
            if (head_inst->isVector()) {
                return StallReason::VectorReadyButNotIssued;
            } else {
                return StallReason::ScalarReadyButNotIssued;
            }
        }
    }

    return StallReason::OtherStall;
}

StallReason
IEW::checkLSQStall(ThreadID tid, bool isLoad)
{
    DynInstPtr head_inst = ldstQueue.getLSQHeadInst(tid, isLoad);
    return checkLoadStoreInst(head_inst);
}

void
IEW::setRob(ROB *rob)
{
    this->rob = rob;
}

} // namespace o3
} // namespace gem5
