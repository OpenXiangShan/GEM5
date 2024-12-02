/*
 * Copyright 2014 Google, Inc.
 * Copyright (c) 2010-2014, 2017, 2020 ARM Limited
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
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

#include "cpu/o3/commit.hh"

#include <algorithm>
#include <set>
#include <string>

#include "arch/riscv/decoder.hh"
#include "arch/riscv/faults.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/compiler.hh"
#include "base/loader/symtab.hh"
#include "base/logging.hh"
#include "base/output.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/exetrace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/thread_state.hh"
#include "cpu/timebuf.hh"
#include "debug/Activity.hh"
#include "debug/Commit.hh"
#include "debug/CommitRate.hh"
#include "debug/CommitTrace.hh"
#include "debug/Counters.hh"
#include "debug/Diff.hh"
#include "debug/Drain.hh"
#include "debug/ExecFaulting.hh"
#include "debug/FTBStats.hh"
#include "debug/Faults.hh"
#include "debug/HtmCpu.hh"
#include "debug/InstCommited.hh"
#include "debug/O3PipeView.hh"
#include "params/BaseO3CPU.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"
#include "sim/faults.hh"
#include "sim/full_system.hh"

namespace gem5
{

namespace o3
{

void
Commit::processTrapEvent(ThreadID tid)
{
    // This will get reset by commit if it was switched out at the
    // time of this event processing.
    trapSquash[tid] = true;
    // update priv mode
    toIEW->commitInfo[0].clearInterrupt = true;
}

Commit::Commit(CPU *_cpu, branch_prediction::BPredUnit *_bp, const BaseO3CPUParams &params)
    : commitPolicy(params.smtCommitPolicy),
      cpu(_cpu),
      bp(_bp),
      iewToCommitDelay(params.iewToCommitDelay),
      commitToIEWDelay(params.commitToIEWDelay),
      renameToROBDelay(params.renameToROBDelay),
      fetchToCommitDelay(params.commitToFetchDelay),
      renameWidth(params.renameWidth),
      commitWidth(params.commitWidth),
      numThreads(params.numThreads),
      drainPending(false),
      drainImminent(false),
      trapLatency(params.trapLatency),
      canHandleInterrupts(true),
      avoidQuiesceLiveLock(false),
      stats(_cpu, this),
      archDBer(params.arch_db)
{
    if (commitWidth > MaxWidth)
        fatal("commitWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             commitWidth, static_cast<int>(MaxWidth));

    _status = Active;
    _nextStatus = Inactive;

    if (commitPolicy == CommitPolicy::RoundRobin) {
        //Set-Up Priority List
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            priority_list.push_back(tid);
        }
    }

    for (ThreadID tid = 0; tid < MaxThreads; tid++) {
        commitStatus[tid] = Idle;
        changedROBNumEntries[tid] = false;
        trapSquash[tid] = false;
        tcSquash[tid] = false;
        squashAfterInst[tid] = nullptr;
        pc[tid].reset(params.isa[0]->newPCState());
        youngestSeqNum[tid] = 0;
        lastCommitedSeqNum[tid] = 0;
        trapInFlight[tid] = false;
        committedStores[tid] = false;
        checkEmptyROB[tid] = false;
        renameMap[tid] = nullptr;
        htmStarts[tid] = 0;
        htmStops[tid] = 0;
    }
    interrupt = NoFault;

    registerExitCallback([this]() {
        auto out_handle = simout.create("misPredIndirect.txt", false, true);
        std::vector<std::pair<Addr, unsigned>> tempVec;
        for (auto &it : misPredIndirect) {
            tempVec.push_back(std::make_pair(it.first, it.second));
        }
        std::sort(tempVec.begin(), tempVec.end(),
            [](const std::pair<Addr, unsigned> &a,
               const std::pair<Addr, unsigned> &b) {
                return a.second > b.second;
            });
        for (auto it : tempVec) {
            *out_handle->stream() << std::oct << it.second << " " << std::hex << it.first << std::endl;
        }
        simout.close(out_handle);
    });

    faultNum.insert(RiscvISA::ExceptionCode::LOAD_PAGE);
    faultNum.insert(RiscvISA::ExceptionCode::STORE_PAGE);
    faultNum.insert(RiscvISA::ExceptionCode::INST_PAGE);
    faultNum.insert(RiscvISA::ExceptionCode::AMO_PAGE);
    faultNum.insert(RiscvISA::ExceptionCode::INST_G_PAGE);
    faultNum.insert(RiscvISA::ExceptionCode::LOAD_G_PAGE);
    faultNum.insert(RiscvISA::ExceptionCode::STORE_G_PAGE);

}

std::string Commit::name() const { return cpu->name() + ".commit"; }

void
Commit::regProbePoints()
{
    ppCommit = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Commit");
    ppCommitStall = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "CommitStall");
    ppSquash = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Squash");
}

Commit::CommitStats::CommitStats(CPU *cpu, Commit *commit)
    : statistics::Group(cpu, "commit"),
      ADD_STAT(commitSquashedInsts, statistics::units::Count::get(),
               "The number of squashed insts skipped by commit"),
      ADD_STAT(commitNonSpecStalls, statistics::units::Count::get(),
               "The number of times commit has been forced to stall to "
               "communicate backwards"),
      ADD_STAT(branchMispredicts, statistics::units::Count::get(),
               "The number of times a branch was mispredicted"),
      ADD_STAT(numCommittedDist, statistics::units::Count::get(),
               "Number of insts commited each cycle"),
      ADD_STAT(instsCommitted, statistics::units::Count::get(),
               "Number of instructions committed"),
      ADD_STAT(pagefaulttimes, statistics::units::Count::get(),
               "Number of pagefaulttimes"),
      ADD_STAT(opsCommitted, statistics::units::Count::get(),
               "Number of ops (including micro ops) committed"),
      ADD_STAT(memRefs, statistics::units::Count::get(),
               "Number of memory references committed"),
      ADD_STAT(loads, statistics::units::Count::get(), "Number of loads committed"),
      ADD_STAT(amos, statistics::units::Count::get(),
               "Number of atomic instructions committed"),
      ADD_STAT(membars, statistics::units::Count::get(),
               "Number of memory barriers committed"),
      ADD_STAT(branches, statistics::units::Count::get(),
               "Number of branches committed"),
      ADD_STAT(vectorInstructions, statistics::units::Count::get(),
               "Number of committed Vector instructions."),
      ADD_STAT(floating, statistics::units::Count::get(),
               "Number of committed floating point instructions."),
      ADD_STAT(integer, statistics::units::Count::get(),
               "Number of committed integer instructions."),
      ADD_STAT(functionCalls, statistics::units::Count::get(),
               "Number of function calls committed."),
      ADD_STAT(committedInstType, statistics::units::Count::get(),
               "Class of committed instruction"),
      ADD_STAT(commitEligibleSamples, statistics::units::Cycle::get(),
               "number cycles where commit BW limit reached"),
      ADD_STAT(segUnitStrideNF, statistics::units::Count::get(),
               "Distribution of segment unit stride NF"),
      ADD_STAT(segStrideNF, statistics::units::Count::get(),
               "Distribution of segment stride NF"),
      ADD_STAT(segIndexedNF, statistics::units::Count::get(),
               "Distribution of segment indexed NF"),
      ADD_STAT(vectorVma, statistics::units::Count::get(),
               "Number of vector vma enable"),
      ADD_STAT(vectorVmu, statistics::units::Count::get(),
               "Number of vector vmu enable"),
      ADD_STAT(vectorVta, statistics::units::Count::get(),
               "Number of vector vta enable"),
      ADD_STAT(vectorVtu, statistics::units::Count::get(),
               "Number of vector vtu enable")
{
    using namespace statistics;

    commitSquashedInsts.prereq(commitSquashedInsts);
    commitNonSpecStalls.prereq(commitNonSpecStalls);
    branchMispredicts.prereq(branchMispredicts);

    numCommittedDist
        .init(0,commit->commitWidth,1)
        .flags(statistics::pdf);

    segUnitStrideNF
        .init(0, 8, 1)
        .flags(statistics::pdf);

    segStrideNF
        .init(0, 8, 1)
        .flags(statistics::pdf);

    segIndexedNF
        .init(0, 8, 1)
        .flags(statistics::pdf);

    instsCommitted
        .init(cpu->numThreads)
        .flags(total);

    pagefaulttimes
        .init(cpu->numThreads)
        .flags(total);

    opsCommitted
        .init(cpu->numThreads)
        .flags(total);

    memRefs
        .init(cpu->numThreads)
        .flags(total);

    loads
        .init(cpu->numThreads)
        .flags(total);

    amos
        .init(cpu->numThreads)
        .flags(total);

    membars
        .init(cpu->numThreads)
        .flags(total);

    branches
        .init(cpu->numThreads)
        .flags(total);

    vectorInstructions
        .init(cpu->numThreads)
        .flags(total);

    floating
        .init(cpu->numThreads)
        .flags(total);

    integer
        .init(cpu->numThreads)
        .flags(total);

    functionCalls
        .init(commit->numThreads)
        .flags(total);

    committedInstType
        .init(commit->numThreads,enums::Num_OpClass)
        .flags(total | pdf | dist);

    committedInstType.ysubnames(enums::OpClassStrings);
}

void
Commit::setThreads(std::vector<ThreadState *> &threads)
{
    thread = threads;
}

void
Commit::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to send information back to IEW.
    toIEW = timeBuffer->getWire(0);

    // Setup wire to read data from IEW (for the ROB).
    robInfoFromIEW = timeBuffer->getWire(-iewToCommitDelay);
}

void
Commit::setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)
{
    fetchQueue = fq_ptr;

    // Setup wire to get instructions from rename (for the ROB).
    fromFetch = fetchQueue->getWire(-fetchToCommitDelay);
}

void
Commit::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to get instructions from rename (for the ROB).
    fromRename = renameQueue->getWire(-renameToROBDelay);
}

void
Commit::setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)
{
    iewQueue = iq_ptr;

    // Setup wire to get instructions from IEW.
    fromIEW = iewQueue->getWire(-iewToCommitDelay);
}

void
Commit::setIEWStage(IEW *iew_stage)
{
    iewStage = iew_stage;
}

void
Commit::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Commit::setRenameMap(UnifiedRenameMap rm_ptr[MaxThreads])
{
    for (ThreadID tid = 0; tid < numThreads; tid++)
        renameMap[tid] = &rm_ptr[tid];
}

void Commit::setROB(ROB *rob_ptr) { rob = rob_ptr; }

void
Commit::startupStage()
{
    rob->setActiveThreads(activeThreads);
    rob->resetEntries();

    // Broadcast the number of free entries.
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        toIEW->commitInfo[tid].usedROB = true;
        toIEW->commitInfo[tid].freeROBEntries = rob->numFreeEntries(tid);
        toIEW->commitInfo[tid].emptyROB = true;
    }

    // Commit must broadcast the number of free entries it has at the
    // start of the simulation, so it starts as active.
    cpu->activateStage(CPU::CommitIdx);

    cpu->activityThisCycle();
}

void
Commit::clearStates(ThreadID tid)
{
    commitStatus[tid] = Idle;
    changedROBNumEntries[tid] = false;
    checkEmptyROB[tid] = false;
    trapInFlight[tid] = false;
    committedStores[tid] = false;
    trapSquash[tid] = false;
    tcSquash[tid] = false;
    pc[tid].reset(cpu->tcBase(tid)->getIsaPtr()->newPCState());
    lastCommitedSeqNum[tid] = 0;
    squashAfterInst[tid] = NULL;
}

void Commit::drain() { drainPending = true; }

void
Commit::drainResume()
{
    drainPending = false;
    drainImminent = false;
}

void
Commit::drainSanityCheck() const
{
    assert(isDrained());
    rob->drainSanityCheck();

    // hardware transactional memory
    // cannot drain partially through a transaction
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (executingHtmTransaction(tid)) {
            panic("cannot drain partially through a HTM transaction");
        }
    }
}

bool
Commit::isDrained() const
{
    /* Make sure no one is executing microcode. There are two reasons
     * for this:
     * - Hardware virtualized CPUs can't switch into the middle of a
     *   microcode sequence.
     * - The current fetch implementation will most likely get very
     *   confused if it tries to start fetching an instruction that
     *   is executing in the middle of a ucode sequence that changes
     *   address mappings. This can happen on for example x86.
     */
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (pc[tid]->microPC() != 0)
            return false;
    }

    /* Make sure that all instructions have finished committing before
     * declaring the system as drained. We want the pipeline to be
     * completely empty when we declare the CPU to be drained. This
     * makes debugging easier since CPU handover and restoring from a
     * checkpoint with a different CPU should have the same timing.
     */
    return rob->isEmpty() &&
        interrupt == NoFault;
}

void
Commit::takeOverFrom()
{
    _status = Active;
    _nextStatus = Inactive;
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        commitStatus[tid] = Idle;
        changedROBNumEntries[tid] = false;
        trapSquash[tid] = false;
        tcSquash[tid] = false;
        squashAfterInst[tid] = NULL;
    }
    rob->takeOverFrom();
}

void
Commit::deactivateThread(ThreadID tid)
{
    std::list<ThreadID>::iterator thread_it = std::find(priority_list.begin(),
            priority_list.end(), tid);

    if (thread_it != priority_list.end()) {
        priority_list.erase(thread_it);
    }
}

bool
Commit::executingHtmTransaction(ThreadID tid) const
{
    if (tid == InvalidThreadID)
        return false;
    else
        return (htmStarts[tid] > htmStops[tid]);
}

void
Commit::resetHtmStartsStops(ThreadID tid)
{
    if (tid != InvalidThreadID)
    {
        htmStarts[tid] = 0;
        htmStops[tid] = 0;
    }
}


void
Commit::updateStatus()
{
    // reset ROB changed variable
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        changedROBNumEntries[tid] = false;

        // Also check if any of the threads has a trap pending
        if (commitStatus[tid] == TrapPending ||
            commitStatus[tid] == FetchTrapPending) {
            _nextStatus = Active;
        }
    }

    if (_nextStatus == Inactive && _status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");
        cpu->deactivateStage(CPU::CommitIdx);
    } else if (_nextStatus == Active && _status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");
        cpu->activateStage(CPU::CommitIdx);
    }

    _status = _nextStatus;
}

bool
Commit::changedROBEntries()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (changedROBNumEntries[tid]) {
            return true;
        }
    }

    return false;
}

size_t
Commit::numROBFreeEntries(ThreadID tid)
{
    return rob->numFreeEntries(tid);
}

void
Commit::generateTrapEvent(ThreadID tid, Fault inst_fault)
{
    DPRINTF(Commit, "Generating trap event for [tid:%i]\n", tid);

    EventFunctionWrapper *trap = new EventFunctionWrapper(
        [this, tid]{ processTrapEvent(tid); },
        "Trap", true, Event::CPU_Tick_Pri);

    Cycles latency = std::dynamic_pointer_cast<SyscallRetryFault>(inst_fault) ?
                     cpu->syscallRetryLatency : trapLatency;

    // hardware transactional memory
    if (inst_fault != nullptr &&
        std::dynamic_pointer_cast<GenericHtmFailureFault>(inst_fault)) {
        // TODO
        // latency = default abort/restore latency
        // could also do some kind of exponential back off if desired
    }

    cpu->schedule(trap, cpu->clockEdge(latency));
    trapInFlight[tid] = true;
    thread[tid]->trapPending = true;
}

void
Commit::generateTCEvent(ThreadID tid)
{
    assert(!trapInFlight[tid]);
    DPRINTF(Commit, "Generating TC squash event for [tid:%i]\n", tid);

    tcSquash[tid] = true;
}

void
Commit::squashAll(ThreadID tid)
{
    // If we want to include the squashing instruction in the squash,
    // then use one older sequence number.
    // Hopefully this doesn't mess things up.  Basically I want to squash
    // all instructions of this thread.
    InstSeqNum squashed_inst = rob->isEmpty(tid) ?
        lastCommitedSeqNum[tid] : rob->readHeadInst(tid)->seqNum - 1;

    // All younger instructions will be squashed. Set the sequence
    // number as the youngest instruction in the ROB (0 in this case.
    // Hopefully nothing breaks.)
    youngestSeqNum[tid] = lastCommitedSeqNum[tid];

    rob->squash(squashed_inst, tid);
    changedROBNumEntries[tid] = true;

    // Send back the sequence number of the squashed instruction.
    toIEW->commitInfo[tid].doneSeqNum = squashed_inst;

    // Send back the squash signal to tell stages that they should
    // squash.
    toIEW->commitInfo[tid].squash = true;

    // Send back the rob squashing signal so other stages know that
    // the ROB is in the process of squashing.
    toIEW->commitInfo[tid].robSquashing = true;

    toIEW->commitInfo[tid].mispredictInst = NULL;
    toIEW->commitInfo[tid].squashInst = NULL;

    set(toIEW->commitInfo[tid].pc, pc[tid]);

    toIEW->commitInfo[tid].squashedStreamId = committedStreamId;
    toIEW->commitInfo[tid].squashedTargetId = committedTargetId;
    toIEW->commitInfo[tid].squashedLoopIter = committedLoopIter;

    cpu->mmu->useNewPriv(cpu->getContext(tid));

    squashInflightAndUpdateVersion(tid);
}

void
Commit::squashFromTrap(ThreadID tid)
{
    squashAll(tid);

    toIEW->commitInfo[tid].isTrapSquash = true;
    toIEW->commitInfo[tid].committedPC = committedPC[tid];

    DPRINTF(Commit, "Squashing from trap, restarting at PC %s\n", *pc[tid]);

    thread[tid]->trapPending = false;
    thread[tid]->noSquashFromTC = false;
    trapInFlight[tid] = false;

    trapSquash[tid] = false;

    commitStatus[tid] = ROBSquashing;
    cpu->activityThisCycle();
}

void
Commit::squashFromTC(ThreadID tid)
{
    squashAll(tid);

    DPRINTF(Commit, "Squashing from TC, restarting at PC %s\n", *pc[tid]);

    thread[tid]->noSquashFromTC = false;
    assert(!thread[tid]->trapPending);

    commitStatus[tid] = ROBSquashing;
    cpu->activityThisCycle();

    tcSquash[tid] = false;
}

void
Commit::squashFromSquashAfter(ThreadID tid)
{
    DPRINTF(Commit, "Squashing after squash after request, "
            "restarting at PC %s\n", *pc[tid]);

    squashAll(tid);
    // Make sure to inform the fetch stage of which instruction caused
    // the squash. It'll try to re-fetch an instruction executing in
    // microcode unless this is set.
    toIEW->commitInfo[tid].squashInst = squashAfterInst[tid];
    squashAfterInst[tid] = NULL;

    commitStatus[tid] = ROBSquashing;
    cpu->activityThisCycle();
}

void
Commit::squashAfter(ThreadID tid, const DynInstPtr &head_inst)
{
    DPRINTF(Commit, "Executing squash after for [tid:%i] inst [sn:%llu]\n",
            tid, head_inst->seqNum);

    assert(!squashAfterInst[tid] || squashAfterInst[tid] == head_inst);
    commitStatus[tid] = SquashAfterPending;
    squashAfterInst[tid] = head_inst;
}

void
Commit::tick()
{
    wroteToTimeBuffer = false;
    _nextStatus = Inactive;

    if (activeThreads->empty())
        return;

    if (cpu->curCycle() - lastCommitCycle > 20000) {
        if (maybeStucked) {
            panic("cpu stucked!!\n");
        }
        warn("cpu may be stucked\n");
        maybeStucked = true;
    } else {
        maybeStucked = false;
    }

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // Check if any of the threads are done squashing.  Change the
    // status if they are done.
    while (threads != end) {
        ThreadID tid = *threads++;

        // Clear the bit saying if the thread has committed stores
        // this cycle.
        committedStores[tid] = false;

        if (commitStatus[tid] == ROBSquashing) {

            if (rob->isDoneSquashing(tid)) {
                commitStatus[tid] = Running;
                DPRINTF(Commit,"[tid:%i] Switch from ROB squashing to Running.\n", tid);
            } else {
                DPRINTF(Commit,"[tid:%i] Still Squashing, cannot commit any"
                        " insts this cycle.\n", tid);
                rob->doSquash(tid);
                changedROBNumEntries[tid] = true;
                toIEW->commitInfo[tid].robSquashing = true;
                wroteToTimeBuffer = true;
            }
        }
    }

    commit();

    markCompletedInsts();

    threads = activeThreads->begin();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!rob->isEmpty(tid) && rob->readHeadInst(tid)->readyToCommit()) {
            // The ROB has more instructions it can commit. Its next status
            // will be active.
            _nextStatus = Active;

            [[maybe_unused]] const DynInstPtr &inst = rob->readHeadInst(tid);

            DPRINTF(Commit,"[tid:%i] Instruction [sn:%llu] PC %s is head of"
                    " ROB and ready to commit\n",
                    tid, inst->seqNum, inst->pcState());

        } else if (!rob->isEmpty(tid)) {
            const DynInstPtr &inst = rob->readHeadInst(tid);

            ppCommitStall->notify(inst);

            DPRINTF(Commit,"[tid:%i] Can't commit, Instruction [sn:%llu] PC "
                    "%s is head of ROB and not ready\n",
                    tid, inst->seqNum, inst->pcState());
            DPRINTF(Counters, "instruction ready tick:%lu\n", inst->readyTick);
            DPRINTF(Commit, "%s\n", inst->staticInst->disassemble(inst->pcState().instAddr()).c_str());
        }

        DPRINTF(Commit, "[tid:%i] ROB has %d insts & %d free entries.\n",
                tid, rob->countInsts(tid), rob->numFreeEntries(tid));
    }


    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity This Cycle.\n");
        cpu->activityThisCycle();
    }

    updateStatus();
}

void
Commit::handleInterrupt()
{
    // Verify that we still have an interrupt to handle
    if (!cpu->checkInterrupts(0)) {
        DPRINTF(Commit, "Pending interrupt is cleared by requestor before "
                "it got handled. Restart fetching from the orig path.\n");
        toIEW->commitInfo[0].clearInterrupt = true;
        interrupt = NoFault;
        avoidQuiesceLiveLock = true;
        return;
    }

    // Wait until all in flight instructions are finished before enterring
    // the interrupt.
    if (canHandleInterrupts && cpu->instList.empty()) {
        // Squash or record that I need to squash this cycle if
        // an interrupt needed to be handled.
        DPRINTF(Commit, "Interrupt detected.\n");

        // Clear the interrupt now that it's going to be handled
        // toIEW->commitInfo[0].clearInterrupt = true;

        assert(!thread[0]->noSquashFromTC);
        thread[0]->noSquashFromTC = true;

        if (cpu->checker) {
            cpu->checker->handlePendingInt();
        }

        // CPU will handle interrupt. Note that we ignore the local copy of
        // interrupt. This is because the local copy may no longer be the
        // interrupt that the interrupt controller thinks is being handled.
        if (cpu->difftestEnabled()) {
            cpu->difftestRaiseIntr(cpu->getInterruptsNO() | (1ULL << 63));
        }

        DPRINTF(CommitTrace, "Handle interrupt No.%lx\n", cpu->getInterruptsNO() | (1ULL << 63));
        cpu->processInterrupts(cpu->getInterrupts());

        cpu->mmu->setOldPriv(cpu->getContext(0));

        thread[0]->noSquashFromTC = false;

        commitStatus[0] = TrapPending;

        interrupt = NoFault;

        // Generate trap squash event.
        generateTrapEvent(0, interrupt);

        avoidQuiesceLiveLock = false;
    } else {
        DPRINTF(Commit, "Interrupt pending: instruction is %sin "
                "flight, ROB is %sempty\n",
                canHandleInterrupts ? "not " : "",
                cpu->instList.empty() ? "" : "not " );
    }
}

void
Commit::propagateInterrupt()
{
    // Don't propagate intterupts if we are currently handling a trap or
    // in draining and the last observable instruction has been committed.
    if (commitStatus[0] == TrapPending || interrupt || trapSquash[0] ||
            tcSquash[0] || drainImminent)
        return;

    // Process interrupts if interrupts are enabled, not in PAL
    // mode, and no other traps or external squashes are currently
    // pending.
    // @todo: Allow other threads to handle interrupts.

    // Get any interrupt that happened
    interrupt = cpu->getInterrupts();

    // Tell fetch that there is an interrupt pending.  This
    // will make fetch wait until it sees a non PAL-mode PC,
    // at which point it stops fetching instructions.
    if (interrupt != NoFault)
        toIEW->commitInfo[0].interruptPending = true;
}

void
Commit::commit()
{
    if (FullSystem) {
        // Check if we have a interrupt and get read to handle it
        if (cpu->checkInterrupts(0))
            propagateInterrupt();
    }

    ////////////////////////////////////
    // Check for any possible squashes, handle them first
    ////////////////////////////////////
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    int num_squashing_threads = 0;

    while (threads != end) {
        ThreadID tid = *threads++;

        // Not sure which one takes priority.  I think if we have
        // both, that's a bad sign.
        if (trapSquash[tid]) {
            assert(!tcSquash[tid]);
            squashFromTrap(tid);

            // If the thread is trying to exit (i.e., an exit syscall was
            // executed), this trapSquash was originated by the exit
            // syscall earlier. In this case, schedule an exit event in
            // the next cycle to fully terminate this thread
            if (cpu->isThreadExiting(tid))
                cpu->scheduleThreadExitEvent(tid);
        } else if (tcSquash[tid]) {
            assert(commitStatus[tid] != TrapPending);
            squashFromTC(tid);
        } else if (commitStatus[tid] == SquashAfterPending) {
            // A squash from the previous cycle of the commit stage (i.e.,
            // commitInsts() called squashAfter) is pending. Squash the
            // thread now.
            squashFromSquashAfter(tid);
        }

        // Squashed sequence number must be older than youngest valid
        // instruction in the ROB. This prevents squashes from younger
        // instructions overriding squashes from older instructions.
        if (fromIEW->squash[tid] &&
            commitStatus[tid] != TrapPending &&
            fromIEW->squashedSeqNum[tid] <= youngestSeqNum[tid]) {

            if (fromIEW->mispredictInst[tid]) {
                DPRINTF(Commit,
                    "[tid:%i] Squashing due to branch mispred "
                    "PC:%#x [sn:%llu]\n",
                    tid,
                    fromIEW->mispredictInst[tid]->pcState().instAddr(),
                    fromIEW->squashedSeqNum[tid]);
            } else {
                DPRINTF(Commit,
                    "[tid:%i] Squashing due to order violation [sn:%llu]\n",
                    tid, fromIEW->squashedSeqNum[tid]);
            }

            DPRINTF(Commit, "[tid:%i] Redirecting to PC %#x\n",
                    tid, *fromIEW->pc[tid]);

            commitStatus[tid] = ROBSquashing;

            // If we want to include the squashing instruction in the squash,
            // then use one older sequence number.
            InstSeqNum squashed_inst = fromIEW->squashedSeqNum[tid];

            if (fromIEW->includeSquashInst[tid]) {
                squashed_inst--;
            }

            // All younger instructions will be squashed. Set the sequence
            // number as the youngest instruction in the ROB.
            youngestSeqNum[tid] = squashed_inst;

            rob->squash(squashed_inst, tid);
            changedROBNumEntries[tid] = true;

            toIEW->commitInfo[tid].doneSeqNum = squashed_inst;

            toIEW->commitInfo[tid].squash = true;

            // Send back the rob squashing signal so other stages know that
            // the ROB is in the process of squashing.
            toIEW->commitInfo[tid].robSquashing = true;

            toIEW->commitInfo[tid].mispredictInst =
                fromIEW->mispredictInst[tid];
            toIEW->commitInfo[tid].branchTaken =
                fromIEW->branchTaken[tid];

            auto squashed_inst_ptr = rob->findInst(tid, squashed_inst);
            toIEW->commitInfo[tid].squashInst = squashed_inst_ptr;
            if (!squashed_inst_ptr) {
                DPRINTF(Commit,
                        "Unable to find squashed instruction in ROB\n");
            }
            toIEW->commitInfo[tid].squashedStreamId = fromIEW->squashedStreamId[tid];
            toIEW->commitInfo[tid].squashedTargetId = fromIEW->squashedTargetId[tid];
            toIEW->commitInfo[tid].squashedLoopIter = fromIEW->squashedLoopIter[tid];

            // toIEW->commitInfo[tid].doneFsqId =
            //                         toIEW->commitInfo[tid].squashInst->getFsqId();
            if (toIEW->commitInfo[tid].mispredictInst) {
                if (toIEW->commitInfo[tid].mispredictInst->isUncondCtrl()) {
                     toIEW->commitInfo[tid].branchTaken = true;
                }
                ++stats.branchMispredicts;
            }

            set(toIEW->commitInfo[tid].pc, fromIEW->pc[tid]);
            squashInflightAndUpdateVersion(tid);
        }

        if (commitStatus[tid] == ROBSquashing) {
            num_squashing_threads++;
        }
    }

    // If commit is currently squashing, then it will have activity for the
    // next cycle. Set its next status as active.
    if (num_squashing_threads) {
        _nextStatus = Active;
    }

    if (num_squashing_threads != numThreads) {
        // If we're not currently squashing, then get instructions.
        getInsts();

        // Try to commit any instructions.
        commitInsts();
    }

    //Check for any activity
    threads = activeThreads->begin();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (changedROBNumEntries[tid]) {
            toIEW->commitInfo[tid].usedROB = true;
            toIEW->commitInfo[tid].freeROBEntries = rob->numFreeEntries(tid);

            wroteToTimeBuffer = true;
            changedROBNumEntries[tid] = false;
            if (rob->isEmpty(tid))
                checkEmptyROB[tid] = true;
        }

        // ROB is only considered "empty" for previous stages if: a)
        // ROB is empty, b) there are no outstanding stores, c) IEW
        // stage has received any information regarding stores that
        // committed.
        // c) is checked by making sure to not consider the ROB empty
        // on the same cycle as when stores have been committed.
        // d) no squashAfter is pending
        // @todo: Make this handle multi-cycle communication between
        // commit and IEW.
        if (checkEmptyROB[tid] && rob->isEmpty(tid) &&
            !iewStage->hasStoresToWB(tid) && !committedStores[tid] && commitStatus[tid] != SquashAfterPending) {
            checkEmptyROB[tid] = false;
            toIEW->commitInfo[tid].usedROB = true;
            toIEW->commitInfo[tid].emptyROB = true;
            toIEW->commitInfo[tid].freeROBEntries = rob->numFreeEntries(tid);
            wroteToTimeBuffer = true;
        }

    }
}
void
Commit::updateMstatusSd(ThreadID tid){
    RiscvISA::STATUS mstatus = cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, tid);
    mstatus.sd = (mstatus.fs == 3) || (mstatus.vs == 3);
    cpu->setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, (RegVal)mstatus, tid);
}

void
Commit::commitInsts()
{
    ////////////////////////////////////
    // Handle commit
    // Note that commit will be handled prior to putting new
    // instructions in the ROB so that the ROB only tries to commit
    // instructions it has in this current cycle, and not instructions
    // it is writing in during this cycle.  Can't commit and squash
    // things at the same time...
    ////////////////////////////////////

    DPRINTF(Commit, "Trying to commit instructions in the ROB.\n");

    unsigned num_committed = 0;

    DynInstPtr head_inst;

    int commit_width = commitWidth;
    int count_ = 0;
    ThreadID tid = getCommittingThread();
    if (tid > -1) {
        for (auto& it : *(rob->getInstList(tid))) {
            count_++;
            if (it->opClass() == FMAAccOp) {
                commit_width++;
            }
            if (count_ >= commitWidth ||
                commit_width >= commitWidth * 2) {
                break;
            }
        }
    }

    // Commit as many instructions as possible until the commit bandwidth
    // limit is reached, or it becomes impossible to commit any more.
    while (num_committed < commit_width) {
        // hardware transactionally memory
        // If executing within a transaction,
        // need to handle interrupts specially

        ThreadID commit_thread = getCommittingThread();

        // Check for any interrupt that we've already squashed for
        // and start processing it.
        if (interrupt != NoFault) {
            // If inside a transaction, postpone interrupts
            if (executingHtmTransaction(commit_thread)) {
                cpu->clearInterrupts(0);
                toIEW->commitInfo[0].clearInterrupt = true;
                interrupt = NoFault;
                avoidQuiesceLiveLock = true;
            } else {
                handleInterrupt();
            }
        }

        // ThreadID commit_thread = getCommittingThread();

        if (commit_thread == -1 || !rob->isHeadReady(commit_thread))
            break;

        head_inst = rob->readHeadInst(commit_thread);

        ThreadID tid = head_inst->threadNumber;

        assert(tid == commit_thread);

        DPRINTF(Commit,
                "Trying to commit head instruction, [tid:%i] [sn:%llu]\n",
                tid, head_inst->seqNum);

        // If the head instruction is squashed, it is ready to retire
        // (be removed from the ROB) at any time.
        if (head_inst->isSquashed()) {

            DPRINTF(Commit, "Retiring squashed instruction from "
                    "ROB.\n");

            rob->retireHead(commit_thread);

            ++stats.commitSquashedInsts;
            // Notify potential listeners that this instruction is squashed
            ppSquash->notify(head_inst);

            // Record that the number of ROB entries has changed.
            changedROBNumEntries[tid] = true;
        } else {
            set(pc[tid], head_inst->pcState());

            // Try to commit the head instruction.
            bool commit_success = commitHead(head_inst, num_committed);

            if (commit_success) {
                cpu->perfCCT->updateInstPos(head_inst->seqNum, PerfRecord::AtCommit);
                cpu->perfCCT->commitMeta(head_inst->seqNum);
                lastCommitCycle = cpu->curCycle();
                head_inst->printDisassemblyAndResult(cpu->name());
                const auto &head_rv_pc = head_inst->pcState().as<RiscvISA::PCState>();
                if (bp->isStream()) {
                    auto dbsp = dynamic_cast<branch_prediction::stream_pred::DecoupledStreamBPU*>(bp);
                    Addr branchAddr = head_inst->pcState().instAddr();
                    Addr targetAddr = head_rv_pc.npc();
                    Addr fallThruPC = head_rv_pc.getFallThruPC();
                    if (targetAddr < branchAddr || dbsp->loopDetector->findLoop(branchAddr)) {
                        dbsp->loopDetector->update(branchAddr, targetAddr, fallThruPC);
                    }
                    if (targetAddr > branchAddr && head_inst->isControl()) {
                        dbsp->loopDetector->setRecentForwardTakenPC(branchAddr, targetAddr);
                    }
                }

                if (bp->isFTB()) {
                    auto dbftb = dynamic_cast<branch_prediction::ftb_pred::DecoupledBPUWithFTB*>(bp);
                    bool miss = head_inst->mispredicted();
                    if (head_inst->isReturn()) {
                        DPRINTF(FTBRAS, "commit inst PC %x miss %d real target %x pred target %x\n",
                                head_inst->pcState().instAddr(), miss,
                                head_rv_pc.npc(), *(head_inst->predPC));
                    }

                    // FIXME: ignore mret/sret/uret in correspond with RTL
                    if (!head_inst->isNonSpeculative() && head_inst->isControl()) {
                        dbftb->commitBranch(head_inst, miss);
                        if (!head_inst->isReturn() && head_inst->isIndirectCtrl() && miss) {
                            misPredIndirect[head_inst->pcState().instAddr()]++;
                        }
                    }
                    dbftb->notifyInstCommit(head_inst);
                }
                if (head_inst->isUpdateVsstatusSd()) {
                    auto v = cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VIRMODE, tid);
                    RiscvISA::HSTATUS hstatus = cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HSTATUS, tid);
                    RiscvISA::VSSTATUS vsstatus =
                        cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, tid);
                    RiscvISA::VSSTATUS32 vsstatus32 =
                        cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, tid);

                    if (v) {
                        if (hstatus.vsxl ==1) {
                            vsstatus32.sd = (vsstatus32.fs == 3);
                            cpu->setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, (RegVal)vsstatus32, tid);
                        } else {
                            vsstatus.sd = (vsstatus.fs == 3);
                            cpu->setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, (RegVal)vsstatus, tid);
                        }
                    }

                }
                if (head_inst->isUpdateMstatusSd()) {
                    updateMstatusSd(tid);
                }

                ++num_committed;
                stats.committedInstType[tid][head_inst->opClass()]++;
                ppCommit->notify(head_inst);

                // hardware transactional memory

                // update nesting depth
                if (head_inst->isHtmStart())
                    htmStarts[tid]++;

                // sanity check
                if (head_inst->inHtmTransactionalState()) {
                    assert(executingHtmTransaction(tid));
                } else {
                    assert(!executingHtmTransaction(tid));
                }

                // update nesting depth
                if (head_inst->isHtmStop())
                    htmStops[tid]++;

                changedROBNumEntries[tid] = true;

                // Set the doneSeqNum to the youngest committed instruction.
                toIEW->commitInfo[tid].doneSeqNum = head_inst->seqNum;

                if (head_inst->getFsqId() > 1) {
                    toIEW->commitInfo[tid].doneFsqId =
                        head_inst->getFsqId() - 1;
                }
                committedStreamId = head_inst->getFsqId();
                committedTargetId = head_inst->getFtqId();
                committedLoopIter = head_inst->getLoopIteration();

                if (tid == 0)
                    canHandleInterrupts = !head_inst->isDelayedCommit();

                // at this point store conditionals should either have
                // been completed or predicated false
                assert(!head_inst->isStoreConditional() ||
                       head_inst->isCompleted() ||
                       !head_inst->readPredicate());

                // Updates misc. registers.
                head_inst->updateMiscRegs();
                if (head_inst->staticInst->isVectorConfig()) {
                    auto vset = static_cast<RiscvISA::VConfOp*>(head_inst->staticInst.get());
                    if (!(vset->vtypeIsImm)) {
                        auto tc = head_inst->tcBase();
                        RiscvISA::VTYPE new_vtype = head_inst->readMiscReg(RiscvISA::MISCREG_VTYPE);
                        tc->getDecoderPtr()->as<RiscvISA::Decoder>().setVtype(new_vtype);
                    }
                }
                if (head_inst->isFloating() && head_inst->isLoad()){
                    RiscvISA::STATUS status = cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, tid);
                    status.sd = 1;
                    status.fs = 3;
                    cpu->setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, (RegVal)status, tid);
                }
                if (head_inst->isUpdateVsstatusSd()) {
                    auto v = cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VIRMODE, tid);
                    RiscvISA::HSTATUS hstatus = cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HSTATUS, tid);
                    RiscvISA::VSSTATUS vsstatus =
                        cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, tid);
                    RiscvISA::VSSTATUS32 vsstatus32 =
                        cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, tid);

                    if (v) {
                        if (hstatus.vsxl ==1) {
                            vsstatus32.sd = (vsstatus32.fs == 3);
                            cpu->setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, (RegVal)vsstatus32, tid);
                        } else {
                            vsstatus.sd = (vsstatus.fs == 3);
                            cpu->setMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, (RegVal)vsstatus, tid);
                        }
                    }

                }

                if (cpu->difftestEnabled()) {
                    diffInst(tid, head_inst);
                }

                // Check instruction execution if it successfully commits and
                // is not carrying a fault.
                if (cpu->checker) {
                    cpu->checker->verify(head_inst);
                }

                cpu->traceFunctions(pc[tid]->instAddr());

                head_inst->staticInst->advancePC(*pc[tid]);

                // Keep track of the last sequence number commited
                lastCommitedSeqNum[tid] = head_inst->seqNum;

                // If this is an instruction that doesn't play nicely with
                // others squash everything and restart fetch
                if (head_inst->isSquashAfter())
                    squashAfter(tid, head_inst);

                if (drainPending) {
                    if (pc[tid]->microPC() == 0 && interrupt == NoFault &&
                        !thread[tid]->trapPending) {
                        // Last architectually committed instruction.
                        // Squash the pipeline, stall fetch, and use
                        // drainImminent to disable interrupts
                        DPRINTF(Drain, "Draining: %i:%s\n", tid, *pc[tid]);
                        squashAfter(tid, head_inst);
                        cpu->commitDrained(tid);
                        drainImminent = true;
                    }
                }

                bool onInstBoundary = !head_inst->isMicroop() ||
                                      head_inst->isLastMicroop() ||
                                      !head_inst->isDelayedCommit();

                if (onInstBoundary) {
                    int count = 0;
                    Addr oldpc;
                    // Make sure we're not currently updating state while
                    // handling PC events.
                    assert(!thread[tid]->noSquashFromTC &&
                           !thread[tid]->trapPending);
                    do {
                        oldpc = pc[tid]->instAddr();
                        thread[tid]->pcEventQueue.service(
                                oldpc, thread[tid]->getTC());
                        count++;
                    } while (oldpc != pc[tid]->instAddr());
                    if (count > 1) {
                        DPRINTF(Commit,
                                "PC skip function event, stopping commit\n");
                        break;
                    }
                }

                // Check if an instruction just enabled interrupts and we've
                // previously had an interrupt pending that was not handled
                // because interrupts were subsequently disabled before the
                // pipeline reached a place to handle the interrupt. In that
                // case squash now to make sure the interrupt is handled.
                //
                // If we don't do this, we might end up in a live lock
                // situation.
                if (!interrupt && avoidQuiesceLiveLock &&
                    onInstBoundary && cpu->checkInterrupts(0))
                    squashAfter(tid, head_inst);
            } else {
                DPRINTF(Commit, "Unable to commit head instruction PC:%s "
                        "[tid:%i] [sn:%llu].\n",
                        head_inst->pcState(), tid ,head_inst->seqNum);
                break;
            }
        }
    }

    DPRINTF(CommitRate, "%i\n", num_committed);
    stats.numCommittedDist.sample(num_committed);

    if (num_committed == commitWidth) {
        stats.commitEligibleSamples++;
    }
}



void
Commit::diffInst(ThreadID tid, const DynInstPtr &inst) {
    cpu->diffInfo.lastCommittedMsg.push(inst->genDisassembly());
    if (cpu->diffInfo.lastCommittedMsg.size() > 20) {
        cpu->diffInfo.lastCommittedMsg.pop();
    }
    cpu->diffInfo.inst = inst->staticInst;
    cpu->diffInfo.pc = &inst->pcState();
    for (int i = 0; i < inst->numDestRegs(); i++) {
        const auto &dest = inst->destRegIdx(i);
        if ((dest.isFloatReg() || dest.isIntReg()) && !dest.isZeroReg()) {
            cpu->diffInfo.scalarResults.at(i) = cpu->getArchReg(dest, tid);
        } else if (dest.isVecReg()) {
            assert(i == 0);
            cpu->getArchReg(dest, &(cpu->diffInfo.vecResult), tid);
        }
    }
    cpu->diffInfo.curInstStrictOrdered =
        inst->strictlyOrdered();
    cpu->diffInfo.physEffAddr = inst->physEffAddr;
    cpu->diffInfo.effSize = inst->effSize;
    cpu->diffInfo.goldenValue = inst->getGolden();
    cpu->difftestStep(tid, inst->seqNum);
}


bool
Commit::commitHead(const DynInstPtr &head_inst, unsigned inst_num)
{
    assert(head_inst);

    ThreadID tid = head_inst->threadNumber;

    // If the instruction is not executed yet, then it will need extra
    // handling.  Signal backwards that it should be executed.
    if (!head_inst->isExecuted()) {
        // Make sure we are only trying to commit un-executed instructions we
        // think are possible.
        assert(head_inst->isNonSpeculative() || head_inst->isStoreConditional()
               || head_inst->isReadBarrier() || head_inst->isWriteBarrier()
               || head_inst->isAtomic()
               || (head_inst->isLoad() && head_inst->strictlyOrdered()));

        DPRINTF(Commit,
                "Encountered a barrier or non-speculative "
                "instruction [tid:%i] [sn:%llu] "
                "at the head of the ROB, PC %s.\n",
                tid, head_inst->seqNum, head_inst->pcState());

        if (inst_num > 0 || !iewStage->flushAllStores(tid)) {
            DPRINTF(Commit,
                    "[tid:%i] [sn:%llu] "
                    "Waiting for all stores to writeback.\n",
                    tid, head_inst->seqNum);
            return false;
        }

        toIEW->commitInfo[tid].nonSpecSeqNum = head_inst->seqNum;

        // Change the instruction so it won't try to commit again until
        // it is executed.
        head_inst->clearCanCommit();

        if (head_inst->isLoad() && head_inst->strictlyOrdered()) {
            DPRINTF(Commit, "[tid:%i] [sn:%llu] "
                    "Strictly ordered load, PC %s.\n",
                    tid, head_inst->seqNum, head_inst->pcState());
            toIEW->commitInfo[tid].strictlyOrdered = true;
            toIEW->commitInfo[tid].strictlyOrderedLoad = head_inst;
        } else {
            ++stats.commitNonSpecStalls;
        }

        return false;
    }

    // Check if the instruction caused a fault.  If so, trap.
    Fault inst_fault = head_inst->getFault();

    // hardware transactional memory
    // if a fault occurred within a HTM transaction
    // ensure that the transaction aborts
    if (inst_fault != NoFault && head_inst->inHtmTransactionalState()) {
        // There exists a generic HTM fault common to all ISAs
        if (!std::dynamic_pointer_cast<GenericHtmFailureFault>(inst_fault)) {
            DPRINTF(HtmCpu, "%s - fault (%s) encountered within transaction"
                            " - converting to GenericHtmFailureFault\n",
            head_inst->staticInst->getName(), inst_fault->name());
            inst_fault = std::make_shared<GenericHtmFailureFault>(
                head_inst->getHtmTransactionUid(),
                HtmFailureFaultCause::EXCEPTION);
        }
        // If this point is reached and the fault inherits from the HTM fault,
        // then there is no need to raise a new fault
    }

    // Stores mark themselves as completed.
    if (!head_inst->isStore() && inst_fault == NoFault) {
        head_inst->setCompleted();
    }

    if (inst_fault != NoFault) {
        DPRINTF(CommitTrace, "[sn:%lu pc:%#lx] %s has a fault, mepc: %#lx, mcause: %#lx, mtval: %#lx\n",
                head_inst->seqNum, head_inst->pcState().instAddr(),
                head_inst->staticInst->disassemble(head_inst->pcState().instAddr()),
                cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_MEPC, tid),
                cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_MCAUSE, tid),
                cpu->readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_MTVAL, tid)
                );

        if (!iewStage->flushAllStores(tid) || inst_num > 0) {
            DPRINTF(Commit,
                    "[tid:%i] [sn:%llu] "
                    "Stores outstanding, fault must wait.\n",
                    tid, head_inst->seqNum);
            return false;
        }
        if (faultNum.find(inst_fault->exception()) != faultNum.end()) {
            stats.pagefaulttimes[tid]++;
        }

        head_inst->setCompleted();

        // If instruction has faulted, let the checker execute it and
        // check if it sees the same fault and control flow.
        if (cpu->checker) {
            // Need to check the instruction before its fault is processed
            cpu->checker->verify(head_inst);
        }

        assert(!thread[tid]->noSquashFromTC);

        // Mark that we're in state update mode so that the trap's
        // execution doesn't generate extra squashes.
        thread[tid]->noSquashFromTC = true;

        // Execute the trap.  Although it's slightly unrealistic in
        // terms of timing (as it doesn't wait for the full timing of
        // the trap event to complete before updating state), it's
        // needed to update the state as soon as possible.  This
        // prevents external agents from changing any specific state
        // that the trap need.
        cpu->trap(inst_fault, tid,
                  head_inst->notAnInst() ? nullStaticInstPtr :
                      head_inst->staticInst);

        cpu->mmu->setOldPriv(cpu->getContext(tid));

        // Exit state update mode to avoid accidental updating.
        thread[tid]->noSquashFromTC = false;

        commitStatus[tid] = TrapPending;

        DPRINTF(
            Commit,
            "[tid:%i] [sn:%llu] %s Committing instruction with fault %s\n",
            tid, head_inst->seqNum,
            head_inst->staticInst->disassemble(
                head_inst->pcState().instAddr()).c_str(), inst_fault->name());


        if (head_inst->traceData) {
            // We ignore ReExecution "faults" here as they are not real
            // (architectural) faults but signal flush/replays.
            if (debug::ExecFaulting
                && dynamic_cast<ReExec*>(inst_fault.get()) == nullptr) {

                head_inst->traceData->setFaulting(true);
                head_inst->traceData->setFetchSeq(head_inst->seqNum);
                head_inst->traceData->setCPSeq(thread[tid]->numOp);
                head_inst->traceData->dump();
            }
            delete head_inst->traceData;
            head_inst->traceData = NULL;
        }

        if (cpu->difftestEnabled() && inst_fault->isFromISA()) {
            auto priv = cpu->readMiscRegNoEffect(
                RiscvISA::MiscRegIndex::MISCREG_PRV, tid);
            RegVal cause = 0;
            if (priv == RiscvISA::PRV_M) {
                DPRINTF(Commit, "Force to raise exception at machine mode\n");
                cause = cpu->readMiscReg(
                    RiscvISA::MiscRegIndex::MISCREG_MCAUSE, tid);
            } else if (priv == RiscvISA::PRV_S) {
                DPRINTF(Commit, "Force to raise exception at supvs mode\n");
                cause = cpu->readMiscReg(
                    RiscvISA::MiscRegIndex::MISCREG_SCAUSE, tid);
            } else {
                DPRINTF(Commit, "Force to raise exception at user mode\n");
                assert(priv == RiscvISA::PRV_U);
                cause = cpu->readMiscReg(
                    RiscvISA::MiscRegIndex::MISCREG_UCAUSE, tid);
            }
            auto exception_no =inst_fault->exception();
            if (faultNum.find(exception_no) != faultNum.end()) {
                DPRINTF(Commit, "Force to raise No.%lu exception at page fault\n", inst_fault);
                cpu->setExceptionGuideExecInfo(
                    exception_no, cpu->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTVAL, tid),
                    cpu->readMiscReg(RiscvISA::MiscRegIndex::MISCREG_STVAL, tid), false, 0, tid);
            }
            if (cause == RiscvISA::ExceptionCode::ECALL_USER ||
                cause == RiscvISA::ExceptionCode::ECALL_SUPER ||
                cause == RiscvISA::ExceptionCode::ECALL_MACHINE) {
                diffInst(tid, head_inst);
            }

            // Maybe set interrupt here?

        }

        // Generate trap squash event.
        generateTrapEvent(tid, inst_fault);
        return false;
    }

    // if (IsSerializeAfter, IsNonSpeculative, IsReturn)
    if (head_inst->isSerializeAfter() && head_inst->isNonSpeculative() && head_inst->isReturn()) {
        DPRINTF(CommitTrace, "Priv return [sn:%llu] PC %s: %s, mepc: %#lx, sepc: %#lx\n", head_inst->seqNum,
                head_inst->pcState(), head_inst->staticInst->disassemble(head_inst->pcState().instAddr()),
                cpu->readMiscRegNoEffect(RiscvISA::MISCREG_MEPC, tid),
                cpu->readMiscRegNoEffect(RiscvISA::MISCREG_SEPC, tid));
    }

    committedPC[tid] = head_inst->pcState().instAddr();

    updateComInstStats(head_inst);

    // head_inst->printDisassembly();
    uint64_t delta = (curTick() - lastCommitTick) / 500;
    if (head_inst->isLoad() && (delta > 250)) {
        DPRINTF(CommitTrace, "Inst[sn:%lu] commit blocked cycles: %lu\n",
                head_inst->seqNum, delta);
    }
    if (head_inst->isControl() && head_inst->pcState().instAddr() == 0x229d6) {
        DPRINTF(CommitTrace,
                "%#lx taken: %i, predicted taken: %i, mispredicted: %i\n",
                head_inst->pcState().instAddr(),
                head_inst->readPredTaken() ^ head_inst->mispredicted(),
                head_inst->readPredTaken(), head_inst->mispredicted());
    }

    if (archDBer && head_inst->isMemRef())
        dumpTicks(head_inst);
    lastCommitTick = curTick();

    DPRINTF(Commit,
            "[tid:%i] [sn:%llu] Committing instruction with PC %s\n",
            tid, head_inst->seqNum, head_inst->pcState());

    DPRINTF(
        InstCommited, "[instCommited: %d, %s]\n", head_inst->opClass(),
        head_inst->staticInst->disassemble(head_inst->pcState().instAddr()));

    if (head_inst->traceData) {
        head_inst->traceData->setFetchSeq(head_inst->seqNum);
        head_inst->traceData->setCPSeq(thread[tid]->numOp);
        head_inst->traceData->dump();
        delete head_inst->traceData;
        head_inst->traceData = NULL;
    }
    if (head_inst->isReturn()) {
        DPRINTF(Commit,
                "[tid:%i] [sn:%llu] Return Instruction Committed PC %s \n",
                tid, head_inst->seqNum, head_inst->pcState());
    }

    if (head_inst->isStoreConditional()) {
        DPRINTF(Commit, "[tid:%i] [sn:%llu] Store Conditional success: %i\n", tid, head_inst->seqNum,
                head_inst->lockedWriteSuccess());
        cpu->setSCSuccess(head_inst->lockedWriteSuccess(), head_inst->physEffAddr);
    }

    // Update the commit rename map
    for (int i = 0; i < head_inst->numDestRegs(); i++) {
        renameMap[tid]->setEntry(head_inst->flattenedDestIdx(i),
                                 head_inst->renamedDestIdx(i));
        DPRINTF(Commit, "Committing rename map entry %s -> p%i\n",
                head_inst->destRegIdx(i),
                head_inst->renamedDestIdx(i)->flatIndex());
    }

    // hardware transactional memory
    // the HTM UID is purely for correctness and debugging purposes
    if (head_inst->isHtmStart())
        iewStage->setLastRetiredHtmUid(tid, head_inst->getHtmTransactionUid());

    // Finally clear the head ROB entry.
    rob->retireHead(tid);

    // If this was a store, record it for this cycle.
    if (head_inst->isStore() || head_inst->isAtomic())
        committedStores[tid] = true;

    // Return true to indicate that we have committed an instruction.
    return true;
}

void
Commit::getInsts()
{
    DPRINTF(Commit, "Getting instructions from Rename stage.\n");

    // Read any renamed instructions and place them into the ROB.
    int insts_to_process = std::min((int)renameWidth * 2, fromRename->size);

    for (int inst_num = 0; inst_num < insts_to_process; ++inst_num) {
        const DynInstPtr &inst = fromRename->insts[inst_num];
        ThreadID tid = inst->threadNumber;

        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();
        }

        if (!inst->isSquashed() &&
            commitStatus[tid] != ROBSquashing &&
            commitStatus[tid] != TrapPending) {
            changedROBNumEntries[tid] = true;

            DPRINTF(Commit, "[tid:%i] [sn:%llu] Inserting PC %s into ROB.\n",
                    tid, inst->seqNum, inst->pcState());

            rob->insertInst(inst);

            assert(rob->getThreadEntries(tid) <= rob->getMaxEntries(tid));

            youngestSeqNum[tid] = inst->seqNum;
        } else {
            DPRINTF(Commit, "[tid:%i] [sn:%llu] "
                    "Instruction PC %s was squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());
        }
    }
}


void
Commit::squashInflightAndUpdateVersion(ThreadID tid)
{
    DPRINTF(Commit, "Squashing in-flight renamed instructions\n");
    int cycle = 0;  // Mark instructions renamed this cycle as squashed
    auto tb_ptr = renameQueue->getWire(cycle);
    DPRINTF(Commit, "%u insts in flight at cycle %d\n", tb_ptr->size,
            cycle);
    for (unsigned i_idx = 0; i_idx < tb_ptr->size; i_idx++) {
        const DynInstPtr &inst = tb_ptr->insts[i_idx];
        DPRINTF(Commit, "[tid:%i] [sn:%llu] Squashing in-flight "
                "instruction PC %s\n",
                inst->threadNumber, inst->seqNum, inst->pcState());
        inst->setSquashed();
    }

    localSquashVer.update(localSquashVer.nextVersion());
    toIEW->commitInfo[tid].squashVersion = localSquashVer;
    DPRINTF(Commit, "Updating squash version to %u\n",
            localSquashVer.getVersion());
}

void
Commit::markCompletedInsts()
{
    // Grab completed insts out of the IEW instruction queue, and mark
    // instructions completed within the ROB.
    for (int inst_num = 0; inst_num < fromIEW->size; ++inst_num) {
        assert(fromIEW->insts[inst_num]);
        if (!fromIEW->insts[inst_num]->isSquashed()) {
            DPRINTF(Commit, "[tid:%i] Marking PC %s, [sn:%llu] ready "
                    "within ROB.\n",
                    fromIEW->insts[inst_num]->threadNumber,
                    fromIEW->insts[inst_num]->pcState(),
                    fromIEW->insts[inst_num]->seqNum);

            // Mark the instruction as ready to commit.
            fromIEW->insts[inst_num]->setCanCommit();
        }
    }
}

void
Commit::updateComInstStats(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;

    if (!inst->isMicroop() || inst->isLastMicroop())
        stats.instsCommitted[tid]++;
    stats.opsCommitted[tid]++;

    // To match the old model, don't count nops and instruction
    // prefetches towards the total commit count.
    if ((!inst->isNop() || inst->staticInst->isMov()) &&
        !inst->isInstPrefetch()) {
        cpu->instDone(tid, inst);
    }

    //
    //  Control Instructions
    //
    //
    if (inst->isControl()) {
        bool mispred = inst->mispredicted();
        std::unique_ptr<PCStateBase> tmp_next_pc(inst->pcState().clone());
        inst->staticInst->advancePC(*tmp_next_pc);
        // add if taken
        if (tmp_next_pc->instAddr() != inst->fallThruPC) {
            BranchInfo temp = {inst->pcState().instAddr(),
                               tmp_next_pc->instAddr()};
            if (branchLog.size() >= 20) {
                branchLog.pop_front();
            }
            branchLog.push_back(temp);
        }
        // tracing recent taken branches
        DPRINTF(DecoupleBP, "Control inst %lu, PC: %#lx -> target: %#lx\n",
                inst->seqNum, inst->pcState().instAddr(),
                tmp_next_pc->instAddr());
        DPRINTF(DecoupleBP, "mispredicted: %i, pred taken: %i\n", mispred,
                inst->readPredTaken());
        DPRINTF(DecoupleBP, "Start print branch logs\n");
        for (auto it : branchLog) {
            DPRINTF(DecoupleBP, "control pc: %#lx -> target pc: %#lx\n,",
                    it.pc, it.target);
        }
        DPRINTF(DecoupleBP, "End\n");
        
       
        stats.branches[tid]++;
    }

    //
    //  Memory references
    //
    if (inst->isMemRef()) {
        stats.memRefs[tid]++;

        if (inst->isLoad()) {
            stats.loads[tid]++;
        }

        if (inst->isAtomic()) {
            stats.amos[tid]++;
        }
    }

    if (inst->isFullMemBarrier()) {
        stats.membars[tid]++;
    }

    // Integer Instruction
    if (inst->isInteger())
        stats.integer[tid]++;

    // Floating Point Instruction
    if (inst->isFloating())
        stats.floating[tid]++;
    // Vector Instruction
    if (inst->isVector()) {
        stats.vectorInstructions[tid]++;
        if (!inst->isMicroop() || inst->isLastMicroop()) {
            auto vecInst = dynamic_cast<RiscvISA::VectorMicroInst *>(inst->staticInst.get());
            if (inst->opClass() == enums::VectorSegUnitStrideLoad) {
                stats.segUnitStrideNF.sample(vecInst->vmi.nf);
            } else if (inst->opClass() == enums::VectorSegStridedLoad) {
                stats.segStrideNF.sample(vecInst->vmi.nf);
            } else if (inst->opClass() == enums::VectorSegIndexedLoad) {
                stats.segIndexedNF.sample(vecInst->vmi.nf);
            }
        }
        if (inst->isMicroop()) {
            auto vecInst = dynamic_cast<RiscvISA::VectorMicroInst *>(inst->staticInst.get());
            if (vecInst->vma) {
                stats.vectorVma++;
            } else {
                stats.vectorVmu++;
            }

            if (vecInst->vta) {
                stats.vectorVta++;
            } else {
                stats.vectorVtu++;
            }
        }
    }

    // Function Calls
    if (inst->isCall())
        stats.functionCalls[tid]++;

}

////////////////////////////////////////
//                                    //
//  SMT COMMIT POLICY MAINTAINED HERE //
//                                    //
////////////////////////////////////////
ThreadID
Commit::getCommittingThread()
{
    if (numThreads > 1) {
        switch (commitPolicy) {
          case CommitPolicy::RoundRobin:
            return roundRobin();

          case CommitPolicy::OldestReady:
            return oldestReady();

          default:
            return InvalidThreadID;
        }
    } else {
        assert(!activeThreads->empty());
        ThreadID tid = activeThreads->front();

        if (commitStatus[tid] == Running ||
            commitStatus[tid] == Idle ||
            commitStatus[tid] == FetchTrapPending) {
            return tid;
        } else {
            return InvalidThreadID;
        }
    }
}

ThreadID
Commit::roundRobin()
{
    std::list<ThreadID>::iterator pri_iter = priority_list.begin();
    std::list<ThreadID>::iterator end      = priority_list.end();

    while (pri_iter != end) {
        ThreadID tid = *pri_iter;

        if (commitStatus[tid] == Running ||
            commitStatus[tid] == Idle ||
            commitStatus[tid] == FetchTrapPending) {

            if (rob->isHeadReady(tid)) {
                priority_list.erase(pri_iter);
                priority_list.push_back(tid);

                return tid;
            }
        }

        pri_iter++;
    }

    return InvalidThreadID;
}

ThreadID
Commit::oldestReady()
{
    unsigned oldest = 0;
    unsigned oldest_seq_num = 0;
    bool first = true;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!rob->isEmpty(tid) &&
            (commitStatus[tid] == Running ||
             commitStatus[tid] == Idle ||
             commitStatus[tid] == FetchTrapPending)) {

            if (rob->isHeadReady(tid)) {

                const DynInstPtr &head_inst = rob->readHeadInst(tid);

                if (first) {
                    oldest = tid;
                    oldest_seq_num = head_inst->seqNum;
                    first = false;
                } else if (head_inst->seqNum < oldest_seq_num) {
                    oldest = tid;
                    oldest_seq_num = head_inst->seqNum;
                }
            }
        }
    }

    if (!first) {
        return oldest;
    } else {
        return InvalidThreadID;
    }
}

void
Commit::dumpTicks(const DynInstPtr &inst)
{
    assert(archDBer);
    archDBer->memTraceWrite(curTick(), inst->isLoad(), inst->pcState().instAddr(), inst->effAddr, inst->physEffAddr,
                            inst->firstIssue, inst->translatedTick, inst->completionTick, curTick(), 0, inst->pf_source);
}

} // namespace o3
} // namespace gem5
