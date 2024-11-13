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
#include <cstdint>
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
#include "debug/Activity.hh"
#include "debug/Counters.hh"
#include "debug/DecoupleBPProbe.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/FetchFault.hh"
#include "debug/FetchVerbose.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
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

Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port", _cpu), fetch(_fetch)
{}


Fetch::Fetch(CPU *_cpu, const BaseO3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      cpu(_cpu),
      branchPred(nullptr),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      decodeWidth(params.decodeWidth),
      retryPkt(),
      retryTid(InvalidThreadID),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchBufferMask(fetchBufferSize - 1),
      fetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      icachePort(this, _cpu),
      finishTranslationEvent(this), fetchStats(_cpu, this)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));

    fetchBuffer.resize(MaxThreads);
    fetchBufferStartAddr.resize(MaxThreads);
    fetchBlockStartAddr.resize(MaxThreads);
    fetchBufferValid.resize(MaxThreads);
    fetchBlockSize.resize(MaxThreads);
    currentfetchBlockID.resize(MaxThreads);
    memReq.resize(MaxThreads);
    fetchStatus.resize(MaxThreads);
    currentPC.resize(MaxThreads);
    currentDecoderInputOffsetInFetchBuffer.resize(MaxThreads);
    fetchBlockEndWithTaken.resize(MaxThreads);
    lastFetchBlockFallThrough.resize(MaxThreads);
    lastSentReqAmount.resize(MaxThreads);

    for (int i = 0; i < MaxThreads; i++) {
        fetchBuffer[i].resize(MaxFetchBlockPerCycle);
        fetchBufferStartAddr[i].resize(MaxFetchBlockPerCycle);
        fetchBlockStartAddr[i].resize(MaxFetchBlockPerCycle);
        fetchBufferValid[i].resize(MaxFetchBlockPerCycle);
        fetchBlockSize[i].resize(MaxFetchBlockPerCycle);
        fetchBlockEndWithTaken[i].resize(MaxFetchBlockPerCycle);
        memReq[i].resize(MaxFetchBlockPerCycle * 2); // *2 for crossline fetch block
        fetchStatus[i].resize(MaxFetchBlockPerCycle * 2);

        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            fetchStatus[i][fetchBlockId * 2] = Idle;
            fetchStatus[i][fetchBlockId * 2 + 1] = Idle;
            fetchBuffer[i][fetchBlockId] = new uint8_t[fetchBufferSize];
        }
    }

    for (int i = 0; i < MaxThreads; i++) {
        decoder[i] = nullptr;
        pc[i].reset(params.isa[0]->newPCState());
        fetchOffset[i] = 0;
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        stalls[i] = {false, false};
        lastIcacheStall[i] = 0;
        issuePipelinedIfetch[i] = false;
        currentfetchBlockID[i] = 0;
        currentPC[i] = 0;
        currentDecoderInputOffsetInFetchBuffer[i] = 0;
        lastFetchBlockFallThrough[i] = false;
        lastSentReqAmount[i] = 0;
    }

    branchPred = params.branchPred;

    if (isStreamPred()) {
        dbsp = dynamic_cast<branch_prediction::stream_pred::DecoupledStreamBPU*>(branchPred);
        dbpftb = nullptr;
        assert(dbsp);
        usedUpFetchTargets = true;
    } else if (isFTBPred()) {
        dbsp = nullptr;
        dbpftb = dynamic_cast<branch_prediction::ftb_pred::DecoupledBPUWithFTB*>(branchPred);
        assert(dbpftb);
        usedUpFetchTargets = true;
        enableLoopBuffer = dbpftb->enableLoopBuffer;
        dbpftb->setCpu(_cpu);
        if (enableLoopBuffer) {
            loopBuffer = &dbpftb->lb;
        }
    }

    assert(params.decoder.size());
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
    }

    // Get the size of an instruction.
    instSize = decoder[0]->moreBytesSize();
    // stallReason size should be the same as decodeWidth,renameWidth,dispWidth
    stallReason.resize(decodeWidth, StallReason::NoStall);
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
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
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
             frontendBound - frontendLatencyBound)
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

        std::map<Fetch::ThreadStatus, const char*> fetchStatusStr = {
            {Running, "Running"},
            {Idle, "Idle"},
            {Squashing, "Squashing"},
            {Blocked, "Blocked"},
            {Fetching, "Fetching"},
            {TrapPending, "TrapPending"},
            {QuiescePending, "QuiescePending"},
            {ItlbWait, "ItlbWait"},
            {IcacheWaitResponse, "IcacheWaitResponse"},
            {IcacheWaitRetry, "IcacheWaitRetry"},
            {IcacheAccessComplete, "IcacheAccessComplete"},
            {NoGoodAddr, "NoGoodAddr"}
        };

        for (int i = 0; i < NumFetchStatus; i++) {
            fetchStatusDist.subname(i, fetchStatusStr[static_cast<Fetch::ThreadStatus>(i)]);
        }
        decodeStalls
            .prereq(decodeStalls);
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
}

void
Fetch::clearStates(ThreadID tid)
{
    fetchStatus[tid].assign(MaxFetchBlockPerCycle * 2, Running);
    set(pc[tid], cpu->pcState(tid));
    fetchOffset[tid] = 0;
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    memReq[tid].clear();
    stalls[tid].decode = false;
    stalls[tid].drain = false;
    fetchBufferStartAddr[tid].assign(MaxFetchBlockPerCycle, 0);
    fetchBlockStartAddr[tid].assign(MaxFetchBlockPerCycle, 0);
    fetchBlockEndWithTaken[tid].assign(MaxFetchBlockPerCycle, false);
    fetchBufferValid[tid].assign(MaxFetchBlockPerCycle, false);
    lastFetchBlockFallThrough[tid] = false;
    currentfetchBlockID[tid] = 0;
    fetchQueue[tid].clear();

    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);
}

void
Fetch::resetStage()
{
    numInst = 0;
    interruptPending = false;
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            fetchStatus[tid][fetchBlockId * 2] = Running;
            fetchStatus[tid][fetchBlockId * 2 + 1] = Running;
        }
        set(pc[tid], cpu->pcState(tid));
        fetchOffset[tid] = 0;
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        memReq[tid].clear();

        stalls[tid].decode = false;
        stalls[tid].drain = false;

        fetchBufferStartAddr[tid].assign(MaxFetchBlockPerCycle, 0);
        fetchBlockStartAddr[tid].assign(MaxFetchBlockPerCycle, 0);
        fetchBlockEndWithTaken[tid].assign(MaxFetchBlockPerCycle, false);
        fetchBufferValid[tid].assign(MaxFetchBlockPerCycle, false);
        currentfetchBlockID[tid] = 0;
        lastFetchBlockFallThrough[tid] = false;

        fetchQueue[tid].clear();

        priorityList.push_back(tid);
    }

    wroteToTimeBuffer = false;
    _status = Inactive;
    if (enableLoopBuffer) {
        loopBuffer->deactivate(true);
        currentLoopIter = 0;
        loopBuffer->clearState();
    }

    if (isStreamPred()) {
        dbsp->resetPC(pc[0]->instAddr());
    } else if (isFTBPred()) {
        dbpftb->resetPC(pc[0]->instAddr());
    }
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());
    uint64_t fetchReqNum = pkt->req->getReqNum();
    uint64_t fetchBlockId = fetchReqNum / 2;

    DPRINTF(Fetch, "[tid:%i] Waking up from cache miss.\n", tid);
    assert(!cpu->switchedOut());


    // Only change the status if it's still waiting on the icache access
    // to return.
    if (fetchStatus[tid][fetchReqNum] != IcacheWaitResponse || pkt->req != memReq[tid][fetchReqNum]) {
        DPRINTF(Fetch, "delete pkt %#lx\n", pkt->getAddr());
        ++fetchStats.icacheSquashes;
        delete pkt;
        return;
    }

    if (pkt->isRetriedPkt()) {
        DPRINTF(Fetch, "[tid:%i] Retried pkt.\n", tid);
        DPRINTF(Fetch, "[tid:%i] send next pkt, addr: %#x, size: %d\n", tid,
                pkt->req->getVaddr() + 64 - pkt->req->getVaddr() % 64, fetchBufferSize - pkt->getSize());
        RequestPtr mem_req =
            std::make_shared<Request>(pkt->req->getVaddr(), pkt->req->getSize(), Request::INST_FETCH,
                                      cpu->instRequestorId(), pkt->req->getPC(), cpu->thread[tid]->contextId());

        mem_req->taskId(cpu->taskId());
        mem_req->setReqNum(fetchReqNum);
        memReq[tid][fetchReqNum] = mem_req;

        fetchStatus[tid][fetchReqNum] = ItlbWait;
        FetchTranslation *trans = new FetchTranslation(this);
        cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(), trans, BaseMMU::Execute);
        delete pkt;
        return;
    }

    DPRINTF(FetchVerbose, "Recv ICache resp for req #%d, blockStartAddr %#x, startAddr %#x, crossline req: %s.\n",
            pkt->req->getReqNum(), pkt->req->getVaddr(), pkt->req->getPC(),
            pkt->req->isSecondLineFetch() ? "true" : "false");

    Addr blockStartAddr = fetchBlockStartAddr[tid][fetchBlockId];
    Addr blockStartOffsetInCacheline = blockStartAddr & (cacheBlkSize - 1);
    Addr bufferStartAddr = fetchBufferStartAddr[tid][fetchBlockId];
    Addr bufferStartOffsetInCacheline = bufferStartAddr & (cacheBlkSize - 1);
    bool isSecondLineReq = pkt->req->isSecondLineFetch();

    // Copy to fetch buffer
    uint64_t thisFetchBlockSize = fetchBlockSize[tid][fetchBlockId];
    uint64_t destOffset = isSecondLineReq ? cacheBlkSize - bufferStartOffsetInCacheline : 0;
    uint8_t *destPtr = fetchBuffer[tid][fetchBlockId] + destOffset;
    const uint8_t *srcPtr = pkt->getConstPtr<uint8_t>() + (isSecondLineReq ? 0 : bufferStartOffsetInCacheline);
    auto copySize = isSecondLineReq ? thisFetchBlockSize + 4 - (cacheBlkSize - bufferStartOffsetInCacheline)
                                    : std::min(cacheBlkSize - bufferStartOffsetInCacheline, thisFetchBlockSize + 4);


    // Sanity check
    DPRINTF(FetchVerbose, "Processing cache resp, copying %d bytes\n", copySize);
    DPRINTF(FetchVerbose, "Processing cache resp, dest offset %d\n", destOffset);
    assert(blockStartAddr == pkt->req->getPC());
    assert(thisFetchBlockSize <= fetchBufferSize - 4);
    assert(copySize <= fetchBufferSize);

    memcpy(destPtr, srcPtr, copySize);

    // Check the other fetch req status
    if (fetchStatus[tid][fetchReqNum ^ 0b1] == IcacheAccessComplete ||
        fetchStatus[tid][fetchReqNum ^ 0b1] == Running) {
        fetchBufferValid[tid][fetchBlockId] = true;
    }

    // Dump fetch buffer
    DPRINTF(FetchVerbose, "Fetch buffer after cache completion (reqNum %d):\n", fetchReqNum);
    for (int i = 0; i < fetchBufferSize; i++) {
        DPRINTF(FetchVerbose, "%d: %02x\n", i, fetchBuffer[tid][fetchBlockId][i]);
    }

    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    cpu->wakeCPU();

    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n", tid);

    switchToActive();


    // Only switch to IcacheAccessComplete if we're not stalled as well.
    if (checkStall(tid)) {
        fetchStatus[tid][fetchReqNum] = Blocked;
    } else {
        fetchStatus[tid][fetchReqNum] = IcacheAccessComplete;
    }

    pkt->req->setAccessLatency();
    cpu->ppInstAccessComplete->notify(pkt);
    // Reset the mem req to NULL.
    delete pkt;
    memReq[tid][fetchReqNum] = nullptr;
}

void
Fetch::drainResume()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        stalls[i].decode = false;
        stalls[i].drain = false;
    }
}

void
Fetch::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPkt.size() == 0);
    assert(retryTid == InvalidThreadID);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(!memReq[i][0]);
        assert(fetchStatus[i][0] == Idle || stalls[i].drain);
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
        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            if (fetchStatus[i][fetchBlockId] != Idle) {
                if (fetchStatus[i][fetchBlockId] == Blocked && stalls[i].drain)
                    continue;
                else
                    return false;
            }
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
    assert(cpu->isDraining());
    assert(!stalls[tid].drain);
    DPRINTF(Drain, "%i: Thread drained.\n", tid);
    stalls[tid].drain = true;
}

void
Fetch::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
        fetchStatus[0][fetchBlockId] = Running;
    }
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

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());
    uint64_t fetchReqNum = mem_req->getReqNum();

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    if (memReq[tid][fetchReqNum] != NULL) {
        DPRINTF(Fetch, "memReq.addr=%#lx\n", memReq[tid][fetchReqNum]->getVaddr());
    }

    if (fetchStatus[tid][fetchReqNum] != ItlbWait || (mem_req != memReq[tid][fetchReqNum])) {
            DPRINTF(Fetch, "[tid:%i] Ignoring itlb completed after squash\n",
                    tid);
            DPRINTF(Fetch, "[tid:%i] Ignoring req addr=%#lx\n",
                    tid, mem_req->getVaddr());
            ++fetchStats.tlbSquashes;
            return;
    }


    // If translation was successful, attempt to read the icache block.
    if (fault == NoFault) {
        // Check that we're not going off into random memory
        // If we have, just wait around for commit to squash something and put
        // us on the right track
        if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
            DPRINTF(Fetch, "Address %#x is outside of physical memory, stopping fetch, %lu\n",
                    mem_req->getPaddr(), curTick());
            fetchStatus[tid][fetchReqNum] = NoGoodAddr;
            setAllFetchStalls(StallReason::OtherFetchStall);
            memReq[tid][fetchReqNum] = nullptr;
            return;
        }

        // Build packet here.
        PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
        data_pkt->dataDynamic(new uint8_t[cacheBlkSize]);

        DPRINTF(Fetch, "[tid:%i] Fetching data for addr %#x, pc=%#lx\n",
                    tid, mem_req->getVaddr(), mem_req->getPC());

        DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

        fetchStats.cacheLines++;

        // Access the cache.
        if (!icachePort.sendTimingReq(data_pkt)) {
            //assert(retryPkt == NULL);
            // assert(retryTid == InvalidThreadID);
            DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

            fetchStatus[tid][fetchReqNum] = IcacheWaitRetry;
            data_pkt->setRetriedPkt();
            DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                    mem_req->getVaddr());
            setAllFetchStalls(StallReason::IcacheStall);
            retryPkt.push_back(data_pkt);
            retryTid = tid;
            cacheBlocked = true;

        } else {
            DPRINTF(Fetch, "[tid:%i] Doing ICache access for fetch req #%d.\n", tid, fetchReqNum);
            DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache "
                    "response.\n", tid);
            lastIcacheStall[tid] = curTick();
            fetchStatus[tid][fetchReqNum] = IcacheWaitResponse;
            setAllFetchStalls(StallReason::IcacheStall);
            // Notify Fetch Request probe when a packet containing a fetch
            // request is successfully sent
            ppFetchRequestSent->notify(mem_req);
        }
    } else {
        DPRINTF(FetchFault, "fault, mem_req.addr=%#lx\n", mem_req->getVaddr());
        // Don't send an instruction to decode if we can't handle it.
        if (!(numInst < fetchWidth) ||
                !(fetchQueue[tid].size() < fetchQueueSize)) {
            if (finishTranslationEvent.scheduled() && finishTranslationEvent.getReq() != mem_req) {
                DPRINTF(FetchFault, "fault, mem_req.addr=%#lx, finishTranslationEvent.getReq().addr=%#lx, mem_req.addr=%#lx\n",
                        mem_req->getVaddr(),
                        finishTranslationEvent.getReq()->getVaddr(), mem_req->getVaddr());
                return;
            }
            assert(!finishTranslationEvent.scheduled());
            finishTranslationEvent.setFault(fault);
            finishTranslationEvent.setReq(mem_req);
            cpu->schedule(finishTranslationEvent,
                          cpu->clockEdge(Cycles(1)));
            return;
        }
        DPRINTF(Fetch,
                "[tid:%i] Got back req with addr %#x but expected %#x\n",
                tid, mem_req->getVaddr(), memReq[tid][fetchReqNum]->getVaddr());
        // Translation faulted, icache request won't be sent.
        memReq[tid][fetchReqNum] = nullptr;

        // Send the fault to commit.  This thread will not do anything
        // until commit handles the fault.  The only other way it can
        // wake up is if a squash comes along and changes the PC.
        const PCStateBase &fetch_pc = *pc[tid];

        DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
        // We will use a nop in ordier to carry the fault.
        // DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
        //         fetch_pc, fetch_pc, false);
        // instruction->setVersion(localSquashVer);
        // instruction->setNotAnInst();

        // instruction->setPredTarg(fetch_pc);
        // instruction->fault = fault;
        // std::unique_ptr<PCStateBase> next_pc(fetch_pc.clone());
        // instruction->staticInst->advancePC(*next_pc);
        // set(instruction->predPC, next_pc);

        // wroteToTimeBuffer = true;

        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();

        fetchStatus[tid][fetchReqNum] = TrapPending;
        setAllFetchStalls(StallReason::TrapStall);

        DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
        DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
                tid, fault->name(), *pc[tid]);
    }
    _status = updateFetchStatus();
}

void
Fetch::doSquash(const PCStateBase &new_pc, const DynInstPtr squashInst, const InstSeqNum seqNum,
        ThreadID tid)
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

    set(pc[tid], new_pc);
    fetchOffset[tid] = 0;
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle * 2; fetchBlockId++) {
        if (fetchStatus[tid][fetchBlockId] == IcacheWaitResponse) {
            DPRINTF(Fetch, "[tid:%i] fetchBlockId #%ld Squashing outstanding ICache miss.\n", tid, fetchBlockId);
            memReq[tid][fetchBlockId] = nullptr;
        } else if (fetchStatus[tid][fetchBlockId] == ItlbWait) {
            DPRINTF(Fetch, "[tid:%i] fetchBlockId #%ld Squashing outstanding iTLB miss.\n", tid, fetchBlockId);
            memReq[tid][fetchBlockId] = nullptr;
        }
    }

    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == tid) {
        assert(cacheBlocked);
        for (auto it : retryPkt) {
            delete it;
        }
        retryPkt.clear();
        retryTid = InvalidThreadID;
    }

    for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle * 2; fetchBlockId++) {
        fetchStatus[tid][fetchBlockId] = Squashing;
    }
    setAllFetchStalls(StallReason::BpStall);  // may caused by other stages like load and store

    // Clear fetchBuffer
    fetchBufferValid[tid].assign(MaxFetchBlockPerCycle, false);

    // Clear states
    lastFetchBlockFallThrough[tid] = false;
    currentfetchBlockID[tid] = 0;
    currentDecoderInputOffsetInFetchBuffer[tid] = 0;
    lastSentReqAmount[tid] = 0;

    // Empty fetch queue
    fetchQueue[tid].clear();

    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    usedUpFetchTargets = true;

    ++fetchStats.squashCycles;

    if (enableLoopBuffer) {
        loopBuffer->deactivate(true);
        currentLoopIter = 0;
        loopBuffer->clearState();

        currentFtqEntryInsts.first = new_pc.instAddr();
        currentFtqEntryInsts.second.clear();
    }
}

void
Fetch::flushFetchBuffer()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            fetchBufferValid[i][fetchBlockId] = false;
        }
    }
}

Addr
Fetch::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    if (isFTBPred()) {
        return dbpftb->getPreservedReturnAddr(dynInst);
    } else {
        panic("getPreservedReturnAddr not implemented for this bpu");
        return 0;
    }
}

void
Fetch::squashFromDecode(const PCStateBase &new_pc, const DynInstPtr squashInst,
        const InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}

bool
Fetch::checkStall(ThreadID tid) const
{
    bool ret_val = false;

    if (stalls[tid].drain) {
        assert(cpu->isDraining());
        DPRINTF(Fetch,"[tid:%i] Drain stall detected.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    // Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            if (fetchStatus[tid][fetchBlockId] == Running || fetchStatus[tid][fetchBlockId] == Squashing ||
                fetchStatus[tid][fetchBlockId] == IcacheAccessComplete) {

                if (_status == Inactive) {
                    DPRINTF(Activity, "[tid:%i] Activating stage.\n", tid);

                    if (fetchStatus[tid][fetchBlockId] == IcacheAccessComplete) {
                        DPRINTF(Activity,
                                "[tid:%i] Activating fetch due to cache"
                                "completion\n",
                                tid);
                    }

                    cpu->activateStage(CPU::FetchIdx);
                }

                return Active;
            }
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
Fetch::squash(const PCStateBase &new_pc, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}

void
Fetch::tick()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;

    // get the distribution of fetch status
    fetchStats.fetchStatusDist[fetchStatus[0]]++;

    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change =  status_change || updated_status;
    }

    DPRINTF(Fetch, "Running stage.\n");

    if (fromCommit->commitInfo[0].emptyROB) {
        waitForVsetvl = false;
    }

    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        fetch(status_change);
    }

    toDecode->fetchStallReason = stallReason;

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }


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

    issuePipelinedIfetch[0] = issuePipelinedIfetch[0] && !interruptPending;


    // Send instructions enqueued into the fetch queue to decode.
    // Limit rate by fetchWidth.  Stall if decode is stalled.
    unsigned insts_to_decode = 0;
    unsigned available_insts = 0;

    for (auto tid : *activeThreads) {
        if (!stalls[tid].decode) {
            available_insts += fetchQueue[tid].size();
        }
    }

    // Pick a random thread to start trying to grab instructions from
    auto tid_itr = activeThreads->begin();
    std::advance(tid_itr,
            random_mt.random<uint8_t>(0, activeThreads->size() - 1));

    int decode_width = decodeWidth;
    int count_ = 0;
    for (auto &it0 : fetchQueue){
        for (auto &it1 : it0) {
            count_++;
            if (it1->opClass() == FMAAccOp) {
                    decode_width++;
            }
            if (count_ >= decodeWidth ||
                decode_width >= decodeWidth * 2) {
                break;
            }
        }
    }

    while (available_insts != 0 && insts_to_decode < decode_width) {
        ThreadID tid = *tid_itr;
        if (!stalls[tid].decode && !fetchQueue[tid].empty()) {
            const auto& inst = fetchQueue[tid].front();
            toDecode->insts[toDecode->size++] = inst;
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                    "from fetch queue. Fetch queue size: %i.\n",
                    tid, inst->seqNum, fetchQueue[tid].size());

            wroteToTimeBuffer = true;
            fetchQueue[tid].pop_front();
            insts_to_decode++;
            available_insts--;
        }

        tid_itr++;
        // Wrap around if at end of active threads list
        if (tid_itr == activeThreads->end())
            tid_itr = activeThreads->begin();
    }

    // fetch totally stalled
    if (stalls[*tid_itr].decode) { // If decode stalled, use decode's stall reason
        setAllFetchStalls(fromDecode->decodeInfo[*tid_itr].blockReason);
    } else if (insts_to_decode == 0) {  // fetch stalled
        if (stallReason[0] != StallReason::NoStall) {   //  previously set stall reason
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

    // Intel TopDown method for measuring frontend bubbles
    // Count unutilized issue slots when backend is not stalled (decode not stalled)
    // For N-wide machine, if frontend supplies 0 instructions:
    // - fetchBubbles += N (count total empty slots)
    // - fetchBubbles_max += 1 (count occurrence of all slots being empty)
    if (!stalls[*tid_itr].decode && !fromCommit->commitInfo[*tid_itr].robSquashing) { // backend not stalled
        int unused_slots = decode_width - insts_to_decode;
        if (unused_slots > 0) { // has empty slots
            fetchStats.fetchBubbles += unused_slots; // add number of empty slots
            if (unused_slots == decode_width) { // all slots empty, insts_to_decode == 0
                fetchStats.fetchBubbles_max++; // count max bubble occurrence
            }
        }
    }

    if (stalls[*tid_itr].decode) {
        fetchStats.decodeStalls++;
    }

    // If there was activity this cycle, inform the CPU of it.
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    // Reset the number of the instruction we've fetched.
    numInst = 0;

    if (isStreamPred()) {
        assert(dbsp);
        dbsp->tick();
        usedUpFetchTargets = !dbsp->trySupplyFetchWithTarget(pc[0]->instAddr());
    } else if (isFTBPred()) {
        assert(dbpftb);
        // TODO: remove ideal_tick()
        if (dbpftb->enableTwoTaken){
            dbpftb->ideal_tick();
        } else {
            dbpftb->tick();
        }
        usedUpFetchTargets = !dbpftb->trySupplyFetchWithTarget(pc[0]->instAddr(), currentFetchTargetInLoop);
    }
}

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
    if (fromCommit->commitInfo[tid].squash) {

        DPRINTF(Fetch,
                "[tid:%i] Squashing instructions due to squash "
                "from commit.\n",
                tid);
        DPRINTF(Fetch, "[tid:%i] Squashing seqNum: %llu\n",
                tid, fromCommit->commitInfo[tid].doneSeqNum);
        // In any case, squash.
        squash(*fromCommit->commitInfo[tid].pc, fromCommit->commitInfo[tid].doneSeqNum,
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
            }
        }
    }

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
                }
            } else {
                warn("Unexpected non-control squash from decode.\n");
            }
        }

        // Assume both status enters and exits squashing
        if (fetchStatus[tid][0] != Squashing) {

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

    bool status_change = false;

    for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle * 2; fetchBlockId++) {

        if (checkStall(tid) && fetchStatus[tid][fetchBlockId] != IcacheWaitResponse &&
            fetchStatus[tid][fetchBlockId] != IcacheWaitRetry && fetchStatus[tid][fetchBlockId] != ItlbWait &&
            fetchStatus[tid][fetchBlockId] != QuiescePending) {
            DPRINTF(Fetch, "[tid:%i] Setting to blocked\n", tid);

            fetchStatus[tid][fetchBlockId] = Blocked;

            status_change = true;
            continue;
        }

        if (fetchStatus[tid][fetchBlockId] == Blocked || fetchStatus[tid][fetchBlockId] == Squashing) {
            // Switch status to running if fetch isn't being told to block or
            // squash this cycle.
            DPRINTF(Fetch, "[tid:%i] Fetch block %d done squashing, switching to running.\n", tid, fetchBlockId);

            fetchStatus[tid][fetchBlockId] = Running;

            status_change = true;
            continue;
        }
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return status_change;
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
            instruction->staticInst->isMov());
    if (isDecoupledFrontend()) {
        if (isStreamPred()) {
            DPRINTF(DecoupleBP, "Set instruction %lu with stream id %lu, fetch id %lu\n",
                    instruction->seqNum, dbsp->getSupplyingStreamId(), dbsp->getSupplyingTargetId());
            instruction->setFsqId(dbsp->getSupplyingStreamId());
            instruction->setFtqId(dbsp->getSupplyingTargetId());
        } else if (isFTBPred()) {
            DPRINTF(DecoupleBP, "Set instruction %lu with stream id %lu, fetch id %lu\n",
                    instruction->seqNum, dbpftb->getSupplyingStreamId(), dbpftb->getSupplyingTargetId());
            instruction->setFsqId(dbpftb->getSupplyingStreamId());
            instruction->setFtqId(dbpftb->getSupplyingTargetId());
        }
    }

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

void
Fetch::sendFetchRequest()
{
    ThreadID tid = getFetchingThread();

    assert(!cpu->switchedOut());

    if (tid == InvalidThreadID) {
        // Breaks looping condition in tick()
        threadFetched = numFetchingThreads;

        if (numThreads == 1) {  // @todo Per-thread stats
            profileStall(0);
        }

        return;
    }

    if (isDecoupledFrontend()) {
        if (isStreamPred()) {
            if (!dbsp->fetchTargetAvailable()) {
                DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
                setAllFetchStalls(StallReason::FTQBubble);
                return;
            }
        } else if (isFTBPred()) {
            if (!dbpftb->fetchTargetAvailable()) {
                dbpftb->addFtqNotValid();
                DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
                setAllFetchStalls(StallReason::FTQBubble);
                return;
            }
        }
    }

    if (!isFTBPred())
        fatal("Only Decoupled FTBPred is supported");

    for (int fetchBlockIdx = 0; fetchBlockIdx < MaxFetchBlockPerCycle; fetchBlockIdx++) {
        // Generate fetch request for each fetch block and send them to iTLB & ICache

        // Get FTQ entry from FTQ
        if (!dbpftb->fetchTargetAvailable(fetchBlockIdx)) {
            DPRINTF(FetchVerbose, "fetch block %d skipped because ftq not available.\n", fetchBlockIdx);
            break;
        }

        auto entry = dbpftb->getSupplyingFetchTarget(fetchBlockIdx);
        lastSentReqAmount[tid] = fetchBlockIdx + 1;

        // Check if ICache bank conflict

        // Send memory request
        Addr decoderPCMask = decoder[tid]->pcMask();

        Addr blockStartAddr = entry.startPC;
        Addr bufferStartAddr = blockStartAddr & decoderPCMask;
        Addr reqAddr = bufferStartAddr & ~(cacheBlkSize - 1); // Cacheline Aligned
        Addr blockStartOffsetInCacheline = blockStartAddr & (cacheBlkSize - 1);
        Addr bufferStartOffsetInCacheline = bufferStartAddr& (cacheBlkSize - 1);
        uint64_t length = entry.endPC - entry.startPC + 2 + 2;  // +2 for false hit on half RVI
        bool crossline = bufferStartOffsetInCacheline + length > cacheBlkSize;

        uint64_t memReqId = fetchBlockIdx * 2; // Space for crossline requests

        // Predicted fetch block size should be smaller than fetchBufferSize
        assert(length <= fetchBufferSize);

        RequestPtr mem_req =
            std::make_shared<Request>(reqAddr, cacheBlkSize, Request::INST_FETCH, cpu->instRequestorId(),
                                      blockStartAddr, cpu->thread[tid]->contextId());

        mem_req->taskId(cpu->taskId());
        mem_req->setReqNum(memReqId);
        if (crossline) {
            mem_req->setFirstLineFetch();
        }

        DPRINTF(Fetch,
                "[tid:%i] Fetching fetch block #%d, blockStartAddr %#x, length %d, taken %s, bufferStartAddr %#x, "
                "cacheReqAddr %#x\n",
                tid, fetchBlockIdx, blockStartAddr, length, entry.taken ? "true" : "false", bufferStartAddr, reqAddr);

        memReq[tid][memReqId] = mem_req;
        fetchStatus[tid][memReqId] = ItlbWait;

        // Send out to iTLB for translation
        FetchTranslation *trans = new FetchTranslation(this);
        cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(), trans, BaseMMU::Execute);

        if (crossline) {
            memReqId++;
            RequestPtr mem_req =
                std::make_shared<Request>(reqAddr + cacheBlkSize, cacheBlkSize, Request::INST_FETCH,
                                          cpu->instRequestorId(), blockStartAddr, cpu->thread[tid]->contextId());

            mem_req->taskId(cpu->taskId());
            mem_req->setReqNum(memReqId);
            mem_req->setSecondLineFetch();

            DPRINTF(FetchVerbose, "Sending crossline ICache req for fetch block %d, num %d\n", fetchBlockIdx,
                    mem_req->getReqNum());

            memReq[tid][memReqId] = mem_req;
            fetchStatus[tid][memReqId] = ItlbWait;

            // Send out to iTLB for translation
            FetchTranslation *trans = new FetchTranslation(this);
            cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(), trans, BaseMMU::Execute);
        }


        fetchBlockStartAddr[tid][fetchBlockIdx] = blockStartAddr;
        fetchBufferStartAddr[tid][fetchBlockIdx] = bufferStartAddr;
        fetchBlockEndWithTaken[tid][fetchBlockIdx] = entry.taken;
        fetchBufferValid[tid][fetchBlockIdx] = false;
        fetchBlockSize[tid][fetchBlockIdx] = entry.endPC - entry.startPC;


        if (entry.taken) {
            assert(entry.endPC == entry.takenPC);
        }
    }

    if (lastSentReqAmount[tid] > 0) {
        // Set to first fetch block start PC if sent request successfully
        if (!lastFetchBlockFallThrough[tid]) {
            currentPC[tid] = fetchBlockStartAddr[tid][0];
        } else if (currentPC[tid] - fetchBufferStartAddr[tid][0] == 4) {
            // Skip first 4B if fall through PC is +4B then the bufferStartAddr
            currentDecoderInputOffsetInFetchBuffer[tid] = 4;
        }
        dbpftb->finishCurrentFetchTarget(lastSentReqAmount[tid]);
    }
}

void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    ThreadID tid = getFetchingThread();
    auto *dec_ptr = decoder[tid];
    const Addr pc_mask = dec_ptr->pcMask();

    assert(!cpu->switchedOut());

    if (tid == InvalidThreadID) {
        // Breaks looping condition in tick()
        threadFetched = numFetchingThreads;

        if (numThreads == 1) {  // @todo Per-thread stats
            profileStall(0);
        }

        return;
    }

    if (isDecoupledFrontend()) {
        if (isStreamPred()) {
            if (!dbsp->fetchTargetAvailable()) {
                DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
                return;
            }
        } else if (isFTBPred()) {
            if (!dbpftb->fetchTargetAvailable()) {
                dbpftb->addFtqNotValid();
                DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
                return;
            }
        }
    }

    // Check if last cycle ICache access is completed
    for (auto fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle * 2; fetchBlockId++) {
        if (fetchStatus[tid][fetchBlockId] == IcacheAccessComplete) {
            DPRINTF(Fetch, "[tid:%i] Icache access for fetch req #%d is complete.\n", tid, fetchBlockId);
            fetchStatus[tid][fetchBlockId] = Running;
        }
    }
    DPRINTF(FetchVerbose, "dumping all fetch block status.\n");
    for (auto fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle * 2; fetchBlockId++) {
        DPRINTF(FetchVerbose, "fetch block#%d status: %s\n", fetchBlockId, fetchStatus[tid][fetchBlockId]);
    }
    DPRINTF(FetchVerbose, "current fetch block ID: %d\n", currentfetchBlockID[tid]);
    if (std::all_of(fetchStatus[tid].begin(), fetchStatus[tid].end(), [](ThreadStatus s){ return s == Running; })) {
        // If all req satisfied, go on fetching
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
    } else {
        DPRINTF(Fetch, "Still waiting for something, cannot fetch.\n");
        return;
    }

    DPRINTF(Fetch, "Attempting to fetch for [tid:%i]\n", tid);

    bool ibufferNearlyFull = fetchQueue[tid].size() > fetchQueueSize - fetchWidth;
    if (ibufferNearlyFull) {
        DPRINTF(Fetch, "Fetch queue is full, skipping fetch.\n");
    }

    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize && !ibufferNearlyFull) {
        // We need to process more memory if we aren't going to get a
        // StaticInst from the rom, the current macroop, or what's already
        // in the decoder.
        // insts from loop buffer is decoded, we do not need instruction bytes
        bool need_mem = !dec_ptr->instReady();
        Addr this_pc_addr = currentPC[tid];
        auto this_pc = RiscvISA::PCState(this_pc_addr);
        Addr decoder_supply_addr =
            (fetchBufferStartAddr[tid][currentfetchBlockID[tid]] + currentDecoderInputOffsetInFetchBuffer[tid]);
        auto decoder_supply = RiscvISA::PCState(decoder_supply_addr);
        StaticInstPtr staticInst = NULL;

        DPRINTF(FetchVerbose, "Current Fetch Block: #%d, blockStartAddr %#x, bufferStartAddr %#x, blockSize: %d\n",
                currentfetchBlockID[tid], fetchBlockStartAddr[tid][currentfetchBlockID[tid]],
                fetchBufferStartAddr[tid][currentfetchBlockID[tid]], fetchBlockSize[tid][currentfetchBlockID[tid]]);

        if (need_mem) {
            if (!fetchBufferValid[tid][currentfetchBlockID[tid]]) {
                // Nothing to do, fetch buffer not yet valid
                DPRINTF(Fetch, "Fetch buffer not valid yet, abort fetching.\n");
                break;
            } else if (currentPC[tid] < fetchBlockStartAddr[tid][currentfetchBlockID[tid]]) {
                fatal("current PC (%#x) is smaller than fetch block start addr (%#x), something wrong.\n",
                      currentPC[tid], fetchBlockStartAddr[tid][currentfetchBlockID[tid]]);
            } else if (currentPC[tid] - fetchBlockStartAddr[tid][currentfetchBlockID[tid]] >= fetchBufferSize) {
                DPRINTF(FetchVerbose, "current PC %#x \n", currentPC[tid]);
                fatal("All instr code from fetch buffer %d has all been fed, somthing wrong.\n",
                      currentfetchBlockID[tid]);
            } else {
                DPRINTF(Fetch, "Supplying fetch from fetchBuffer #%d, offset in buffer: %d\n",
                        currentfetchBlockID[tid], currentDecoderInputOffsetInFetchBuffer[tid]);
                DPRINTF(FetchVerbose, "Feeding decoder with addr %#x\n", decoder_supply_addr);
                if ((currentPC[tid] > decoder_supply_addr && currentPC[tid] - decoder_supply_addr > 4) ||
                    (currentPC[tid] < decoder_supply_addr && decoder_supply_addr - currentPC[tid] > 2)) {
                    fatal("Supplying currect PC (%#x) from unexpected address (%#x).\n", currentPC[tid],
                          decoder_supply_addr);
                }
                memcpy(dec_ptr->moreBytesPtr(),
                       fetchBuffer[tid][currentfetchBlockID[tid]] + currentDecoderInputOffsetInFetchBuffer[tid],
                       instSize);

                dec_ptr->moreBytes(this_pc, decoder_supply_addr);
                if (dec_ptr->needMoreBytes()) {
                    currentDecoderInputOffsetInFetchBuffer[tid] += instSize;
                }
            }
        }

        // Extract as many instructions and/or microops as we can from
        // the memory we've processed so far.
        do {
            if (!dec_ptr->instReady()) {
                // Need more bytes
                break;
            }

            staticInst = dec_ptr->decode(this_pc);

            auto next_pc = this_pc.clone();
            staticInst->advancePC(*next_pc);

            currentPC[tid] += this_pc.npc() - this_pc.pc();

            ++fetchStats.insts;

            DynInstPtr instruction = buildInst(tid, staticInst, nullptr, this_pc, *next_pc, true);

            // if (staticInst->isVectorConfig()) {
            //     waitForVsetvl = dec_ptr->stall();
            // }

            instruction->setVersion(localSquashVer);

            ppFetch->notify(instruction);
            numInst++;

#if TRACING_ON
            if (debug::O3PipeView) {
                instruction->fetchTick = curTick();
                DPRINTF(O3PipeView, "Record fetch for inst sn:%lu\n", instruction->seqNum);
            }
#endif

            // If we're branching after this instruction, quit fetching
            // from the same block.

        } while (dec_ptr->instReady() && numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize);

        if (fetchBufferValid[tid][currentfetchBlockID[tid]] &&
            currentPC[tid] - fetchBlockStartAddr[tid][currentfetchBlockID[tid]] >=
                fetchBlockSize[tid][currentfetchBlockID[tid]]) {

            lastFetchBlockFallThrough[tid] = !fetchBlockEndWithTaken[tid][currentfetchBlockID[tid]];
            currentDecoderInputOffsetInFetchBuffer[tid] = 0;

            // We done processing one block
            DPRINTF(FetchVerbose, "Done processing fetch block #%d\n", currentfetchBlockID[tid]);
            fetchBufferValid[tid][currentfetchBlockID[tid]] = false;

            if (currentfetchBlockID[tid] == lastSentReqAmount[tid] - 1) {
                // All fetch block have been processed.
                DPRINTF(Fetch, "All fetch block have been processed this cycle.\n");
                currentfetchBlockID[tid] = 0;  // Reset for next cycle
                break;
            }


            // Move to next fetch block
            currentfetchBlockID[tid]++;
            if (!lastFetchBlockFallThrough[tid]) {
                currentPC[tid] = fetchBlockStartAddr[tid][currentfetchBlockID[tid]];
            } else {
                DPRINTF(FetchVerbose, "currentPC fallthrough with fetch block not taken.\n");
                DPRINTF(FetchVerbose, "currentPC: %#x\n", currentPC[tid]);
                lastFetchBlockFallThrough[tid] = false;  // Reset fallthrough flag

                // Sanity check
                if (currentPC[tid] < fetchBlockStartAddr[tid][currentfetchBlockID[tid]] ||
                    (currentPC[tid] > fetchBlockStartAddr[tid][currentfetchBlockID[tid]] &&
                     currentPC[tid] - fetchBlockStartAddr[tid][currentfetchBlockID[tid]] > 4)) {
                    fatal(
                        "Fallthrough with fetch block not taken, but currentPC (%#x) does not match next "
                        "fetchBlockStartAddr (%#x).\n",
                        currentPC[tid], fetchBlockStartAddr[tid][currentfetchBlockID[tid]]);
                }
                // Skip first 4B if fall through PC is +4B then the bufferStartAddr
                if (currentPC[tid] - fetchBufferStartAddr[tid][currentfetchBlockID[tid]] == 4) {
                    currentDecoderInputOffsetInFetchBuffer[tid] = 4;
                }
            }
        }
    }

    // If returning from the delay of a cache miss, then update the status
    // to running, otherwise do the cache access.  Possibly move this up
    // to tick() function.
    if (std::all_of(fetchStatus[tid].begin(), fetchStatus[tid].end(), [](ThreadStatus s) { return s == Running; })) {
        if (!fetchBufferValid[tid][0]) {
            // DPRINTF(Fetch, "[tid:%i] Attempting to translate and read "
            //         "instruction, starting at PC %s.\n", tid, this_pc);

            DPRINTF(FetchVerbose,
                    "all fetch block status is running and buffer #0 not valid, sending ICache request.\n");

            sendFetchRequest();

            // if (fetchStatus[tid] == IcacheWaitResponse)
            //     ++fetchStats.icacheStallCycles;
            // else if (fetchStatus[tid] == ItlbWait)
            //     ++fetchStats.tlbCycles;
            // else
            //     ++fetchStats.miscStallCycles;
            return;
        }
        // else if (checkInterrupt(this_pc.instAddr()) && !delayedCommit[tid]) {
        //     // Stall CPU if an interrupt is posted and we're not issuing
        //     // an delayed commit micro-op currently (delayed commit
        //     // instructions are not interruptable by interrupts, only faults)
        //     ++fetchStats.miscStallCycles;
        //     DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
        //     return;
        // }
        if (ftqEmpty()) {
            DPRINTF(
                Fetch, "[tid:%i] Fetch is stalled due to ftq empty\n", tid);
        }
    } else {
        if (fetchStatus[tid][currentfetchBlockID[tid]] == Idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }

        // Status is Idle, so fetch should do nothing.
        return;
    }

    ++fetchStats.cycles;

    // If the read of the first instruction was successful, then grab the
    // instructions from the rest of the cache line and put them into the
    // queue heading to decode.

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to "
            "decode.\n", tid);


    // Loop through instruction memory from the cache.
    // Keep issuing while fetchWidth is available and branch is not
    // predicted taken
    StallReason stall = StallReason::NoStall;

    DPRINTF(FetchVerbose, "FetchQue start dumping\n");
    for (auto it : fetchQueue[tid]) {
        DPRINTF(FetchVerbose, "[sn:%llu] %#x: %s\n", it->seqNum, it->getPC(),
                it->staticInst->disassemble(it->pcState().instAddr()));
    }

    if (stall != StallReason::NoStall) {
        setAllFetchStalls(stall);
    }

    if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth "
                "for this cycle.\n", tid);
    }

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }
}

void
Fetch::recvReqRetry()
{
    if (retryPkt.size() == 0) {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
        return;
    }
    assert(cacheBlocked);
    // assert(retryTid != InvalidThreadID);
    // assert(fetchStatus[retryTid] == IcacheWaitRetry);

    for (auto it = retryPkt.begin(); it != retryPkt.end();) {
        if (icachePort.sendTimingReq(*it)) {
            fetchStatus[retryTid] = IcacheWaitResponse;
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify((*it)->req);
            it = retryPkt.erase(it);
        } else {
            it++;
        }
    }

    if (retryPkt.size() == 0) {
        retryTid = InvalidThreadID;
        cacheBlocked = false;
    }
}

///////////////////////////////////////
//                                   //
//  SMT FETCH POLICY MAINTAINED HERE //
//                                   //
///////////////////////////////////////
ThreadID
Fetch::getFetchingThread()
{
    if (numThreads > 1) {
        switch (fetchPolicy) {
          case SMTFetchPolicy::RoundRobin:
            return roundRobin();
          case SMTFetchPolicy::IQCount:
            return iqCount();
          case SMTFetchPolicy::LSQCount:
            return lsqCount();
          case SMTFetchPolicy::Branch:
            return branchCount();
          default:
            return InvalidThreadID;
        }
    } else {
        std::list<ThreadID>::iterator thread = activeThreads->begin();
        if (thread == activeThreads->end()) {
            return InvalidThreadID;
        }

        ThreadID tid = *thread;

        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            if (fetchStatus[tid][fetchBlockId] == Running || fetchStatus[tid][fetchBlockId] == IcacheAccessComplete ||
                fetchStatus[tid][fetchBlockId] == Idle) {
                return tid;
            }
        }

        // Not found
        return InvalidThreadID;
    }
}


ThreadID
Fetch::roundRobin()
{
    std::list<ThreadID>::iterator pri_iter = priorityList.begin();
    std::list<ThreadID>::iterator end      = priorityList.end();

    ThreadID high_pri;

    while (pri_iter != end) {
        high_pri = *pri_iter;

        assert(high_pri <= numThreads);

        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            if (fetchStatus[high_pri][fetchBlockId] == Running ||
                fetchStatus[high_pri][fetchBlockId] == IcacheAccessComplete ||
                fetchStatus[high_pri][fetchBlockId] == Idle) {

                priorityList.erase(pri_iter);
                priorityList.push_back(high_pri);

                return high_pri;
            }
        }

        pri_iter++;
    }

    return InvalidThreadID;
}

ThreadID
Fetch::iqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned iqCount = cpu->getIQInsts();

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(iqCount);
        threadMap[iqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            if (fetchStatus[high_pri][fetchBlockId] == Running ||
                fetchStatus[high_pri][fetchBlockId] == IcacheAccessComplete ||
                fetchStatus[high_pri][fetchBlockId] == Idle)
                return high_pri;
        }

        PQ.pop();
    }

    return InvalidThreadID;
}

ThreadID
Fetch::lsqCount()
{
    //sorted from lowest->highest
    std::priority_queue<unsigned, std::vector<unsigned>,
                        std::greater<unsigned> > PQ;
    std::map<unsigned, ThreadID> threadMap;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        unsigned ldstqCount = fromIEW->iewInfo[tid].ldstqCount;

        //we can potentially get tid collisions if two threads
        //have the same iqCount, but this should be rare.
        PQ.push(ldstqCount);
        threadMap[ldstqCount] = tid;
    }

    while (!PQ.empty()) {
        ThreadID high_pri = threadMap[PQ.top()];

        for (uint64_t fetchBlockId = 0; fetchBlockId < MaxFetchBlockPerCycle; fetchBlockId++) {
            if (fetchStatus[high_pri][fetchBlockId] == Running ||
                fetchStatus[high_pri][fetchBlockId] == IcacheAccessComplete ||
                fetchStatus[high_pri][fetchBlockId] == Idle)
                return high_pri;
        }

        PQ.pop();
    }

    return InvalidThreadID;
}

ThreadID
Fetch::branchCount()
{
    panic("Branch Count Fetch policy unimplemented\n");
    return InvalidThreadID;
}

void
Fetch::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (stalls[tid].drain) {
        ++fetchStats.pendingDrainCycles;
        DPRINTF(Fetch, "Fetch is waiting for a drain!\n");
    } else if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
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

} // namespace o3
} // namespace gem5
