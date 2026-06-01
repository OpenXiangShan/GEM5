/*
 * Copyright (c) 2011-2012, 2014, 2016, 2017, 2019-2020 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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
 * Copyright (c) 2011 Regents of the University of California
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

#include "cpu/o3/cpu.hh"

#include <algorithm>
#include <cassert>
#include <limits>
#include <memory>

#include "arch/riscv/regs/misc.hh"
#include "config/the_isa.hh"
#include "cpu/activity.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/checker/thread_context.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/thread_context.hh"
#include "cpu/o3/trace/TraceReader.hh"
#include "cpu/reg_class.hh"
#include "cpu/simple_thread.hh"
#include "cpu/thread_context.hh"
#include "debug/Activity.hh"
#include "debug/Commit.hh"
#include "debug/Drain.hh"
#include "debug/O3CPU.hh"
#include "debug/Quiesce.hh"
#include "debug/TaskGraph.hh"
#include "debug/ValueCommit.hh"
#include "enums/MemoryMode.hh"
#include "sim/async.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/stat_control.hh"
#include "sim/system.hh"

namespace gem5
{

struct BaseCPUParams;

namespace o3
{

namespace
{

bool
samePrepareSummary(const PipelineTimeBufferSnapshots::PrepareSummary &lhs,
                   const PipelineTimeBufferSnapshots::PrepareSummary &rhs)
{
    return lhs.cycle == rhs.cycle &&
           lhs.valid == rhs.valid &&
           lhs.forwardInstRefs == rhs.forwardInstRefs &&
           lhs.fetchGroups == rhs.fetchGroups &&
           lhs.squashSignals == rhs.squashSignals &&
           lhs.robSquashingSignals == rhs.robSquashingSignals &&
           lhs.branchMispredictSignals == rhs.branchMispredictSignals &&
           lhs.resolvedCFIs == rhs.resolvedCFIs;
}

bool
sameStallLatch(const StallSignalLatch &lhs, const StallSignalLatch &rhs)
{
    for (int tid = 0; tid < MaxThreads; ++tid) {
        if (lhs.block[tid] != rhs.block[tid] ||
            lhs.reason[tid] != rhs.reason[tid]) {
            return false;
        }
    }
    return true;
}

bool
sameStallReasonVector(const std::vector<StallReason> &lhs,
                      const std::vector<StallReason> &rhs)
{
    if (lhs.size() != rhs.size())
        return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i])
            return false;
    }
    return true;
}

bool
sameFetchInstSeqNums(const std::vector<InstSeqNum> &expected,
                     const FetchStruct &actual)
{
    if (expected.size() != static_cast<size_t>(actual.size))
        return false;
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!actual.insts[i] || actual.insts[i]->seqNum != expected[i])
            return false;
    }
    return true;
}

} // anonymous namespace

CPU::CPU(const BaseO3CPUParams &params)
    : BaseCPU(params),
      mmu(params.mmu),
      tickEvent([this]{ tick(); }, "O3CPU tick",
                false, Event::CPU_Tick_Pri),
      threadExitEvent([this]{ exitThreads(); }, "O3CPU exit threads",
                false, Event::CPU_Exit_Pri),
#ifndef NDEBUG
      instcount(0),
#endif
      removeInstsThisCycle(false),
      fetch(this, params),
      decode(this, params),
      rename(this, params),
      iew(this, params),
      commit(this, fetch.getBp(), params),

      regFile(params.numPhysIntRegs,
              params.numPhysFloatRegs,
              params.numPhysVecRegs,
              params.numPhysVecPredRegs,
              params.numPhysCCRegs,
              params.numPhysRMiscRegs,
              params.isa[0]->regClasses()),

      freeList(name() + ".freelist", &regFile),

      rob(this, params),

      scoreboard(name() + ".scoreboard", regFile.totalNumPhysRegs()),

      isa(numThreads, NULL),

      timeBuffer(params.backComSize, params.forwardComSize),
      fetchTimebuffer(params.backComSize, params.forwardComSize),
      decodeTimebuffer(params.backComSize, params.forwardComSize),
      renameTimebuffer(params.backComSize, params.forwardComSize),
      iewTimebuffer(params.backComSize, params.forwardComSize),
      activityRec(name(), NumStages,
                  params.backComSize + params.forwardComSize,
                  params.activity),
      globalSeqNum(1),
      system(params.system),
      lastRunningCycle(curCycle()),
      archDBer(params.arch_db),
      perfCCT(new PerfCCT(params.arch_db && params.arch_db->dumpLifetime, params.arch_db)),
      ipc_r("ipc", "", 1000, archDBer),
      cpi_r("cpi", "", 1000, archDBer),
      issueWidth(params.decodeWidth),
      enableMoveElimination(params.enableMoveElimination),
      enableConstantFolding(params.enableConstantFolding),
      enableMovImmElimination(params.enableMovImmElimination),
      taskGraphFetchToDecodeDelay(params.fetchToDecodeDelay),
      taskGraphDecodeToFetchDelay(params.decodeToFetchDelay),
      taskGraphDecodeToRenameDelay(params.decodeToRenameDelay),
      taskGraphRenameToIEWDelay(params.renameToIEWDelay),
      taskGraphRenameToCommitDelay(params.renameToROBDelay),
      taskGraphIEWToCommitDelay(params.iewToCommitDelay),
      taskGraphCommitToIEWDelay(params.commitToIEWDelay),
      taskGraphIEWToRenameDelay(params.iewToRenameDelay),
      taskGraphCommitToRenameDelay(params.commitToRenameDelay),
      taskGraphCommitToDecodeDelay(params.commitToDecodeDelay),
      taskGraphCommitToFetchDelay(params.commitToFetchDelay),
      taskRuntime(this, params.system),
      cpuStats(this),
      valuePred(params.valuePred)
{
    fatal_if(FullSystem && params.numThreads > 1,
            "SMT is not supported in O3 in full system mode currently.");

    fatal_if(!FullSystem && params.numThreads < params.workload.size(),
            "More workload items (%d) than threads (%d) on CPU %s.",
            params.workload.size(), params.numThreads, name());

    if (!params.switched_out) {
        _status = Running;
    } else {
        _status = SwitchedOut;
    }

    if (params.checker) {
        BaseCPU *temp_checker = params.checker;
        checker = dynamic_cast<Checker<DynInstPtr> *>(temp_checker);
        checker->setIcachePort(&fetch.getInstPort());
        checker->setSystem(params.system);
    } else {
        checker = NULL;
    }

    if (!FullSystem) {
        thread.resize(numThreads);
        tids.resize(numThreads);
    }

    // The stages also need their CPU pointer setup.  However this
    // must be done at the upper level CPU because they have pointers
    // to the upper level CPU, and not this CPU.

    // Set up Pointers to the activeThreads list for each stage
    fetch.setActiveThreads(&activeThreads);
    decode.setActiveThreads(&activeThreads);
    rename.setActiveThreads(&activeThreads);
    iew.setActiveThreads(&activeThreads);
    commit.setActiveThreads(&activeThreads);

    // Give each of the stages the time buffer they will use.
    fetch.setTimeBuffer(&timeBuffer);
    decode.setTimeBuffer(&timeBuffer);
    rename.setTimeBuffer(&timeBuffer);
    iew.setTimeBuffer(&timeBuffer);
    commit.setTimeBuffer(&timeBuffer);

    // Also setup each of the stages' queues.
    fetch.setFetchQueue(&fetchTimebuffer);
    decode.setFetchQueue(&fetchTimebuffer);
    commit.setFetchQueue(&fetchTimebuffer);
    decode.setDecodeQueue(&decodeTimebuffer);
    rename.setDecodeQueue(&decodeTimebuffer);
    rename.setRenameQueue(&renameTimebuffer);
    iew.setRenameQueue(&renameTimebuffer);
    iew.setIEWQueue(&iewTimebuffer);
    commit.setIEWQueue(&iewTimebuffer);
    commit.setRenameQueue(&renameTimebuffer);

    decode.setFetchStage(&fetch);
    commit.setIEWStage(&iew);
    commit.setDecodeStage(&decode);
    rename.setIEWStage(&iew);
    rename.setCommitStage(&commit);

    fetch.setStallSignals(&stallSignalBank.legacyView());
    decode.setStallSignals(&stallSignalBank.legacyView());
    rename.setStallSignals(&stallSignalBank.legacyView());
    iew.setStallSignals(&stallSignalBank.legacyView());
    commit.setStallSignals(&stallSignalBank.legacyView());
    fetch.setStallSignalBank(&stallSignalBank);
    decode.setStallSignalBank(&stallSignalBank);
    rename.setStallSignalBank(&stallSignalBank);
    iew.setStallSignalBank(&stallSignalBank);
    commit.setStallSignalBank(&stallSignalBank);
    const unsigned task_window = std::max(taskRuntime.windowCycles(),
                                          taskRuntime.maxInFlightCycles());
    stallSignalBank.configureWindow(task_window);
    pipelineSnapshots.configureWindow(task_window);

    ThreadID active_threads;
    if (FullSystem) {
        active_threads = 1;
    } else {
        active_threads = params.workload.size();

        if (active_threads > MaxThreads) {
            panic("Workload Size too large. Increase the 'MaxThreads' "
                  "constant in cpu/o3/limits.hh or edit your workload size.");
        }
    }

    // Make Sure That this a Valid Architeture
    assert(numThreads);
    const auto &regClasses = params.isa[0]->regClasses();

    assert(params.numPhysIntRegs >=
            numThreads * regClasses.at(IntRegClass).numRegs());
    assert(params.numPhysFloatRegs >=
            numThreads * regClasses.at(FloatRegClass).numRegs());
    assert(params.numPhysVecRegs >=
            numThreads * regClasses.at(VecRegClass).numRegs());
    assert(params.numPhysVecPredRegs >=
            numThreads * regClasses.at(VecPredRegClass).numRegs());
    assert(params.numPhysCCRegs >=
            numThreads * regClasses.at(CCRegClass).numRegs());

    // Just make this a warning and go ahead anyway, to keep from having to
    // add checks everywhere.
    warn_if(regClasses.at(CCRegClass).numRegs() == 0 &&
            params.numPhysCCRegs != 0,
            "Non-zero number of physical CC regs specified, even though\n"
            "    ISA does not use them.");

    rename.setScoreboard(&scoreboard);
    iew.setScoreboard(&scoreboard);
    iew.setRob(&rob);

    // Setup the rename map for whichever stages need it.
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        isa[tid] = dynamic_cast<TheISA::ISA *>(params.isa[tid]);
        warn("Setting isa ptr of cpu to %p", isa[tid]);
        commitRenameMap[tid].init(regClasses, &regFile, &freeList);
        renameMap[tid].init(regClasses, &regFile, &freeList);
    }

    // Initialize rename map to assign physical registers to the
    // architectural registers for active threads only.
    for (ThreadID tid = 0; tid < active_threads; tid++) {
        for (auto type = (RegClassType)0; type <= RMiscRegClass;
                type = (RegClassType)(type + 1)) {
            for (RegIndex ridx = 0; ridx < regClasses.at(type).numRegs();
                    ++ridx) {
                // Note that we can't use the rename() method because we don't
                // want special treatment for the zero register at this point
                RegId rid = RegId(type, ridx);
                PhysRegIdPtr phys_reg = freeList.getReg(type);
                renameMap[tid].setEntry(rid, VirtRegId(phys_reg));
                commitRenameMap[tid].setEntry(rid, VirtRegId(phys_reg));
            }
        }
    }

    rename.setRenameMap(renameMap);
    commit.setRenameMap(commitRenameMap);
    rename.setFreeList(&freeList);

    // Setup the ROB for whichever stages need it.
    commit.setROB(&rob);

    lastActivatedCycle = 0;

    DPRINTF(O3CPU, "Creating O3CPU object.\n");

    // Setup any thread state.
    thread.resize(numThreads);

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (FullSystem) {
            // SMT is not supported in FS mode yet.
            assert(numThreads == 1);
            thread[tid] = new ThreadState(this, 0, NULL);
        } else {
            if (tid < params.workload.size()) {
                DPRINTF(O3CPU, "Workload[%i] process is %#x", tid,
                        thread[tid]);
                thread[tid] = new ThreadState(this, tid, params.workload[tid]);
            } else {
                //Allocate Empty thread so M5 can use later
                //when scheduling threads to CPU
                Process* dummy_proc = NULL;

                thread[tid] = new ThreadState(this, tid, dummy_proc);
            }
        }

        gem5::ThreadContext *tc;

        // Setup the TC that will serve as the interface to the threads/CPU.
        auto *o3_tc = new ThreadContext;

        tc = o3_tc;

        // If we're using a checker, then the TC should be the
        // CheckerThreadContext.
        if (params.checker) {
            tc = new CheckerThreadContext<ThreadContext>(o3_tc, checker);
        }

        o3_tc->cpu = this;
        o3_tc->thread = thread[tid];

        // Give the thread the TC.
        thread[tid]->tc = tc;

        // Add the TC to the CPU's list of TC's.
        threadContexts.push_back(tc);
    }

    // O3CPU always requires an interrupt controller.
    if (!params.switched_out && interrupts.empty()) {
        fatal("O3CPU %s has no interrupt controller.\n"
              "Ensure createInterruptController() is called.\n", name());
    }
}

void
CPU::regProbePoints()
{
    BaseCPU::regProbePoints();

    ppInstAccessComplete = new ProbePointArg<PacketPtr>(
            getProbeManager(), "InstAccessComplete");
    ppDataAccessComplete = new ProbePointArg<
        std::pair<DynInstPtr, PacketPtr>>(
                getProbeManager(), "DataAccessComplete");

    fetch.regProbePoints();
    rename.regProbePoints();
    iew.regProbePoints();
    commit.regProbePoints();
}

CPU::CPUStats::CPUStats(CPU *cpu)
    : statistics::Group(cpu),
      ADD_STAT(timesIdled, statistics::units::Count::get(),
               "Number of times that the entire CPU went into an idle state "
               "and unscheduled itself"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Total number of cycles that the CPU has spent unscheduled due "
               "to idling"),
      ADD_STAT(quiesceCycles, statistics::units::Cycle::get(),
               "Total number of cycles that CPU has spent quiesced or waiting "
               "for an interrupt"),
      ADD_STAT(committedInsts, statistics::units::Count::get(),
               "Number of Instructions Simulated"),
      ADD_STAT(committedOps, statistics::units::Count::get(),
               "Number of Ops (including micro ops) Simulated"),
      ADD_STAT(cpi, statistics::units::Rate<
                    statistics::units::Cycle, statistics::units::Count>::get(),
               "CPI: Cycles Per Instruction"),
      ADD_STAT(totalCpi, statistics::units::Rate<
                    statistics::units::Cycle, statistics::units::Count>::get(),
               "CPI: Total CPI of All Threads"),
      ADD_STAT(ipc, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "IPC: Instructions Per Cycle"),
      ADD_STAT(totalIpc, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "IPC: Total IPC of All Threads"),
      ADD_STAT(baseRetiring, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Retiring"),
      ADD_STAT(frontendBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Frontend Bound"),
      ADD_STAT(frontendLatencyBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Frontend Latency Bound"),
      ADD_STAT(frontendBandwidthBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Frontend Bandwidth Bound"),
      ADD_STAT(badSpecBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Bad Speculation"),
      ADD_STAT(branchMissPrediction, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Branch Missprediction"),
      ADD_STAT(machineClears, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Machine Clears"),
      ADD_STAT(backendBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level1: Backend Bound"),
      ADD_STAT(coreBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Core Bound"),
      ADD_STAT(memoryBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level2: |--Memory Bound"),
      ADD_STAT(l1Bound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--L1 Bound"),
      ADD_STAT(l2Bound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--L2 Bound"),
      ADD_STAT(l3Bound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--L3 Bound"),
      ADD_STAT(memBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--Memory Bound"),
      ADD_STAT(storeBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
               "Level3:    |--Store Bound"),
      ADD_STAT(intRegfileReads, statistics::units::Count::get(),
               "Number of integer regfile reads"),
      ADD_STAT(intRegfileWrites, statistics::units::Count::get(),
               "Number of integer regfile writes"),
      ADD_STAT(fpRegfileReads, statistics::units::Count::get(),
               "Number of floating regfile reads"),
      ADD_STAT(fpRegfileWrites, statistics::units::Count::get(),
               "Number of floating regfile writes"),
      ADD_STAT(vecRegfileReads, statistics::units::Count::get(),
               "number of vector regfile reads"),
      ADD_STAT(vecRegfileWrites, statistics::units::Count::get(),
               "number of vector regfile writes"),
      ADD_STAT(vecPredRegfileReads, statistics::units::Count::get(),
               "number of predicate regfile reads"),
      ADD_STAT(vecPredRegfileWrites, statistics::units::Count::get(),
               "number of predicate regfile writes"),
      ADD_STAT(ccRegfileReads, statistics::units::Count::get(),
               "number of cc regfile reads"),
      ADD_STAT(ccRegfileWrites, statistics::units::Count::get(),
               "number of cc regfile writes"),
      ADD_STAT(miscRegfileReads, statistics::units::Count::get(),
               "number of misc regfile reads"),
      ADD_STAT(miscRegfileWrites, statistics::units::Count::get(),
               "number of misc regfile writes"),
      ADD_STAT(lastCommitTick, statistics::units::Count::get(),
               "The last tick to commit an instruction")
{
    // Register any of the O3CPU's stats here.
    timesIdled
        .prereq(timesIdled);

    idleCycles
        .prereq(idleCycles);

    quiesceCycles
        .prereq(quiesceCycles);

    // Number of Instructions simulated
    // --------------------------------
    // Should probably be in Base CPU but need templated
    // MaxThreads so put in here instead
    committedInsts
        .init(cpu->numThreads)
        .flags(statistics::total);

    committedOps
        .init(cpu->numThreads)
        .flags(statistics::total);

    cpi
        .precision(6);
    cpi = cpu->baseStats.numCycles / committedInsts;

    totalCpi
        .precision(6);
    totalCpi = cpu->baseStats.numCycles / sum(committedInsts);

    ipc
        .precision(6);
    ipc = committedInsts / cpu->baseStats.numCycles;

    totalIpc
        .precision(6);
    totalIpc = sum(committedInsts) / cpu->baseStats.numCycles;

    baseRetiring = committedInsts / (cpu->issueWidth * cpu->baseStats.numCycles);

    frontendBound = cpu->fetch.getFetchStats().fetchBubbles /
        (cpu->issueWidth * cpu->baseStats.numCycles);

    frontendLatencyBound = cpu->fetch.getFetchStats().fetchBubbles_max / cpu->baseStats.numCycles;

    frontendBandwidthBound = frontendBound - frontendLatencyBound;

    // badSpecBound = (INST_SPEC - INST_RETIRED + RECOVERY_BUBBLE)/(IssueBW * CPU_CYCLES)
    badSpecBound = (cpu->iew.getIEWStats().dispatchedInsts - committedInsts + cpu->commit.getCommitStats().recovery_bubble) /
         (cpu->issueWidth * cpu->baseStats.numCycles);

    // branchMissPrediction = Bad Speculation * BR_MIS_PRED/TOTAL_FLUSH
    branchMissPrediction = badSpecBound * cpu->commit.getCommitStats().branchMispredicts / cpu->commit.getCommitStats().totalSquash;

    machineClears = badSpecBound - branchMissPrediction;

    backendBound = 1 - (frontendBound + badSpecBound + baseRetiring);

    Scheduler* scheduler = cpu->iew.getScheduler();
    const auto &stats = scheduler->getStats();

    // Calculate raw proportions first
    auto rawCore = stats.exec_stall_cycle - stats.memstall_any_load - stats.memstall_any_store;
    auto rawMemory = stats.memstall_any_load + stats.memstall_any_store;
    auto rawTotal = rawCore + rawMemory;

    // Scale Level 2: ensure Core + Memory = Backend
    coreBound = backendBound * rawCore / rawTotal;
    memoryBound = backendBound * rawMemory / rawTotal;

    // Scale Level 3: ensure sub-components sum to Memory
    auto rawL1 = stats.memstall_any_load - stats.memstall_l1miss;
    auto rawL2 = stats.memstall_l1miss - stats.memstall_l2miss;
    auto rawL3 = stats.memstall_l2miss - stats.memstall_l3miss;
    auto rawL3Total = rawL1 + rawL2 + rawL3 + stats.memstall_l3miss + stats.memstall_any_store;

    l1Bound = memoryBound * rawL1 / rawL3Total;
    l2Bound = memoryBound * rawL2 / rawL3Total;
    l3Bound = memoryBound * rawL3 / rawL3Total;
    memBound = memoryBound * stats.memstall_l3miss / rawL3Total;
    storeBound = memoryBound * stats.memstall_any_store / rawL3Total;

    intRegfileReads
        .prereq(intRegfileReads);

    intRegfileWrites
        .prereq(intRegfileWrites);

    fpRegfileReads
        .prereq(fpRegfileReads);

    fpRegfileWrites
        .prereq(fpRegfileWrites);

    vecRegfileReads
        .prereq(vecRegfileReads);

    vecRegfileWrites
        .prereq(vecRegfileWrites);

    vecPredRegfileReads
        .prereq(vecPredRegfileReads);

    vecPredRegfileWrites
        .prereq(vecPredRegfileWrites);

    ccRegfileReads
        .prereq(ccRegfileReads);

    ccRegfileWrites
        .prereq(ccRegfileWrites);

    miscRegfileReads
        .prereq(miscRegfileReads);

    miscRegfileWrites
        .prereq(miscRegfileWrites);
}

const PipelineTimeBufferSnapshots::Frame *
CPU::pipelineInputSnapshot(Cycles cycle)
{
    const auto *frame = pipelineSnapshots.inputFrame(cycle);
    const bool hit = frame != nullptr;
    taskRuntime.recordTimeBufferStageInputRead(hit);
    return frame;
}

const TimeStruct *
CPU::pipelineInputBackward(Cycles cycle, int offset)
{
    const auto *frame = pipelineInputSnapshot(cycle);
    const TimeStruct *slot = frame ? frame->backward.get(offset) : nullptr;
    taskRuntime.recordTimeBufferBackwardSlotRead(slot != nullptr);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !frame,
             "Missing current-cycle TimeBuffer input frame for backward "
             "slot read: cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    panic_if(require_hit && !slot,
             "Missing current-cycle backward TimeBuffer input slot: "
             "cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    return slot;
}

const TimeStruct *
CPU::pipelineInputFetchBackward(Cycles cycle, int offset)
{
    const auto *frame = pipelineInputSnapshot(cycle);
    const TimeStruct *slot = frame ? frame->backward.get(offset) : nullptr;
    taskRuntime.recordTimeBufferBackwardSlotRead(slot != nullptr);
    taskRuntime.recordTimeBufferFetchBackwardSlotRead(slot != nullptr);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !frame,
             "Missing current-cycle TimeBuffer input frame for Fetch "
             "backward slot read: cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    panic_if(require_hit && !slot,
             "Missing current-cycle Fetch backward TimeBuffer input slot: "
             "cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    return slot;
}

const FetchStruct *
CPU::pipelineInputFetchToDecode(Cycles cycle, int offset)
{
    const auto *frame = pipelineInputSnapshot(cycle);
    const FetchStruct *slot = frame ?
        frame->fetchToDecode.get(offset) : nullptr;
    taskRuntime.recordTimeBufferFetchToDecodeSlotRead(slot != nullptr);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !frame,
             "Missing current-cycle TimeBuffer input frame for "
             "Fetch-to-Decode slot read: cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    panic_if(require_hit && !slot,
             "Missing current-cycle Fetch-to-Decode input slot: "
             "cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    return slot;
}

const DecodeStruct *
CPU::pipelineInputDecodeToRename(Cycles cycle, int offset)
{
    const auto *frame = pipelineInputSnapshot(cycle);
    const DecodeStruct *slot = frame ?
        frame->decodeToRename.get(offset) : nullptr;
    taskRuntime.recordTimeBufferDecodeToRenameSlotRead(slot != nullptr);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !frame,
             "Missing current-cycle TimeBuffer input frame for "
             "Decode-to-Rename slot read: cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    panic_if(require_hit && !slot,
             "Missing current-cycle Decode-to-Rename input slot: "
             "cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    return slot;
}

const RenameStruct *
CPU::pipelineInputRenameToIEW(Cycles cycle, int offset)
{
    const auto *frame = pipelineInputSnapshot(cycle);
    const RenameStruct *slot = frame ?
        frame->renameToIEW.get(offset) : nullptr;
    taskRuntime.recordTimeBufferRenameToIEWSlotRead(slot != nullptr);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !frame,
             "Missing current-cycle TimeBuffer input frame for "
             "Rename-to-IEW slot read: cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    panic_if(require_hit && !slot,
             "Missing current-cycle Rename-to-IEW input slot: "
             "cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    return slot;
}

const RenameStruct *
CPU::pipelineInputRenameToCommit(Cycles cycle, int offset)
{
    const auto *frame = pipelineInputSnapshot(cycle);
    const RenameStruct *slot = frame ?
        frame->renameToIEW.get(offset) : nullptr;
    taskRuntime.recordTimeBufferRenameToCommitSlotRead(slot != nullptr);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !frame,
             "Missing current-cycle TimeBuffer input frame for "
             "Rename-to-Commit slot read: cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    panic_if(require_hit && !slot,
             "Missing current-cycle Rename-to-Commit input slot: "
             "cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    return slot;
}

const IEWStruct *
CPU::pipelineInputIEWToCommit(Cycles cycle, int offset)
{
    const auto *frame = pipelineInputSnapshot(cycle);
    const IEWStruct *slot = frame ?
        frame->iewToCommit.get(offset) : nullptr;
    taskRuntime.recordTimeBufferIEWToCommitSlotRead(slot != nullptr);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !frame,
             "Missing current-cycle TimeBuffer input frame for "
             "IEW-to-Commit slot read: cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    panic_if(require_hit && !slot,
             "Missing current-cycle IEW-to-Commit input slot: "
             "cycle=%llu offset=%d",
             static_cast<unsigned long long>(cycle), offset);
    return slot;
}

const StallSignalLatch *
CPU::stallSignalSnapshot(Cycles cycle, StallSignalEdge edge)
{
    const auto *latch = stallSignalBank.snapshot(cycle, edge);
    taskRuntime.recordStallSignalInputRead(latch != nullptr);
    if (!latch && cycle != curCycle())
        taskRuntime.recordStallSignalFutureReadBlock();
    return latch;
}

const StallSignalLatch &
CPU::stallSignalSnapshotOrCurrent(Cycles cycle, StallSignalEdge edge)
{
    const auto *latch = stallSignalSnapshot(cycle, edge);
    const bool require_hit = taskRuntime.enabled() && cycle == curCycle();
    panic_if(require_hit && !latch,
             "Missing current-cycle stall signal snapshot: cycle=%llu edge=%u",
             static_cast<unsigned long long>(cycle),
             static_cast<unsigned>(edge));
    return latch ? *latch : stallSignalBank.snapshot(edge);
}

void
CPU::checkFutureWavefrontPrepare(Cycles cycle)
{
    if (!pendingFutureWavefrontPrepare.valid)
        return;

    if (pendingFutureWavefrontPrepare.cycle != cycle) {
        taskRuntime.recordFutureWavefrontPrepareCheck(false, false);
        pendingFutureWavefrontPrepare.valid = false;
        return;
    }

    const StallSignalLatch *commit_to_iew =
        stallSignalBank.snapshot(cycle, StallSignalEdge::CommitToIEW);
    // This wavefront only reuses IEW's prepare result.  Late IEW->Rename
    // latch changes are validated by the consumer wavefronts that reuse them.
    const bool latch_match = commit_to_iew &&
        sameStallLatch(pendingFutureWavefrontPrepare.commitToIEW,
                       *commit_to_iew);

    taskRuntime.recordFutureWavefrontPrepareCheck(true, latch_match);
    if (taskRuntime.traceEnabled() && !latch_match) {
        DPRINTF(TaskGraph,
                "Future wavefront prepare mismatch cycle=%llu\n",
                cycle);
    }

    pendingFutureWavefrontPrepare.valid = false;
}

void
CPU::checkFutureRenameWavefrontPrepare(Cycles cycle)
{
    if (!pendingFutureRenameWavefrontPrepare.valid)
        return;

    if (pendingFutureRenameWavefrontPrepare.cycle != cycle) {
        taskRuntime.recordFutureRenameWavefrontPrepareCheck(false, false);
        pendingFutureRenameWavefrontPrepare.valid = false;
        return;
    }

    const StallSignalLatch *rename_to_decode =
        stallSignalBank.snapshot(cycle, StallSignalEdge::RenameToDecode);
    const bool latch_match = rename_to_decode &&
        sameStallLatch(pendingFutureRenameWavefrontPrepare.renameToDecode,
                       *rename_to_decode);

    taskRuntime.recordFutureRenameWavefrontPrepareCheck(true, latch_match);
    if (taskRuntime.traceEnabled() && !latch_match) {
        DPRINTF(TaskGraph,
                "Future rename wavefront prepare mismatch cycle=%llu\n",
                cycle);
    }

    pendingFutureRenameWavefrontPrepare.valid = false;
}

void
CPU::checkFutureDecodeWavefrontPrepare(Cycles cycle)
{
    if (!pendingFutureDecodeWavefrontPrepare.valid)
        return;

    if (pendingFutureDecodeWavefrontPrepare.cycle != cycle) {
        taskRuntime.recordFutureDecodeWavefrontPrepareCheck(false, false);
        pendingFutureDecodeWavefrontPrepare.valid = false;
        return;
    }

    const StallSignalLatch *decode_to_fetch =
        stallSignalBank.snapshot(cycle, StallSignalEdge::DecodeToFetch);
    const bool latch_match = decode_to_fetch &&
        sameStallLatch(pendingFutureDecodeWavefrontPrepare.decodeToFetch,
                       *decode_to_fetch);

    taskRuntime.recordFutureDecodeWavefrontPrepareCheck(true, latch_match);
    if (taskRuntime.traceEnabled() && !latch_match) {
        DPRINTF(TaskGraph,
                "Future decode wavefront prepare mismatch cycle=%llu\n",
                cycle);
    }

    pendingFutureDecodeWavefrontPrepare.valid = false;
}

void
CPU::checkFutureFetchWavefrontPrepare(Cycles cycle)
{
    if (!pendingFutureFetchWavefrontPrepare.valid)
        return;

    if (pendingFutureFetchWavefrontPrepare.cycle != cycle) {
        taskRuntime.recordFutureFetchWavefrontPrepareCheck(false, false);
        pendingFutureFetchWavefrontPrepare.valid = false;
        return;
    }

    const FetchStruct &fetch_to_decode = fetchTimebuffer[0];
    const bool output_match =
        pendingFutureFetchWavefrontPrepare.size ==
            static_cast<unsigned>(fetch_to_decode.size) &&
        sameStallReasonVector(
            pendingFutureFetchWavefrontPrepare.fetchStallReason,
            fetch_to_decode.fetchStallReason) &&
        sameFetchInstSeqNums(
            pendingFutureFetchWavefrontPrepare.instSeqNums,
            fetch_to_decode);

    taskRuntime.recordFutureFetchWavefrontPrepareCheck(true, output_match);
    if (taskRuntime.traceEnabled() && !output_match) {
        DPRINTF(TaskGraph,
                "Future fetch wavefront prepare mismatch cycle=%llu "
                "expectedSize=%u actualSize=%i\n",
                cycle, pendingFutureFetchWavefrontPrepare.size,
                fetch_to_decode.size);
    }

    pendingFutureFetchWavefrontPrepare.valid = false;
}

void
CPU::tick()
{
    DPRINTF(O3CPU, "\n\nO3CPU: Ticking main, O3CPU.\n");
    assert(!switchedOut());
    assert(drainState() != DrainState::Drained);

    ++baseStats.numCycles;
    ipc_r.roll(1);
    cpi_r++;
    updateCycleCounters(BaseCPU::CPU_STATE_ON);

//    activity = false;

    const Cycles cycle = curCycle();
    bool future_prepare_allowed = false;
    const bool timebuffer_prepare_enabled =
        taskRuntime.timeBufferPrepareEnabled();
    taskRuntime.onSerialTickBegin(cycle);
    taskRuntime.recordWavefrontPlan(cycle,
            {taskGraphFetchToDecodeDelay, taskGraphDecodeToRenameDelay,
             taskGraphRenameToIEWDelay, taskGraphRenameToCommitDelay,
             taskGraphIEWToCommitDelay});
    if (taskRuntime.enabled()) {
        const unsigned candidate_cycles =
            std::min(taskRuntime.windowCycles(),
                     taskRuntime.maxInFlightCycles());
        const Event *next_event = eventQueue()->getHead();
        const bool has_next_event = next_event != nullptr;
        const Tick next_event_tick =
            has_next_event ? next_event->when() : MaxTick;
        const int next_event_priority =
            has_next_event ? next_event->priority() : Event::Maximum_Pri;
        unsigned committable_cycles = candidate_cycles;
        unsigned blocked_offset = 0;
        bool blocked_by_earlier_tick_event = false;
        for (unsigned offset = 1; offset <= candidate_cycles; ++offset) {
            const Tick future_cpu_tick = clockEdge(Cycles(offset));
            const bool event_before_cpu =
                has_next_event && next_event_tick < future_cpu_tick;
            const bool event_at_cpu_priority =
                has_next_event &&
                next_event_tick == future_cpu_tick &&
                next_event_priority <= Event::CPU_Tick_Pri;
            if (event_before_cpu || event_at_cpu_priority) {
                committable_cycles = offset - 1;
                blocked_offset = offset;
                blocked_by_earlier_tick_event = event_before_cpu;
                break;
            }
        }
        taskRuntime.recordEventHorizon(cycle, candidate_cycles,
                committable_cycles, has_next_event, next_event_tick,
                next_event_priority, blocked_offset,
                blocked_by_earlier_tick_event,
                blocked_offset != 0 ? next_event : nullptr);
        if (taskRuntime.traceEnabled() && has_next_event &&
            blocked_offset != 0) {
            DPRINTF(TaskGraph,
                    "Event horizon blocker cycle=%llu blockedOffset=%u "
                    "blockByEarlierTick=%i nextTick=%llu nextPriority=%d "
                    "nextEvent=%s/%s\n",
                    cycle, blocked_offset, blocked_by_earlier_tick_event,
                    next_event_tick, next_event_priority,
                    next_event->name(), next_event->description());
        }
        const bool event_horizon_allows_future_prepare =
            committable_cycles > 0;
        const bool speculation_allows_future_prepare =
            taskRuntime.speculativePrepareAllowed();
        future_prepare_allowed =
            event_horizon_allows_future_prepare &&
            speculation_allows_future_prepare;
        if (event_horizon_allows_future_prepare &&
            !speculation_allows_future_prepare) {
            taskRuntime.recordSpecTaskThrottled();
        }

        const unsigned slots = pipelineSnapshots.captureInputs(
                cycle, timeBuffer, fetchTimebuffer, decodeTimebuffer,
                renameTimebuffer, iewTimebuffer);
        taskRuntime.recordTimeBufferSnapshot(true, slots);
        if (taskRuntime.traceEnabled()) {
            DPRINTF(TaskGraph,
                    "TimeBuffer input snapshot cycle=%llu slots=%u\n",
                    cycle, slots);
        }

        if (timebuffer_prepare_enabled) {
            auto merge_input_summary =
                [this](const PipelineTimeBufferSnapshots::PrepareSummary
                       &summary)
            {
                pipelineSnapshots.mergeInputSummary(summary);
                const uint64_t control_signals =
                    summary.squashSignals +
                    summary.robSquashingSignals +
                    summary.branchMispredictSignals;
                taskRuntime.recordTimeBufferPrepareMerge(
                        summary.forwardInstRefs, control_signals,
                        summary.resolvedCFIs);
                if (taskRuntime.traceEnabled()) {
                    DPRINTF(TaskGraph,
                            "TimeBuffer prepare merge cycle=%llu "
                            "instRefs=%llu control=%llu resolvedCFIs=%llu "
                            "fetchGroups=%llu\n",
                            summary.cycle, summary.forwardInstRefs,
                            control_signals, summary.resolvedCFIs,
                            summary.fetchGroups);
                }
            };

            auto expected_future_summary = std::make_shared<
                    PipelineTimeBufferSnapshots::PrepareSummary>();
            const bool has_expected_future =
                pendingFutureTimeBufferPrepare.valid;
            if (has_expected_future) {
                *expected_future_summary =
                    pendingFutureTimeBufferPrepare.summary;
                pendingFutureTimeBufferPrepare.valid = false;
            }
            const PipelineTimeBufferSnapshots *snapshots = &pipelineSnapshots;
            const bool can_reuse_future =
                has_expected_future && expected_future_summary->cycle == cycle;
            if (can_reuse_future) {
                taskRuntime.recordFutureTimeBufferPrepareReuse();
                merge_input_summary(*expected_future_summary);

                auto verify_summary = std::make_shared<
                        PipelineTimeBufferSnapshots::PrepareSummary>();
                taskRuntime.submitWeak(
                        {cycle, TaskStage::Runtime, 3, InvalidThreadID, 0},
                        slots,
                        [snapshots, verify_summary] {
                            *verify_summary =
                                snapshots->prepareInputSummary();
                        },
                        [this, expected_future_summary, verify_summary] {
                            const bool summary_match =
                                samePrepareSummary(*expected_future_summary,
                                                   *verify_summary);
                            taskRuntime.recordFutureTimeBufferPrepareCheck(
                                    true, summary_match);
                            if (taskRuntime.traceEnabled() &&
                                !summary_match) {
                                DPRINTF(TaskGraph,
                                        "Future TimeBuffer prepare mismatch "
                                        "expectedCycle=%llu actualCycle=%llu "
                                        "cycleMatch=1\n",
                                        expected_future_summary->cycle,
                                        verify_summary->cycle);
                            }
                        });
            } else {
                if (has_expected_future) {
                    taskRuntime.recordFutureTimeBufferPrepareCheck(false,
                                                                   false);
                    if (taskRuntime.traceEnabled()) {
                        DPRINTF(TaskGraph,
                                "Future TimeBuffer prepare stale "
                                "expectedCycle=%llu actualCycle=%llu\n",
                                expected_future_summary->cycle, cycle);
                    }
                }

                auto prepare_summary = std::make_shared<
                        PipelineTimeBufferSnapshots::PrepareSummary>();
                taskRuntime.submitWeak(
                        {cycle, TaskStage::Runtime, 1, InvalidThreadID, 0},
                        slots,
                        [snapshots, prepare_summary] {
                            *prepare_summary =
                                snapshots->prepareInputSummary();
                        },
                        [merge_input_summary, prepare_summary] {
                            merge_input_summary(*prepare_summary);
                        });
            }
        } else {
            pendingFutureTimeBufferPrepare.valid = false;
        }
    }

    bool future_commit_probe_submitted = false;
    auto future_backward_slot = [this](int offset) -> const TimeStruct *
    {
        if (offset < -timeBuffer.pastCycles() ||
            offset > timeBuffer.futureCycles()) {
            return nullptr;
        }
        return &timeBuffer[offset];
    };
    auto future_fetch_slot = [this](int offset) -> const FetchStruct *
    {
        if (offset < -fetchTimebuffer.pastCycles() ||
            offset > fetchTimebuffer.futureCycles()) {
            return nullptr;
        }
        return &fetchTimebuffer[offset];
    };
    auto future_rename_slot = [this](int offset) -> const RenameStruct *
    {
        if (offset < -renameTimebuffer.pastCycles() ||
            offset > renameTimebuffer.futureCycles()) {
            return nullptr;
        }
        return &renameTimebuffer[offset];
    };
    auto future_iew_slot = [this](int offset) -> const IEWStruct *
    {
        if (offset < -iewTimebuffer.pastCycles() ||
            offset > iewTimebuffer.futureCycles()) {
            return nullptr;
        }
        return &iewTimebuffer[offset];
    };
    auto future_decode_slot = [this](int offset) -> const DecodeStruct *
    {
        if (offset < -decodeTimebuffer.pastCycles() ||
            offset > decodeTimebuffer.futureCycles()) {
            return nullptr;
        }
        return &decodeTimebuffer[offset];
    };
    auto record_future_iew_wavefront_skip =
        [this](FutureWavefrontSkipReason reason)
    {
        taskRuntime.recordFutureWavefrontPrepareSkipped();
        taskRuntime.recordFutureWavefrontSkipReason(reason);
        iew.recordFuturePrepareSkipped();
    };
    auto record_future_rename_wavefront_skip =
        [this](FutureWavefrontSkipReason reason)
    {
        taskRuntime.recordFutureRenameWavefrontPrepareSkipped();
        taskRuntime.recordFutureWavefrontSkipReason(reason);
        rename.recordFuturePrepareSkipped();
    };
    auto record_future_decode_wavefront_skip =
        [this](FutureWavefrontSkipReason reason)
    {
        taskRuntime.recordFutureDecodeWavefrontPrepareSkipped();
        taskRuntime.recordFutureWavefrontSkipReason(reason);
        decode.recordFuturePrepareSkipped();
    };
    auto record_future_fetch_wavefront_skip =
        [this](FutureWavefrontSkipReason reason)
    {
        taskRuntime.recordFutureFetchWavefrontPrepareSkipped();
        taskRuntime.recordFutureWavefrontSkipReason(reason);
        fetch.recordFutureToDecodePrepareSkipped();
    };
    auto record_future_rename_candidate_prepare =
        [this](Cycles future_cycle,
               const StallSignalLatch &candidate_iew_to_rename,
               const DecodeStruct *future_decode_to_rename,
               const TimeStruct *future_iew_to_rename,
               const TimeStruct *future_commit_to_rename,
               const IEW::FutureDispatchCandidateProfile &dispatch_profile)
    {
        using IEWBlockReason = IEW::FutureActiveDispatchPreviewBlockReason;
        if (!dispatch_profile.valid ||
            dispatch_profile.blockReason != IEWBlockReason::SchedulerNotReady) {
            return;
        }

        Rename::RenamePrepareInput candidate_input;
        if (!rename.buildFutureDecodeLatchInput(
                    future_cycle, candidate_iew_to_rename,
                    future_decode_to_rename, future_iew_to_rename,
                    future_commit_to_rename, candidate_input, false)) {
            return;
        }

        Rename::FutureCandidatePrepareProfile rename_profile;
        rename_profile.valid = true;
        rename_profile.blockReason =
            static_cast<unsigned>(dispatch_profile.blockReason);
        rename_profile.schedulerReason =
            static_cast<unsigned>(dispatch_profile.schedulerBlockReason);
        rename_profile.fixedBufferPops = dispatch_profile.fixedBufferPops;
        rename_profile.dispatchedBeforeBlock =
            dispatch_profile.dispatchedBeforeBlock;

        rename.setPendingFutureCandidatePrepare(
                rename.previewFuturePrepare(candidate_input),
                rename_profile, candidate_input);
    };
    auto submit_future_commit_probe = [&]
    {
        if (!taskRuntime.enabled() || !future_prepare_allowed ||
            future_commit_probe_submitted) {
            return;
        }

        const Cycles future_cycle = cycle + Cycles(1);
        const int rename_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToCommitDelay));
        const int iew_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToCommitDelay));
        constexpr int pre_advance_shift = 1;
        commit.probeFuturePrepare(future_cycle,
                future_backward_slot(iew_to_commit_offset +
                                     pre_advance_shift),
                future_rename_slot(rename_to_commit_offset +
                                   pre_advance_shift),
                future_iew_slot(iew_to_commit_offset + pre_advance_shift));
        future_commit_probe_submitted = true;
    };

    bool future_wavefront_prepare_submitted = false;
    auto submit_future_commit_iew_wavefront_probe = [&]
    {
        if (!taskRuntime.enabled() || !future_prepare_allowed ||
            future_wavefront_prepare_submitted) {
            return;
        }

        const Cycles future_cycle = cycle + Cycles(1);
        const int commit_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToIEWDelay));
        const int iew_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToCommitDelay));
        const int rename_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToCommitDelay));
        const int rename_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToIEWDelay));
        constexpr int pre_advance_shift = 1;

        const TimeStruct *future_backward =
            future_backward_slot(iew_to_commit_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_iew =
            future_backward_slot(commit_to_iew_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_commit =
            future_rename_slot(rename_to_commit_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_iew =
            future_rename_slot(rename_to_iew_offset + pre_advance_shift);
        const IEWStruct *future_iew_to_commit =
            future_iew_slot(iew_to_commit_offset + pre_advance_shift);

        auto result = std::make_shared<PendingFutureWavefrontPrepare>();
        taskRuntime.recordFutureWavefrontPrepareProbe();
        iew.recordFuturePrepareProbe();
        future_wavefront_prepare_submitted = true;

        StallSignalLatch commit_to_iew;
        if (!commit.previewFutureIEWLatch(
                    future_cycle, future_backward, future_rename_to_commit,
                    future_iew_to_commit, commit_to_iew)) {
            record_future_iew_wavefront_skip(
                    FutureWavefrontSkipReason::CommitPreview);
            return;
        }

        auto iew_input = std::make_shared<IEW::IEWPrepareInput>();
        if (!iew.buildFutureRenameLatchInput(
                    future_cycle, commit_to_iew, future_rename_to_iew,
                    future_commit_to_iew, *iew_input)) {
            record_future_iew_wavefront_skip(
                    FutureWavefrontSkipReason::IEWInput);
            return;
        }

        taskRuntime.submitWeak(
                {future_cycle, TaskStage::IEW, 2, InvalidThreadID, 0},
                std::max(1u, static_cast<unsigned>(numThreads) * 2),
                [this, result, future_cycle, commit_to_iew, iew_input] {
                    result->cycle = future_cycle;
                    result->commitToIEW = commit_to_iew;
                    result->iewPrepare =
                        iew.previewFuturePrepare(*iew_input);
                    result->valid = true;
                },
                [this, result, record_future_iew_wavefront_skip] {
                    if (result->valid) {
                        pendingFutureWavefrontPrepare = *result;
                        taskRuntime.recordFutureWavefrontPrepareMerge();
                        iew.setPendingFuturePrepare(result->iewPrepare);
                    } else {
                        if (result->valid)
                            taskRuntime.recordSpecTaskDiscarded();
                        record_future_iew_wavefront_skip(
                                FutureWavefrontSkipReason::IEWPreview);
                    }
                },
                TaskLifetime::CrossTimeBufferAdvance);
    };

    bool future_rename_wavefront_prepare_submitted = false;
    auto submit_future_commit_iew_rename_wavefront_probe = [&]
    {
        if (!taskRuntime.enabled() || !future_prepare_allowed ||
            future_rename_wavefront_prepare_submitted) {
            return;
        }

        const Cycles future_cycle = cycle + Cycles(1);
        const int commit_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToIEWDelay));
        const int iew_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToCommitDelay));
        const int rename_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToCommitDelay));
        const int rename_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToIEWDelay));
        const int decode_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphDecodeToRenameDelay));
        const int iew_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToRenameDelay));
        const int commit_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToRenameDelay));
        constexpr int pre_advance_shift = 1;

        const TimeStruct *future_commit_backward =
            future_backward_slot(iew_to_commit_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_iew =
            future_backward_slot(commit_to_iew_offset + pre_advance_shift);
        const TimeStruct *future_iew_to_rename =
            future_backward_slot(iew_to_rename_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_rename =
            future_backward_slot(commit_to_rename_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_commit =
            future_rename_slot(rename_to_commit_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_iew =
            future_rename_slot(rename_to_iew_offset + pre_advance_shift);
        const IEWStruct *future_iew_to_commit =
            future_iew_slot(iew_to_commit_offset + pre_advance_shift);
        const DecodeStruct *future_decode_to_rename =
            future_decode_slot(decode_to_rename_offset + pre_advance_shift);

        auto result = std::make_shared<
                PendingFutureRenameWavefrontPrepare>();
        taskRuntime.recordFutureRenameWavefrontPrepareProbe();
        rename.recordFuturePrepareProbe();
        future_rename_wavefront_prepare_submitted = true;

        StallSignalLatch commit_to_iew;
        if (!commit.previewFutureIEWLatch(
                    future_cycle, future_commit_backward,
                    future_rename_to_commit, future_iew_to_commit,
                    commit_to_iew)) {
            record_future_rename_wavefront_skip(
                    FutureWavefrontSkipReason::CommitPreview);
            return;
        }

        IEW::IEWPrepareInput iew_input;
        if (!iew.buildFutureRenameLatchInput(
                    future_cycle, commit_to_iew, future_rename_to_iew,
                    future_commit_to_iew, iew_input)) {
            record_future_rename_wavefront_skip(
                    FutureWavefrontSkipReason::IEWInput);
            return;
        }

        StallSignalLatch iew_to_rename;
        IEW::IEWPrepareResult iew_prepare;
        IEW::FutureActiveDispatchPreviewOutcome iew_dispatch_outcome =
            IEW::FutureActiveDispatchPreviewOutcome::NumOutcomes;
        IEW::FutureActiveDispatchPreviewBlockReason iew_dispatch_block =
            IEW::FutureActiveDispatchPreviewBlockReason::NumReasons;
        IEW::FutureDispatchCandidateProfile iew_dispatch_profile;
        if (!iew.previewFutureRenameLatch(
                    iew_input, future_rename_to_iew, future_commit_to_iew,
                    iew_to_rename, &iew_prepare, &iew_dispatch_outcome,
                    &iew_dispatch_block, &iew_dispatch_profile)) {
            record_future_rename_candidate_prepare(
                    future_cycle, iew_to_rename, future_decode_to_rename,
                    future_iew_to_rename, future_commit_to_rename,
                    iew_dispatch_profile);
            iew.recordFutureActiveDispatchPreviewSkipped(
                    iew_input, iew_prepare, iew_dispatch_block);
            record_future_rename_wavefront_skip(
                    FutureWavefrontSkipReason::IEWPreview);
            return;
        }
        iew.recordFutureActiveDispatchPreviewAccepted(
                iew_input, iew_prepare, iew_dispatch_outcome);

        auto rename_input = std::make_shared<Rename::RenamePrepareInput>();
        if (!rename.buildFutureDecodeLatchInput(
                    future_cycle, iew_to_rename, future_decode_to_rename,
                    future_iew_to_rename, future_commit_to_rename,
                    *rename_input)) {
            record_future_rename_wavefront_skip(
                    FutureWavefrontSkipReason::RenameInput);
            return;
        }

        taskRuntime.submitWeak(
                {future_cycle, TaskStage::Rename, 2, InvalidThreadID, 0},
                std::max(1u, static_cast<unsigned>(numThreads) * 3),
                [this, result, future_cycle, iew_to_rename, rename_input] {
                    result->cycle = future_cycle;
                    if (!rename.previewFutureDecodeLatch(
                                *rename_input, result->renameToDecode,
                                &result->renamePrepare)) {
                        return;
                    }
                    result->valid = true;
                },
                [this, result, record_future_rename_wavefront_skip] {
                    if (result->valid) {
                        pendingFutureRenameWavefrontPrepare = *result;
                        taskRuntime.recordFutureRenameWavefrontPrepareMerge();
                        rename.setPendingFuturePrepare(
                                result->renamePrepare);
                    } else {
                        rename.recordFuturePreviewSkipped(
                                result->renamePrepare);
                        if (result->valid)
                            taskRuntime.recordSpecTaskDiscarded();
                        record_future_rename_wavefront_skip(
                                FutureWavefrontSkipReason::RenamePreview);
                    }
                },
                TaskLifetime::CrossTimeBufferAdvance);
    };

    bool future_decode_wavefront_prepare_submitted = false;
    auto submit_future_commit_iew_rename_decode_wavefront_probe = [&]
    {
        if (!taskRuntime.enabled() || !future_prepare_allowed ||
            future_decode_wavefront_prepare_submitted) {
            return;
        }

        const Cycles future_cycle = cycle + Cycles(1);
        const int commit_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToIEWDelay));
        const int iew_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToCommitDelay));
        const int rename_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToCommitDelay));
        const int rename_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToIEWDelay));
        const int decode_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphDecodeToRenameDelay));
        const int iew_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToRenameDelay));
        const int commit_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToRenameDelay));
        const int fetch_to_decode_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphFetchToDecodeDelay));
        const int commit_to_decode_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToDecodeDelay));
        constexpr int pre_advance_shift = 1;

        const TimeStruct *future_commit_backward =
            future_backward_slot(iew_to_commit_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_iew =
            future_backward_slot(commit_to_iew_offset + pre_advance_shift);
        const TimeStruct *future_iew_to_rename =
            future_backward_slot(iew_to_rename_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_rename =
            future_backward_slot(commit_to_rename_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_decode =
            future_backward_slot(commit_to_decode_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_commit =
            future_rename_slot(rename_to_commit_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_iew =
            future_rename_slot(rename_to_iew_offset + pre_advance_shift);
        const IEWStruct *future_iew_to_commit =
            future_iew_slot(iew_to_commit_offset + pre_advance_shift);
        const DecodeStruct *future_decode_to_rename =
            future_decode_slot(decode_to_rename_offset + pre_advance_shift);
        const FetchStruct *future_fetch_to_decode =
            future_fetch_slot(fetch_to_decode_offset + pre_advance_shift);

        auto result = std::make_shared<
                PendingFutureDecodeWavefrontPrepare>();
        taskRuntime.recordFutureDecodeWavefrontPrepareProbe();
        decode.recordFuturePrepareProbe();
        future_decode_wavefront_prepare_submitted = true;

        StallSignalLatch commit_to_iew;
        if (!commit.previewFutureIEWLatch(
                    future_cycle, future_commit_backward,
                    future_rename_to_commit, future_iew_to_commit,
                    commit_to_iew)) {
            record_future_decode_wavefront_skip(
                    FutureWavefrontSkipReason::CommitPreview);
            return;
        }

        IEW::IEWPrepareInput iew_input;
        if (!iew.buildFutureRenameLatchInput(
                    future_cycle, commit_to_iew, future_rename_to_iew,
                    future_commit_to_iew, iew_input)) {
            record_future_decode_wavefront_skip(
                    FutureWavefrontSkipReason::IEWInput);
            return;
        }

        StallSignalLatch iew_to_rename;
        IEW::IEWPrepareResult iew_prepare;
        IEW::FutureActiveDispatchPreviewOutcome iew_dispatch_outcome =
            IEW::FutureActiveDispatchPreviewOutcome::NumOutcomes;
        IEW::FutureActiveDispatchPreviewBlockReason iew_dispatch_block =
            IEW::FutureActiveDispatchPreviewBlockReason::NumReasons;
        IEW::FutureDispatchCandidateProfile iew_dispatch_profile;
        if (!iew.previewFutureRenameLatch(
                    iew_input, future_rename_to_iew, future_commit_to_iew,
                    iew_to_rename, &iew_prepare, &iew_dispatch_outcome,
                    &iew_dispatch_block, &iew_dispatch_profile)) {
            record_future_rename_candidate_prepare(
                    future_cycle, iew_to_rename, future_decode_to_rename,
                    future_iew_to_rename, future_commit_to_rename,
                    iew_dispatch_profile);
            iew.recordFutureActiveDispatchPreviewSkipped(
                    iew_input, iew_prepare, iew_dispatch_block);
            record_future_decode_wavefront_skip(
                    FutureWavefrontSkipReason::IEWPreview);
            return;
        }
        iew.recordFutureActiveDispatchPreviewAccepted(
                iew_input, iew_prepare, iew_dispatch_outcome);

        Rename::RenamePrepareInput rename_input;
        if (!rename.buildFutureDecodeLatchInput(
                    future_cycle, iew_to_rename, future_decode_to_rename,
                    future_iew_to_rename, future_commit_to_rename,
                    rename_input)) {
            record_future_decode_wavefront_skip(
                    FutureWavefrontSkipReason::RenameInput);
            return;
        }

        StallSignalLatch rename_to_decode;
        Rename::RenamePrepareResult rename_prepare;
        if (!rename.previewFutureDecodeLatch(
                    rename_input, rename_to_decode, &rename_prepare)) {
            rename.recordFuturePreviewSkipped(rename_prepare);
            record_future_decode_wavefront_skip(
                    FutureWavefrontSkipReason::RenamePreview);
            return;
        }

        auto decode_input = std::make_shared<Decode::DecodePrepareInput>();
        if (!decode.buildFutureFetchLatchInput(
                    future_cycle, rename_to_decode, future_fetch_to_decode,
                    future_commit_to_decode, *decode_input)) {
            record_future_decode_wavefront_skip(
                    FutureWavefrontSkipReason::DecodeInput);
            return;
        }

        taskRuntime.submitWeak(
                {future_cycle, TaskStage::Decode, 2, InvalidThreadID, 0},
                std::max(1u, static_cast<unsigned>(numThreads) * 4),
                [this, result, future_cycle, decode_input] {
                    result->cycle = future_cycle;
                    if (!decode.previewFutureFetchLatch(
                                *decode_input, result->decodeToFetch,
                                &result->decodePrepare)) {
                        return;
                    }
                    result->valid = true;
                },
                [this, result, record_future_decode_wavefront_skip] {
                    if (result->valid) {
                        pendingFutureDecodeWavefrontPrepare = *result;
                        taskRuntime.recordFutureDecodeWavefrontPrepareMerge();
                        decode.setPendingFuturePrepare(
                                result->decodePrepare);
                    } else {
                        decode.recordFuturePreviewSkipped(
                                result->decodePrepare);
                        if (result->valid)
                            taskRuntime.recordSpecTaskDiscarded();
                        record_future_decode_wavefront_skip(
                                FutureWavefrontSkipReason::DecodePreview);
                    }
                },
                TaskLifetime::CrossTimeBufferAdvance);
    };

    bool future_fetch_wavefront_prepare_submitted = false;
    auto submit_future_commit_iew_rename_decode_fetch_wavefront_probe = [&]
    {
        if (!taskRuntime.enabled() || !future_prepare_allowed ||
            future_fetch_wavefront_prepare_submitted) {
            return;
        }

        const Cycles future_cycle = cycle + Cycles(1);
        const int commit_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToIEWDelay));
        const int iew_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToCommitDelay));
        const int rename_to_commit_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToCommitDelay));
        const int rename_to_iew_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphRenameToIEWDelay));
        const int decode_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphDecodeToRenameDelay));
        const int iew_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphIEWToRenameDelay));
        const int commit_to_rename_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToRenameDelay));
        const int fetch_to_decode_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphFetchToDecodeDelay));
        const int decode_to_fetch_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphDecodeToFetchDelay));
        const int commit_to_decode_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToDecodeDelay));
        const int commit_to_fetch_offset = -static_cast<int>(
                static_cast<uint64_t>(taskGraphCommitToFetchDelay));
        constexpr int pre_advance_shift = 1;

        const TimeStruct *future_commit_backward =
            future_backward_slot(iew_to_commit_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_iew =
            future_backward_slot(commit_to_iew_offset + pre_advance_shift);
        const TimeStruct *future_iew_to_rename =
            future_backward_slot(iew_to_rename_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_rename =
            future_backward_slot(commit_to_rename_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_decode =
            future_backward_slot(commit_to_decode_offset + pre_advance_shift);
        const TimeStruct *future_decode_to_fetch =
            future_backward_slot(decode_to_fetch_offset + pre_advance_shift);
        const TimeStruct *future_commit_to_fetch =
            future_backward_slot(commit_to_fetch_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_commit =
            future_rename_slot(rename_to_commit_offset + pre_advance_shift);
        const RenameStruct *future_rename_to_iew =
            future_rename_slot(rename_to_iew_offset + pre_advance_shift);
        const IEWStruct *future_iew_to_commit =
            future_iew_slot(iew_to_commit_offset + pre_advance_shift);
        const DecodeStruct *future_decode_to_rename =
            future_decode_slot(decode_to_rename_offset + pre_advance_shift);
        const FetchStruct *future_fetch_to_decode =
            future_fetch_slot(fetch_to_decode_offset + pre_advance_shift);

        taskRuntime.recordFutureFetchWavefrontPrepareProbe();
        fetch.recordFutureToDecodePrepareProbe();
        future_fetch_wavefront_prepare_submitted = true;

        StallSignalLatch commit_to_iew;
        if (!commit.previewFutureIEWLatch(
                    future_cycle, future_commit_backward,
                    future_rename_to_commit, future_iew_to_commit,
                    commit_to_iew)) {
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::CommitPreview);
            return;
        }

        IEW::IEWPrepareInput iew_input;
        if (!iew.buildFutureRenameLatchInput(
                    future_cycle, commit_to_iew, future_rename_to_iew,
                    future_commit_to_iew, iew_input)) {
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::IEWInput);
            return;
        }

        StallSignalLatch iew_to_rename;
        IEW::IEWPrepareResult iew_prepare;
        IEW::FutureActiveDispatchPreviewOutcome iew_dispatch_outcome =
            IEW::FutureActiveDispatchPreviewOutcome::NumOutcomes;
        IEW::FutureActiveDispatchPreviewBlockReason iew_dispatch_block =
            IEW::FutureActiveDispatchPreviewBlockReason::NumReasons;
        IEW::FutureDispatchCandidateProfile iew_dispatch_profile;
        if (!iew.previewFutureRenameLatch(
                    iew_input, future_rename_to_iew, future_commit_to_iew,
                    iew_to_rename, &iew_prepare, &iew_dispatch_outcome,
                    &iew_dispatch_block, &iew_dispatch_profile)) {
            record_future_rename_candidate_prepare(
                    future_cycle, iew_to_rename, future_decode_to_rename,
                    future_iew_to_rename, future_commit_to_rename,
                    iew_dispatch_profile);
            iew.recordFutureActiveDispatchPreviewSkipped(
                    iew_input, iew_prepare, iew_dispatch_block);
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::IEWPreview);
            return;
        }
        iew.recordFutureActiveDispatchPreviewAccepted(
                iew_input, iew_prepare, iew_dispatch_outcome);

        Rename::RenamePrepareInput rename_input;
        if (!rename.buildFutureDecodeLatchInput(
                    future_cycle, iew_to_rename, future_decode_to_rename,
                    future_iew_to_rename, future_commit_to_rename,
                    rename_input)) {
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::RenameInput);
            return;
        }

        StallSignalLatch rename_to_decode;
        Rename::RenamePrepareResult rename_prepare;
        if (!rename.previewFutureDecodeLatch(
                    rename_input, rename_to_decode, &rename_prepare)) {
            rename.recordFuturePreviewSkipped(rename_prepare);
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::RenamePreview);
            return;
        }

        Decode::DecodePrepareInput decode_input;
        if (!decode.buildFutureFetchLatchInput(
                    future_cycle, rename_to_decode, future_fetch_to_decode,
                    future_commit_to_decode, decode_input)) {
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::DecodeInput);
            return;
        }

        StallSignalLatch decode_to_fetch;
        Decode::DecodePrepareResult decode_prepare;
        if (!decode.previewFutureFetchLatch(
                    decode_input, decode_to_fetch, &decode_prepare)) {
            decode.recordFuturePreviewSkipped(decode_prepare);
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::DecodePreview);
            return;
        }

        auto fetch_input =
            std::make_shared<Fetch::FutureDecodeQueueInput>();
        Fetch::FutureDecodeQueueInputSkipInfo fetch_input_skip;
        if (!fetch.buildFutureDecodeQueueInput(
                    future_cycle, decode_to_fetch, future_decode_to_fetch,
                    future_commit_to_fetch, *fetch_input,
                    &fetch_input_skip)) {
            fetch.recordFutureDecodeQueueInputSkipped(fetch_input_skip);
            record_future_fetch_wavefront_skip(
                    FutureWavefrontSkipReason::FetchInput);
            return;
        }
        fetch.recordFutureDecodeQueueInputAccepted(*fetch_input);

        auto result = std::make_shared<
                PendingFutureFetchWavefrontPrepare>();
        taskRuntime.submitWeak(
                {future_cycle, TaskStage::Fetch, 4, InvalidThreadID, 0},
                std::max(1u, static_cast<unsigned>(numThreads) * 5),
                [this, result, future_cycle, fetch_input] {
                    result->cycle = future_cycle;
                    if (!fetch.previewFutureDecodeQueue(
                                *fetch_input, result->size,
                                result->fetchStallReason,
                                result->instSeqNums,
                                &result->fetchToDecodePrepare)) {
                        return;
                    }
                    result->valid = true;
                },
                [this, result, record_future_fetch_wavefront_skip] {
                    if (result->valid) {
                        pendingFutureFetchWavefrontPrepare = *result;
                        taskRuntime.recordFutureFetchWavefrontPrepareMerge();
                        fetch.setPendingFutureToDecodePrepare(
                                result->fetchToDecodePrepare);
                    } else {
                        if (result->valid)
                            taskRuntime.recordSpecTaskDiscarded();
                        record_future_fetch_wavefront_skip(
                                FutureWavefrontSkipReason::FetchPreview);
                    }
                },
                TaskLifetime::CrossTimeBufferAdvance);
    };

    bool future_timebuffer_prepare_submitted = false;
    bool future_timebuffer_prepare_merged = false;
    unsigned future_timebuffer_prepare_slots = 0;
    auto future_timebuffer_prepare_summary = std::make_shared<
            PipelineTimeBufferSnapshots::PrepareSummary>();
    auto submit_future_timebuffer_prepare = [&]
    {
        if (!taskRuntime.enabled() || !timebuffer_prepare_enabled ||
            !future_prepare_allowed ||
            future_timebuffer_prepare_submitted) {
            return;
        }

        const Cycles future_cycle = cycle + Cycles(1);
        PipelineTimeBufferSnapshots::Frame frame;
        constexpr int pre_advance_shift = 1;
        future_timebuffer_prepare_slots = frame.captureShifted(
                future_cycle, timeBuffer, fetchTimebuffer, decodeTimebuffer,
                renameTimebuffer, iewTimebuffer, pre_advance_shift);
        *future_timebuffer_prepare_summary =
            PipelineTimeBufferSnapshots::summarizeFrame(frame);
        future_timebuffer_prepare_submitted = true;

        taskRuntime.submitWeak(
                {future_cycle, TaskStage::Runtime, 2, InvalidThreadID, 0},
                future_timebuffer_prepare_slots,
                [] {},
                [&future_timebuffer_prepare_merged] {
                    future_timebuffer_prepare_merged = true;
                });
    };

    stallSignalBank.beginCycle(cycle);
    if (taskRuntime.traceEnabled()) {
        DPRINTF(TaskGraph,
                "Stall signal bank publish cycle=%llu valid=%i\n",
                cycle, stallSignalBank.valid());
    }

    auto wait_future_stage_prepare =
        [this, cycle](TaskStage stage, uint8_t phase)
    {
        if (!taskRuntime.enabled())
            return;

        taskRuntime.waitForOrder(
                {cycle, stage, phase, InvalidThreadID,
                 std::numeric_limits<uint64_t>::max()});
    };

    //Tick each of the stages
    wait_future_stage_prepare(TaskStage::Commit, 2);
    taskRuntime.runStrong({cycle, TaskStage::Commit, 0, InvalidThreadID, 0},
            [this] { commit.tick(); });
    wait_future_stage_prepare(TaskStage::IEW, 2);
    taskRuntime.runStrong({cycle, TaskStage::IEW, 0, InvalidThreadID, 0},
            [this] { iew.tick(); });
    wait_future_stage_prepare(TaskStage::Rename, 2);
    taskRuntime.runStrong({cycle, TaskStage::Rename, 0, InvalidThreadID, 0},
            [this] { rename.tick(); });
    submit_future_commit_probe();
    submit_future_commit_iew_wavefront_probe();
    wait_future_stage_prepare(TaskStage::Decode, 2);
    taskRuntime.runStrong({cycle, TaskStage::Decode, 0, InvalidThreadID, 0},
            [this] { decode.tick(); });
    submit_future_commit_iew_rename_wavefront_probe();
    wait_future_stage_prepare(TaskStage::Fetch, 4);
    taskRuntime.runStrong({cycle, TaskStage::Fetch, 0, InvalidThreadID, 0},
            [this] { fetch.tick(); });
    submit_future_commit_iew_rename_decode_wavefront_probe();
    submit_future_commit_iew_rename_decode_fetch_wavefront_probe();

    stallSignalBank.endCycle(cycle);
    checkFutureWavefrontPrepare(cycle);
    checkFutureRenameWavefrontPrepare(cycle);
    checkFutureDecodeWavefrontPrepare(cycle);
    checkFutureFetchWavefrontPrepare(cycle);
    taskRuntime.recordStallSignalWindow(cycle,
            stallSignalBank.windowCapacity(),
            stallSignalBank.validWindowSlots(),
            stallSignalBank.edgeCount());
    if (taskRuntime.traceEnabled()) {
        DPRINTF(TaskGraph,
                "Stall signal bank capture cycle=%llu valid=%i\n",
                cycle, stallSignalBank.valid());
    }
    if (taskRuntime.enabled()) {
        const unsigned slots = pipelineSnapshots.captureOutputs(
                cycle, timeBuffer, fetchTimebuffer, decodeTimebuffer,
                renameTimebuffer, iewTimebuffer);
        taskRuntime.recordTimeBufferSnapshot(false, slots);
        taskRuntime.recordTimeBufferSnapshotWindow(
                pipelineSnapshots.windowCapacity(),
                pipelineSnapshots.validInputFrames(),
                pipelineSnapshots.validOutputFrames());
        if (taskRuntime.traceEnabled()) {
            DPRINTF(TaskGraph,
                    "TimeBuffer output snapshot cycle=%llu slots=%u\n",
                    cycle, slots);
        }
    }
    submit_future_timebuffer_prepare();

    // Future probes read current TimeBuffer slots; finish them before the
    // circular buffers advance and invalidate those slot addresses.
    if (taskRuntime.enabled()) {
        const auto pending_tasks = taskRuntime.pendingTaskCount();
        const auto pending_pre_advance_tasks =
            taskRuntime.pendingPreAdvanceTaskCount();
        taskRuntime.recordTimeBufferAdvanceWait(
                cycle, pending_pre_advance_tasks,
                pending_tasks - pending_pre_advance_tasks);
        taskRuntime.waitForPreAdvance();
    }

    fetchTimebuffer.advance();
    decodeTimebuffer.advance();
    renameTimebuffer.advance();
    iewTimebuffer.advance();
    timeBuffer.advance();

    activityRec.advance();

    if (removeInstsThisCycle) {
        cleanUpRemovedInsts();
    }

    if (!tickEvent.scheduled()) {
        if (_status == SwitchedOut) {
            DPRINTF(O3CPU, "Switched out!\n");
            // increment stat
            lastRunningCycle = curCycle();
        } else if (!activityRec.active() || _status == Idle) {
            DPRINTF(O3CPU, "Idle!\n");
            lastRunningCycle = curCycle();
            cpuStats.timesIdled++;
        } else {
            lastRunningCycle = curCycle();
            schedule(tickEvent, clockEdge(Cycles(1)));
            DPRINTF(O3CPU, "Scheduling next tick!\n");
        }
    }

    if (!FullSystem)
        updateThreadPriority();

    tryDrain();

    if (taskRuntime.enabled() && timebuffer_prepare_enabled) {
        if (future_timebuffer_prepare_submitted)
            taskRuntime.waitForAll();

        if (future_prepare_allowed && tickEvent.scheduled() &&
            future_timebuffer_prepare_submitted) {
            assert(future_timebuffer_prepare_merged);
            const auto &summary = *future_timebuffer_prepare_summary;
            const uint64_t control_signals =
                summary.squashSignals +
                summary.robSquashingSignals +
                summary.branchMispredictSignals;
            taskRuntime.recordFutureTimeBufferSnapshot(
                    future_timebuffer_prepare_slots);
            taskRuntime.recordFutureTimeBufferPrepareMerge(
                    summary.forwardInstRefs,
                    control_signals,
                    summary.resolvedCFIs);
            pendingFutureTimeBufferPrepare.summary = summary;
            pendingFutureTimeBufferPrepare.valid = true;
            if (taskRuntime.traceEnabled()) {
                DPRINTF(TaskGraph,
                        "Future TimeBuffer prepare merge "
                        "cycle=%llu instRefs=%llu control=%llu "
                        "resolvedCFIs=%llu fetchGroups=%llu\n",
                        summary.cycle,
                        summary.forwardInstRefs,
                        control_signals,
                        summary.resolvedCFIs,
                        summary.fetchGroups);
            }
        } else {
            if (future_timebuffer_prepare_submitted)
                taskRuntime.recordSpecTaskDiscarded();
            taskRuntime.recordFutureTimeBufferPrepareSkipped();
        }
    }

    bool defer_safe_tasks_to_next_tick = false;
    if (taskRuntime.enabled() && tickEvent.scheduled()) {
        const Event *next_event = eventQueue()->getHead();
        defer_safe_tasks_to_next_tick = next_event == &tickEvent;
    }
    taskRuntime.onSerialTickEnd(curCycle(), defer_safe_tasks_to_next_tick);
}

void
CPU::init()
{
    BaseCPU::init();

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        // Set noSquashFromTC so that the CPU doesn't squash when initially
        // setting up registers.
        thread[tid]->noSquashFromTC = true;
    }

    // Clear noSquashFromTC.
    for (int tid = 0; tid < numThreads; ++tid)
        thread[tid]->noSquashFromTC = false;

    commit.setThreads(thread);
}

void
CPU::startup()
{
    BaseCPU::startup();

    fetch.startupStage();
    decode.startupStage();
    iew.startupStage();
    rename.startupStage();
    commit.startupStage();
}

void
CPU::activateThread(ThreadID tid)
{
    std::list<ThreadID>::iterator isActive =
        std::find(activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling activate thread.\n", tid);
    assert(!switchedOut());

    if (isActive == activeThreads.end()) {
        DPRINTF(O3CPU, "[tid:%i] Adding to active threads list\n", tid);

        activeThreads.push_back(tid);
    }
}

void
CPU::deactivateThread(ThreadID tid)
{
    // hardware transactional memory
    // shouldn't deactivate thread in the middle of a transaction
    assert(!commit.executingHtmTransaction(tid));

    //Remove From Active List, if Active
    std::list<ThreadID>::iterator thread_it =
        std::find(activeThreads.begin(), activeThreads.end(), tid);

    DPRINTF(O3CPU, "[tid:%i] Calling deactivate thread.\n", tid);
    assert(!switchedOut());

    if (thread_it != activeThreads.end()) {
        DPRINTF(O3CPU,"[tid:%i] Removing from active threads list\n",
                tid);
        activeThreads.erase(thread_it);
    }

    fetch.deactivateThread(tid);
    commit.deactivateThread(tid);
}

Counter
CPU::totalInsts() const
{
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numInst;

    return total;
}

Counter
CPU::totalOps() const
{
    Counter total(0);

    ThreadID size = thread.size();
    for (ThreadID i = 0; i < size; i++)
        total += thread[i]->numOp;

    return total;
}

void
CPU::activateContext(ThreadID tid)
{
    assert(!switchedOut());

    // Needs to set each stage to running as well.
    activateThread(tid);

    // We don't want to wake the CPU if it is drained. In that case,
    // we just want to flag the thread as active and schedule the tick
    // event from drainResume() instead.
    if (drainState() == DrainState::Drained)
        return;

    // If we are time 0 or if the last activation time is in the past,
    // schedule the next tick and wake up the fetch unit
    if (lastActivatedCycle == 0 || lastActivatedCycle < curTick()) {
        scheduleTickEvent(Cycles(0));

        // Be sure to signal that there's some activity so the CPU doesn't
        // deschedule itself.
        activityRec.activity();
        fetch.wakeFromQuiesce();

        Cycles cycles(curCycle() - lastRunningCycle);
        // @todo: This is an oddity that is only here to match the stats
        if (cycles != 0)
            --cycles;
        cpuStats.quiesceCycles += cycles;

        lastActivatedCycle = curTick();

        _status = Running;

        BaseCPU::activateContext(tid);
    }
}

void
CPU::suspendContext(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Suspending Thread Context.\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        unscheduleTickEvent();
        lastRunningCycle = curCycle();
        _status = Idle;
    }

    DPRINTF(Quiesce, "Suspending Context\n");

    BaseCPU::suspendContext(tid);
}

void
CPU::haltContext(ThreadID tid)
{
    //For now, this is the same as deallocate
    DPRINTF(O3CPU,"[tid:%i] Halt Context called. Deallocating\n", tid);
    assert(!switchedOut());

    deactivateThread(tid);
    removeThread(tid);

    // If this was the last thread then unschedule the tick event.
    if (activeThreads.size() == 0) {
        if (tickEvent.scheduled())
        {
            unscheduleTickEvent();
        }
        lastRunningCycle = curCycle();
        _status = Idle;
    }
    updateCycleCounters(BaseCPU::CPU_STATE_SLEEP);
}

void
CPU::insertThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Initializing thread into CPU");
    // Will change now that the PC and thread state is internal to the CPU
    // and not in the ThreadContext.
    gem5::ThreadContext *src_tc;
    if (FullSystem)
        src_tc = system->threads[tid];
    else
        src_tc = tcBase(tid);

    //Bind Int Regs to Rename Map
    const auto &regClasses = isa[tid]->regClasses();

    for (auto type = (RegClassType)0; type <= RMiscRegClass;
            type = (RegClassType)(type + 1)) {
        for (RegIndex idx = 0; idx < regClasses.at(type).numRegs(); idx++) {
            PhysRegIdPtr phys_reg = freeList.getReg(type);
            renameMap[tid].setEntry(RegId(type, idx), VirtRegId(phys_reg));
            scoreboard.setReg(phys_reg);
        }
    }

    //Copy Thread Data Into RegFile
    //copyFromTC(tid);

    //Set PC/NPC/NNPC
    pcState(src_tc->pcState(), tid);

    src_tc->setStatus(gem5::ThreadContext::Active);

    activateContext(tid);

    //Reset ROB/IQ/LSQ Entries
    commit.rob->resetEntries();
}

void
CPU::removeThread(ThreadID tid)
{
    DPRINTF(O3CPU,"[tid:%i] Removing thread context from CPU.\n", tid);

    // Copy Thread Data From RegFile
    // If thread is suspended, it might be re-allocated
    // copyToTC(tid);


    // @todo: 2-27-2008: Fix how we free up rename mappings
    // here to alleviate the case for double-freeing registers
    // in SMT workloads.

    // clear all thread-specific states in each stage of the pipeline
    // since this thread is going to be completely removed from the CPU
    commit.clearStates(tid);
    fetch.clearStates(tid);
    decode.clearStates(tid);
    rename.clearStates(tid);
    iew.clearStates(tid);

    // Flush out any old data from the time buffers.
    for (int i = 0; i < timeBuffer.getSize(); ++i) {
        timeBuffer.advance();
        fetchTimebuffer.advance();
        decodeTimebuffer.advance();
        renameTimebuffer.advance();
        iewTimebuffer.advance();
    }

    assert(iew.ldstQueue.getCount(tid) == 0);
    assert(commit.rob->isEmpty(tid));

    // Reset ROB/IQ/LSQ Entries

    // Commented out for now.  This should be possible to do by
    // telling all the pipeline stages to drain first, and then
    // checking until the drain completes.  Once the pipeline is
    // drained, call resetEntries(). - 10-09-06 ktlim
/*
    if (activeThreads.size() >= 1) {
        commit.rob->resetEntries();
        iew.resetEntries();
    }
*/
}

Fault
CPU::getInterrupts()
{
    // Check if there are any outstanding interrupts
    return interrupts[0]->getInterrupt();
}

int
CPU::getInterruptsNO()
{
    // Check if there are any outstanding interrupts
    return interrupts[0]->getInterruptNO();
}

void
CPU::processInterrupts(const Fault &interrupt)
{
    // Check for interrupts here.  For now can copy the code that
    // exists within isa_fullsys_traits.hh.  Also assume that thread 0
    // is the one that handles the interrupts.
    // @todo: Possibly consolidate the interrupt checking code.
    // @todo: Allow other threads to handle interrupts.

    assert(interrupt != NoFault);
    interrupts[0]->updateIntrInfo();

    DPRINTF(O3CPU, "Interrupt %s being handled\n", interrupt->name());
    trap(interrupt, 0, nullptr);
}

void
CPU::trap(const Fault &fault, ThreadID tid, const StaticInstPtr &inst)
{
    // Pass the thread's TC into the invoke method.
    fault->invoke(threadContexts[tid], inst);
}

void
CPU::serializeThread(CheckpointOut &cp, ThreadID tid) const
{
    thread[tid]->serialize(cp);
}

void
CPU::unserializeThread(CheckpointIn &cp, ThreadID tid)
{
    thread[tid]->unserialize(cp);
}

DrainState
CPU::drain()
{
    // Deschedule any power gating event (if any)
    deschedulePowerGatingEvent();

    // If the CPU isn't doing anything, then return immediately.
    if (switchedOut())
        return DrainState::Drained;

    DPRINTF(Drain, "Draining...\n");

    // We only need to signal a drain to the commit stage as this
    // initiates squashing controls the draining. Once the commit
    // stage commits an instruction where it is safe to stop, it'll
    // squash the rest of the instructions in the pipeline and force
    // the fetch stage to stall. The pipeline will be drained once all
    // in-flight instructions have retired.
    commit.drain();

    // Wake the CPU and record activity so everything can drain out if
    // the CPU was not able to immediately drain.
    if (!isCpuDrained())  {
        // If a thread is suspended, wake it up so it can be drained
        for (auto t : threadContexts) {
            if (t->status() == gem5::ThreadContext::Suspended){
                DPRINTF(Drain, "Currently suspended so activate %i \n",
                        t->threadId());
                t->activate();
                // As the thread is now active, change the power state as well
                activateContext(t->threadId());
            }
        }

        wakeCPU();
        activityRec.activity();

        DPRINTF(Drain, "CPU not drained\n");

        return DrainState::Draining;
    } else {
        DPRINTF(Drain, "CPU is already drained\n");
        if (tickEvent.scheduled())
            deschedule(tickEvent);

        // Flush out any old data from the time buffers.  In
        // particular, there might be some data in flight from the
        // fetch stage that isn't visible in any of the CPU buffers we
        // test in isCpuDrained().
        for (int i = 0; i < timeBuffer.getSize(); ++i) {
            timeBuffer.advance();
            fetchTimebuffer.advance();
            decodeTimebuffer.advance();
            renameTimebuffer.advance();
            iewTimebuffer.advance();
        }

        drainSanityCheck();
        return DrainState::Drained;
    }
}

bool
CPU::tryDrain()
{
    if (drainState() != DrainState::Draining || !isCpuDrained())
        return false;

    if (tickEvent.scheduled())
        deschedule(tickEvent);

    DPRINTF(Drain, "CPU done draining, processing drain event\n");
    signalDrainDone();

    return true;
}

void
CPU::drainSanityCheck() const
{
    assert(isCpuDrained());
    fetch.drainSanityCheck();
    decode.drainSanityCheck();
    rename.drainSanityCheck();
    iew.drainSanityCheck();
    commit.drainSanityCheck();
}

bool
CPU::isCpuDrained() const
{
    bool drained(true);

    if (!instList.empty() || !removeList.empty()) {
        DPRINTF(Drain, "Main CPU structures not drained.\n");
        drained = false;
    }

    if (!fetch.isDrained()) {
        DPRINTF(Drain, "Fetch not drained.\n");
        drained = false;
    }

    if (!decode.isDrained()) {
        DPRINTF(Drain, "Decode not drained.\n");
        drained = false;
    }

    if (!rename.isDrained()) {
        DPRINTF(Drain, "Rename not drained.\n");
        drained = false;
    }

    if (!iew.isDrained()) {
        DPRINTF(Drain, "IEW not drained.\n");
        drained = false;
    }

    if (!commit.isDrained()) {
        DPRINTF(Drain, "Commit not drained.\n");
        drained = false;
    }

    return drained;
}

void CPU::commitDrained(ThreadID tid) { fetch.drainStall(tid); }

void
CPU::drainResume()
{
    if (switchedOut())
        return;

    DPRINTF(Drain, "Resuming...\n");
    verifyMemoryMode();

    fetch.drainResume();
    commit.drainResume();

    _status = Idle;
    for (ThreadID i = 0; i < thread.size(); i++) {
        if (thread[i]->status() == gem5::ThreadContext::Active) {
            DPRINTF(Drain, "Activating thread: %i\n", i);
            activateThread(i);
            _status = Running;
        }
    }

    assert(!tickEvent.scheduled());
    if (_status == Running)
        schedule(tickEvent, nextCycle());

    // Reschedule any power gating event (if any)
    schedulePowerGatingEvent();
}

void
CPU::switchOut()
{
    DPRINTF(O3CPU, "Switching out\n");
    BaseCPU::switchOut();

    activityRec.reset();

    _status = SwitchedOut;

    if (checker)
        checker->switchOut();
}

void
CPU::takeOverFrom(BaseCPU *oldCPU)
{
    BaseCPU::takeOverFrom(oldCPU);

    fetch.takeOverFrom();
    decode.takeOverFrom();
    rename.takeOverFrom();
    iew.takeOverFrom();
    commit.takeOverFrom();

    assert(!tickEvent.scheduled());

    auto *oldO3CPU = dynamic_cast<CPU *>(oldCPU);
    if (oldO3CPU)
        globalSeqNum = oldO3CPU->globalSeqNum;

    lastRunningCycle = curCycle();
    _status = Idle;
}

void
CPU::verifyMemoryMode() const
{
    if (!system->isTimingMode()) {
        fatal("The O3 CPU requires the memory system to be in "
              "'timing' mode.\n");
    }
}

RegVal
CPU::readMiscRegNoEffect(int misc_reg, ThreadID tid) const
{
    return isa[tid]->readMiscRegNoEffect(misc_reg);
}

RegVal
CPU::readMiscReg(int misc_reg, ThreadID tid)
{
    cpuStats.miscRegfileReads++;
    return isa[tid]->readMiscReg(misc_reg);
}

void
CPU::setMiscRegNoEffect(int misc_reg, RegVal val, ThreadID tid)
{
    isa[tid]->setMiscRegNoEffect(misc_reg, val);
}

void
CPU::setMiscReg(int misc_reg, RegVal val, ThreadID tid)
{
    cpuStats.miscRegfileWrites++;
    isa[tid]->setMiscReg(misc_reg, val);
}

RegVal
CPU::getReg(PhysRegIdPtr phys_reg)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getReg(phys_reg);
}

RegVal
CPU::getReg(VirtRegId virt_reg)
{
    switch (virt_reg.PhyReg()->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getReg(virt_reg);
}

void
CPU::getReg(PhysRegIdPtr phys_reg, void *val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileReads++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileReads++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileReads++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    regFile.getReg(phys_reg, val);
}

void *
CPU::getWritableReg(PhysRegIdPtr phys_reg)
{
    switch (phys_reg->classValue()) {
      case VecRegClass:
        cpuStats.vecRegfileReads++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileReads++;
        break;
      default:
        break;
    }
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, RegVal val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileWrites++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileWrites++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileWrites++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

void
CPU::setReg(PhysRegIdPtr phys_reg, const void *val)
{
    switch (phys_reg->classValue()) {
      case IntRegClass:
        cpuStats.intRegfileWrites++;
        break;
      case FloatRegClass:
        cpuStats.fpRegfileWrites++;
        break;
      case CCRegClass:
        cpuStats.ccRegfileWrites++;
        break;
      case VecRegClass:
      case VecElemClass:
        cpuStats.vecRegfileWrites++;
        break;
      case VecPredRegClass:
        cpuStats.vecPredRegfileWrites++;
        break;
      default:
        break;
    }
    regFile.setReg(phys_reg, val);
}

RegVal
CPU::getArchReg(const RegId &reg, ThreadID tid)
{
    VirtRegId virt_reg = commitRenameMap[tid].lookup(reg);
    return regFile.getReg(virt_reg);
}

void
CPU::getArchReg(const RegId &reg, void *val, ThreadID tid)
{
    VirtRegId virt_reg = commitRenameMap[tid].lookup(reg);
    assert(!virt_reg.IEOper());
    regFile.getReg(virt_reg.PhyReg(), val);
}

void *
CPU::getWritableArchReg(const RegId &reg, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg).PhyReg();
    return regFile.getWritableReg(phys_reg);
}

void
CPU::setArchReg(const RegId &reg, RegVal val, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg).PhyReg();
    regFile.setReg(phys_reg, val);
}

void
CPU::setArchReg(const RegId &reg, const void *val, ThreadID tid)
{
    PhysRegIdPtr phys_reg = commitRenameMap[tid].lookup(reg).PhyReg();
    regFile.setReg(phys_reg, val);
}

const PCStateBase &
CPU::pcState(ThreadID tid)
{
    return commit.pcState(tid);
}

void
CPU::pcState(const PCStateBase &val, ThreadID tid)
{
    commit.pcState(val, tid);
}

void
CPU::squashFromTC(ThreadID tid)
{
    thread[tid]->noSquashFromTC = true;
    commit.generateTCEvent(tid);
}

CPU::ListIt
CPU::addInst(const DynInstPtr &inst)
{
    instList.push_back(inst);

    return --(instList.end());
}

void
CPU::instDone(ThreadID tid, const DynInstPtr &inst)
{
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        thread[tid]->numInst++;
        thread[tid]->threadStats.numInsts++;
        cpuStats.committedInsts[tid]++;
        ipc_r++;
        cpi_r.roll(1);
        if (inst->staticInst->isFusion()) {
            thread[tid]->numInst++;
            thread[tid]->threadStats.numInsts++;
            cpuStats.committedInsts[tid]++;
            ipc_r++;
            cpi_r.roll(1);
        }

        uint64_t committedInsts = totalInsts();

        if (this->nextDumpInstCount && !dump_done
                && committedInsts >= this->nextDumpInstCount) {
            fprintf(stderr, "Will trigger stat dump and reset\n");
            statistics::schedStatEvent(true, true, curTick(), 0);
            scheduleInstStop(tid,0,"Will trigger stat dump and reset");
            dump_done = true;

            /*if (this->repeatDumpInstCount) {
                this->nextDumpInstCount += this->repeatDumpInstCount;
            };*/
        }

        // Check for instruction-count-based events.
        thread[tid]->comInstEventQueue.serviceEvents(thread[tid]->numInst);

        if (this->warmupInstCount && !warmup_done && committedInsts >= this->warmupInstCount) {
            fprintf(stderr, "Will trigger stat dump and reset\n");
            statistics::schedStatEvent(true, true, curTick(), 0);
            scheduleInstStop(tid,0,"Will trigger stat dump and reset");
            warmup_done = true;
        }
    }

    thread[tid]->numOp++;
    thread[tid]->threadStats.numOps++;
    cpuStats.committedOps[tid]++;

    probeInstCommit(inst->staticInst, inst->pcState().instAddr());
    cpuStats.lastCommitTick = curTick();
}

void
CPU::removeFrontInst(const DynInstPtr &inst)
{
    DPRINTF(O3CPU, "Removing committed instruction [tid:%i] PC %s "
            "[sn:%lli]\n",
            inst->threadNumber, inst->pcState(), inst->seqNum);

    removeInstsThisCycle = true;

    instList.erase(inst->getInstListIt());
}

void
CPU::removeInstsNotInROB(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %i: Deleting instructions from instruction"
            " list.\n", tid);

    ListIt end_it;

    bool rob_empty = false;

    if (instList.empty()) {
        return;
    } else if (rob.isEmpty(tid)) {
        DPRINTF(O3CPU, "ROB is empty, squashing all insts.\n");
        end_it = instList.begin();
        rob_empty = true;
    } else {
        end_it = (rob.readTailInst(tid))->getInstListIt();
        DPRINTF(O3CPU, "ROB is not empty, squashing insts not in ROB.\n");
    }

    removeInstsThisCycle = true;

    ListIt inst_it = instList.end();

    inst_it--;

    // Walk through the instruction list, removing any instructions
    // that were inserted after the given instruction iterator, end_it.
    while (inst_it != end_it) {
        assert(!instList.empty());

        inst_it = squashInstIt(inst_it, tid);
    }

    // If the ROB was empty, then we actually need to remove the first
    // instruction as well.
    if (rob_empty) {
        squashInstIt(inst_it, tid);
    }
}

void
CPU::removeInstsUntil(const InstSeqNum &seq_num, ThreadID tid)
{
    assert(!instList.empty());

    removeInstsThisCycle = true;

    ListIt inst_iter = instList.end();

    inst_iter--;

    DPRINTF(O3CPU, "Deleting instructions from instruction "
            "list that are from [tid:%i] and above [sn:%lli] (end=%lli).\n",
            tid, seq_num, (*inst_iter)->seqNum);

    while ((*inst_iter)->seqNum > seq_num) {

        bool break_loop = (inst_iter == instList.begin());

        inst_iter = squashInstIt(inst_iter, tid);

        if (break_loop)
            break;
    }
}

CPU::ListIt
CPU::squashInstIt(ListIt &instIt, ThreadID tid)
{
    if ((*instIt)->threadNumber == tid) {
        DPRINTF(O3CPU, "Squashing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*instIt)->threadNumber,
                (*instIt)->seqNum,
                (*instIt)->pcState());

        // Mark it as squashed.
        (*instIt)->setSquashed();

        // @todo: Formulate a consistent method for deleting
        // instructions from the instruction list
        // Remove the instruction from the list.
        instIt = instList.erase(instIt);
    }
    return --instIt;
}

void
CPU::flushTLBs()
{
    BaseCPU::flushTLBs();
    fetch.flushFetchBuffer();
}

void
CPU::cleanUpRemovedInsts()
{
    while (!removeList.empty()) {
        DPRINTF(O3CPU, "Removing instruction, "
                "[tid:%i] [sn:%lli] PC %s\n",
                (*removeList.front())->threadNumber,
                (*removeList.front())->seqNum,
                (*removeList.front())->pcState());

        instList.erase(removeList.front());

        removeList.pop_front();
    }

    removeInstsThisCycle = false;
}
/*
void
CPU::removeAllInsts()
{
    instList.clear();
}
*/
void
CPU::dumpInsts()
{
    int num = 0;

    ListIt inst_list_it = instList.begin();

    cprintf("Dumping Instruction List\n");

    while (inst_list_it != instList.end()) {
        cprintf("Instruction:%i\nPC:%#x\n[tid:%i]\n[sn:%lli]\nIssued:%i\n"
                "Squashed:%i\n\n",
                num, (*inst_list_it)->pcState().instAddr(),
                (*inst_list_it)->threadNumber,
                (*inst_list_it)->seqNum, (*inst_list_it)->isIssued(),
                (*inst_list_it)->isSquashed());
        inst_list_it++;
        ++num;
    }
}
/*
void
CPU::wakeDependents(const DynInstPtr &inst)
{
    iew.wakeDependents(inst);
}
*/
void
CPU::wakeCPU()
{
    if (activityRec.active() || tickEvent.scheduled()) {
        DPRINTF(Activity, "CPU already running.\n");
        return;
    }

    DPRINTF(Activity, "Waking up CPU\n");

    Cycles cycles(curCycle() - lastRunningCycle);
    // @todo: This is an oddity that is only here to match the stats
    if (cycles > 1) {
        --cycles;
        cpuStats.idleCycles += cycles;
        baseStats.numCycles += cycles;
    }

    schedule(tickEvent, clockEdge());
}

void
CPU::wakeup(ThreadID tid)
{
    if (thread[tid]->status() != gem5::ThreadContext::Suspended)
        return;

    wakeCPU();

    DPRINTF(Quiesce, "Suspended Processor woken\n");
    threadContexts[tid]->activate();
}

ThreadID
CPU::getFreeTid()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!tids[tid]) {
            tids[tid] = true;
            return tid;
        }
    }

    return InvalidThreadID;
}

void
CPU::updateThreadPriority()
{
    if (activeThreads.size() > 1) {
        //DEFAULT TO ROUND ROBIN SCHEME
        //e.g. Move highest priority to end of thread list
        std::list<ThreadID>::iterator list_begin = activeThreads.begin();

        unsigned high_thread = *list_begin;

        activeThreads.erase(list_begin);

        activeThreads.push_back(high_thread);
    }
}

void
CPU::addThreadToExitingList(ThreadID tid)
{
    DPRINTF(O3CPU, "Thread %d is inserted to exitingThreads list\n", tid);

    // the thread trying to exit can't be already halted
    assert(tcBase(tid)->status() != gem5::ThreadContext::Halted);

    // make sure the thread has not been added to the list yet
    assert(exitingThreads.count(tid) == 0);

    // add the thread to exitingThreads list to mark that this thread is
    // trying to exit. The boolean value in the pair denotes if a thread is
    // ready to exit. The thread is not ready to exit until the corresponding
    // exit trap event is processed in the future. Until then, it'll be still
    // an active thread that is trying to exit.
    exitingThreads.emplace(std::make_pair(tid, false));
}

bool
CPU::isThreadExiting(ThreadID tid) const
{
    return exitingThreads.count(tid) == 1;
}

void
CPU::scheduleThreadExitEvent(ThreadID tid)
{
    assert(exitingThreads.count(tid) == 1);

    // exit trap event has been processed. Now, the thread is ready to exit
    // and be removed from the CPU.
    exitingThreads[tid] = true;

    // we schedule a threadExitEvent in the next cycle to properly clean
    // up the thread's states in the pipeline. threadExitEvent has lower
    // priority than tickEvent, so the cleanup will happen at the very end
    // of the next cycle after all pipeline stages complete their operations.
    // We want all stages to complete squashing instructions before doing
    // the cleanup.
    if (!threadExitEvent.scheduled()) {
        schedule(threadExitEvent, nextCycle());
    }
}

void
CPU::exitThreads()
{
    // there must be at least one thread trying to exit
    assert(exitingThreads.size() > 0);

    // terminate all threads that are ready to exit
    auto it = exitingThreads.begin();
    while (it != exitingThreads.end()) {
        ThreadID thread_id = it->first;
        bool readyToExit = it->second;

        if (readyToExit) {
            DPRINTF(O3CPU, "Exiting thread %d\n", thread_id);
            haltContext(thread_id);
            tcBase(thread_id)->setStatus(gem5::ThreadContext::Halted);
            it = exitingThreads.erase(it);
        } else {
            it++;
        }
    }
}

void
CPU::htmSendAbortSignal(ThreadID tid, uint64_t htm_uid,
        HtmFailureFaultCause cause)
{
    const Addr addr = 0x0ul;
    const int size = 8;
    const Request::Flags flags =
      Request::PHYSICAL|Request::STRICT_ORDER|Request::HTM_ABORT;

    // O3-specific actions
    iew.ldstQueue.resetHtmStartsStops(tid);
    commit.resetHtmStartsStops(tid);

    // notify l1 d-cache (ruby) that core has aborted transaction
    RequestPtr req =
        std::make_shared<Request>(addr, size, flags, _dataRequestorId);

    req->taskId(taskId());
    req->setContext(thread[tid]->contextId());
    req->setHtmAbortCause(cause);

    assert(req->isHTMAbort());

    PacketPtr abort_pkt = Packet::createRead(req);
    uint8_t *memData = new uint8_t[8];
    assert(memData);
    abort_pkt->dataStatic(memData);
    abort_pkt->setHtmTransactional(htm_uid);

    // TODO include correct error handling here
    if (!iew.ldstQueue.getDataPort().sendTimingReq(abort_pkt)) {
        panic("HTM abort signal was not sent to the memory subsystem.");
    }
}

void
CPU::readGem5Regs()
{
    for (int i = 0; i < 32; i++) {
        diffAllStates->gem5RegFile[i] = readArchIntReg(i, 0);
        diffAllStates->gem5RegFile[i + 32] = readArchFloatReg(i, 0);
        readArchVecReg(i, (uint64_t*)&diffAllStates->gem5RegFile.vr[i], 0);
    }
}

RegVal
CPU::readArchIntReg(int reg_idx, ThreadID tid)
{

    cpuStats.intRegfileReads++;
    PhysRegIdPtr phys_reg =
        commitRenameMap[tid].lookup(RegId(IntRegClass, reg_idx)).PhyReg();

    DPRINTF(Scoreboard, "Get map: x%i -> p%i\n", reg_idx, phys_reg->flatIndex());

    return regFile.getReg(phys_reg);
}

RegVal
CPU::readArchFloatReg(int reg_idx, ThreadID tid)
{
    cpuStats.fpRegfileReads++;
    PhysRegIdPtr phys_reg =
        commitRenameMap[tid].lookup(RegId(FloatRegClass, reg_idx)).PhyReg();
    DPRINTF(Scoreboard, "Get map: f%i -> p%i\n", reg_idx, phys_reg->flatIndex());

    return regFile.getReg(phys_reg);
}

void
CPU::readArchVecReg(int reg_idx, uint64_t *val,ThreadID tid)
{
    cpuStats.vecRegfileReads++;
    PhysRegIdPtr phys_reg =
        commitRenameMap[tid].lookup(RegId(VecRegClass, reg_idx)).PhyReg();
    DPRINTF(Scoreboard, "Get map: v%i -> p%i\n", reg_idx, phys_reg->flatIndex());

    regFile.getReg(phys_reg, val);
}

bool
CPU::isTraceInstruction(InstSeqNum seqNum) const
{
    return fetch.isTraceInstruction(seqNum);
}

const o3::TraceInstruction*
CPU::getTraceInstMetadata(InstSeqNum seqNum) const
{
    return fetch.getTraceInstMetadata(seqNum);
}

uint64_t
CPU::getTraceIndexForSeqNum(InstSeqNum seqNum) const
{
    return fetch.findTraceIndexForSeqNum(seqNum);
}

Addr
CPU::getTracePCByIndex(uint64_t index)
{
    return fetch.getTracePCByIndex(index);
}

void
CPU::cleanupTraceMetadataOnCommit(InstSeqNum seqNum)
{
    if (isTraceMode()) {
        fetch.cleanupTraceMetadataOnCommit(seqNum);
    }
}

InstSeqNum
CPU::getOldestInFlightSeqNum() const
{
    if (!instList.empty()) {
        return instList.front()->seqNum;
    }
    // No in-flight instructions: return a large value so cleanup can proceed.
    return std::numeric_limits<InstSeqNum>::max();
}

} // namespace o3
} // namespace gem5
