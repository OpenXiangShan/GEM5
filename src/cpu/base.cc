/*
 * Copyright (c) 2011-2012,2016-2017, 2019-2020 ARM Limited
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
 * Copyright (c) 2002-2005 The Regents of The University of Michigan
 * Copyright (c) 2011 Regents of the University of California
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * Copyright (c) 2013 Mark D. Hill and David A. Wood
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

#include "cpu/base.hh"

#include <iostream>
#include <sstream>
#include <string>

#include "arch/generic/tlb.hh"
#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/cprintf.hh"
#include "base/loader/symtab.hh"
#include "base/logging.hh"
#include "base/output.hh"
#include "base/trace.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/difftest.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/thread_context.hh"
#include "debug/Diff.hh"
#include "debug/Diff2.hh"
#include "debug/DiffValue.hh"
#include "debug/DumpCommit.hh"
#include "debug/Mwait.hh"
#include "debug/SyscallVerbose.hh"
#include "debug/Thread.hh"
#include "mem/page_table.hh"
#include "params/BaseCPU.hh"
#include "sim/clocked_object.hh"
#include "sim/core.hh"
#include "sim/full_system.hh"
#include "sim/process.hh"
#include "sim/root.hh"
#include "sim/sim_events.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

// Hack
#include "sim/stat_control.hh"

namespace gem5
{

std::unique_ptr<BaseCPU::GlobalStats> BaseCPU::globalStats;

std::vector<BaseCPU *> BaseCPU::cpuList;

// This variable reflects the max number of threads in any CPU.  Be
// careful to only use it once all the CPUs that you care about have
// been initialized
int maxThreadsPerCPU = 1;

CPUProgressEvent::CPUProgressEvent(BaseCPU *_cpu, Tick ival)
    : Event(Event::Progress_Event_Pri), _interval(ival), lastNumInst(0),
      cpu(_cpu), _repeatEvent(true)
{
    if (_interval)
        cpu->schedule(this, curTick() + _interval);
}

void
CPUProgressEvent::process()
{
    Counter temp = cpu->totalOps();

    if (_repeatEvent)
        cpu->schedule(this, curTick() + _interval);

    if (cpu->switchedOut()) {
        return;
    }

#ifndef NDEBUG
    double ipc = double(temp - lastNumInst) / (_interval / cpu->clockPeriod());

    DPRINTFN("%s progress event, total committed:%i, progress insts committed: "
             "%lli, IPC: %0.8d\n", cpu->name(), temp, temp - lastNumInst,
             ipc);
    ipc = 0.0;
#else
    cprintf("%lli: %s progress event, total committed:%i, progress insts "
            "committed: %lli\n", curTick(), cpu->name(), temp,
            temp - lastNumInst);
#endif
    lastNumInst = temp;
}

const char *
CPUProgressEvent::description() const
{
    return "CPU Progress";
}

BaseCPU::BaseCPU(const Params &p, bool is_checker)
    : ClockedObject(p),
      instCnt(0),
      _cpuId(p.cpu_id),
      _socketId(p.socket_id),
      _instRequestorId(p.system->getRequestorId(this, "inst")),
      _dataRequestorId(p.system->getRequestorId(this, "data")),
      _taskId(context_switch_task_id::Unknown),
      _pid(invldPid),
      _switchedOut(p.switched_out),
      _cacheLineSize(p.system->cacheLineSize()),
      interrupts(p.interrupts),
      numThreads(p.numThreads),
      system(p.system),
      previousCycle(0),
      previousState(CPU_STATE_SLEEP),
      functionTraceStream(nullptr),
      currentFunctionStart(0),
      currentFunctionEnd(0),
      functionEntryTick(0),
      baseStats(this),
      addressMonitor(p.numThreads),
      syscallRetryLatency(p.syscallRetryLatency),
      pwrGatingLatency(p.pwr_gating_latency),
      powerGatingOnIdle(p.power_gating_on_idle),
      enterPwrGatingEvent([this] { enterPwrGating(); }, name()),
      warmupInstCount(p.warmupInstCount),
      enableDifftest(p.enable_difftest),
      dumpCommitFlag(p.dump_commit),
      dumpStartNum(p.dump_start),
      enableRVV(p.enable_riscv_vector),
      enableRVHDIFF(p.enable_riscv_h),
      noHypeMode(false),
      enableMemDedup(p.enable_mem_dedup)
{
    // if Python did not provide a valid ID, do it here
    if (_cpuId == -1 ) {
        _cpuId = cpuList.size();
    }

    // add self to global list of CPUs
    cpuList.push_back(this);

    DPRINTF(SyscallVerbose, "Constructing CPU with id %d, socket id %d\n",
            _cpuId, _socketId);

    if (numThreads > maxThreadsPerCPU)
        maxThreadsPerCPU = numThreads;

    functionTracingEnabled = false;
    if (p.function_trace) {
        const std::string fname = csprintf("ftrace.%s", name());
        functionTraceStream = simout.findOrCreate(fname)->stream();

        currentFunctionStart = currentFunctionEnd = 0;
        functionEntryTick = p.function_trace_start;

        if (p.function_trace_start == 0) {
            functionTracingEnabled = true;
        } else {
            Event *event = new EventFunctionWrapper(
                [this]{ enableFunctionTrace(); }, name(), true);
            schedule(event, p.function_trace_start);
        }
    }

    tracer = params().tracer;

    if (params().isa.size() != numThreads) {
        fatal("Number of ISAs (%i) assigned to the CPU does not equal number "
              "of threads (%i).\n", params().isa.size(), numThreads);
    }

    diffAllStates = std::make_shared<DiffAllStates>();
    if (enableDifftest) {
        assert(params().difftest_ref_so.length() > 2);
        diffAllStates->diff.nemu_reg = &(diffAllStates->referenceRegFile);
        diffAllStates->diff.nemu_this_pc = 0x80000000u;
        diffAllStates->diff.cpu_id = params().cpu_id;
        warn("cpu_id set to %d\n", params().cpu_id);

        if (params().difftest_ref_so.find("spike") != std::string::npos) {
            assert(!system->multiCore());
            diffAllStates->proxy = new SpikeProxy(
                params().cpu_id, params().difftest_ref_so.c_str(),
                params().nemuSDimg.size() && params().nemuSDCptBin.size());
        } else {
            diffAllStates->proxy =
                new NemuProxy(params().cpu_id, params().difftest_ref_so.c_str(),
                              params().nemuSDimg.size() && params().nemuSDCptBin.size(), system->enabledMemDedup(),
                              system->multiCore());
        }

        warn("Difftest is enabled with ref so: %s.\n", params().difftest_ref_so.c_str());

        diffAllStates->proxy->regcpy(&(diffAllStates->gem5RegFile), REF_TO_DUT);
        diffAllStates->diff.dynamic_config.ignore_illegal_mem_access = false;
        diffAllStates->diff.dynamic_config.debug_difftest = false;
        diffAllStates->proxy->update_config(&diffAllStates->diff.dynamic_config);
        if (params().nemuSDimg.size() && params().nemuSDCptBin.size()) {
            diffAllStates->proxy->sdcard_init(params().nemuSDimg.c_str(),
                               params().nemuSDCptBin.c_str());
        }
        diffAllStates->diff.will_handle_intr = false;
    } else {
        warn("Difftest is disabled\n");
        diffAllStates->hasCommit = true;
    }

    if (dumpCommitFlag) {
        registerExitCallback([this]() {
            auto out_handle = simout.create("dumpCommit.txt", false, true);
            for (auto iter : committedInsts) {
                *out_handle->stream() << std::hex << iter.first << " " << iter.second << std::endl;
            }
            simout.close(out_handle);
        });
    }
}

void
BaseCPU::enableFunctionTrace()
{
    functionTracingEnabled = true;
}

BaseCPU::~BaseCPU()
{
}

void
BaseCPU::postInterrupt(ThreadID tid, int int_num, int index)
{
    interrupts[tid]->post(int_num, index);
    // Only wake up syscall emulation if it is not waiting on a futex.
    // This is to model the fact that instructions such as ARM SEV
    // should wake up a WFE sleep, but not a futex syscall WAIT.
    if (FullSystem || !system->futexMap.is_waiting(threadContexts[tid]))
        wakeup(tid);
}

void
BaseCPU::armMonitor(ThreadID tid, Addr address)
{
    assert(tid < numThreads);
    AddressMonitor &monitor = addressMonitor[tid];

    monitor.armed = true;
    monitor.vAddr = address;
    monitor.pAddr = 0x0;
    DPRINTF(Mwait, "[tid:%d] Armed monitor (vAddr=0x%lx)\n", tid, address);
}

bool
BaseCPU::mwait(ThreadID tid, PacketPtr pkt)
{
    assert(tid < numThreads);
    AddressMonitor &monitor = addressMonitor[tid];

    if (!monitor.gotWakeup) {
        int block_size = cacheLineSize();
        uint64_t mask = ~((uint64_t)(block_size - 1));

        assert(pkt->req->hasPaddr());
        monitor.pAddr = pkt->getAddr() & mask;
        monitor.waiting = true;

        DPRINTF(Mwait, "[tid:%d] mwait called (vAddr=0x%lx, "
                "line's paddr=0x%lx)\n", tid, monitor.vAddr, monitor.pAddr);
        return true;
    } else {
        monitor.gotWakeup = false;
        return false;
    }
}

void
BaseCPU::mwaitAtomic(ThreadID tid, ThreadContext *tc, BaseMMU *mmu)
{
    assert(tid < numThreads);
    AddressMonitor &monitor = addressMonitor[tid];

    RequestPtr req = std::make_shared<Request>();

    Addr addr = monitor.vAddr;
    int block_size = cacheLineSize();
    uint64_t mask = ~((uint64_t)(block_size - 1));
    int size = block_size;

    //The address of the next line if it crosses a cache line boundary.
    Addr secondAddr = roundDown(addr + size - 1, block_size);

    if (secondAddr > addr)
        size = secondAddr - addr;

    req->setVirt(addr, size, 0x0, dataRequestorId(),
            tc->pcState().instAddr());

    // translate to physical address
    Fault fault = mmu->translateAtomic(req, tc, BaseMMU::Read);
    assert(fault == NoFault);

    monitor.pAddr = req->getPaddr() & mask;
    monitor.waiting = true;

    DPRINTF(Mwait, "[tid:%d] mwait called (vAddr=0x%lx, line's paddr=0x%lx)\n",
            tid, monitor.vAddr, monitor.pAddr);
}

void
BaseCPU::init()
{
    // Set up instruction-count-based termination events, if any. This needs
    // to happen after threadContexts has been constructed.
    if (params().max_insts_any_thread != 0) {
        const char *cause = "a thread reached the max instruction count";
        for (ThreadID tid = 0; tid < numThreads; ++tid)
            scheduleInstStop(tid, params().max_insts_any_thread, cause);
    }

    // Set up instruction-count-based termination events for SimPoints
    // Typically, there are more than one action points.
    // Simulation.py is responsible to take the necessary actions upon
    // exitting the simulation loop.
    if (!params().simpoint_start_insts.empty()) {
        const char *cause = "simpoint starting point found";
        for (size_t i = 0; i < params().simpoint_start_insts.size(); ++i)
            scheduleInstStop(0, params().simpoint_start_insts[i], cause);
    }

    if (params().max_insts_all_threads != 0) {
        const char *cause = "all threads reached the max instruction count";

        // allocate & initialize shared downcounter: each event will
        // decrement this when triggered; simulation will terminate
        // when counter reaches 0
        int *counter = new int;
        *counter = numThreads;
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            Event *event = new CountedExitEvent(cause, *counter);
            threadContexts[tid]->scheduleInstCountEvent(
                    event, params().max_insts_all_threads);
        }
    }

    if (!params().switched_out) {
        registerThreadContexts();

        verifyMemoryMode();
    }

    // Set CPU clock period for debug printing
    setCpuClockPeriod(clockPeriod());
}

void
BaseCPU::startup()
{
    if (params().progress_interval) {
        new CPUProgressEvent(this, params().progress_interval);
    }

    if (_switchedOut)
        powerState->set(enums::PwrState::OFF);

    // Assumption CPU start to operate instantaneously without any latency
    if (powerState->get() == enums::PwrState::UNDEFINED)
        powerState->set(enums::PwrState::ON);

    if (system->multiCore()) {
        goldenMemPtr = system->getGoldenMemPtr();
        _goldenMemManager = system->getGoldenMemManager();

        diffAllStates->proxy->initState(params().cpu_id, goldenMemPtr);
    } else {
        goldenMemPtr = nullptr;
        _goldenMemManager = nullptr;
    }
    diffInfo.scalarResults.resize(MaxDestRegisters);

}

probing::PMUUPtr
BaseCPU::pmuProbePoint(const char *name)
{
    probing::PMUUPtr ptr;
    ptr.reset(new probing::PMU(getProbeManager(), name));

    return ptr;
}

void
BaseCPU::regProbePoints()
{
    ppAllCycles = pmuProbePoint("Cycles");
    ppActiveCycles = pmuProbePoint("ActiveCycles");

    ppRetiredInsts = pmuProbePoint("RetiredInsts");
    ppRetiredInstsPC = pmuProbePoint("RetiredInstsPC");
    ppRetiredLoads = pmuProbePoint("RetiredLoads");
    ppRetiredStores = pmuProbePoint("RetiredStores");
    ppRetiredBranches = pmuProbePoint("RetiredBranches");

    ppSleeping = new ProbePointArg<bool>(this->getProbeManager(),
                                         "Sleeping");
}

void
BaseCPU::probeInstCommit(const StaticInstPtr &inst, Addr pc)
{
    if (!inst->isMicroop() || inst->isLastMicroop()) {
        ppRetiredInsts->notify(1);
        ppRetiredInstsPC->notify(pc);
    }

    if (inst->isLoad())
        ppRetiredLoads->notify(1);

    if (inst->isStore() || inst->isAtomic())
        ppRetiredStores->notify(1);

    if (inst->isControl())
        ppRetiredBranches->notify(1);
}

BaseCPU::
BaseCPUStats::BaseCPUStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(numCycles, statistics::units::Cycle::get(),
               "Number of cpu cycles simulated"),
      ADD_STAT(numWorkItemsStarted, statistics::units::Count::get(),
               "Number of work items this cpu started"),
      ADD_STAT(numWorkItemsCompleted, statistics::units::Count::get(),
               "Number of work items this cpu completed")
{
}

void
BaseCPU::regStats()
{
    ClockedObject::regStats();

    if (!globalStats) {
        /* We need to construct the global CPU stat structure here
         * since it needs a pointer to the Root object. */
        globalStats.reset(new GlobalStats(Root::root()));
    }

    using namespace statistics;

    int size = threadContexts.size();
    if (size > 1) {
        for (int i = 0; i < size; ++i) {
            std::stringstream namestr;
            ccprintf(namestr, "%s.ctx%d", name(), i);
            threadContexts[i]->regStats(namestr.str());
        }
    } else if (size == 1)
        threadContexts[0]->regStats(name());
}

Port &
BaseCPU::getPort(const std::string &if_name, PortID idx)
{
    // Get the right port based on name. This applies to all the
    // subclasses of the base CPU and relies on their implementation
    // of getDataPort and getInstPort.
    if (if_name == "dcache_port")
        return getDataPort();
    else if (if_name == "icache_port")
        return getInstPort();
    else
        return ClockedObject::getPort(if_name, idx);
}

void
BaseCPU::registerThreadContexts()
{
    assert(system->multiThread || numThreads == 1);

    fatal_if(interrupts.size() != numThreads,
             "CPU %s has %i interrupt controllers, but is expecting one "
             "per thread (%i)\n",
             name(), interrupts.size(), numThreads);

    for (ThreadID tid = 0; tid < threadContexts.size(); ++tid) {
        ThreadContext *tc = threadContexts[tid];

        system->registerThreadContext(tc);

        if (!FullSystem)
            tc->getProcessPtr()->assignThreadContext(tc->contextId());

        interrupts[tid]->setThreadContext(tc);
        tc->getIsaPtr()->setThreadContext(tc);
    }
}

void
BaseCPU::deschedulePowerGatingEvent()
{
    if (enterPwrGatingEvent.scheduled()){
        deschedule(enterPwrGatingEvent);
    }
}

void
BaseCPU::schedulePowerGatingEvent()
{
    for (auto tc : threadContexts) {
        if (tc->status() == ThreadContext::Active)
            return;
    }

    if (powerState->get() == enums::PwrState::CLK_GATED &&
        powerGatingOnIdle) {
        assert(!enterPwrGatingEvent.scheduled());
        // Schedule a power gating event when clock gated for the specified
        // amount of time
        schedule(enterPwrGatingEvent, clockEdge(pwrGatingLatency));
    }
}

int
BaseCPU::findContext(ThreadContext *tc)
{
    ThreadID size = threadContexts.size();
    for (ThreadID tid = 0; tid < size; ++tid) {
        if (tc == threadContexts[tid])
            return tid;
    }
    return 0;
}

void
BaseCPU::activateContext(ThreadID thread_num)
{
    DPRINTF(Thread, "activate contextId %d\n",
            threadContexts[thread_num]->contextId());
    // Squash enter power gating event while cpu gets activated
    if (enterPwrGatingEvent.scheduled())
        deschedule(enterPwrGatingEvent);
    // For any active thread running, update CPU power state to active (ON)
    powerState->set(enums::PwrState::ON);

    updateCycleCounters(CPU_STATE_WAKEUP);
}

void
BaseCPU::suspendContext(ThreadID thread_num)
{
    DPRINTF(Thread, "suspend contextId %d\n",
            threadContexts[thread_num]->contextId());
    // Check if all threads are suspended
    for (auto t : threadContexts) {
        if (t->status() != ThreadContext::Suspended) {
            return;
        }
    }

    // All CPU thread are suspended, update cycle count
    updateCycleCounters(CPU_STATE_SLEEP);

    // All CPU threads suspended, enter lower power state for the CPU
    powerState->set(enums::PwrState::CLK_GATED);

    // If pwrGatingLatency is set to 0 then this mechanism is disabled
    if (powerGatingOnIdle) {
        // Schedule power gating event when clock gated for pwrGatingLatency
        // cycles
        schedule(enterPwrGatingEvent, clockEdge(pwrGatingLatency));
    }
}

void
BaseCPU::haltContext(ThreadID thread_num)
{
    updateCycleCounters(BaseCPU::CPU_STATE_SLEEP);
}

void
BaseCPU::enterPwrGating(void)
{
    powerState->set(enums::PwrState::OFF);
}

void
BaseCPU::switchOut()
{
    assert(!_switchedOut);
    _switchedOut = true;

    // Flush all TLBs in the CPU to avoid having stale translations if
    // it gets switched in later.
    flushTLBs();

    // Go to the power gating state
    powerState->set(enums::PwrState::OFF);
}

void
BaseCPU::takeOverFrom(BaseCPU *oldCPU)
{
    assert(threadContexts.size() == oldCPU->threadContexts.size());
    assert(_cpuId == oldCPU->cpuId());
    assert(_switchedOut);
    assert(oldCPU != this);
    _pid = oldCPU->getPid();
    _taskId = oldCPU->taskId();
    // Take over the power state of the switchedOut CPU
    powerState->set(oldCPU->powerState->get());

    previousState = oldCPU->previousState;
    previousCycle = oldCPU->previousCycle;

    _switchedOut = false;

    ThreadID size = threadContexts.size();
    for (ThreadID i = 0; i < size; ++i) {
        ThreadContext *newTC = threadContexts[i];
        ThreadContext *oldTC = oldCPU->threadContexts[i];

        newTC->getIsaPtr()->setThreadContext(newTC);

        newTC->takeOverFrom(oldTC);

        assert(newTC->contextId() == oldTC->contextId());
        assert(newTC->threadId() == oldTC->threadId());
        system->replaceThreadContext(newTC, newTC->contextId());

        /* This code no longer works since the zero register (e.g.,
         * r31 on Alpha) doesn't necessarily contain zero at this
         * point.
           if (debug::Context)
            ThreadContext::compare(oldTC, newTC);
        */

        newTC->getMMUPtr()->takeOverFrom(oldTC->getMMUPtr());

        // Checker whether or not we have to transfer CheckerCPU
        // objects over in the switch
        CheckerCPU *old_checker = oldTC->getCheckerCpuPtr();
        CheckerCPU *new_checker = newTC->getCheckerCpuPtr();
        if (old_checker && new_checker) {
            new_checker->getMMUPtr()->takeOverFrom(old_checker->getMMUPtr());
        }
    }

    interrupts = oldCPU->interrupts;
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        interrupts[tid]->setThreadContext(threadContexts[tid]);
    }
    oldCPU->interrupts.clear();

    // All CPUs have an instruction and a data port, and the new CPU's
    // ports are dangling while the old CPU has its ports connected
    // already. Unbind the old CPU and then bind the ports of the one
    // we are switching to.
    getInstPort().takeOverFrom(&oldCPU->getInstPort());
    getDataPort().takeOverFrom(&oldCPU->getDataPort());

    // If old CPU enabled difftest, move it to this CPU as well
    auto [enable_diff, diff_all] = oldCPU->getDiffAllStates();
    if (enable_diff) {
        warn("Take over difftest state to new CPU\n");
        enableDifftest = enable_diff;
        takeOverDiffAllStates(diff_all);
    }
}

void
BaseCPU::flushTLBs()
{
    for (ThreadID i = 0; i < threadContexts.size(); ++i) {
        ThreadContext &tc(*threadContexts[i]);
        CheckerCPU *checker(tc.getCheckerCpuPtr());

        tc.getMMUPtr()->flushAll();
        if (checker) {
            checker->getMMUPtr()->flushAll();
        }
    }
}

void
BaseCPU::serialize(CheckpointOut &cp) const
{
    SERIALIZE_SCALAR(instCnt);

    if (!_switchedOut) {
        /* Unlike _pid, _taskId is not serialized, as they are dynamically
         * assigned unique ids that are only meaningful for the duration of
         * a specific run. We will need to serialize the entire taskMap in
         * system. */
        SERIALIZE_SCALAR(_pid);

        // Serialize the threads, this is done by the CPU implementation.
        for (ThreadID i = 0; i < numThreads; ++i) {
            ScopedCheckpointSection sec(cp, csprintf("xc.%i", i));
            interrupts[i]->serialize(cp);
            serializeThread(cp, i);
        }
    }
}

void
BaseCPU::unserialize(CheckpointIn &cp)
{
    UNSERIALIZE_SCALAR(instCnt);

    if (!_switchedOut) {
        UNSERIALIZE_SCALAR(_pid);

        // Unserialize the threads, this is done by the CPU implementation.
        for (ThreadID i = 0; i < numThreads; ++i) {
            ScopedCheckpointSection sec(cp, csprintf("xc.%i", i));
            interrupts[i]->unserialize(cp);
            unserializeThread(cp, i);
        }
    }
}

void
BaseCPU::scheduleInstStop(ThreadID tid, Counter insts, const char *cause)
{
    const Tick now(getCurrentInstCount(tid));
    Event *event(new LocalSimLoopExitEvent(cause, 0));

    threadContexts[tid]->scheduleInstCountEvent(event, now + insts);
}

Tick
BaseCPU::getCurrentInstCount(ThreadID tid)
{
    return threadContexts[tid]->getCurrentInstCount();
}

AddressMonitor::AddressMonitor()
{
    armed = false;
    waiting = false;
    gotWakeup = false;
}

bool
AddressMonitor::doMonitor(PacketPtr pkt)
{
    assert(pkt->req->hasPaddr());
    if (armed && waiting) {
        if (pAddr == pkt->getAddr()) {
            DPRINTF(Mwait, "pAddr=0x%lx invalidated: waking up core\n",
                    pkt->getAddr());
            waiting = false;
            return true;
        }
    }
    return false;
}


void
BaseCPU::traceFunctionsInternal(Addr pc)
{
    if (loader::debugSymbolTable.empty())
        return;

    // if pc enters different function, print new function symbol and
    // update saved range.  Otherwise do nothing.
    if (pc < currentFunctionStart || pc >= currentFunctionEnd) {
        auto it = loader::debugSymbolTable.findNearest(
                pc, currentFunctionEnd);

        std::string sym_str;
        if (it == loader::debugSymbolTable.end()) {
            // no symbol found: use addr as label
            sym_str = csprintf("%#x", pc);
            currentFunctionStart = pc;
            currentFunctionEnd = pc + 1;
        } else {
            sym_str = it->name;
            currentFunctionStart = it->address;
        }

        ccprintf(*functionTraceStream, " (%d)\n%d: %s",
                 curTick() - functionEntryTick, curTick(), sym_str);
        functionEntryTick = curTick();
    }
}


BaseCPU::GlobalStats::GlobalStats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(simInsts, statistics::units::Count::get(),
             "Number of instructions simulated"),
    ADD_STAT(simOps, statistics::units::Count::get(),
             "Number of ops (including micro ops) simulated"),
    ADD_STAT(hostInstRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Second>::get(),
             "Simulator instruction rate (inst/s)"),
    ADD_STAT(hostOpRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Second>::get(),
             "Simulator op (including micro ops) rate (op/s)")
{
    simInsts
        .functor(BaseCPU::numSimulatedInsts)
        .precision(0)
        .prereq(simInsts)
        ;

    simOps
        .functor(BaseCPU::numSimulatedOps)
        .precision(0)
        .prereq(simOps)
        ;

    hostInstRate
        .precision(0)
        .prereq(simInsts)
        ;

    hostOpRate
        .precision(0)
        .prereq(simOps)
        ;

    hostInstRate = simInsts / hostSeconds;
    hostOpRate = simOps / hostSeconds;
}

void
BaseCPU::csrDiffMessage(uint64_t gem5_val, uint64_t ref_val, int error_num, uint64_t &error_reg, InstSeqNum seq,
                        std::string error_csr_name, int &diff_at)
{
    DPRINTF(DiffValue, "Inst [sn:%lli] pc: %#lx\n", seq, diffInfo.pc->instAddr());
    DPRINTF(DiffValue, "Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
            error_csr_name, ref_val, gem5_val);
    diffInfo.errorCsrsValue[error_num] = 1;
    error_reg = gem5_val;
    if (!diff_at)
        diff_at = ValueDiff;
}



std::pair<int, bool>
BaseCPU::diffWithNEMU(ThreadID tid, InstSeqNum seq)
{
    int diff_at = DiffAt::NoneDiff;
    bool npc_match = false;
    bool is_mmio = diffInfo.curInstStrictOrdered;

    if (diffInfo.inst->isStoreConditional()) {
        diffAllStates->proxy->uarchstatus_cpy(&diffAllStates->diff.sync, DIFFTEST_TO_REF);
    }

    if (diffAllStates->diff.will_handle_intr) {
        diffAllStates->proxy->regcpy(diffAllStates->diff.nemu_reg, REF_TO_DIFFTEST);
        diffAllStates->diff.nemu_this_pc = diffAllStates->diff.nemu_reg->pc;
        diffAllStates->diff.will_handle_intr = false;
    }

    if (is_mmio) {
        DPRINTF(Diff, "Skip step NEMU due to mmio access\n");
        diffAllStates->referenceRegFile.pc = diffInfo.pc->as<RiscvISA::PCState>().npc();
        if (diffInfo.inst->numDestRegs() > 0) {
            assert(diffInfo.inst->numDestRegs()==1);
            const auto &dest = diffInfo.inst->destRegIdx(0);
            unsigned index = dest.index() + (dest.isFloatReg() ? FPRegIndexBase : IntRegIndexBase);
            diffAllStates->referenceRegFile[index] = diffInfo.scalarResults[0];
        }
        diffAllStates->proxy->regcpy(&(diffAllStates->referenceRegFile), DUT_TO_REF);

        uint64_t next_pc = diffAllStates->diff.nemu_reg->pc;
        // replace with "this pc" for checking
        diffAllStates->diff.nemu_commit_inst_pc = diffInfo.pc->instAddr();
        diffAllStates->diff.nemu_this_pc = next_pc;
        diffAllStates->diff.npc = next_pc;
        return std::make_pair(NoneDiff, true);
    } else {
        // difftest step start
        DPRINTF(Diff, "Step NEMU\n");
        diffAllStates->proxy->exec(1);
        diffAllStates->proxy->regcpy(diffAllStates->diff.nemu_reg, REF_TO_DIFFTEST);

        uint64_t next_pc = diffAllStates->diff.nemu_reg->pc;
        // replace with "this pc" for checking
        diffAllStates->diff.nemu_commit_inst_pc = diffAllStates->diff.nemu_this_pc;
        diffAllStates->diff.nemu_this_pc = next_pc;
        diffAllStates->diff.npc = next_pc;
    }
    // difftest step end

    auto gem5_pc = diffInfo.pc->instAddr();
    diffAllStates->gem5RegFile.pc = gem5_pc;
    auto nemu_pc = diffAllStates->diff.nemu_commit_inst_pc;

    if (nemu_pc != gem5_pc) {
        // warn("NEMU store addr: %#lx\n", nemu_store_addr);
        diffMsg << csprintf("Inst [sn:%lli]\n", seq);
        diffMsg << csprintf( "Diff at %s, NEMU: %#lx, GEM5: %#lx\n", "PC", nemu_pc,
                gem5_pc);
        if (!diff_at) {
            diff_at = PCDiff;
            diffInfo.errorPcValue = 1;
            diffMsg << csprintf("GEM5 pc: %#lx, NEMU npc: %#lx\n", gem5_pc,
                    diffAllStates->diff.npc);
            if (diffAllStates->diff.npc == gem5_pc) {
                npc_match = true;
            }
        }
    }
    DPRINTF(Diff2, "pc %#x inst %#x @ %s\n", gem5_pc, diffInfo.pc->instAddr(),
            diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
    DPRINTF(Diff, "Inst [sn:%lli] PC, NEMU: %#lx, GEM5: %#lx\n", seq, nemu_pc,
            gem5_pc);

    DPRINTF(Diff, "Inst [sn:%llu] @ %#lx in GEM5 is %s\n", seq,
            diffInfo.pc->instAddr(),
            diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
    auto machInst = dynamic_cast<RiscvISA::RiscvStaticInst &>(*diffInfo.inst).machInst;
    DPRINTF(Diff, "MachInst: %#lx\n", machInst);


    if (enableRVV) {
        if (diffInfo.inst->isVector()) {
            readGem5Regs();
            uint64_t* nemu_val = (uint64_t*)&(diffAllStates->referenceRegFile.vr[0]);
            uint64_t* gem5_val = (uint64_t*)&(diffAllStates->gem5RegFile.vr[0]);
            bool maybe_error = false;
            int error_idx = 0;
            for (int i=0; i < RiscvISA::NumVecElemPerVecReg * 32; i++) {
                if (nemu_val[i] != gem5_val[i]) {
                    // diff_at = ValueDiff;
                    maybe_error = true;
                    error_idx = (i >> 1) * 2;
                    break;
                }
            }

            if (maybe_error) {
                std::string gem5_val_, nemu_val_;
                for (int j=RiscvISA::NumVecElemPerVecReg-1; j>=0; j--) {
                    gem5_val_ += csprintf("%016lx", gem5_val[j + error_idx]);
                    if (j != 0) {
                        gem5_val_+="_";
                    }
                }
                for (int j=RiscvISA::NumVecElemPerVecReg-1; j>=0; j--) {
                    nemu_val_ += csprintf("%016lx", nemu_val[j + error_idx]);
                    if (j != 0) {
                        nemu_val_ += "_";
                    }
                }
                warn("May be diff at v%d\n Ref  value: %s\n GEM5 value: %s\n",
                    (error_idx>>1), nemu_val_, gem5_val_);
                diff_at = ValueDiff;
            }
        }

        // vtype
        uint64_t gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VTYPE, tid);
        diffAllStates->gem5RegFile.vtype = gem5_val;
        uint64_t ref_val = diffAllStates->referenceRegFile.vtype;
        // unable highest digit comparison of vtype because the older version of NEMU (used by GCBH) did not support it well.
        if (gem5_val % (1ULL<<63) != ref_val % (1ULL<<63)) {
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "vtype", ref_val, gem5_val);
            if (!diff_at) {
                diff_at = ValueDiff;
            }
        }

        // vstart now do not diff
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTART, tid);
        diffAllStates->gem5RegFile.vstart = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vstart;

        // vxsat
        diffAllStates->gem5RegFile.vxsat = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VXSAT, tid);
        // vxrm
        diffAllStates->gem5RegFile.vxrm = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VXRM, tid);
        // vcsr
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VCSR, tid);
        diffAllStates->gem5RegFile.vcsr = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vcsr;
        if (gem5_val != ref_val) {
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "vcsr", ref_val, gem5_val);
            if (!diff_at) {
                diff_at = ValueDiff;
            }
        }

        // vl
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VL, tid);
        diffAllStates->gem5RegFile.vl = gem5_val;
        ref_val = diffAllStates->referenceRegFile.vl;
        if (gem5_val != ref_val) {
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "vl", ref_val, gem5_val);
            if (!diff_at) {
                diff_at = ValueDiff;
            }
        }
    }

    // always check some CSR regs
    {
        // mstatus
        auto gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_STATUS, tid);
        // readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_STATUS, 0);
        auto ref_val = diffAllStates->referenceRegFile.mstatus;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mstatus, diffAllStates->gem5RegFile.mstatus, seq, "mstatus",
                           diff_at);
            diffMsg <<
                csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                         "mstatus", ref_val, gem5_val);
        }
        //stval
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_STVAL, tid);
        ref_val = diffAllStates->referenceRegFile.stval;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::stval, diffAllStates->gem5RegFile.stval, seq, "stval",
                           diff_at);
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n", "stval",
                    ref_val, gem5_val);
        }

        // mtval
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_MTVAL, tid);
        ref_val = diffAllStates->referenceRegFile.mtval;
        DPRINTF(Diff, "stvmtvalal:\tGEM5: %#lx,\tREF: %#lx\n", gem5_val, ref_val);
        if (gem5_val != ref_val) {
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n", "mtval",
                    ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::mtval] = 1;
            diffAllStates->gem5RegFile.mtval = gem5_val;
            if (!diff_at)
                diff_at = ValueDiff;
        }
            //DIFFTEST_STVDIFFTEST_STVAL

        // mode
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_PRV, tid);
        ref_val = diffAllStates->referenceRegFile.mode;
        DPRINTF(Diff, "priv:\tGEM5: %#lx,\tREF: %#lx\n", gem5_val, ref_val);
        if (gem5_val != ref_val) {
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n", "priv",
                    ref_val, gem5_val);
            // diffInfo.errorCsrsValue[CsrRegIndex::stval] = 1;
            diffAllStates->gem5RegFile.mode = gem5_val;
            if (!diff_at)
                diff_at = ValueDiff;
        }

        // mcause
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_MCAUSE, tid);
        ref_val = diffAllStates->referenceRegFile.mcause;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mcause, diffAllStates->gem5RegFile.mcause, seq, "mcause",
                           diff_at);
            diffMsg <<
                csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "mcause", ref_val, gem5_val);
        }

        // scause
        gem5_val = readMiscRegNoEffect(
            RiscvISA::MiscRegIndex::MISCREG_SCAUSE, tid);
        ref_val = diffAllStates->referenceRegFile.scause;
        DPRINTF(Diff, "scause:\tGEM5: %#lx,\tREF: %#lx\n", gem5_val, ref_val);
        if (gem5_val != ref_val) {
            diffMsg <<
                csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "scause", ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::scause] = 1;
            diffAllStates->gem5RegFile.scause = gem5_val;
            if (!diff_at)
                diff_at = ValueDiff;
        }
        // satp
        gem5_val =
            readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_SATP, tid);
        ref_val = diffAllStates->referenceRegFile.satp;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::satp, diffAllStates->gem5RegFile.satp, seq, "satp",
                           diff_at);
            diffMsg << csprintf("CPU%i Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033"
                    "[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    cpuId(), "satp", ref_val, gem5_val);
        }

        // mie
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_IE, tid);
        ref_val = diffAllStates->referenceRegFile.mie;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mie, diffAllStates->gem5RegFile.mie, seq, "mie", diff_at);
            diffMsg << csprintf("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n", "mie",
                    ref_val, gem5_val);
        }
        // mip
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_IP, tid);
        ref_val = diffAllStates->referenceRegFile.mip;
        if (gem5_val != ref_val) {
            warn("mip:\tGEM5: %#lx,\tREF: %#lx\n", gem5_val, ref_val);
            diffMsg <<
                csprintf("%s at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    gem5_val == ref_val ? "match" : "diff", "mip", ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::mip] = 1;
            diffAllStates->gem5RegFile.mip = gem5_val;
        }
        //mepc
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MEPC, tid);
        ref_val = diffAllStates->referenceRegFile.mepc;
        if (gem5_val != ref_val) {
            warn("Inst [sn:%lli] pc: %#lx\n", seq, diffInfo.pc->instAddr());
            warn("Diff at \033[31m%s\033[0m Ref value: \033[31m"
                    "%#lx\033[0m, GEM5 value: \033[31m%#lx\033[0m\n",
                    "mepc", ref_val, gem5_val);
            diffInfo.errorCsrsValue[CsrRegIndex::mepc] = 1;
            diffAllStates->gem5RegFile.mepc = gem5_val;
        }
    }

    if (enableRVHDIFF){
        //h difftest
        //mtval2
        auto gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTVAL2, tid);
        auto ref_val = diffAllStates->referenceRegFile.mtval2;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mtval2, diffAllStates->gem5RegFile.mtval2, seq, "mtval2",
                           diff_at);
        }
        //mtinst
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTINST, tid);
        ref_val = diffAllStates->referenceRegFile.mtinst;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::mtinst, diffAllStates->gem5RegFile.mtinst, seq, "mtinst",
                           diff_at);
        }
        //hstatus
        gem5_val = readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_HSTATUS, tid);
        ref_val = diffAllStates->referenceRegFile.hstatus;
        if ((gem5_val != ref_val)) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hstatus, diffAllStates->gem5RegFile.hstatus, seq, "hstatus",
                           diff_at);
        }
        //hideleg
        gem5_val = readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_HIDELEG, tid);
        ref_val = diffAllStates->referenceRegFile.hideleg;
        if ((gem5_val != ref_val)) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hideleg, diffAllStates->gem5RegFile.hideleg, seq, "hideleg",
                           diff_at);
        }
        //hedeleg
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HEDELEG, tid);
        ref_val = diffAllStates->referenceRegFile.hedeleg;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hedeleg, diffAllStates->gem5RegFile.hedeleg, seq, "hedeleg",
                           diff_at);
        }
        //hcounteren
        gem5_val = readMiscRegNoEffect(RiscvISA::MiscRegIndex::MISCREG_HCOUNTEREN, tid);
        ref_val = diffAllStates->referenceRegFile.hcounteren;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hcounteren, diffAllStates->gem5RegFile.hcounteren, seq,
                           "hcounteren", diff_at);
        }

        //htval
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HTVAL, tid);
        ref_val = diffAllStates->referenceRegFile.htval;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::htval, diffAllStates->gem5RegFile.htval, seq, "htval",
                           diff_at);
        }

        // htinst
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HTINST, tid);
        ref_val = diffAllStates->referenceRegFile.htinst;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::htinst, diffAllStates->gem5RegFile.htinst, seq, "htinst",
                           diff_at);
        }
        // hgatp
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HGATP, tid);
        ref_val = diffAllStates->referenceRegFile.hgatp;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::hgatp, diffAllStates->gem5RegFile.hgatp, seq, "hgatp",
                           diff_at);
        }
        // vsstatus
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSSTATUS, tid);
        ref_val = diffAllStates->referenceRegFile.vsstatus;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsstatus, diffAllStates->gem5RegFile.vsstatus, seq,
                           "vsstatus", diff_at);
        }
        // vstvec
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTVEC, tid);
        ref_val = diffAllStates->referenceRegFile.vstvec;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vstvec, diffAllStates->gem5RegFile.vstvec, seq, "vstvec",
                           diff_at);
        }
        // vsepc
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSEPC, tid);
        ref_val = diffAllStates->referenceRegFile.vsepc;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsepc, diffAllStates->gem5RegFile.vsepc, seq, "vsepc",
                           diff_at);
        }
        // vscause
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSCAUSE, tid);
        ref_val = diffAllStates->referenceRegFile.vscause;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vscause, diffAllStates->gem5RegFile.vscause, seq, "vscause",
                           diff_at);
        }
        // vstval
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTVAL, tid);
        ref_val = diffAllStates->referenceRegFile.vstval;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vstval, diffAllStates->gem5RegFile.vstval, seq, "vstval",
                           diff_at);
        }

        // vsatp
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSATP, tid);
        ref_val = diffAllStates->referenceRegFile.vsatp;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsatp, diffAllStates->gem5RegFile.vsatp, seq, "vsatp",
                           diff_at);
        }

        // vsscratch
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSSCRATCH, tid);
        ref_val = diffAllStates->referenceRegFile.vsscratch;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::vsscratch, diffAllStates->gem5RegFile.vsscratch, seq,
                           "vsscratch", diff_at);
        }
        // cpu.v diff
        gem5_val = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VIRMODE, tid);
        ref_val = diffAllStates->referenceRegFile.v;
        if (gem5_val != ref_val) {
            csrDiffMessage(gem5_val, ref_val, CsrRegIndex::v, diffAllStates->gem5RegFile.v, seq, "v", diff_at);
        }
    }



        if (diff_at != NoneDiff) {
            DPRINTF(Diff, "Inst [sn:%llu] @ \033[31m%#lx\033[0m in GEM5 is \033[31m%s\033[0m\n", seq,
                    diffInfo.pc->instAddr(),
                    diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
        }


    for (int dest_idx = 0; dest_idx < diffInfo.inst->numDestRegs(); dest_idx++) {

        const auto &dest = diffInfo.inst->destRegIdx(dest_idx);
        auto dest_tag = dest.index() + dest.isFloatReg() * 32;

        if ((dest.isFloatReg() || dest.isIntReg()) && !dest.isZeroReg()) {
            auto gem5_val = diffInfo.scalarResults[dest_idx];
            auto nemu_val = diffAllStates->referenceRegFile[dest_tag];
            DPRINTF(Diff, "At %s Ref value: %#lx, GEM5 value: %#lx\n",
                    reg_name[dest_tag], nemu_val, gem5_val);

            if (gem5_val != nemu_val) {
                if (diffInfo.inst->isMemRef()) {
                    diffMsg << csprintf("%s addr: %#lx, size: %u\n",
                                        diffInfo.inst->isAtomic() ? "AMO"
                                        : diffInfo.inst->isLoad() ? "Load"
                                                                  : "Store",
                                        diffInfo.physEffAddr, diffInfo.effSize);
                }

                if (system->multiCore() && (diffInfo.inst->isLoad() || diffInfo.inst->isAtomic()) &&
                    _goldenMemManager->inPmem(diffInfo.physEffAddr)) {
                    warn("Difference on %s instr found in multicore mode, check in golden memory\n",
                         diffInfo.inst->isLoad() ? "load" : "amo");
                    uint8_t *golden_ptr = diffInfo.goldenValue;

                    // a lambda function to sync memory and register from golden results to ref
                    auto sync_mem_reg = [&]() {
                        diffAllStates->proxy->memcpy(diffInfo.physEffAddr, golden_ptr, diffInfo.effSize,
                                                     DIFFTEST_TO_REF);
                        diffAllStates->referenceRegFile[dest_tag] = gem5_val;
                        diffAllStates->proxy->regcpy(&(diffAllStates->referenceRegFile), DUT_TO_REF);
                    };

                    if (diffInfo.inst->isLoad() && memcmp(golden_ptr, &gem5_val, diffInfo.effSize) == 0) {
                        DPRINTF(Diff, "Load content matched in golden memory. Sync from golden to ref\n");
                        sync_mem_reg();
                        continue;
                    } else if (diffInfo.inst->isAtomic()) {
                        DPRINTF(Diff, "Golden mem old value: %#lx, GEM5 old value: %#lx\n", diffInfo.amoOldGoldenValue,
                                gem5_val);
                        DPRINTF(Diff, "New golden value: %#lx\n", *(uint64_t *)golden_ptr);
                        if (memcmp(&diffInfo.amoOldGoldenValue, &gem5_val, diffInfo.effSize) == 0) {
                            DPRINTF(Diff, "Atomic encountered, old value matched. Sync from golden to ref\n");
                            sync_mem_reg();
                            continue;
                        } else {
                            warn("Atomic old value not matched!\n");
                        }
                    }
                }

                if (dest.isFloatReg() && (gem5_val ^ nemu_val) == ((0xffffffffULL) << 32)) {
                    DPRINTF(Diff,
                            "Difference might be caused by box,"
                            " ignore it\n");
                } else {
                    for (int i = 0; i < diffInfo.inst->numSrcRegs(); i++) {
                        const auto &src = diffInfo.inst->srcRegIdx(i);
                        warn("Src%d %s = %lx\n", i,
                                reg_name[src.index()],
                                diffInfo.getSrcReg(src));
                        // threadContexts[curThread]->getReg(src));
                    }
                    bool skipCSR = false;
                    for (auto iter : skipCSRs) {
                        if ((machInst & 0xfff00073) == iter) {
                            skipCSR = true;
                            DPRINTF(Diff, "This is an csr instruction, skip!\n");
                            diffAllStates->referenceRegFile[dest_tag] = gem5_val;
                            diffAllStates->proxy->regcpy(&(diffAllStates->referenceRegFile), DUT_TO_REF);
                            break;
                        }
                    }
                    warn("Inst src count: %u, dest count: %u\n",
                            diffInfo.inst->numSrcRegs(),
                            diffInfo.inst->numDestRegs());
                    diffMsg << csprintf("Inst [sn:%lli] pc: %#lx\n", seq, diffInfo.pc->instAddr());
                    diffMsg << csprintf(
                        "Diff at \033[31m%s\033[0m Ref value: \033[31m%#lx\033[0m, "
                        "GEM5 value: \033[31m%#lx\033[0m\n",
                        reg_name[dest_tag], nemu_val, gem5_val);
                    diffInfo.errorRegsValue[dest_tag] = 1;
                    if (dest_tag < 32)
                        diffAllStates->gem5RegFile.gpr[dest_tag]._64 = gem5_val;
                    else if (dest_tag >= 32 && dest_tag < 64)
                        diffAllStates->gem5RegFile.fpr[dest_tag - 32]._64 = gem5_val;

                    diffAllStates->gem5RegFile.pc = gem5_pc;
                    if (!diff_at && !skipCSR) {
                        diff_at = ValueDiff;
                    }
                }
            }
        }
    }
    if (diff_at) {
        diffMsg << csprintf("In CPU%d: NEMU PC: %#10lx, GEM5 PC: %#10lx, inst: %s\n", cpuId(),
        nemu_pc, gem5_pc,
        diffInfo.inst->disassemble(diffInfo.pc->instAddr()).c_str());
    }
    return std::make_pair(diff_at, npc_match);
}

void
BaseCPU::clearDiffMismatch(ThreadID tid, InstSeqNum seq) {
    diffMsg.str(std::string());
    memset(diffInfo.errorRegsValue, 0, sizeof(diffInfo.errorRegsValue));
    memset(diffInfo.errorCsrsValue, 0, sizeof(diffInfo.errorCsrsValue));
    diffInfo.errorPcValue = 0;
}

void
BaseCPU::reportDiffMismatch(ThreadID tid, InstSeqNum seq)
{
    warn("%s", diffMsg.str());
    diffAllStates->proxy->isa_reg_display();
    displayGem5Regs();
    warn("start dump last %lu committed msg\n", diffInfo.lastCommittedMsg.size());
    while (diffInfo.lastCommittedMsg.size()) {
        auto &inst = diffInfo.lastCommittedMsg.front();
        warn("V %s\n", inst->genDisassembly());
        diffInfo.lastCommittedMsg.pop();
    }
}

void
BaseCPU::difftestStep(ThreadID tid, InstSeqNum seq)
{
    bool should_diff = false;
    DPRINTF(DumpCommit, "[sn:%llu] %#lx, %s\n",
            seq, diffInfo.pc->instAddr(), diffInfo.inst->disassemble(diffInfo.pc->instAddr()));
    DPRINTF(Diff, "DiffTest step on inst pc: %#lx: %s\n",
            diffInfo.pc->instAddr(),
            diffInfo.inst->disassemble(diffInfo.pc->instAddr()));

    bool is_fence = diffInfo.inst->isReadBarrier() || diffInfo.inst->isWriteBarrier();
    bool fence_should_diff = is_fence && !diffInfo.inst->isMicroop();
    bool lr_should_diff = diffInfo.inst->isLoadReserved();
    bool amo_should_diff = diffInfo.inst->isAtomic() && diffInfo.inst->numDestRegs() > 0;
    bool is_sc = diffInfo.inst->isStoreConditional() && diffInfo.inst->isDelayedCommit();
    bool other_should_diff = !diffInfo.inst->isAtomic() && !is_fence && !is_sc &&
                             (!diffInfo.inst->isMicroop() || diffInfo.inst->isLastMicroop());

    if (fence_should_diff || amo_should_diff || is_sc || other_should_diff || lr_should_diff) {
        should_diff = true;
        if (!diffAllStates->hasCommit && diffInfo.pc->instAddr() == 0x80000000u) {
            diffAllStates->hasCommit = true;
            readGem5Regs();
            diffAllStates->gem5RegFile.pc = diffInfo.pc->instAddr();
            if (noHypeMode) {
                auto start = pmemStart + pmemSize * diffAllStates->diff.cpu_id;
                warn("Start memcpy to NEMU from %#lx, size=%lu \n", (uint64_t)start, pmemSize);
                diffAllStates->proxy->memcpy(0x80000000u, start, pmemSize, DUT_TO_REF);
            } else if (enableMemDedup) {
                warn("Let ref share a COW mirror of root memory\n");
                assert(diffAllStates->proxy->ref_get_backed_memory);
                diffAllStates->proxy->ref_get_backed_memory(system->createCopyOnWriteBranch(), pmemSize);
            } else {
                warn("Start memcpy to NEMU from %#lx, size=%lu\n", (uint64_t)pmemStart, pmemSize);
                diffAllStates->proxy->memcpy(0x80000000u, pmemStart, pmemSize, DUT_TO_REF);
            }
            warn("Start regcpy to NEMU\n");
            diffAllStates->proxy->regcpy(&(diffAllStates->gem5RegFile), DUT_TO_REF);
        }
    }

    if (enableDifftest && should_diff) {
        auto [diff_at, npc_match] = diffWithNEMU(tid, seq);
        if (diff_at != NoneDiff) {
            if (npc_match && diff_at == PCDiff) {
                // warn("Found PC mismatch, Let NEMU run one more
                // instruction\n");
                std::tie(diff_at, npc_match) = diffWithNEMU(tid, 0);
                if (diff_at != NoneDiff) {
                    reportDiffMismatch(tid, seq);
                    panic("Difftest failed again!\n");

                } else {
                    clearDiffMismatch(tid, seq);
                    DPRINTF(Diff,
                            "Difftest matched again, "
                            "NEMU seems to commit the failed mem instruction\n");
                }
            } else {
                reportDiffMismatch(tid, seq);
                panic("Difftest failed!\n");
            }
        } else {
            clearDiffMismatch(tid, seq);
        }
    }
    committedInstNum++;
    if (dumpCommitFlag && committedInstNum >= dumpStartNum) {
        committedInsts.push_back(
            std::make_pair(diffInfo.pc->instAddr(), diffInfo.inst->disassemble(diffInfo.pc->instAddr()).c_str()));
    }
}

void
BaseCPU::displayGem5Regs()
{
    readGem5Regs();
    std::string str;
    //reg
    for (size_t i = 0; i < 32; i++)
    {
        if ( diffInfo.errorRegsValue[i] )
            str += csprintf("\033[31m%04s : %16lx \033[0m",reg_name[i]  ,diffAllStates->gem5RegFile.gpr[i]._64);
        else
            str += csprintf("%04s : %16lx ",reg_name[i]  ,diffAllStates->gem5RegFile.gpr[i]._64);

        if (i%4 == 3) str += csprintf("\n");
    }
    warn("gem5-rRegsDisplay : \n%s",str) ;
    str.clear();

    //fp
    for (size_t i = 0; i < 32; i++)
    {
        if ( diffInfo.errorRegsValue[i+32] )
            str += csprintf("\033[31m%04s : %16lx \033[0m",reg_name[i+32]  ,diffAllStates->gem5RegFile.fpr[i]._64);
        else
            str += csprintf("%04s : %16lx ",reg_name[i+32]  ,diffAllStates->gem5RegFile.fpr[i]._64);

        if (i%4 == 3) str += csprintf("\n");
    }
    warn("gem5-fRegsDisplay : \n%s",str);
    str.clear();

    //csr
    str += csprintf("pc : %16lx      ",diffAllStates->gem5RegFile.pc);
    if (diffInfo.errorCsrsValue[CsrRegIndex::mstatus])
        str += csprintf("\033[31mmstatus : %16lx\033[0m", diffAllStates->gem5RegFile.mstatus);
    else
        str += csprintf("mstatus : %16lx", diffAllStates->gem5RegFile.mstatus);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mcause])
        str += csprintf(" \033[31mmcause : %16lx\033[0m ", diffAllStates->gem5RegFile.mcause);
    else
        str += csprintf(" mcause : %16lx ", diffAllStates->gem5RegFile.mcause);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mepc])
        str += csprintf("\033[31mmepc    : %16lx\033[0m\n", diffAllStates->gem5RegFile.mepc);
    else
        str += csprintf("mepc    : %16lx\n", diffAllStates->gem5RegFile.mepc);

    str += csprintf("\t\t\t   sstatus : %16lx scause : %16lx",
        diffAllStates->gem5RegFile.sstatus,diffAllStates->gem5RegFile.scause);
    str += csprintf(" sepc    : %16lx\n", diffAllStates->gem5RegFile.sepc);

    if (diffInfo.errorCsrsValue[CsrRegIndex::satp])
        str += csprintf("\033[31msatp    : %16lx\033[0m\n", diffAllStates->gem5RegFile.satp);
    else
        str += csprintf("satp    : %16lx\n", diffAllStates->gem5RegFile.satp);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mip])
        str += csprintf("\033[31mmip     : %16lx\033[0m", diffAllStates->gem5RegFile.mip);
    else
        str += csprintf("mip     : %16lx", diffAllStates->gem5RegFile.mip);

    if (diffInfo.errorCsrsValue[CsrRegIndex::mie])
        str += csprintf(" \033[31mmie\033[0m     : %16lx ", diffAllStates->gem5RegFile.mie);
    else {
        str += csprintf(" mie     : %16lx ", diffAllStates->gem5RegFile.mie);
    }


    str += csprintf("mscratch: %16lx sscratch: %16lx\n",
        diffAllStates->gem5RegFile.mtval, diffAllStates->gem5RegFile.stval);
    str += csprintf("mideleg : %16lx medeleg : %16lx\n",
                diffAllStates->gem5RegFile.mideleg, diffAllStates->gem5RegFile.medeleg);

    str += csprintf("mtval   : %16lx stval   : %16lx ",
        diffAllStates->gem5RegFile.mtval, diffAllStates->gem5RegFile.stval);
    str += csprintf("mtvec   : %16lx stvec   : %16lx\n",
        diffAllStates->gem5RegFile.mtvec,diffAllStates->gem5RegFile.stvec);
    str += csprintf("privilege mode : %x\n", diffAllStates->gem5RegFile.mode);
    warn("gem5-CsrDisplay : \n%s",str) ;
    str.clear();

    //vector
    for (size_t i = 0; i < 32; i++)
    {
        str += csprintf("v%02d : ", i);
        for (int j=RiscvISA::NumVecElemPerVecReg-1; j>=0; j--) {
            str +=csprintf("%016lx", diffAllStates->gem5RegFile.vr[i]._64[j]);
            if (j != 0) {
                str+="_";
            }
            else {
                str+="\t";
            }
        }
        if (i%2 == 1)
            str +=csprintf("\n");
    }
    str += csprintf("vtype   : %16lx vstart   : %16lx  ",
                diffAllStates->gem5RegFile.vtype,diffAllStates->gem5RegFile.vstart);
    str += csprintf("vxsat   : %16lx\n", diffAllStates->gem5RegFile.vxsat);
    str += csprintf("vxrm    : %16lx vl       : %16lx  ",
                diffAllStates->gem5RegFile.vxrm,diffAllStates->gem5RegFile.vl);
    str += csprintf("vcsr    : %16lx\n", diffAllStates->gem5RegFile.vcsr);
    warn("gem5-VectorDisplay : \n%s\n\n", str);
    str.clear();

}

void
BaseCPU::difftestRaiseIntr(uint64_t no)
{
    diffAllStates->diff.will_handle_intr = true;
    diffAllStates->proxy->raise_intr(no);
}

void
BaseCPU::clearGuideExecInfo()
{
    diffAllStates->diff.guide.force_raise_exception = false;
    diffAllStates->diff.guide.force_set_jump_target = false;
}

void
BaseCPU::enableDiffPrint()
{
    diffAllStates->diff.dynamic_config.debug_difftest = true;
    diffAllStates->proxy->update_config(&diffAllStates->diff.dynamic_config);
}

void BaseCPU::setSCSuccess(bool success, paddr_t addr)
{
    diffAllStates->diff.sync.lrscValid = success;
    diffAllStates->diff.sync.lrscAddr = addr; // used for spike diff
}

void
BaseCPU::setExceptionGuideExecInfo(uint64_t exception_num, uint64_t mtval, uint64_t stval, bool force_set_jump_target,
                                   uint64_t jump_target, ThreadID tid)
{
    auto &gd = diffAllStates->diff.guide;
    gd.force_raise_exception = true;
    gd.exception_num = exception_num;
    gd.mtval = mtval;
    gd.stval = stval;
    gd.mtval2 = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_MTVAL2, tid);
    gd.htval = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_HTVAL, tid);
    gd.vstval = readMiscReg(RiscvISA::MiscRegIndex::MISCREG_VSTVAL, tid);
    gd.force_set_jump_target = force_set_jump_target;
    gd.jump_target = jump_target;

    // diffAllStates->diff.dynamic_config.debug_difftest = true;
    // diffAllStates->proxy->update_config(&diffAllStates->diff.dynamic_config);

    diffAllStates->proxy->guided_exec(&(diffAllStates->diff.guide));

    diffAllStates->proxy->regcpy(diffAllStates->diff.nemu_reg, REF_TO_DIFFTEST);
    diffAllStates->diff.nemu_this_pc = diffAllStates->diff.nemu_reg->pc;
    DPRINTF(Diff, "After guided exec on NEMU, new PC: %#lx\n", diffAllStates->diff.nemu_this_pc);

    // diffAllStates->diff.dynamic_config.debug_difftest = false;
    // diffAllStates->proxy->update_config(&diffAllStates->diff.dynamic_config);
}

void
BaseCPU::checkL1DRefill(Addr paddr, const uint8_t* refill_data, size_t size) {
    assert(size == 64);
    if (system->multiCore()) {
        uint8_t *golden_ptr = (uint8_t *)_goldenMemManager->guestToHost(paddr);
        if (memcmp(golden_ptr, refill_data, size)) {
            panic("Refill data diff with Golden addr %#lx with size %d\n", paddr, size);
        }
    }
}
} // namespace gem5
