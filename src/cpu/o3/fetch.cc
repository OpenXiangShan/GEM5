/*
 * Copyright (c) 2010-2014 ARM Limited
 * Copyright (c) 2012-2013 AMD
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

#include "cpu/o3/fetch.hh"

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <queue>

#include "arch/generic/tlb.hh"
#include "arch/riscv/decoder.hh"
#include "arch/riscv/pcstate.hh"
#include "base/debug_helper.hh"
#include "base/random.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/exetrace.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/trace/TraceFetch.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "cpu/valuepred/example_value_predictor_metadata.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/FetchFault.hh"
#include "debug/FetchVerbose.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
#include "debug/TraceReader.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port", _cpu), fetch(_fetch)
{}

void
Fetch::FetchTranslation::markMptDelayed()
{
    if (!mptDelayed) {
        mptDelayed = true;
        mptStartTick = curTick();
    }
}


Fetch::Fetch(CPU *_cpu, const BaseO3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      cpu(_cpu),
      branchPred(nullptr),
      resolveQueueSize(params.resolveQueueSize),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      decodeWidth(params.decodeWidth),
      retryPkt(),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchTranslationQueueSize(params.fetchTranslationQueueSize),
      fetchTranslationIssueWidth(params.fetchTranslationIssueWidth),
      fetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      icachePort(this, _cpu),
      finishTranslationEvent(this), fetchStats(_cpu, this),
      valuePred(params.valuePred)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));
    if (fetchTranslationQueueSize == 0 || fetchTranslationIssueWidth == 0) {
        fatal("Fetch translation queue size and issue width must be positive\n");
    }

    smtBorrowThrottleHoldCycles = params.smtBorrowThrottleCycles;
    for (int i = 0; i < MaxThreads; i++) {
        setThreadStatus(i, Idle);
        decoder[i] = nullptr;
        threads[i].fetchpc.reset(params.isa[0]->newPCState());
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        lastIcacheStall[i] = 0;
        smtBorrowThrottleCycles[i] = 0;
    }
    smtLdstqHighWater = params.smtBorrowLdstqHighWater;
    if (smtLdstqHighWater == 0) {
        smtLdstqHighWater =
            (params.LQEntries + params.SQEntries) *
            params.smtBorrowLdstqHighWaterPercent / 100;
    }

    branchPred = params.branchPred;

    // This fetch implementation only supports the decoupled frontend with the
    // decoupled BTB predictor. Fail fast to avoid silently using legacy paths.
    assert(branchPred);
    assert(branchPred->isDecoupled());
    assert(branchPred->isBTB());

    dbpbtb =
        dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB*>(
            branchPred);
    assert(dbpbtb);
    dbpbtb->setCpu(_cpu);

    assert(params.decoder.size());
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
        // Set the size and allocate data for each fetch buffer instance
        threads[tid].size = fetchBufferSize;
        threads[tid].data = new uint8_t[fetchBufferSize];
    }

    initDecodeScheduler();

    // Get the size of an instruction.
    // stallReason size should be the same as decodeWidth,renameWidth,dispWidth
    stallReason.resize(decodeWidth, StallReason::NoStall);

    traceFetch = std::make_unique<TraceFetch>(*this, params);

    if (isTraceMode() && traceFetch && !traceFetch->allowDecoupledFrontend()) {
        fatal("Trace mode requires allowDecoupledFrontend=true for decoupled+BTB-only fetch\n");
    }
}

Fetch::~Fetch() = default;

bool
Fetch::isTraceMode() const
{
    return traceFetch && traceFetch->enabled();
}

bool
Fetch::isTraceEOF() const
{
    return traceFetch && traceFetch->isEOF();
}

std::string Fetch::name() const { return cpu->name() + ".fetch"; }

void
Fetch::regProbePoints()
{
    ppFetch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Fetch");
    ppFetchRequestSent = new ProbePointArg<RequestPtr>(cpu->getProbeManager(),
                                                       "FetchRequest");

}

Fetch::FetchStatGroup::FetchStatGroup(CPU *cpu, Fetch *fetch)
    : statistics::Group(cpu, "fetch"),
    ADD_STAT(icacheStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch is stalled on an Icache miss"),
    ADD_STAT(insts, statistics::units::Count::get(),
             "Number of instructions fetch has processed"),
    ADD_STAT(branches, statistics::units::Count::get(),
             "Number of branches that fetch encountered"),
    ADD_STAT(predictedBranches, statistics::units::Count::get(),
             "Number of branches that fetch has predicted taken"),
    ADD_STAT(cycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has run and was not squashing or "
             "blocked"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent squashing"),
    ADD_STAT(tlbCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for tlb"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle"),
    ADD_STAT(smtidleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle per tid"),         
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
    ADD_STAT(smtblockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked per tid"),         
    ADD_STAT(miscStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on interrupts, or bad "
             "addresses, or out of MSHRs"),
    ADD_STAT(pendingDrainCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on pipes to drain"),
    ADD_STAT(noActiveThreadStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to no active thread to fetch from"),
    ADD_STAT(pendingTrapStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending traps"),
    ADD_STAT(pendingQuiesceStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending quiesce instructions"),
    ADD_STAT(icacheWaitRetryStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to full MSHR"),
    ADD_STAT(cacheLines, statistics::units::Count::get(),
             "Number of cache lines fetched"),
    ADD_STAT(icacheSquashes, statistics::units::Count::get(),
             "Number of outstanding Icache misses that were squashed"),
    ADD_STAT(tlbSquashes, statistics::units::Count::get(),
             "Number of outstanding ITLB misses that were squashed"),
    ADD_STAT(mptSquashes, statistics::units::Count::get(),
             "Number of outstanding instruction MPT checks that were squashed"),
    ADD_STAT(mptTranslations, statistics::units::Count::get(),
             "Number of fetch translations that entered the MPT pipeline"),
    ADD_STAT(mptWaitCycles, statistics::units::Cycle::get(),
             "Cycles spent waiting for instruction MPT checks"),
    ADD_STAT(nisnDist, statistics::units::Count::get(),
             "Number of instructions fetched each cycle (Total)"),
    ADD_STAT(idleRate, statistics::units::Ratio::get(),
             "Ratio of cycles fetch was idle",
             idleCycles / cpu->baseStats.numCycles),
    ADD_STAT(branchRate, statistics::units::Ratio::get(),
             "Number of branch fetches per cycle",
             branches / cpu->baseStats.numCycles),
    ADD_STAT(rate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of inst fetches per cycle",
             insts / cpu->baseStats.numCycles),
    ADD_STAT(fetchStatusDist, statistics::units::Count::get(),
             "Distribution of fetch status"),
    ADD_STAT(decodeStalls, statistics::units::Count::get(),
             "Number of decode stalls"),
    ADD_STAT(smtdecodeStalls, statistics::units::Count::get(),
             "Number of decode stalls per tid"),  
    ADD_STAT(smtftqempty, statistics::units::Count::get(),
             "Number of ftq empty per tid"),                  
    ADD_STAT(decodeStallRate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of decode stalls per cycle",
             decodeStalls / cpu->baseStats.numCycles),
    ADD_STAT(fetchBubbles, statistics::units::Count::get(),
             "Unutilized issue-pipeline slots while there is no backend-stall"),
    ADD_STAT(fetchBubbles_max, statistics::units::Count::get(),
             "Cycles that fetch 0 instruction while there is no backend-stall"),
    ADD_STAT(frontendBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Bound",
             fetchBubbles / (cpu->baseStats.numCycles * fetch->decodeWidth)),
    ADD_STAT(frontendLatencyBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Latency Bound",
             fetchBubbles_max / cpu->baseStats.numCycles),
    ADD_STAT(frontendBandwidthBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Bandwidth Bound",
             frontendBound - frontendLatencyBound),
    ADD_STAT(resolveQueueFullEvents, statistics::units::Count::get(),
             "Number of events the resolve queue becomes full"),
    ADD_STAT(resolveEnqueueFailEvent, statistics::units::Count::get(),
             "Number of times an entry could not be enqueued to the resolve queue"),
    ADD_STAT(resolveDequeueCount, statistics::units::Count::get(),
             "Number of times an entry is dequeued from the resolve queue"),
    ADD_STAT(resolveEnqueueCount, statistics::units::Count::get(),
             "Number of times an entry is enqueued to the resolve queue"),
    ADD_STAT(resolveQueueOccupancy, statistics::units::Count::get(),
             "Number of entries in the resolve queue"),
    ADD_STAT(traceMetaStores, statistics::units::Count::get(),
             "Number of stored trace metadata records (seqNum -> traceInst)"),
    ADD_STAT(traceMetaCleanupSquashCalls, statistics::units::Count::get(),
             "Number of times cleanup was called due to squash/rollback"),
    ADD_STAT(traceMetaCleanupSquashEntries, statistics::units::Count::get(),
             "Total entries erased by squash/rollback cleanups"),
    ADD_STAT(traceMetaCleanupCommitCalls, statistics::units::Count::get(),
             "Number of times cleanup was called on successful commit")
{
        icacheStallCycles
            .prereq(icacheStallCycles);
        insts
            .prereq(insts);
        branches
            .prereq(branches);
        predictedBranches
            .prereq(predictedBranches);
        cycles
            .prereq(cycles);
        squashCycles
            .prereq(squashCycles);
        tlbCycles
            .prereq(tlbCycles);
        idleCycles
            .prereq(idleCycles);
        blockedCycles
            .prereq(blockedCycles);
        cacheLines
            .prereq(cacheLines);
        miscStallCycles
            .prereq(miscStallCycles);
        pendingDrainCycles
            .prereq(pendingDrainCycles);
        noActiveThreadStallCycles
            .prereq(noActiveThreadStallCycles);
        pendingTrapStallCycles
            .prereq(pendingTrapStallCycles);
        pendingQuiesceStallCycles
            .prereq(pendingQuiesceStallCycles);
        icacheWaitRetryStallCycles
            .prereq(icacheWaitRetryStallCycles);
        icacheSquashes
            .prereq(icacheSquashes);
        tlbSquashes
            .prereq(tlbSquashes);
        mptSquashes
            .prereq(mptSquashes);
        mptTranslations
            .prereq(mptTranslations);
        mptWaitCycles
            .prereq(mptWaitCycles);
        nisnDist
            .init(/* base value */ 0,
              /* last value */ fetch->fetchWidth,
              /* bucket size */ 1)
            .flags(statistics::pdf);
        idleRate
            .prereq(idleRate);
        branchRate
            .flags(statistics::total);
        rate
            .flags(statistics::total);
        fetchStatusDist
            .init(NumFetchStatus)
            .flags(statistics::pdf | statistics::total);

        for (int i = 0; i < NumFetchStatus; i++) {
            fetchStatusDist.subname(i, fetch->fetchStatusStr[static_cast<Fetch::ThreadStatus>(i)]);
        }
        decodeStalls
            .prereq(decodeStalls);
        smtdecodeStalls
            .init(fetch->numThreads)
            .flags(statistics::total);  
        smtftqempty
            .init(fetch->numThreads)
            .flags(statistics::total);
        smtidleCycles
            .init(fetch->numThreads)
            .flags(statistics::total);
        smtblockedCycles
            .init(fetch->numThreads)
            .flags(statistics::total);     
        decodeStallRate
            .flags(statistics::total);
        fetchBubbles
            .prereq(fetchBubbles);
        fetchBubbles_max
            .prereq(fetchBubbles_max);
        frontendBound
            .flags(statistics::total);
        frontendLatencyBound
            .flags(statistics::total);
        frontendBandwidthBound
            .flags(statistics::total);
        resolveEnqueueCount
            .init(1, 8, 1);
        resolveQueueOccupancy
            .init(0, 32, 1);
        traceMetaStores
            .prereq(traceMetaStores);
        traceMetaCleanupSquashCalls
            .prereq(traceMetaCleanupSquashCalls);
        traceMetaCleanupSquashEntries
            .prereq(traceMetaCleanupSquashEntries);
        traceMetaCleanupCommitCalls
            .prereq(traceMetaCleanupCommitCalls);
}
void
Fetch::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
}

void
Fetch::initDecodeScheduler()
{
     // Initialize counters (same as before)
    lsqCounter = new InstsCounter();
    iqCounter  = new InstsCounter();
    robCounter = new InstsCounter();
    DPRINTF(Fetch, "Initialized SMT Decode Scheduler: 0\n");

    for (ThreadID tid = 0; tid < numThreads; tid++) 
    {
        lsqCounter->setCounter(tid, 0);
        iqCounter->setCounter(tid, 0);
        robCounter->setCounter(tid, 0);
    }
    DPRINTF(Fetch, "Initialized SMT Decode Scheduler: 1\n");
    
    if (smtDecodePolicy == "icount") {
        // Use ROB as default counter for icount
        decodeScheduler = new ICountScheduler(numThreads, robCounter);
    }
    else if (smtDecodePolicy == "delayed") {
        decodeScheduler = new DelayedICountScheduler(numThreads, robCounter, delayedSchedulerDelay);
    }
    else if (smtDecodePolicy == "multi_priority") {
        decodeScheduler = new MultiPrioritySched(numThreads, {lsqCounter, iqCounter, robCounter});
    }
    else {
        // Default: round-robin like (use delayed with thread cycling)
        decodeScheduler = new DelayedICountScheduler(numThreads, robCounter, numThreads);
    }

    DPRINTF(Fetch, "Initialized SMT Decode Scheduler: %s\n", smtDecodePolicy.c_str());
}

void
Fetch::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Fetch::setFetchQueue(TimeBuffer<FetchStruct> *ftb_ptr)
{
    // Create wire to write information to proper place in fetch time buf.
    toDecode = ftb_ptr->getWire(0);

    // initialize to toDecode stall vector
    toDecode->fetchStallReason = stallReason;
}

void
Fetch::startupStage()
{
    assert(priorityList.empty());
    resetStage();

    // Fetch needs to start fetching instructions at the very beginning,
    // so it must start up in active state.
    switchToActive();

    if (isTraceMode() && !traceFetch->initTraceMode()) {
        fatal("Failed to initialize trace mode\n");
    }
}

void
Fetch::clearStates(ThreadID tid)
{
    setThreadStatus(tid, Running);
    set(threads[tid].fetchpc, cpu->pcState(tid));
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    threads[tid].cacheReq.reset();
    fetchTranslationQueue[tid].clear();
    threads[tid].reset();
    fetchQueue[tid].clear();

    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);
}

void
Fetch::resetStage()
{
    numInst = 0;
    interruptPending = false;
    for (auto *pkt : retryPkt) {
        delete pkt;
    }
    retryPkt.clear();
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        setThreadStatus(tid, Running);
        set(threads[tid].fetchpc, cpu->pcState(tid));
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        threads[tid].cacheReq.reset();
        fetchTranslationQueue[tid].clear();

        threads[tid].reset();
        ftqEntryFetchedInsts[tid] = 0;

        fetchQueue[tid].clear();

        priorityList.push_back(tid);
        waitForVsetvl[tid] = false;
        smtBorrowThrottleCycles[tid] = 0;
    }

    wroteToTimeBuffer = false;
    _status = Inactive;

    if (traceFetch) {
        traceFetch->resetStage();
    }

    assert(dbpbtb);
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        dbpbtb->resetPC(tid, threads[tid].fetchpc->instAddr());
    }
}

bool
Fetch::handleMultiCacheLineFetch(Addr vaddr, ThreadID tid, Addr pc)
{
    DPRINTF(Fetch,
            "[tid:%i] Waiting for queued fetch translation at %#x, pc=%#lx\n",
            tid, vaddr, pc);
    setThreadStatus(tid, WaitingCache);
    serviceFetchTranslationQueue(tid);
    if (threads[tid].cacheReq.getOverallStatus() == CacheIdle) {
        setAllFetchStalls(StallReason::ITlbStall);
    }
    return true;
}

void
Fetch::serviceFetchTranslationQueue(ThreadID tid)
{
    if (!dbpbtb || !dbpbtb->ftqHasFetching(tid)) {
        return;
    }

    auto &queue = fetchTranslationQueue[tid];
    const uint64_t head_id = dbpbtb->ftqHeadId(tid);
    while (!queue.empty() && queue.front().targetId < head_id) {
        queue.pop_front();
    }
    if (!queue.empty() && queue.front().targetId > head_id) {
        queue.clear();
    }

    uint64_t next_id = queue.empty() ? head_id : queue.back().targetId + 1;
    while (queue.size() < fetchTranslationQueueSize &&
           dbpbtb->ftqHasTarget(next_id, tid)) {
        const auto &target = dbpbtb->ftqTarget(next_id, tid);
        FetchTranslationBlock block;
        block.targetId = next_id;
        block.baseAddr = target.startPC;

        Addr fetch_pc = target.startPC;
        unsigned fetch_size = cacheBlkSize - fetch_pc % cacheBlkSize;
        block.requests[0] = std::make_shared<Request>(
            fetch_pc, fetch_size, Request::INST_FETCH,
            cpu->instRequestorId(), target.startPC,
            cpu->thread[tid]->contextId());
        block.requests[0]->taskId(cpu->taskId());
        block.requests[0]->setMisalignedFetch();
        block.requests[0]->setReqNum(1);

        fetch_pc += fetch_size;
        assert(fetch_pc % cacheBlkSize == 0);
        fetch_size = fetchBufferSize - fetch_size;
        panic_if(fetch_size == 0,
                 "Fetch translation block must span two cache lines\n");
        block.requests[1] = std::make_shared<Request>(
            fetch_pc, fetch_size, Request::INST_FETCH,
            cpu->instRequestorId(), target.startPC,
            cpu->thread[tid]->contextId());
        block.requests[1]->taskId(cpu->taskId());
        block.requests[1]->setMisalignedFetch();
        block.requests[1]->setReqNum(2);

        queue.push_back(std::move(block));
        ++next_id;
    }

    for (auto &block : queue) {
        for (unsigned request = 0; request < block.requests.size() &&
             fetchTranslationsIssuedThisCycle < fetchTranslationIssueWidth;
             ++request) {
            if (block.issued[request]) {
                continue;
            }
            block.issued[request] = true;
            ++fetchTranslationsIssuedThisCycle;
            auto *translation = new FetchTranslation(this);
            cpu->mmu->translateTiming(
                block.requests[request], cpu->thread[tid]->getTC(),
                translation, BaseMMU::Execute);
        }
        if (fetchTranslationsIssuedThisCycle >=
            fetchTranslationIssueWidth) {
            break;
        }
    }

    activateFetchTranslationHead(tid);
}

bool
Fetch::activateFetchTranslationHead(ThreadID tid)
{
    auto &queue = fetchTranslationQueue[tid];
    if (queue.empty() || !dbpbtb || !dbpbtb->ftqHasFetching(tid)) {
        return false;
    }

    FetchTranslationBlock &block = queue.front();
    if (block.targetId != dbpbtb->ftqHeadId(tid) || block.activated ||
        !block.allCompleted() || threads[tid].valid) {
        return false;
    }

    CacheRequestStatus status = threads[tid].cacheReq.getOverallStatus();
    if (status != CacheIdle && status != AccessComplete) {
        return false;
    }

    threads[tid].cacheReq.reset();
    threads[tid].cacheReq.baseAddr = block.baseAddr;
    threads[tid].cacheReq.totalSize = fetchBufferSize;
    for (const auto &request : block.requests) {
        threads[tid].cacheReq.addRequest(request);
        updateCacheRequestStatusByRequest(tid, request, TlbWait);
    }

    for (unsigned request = 0; request < block.requests.size(); ++request) {
        if (block.faults[request] != NoFault) {
            handleTranslationFault(
                tid, block.requests[request], block.faults[request]);
            if (!finishTranslationEvent.scheduled()) {
                block.activated = true;
            }
            return true;
        }
    }

    block.activated = true;
    for (const auto &request : block.requests) {
        handleSuccessfulTranslation(tid, request, block.baseAddr);
    }
    return true;
}

bool
Fetch::waitingForFetchTranslation(ThreadID tid) const
{
    if (!dbpbtb || !dbpbtb->ftqHasFetching(tid) ||
        fetchTranslationQueue[tid].empty()) {
        return false;
    }
    const auto &block = fetchTranslationQueue[tid].front();
    return block.targetId == dbpbtb->ftqHeadId(tid) &&
           !block.activated;
}

bool
Fetch::processMultiCacheLineCompletion(ThreadID tid, PacketPtr pkt)
{
    DPRINTF(Fetch, "[tid:%i] Processing dual cacheline fetch completion for addr %#lx.\n",
            tid, pkt->getAddr());

    // Mark this packet as completed in the cache request (this also stores the packet)
    bool found_packet = threads[tid].cacheReq.markCompletedAndStorePacket(pkt);
    if (!found_packet) {
        DPRINTF(Fetch, "[tid:%i] Packet doesn't match current requests, deleting pkt %#lx\n",
                tid, pkt->getAddr());
        DPRINTF(Fetch, "[tid:%i] Expected requests: ", tid);
        for (size_t i = 0; i < threads[tid].cacheReq.requests.size(); i++) {
            DPRINTF(Fetch, "req[%d]=0x%lx ", i, threads[tid].cacheReq.requests[i]->getVaddr());
        }
        DPRINTF(Fetch, "\n");
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] Packet successfully matched and stored. Current status: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Check if we're still waiting for other packets
    if (!threads[tid].cacheReq.allCompleted()) {
        DPRINTF(Fetch, "[tid:%i] Waiting for remaining packets. Completed: %d, Total: %d\n",
                tid, threads[tid].cacheReq.completedPackets, threads[tid].cacheReq.packets.size());

        bool waitingOnRetry = false;
        for (const auto status : threads[tid].cacheReq.requestStatus) {
            if (status == CacheWaitRetry) {
                waitingOnRetry = true;
                break;
            }
        }

        if (waitingOnRetry && cacheBlocked && !retryPkt.empty()) {
            PacketPtr queuedPkt = retryPkt.front();
            const ThreadID queuedTid =
                cpu->contextToThread(queuedPkt->req->contextId());
            const bool sameThreadRetry = queuedTid == tid &&
                threads[tid].cacheReq.findRequestIndex(queuedPkt->req) != SIZE_MAX;

            if (sameThreadRetry && icachePort.sendTimingReq(queuedPkt)) {
                DPRINTF(Fetch,
                        "[tid:%i] Retrying matching queued I-cache packet %#lx "
                        "after sibling response\n",
                        tid, queuedPkt->req->getVaddr());
                updateCacheRequestStatusByRequest(tid, queuedPkt->req,
                                                  CacheWaitResponse);
                ppFetchRequestSent->notify(queuedPkt->req);
                retryPkt.erase(retryPkt.begin());
                if (retryPkt.empty()) {
                    cacheBlocked = false;
                }
            }
        }

        return false;  // Return false to indicate we're still waiting
    }

    // All packets have arrived - merge them directly into fetchBuffer
    DPRINTF(Fetch, "[tid:%i] All packets arrived, merging data into fetchBuffer.\n", tid);

    // Find the packets by request number
    PacketPtr firstPkt = nullptr;
    PacketPtr secondPkt = nullptr;

    for (size_t i = 0; i < threads[tid].cacheReq.packets.size(); i++) {
        if (threads[tid].cacheReq.requests[i]->getReqNum() == 1) {
            firstPkt = threads[tid].cacheReq.packets[i];
        } else if (threads[tid].cacheReq.requests[i]->getReqNum() == 2) {
            secondPkt = threads[tid].cacheReq.packets[i];
        }
    }

    assert(firstPkt && secondPkt);

    // Copy merged data directly into fetchBuffer
    memcpy(threads[tid].data, firstPkt->getConstPtr<uint8_t>(), firstPkt->getSize());
    memcpy(threads[tid].data + firstPkt->getSize(), secondPkt->getConstPtr<uint8_t>(), secondPkt->getSize());
    threads[tid].valid = true;

    // Clean up the packets
    delete firstPkt;
    delete secondPkt;

    DPRINTF(Fetch, "[tid:%i] Dual cacheline fetch completion processed successfully.\n", tid);
    return true;
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());
    assert(pkt->req->isMisalignedFetch() && "Only multi-cacheline fetch is supported");

    bool allCompleted = processMultiCacheLineCompletion(tid, pkt);
    // If we're still waiting for another packet, return early
    if (!allCompleted) {
        return;
    }

    // Check if this completion should be processed
    // Either thread is waiting for cache, or cache just completed
    CacheRequestStatus cacheStatus = threads[tid].cacheReq.getOverallStatus();
    if (!hasPendingCacheRequests(tid) && cacheStatus != AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Thread not waiting for cache and no completion, ignoring\n", tid);
        ++fetchStats.icacheSquashes;
        return;
    }

    // Data has been merged into fetchBuffer, we can proceed
    DPRINTF(Fetch, "[tid:%i] All misaligned packets received and merged.\n", tid);

    assert(!cpu->switchedOut());

    // Trace 按需消费：不在 icache 完成时写入 trace 指令码，避免批量消费。
    if (isTraceMode()) {
        DPRINTF(TraceReader,
                "[TRACE] Icache completion: keep timing only; no trace bytes injection\n");
    }

    // Verify fetchBufferPC alignment with the supplying FSQ entry.
    if (threads[tid].valid && dbpbtb->ftqHasFetching(tid)) {
        const auto &stream = dbpbtb->ftqFetchingTarget(tid);
        if (threads[tid].startPC != stream.startPC) {
            panic("fetchBufferPC %#x should be aligned with FSQ startPC %#x",
                  threads[tid].startPC, stream.startPC);
        }
    }

    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    cpu->wakeCPU();

    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
            tid);

    switchToActive();

    // Transition from WaitingCache back to Running when cache access completes
    setThreadStatus(tid, Running);
}

void
Fetch::drainResume()
{
}

void
Fetch::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPkt.size() == 0);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(threads[i].cacheReq.packets.empty());
        assert(fetchStatus[i] == Idle);
    }

    branchPred->drainSanityCheck();
}

bool
Fetch::isDrained() const
{
    /* Make sure that threads are either idle of that the commit stage
     * has signaled that draining has completed by setting the drain
     * stall flag. This effectively forces the pipeline to be disabled
     * until the whole system is drained (simulation may continue to
     * drain other components).
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify fetch queues are drained
        if (!fetchQueue[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (fetchStatus[i] != Idle) {
            return false;
        }
    }

    /* The pipeline might start up again in the middle of the drain
     * cycle if the finish translation event is scheduled, so make
     * sure that's not the case.
     */
    return !finishTranslationEvent.scheduled();
}

void
Fetch::takeOverFrom()
{
    assert(cpu->getInstPort().isConnected());
    resetStage();

}

void
Fetch::drainStall(ThreadID tid)
{
}

void
Fetch::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    setThreadStatus(0, Running);
}

void
Fetch::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::FetchIdx);

        _status = Active;
    }
}

void
Fetch::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);

        _status = Inactive;
    }
}

void
Fetch::deactivateThread(ThreadID tid)
{
    // Update priority list
    auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    if (thread_it != priorityList.end()) {
        priorityList.erase(thread_it);
    }
}

bool
Fetch::lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc)
{
    // Do branch prediction check here.
    // A bit of a misnomer...next_PC is actually the current PC until
    // this function updates it.
    bool predict_taken = false;

    // Decoupled+BTB-only: compute next PC directly from the supplying FSQ entry.
    ThreadID tid = inst->threadNumber;
    assert(dbpbtb);
    assert(dbpbtb->ftqHasFetching(tid));
    const auto &stream = dbpbtb->ftqFetchingTarget(tid);

    const Addr curr_pc = next_pc.instAddr();
    assert(stream.startPC <= curr_pc && curr_pc < stream.predEndPC);

    bool run_out = false;

    // Taken when the current PC matches the predicted control PC.
    predict_taken = stream.predTaken && (curr_pc == stream.predBranchInfo.pc);
    if (predict_taken) {
        auto &rpc = next_pc.as<GenericISA::PCStateWithNext>();
        rpc.pc(stream.predBranchInfo.target);
        rpc.npc(stream.predBranchInfo.target + 4);
        rpc.uReset();
        run_out = true;
    } else if (inst->staticInst->isMicroop()) {
        // Microops must advance uPC explicitly; they do not rely on decoder NPC.
        inst->staticInst->advancePC(next_pc);
        run_out = next_pc.instAddr() >= stream.predEndPC;
    } else {
        // Sequential fetch: decoder already computed npc with correct inst size.
        auto &rpc = next_pc.as<RiscvISA::PCState>();
        const Addr fall_thru = rpc.npc();
        rpc.pc(fall_thru);
        // Placeholder; decoder will overwrite npc on the next decode.
        rpc.npc(fall_thru + 4);
        rpc.uReset();
        run_out = fall_thru >= stream.predEndPC;
    }

    // Track how many dynamic instructions were fetched for this (legacy) FTQ/FSQ entry.
    ftqEntryFetchedInsts[tid]++;
    if (run_out) {
        dbpbtb->consumeFetchTarget(ftqEntryFetchedInsts[tid], tid);
        ftqEntryFetchedInsts[tid] = 0;
        threads[tid].valid = false;
        DPRINTF(DecoupleBP, "Used up fetch targets.\n");
    }

    inst->setLoopIteration(currentLoopIter);

    // For decoupled frontend, the instruction type is predicted with BTB
    if (!predict_taken) {
        inst->setPredTarg(next_pc);
        inst->setPredTaken(false);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x predicted to be taken to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
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

bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc)
{
    assert(!cpu->switchedOut());

    // Check for blocking conditions
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n", tid);
        setAllFetchStalls(StallReason::IcacheStall);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n", tid);
        setAllFetchStalls(StallReason::IntStall);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x, pc=%#lx\n",
            tid, vaddr, vaddr, pc);

    // With 66-byte fetchBufferSize, we always need to access 2 cache lines
    return handleMultiCacheLineFetch(vaddr, tid, pc);
}

bool
Fetch::validateTranslationRequest(ThreadID tid, const RequestPtr &mem_req,
                                   bool mpt_delayed)
{
    for (const auto &block : fetchTranslationQueue[tid]) {
        for (const auto &request : block.requests) {
            if (mem_req == request) {
                return true;
            }
        }
    }
    DPRINTF(Fetch,
            "[tid:%i] Ignoring translation completed after squash for %#lx\n",
            tid, mem_req->getVaddr());
    if (mpt_delayed) {
        ++fetchStats.mptSquashes;
    } else {
        ++fetchStats.tlbSquashes;
    }
    return false;
}

void
Fetch::handleSuccessfulTranslation(ThreadID tid, const RequestPtr &mem_req, Addr fetchPC)
{
    // Check that we're not going off into random memory
    if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
        DPRINTF(Fetch, "Address %#x is outside of physical memory, stopping fetch, %lu\n",
                mem_req->getPaddr(), curTick());

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed);
        setAllFetchStalls(StallReason::OtherFetchStall);
        // Note: Don't reset here, let the caller handle cleanup based on overall status
        return;
    }

    // Build packet here.
    PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
    data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);
    // All requests are multi-cacheline, always set send right away
    data_pkt->setSendRightAway();

    DPRINTF(Fetch, "[tid:%i] Fetching data for addr %#x, pc=%#lx\n",
                tid, mem_req->getVaddr(), fetchPC);

    threads[tid].startPC = fetchPC;
    threads[tid].valid = false;
    DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

    fetchStats.cacheLines++;

    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] I-cache port already waiting for retry, queueing %#lx\n",
                tid, mem_req->getVaddr());

        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitRetry);
        setAllFetchStalls(StallReason::IcacheStall);
        retryPkt.push_back(data_pkt);
        return;
    }

    // Access the cache.
    if (!icachePort.sendTimingReq(data_pkt)) {
        DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitRetry);
        data_pkt->setRetriedPkt();
        DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                mem_req->getVaddr());
        setAllFetchStalls(StallReason::IcacheStall);
        retryPkt.push_back(data_pkt);
        cacheBlocked = true;
    } else {
        DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
        DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache response.\n", tid);
        lastIcacheStall[tid] = curTick();

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitResponse);
        setAllFetchStalls(StallReason::IcacheStall);
        // Notify Fetch Request probe when a packet containing a fetch request is successfully sent
        ppFetchRequestSent->notify(mem_req);
    }
}

void
Fetch::handleTranslationFault(ThreadID tid, const RequestPtr &mem_req, const Fault &fault)
{
    DPRINTF(FetchFault, "fault, mem_req.addr=%#lx\n", mem_req->getVaddr());

    // Don't send an instruction to decode if we can't handle it.
    if (!(numInst < fetchWidth) || !(fetchQueue[tid].size() < fetchQueueSize)) {
        if (finishTranslationEvent.scheduled() && finishTranslationEvent.getReq() != mem_req) {
            DPRINTF(FetchFault, "fault, finishTranslationEvent.getReq().addr=%#lx, mem_req.addr=%#lx\n",
                    finishTranslationEvent.getReq()->getVaddr(), mem_req->getVaddr());
            return;
        }
        assert(!finishTranslationEvent.scheduled());
        finishTranslationEvent.setFault(fault);
        finishTranslationEvent.setReq(mem_req);
        cpu->schedule(finishTranslationEvent, cpu->clockEdge(Cycles(1)));
        return;
    }

    DPRINTF(Fetch, "[tid:%i] Got back req with addr %#x but expected base addr %#x\n",
            tid, mem_req->getVaddr(), threads[tid].cacheReq.baseAddr);

    // Update new cache request status system
    updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed);

    // Translation faulted, icache request won't be sent.
    threads[tid].cacheReq.reset();

    // Send the fault to commit.  This thread will not do anything
    // until commit handles the fault.  The only other way it can
    // wake up is if a squash comes along and changes the PC.
    const PCStateBase &fetch_pc = *threads[tid].fetchpc;

    DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
    // We will use a nop in order to carry the fault.
    DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
            fetch_pc, fetch_pc, false);
    instruction->setVersion(localSquashVer[tid]);
    instruction->setNotAnInst();

    instruction->setPredTarg(fetch_pc);
    instruction->fault = fault;
    std::unique_ptr<PCStateBase> next_pc(fetch_pc.clone());
    instruction->staticInst->advancePC(*next_pc);
    set(instruction->predPC, next_pc);

    wroteToTimeBuffer = true;

    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();

    setThreadStatus(tid, TrapPending);
    setAllFetchStalls(StallReason::TrapStall);

    DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
    DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
            tid, fault->name(), *threads[tid].fetchpc);
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req,
                          bool mpt_delayed, Tick mpt_start_tick)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    DPRINTF(Fetch, "[tid:%i] Translation completed for addr %#lx\n",
            tid, mem_req->getVaddr());

    // Validate if this request should be processed
    if (mpt_delayed) {
        ++fetchStats.mptTranslations;
        fetchStats.mptWaitCycles += cpu->ticksToCycles(
            curTick() - mpt_start_tick);
    }

    if (!validateTranslationRequest(tid, mem_req, mpt_delayed)) {
        return;
    }

    bool found = false;
    for (auto &block : fetchTranslationQueue[tid]) {
        for (unsigned request = 0; request < block.requests.size(); ++request) {
            if (block.requests[request] == mem_req) {
                block.faults[request] = fault;
                block.completed[request] = true;
                block.mptDelayed[request] = mpt_delayed;
                found = true;
                break;
            }
        }
        if (found) {
            break;
        }
    }
    panic_if(!found, "Validated fetch translation request disappeared\n");
    switchToActive();
}

void
Fetch::doSquash(PCStateBase &new_pc, const DynInstPtr squashInst, const InstSeqNum seqNum,
        ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s. seqNum: %lu\n",
            tid, new_pc, seqNum);
    if (squashInst) {
        DPRINTF(Fetch, "[tid:%i] Squash caused by inst at PC: %s, seqNum: %lu\n",
                tid, squashInst->pcState(), squashInst->seqNum);
    }

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
        set(threads[tid].fetchpc, new_pc);
        DPRINTF(Fetch, "[tid:%i] pc is misaligned, aligned to %#lx\n", tid, new_pc.instAddr());
    } else {
        set(threads[tid].fetchpc, new_pc);
    }
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    DPRINTF(Fetch, "[tid:%i] Squash: clear cacheReq, current fetchStatus[tid]=%d\n", tid, fetchStatus[tid]);

    // Cancel all active cache requests in new status system
    threads[tid].cacheReq.cancelAllRequests();
    DPRINTF(Fetch, "[tid:%i] Squash: cancelled all cache requests, status: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Reset the cache request after cancelling
    threads[tid].cacheReq.reset();
    fetchTranslationQueue[tid].clear();

    // Drop any retry packets that belong to this squashed thread.
    for (auto it = retryPkt.begin(); it != retryPkt.end();) {
        if (cpu->contextToThread((*it)->req->contextId()) == tid) {
            delete *it;
            it = retryPkt.erase(it);
        } else {
            ++it;
        }
    }
    if (retryPkt.empty()) {
        cacheBlocked = false;
    }

    if (squashInst && !squashInst->isControl()) {
        // csrrw satp need to flush all fetch targets
        threads[tid].valid = false;
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

    // Force a new I-cache request for the next FTQ head after squash.
    threads[tid].valid = false;
    ftqEntryFetchedInsts[tid] = 0;

    if (traceFetch) {
        traceFetch->handleTraceSquash(tid, new_pc, squashInst, seqNum);
    }

    ++fetchStats.squashCycles;
}

void
Fetch::flushFetchBuffer()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        threads[i].valid = false;
    }
}

Addr
Fetch::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    assert(dbpbtb);
    return dbpbtb->getPreservedReturnAddr(dynInst);
}

void
Fetch::squashFromDecode(PCStateBase &new_pc, const DynInstPtr squashInst,
        const InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    //Check Running
    std::list<ThreadID>::iterator act_tid = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (act_tid != end) {
        ThreadID tid = *act_tid++;

        if (canFetchInstructions(tid) || fetchStatus[tid] == Squashing ||
            threads[tid].cacheReq.getOverallStatus() == AccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
                    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                            "completion\n",tid);
                }

                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

void
Fetch::squash(PCStateBase &new_pc, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);
    assert(new_pc.instAddr() % 2 == 0 && "squash PC should be 2-byte aligned");

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}

void
Fetch::tick()
{
    // Initialize state for this tick cycle
    bool status_change = initializeTickState();

    // Simple decoupled+BTB ordering:
    // - first consume incoming squashes/redirects (in initializeTickState())
    // - then advance predictor pipeline + try to supply an FTQ head
    // - then run fetch using the supplied FTQ entry (if any)
    assert(dbpbtb);
    dbpbtb->tick();
    fetchTranslationsIssuedThisCycle = 0;
    for (ThreadID offset = 0; offset < numThreads; ++offset) {
        const ThreadID tid =
            (fetchTranslationNextThread + offset) % numThreads;
        serviceFetchTranslationQueue(tid);
    }
    fetchTranslationNextThread =
        (fetchTranslationNextThread + 1) % numThreads;

    // Perform fetch operations and instruction delivery
    fetchAndProcessInstructions(status_change);
}

bool
Fetch::initializeTickState()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;
    setAllFetchStalls(StallReason::NoStall);

    // get the distribution of fetch status
    fetchStats.fetchStatusDist[fetchStatus[0]]++;

    // Check signal updates for all active threads
    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change =  status_change || updated_status;
        if (fromCommit->commitInfo[tid].emptyROB) {
            waitForVsetvl[tid] = false;
        }
    }

    DPRINTF(Fetch, "Running stage.\n");
    return status_change;
}

void
Fetch::fetchAndProcessInstructions(bool status_change)
{
    // Fetch instructions from active threads
    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        // Fetch each of the actively fetching threads.
        fetch(status_change);
    }

    // Pass stall reasons to decode stage
    toDecode->fetchStallReason = stallReason;

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }

    // Handle interrupt processing in full system mode
    handleInterrupts();

    // Send instructions to decode stage, update stall reasons and measure frontend bubbles.
    sendInstructionsToDecode();
}

void
Fetch::handleInterrupts()
{
    if (FullSystem) {
        if (fromCommit->commitInfo[0].interruptPending) {
            DPRINTF(Fetch, "Set interrupt pending.\n");
            interruptPending = true;
        }

        if (fromCommit->commitInfo[0].clearInterrupt) {
            DPRINTF(Fetch, "Clear interrupt pending.\n");
            interruptPending = false;
        }
    }
}

ThreadID
Fetch::selectUnstalledThread()
{
    ThreadID selected = InvalidThreadID;
    bool has_candidate = false;
    bool has_unthrottled_candidate = false;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        const bool candidate = !stallSig->blockFetch[tid] &&
                               !fetchQueue[tid].empty();
        if (!candidate) {
            smtBorrowThrottleCycles[tid] = 0;
            lsqCounter->setCounter(tid, UINT64_MAX);
            iqCounter->setCounter(tid, UINT64_MAX);
            robCounter->setCounter(tid, UINT64_MAX);
            continue;
        }
        has_candidate = true;

        const bool throttle_now =
            smtHasBorrowThrottleStall(fromIEW->iewInfo[tid]) ||
            smtHasMemoryPressure(fromIEW->iewInfo[tid], smtLdstqHighWater);
        if (throttle_now) {
            smtBorrowThrottleCycles[tid] = smtBorrowThrottleHoldCycles;
        } else if (smtBorrowThrottleCycles[tid] > 0) {
            --smtBorrowThrottleCycles[tid];
        }

        const bool throttled = smtBorrowThrottleCycles[tid] > 0;
        if (!throttled) {
            has_unthrottled_candidate = true;
        }

        lsqCounter->setCounter(
            tid, throttled ? UINT64_MAX : fromIEW->iewInfo[tid].ldstqCount);
        iqCounter->setCounter(
            tid, throttled ? UINT64_MAX : fromIEW->iewInfo[tid].iqCount);
        robCounter->setCounter(
            tid, throttled ? UINT64_MAX : fromIEW->iewInfo[tid].robCount);

        DPRINTF(Fetch,
                "[tid:%i] lsq=%u iq=%u rob=%u throttled=%u mem_pressure=%u hold=%u\n",
                tid, fromIEW->iewInfo[tid].ldstqCount,
                fromIEW->iewInfo[tid].iqCount, fromIEW->iewInfo[tid].robCount,
                throttled,
                smtHasMemoryPressure(fromIEW->iewInfo[tid], smtLdstqHighWater),
                smtBorrowThrottleCycles[tid]);
    }

    if (has_candidate && !has_unthrottled_candidate) {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            if (stallSig->blockFetch[tid] || fetchQueue[tid].empty()) {
                continue;
            }
            lsqCounter->setCounter(tid, fromIEW->iewInfo[tid].ldstqCount);
            iqCounter->setCounter(tid, fromIEW->iewInfo[tid].iqCount);
            robCounter->setCounter(tid, fromIEW->iewInfo[tid].robCount);
        }
    }

    if (has_candidate) {
        selected = decodeScheduler->getThread();
    }

    return selected;
}

void
Fetch::sendInstructionsToDecode()
{

    // Reset the number of instructions we've fetched
    numInst = 0;

    bool any_thread_active = false;
    for (int i = 0; i < numThreads; i++) {
        if (!stallSig->blockFetch[i]) {
            any_thread_active = true;
            //break;
        }else{
            fetchStats.smtdecodeStalls[i]++; 
        }
    }

    if (!any_thread_active) {
        // All threads are blocked, no instructions to send
        ThreadID blocked_tid = InvalidThreadID;
        for (int i = 0; i < numThreads; i++) {
            if (stallSig->blockFetch[i]) {
                blocked_tid = i;
                break;
            }
        }

        if (blocked_tid != InvalidThreadID) {
            setAllFetchStalls(stallSig->fetchBlockReason[blocked_tid]);
        }

        toDecode->fetchStallReason = stallReason;

        for (int i = 0; i < numThreads; i++) {
            measureFrontendBubbles(0, i);
        }
        return;
    }

    ThreadID tid =selectUnstalledThread();

    if(tid == -1)
    {
        DPRINTF(Fetch, "All threads are stalled, no thread selected.\n");
        return;
    }
    DPRINTF(Fetch, "select Unstalled [tid:%i]\n",tid);

    // fetch totally stalled
    if (stallSig->blockFetch[tid]) {
        // If decode stalled, use decode's stall reason
        DPRINTF(Fetch, "[tid:%i] Fetch stalled\n", tid);
        setAllFetchStalls(stallSig->fetchBlockReason[tid]);
    }

    int insts_to_decode = 0;
    auto& insts = fetchQueue[tid];
    while (!insts.empty() && insts_to_decode < decodeWidth) {
        const auto& inst = insts.front();
        toDecode->insts[toDecode->size++] = inst;
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                "from fetch queue. Fetch queue size: %i.\n",
                tid, inst->seqNum, insts.size());

        wroteToTimeBuffer = true;
        insts.pop_front();
        insts_to_decode++;
    }

    // Update stall reasons based on fetch/decode status
    updateStallReasons(insts_to_decode, tid);

    // Intel TopDown method for measuring frontend bubbles
    measureFrontendBubbles(insts_to_decode, tid);

    // If there was activity this cycle, inform the CPU of it
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
Fetch::updateStallReasons(unsigned insts_to_decode, ThreadID tid)
{
    if (stallSig->blockFetch[tid]) {
        setAllFetchStalls(stallSig->fetchBlockReason[tid]);
    } else if (insts_to_decode == 0) {
        // fetch stalled
        if (stallReason[0] != StallReason::NoStall) {
            // previously set stall reason
            setAllFetchStalls(stallReason[0]);
        } else {
            setAllFetchStalls(StallReason::OtherFetchStall);
        }
    } else {
        // fetch partially stalled or no stall
        for (int i = 0; i < stallReason.size(); i++) {
            if (i < insts_to_decode)
                stallReason[i] = StallReason::NoStall;
            else {
                stallReason[i] = StallReason::FetchFragStall;
            }
        }
    }

    toDecode->fetchStallReason = stallReason;
}

void
Fetch::measureFrontendBubbles(unsigned insts_to_decode, ThreadID tid)
{
    // Intel TopDown method for measuring frontend bubbles
    // Count unutilized issue slots when backend is not stalled (decode not stalled)
    // For N-wide machine, if frontend supplies 0 instructions:
    // - fetchBubbles += N (count total empty slots)
    // - fetchBubbles_max += 1 (count occurrence of all slots being empty)
    if (!stallSig->blockFetch[tid] && !fromCommit->commitInfo[tid].robSquashing) {
        // backend not stalled
        int unused_slots = decodeWidth - insts_to_decode;
        if (unused_slots > 0) {
            // has empty slots
            fetchStats.fetchBubbles += unused_slots; // add number of empty slots
            if (unused_slots == decodeWidth) {
                // all slots empty, insts_to_decode == 0
                fetchStats.fetchBubbles_max++; // count max bubble occurrence
            }
        }
    }

    if (stallSig->blockFetch[tid]) {
        fetchStats.decodeStalls++;
        //fetchStats.smtdecodeStalls[tid]++;
    }
}

bool
Fetch::checkSignalsAndUpdate(ThreadID tid)
{
    // Check squash signals from commit.
    bool commitSquashed = handleCommitSignals(tid);

    handleIEWSignals();

    if (commitSquashed) {
        return true;
    }

    if (handleDecodeSquash(tid)) {
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

    // Handle WaitingCache state: check if cache request is complete
    if (fetchStatus[tid] == WaitingCache &&
        threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
        // Cache access completed, transition to Running
        setThreadStatus(tid, Running);
        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return false;
}

void
Fetch::handleIEWSignals()
{
    // Currently resolve stage training is a btb-only feature
    if (!isBTBPred()) {
        return;
    }

    const bool had_pending_resolve = !resolveQueue.empty();
    uint8_t enqueueCount = 0;
    uint8_t enqueueSize = 0;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        enqueueSize += fromIEW->iewInfo[tid].resolvedCFIs.size();
    }

    if (resolveQueueSize && resolveQueue.size() > resolveQueueSize - 4) {
        fetchStats.resolveQueueFullEvents++;
        fetchStats.resolveEnqueueFailEvent += enqueueSize;
    } else {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            auto &incoming = fromIEW->iewInfo[tid].resolvedCFIs;
            for (const auto &resolved : incoming) {
                bool merged = false;
                for (auto &queued : resolveQueue) {
                    if (queued.resolvedTid == tid &&
                        queued.resolvedFTQId == resolved.ftqId) {
                        queued.resolvedInstPC.push_back(resolved.pc);
                        merged = true;
                        break;
                    }
                }

                if (merged) {
                    continue;
                }

                ResolveQueueEntry new_entry;
                new_entry.resolvedTid = tid;
                new_entry.resolvedFTQId = resolved.ftqId;
                new_entry.resolvedInstPC.push_back(resolved.pc);
                resolveQueue.push_back(std::move(new_entry));
                enqueueCount++;
            }
        }
        fetchStats.resolveEnqueueCount.sample(enqueueCount);
    }

    fetchStats.resolveQueueOccupancy.sample(resolveQueue.size());

    // Process only entries that were already pending before this cycle.
    // This preserves a cycle of separation between IEW producing resolved CFIs
    // and fetch consuming them as predictor resolved updates.
    if (had_pending_resolve && !resolveQueue.empty()) {
        auto &entry = resolveQueue.front();
        ThreadID tid = entry.resolvedTid;
        unsigned int stream_id = entry.resolvedFTQId;
        dbpbtb->prepareResolveUpdateEntries(stream_id, tid);
        for (const auto resolvedInstPC : entry.resolvedInstPC) {
            dbpbtb->markCFIResolved(stream_id, resolvedInstPC, tid);
        }
        bool success = dbpbtb->resolveUpdate(stream_id, tid);
        if (success) {
            dbpbtb->notifyResolveSuccess(tid);
            resolveQueue.pop_front();
            fetchStats.resolveDequeueCount++;
        } else {
            dbpbtb->notifyResolveFailure(tid);
        }
    }
}

bool
Fetch::handleCommitSignals(ThreadID tid)
{
    // Check squash signals from commit.
    if (!fromCommit->commitInfo[tid].squash) {
        if (fromCommit->commitInfo[tid].doneFtqId) {
            DPRINTF(DecoupleBP, "Commit stream Id: %lu\n", fromCommit->commitInfo[tid].doneFtqId);
            assert(dbpbtb);
            dbpbtb->commit(fromCommit->commitInfo[tid].doneFtqId, tid);
        }
        return false;
    }

    // Check squash signals from commit.
    DPRINTF(Fetch,
            "[tid:%i] Squashing instructions due to squash "
            "from commit.\n",
            tid);

        InstSeqNum squash_seq = fromCommit->commitInfo[tid].doneSeqNum;
        DynInstPtr squash_inst = fromCommit->commitInfo[tid].squashInst;
        if (fromCommit->commitInfo[tid].isTrapSquash &&
            fromCommit->commitInfo[tid].traceTrapSkipInst) {
            squash_seq = fromCommit->commitInfo[tid].traceTrapSeqNum;
            squash_inst = nullptr;
            DPRINTF(Fetch,
                    "[tid:%i] Trap squash with trace ctrl-flow fault: rollback seq=%llu (skip head)\n",
                    tid, static_cast<unsigned long long>(squash_seq));
        }

    // In any case, squash.
    squash(*fromCommit->commitInfo[tid].pc, squash_seq,
           squash_inst, tid);

    localSquashVer[tid].update(
        fromCommit->commitInfo[tid].squashVersion.getVersion());
    DPRINTF(Fetch, "Updating squash version to %u\n",
            localSquashVer[tid].getVersion());

    auto mispred_inst = fromCommit->commitInfo[tid].mispredictInst;

    if (mispred_inst) {
        DPRINTF(Fetch, "Use mispred inst to redirect, treating as control squash\n");
        const auto corr_pc = fromCommit->commitInfo[tid].pc->as<RiscvISA::PCState>();
        assert(dbpbtb);
        dbpbtb->controlSquash(mispred_inst->getFtqId(), mispred_inst->pcState(),
                              corr_pc, mispred_inst->staticInst,
                              mispred_inst->getInstBytes(), fromCommit->commitInfo[tid].branchTaken,
                              mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(), true);
    } else if (fromCommit->commitInfo[tid].isTrapSquash) {
        DPRINTF(Fetch, "Treating as trap squash\n", tid);
        const auto trap_pc = fromCommit->commitInfo[tid].pc->as<RiscvISA::PCState>();
        assert(dbpbtb);
        dbpbtb->trapSquash(fromCommit->commitInfo[tid].squashedTargetId, fromCommit->commitInfo[tid].committedPC,
                           trap_pc, tid, fromCommit->commitInfo[tid].squashedLoopIter);
    } else {
        if (fromCommit->commitInfo[tid].pc && fromCommit->commitInfo[tid].squashedTargetId != 0) {
            DPRINTF(Fetch, "Squash with stream id and target id from IEW\n");
            const auto nc_pc = fromCommit->commitInfo[tid].pc->as<RiscvISA::PCState>();
            assert(dbpbtb);
            dbpbtb->nonControlSquash(fromCommit->commitInfo[tid].squashedTargetId, nc_pc,
                                     0, tid, fromCommit->commitInfo[tid].squashedLoopIter);
        } else {
            DPRINTF(Fetch, "Dont squash dbq because no meaningful stream\n");
        }
    }

    return true;
}

bool
Fetch::handleDecodeSquash(ThreadID tid)
{
    // Check squash signals from decode.
    if (fromDecode->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from decode.\n",tid);

        auto mispred_inst = fromDecode->decodeInfo[tid].mispredictInst;
        if (fromDecode->decodeInfo[tid].branchMispredict) {
            assert(dbpbtb);
            const auto next_pc =
                fromDecode->decodeInfo[tid].nextPC->as<RiscvISA::PCState>();
            dbpbtb->controlSquash(
                mispred_inst->getFtqId(),
                mispred_inst->pcState(),
                next_pc,
                mispred_inst->staticInst, mispred_inst->getInstBytes(),
                fromDecode->decodeInfo[tid].branchTaken,
                mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(),
                false);
        } else {
            warn("Unexpected non-control squash from decode.\n");
        }

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from decode with PC = %s\n",
                *fromDecode->decodeInfo[tid].nextPC);
            // Squash unless we're already squashing
            squashFromDecode(*fromDecode->decodeInfo[tid].nextPC,
                             fromDecode->decodeInfo[tid].squashInst,
                             fromDecode->decodeInfo[tid].doneSeqNum,
                             tid);

            return true;
        }
    }

    return false;
}

DynInstPtr
Fetch::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    DynInst::Arrays arrays;
    arrays.numSrcs = staticInst->numSrcRegs();
    arrays.numDests = staticInst->numDestRegs();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, staticInst, curMacroop, this_pc, next_pc, seq, cpu);

    cpu->perfCCT->createMeta(instruction);
    cpu->perfCCT->updateInstPos(instruction->seqNum, PerfRecord::AtFetch);

    instruction->setTid(tid);

    instruction->setThreadState(cpu->thread[tid]);

    DPRINTF(Fetch, "[tid:%i] Instruction PC %s created [sn:%lli].\n",
            tid, this_pc, seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->disassemble(this_pc.instAddr()));

    DPRINTF(Fetch, "Is nop: %i, is move: %i\n", instruction->isNop(),
            instruction->isMov());
    assert(dbpbtb);
    DPRINTF(DecoupleBP, "Set instruction %lu with fetch id %lu\n",
            instruction->seqNum, dbpbtb->ftqHeadId(tid));
    instruction->setFtqId(dbpbtb->ftqHeadId(tid));

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, this_pc, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Write the instruction to the first slot in the queue
    // that heads to decode.
    assert(numInst < fetchWidth);
    fetchQueue[tid].push_back(instruction);
    assert(fetchQueue[tid].size() <= fetchQueueSize);
    DPRINTF(Fetch, "[tid:%i] Fetch queue entry created (%i/%i).\n",
            tid, fetchQueue[tid].size(), fetchQueueSize);
    //toDecode->insts[toDecode->size++] = instruction;

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    instruction->fallThruPC = this_pc.getFallThruPC();

    return instruction;
}

bool
Fetch::checkDecoupledFrontend(ThreadID tid)
{
    assert(dbpbtb);
    if (!isTraceMode() && !dbpbtb->ftqHasFetching(tid)) {
        dbpbtb->addFtqNotValid();
        DPRINTF(Fetch, "Skip fetch when FSQ head is not available\n");
        setAllFetchStalls(StallReason::FTQBubble);
        return false;
    }
    return true;
}

bool
Fetch::prepareFetchAddress(ThreadID tid, bool &status_change)
{
    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC - directly use the actual instruction address
    PCStateBase &this_pc = *threads[tid].fetchpc;

    // Handle status transitions and cache access
    if (threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);
        setThreadStatus(tid, Running);
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
        return true;
    } else if (canFetchInstructions(tid)) {
        // If the decoder needs bytes, performInstructionFetch() will issue an
        // I-cache request via sendNextCacheRequest().
        if (!macroop[tid] && !threads[tid].valid) {
            return true;
        } else if (checkInterrupt(this_pc.instAddr()) && !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return false;
        }
        return true;
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            ++fetchStats.smtidleCycles[tid];
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }
        // Status is Idle, so fetch should do nothing.
        return false;
    }
}

void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    auto tid = dbpbtb->getTargetTid();

    if (tid == InvalidThreadID) {
        return;
    }

    if (!checkDecoupledFrontend(tid)) {
        return;
    }

    if (!prepareFetchAddress(tid, status_change)) {
        return;
    }

    ++fetchStats.cycles;

    performInstructionFetch(tid);
}

StallReason
Fetch::checkMemoryNeeds(ThreadID tid, const PCStateBase &this_pc,
                        const StaticInstPtr &curMacroop)
{
    // If we are in the middle of a macro-op, the decoder does not need
    // more memory bytes. It will continue processing the existing instruction.
    if (curMacroop) {
        return StallReason::NoStall;
    }

    // Trace 按需消费：在 decode 前逐条供给解码器。
    if (isTraceMode()) {
        assert(traceFetch);
        return traceFetch->checkMemoryNeeds(tid, this_pc);
    }

    Addr fetch_pc = this_pc.instAddr();

    // Check if fetch buffer is valid and contains this PC
    if (!threads[tid].valid) {
        DPRINTF(Fetch, "[tid:%i] Fetch buffer invalid, stalling on ICache\n", tid);
        return StallReason::IcacheStall;
    }

    // Check if the fetch buffer contains enough bytes for this instruction
    // We need at least 4 bytes to decode any RISC-V instruction (including compressed)
    if (fetch_pc < threads[tid].startPC ||
        fetch_pc + 4 > threads[tid].startPC + fetchBufferSize) {
        DPRINTF(Fetch, "[tid:%i] PC %#x outside fetch buffer range [%#x, %#x), stalling on ICache\n",
                tid, fetch_pc, threads[tid].startPC, threads[tid].startPC + fetchBufferSize);
        return StallReason::IcacheStall;
    }

    // Supply bytes to decoder - always provide 4 bytes for RISC-V
    auto *dec_ptr = decoder[tid];
    Addr offset_in_buffer = fetch_pc - threads[tid].startPC;
    memcpy(dec_ptr->moreBytesPtr(), threads[tid].data + offset_in_buffer, 4);

    DPRINTF(Fetch, "[tid:%i] Supplying 4 bytes from fetchBuffer at PC %#x (offset %d)\n",
            tid, fetch_pc, offset_in_buffer);

    // Call decoder with the actual instruction PC
    decoder[tid]->moreBytes(this_pc, fetch_pc);

    return StallReason::NoStall;
}

bool
Fetch::processSingleInstruction(ThreadID tid, PCStateBase &pc,
                               StaticInstPtr &curMacroop)
{
    auto *dec_ptr = decoder[tid];
    bool predictedBranch = false;
    bool newMacroop = false;

    // Create a copy of the current PC state to calculate the next PC.
    std::unique_ptr<PCStateBase> next_pc(pc.clone());

    // Decode the instruction, handling macro-op transitions.
    StaticInstPtr staticInst = nullptr;
    if (!curMacroop) {
        // Decode a new instruction if not currently in a macro-op.
        staticInst = dec_ptr->decode(pc);
        ++fetchStats.insts;

        if (staticInst->isMacroop()) {
            curMacroop = staticInst;
            DPRINTF(Fetch, "[tid:%i] Macroop instruction decoded\n", tid);
        }
    }
    if (curMacroop) {
        // Fetch the next micro-op from the current macro-op.
        staticInst = curMacroop->fetchMicroop(pc.microPC());
        DPRINTF(Fetch, "[tid:%i] Fetched macroop microop\n", tid);
        // Check if this is the last micro-op.
        newMacroop = staticInst->isLastMicroop();
    }

    // Build the dynamic instruction and add it to the fetch queue
    DynInstPtr instruction =
        buildInst(tid, staticInst, curMacroop, pc, *next_pc, true);

    o3::TraceInstruction traceForThisInst;
    if (isTraceMode()) {
        assert(traceFetch);
        traceFetch->bindPendingTraceMetadata(tid, instruction, pc, traceForThisInst);
    }

    // Special handling for RISC-V vector configuration instructions.
    if (staticInst->isVectorConfig()) {
        waitForVsetvl[tid] = dec_ptr->stall();
        DPRINTF(Fetch, "[tid:%i] Vector config instruction, waitForVsetvl[tid]=%d\n",
                tid, waitForVsetvl[tid]);
    }

    instruction->setVersion(localSquashVer[tid]);
    ppFetch->notify(instruction);
    numInst++;

#if TRACING_ON
    if (debug::O3PipeView) {
        instruction->fetchTick = curTick();
    }
#endif

    // Save current PC to next_pc first
    set(next_pc, pc);

    // Handle branch prediction and update next_pc for both modes
    predictedBranch = lookupAndUpdateNextPC(instruction, *next_pc);

    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Branch detected with PC = %s, target = %s\n",
                instruction->threadNumber, pc, *next_pc);
    }

    if (isTraceMode()) {
        assert(traceFetch);
        traceFetch->postBranchPredict(tid, instruction, traceForThisInst, pc, *next_pc, predictedBranch);
    }

    // A new macro-op also begins if the PC changes discontinuously.
    newMacroop |= pc.instAddr() != next_pc->instAddr();
    if (newMacroop) {
        curMacroop = NULL;
        DPRINTF(Fetch, "[tid:%i] New macroop transition, PC=%s\n",
                tid, pc);
    }

    // Update the main PC state for the next instruction.
    set(pc, *next_pc);

    // Do the value prediction
    if (valuePred && instruction->canLVP()) {
        valuepred::VPPredictRequest predictRequest;
        predictRequest.pc = instruction->getPC();
        predictRequest.seqNo = instruction->seqNum;
        predictRequest.tid = tid;
        // ExampleValuePredictor shows how a predictor can extend the public
        // request with extra fetch-time inputs without changing core fields.
        predictRequest.emplaceExt<valuepred::ExamplePredictRequestExt>(
                curTick(), instruction->opClass());
        instruction->vpResult =
            valuePred->valuePredict(predictRequest, instruction->vpRecord);
    }

    return predictedBranch;
}

void
Fetch::performInstructionFetch(ThreadID tid)
{
    if (isTraceMode()) {
        assert(traceFetch);
        if (traceFetch->maybeStallFetch(tid)) {
            return;
        }
    }

    // Initialize local variables
    PCStateBase &pc_state = *threads[tid].fetchpc;
    StaticInstPtr &curMacroop = macroop[tid];

    // Control flags for main fetch loop
    bool predictedBranch = false;

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to decode.\n", tid);

    // Main instruction fetch loop - process until fetch width or other limits
    // For decoupled frontend (including trace mode), check FTQ availability
    StallReason stall = StallReason::NoStall;
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize &&
           !predictedBranch && !ftqEmpty(tid) && !waitForVsetvl[tid]) {

        // Check memory needs and supply bytes to decoder if required
        stall = checkMemoryNeeds(tid, pc_state, curMacroop);
        if (stall != StallReason::NoStall) {
            break;
        }

        // Inner loop: extract as many instructions as possible from buffered
        // memory. This is primarily for macro-op instructions, which decode
        // into multiple micro-ops.
        do {
            // Process a single instruction, from decoding to PC update.
            predictedBranch = processSingleInstruction(tid, pc_state, curMacroop);

        } while (curMacroop &&
                 numInst < fetchWidth &&
                 fetchQueue[tid].size() < fetchQueueSize);
    }

    // Debug output for fetch queue contents
    DPRINTF(FetchVerbose, "FetchQue start dumping\n");
    for (auto it : fetchQueue[tid]) {
        DPRINTF(FetchVerbose, "inst: %s\n", it->staticInst->disassemble(it->pcState().instAddr()));
    }

    // Handle stall conditions and update statistics
    if (stall != StallReason::NoStall) {
        setAllFetchStalls(stall);
    }

    // Log why fetch stopped
    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, predicted branch instruction encountered.\n", tid);
    } else if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth for this cycle.\n", tid);
    } else if (stall != StallReason::NoStall) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, stalled due to %s.\n", tid,
                stall == StallReason::IcacheStall ? "ICache" : "other reasons");
    } else {
        DPRINTF(Fetch, "[tid:%i] Done fetching, no more instructions to fetch.\n", tid);
    }

    // Update persistent state
    macroop[tid] = curMacroop;

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    assert(fetchStatus[tid] == Running && "Fetch should be running");
    sendNextCacheRequest(tid, pc_state);
}

void
Fetch::sendNextCacheRequest(ThreadID tid, const PCStateBase &pc_state) {
    if (threads[tid].valid) {
        return;
    }

    if (ftqEmpty(tid)) {
        ++fetchStats.smtftqempty[tid];
        DPRINTF(Fetch, "[tid:%i] No FSQ entry available for next fetch\n", tid);
        return;
    }

    assert(dbpbtb);
    const auto &stream = dbpbtb->ftqFetchingTarget(tid);
    const Addr start_pc = stream.startPC;
    const Addr current_pc = pc_state.instAddr();
    threads[tid].startPC = start_pc;

    if (current_pc < stream.startPC ||
        current_pc >= stream.predEndPC) {
        auto &reset_pc = threads[tid].fetchpc->as<RiscvISA::PCState>();
        reset_pc.pc(stream.startPC);
        reset_pc.npc(stream.startPC + 4);
        reset_pc.uReset();
        DPRINTF(Fetch,
                "[tid:%i] Resetting fetch PC to new FTQ stream start %s "
                "(previous PC %#lx outside [%#lx, %#lx))\n",
                tid, *threads[tid].fetchpc, current_pc,
                stream.startPC, stream.predEndPC);
    }

    DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access for new FSQ entry, "
                  "starting at PC %#x (endPC %#x; original PC %s)\n",
            tid, start_pc, stream.predEndPC, pc_state);
    fetchCacheLine(start_pc, tid, pc_state.instAddr());
}

void
Fetch::recvReqRetry()
{
    if (retryPkt.empty()) {
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
        return;
    }
    assert(cacheBlocked);
    retryPendingIcacheRequests();
}

void
Fetch::retryPendingIcacheRequests()
{
    while (!retryPkt.empty()) {
        PacketPtr pkt = retryPkt.front();
        if (!icachePort.sendTimingReq(pkt)) {
            return;
        }

        const ThreadID tid = cpu->contextToThread(pkt->req->contextId());
        updateCacheRequestStatusByRequest(tid, pkt->req, CacheWaitResponse);
        ppFetchRequestSent->notify(pkt->req);
        retryPkt.erase(retryPkt.begin());
    }

    cacheBlocked = false;
}

void
Fetch::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        ++fetchStats.smtblockedCycles[tid];
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    } else if (waitingForFetchTranslation(tid)) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for queued translation!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == CacheWaitResponse) {
        ++fetchStats.icacheStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == TlbWait) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
                "finish!\n", tid);
    } else if (fetchStatus[tid] == TrapPending) {
        ++fetchStats.pendingTrapStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == CacheWaitRetry) {
        ++fetchStats.icacheWaitRetryStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == AccessFailed) {
            DPRINTF(Fetch, "[tid:%i] Fetch predicted non-executable address\n",
                    tid);
    } else {
        DPRINTF(Fetch, "[tid:%i] Unexpected fetch stall reason "
            "(Status: %i)\n",
            tid, fetchStatus[tid]);
    }
}

void
Fetch::setAllFetchStalls(StallReason stall)
{
    for (int i = 0; i < stallReason.size(); i++) {
        stallReason[i] = stall;
    }
}


bool
Fetch::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));

    DPRINTF(Fetch, "received pkt addr=%#lx, req addr=%#lx\n", pkt->getAddr(),
            pkt->req->getVaddr());

    fetch->processCacheCompletion(pkt);

    return true;
}

void
Fetch::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

bool
Fetch::canFetchInstructions(ThreadID tid) const
{
    // Thread must be in Running state
    if (fetchStatus[tid] != Running) {
        return false;  // Covers Idle, Squashing, Blocked, TrapPending
    }

    // Cache must be ready for new requests or have completed data
    CacheRequestStatus cacheStatus = threads[tid].cacheReq.getOverallStatus();
    return (cacheStatus == CacheIdle || cacheStatus == AccessComplete);
}

bool
Fetch::hasPendingCacheRequests(ThreadID tid) const
{
    // Check for any active cache operations (excluding terminal states)
    CacheRequestStatus overallStatus = threads[tid].cacheReq.getOverallStatus();
    return waitingForFetchTranslation(tid) ||
           (overallStatus == TlbWait ||
            overallStatus == CacheWaitResponse ||
            overallStatus == CacheWaitRetry);
}

void
Fetch::setThreadStatus(ThreadID tid, ThreadStatus status)
{
    assert(tid < MaxThreads);

    ThreadStatus oldStatus = fetchStatus[tid];
    fetchStatus[tid] = status;
    DPRINTF(Fetch, "[tid:%d] setThreadStatus: %s -> %s\n", tid, fetchStatusStr[oldStatus], fetchStatusStr[status]);
}

void
Fetch::updateCacheRequestStatus(ThreadID tid, size_t reqIndex,
                               CacheRequestStatus status)
{
    assert(tid < MaxThreads);
    assert(reqIndex < threads[tid].cacheReq.requestStatus.size());

    DPRINTF(Fetch, "[tid:%d] updateCacheRequestStatus[%d]: %d -> %d\n",
            tid, reqIndex, threads[tid].cacheReq.requestStatus[reqIndex], status);

    threads[tid].cacheReq.requestStatus[reqIndex] = status;
}

void
Fetch::updateCacheRequestStatusByRequest(ThreadID tid, const RequestPtr& req,
                                        CacheRequestStatus status)
{
    assert(tid < MaxThreads);

    size_t reqIndex = threads[tid].cacheReq.findRequestIndex(req);
    if (reqIndex != SIZE_MAX) {
        updateCacheRequestStatus(tid, reqIndex, status);
    } else {
        warn("Cannot find req %#x for status update to %d\n", req->getVaddr(), status);
    }
}

void
Fetch::cancelAllCacheRequests(ThreadID tid)
{
    assert(tid < MaxThreads);

    DPRINTF(Fetch, "[tid:%d] cancelAllCacheRequests: status before cancel: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Cancel all cache requests
    threads[tid].cacheReq.cancelAllRequests();

    DPRINTF(Fetch, "[tid:%d] cancelAllCacheRequests: status after cancel: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

}


const o3::TraceInstruction*
Fetch::getTraceInstMetadata(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->getTraceInstMetadata(seqNum) : nullptr;
}

bool
Fetch::isTraceInstruction(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->isTraceInstruction(seqNum) : false;
}

void
Fetch::cleanupTraceMetadataOnCommit(InstSeqNum seqNum)
{
    if (traceFetch) {
        traceFetch->cleanupTraceMetadataOnCommit(seqNum);
    }
}

uint64_t
Fetch::findTraceIndexForSeqNum(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->findTraceIndexForSeqNum(seqNum) : 0;
}

bool
Fetch::lookupTraceIndexForSeqNum(InstSeqNum seqNum, uint64_t &index) const
{
    if (!traceFetch) {
        index = 0;
        return false;
    }
    return traceFetch->lookupTraceIndexForSeqNum(seqNum, index);
}

Addr
Fetch::getTracePCByIndex(uint64_t index)
{
    return traceFetch ? traceFetch->getTracePCByIndex(index) : 0;
}

} // namespace o3
} // namespace gem5
