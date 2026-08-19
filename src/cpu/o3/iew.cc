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

#include <algorithm>
#include <cassert>
#include <queue>

#include "arch/riscv/pcstate.hh"
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
#include "cpu/op_class.hh"
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
      valuePred(params.valuePred),
      enableSelectiveVPFlush(params.enableSelectiveVPFlush),
      cpu(_cpu),
      scheduler(params.scheduler),
      instQueue(_cpu, this, params),
      ldstQueue(_cpu, this, params),
      dispWidth(params.dispWidth),
      commitToIEWDelay(params.commitToIEWDelay),
      renameToIEWDelay(params.renameToIEWDelay),
      enableDispatchStage(params.enableDispatchStage),
      renameWidth(params.renameWidth),
      wbNumInst(0),
      wbCycle(0),
      iewToCommitDelay(params.iewToCommitDelay),
      wbWidth(params.wbWidth),
      enableStoreSetTrain(params.enable_storeSet_train),
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

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        fetchRedirect[tid] = false;
        serializeOnNextInst[tid] = false;
    }

    assert(renameToIEWDelay == 1);

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
             "Number of cycles IEW is idle per thread"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is squashing per thread"),
    ADD_STAT(blockCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is blocking per thread"),
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
    ADD_STAT(smtStallEvents, statistics::units::Count::get(),
             "Number of events the IEW has stalled per thread"),
    ADD_STAT(fetchStallReason, statistics::units::Count::get(),
             "Number of fetch stall reasons each tick (Total)"),
    ADD_STAT(decodeStallReason, statistics::units::Count::get(),
             "Number of decode stall reasons each tick (Total)"),
    ADD_STAT(renameStallReason, statistics::units::Count::get(),
             "Number of rename stall reasons each tick (Total)"),
    ADD_STAT(dispatchStallReason, statistics::units::Count::get(),
             "Number of dispatch stall reasons each tick (Total)")
{
    idleCycles
        .init(cpu->numThreads)
        .flags(statistics::total);

    squashCycles
        .init(cpu->numThreads)
        .flags(statistics::total);

    blockCycles
        .init(cpu->numThreads)
        .flags(statistics::total);

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

    dispatchedInsts
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
        
    smtStallEvents
        .init(StallEventCount,0,cpu->numThreads-1,1)
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
        smtStallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
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
        {StallReason::ControlRecovery, "ControlRecovery"},
        {StallReason::MemVioRecovery, "MemVioRecovery"},
        {StallReason::VPRecovery, "VPRecovery"},
        {StallReason::TrapRecovery, "TrapRecovery"},
        {StallReason::ROBFull, "ROBFull"},
        {StallReason::RegFull, "RegFull"},
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
    : statistics::Group(cpu, "executed_inst"),
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
    // Initialize the checker's dcache port here
    if (cpu->checker) {
        cpu->checker->setDcachePort(&ldstQueue.getDataPort());
    }

    cpu->activateStage(CPU::IEWIdx);
}

void
IEW::clearStates(ThreadID tid)
{
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

    // Setup wire to write instructions and squash signals to commit.
    // Note: This wire is named "toCommit" (previously "execWB") to clarify its purpose.
    // Since iewToCommitDelay == 1, IEW writes to position [-1] and Commit reads from
    // position [-1] (via fromIEW) in the same cycle, achieving zero-cycle latency for
    // IEW→Commit communication. This allows Commit to immediately arbitrate squash
    // signals from IEW (e.g., branch mispredictions) before forwarding to Fetch.
    toCommit = iewQueue->getWire(0);
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

void
IEW::lvpWakeDependents(const DynInstPtr &inst) {
    assert(inst->numDestRegs() == 1);
    if (enableSelectiveVPFlush) {
        scheduler->specWakeUpFromVP(inst);
        DPRINTF(IEW,"[sn:%llu] vp specWakeUp dependents\n", inst->seqNum);
    } else {
        for (int i = 0; i < inst->numDestRegs(); i++) {
            auto dest = inst->renamedDestIdx(i);
            if (dest->isFixedMapping()) {
                continue;
            }
            scheduler->setAllScoreBoard(dest);
            DPRINTF(IEW,"[sn:%llu] vp set scoreboard to true\n", inst->seqNum);
        }
    }
}

bool
IEW::isDrained() const
{
    bool drained = ldstQueue.isDrained() && instQueue.isDrained();
    if (!drained) {
        DPRINTF(Drain, "LDSTQ or IQ not drained.\n");
        return false;
    }

    for (int i=0;i<numThreads;i++) {
        if (!fixedbuffer[i].empty()) {
            DPRINTF(Drain, "%i: Insts not empty.\n", i);
            drained = false;
            break;
        }
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
    recordThreadSquash(tid);

    DPRINTF(IEW, "[tid:%i] Squashing all instructions.\n", tid);

    for (auto& dp : dispQue) {
        for (auto& it : dp) {
            if (it->seqNum > fromCommit->commitInfo[tid].doneSeqNum && (it->threadNumber == tid)) {
                it->setSquashed();
            }
        }
    }

    // Tell the IQ to start squashing.
    instQueue.squash(tid);

    // Tell the LDSTQ to start squashing.
    ldstQueue.squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
    updatedQueues = true;

    fixedbuffer[tid].clear();

    stallSig->blockRename[tid] = true;

    // Clear the skid buffer in case it has any data in it.
    DPRINTF(IEW,
            "Removing skidbuffer instructions until "
            "[sn:%llu] [tid:%i]\n",
            fromCommit->commitInfo[tid].doneSeqNum, tid);
}

void
IEW::squashDueToBranch(const DynInstPtr& inst, ThreadID tid)
{
    recordThreadSquash(tid);

    DPRINTF(IEW, "[tid:%i] [sn:%llu] Squashing from a specific instruction,"
            " PC: %s "
            "\n", tid, inst->seqNum, inst->pcState() );

    if (!toCommit->squash[tid] || inst->seqNum < toCommit->squashedSeqNum[tid]) {
        toFetch->iewInfo[tid].redirectPending = true;
        toCommit->squash[tid] = true;
        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->squashedTargetId[tid] = inst->getFtqId();
        toCommit->squashedLoopIter[tid] = inst->getLoopIteration();
        toCommit->branchTaken[tid] = inst->pcState().branching();

        set(toCommit->pc[tid], inst->pcState());
        inst->staticInst->advancePC(*toCommit->pc[tid]);

        toCommit->mispredictInst[tid] = inst;
        toCommit->includeSquashInst[tid] = false;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "Branch misprediction (pc=%#lx) set target "
                "id to %lu, loop iter to %u\n",
                toCommit->pc[tid]->instAddr(),
                toCommit->squashedTargetId[tid],
                toCommit->squashedLoopIter[tid]);
    }

    stallSig->blockRename[tid] = true;
}

void
IEW::squashDueToMemOrder(const DynInstPtr& inst, ThreadID tid)
{
    recordThreadSquash(tid);

    DPRINTF(IEW, "[tid:%i] Memory violation, squashing violator and younger "
            "insts, PC: %s [sn:%llu].\n", tid, inst->pcState(), inst->seqNum);
    // Need to include inst->seqNum in the following comparison to cover the
    // corner case when a branch misprediction and a memory violation for the
    // same instruction (e.g. load PC) are detected in the same cycle.  In this
    // case the memory violator should take precedence over the branch
    // misprediction because it requires the violator itself to be included in
    // the squash.
    if (!toCommit->squash[tid] || inst->seqNum <= toCommit->squashedSeqNum[tid]) {
        toFetch->iewInfo[tid].redirectPending = true;
        toCommit->squash[tid] = true;

        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->squashedTargetId[tid] = inst->getFtqId();
        toCommit->squashedLoopIter[tid] = inst->getLoopIteration();
        set(toCommit->pc[tid], inst->pcState());
        toCommit->mispredictInst[tid] = NULL;

        // Must include the memory violator in the squash.
        toCommit->includeSquashInst[tid] = true;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "Memory violation (pc=%#lx) set target id "
                "to %lu, loop iter to %u\n",
                toCommit->pc[tid]->instAddr(),
                toCommit->squashedTargetId[tid],
                toCommit->squashedLoopIter[tid]);
    }

    stallSig->blockRename[tid] = true;
}

void
IEW::squashDueToValuePrediction(const DynInstPtr &inst, ThreadID tid)
{
    recordThreadSquash(tid);

    DPRINTF(IEW, "[tid:%i] value prediction error, squashing violator and younger "
            "insts, PC: %s [sn:%llu].\n",
            tid, inst->pcState(), inst->seqNum);
    if (!toCommit->squash[tid] || inst->seqNum < toCommit->squashedSeqNum[tid]) {
        toFetch->iewInfo[tid].redirectPending = true;
        toCommit->squash[tid] = true;

        toCommit->valuePredictionError[tid] = true;
        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->squashedTargetId[tid] = inst->getFtqId();
        toCommit->squashedLoopIter[tid] = inst->getLoopIteration();
        set(toCommit->pc[tid], inst->pcState());

        // advance pc to next instruction
        inst->staticInst->advancePC(*toCommit->pc[tid]);

        toCommit->mispredictInst[tid] = NULL;

        // Even speculatively executed value prediction instructions cannot
        // be squashed after obtaining a correct result.
        toCommit->includeSquashInst[tid] = false;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "value prediction error (pc=%#lx) set target id "
                "to %lu, loop iter to %u\n",
                toCommit->pc[tid]->instAddr(),
                toCommit->squashedTargetId[tid],
                toCommit->squashedLoopIter[tid]);
    }

    stallSig->blockRename[tid] = true;
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
IEW::cacheMissLdReplay(const DynInstPtr& inst)
{
    instQueue.cacheMissLdReplay(inst);
}

void
IEW::cacheUnblocked()
{
    instQueue.cacheUnblocked();
}

void
IEW::readyToFinish(const DynInstPtr& inst)
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

    ThreadID tid = inst->threadNumber;
    if (inst->vpMisprediction) {
        if (!enableSelectiveVPFlush) {
            if (!fetchRedirect[tid] || !toCommit->squash[tid] ||
                toCommit->squashedSeqNum[tid] > inst->seqNum) {
                fetchRedirect[tid] = true;
                squashDueToValuePrediction(inst, tid);
            }
        } else {
            // VP selective flush: cancel data-dependent consumers when possible.
            // If any dependent consumer is already issued, fallback to squash.
            DPRINTF(IEW, "[sn:%llu] VP misprediction detected, "
                    "selective cancel via loadCancel\n", inst->seqNum);
            bool needSquashFallback = scheduler->loadCancel(inst);
            if (needSquashFallback) {
                DPRINTF(IEW, "[sn:%llu] VP fallback to squash due to issued dependent\n",
                        inst->seqNum);
                if (!fetchRedirect[tid] || !toCommit->squash[tid] ||
                    toCommit->squashedSeqNum[tid] > inst->seqNum) {
                    fetchRedirect[tid] = true;
                    squashDueToValuePrediction(inst, tid);
                }
            } else {
                // Writeback with real value to re-wake consumers
                scheduler->writebackWakeup(inst);
            }
        }
    }

    DPRINTF(IEW, "Current wb cycle: %i, width: %i, numInst: %i\nwbActual:%i\n",
            wbCycle, wbWidth, wbNumInst, wbCycle * wbWidth + wbNumInst);
    // Add finished instruction to queue to commit.
    (*iewQueue)[wbCycle].insts[wbNumInst] = inst;
    (*iewQueue)[wbCycle].size++;
}

void
IEW::updateActivate()
{
    bool any_unblocking = false;

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
IEW::checkSerialize(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;
    bool skipserialize = fromCommit->commitInfo[tid].robheadSeqNum >= inst->seqNum;

    if (serializeOnNextInst[tid]) {
        inst->setSerializeBefore();
        serializeOnNextInst[tid] = false;
    }

    if (inst->isSerializeBefore() && !skipserialize) {
        return true;
    } else if (inst->isStoreConditional() || inst->isSerializeAfter()) {
        serializeOnNextInst[tid] = true;
        return false;
    }

    return false;
}

void
IEW::checkSquash()
{
    // Check if there's a squash signal, squash if there is
    // Check stall signals, block if there is.
    // If status was Blocked
    //     if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    for (int i = 0; i < numThreads; i++) {
        if (fromCommit->commitInfo[i].squash) {
            squash(i);
            localSquashVer[i].update(
                fromCommit->commitInfo[i].squashVersion.getVersion());
            DPRINTF(IEW, "Updating squash version to %u\n",
                    localSquashVer[i].getVersion());

            fetchRedirect[i] = false;
            iewStats.stallEvents[ROBWalk]++;
            iewStats.smtStallEvents[ROBWalk].sample(i);
            setAllStalls(
                squashCauseToStallReason(fromCommit->commitInfo[i].squashCause));
        }

        if (fromCommit->commitInfo[i].robSquashing) {
            recordThreadSquash(i);
            DPRINTF(IEW, "[tid:%i] ROB is still squashing.\n", i);

            wroteToTimeBuffer = true;
            iewStats.stallEvents[ROBWalk]++;
            iewStats.smtStallEvents[ROBWalk].sample(i);
            setAllStalls(
                squashCauseToStallReason(fromCommit->commitInfo[i].squashCause));
        }
    }
}

void
IEW::moveInstsToBuffer()
{
    int insts_from_rename = fromRename->size;
    if (insts_from_rename == 0) {
        DPRINTF(IEW, "No instructions from rename to move to buffer.\n");
        return;
    }
    ThreadID tid = fromRename->insts[0]->threadNumber;
    assert(fixedbuffer[tid].empty());
    for (int i = 0; i < insts_from_rename; ++i) {
        const DynInstPtr &inst = fromRename->insts[i];
        assert(inst->threadNumber == tid);
        if (localSquashVer[tid].largerThan(inst->getVersion())) {
            inst->setSquashed();
        } else {
            fixedbuffer[tid].push_back(inst);
        }
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

bool
IEW::canInsertLDSTQue(ThreadID tid)
{
    int freeLQEntries = ldstQueue.getFreeLQEntries(tid);
    int freeSQEntries = ldstQueue.getFreeSQEntries(tid);

    int lastClockLQPopEntries = ldstQueue.getAndResetLastLQPopEntries(tid);
    int lastClockSQPopEntries = ldstQueue.getAndResetLastSQPopEntries(tid);
    if (freeLQEntries >= renameWidth + lastClockLQPopEntries &&
        freeSQEntries >= renameWidth + lastClockSQPopEntries) {
        return true;
    }
    return false;
}

void
IEW::setDispatchAgeCtr(const DynInstPtr& inst, int dispatch_pos)
{
    const uint64_t dispatchAgeScale = std::max<uint64_t>(8, renameWidth);

    assert(dispatch_pos >= 0);
    assert(dispatch_pos < static_cast<int>(dispatchAgeScale));
    inst->ageCtr = static_cast<uint64_t>(cpu->curCycle()) * dispatchAgeScale +
                   static_cast<uint64_t>(dispatch_pos);
    DPRINTF(IEW, "[tid:%i] [sn:%llu] ageCtr=%llu at dispatch pos %d.\n",
            inst->threadNumber, inst->seqNum,
            static_cast<unsigned long long>(inst->ageCtr), dispatch_pos);
}

bool
IEW::threadHasStageWork(ThreadID tid)
{
    if (!fixedbuffer[tid].empty() || scheduler->getIQInsts(tid) != 0 ||
        ldstQueue.getCount(tid) != 0) {
        return true;
    }

    for (const auto &queue : dispQue) {
        for (const auto &inst : queue) {
            if (inst->threadNumber == tid) {
                return true;
            }
        }
    }

    for (int i = 0; i < fromIssue->size; ++i) {
        if (fromIssue->insts[i] && fromIssue->insts[i]->threadNumber == tid) {
            return true;
        }
    }

    for (int i = 0; i < MaxWidth; ++i) {
        if (toCommit->insts[i] && toCommit->insts[i]->threadNumber == tid) {
            return true;
        }
    }

    return false;
}

void
IEW::recordThreadWork(ThreadID tid)
{
    cycleThreadWork[tid] = true;
}

void
IEW::recordThreadSquash(ThreadID tid)
{
    cycleThreadSquash[tid] = true;
    recordThreadWork(tid);
}

void
IEW::dispatchInsts()
{
    if (enableDispatchStage) {
        dispatchInstFromDispQue();
    }

    // check threads stall & status
    SmtActiveThreadArbiter active_arbiter;
    auto freezeActiveThread = [this](ThreadID tid) {
        stallSig->blockRename[tid] = true;
        stallSig->renameBlockReason[tid] = StallReason::OtherFragStall;
        toRename->iewInfo[tid].blockReason = StallReason::OtherFragStall;
    };
    for (int i = 0; i < numThreads; i++) {
        auto &iew_info = toRename->iewInfo[i];
        iew_info.robHeadStallReason =
            checkDispatchStall(i, NumDQ, nullptr, -1);
        iew_info.lqHeadStallReason =
            ldstQueue.lqEmpty(i) ? StallReason::NoStall :
                                   checkLSQStall(i, true);
        iew_info.sqHeadStallReason =
            ldstQueue.sqEmpty(i) ? StallReason::NoStall :
                                   checkLSQStall(i, false);
        iew_info.ldstqCount = ldstQueue.getCount(i);
        iew_info.robCount = rob->getThreadEntries(i);
        iew_info.iqCount = scheduler->getIQInsts(i);

        bool ldst_block = !canInsertLDSTQue(i);
        bool rename_block = stallSig->blockIEW[i] || ldst_block;
        // LDST queue reservation gates new rename input, but the already
        // buffered tail must still drain through per-instruction LSQ checks.
        bool active = !stallSig->blockIEW[i] && !fixedbuffer[i].empty();
        StallReason block_reason = StallReason::NoStall;
        if (stallSig->blockIEW[i]) {
            block_reason = stallSig->iewBlockReason[i];
        } else if (ldst_block) {
            block_reason = iew_info.robHeadStallReason;
            if (block_reason == StallReason::NoStall) {
                block_reason = StallReason::OtherStall;
            }
        }
        iew_info.blockReason = rename_block ? block_reason : StallReason::NoStall;

        stallSig->blockRename[i] = rename_block;
        stallSig->renameBlockReason[i] =
            rename_block ? block_reason : StallReason::NoStall;
        if (active) {
            const auto freeze =
                active_arbiter.observe(i, smtBorrowPriority(iew_info));
            if (freeze.previousActive != InvalidThreadID) {
                freezeActiveThread(freeze.previousActive);
            }
            if (freeze.freezeCurrent) {
                freezeActiveThread(i);
            }
        }
    }
    const ThreadID tid = active_arbiter.selected();

    if (tid != InvalidThreadID) {
        DPRINTF(IEW,"Processing [tid:%i]\n",tid);

        // dispatch to IQ
        if (enableDispatchStage) {
            classifyInstToDispQue(tid);
        } else {
            dispatchInstFromRename(tid);
        }
        // check stall again
        if (!fixedbuffer[tid].empty()) {
            stallSig->blockRename[tid] = true;
            stallSig->renameBlockReason[tid] =
                blockReason == StallReason::NoStall ?
                    StallReason::OtherFragStall : blockReason;
            DPRINTF(IEW, "Dispatch bandwidth full, blocking thread %i\n", tid);
        }

        toRename->iewInfo[tid].robHeadStallReason = checkDispatchStall(tid, NumDQ, nullptr, -1);
        toRename->iewInfo[tid].lqHeadStallReason =
            ldstQueue.lqEmpty(tid) ? StallReason::NoStall : checkLSQStall(tid, true);
        toRename->iewInfo[tid].sqHeadStallReason =
            ldstQueue.sqEmpty(tid) ? StallReason::NoStall : checkLSQStall(tid, false);
        toRename->iewInfo[tid].blockReason = blockReason;
        toRename->iewInfo[tid].ldstqCount = ldstQueue.getCount(tid);
        toRename->iewInfo[tid].robCount = rob->getThreadEntries(tid);
        toRename->iewInfo[tid].iqCount = scheduler->getIQInsts(tid);
    }
}

void
IEW::dispatchInstFromRename(ThreadID tid)
{
    DynInstPtr inst;

    auto &insts_to_dispatch = fixedbuffer[tid];

    bool emptyROB = fromCommit->commitInfo[tid].emptyROB;

    int insts_to_add = insts_to_dispatch.size();
    std::queue<StallReason> dispatch_stalls;
    StallReason breakDispatch = StallReason::NoStall;

    unsigned dispatched = 0;
    int disp_seq = -1;

    scheduler->lookahead(insts_to_dispatch);
    while (!insts_to_dispatch.empty()) {
        bool add_to_iq = false;
        auto &inst = insts_to_dispatch.front();
        disp_seq++;
        int ins = cpu->cpuStats.committedInsts.total();
        if (cpu->hasHintDownStream() && ins % 10000 == 1) {
            cpu->hintDownStream->notifyIns(ins);
        }

        if (inst->isSquashed()) {
            ++iewStats.dispSquashedInsts;
            insts_to_dispatch.pop_front();

            dispatch_stalls.push(StallReason::InstSquashed);
            continue;
        }

        if (checkSerialize(inst)) {
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Dispatch: Serialize instruction encountered.\n", tid, inst->seqNum);
            dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }

        // Check LSQ if inst is LD/ST
        if ((inst->isAtomic() && ldstQueue.sqFull(tid)) || (inst->isLoad() && ldstQueue.lqFull(tid)) ||
            (inst->isStore() && ldstQueue.sqFull(tid))) {
            DPRINTF(IEW, "[tid:%i] Dispatch: %s has become full.\n", tid, inst->isLoad() ? "LQ" : "SQ");

            iewStats.stallEvents[LSQFull]++;
            iewStats.smtStallEvents[LSQFull].sample(tid);

            ++iewStats.lsqFullEvents;
            dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }

        if (!scheduler->ready(inst, disp_seq)) {
            DPRINTF(IEW, "[tid:%i] Dispatch: IQ is full or bwFull.\n", tid);
            iewStats.stallEvents[IQFull]++;
            iewStats.smtStallEvents[IQFull].sample(tid);

            ++iewStats.iqFullEvents;

            dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }

        const int numHtmStarts = ldstQueue.numHtmStarts(tid);
        const int numHtmStops = ldstQueue.numHtmStops(tid);
        const int htmDepth = numHtmStarts - numHtmStops;
        if (htmDepth > 0) {
            inst->setHtmTransactionalState(ldstQueue.getLatestHtmUid(tid), htmDepth);
        } else {
            inst->clearHtmTransactionalState();
        }

        setDispatchAgeCtr(inst, dispatched);

        if (!inst->isNop() && !inst->isEliminated()) {
            scheduler->addProducer(inst);
        }

        if (inst->isAtomic()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Memory instruction "
                    "encountered, adding to LSQ.\n",
                    tid);
            ++iewStats.dispStoreInsts;
            ++iewStats.dispNonSpecInsts;

            ldstQueue.insertStore(inst);
            inst->setCanCommit();
            instQueue.insertNonSpec(inst);
            add_to_iq = false;
        } else if (inst->isLoad()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Memory instruction "
                    "encountered, adding to LSQ.\n",
                    tid);
            ++iewStats.dispLoadInsts;

            ldstQueue.insertLoad(inst);
            add_to_iq = true;
            if (valuePred && inst->vpSupported && inst->vpResult.speculative) {
                lvpWakeDependents(inst);
            }
        } else if (inst->isStore()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Memory instruction "
                    "encountered, adding to LSQ.\n",
                    tid);
            ++iewStats.dispStoreInsts;

            ldstQueue.insertStore(inst);
            if (inst->isStoreConditional()) {
                ++iewStats.dispNonSpecInsts;
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                add_to_iq = false;
            } else {
                add_to_iq = true;
            }
        } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
            inst->setCanCommit();
            instQueue.insertBarrier(inst);
            add_to_iq = false;
        } else if (inst->isNop() || inst->isEliminated()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Nop instruction [sn:%llu] encountered, "
                    "skipping.\n",
                    tid, inst->seqNum);
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
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Nonspeculative instruction "
                    "encountered, skipping.\n",
                    tid);
            inst->setCanCommit();
            instQueue.insertNonSpec(inst);
            add_to_iq = false;
        }

        if (add_to_iq) {
            instQueue.insert(inst, disp_seq);
        }
        ppDispatch->notify(inst);

        ++iewStats.dispatchedInsts[tid];

        insts_to_dispatch.pop_front();
        dispatched++;
    }

    iewStats.dispDist.sample(dispatched);

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

        iewStats.stallEvents[DispBWFull]++;
        iewStats.smtStallEvents[DispBWFull].sample(tid);
        
    }

}

void
IEW::classifyInstToDispQue(ThreadID tid)
{
    auto &insts_to_dispatch = fixedbuffer[tid];

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
        int id = getInstDQType(inst);
        if (dispQue[id].size() < dqSize[id]) {
            if (inst->isSquashed()) {
                ++iewStats.dispSquashedInsts;
                insts_to_dispatch.pop_front();

                dispatch_stalls.push(StallReason::InstSquashed);
                continue;
            }

            if (checkSerialize(inst)) {
                DPRINTF(IEW, "[tid:%i] [sn:%llu] Dispatch: Serialize instruction encountered.\n", tid, inst->seqNum);
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

            setDispatchAgeCtr(inst, dispatched);

            if (inst->isAtomic()) {
                ++iewStats.dispStoreInsts;
                ++iewStats.dispNonSpecInsts;
            } else if (inst->isLoad()) {
                ++iewStats.dispLoadInsts;
            } else if (inst->isStore()) {
                ++iewStats.dispStoreInsts;
                if (inst->isStoreConditional()) {
                    ++iewStats.dispNonSpecInsts;
                }
            }
            ++iewStats.dispatchedInsts[tid];
            dispQue[id].push_back(inst);

            if (!inst->isNop() && !inst->isEliminated()) {
                scheduler->addProducer(inst);
            }

            inst->enterDQTick = curTick();
            cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDispQue);

            if (valuePred && inst->vpSupported && inst->vpResult.speculative) {
                lvpWakeDependents(inst);
            }

            insts_to_dispatch.pop_front();
            dispatched++;
        } else {
            dispatch_stalls.push(checkDispatchStall(tid, id, inst, -1));
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
        iewStats.stallEvents[DispBWFull]++;
        iewStats.smtStallEvents[DispBWFull].sample(tid);
    }
}

void
IEW::dispatchInstFromDispQue()
{
    DynInstPtr inst;
    int dis_num_inst = 0;

    for (int i = 0; i < NumDQ; i++) {
        int dispatched = 0;
        int disp_seq = -1;
        scheduler->lookahead(dispQue[i]);
        while (!dispQue[i].empty() && dispatched < dispWidth[i]) {
            inst = dispQue[i].front();
            ThreadID tid = inst->threadNumber;
            disp_seq++;

            // Check for squashed instructions.
            if (inst->isSquashed()) {
                DPRINTF(IEW, "[tid:%i] [sn:%llu] Dispatch: Squashed instruction encountered, "
                        "not adding to IQ.\n", tid, inst->seqNum);

                dispQue[i].pop_front();
                continue;
            }

            // Check for ready conditions.(ready: !full && !bwFull )
            if (!scheduler->ready(inst, disp_seq)) {
                DPRINTF(IEW, "[tid:%i] Dispatch: IQ is full or bwFull.\n", tid);

                iewStats.stallEvents[IQFull]++;
                iewStats.smtStallEvents[IQFull].sample(tid);
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
                iewStats.smtStallEvents[LSQFull].sample(tid);
                ++iewStats.lsqFullEvents;
                break;
            }

            bool add_to_iq = false;
            // Otherwise issue the instruction just fine.
            if (inst->isAtomic()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in store queue
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

                // allocate entry in load queue
                ldstQueue.insertLoad(inst);

                add_to_iq = true;

            } else if (inst->isStore()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in store queue
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
            } else if (inst->isNop() || inst->isEliminated()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Nop instruction [sn:%llu] encountered, "
                        "skipping.\n", tid, inst->seqNum);

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
                DPRINTF(IEW, "[tid:%i] Dispatch: [sn:%llu] dispatched to IQ.\n", tid, inst->seqNum);
                instQueue.insert(inst, disp_seq);
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

    if (inst->isControl()) {
        auto &resolved_cfis = toFetch->iewInfo[tid].resolvedCFIs;
        TimeStruct::IewComm::ResolvedCFIEntry entry;
        entry.ftqId = inst->getFtqId();
        entry.pc = inst->getPC();
        resolved_cfis.push_back(entry);
    }

    if (!fetchRedirect[tid] ||
        !toCommit->squash[tid] ||
        toCommit->squashedSeqNum[tid] > inst->seqNum) {

        // Prevent testing for misprediction on load instructions,
        // that have not been executed.
        bool loadNotExecuted = !inst->isExecuted() && inst->isLoad();

        if (cpu->isTraceMode() && inst->hasTraceBranchInfo()) {
            std::unique_ptr<PCStateBase> new_pc(inst->pcState().clone());
            new_pc->as<RiscvISA::PCState>().npc(inst->traceBranchNextPC());
            inst->pcState(*new_pc);
        }

        if (inst->mispredicted() && !loadNotExecuted &&
            !inst->isNonSpeculative()) {
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
            if (enableStoreSetTrain) {
                instQueue.violation(inst, violator);
            }
            violator->setProducerStorePC(inst->pcState().instAddr());

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
        auto &iew_info = toFetch->iewInfo[tid];
        iew_info.ldstqCount = ldstQueue.getCount(tid);
        iew_info.robCount = rob->getThreadEntries(tid);
        iew_info.iqCount = scheduler->getIQInsts(tid);
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

        // Notify potential listeners that this instruction has started
        // executing
        ppExecute->notify(inst);

        if (inst->isSplitStoreData() &&
            ldstQueue.splitStoreAddrSquashed(inst)) {
            inst->setSquashed();
        }

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

            // avoid "not a load cancel" for using the squashed instruction's data
            scheduler->bypassWriteback(inst);

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

            if (!inst->isSplitStoreData()) {
                inst->setExecuted();
                readyToFinish(inst);
            } else {
                DPRINTF(IEW, "Execute: Split store data, [sn:%lli]\n", inst->seqNum);
                // STD is ready, wake up corresponding load if any
                instQueue.resolveSTLFFailInst(inst->seqNum);
                if (inst->sqIt->splitStoreFinish()) {
                    readyToFinish(inst->sqIt->instruction());
                }
            }
        }

        updateExeInstStats(inst);

        // Check if branch prediction was correct, if not then we need
        // to tell commit to squash in flight instructions.  Only
        // handle this if there hasn't already been something that
        // redirects fetch in this group of instructions.

        // This probably needs to prioritize the redirects if a different
        // scheduler is used.  Currently the scheduler schedules the oldest
        // instruction first, so the branch resolution order will be correct.
        if (!(inst->isLoad() || inst->isStore() || inst->isSplitStoreData())) {
            // because Load/Store become pipeline execution ,Load/Store will
            // call this in `lsq_unit.cc` after execution
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

    for (int inst_num = 0; inst_num < wbWidth &&
             toCommit->insts[inst_num]; inst_num++) {
        DynInstPtr inst = toCommit->insts[inst_num];
        ThreadID tid = inst->threadNumber;

        if (inst->isLoad()) {
            inst->pf_source = ldstQueue.getLoadPFSource(inst);
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
    blockReason = StallReason::NoStall;
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
    cycleThreadWork.fill(false);
    cycleThreadSquash.fill(false);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        toFetch->iewInfo[tid].redirectPending = false;
        toFetch->iewInfo[tid].resolvedCFIs.clear();
    }

    scheduler->tick();
    ldstQueue.tick();

    // dispatch
    moveInstsToBuffer();

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        cycleThreadWork[tid] = threadHasStageWork(tid);
    }

    checkSquash();
    dispatchInsts();

    for (int i = 0;i < dispatchStalls.size();i++) {
        iewStats.dispatchStallReason[dispatchStalls[i]]++;
    }

    // update the LSQ and scheduler before we check for ready instructions to execute
    ldstQueue.processWriteback();
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

    // Check the committed load/store signals to see if there's a load
    // or store to commit.  Also check if it's being told to execute a
    // nonspeculative instruction.
    // This is pretty inefficient...

    auto threads = activeThreads->begin();
    while (threads != activeThreads->end()) {
        ThreadID tid = (*threads++);

        DPRINTF(IEW,"Commit processing [tid:%i]\n",tid);

        if (fromCommit->commitInfo[tid].doneMemSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {
            recordThreadWork(tid);

            // Marks some of the entries in the store queue as canWB and
            // they will be moved to the store buffer when appropriate.
            ldstQueue.commitStores(fromCommit->commitInfo[tid].doneMemSeqNum,tid);
            updateLSQNextCycle = true;
        }

        // Update structures based on instructions committed.
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {
            recordThreadWork(tid);

            ldstQueue.commitLoads(fromCommit->commitInfo[tid].doneSeqNum,tid);
            updateLSQNextCycle = true;

            instQueue.commit(fromCommit->commitInfo[tid].doneSeqNum,tid);
        }

        if (fromCommit->commitInfo[tid].nonSpecSeqNum != 0) {
            recordThreadWork(tid);

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
            wroteToTimeBuffer = true;
        }
    }

    // Classify every thread once per cycle, using work and squash observed
    // across the entire tick rather than only the pre-execute snapshot.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        cycleThreadWork[tid] = cycleThreadWork[tid] || threadHasStageWork(tid);

        if (cycleThreadSquash[tid]) {
            ++iewStats.squashCycles[tid];
        } else if (stallSig->blockRename[tid]) {
            ++iewStats.blockCycles[tid];
        } else if (!cycleThreadWork[tid]) {
            ++iewStats.idleCycles[tid];
        }
    }

    DPRINTF(IEW,"LQ has %i free entries. SQ has %i free entries.\n",
            ldstQueue.numFreeLoadEntries(), ldstQueue.numFreeStoreEntries());

    updateActivate();

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
        !toCommit->squash[tid] ||
        toCommit->squashedSeqNum[tid] > inst->seqNum) {
        // In trace mode, override npc with trace nextPC so that
        // mispredicted() and squash use standard advancePC logic.
        if (cpu->isTraceMode() && inst->hasTraceBranchInfo()) {
            std::unique_ptr<PCStateBase> new_pc(inst->pcState().clone());
            new_pc->as<RiscvISA::PCState>().npc(inst->traceBranchNextPC());
            inst->pcState(*new_pc);
        }

        if (inst->mispredicted() && !inst->isNonSpeculative()) {
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

void
IEW::stlfFailLdReplay(const DynInstPtr &inst, const InstSeqNum &store_seq_num)
{
    instQueue.stlfFailLdReplay(inst, store_seq_num);
}

void
IEW::mdpAddrReplayRegister(const DynInstPtr &inst,
                           const std::vector<InstSeqNum> &store_seq_nums)
{
    instQueue.mdpAddrReplayRegister(inst, store_seq_nums);
}

void
IEW::mdpAddrReplayRegisterStrict(const DynInstPtr &inst,
                                 size_t required_store_completed_idx)
{
    instQueue.mdpAddrReplayRegisterStrict(inst, required_store_completed_idx);
}

void
IEW::mdpAddrReplayPipeDone(const DynInstPtr &inst)
{
    instQueue.mdpAddrReplayPipeDone(inst);
}

void
IEW::mdpAddrReplayUpdateStoreCompletedIdx(ThreadID tid,
                                          size_t store_completed_idx)
{
    instQueue.mdpAddrReplayUpdateStoreCompletedIdx(tid, store_completed_idx);
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

    if (inst->isIssued() && inst->translationStarted() && !inst->translationCompleted()) {
        return StallReason::DTlbStall;
    }

    bool inFlight = inst->isIssued() && inst->hasPendingCacheReq();
    bool lsuStall = inst->isIssued() && !inst->hasPendingCacheReq();
    //Level of the cache hierachy where this request was responded to
    //e.g. 0:in l1, 1:in l2
    int depth=-1;
    if (inFlight) {
        assert(inst->pendingCacheReq);
        depth = inst->pendingCacheReq->mainReq()->depth;
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
    } else if (inFlight && other_stall) {
        return StallReason::OtherMemStall;
    }

    if (lsuStall) {
        return inst->isLoad() ? StallReason::LoadL1Bound : StallReason::StoreL1Bound;
    } else {
        return StallReason::OtherMemStall;
    }
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
    if (inst->isMemRef() || inst->isReadBarrier() || inst->isWriteBarrier() || inst->isNonSpeculative()) {
        return MemDQ;
    }
    // FIX: fcvt_s_w (Int2Fp) reads INT register, needs INT read port -> should go to IntDQ
    if (inst->opClass() == Int2FpOp) {
        return IntDQ;
    }
    if (inst->isFloating() || inst->isVector()) {
        return FVDQ;
    }
    return IntDQ;
}

StallReason
IEW::checkDispatchStall(ThreadID tid, int dq_stall, const DynInstPtr &dispatch_inst, int disp_seq) {
    DynInstPtr head_inst = rob->readHeadInst(tid);
    if (head_inst == rob->dummyInst) {
        if (dq_stall != NumDQ) {
            // this call is from dispatch to classify the reason why an instr cannot be dispatched
            return dqTypeToReason(static_cast<DQType>(dq_stall));
        } else {  // this call is to tell rename the stall status
            return StallReason::NoStall;
        }
    }

    if (dq_stall != NumDQ && disp_seq >= 0) {
        // get dq head inst
        assert(dispQue[dq_stall].size());
        auto &dq_head = dispQue[dq_stall].front();
        bool ready = !scheduler->ready(dq_head, disp_seq);
        if (getInstDQType(dispatch_inst) == getInstDQType(dq_head) && !ready) {
            return dqTypeToReason(static_cast<DQType>(dq_stall));
        }

        if (dispatch_inst->isStore() && !ldstQueue.sqFull()) {
            // store cannot be dispatched while sq is not full
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isLoad() && !(ldstQueue.lqFull() || rob->isFull() || !ready)) {
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isAtCommit() && !(ldstQueue.lqFull() || ldstQueue.sqFull())) {
            return StallReason::MemDQBandwidth;
        }

        if ((dispatch_inst->isFloating() || dispatch_inst->isVector()) &&
            !(rob->isFull() || !ready)) {
            return StallReason::FVDQBandwidth;
        }

        if (dispatch_inst->isInteger() && !(rob->isFull() || !ready)) {
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
            if (ldstQueue.lqEmpty(tid)) {
                return StallReason::InstNotReady;
            }
            return checkLSQStall(tid, true);
        } else if ((head_inst->isStore() || head_inst->isAtomic()) &&
                   ldstQueue.sqFull(tid)) {
            if (ldstQueue.sqEmpty(tid)) {
                return StallReason::InstNotReady;
            }
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
}

StallReason
IEW::checkLSQStall(ThreadID tid, bool isLoad)
{
    if ((isLoad && ldstQueue.lqEmpty(tid)) ||
        (!isLoad && ldstQueue.sqEmpty(tid))) {
        return StallReason::InstNotReady;
    }

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
