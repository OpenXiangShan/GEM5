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

    for (int i = 0; i < MaxThreads; i++) {
        fetchStatus[i] = Idle;
        decoder[i] = nullptr;
        pc[i].reset(params.isa[0]->newPCState());
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        memReq[i] = nullptr;
        stalls[i] = {false, false};
        // fetchBuffer[i] is initialized by its constructor
        lastIcacheStall[i] = 0;
    }

    branchPred = params.branchPred;

    if (isStreamPred()) {
        dbsp = dynamic_cast<branch_prediction::stream_pred::DecoupledStreamBPU*>(branchPred);
        dbpftb = nullptr;
        dbpbtb = nullptr;
        assert(dbsp);
        usedUpFetchTargets = true;
    } else if (isFTBPred()) {
        dbsp = nullptr;
        dbpftb = dynamic_cast<branch_prediction::ftb_pred::DecoupledBPUWithFTB*>(branchPred);
        dbpbtb = nullptr;
        assert(dbpftb);
        usedUpFetchTargets = true;
        dbpftb->setCpu(_cpu);
    } else if (isBTBPred()) {
        dbsp = nullptr;
        dbpftb = nullptr;
        dbpbtb = dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB*>(branchPred);
        assert(dbpbtb);
        usedUpFetchTargets = true;
        dbpbtb->setCpu(_cpu);
    }

    assert(params.decoder.size());
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
        // Set the size and allocate data for each fetch buffer instance
        fetchBuffer[tid].size = fetchBufferSize;
        fetchBuffer[tid].data = new uint8_t[fetchBufferSize];
    }

    // Get the size of an instruction.
    // stallReason size should be the same as decodeWidth,renameWidth,dispWidth
    stallReason.resize(decodeWidth, StallReason::NoStall);

    firstCacheLineDataBuf = new uint8_t[fetchBufferSize];
    secondCacheLineDataBuf = new uint8_t[fetchBufferSize];
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
    fetchStatus[tid] = Running;
    set(pc[tid], cpu->pcState(tid));
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    memReq[tid] = NULL;
    anotherMemReq[tid] = NULL;
    stalls[tid].decode = false;
    stalls[tid].drain = false;
    fetchBuffer[tid].reset();
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
        fetchStatus[tid] = Running;
        set(pc[tid], cpu->pcState(tid));
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        memReq[tid] = NULL;
        anotherMemReq[tid] = NULL;

        stalls[tid].decode = false;
        stalls[tid].drain = false;

        fetchBuffer[tid].reset();

        fetchQueue[tid].clear();

        priorityList.push_back(tid);
    }

    wroteToTimeBuffer = false;
    _status = Inactive;

    // Initialize usedUpFetchTargets to force getting initial FTQ entry
    usedUpFetchTargets = true;

    DPRINTF(Fetch, "resetStage: set usedUpFetchTargets=true for initial FTQ setup\n");

    if (isStreamPred()) {
        dbsp->resetPC(pc[0]->instAddr());
    } else if (isFTBPred()) {
        dbpftb->resetPC(pc[0]->instAddr());
    } else if (isBTBPred()) {
        dbpbtb->resetPC(pc[0]->instAddr());
    }
}

bool
Fetch::handleAlignedFetch(Addr vaddr, ThreadID tid, Addr pc)
{
    DPRINTF(Fetch, "[tid:%i] Handling aligned fetch for addr %#x, pc=%#lx\n", tid, vaddr, pc);

    // For aligned fetch, use normal fetch size and no special handling needed
    fetchMisaligned[tid] = false;

    // Create single memory request for the aligned fetch
    RequestPtr mem_req = std::make_shared<Request>(
        vaddr, fetchBufferSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    mem_req->taskId(cpu->taskId());
    memReq[tid] = mem_req;

    // Store access information
    accessInfo[tid] = std::make_pair(vaddr, vaddr);

    // Initiate translation
    fetchStatus[tid] = ItlbWait;
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);
    return true;
}

bool
Fetch::handleMisalignedFetch(Addr vaddr, ThreadID tid, Addr pc)
{
    DPRINTF(Fetch, "[tid:%i] Handling misaligned fetch for addr %#x, pc=%#lx\n", tid, vaddr, pc);

    fetchMisaligned[tid] = true;
    firstCacheLinePkt[tid] = nullptr;
    secondCacheLinePkt[tid] = nullptr;

    Addr fetchPC = vaddr;
    unsigned fetchSize = cacheBlkSize - fetchPC % cacheBlkSize;  // Size for first cache line

    DPRINTF(Fetch, "[tid:%i] Creating first cache line request: addr=%#x, size=%d\n",
            tid, fetchPC, fetchSize);

    // Create and send first request (tail of first cache line)
    RequestPtr first_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    first_mem_req->taskId(cpu->taskId());
    first_mem_req->setMisalignedFetch();
    first_mem_req->setReqNum(1);

    memReq[tid] = first_mem_req;
    anotherMemReq[tid] = first_mem_req;

    // Initiate translation for first request
    fetchStatus[tid] = ItlbWait;
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(first_mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);

    // Prepare second request (head of second cache line)
    fetchPC += fetchSize;  // Move to start of next cache line
    assert(fetchPC % cacheBlkSize == 0);
    fetchSize = fetchBufferSize - fetchSize;  // Remaining size

    DPRINTF(Fetch, "[tid:%i] Creating second cache line request: addr=%#x, size=%d\n",
            tid, fetchPC, fetchSize);

    // Store access information before creating second request
    accessInfo[tid] = std::make_pair(vaddr, fetchPC);

    // Create and send second request
    RequestPtr second_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    second_mem_req->taskId(cpu->taskId());
    second_mem_req->setMisalignedFetch();
    second_mem_req->setReqNum(2);

    memReq[tid] = second_mem_req;  // Update memReq to point to second request

    // Handle case where we're in retry state - check after both requests are prepared
    if (fetchMisaligned[tid] && fetchStatus[tid] == IcacheWaitRetry) {
        return true;
    }

    DPRINTF(Fetch, "[tid:%i] Initiating translation for second cache line\n", tid);

    // Initiate translation for second request
    fetchStatus[tid] = ItlbWait;
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans2 = new FetchTranslation(this);
    cpu->mmu->translateTiming(second_mem_req, cpu->thread[tid]->getTC(),
                              trans2, BaseMMU::Execute);
    return true;
}

PacketPtr
Fetch::processMisalignedCompletion(ThreadID tid, PacketPtr pkt)
{
    DPRINTF(Fetch, "[tid:%i] Processing misaligned fetch completion.\n", tid);

    // Calculate the correct addresses based on original fetch address
    Addr originalVaddr = accessInfo[tid].first;
    unsigned offset = originalVaddr % cacheBlkSize;
    unsigned firstSize = cacheBlkSize - offset;
    Addr firstAddr = originalVaddr;
    Addr secondAddr = originalVaddr + firstSize;
    unsigned secondSize = fetchBufferSize - firstSize;

    Addr anotherPC = 0;
    unsigned anotherSize = 0;

    // Determine which cache line this packet belongs to and calculate the other
    if (pkt->req->getReqNum() == 1) {
        firstCacheLinePkt[tid] = pkt;
        // If we received first packet, the other is the second packet
        anotherPC = secondAddr;
        anotherSize = secondSize;
    } else if (pkt->req->getReqNum() == 2) {
        secondCacheLinePkt[tid] = pkt;
        // If we received second packet, the other is the first packet
        anotherPC = firstAddr;
        anotherSize = firstSize;
    }

    // Check if we're still waiting for the other packet
    if (firstCacheLinePkt[tid] == nullptr || secondCacheLinePkt[tid] == nullptr) {
        DPRINTF(Fetch, "[tid:%i] Waiting for %s pkt.\n", tid,
                firstCacheLinePkt[tid] == nullptr ? "first" : "second");

        // Handle retry case - need to send the missing request
        if (pkt->isRetriedPkt()) { // if the pkt is a retry pkt, we need to send another request
            DPRINTF(Fetch, "[tid:%i] Retried pkt.\n", tid);
            DPRINTF(Fetch, "[tid:%i] send next pkt, addr: %#x, size: %d\n",
                    tid, anotherPC, anotherSize);

            // Create request for the missing cache line
            RequestPtr mem_req = std::make_shared<Request>(
                                anotherPC,
                                anotherSize,
                                Request::INST_FETCH, cpu->instRequestorId(), pkt->req->getPC(),
                                cpu->thread[tid]->contextId());

            mem_req->taskId(cpu->taskId());
            mem_req->setMisalignedFetch();

            // Set request number based on which packet we received
            if (pkt->req->getReqNum() == 1) {
                mem_req->setReqNum(2);
            } else if (pkt->req->getReqNum() == 2) {
                mem_req->setReqNum(1);
            }

            anotherMemReq[tid] = memReq[tid];
            memReq[tid] = mem_req;

            fetchStatus[tid] = ItlbWait;
            FetchTranslation *trans = new FetchTranslation(this);
            cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                                      trans, BaseMMU::Execute);
        }
        return nullptr;  // Return nullptr to indicate we're still waiting
    }

    // Both packets have arrived - merge them
    DPRINTF(Fetch, "[tid:%i] Both packets arrived, merging data.\n", tid);

    // Copy data from both packets into temporary buffers
    firstCacheLinePkt[tid]->getData(firstCacheLineDataBuf);
    secondCacheLinePkt[tid]->getData(secondCacheLineDataBuf);

    // Determine which packet to use as the final packet based on memReq
    PacketPtr finalPkt;
    if (memReq[tid]->getReqNum() == 2) {
        finalPkt = secondCacheLinePkt[tid];
    } else {
        finalPkt = firstCacheLinePkt[tid];
    }

    // Merge data into the final packet
    finalPkt->setData(firstCacheLineDataBuf, 0, 0, firstCacheLinePkt[tid]->getSize());
    finalPkt->setData(secondCacheLineDataBuf, 0, firstCacheLinePkt[tid]->getSize(),
                      secondCacheLinePkt[tid]->getSize());

    return finalPkt;  // Return the merged packet
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());

    // Handle misaligned fetch completion if this is a misaligned request
    if (pkt->req->isMisalignedFetch() && (pkt->req == memReq[tid] || pkt->req == anotherMemReq[tid])) {
        DPRINTF(Fetch, "[tid:%i] Misaligned pkt receive.\n", tid);
        PacketPtr mergedPkt = processMisalignedCompletion(tid, pkt);

        // If we're still waiting for another packet, return early
        if (mergedPkt == nullptr) {
            return;
        }

        // Use the merged packet for further processing
        pkt = mergedPkt;
        DPRINTF(Fetch, "[tid:%i] Received final misaligned pkt addr=%#lx, mem_req addr=%#lx.\n", tid,
                pkt->getAddr(), pkt->req->getVaddr());
    }

    DPRINTF(Fetch, "[tid:%i] Waking up from cache miss.\n", tid);
    assert(!cpu->switchedOut());

    // Only change the status if it's still waiting on the icache access
    // to return.
    if (fetchStatus[tid] != IcacheWaitResponse ||
        pkt->req != memReq[tid]) {
        DPRINTF(Fetch, "delete pkt %#lx\n", pkt->getAddr());
        ++fetchStats.icacheSquashes;
        delete pkt;
        return;
    }

    fetchBuffer[tid].setData(fetchBuffer[tid].startPC, pkt->getConstPtr<uint8_t>(), fetchBufferSize);

    // Reset usedUpFetchTargets flag when we get new fetch data
    // This allows fetch to continue with the current FTQ entry
    if (usedUpFetchTargets) {
        DPRINTF(Fetch, "[tid:%i] Resetting usedUpFetchTargets after cache completion, "
                "fetchBufferPC=%#x\n", tid, fetchBuffer[tid].startPC);
        usedUpFetchTargets = false;
    }

    // Verify fetchBufferPC alignment with FTQ for decoupled frontend
    if (isDecoupledFrontend() && fetchBuffer[tid].valid) {
        if (isBTBPred() && dbpbtb->fetchTargetAvailable()) {
            auto& ftq_entry = dbpbtb->getSupplyingFetchTarget();
            assert(fetchBuffer[tid].startPC == ftq_entry.startPC &&
                   "fetchBufferPC should be aligned with FTQ startPC,");
            DPRINTF(Fetch, "[tid:%i] Verified fetchBufferPC %#x matches FTQ startPC %#x\n",
                    tid, fetchBuffer[tid].startPC, ftq_entry.startPC);
        } else if (isFTBPred() && dbpftb->fetchTargetAvailable()) {
            auto& ftq_entry = dbpftb->getSupplyingFetchTarget();
            assert(fetchBuffer[tid].startPC == ftq_entry.startPC &&
                   "fetchBufferPC should be aligned with FTQ startPC");
            DPRINTF(Fetch, "[tid:%i] Verified fetchBufferPC %#x matches FTQ startPC %#x\n",
                    tid, fetchBuffer[tid].startPC, ftq_entry.startPC);
        }
    }

    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    cpu->wakeCPU();

    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
            tid);

    switchToActive();

    // Only switch to IcacheAccessComplete if we're not stalled as well.
    if (checkStall(tid)) {
        fetchStatus[tid] = Blocked;
    } else {
        fetchStatus[tid] = IcacheAccessComplete;
    }

    pkt->req->setAccessLatency();
    cpu->ppInstAccessComplete->notify(pkt);
    // Reset the mem req to NULL.
    if (!pkt->req->isMisalignedFetch()) {
        delete pkt;
    } else {
        delete firstCacheLinePkt[tid];
        delete secondCacheLinePkt[tid];
        firstCacheLinePkt[tid] = nullptr;
        secondCacheLinePkt[tid] = nullptr;
    }
    memReq[tid] = NULL;
    anotherMemReq[tid] = NULL;
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
        assert(!memReq[i]);
        assert(fetchStatus[i] == Idle || stalls[i].drain);
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
            if (fetchStatus[i] == Blocked && stalls[i].drain)
                continue;
            else
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
    fetchStatus[0] = Running;
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
    bool predict_taken;

    //  BP  =>  FSQ  =>  FTB  => Fetch
    ThreadID tid = inst->threadNumber;
    if (isDecoupledFrontend()) {
        if (isStreamPred()) {
            std::tie(predict_taken, usedUpFetchTargets) =
                dbsp->decoupledPredict(
                    inst->staticInst, inst->seqNum, next_pc, tid);
            if (usedUpFetchTargets) {
                DPRINTF(DecoupleBP, "Used up fetch targets.\n");
                fetchBuffer[tid].valid = false;  // Invalidate fetch buffer when FTQ entry exhausted
            }
        }
        else  {
            if (isFTBPred()) {
                std::tie(predict_taken, usedUpFetchTargets) =
                    dbpftb->decoupledPredict(
                        inst->staticInst, inst->seqNum, next_pc, tid, currentLoopIter);
            } else if (isBTBPred()) {
                std::tie(predict_taken, usedUpFetchTargets) =
                    dbpbtb->decoupledPredict(
                        inst->staticInst, inst->seqNum, next_pc, tid, currentLoopIter);
            }
            if (usedUpFetchTargets) {
                DPRINTF(DecoupleBP, "Used up fetch targets.\n");
                fetchBuffer[tid].valid = false;  // Invalidate fetch buffer when FTQ entry exhausted
            }
            inst->setLoopIteration(currentLoopIter);
        }
    }

    // For decoupled frontend, the instruction type is predicted with BTB
    if ((isDecoupledFrontend() && !predict_taken) ||
        (!isDecoupledFrontend() && !inst->isControl())) {
        inst->staticInst->advancePC(next_pc);
        inst->setPredTarg(next_pc);
        inst->setPredTaken(false);
        return false;
    }

    if (!isDecoupledFrontend()) {
        predict_taken = branchPred->predict(inst->staticInst, inst->seqNum,
                                            next_pc, tid);
    }

    if (predict_taken) {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be taken to %s\n",
                tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    } else {
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
                "predicted to be not taken\n",
                tid, inst->seqNum, inst->pcState().instAddr());
    }

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

    // For decoupled frontend, trust the BPU-provided addresses (BPU handles alignment)
    // RISC-V C extension: mask lowest bit for instruction alignment
    // This handles cases where PC might be odd due to speculative execution,
    // but no need to throw INST_ADDR_MISALIGNED fault here
    if (vaddr % 2 != 0 || pc % 2 != 0) {
        vaddr = vaddr & ~1;
        pc = pc & ~1;
        DPRINTF(Fetch, "[tid:%i] Fetching address is misaligned, aligned to %#x, %#x\n",
                tid, vaddr, pc);
    }

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
    return handleMisalignedFetch(vaddr, tid, pc);
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());

    // For misaligned fetch, use the stored original fetch address
    // Both request 1 and request 2 should use the same fetchBufferPC
    Addr fetchPC;
    assert(mem_req->isMisalignedFetch());
    fetchPC = accessInfo[tid].first;  // Use stored original fetch address

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    if (memReq[tid] != NULL) {
        DPRINTF(Fetch, "memReq.addr=%#lx\n", memReq[tid]->getVaddr());
    }

    if (anotherMemReq[tid] != NULL) {
        DPRINTF(Fetch, "anotherMemReq.addr=%#lx\n", anotherMemReq[tid]->getVaddr());
    }

    if (!(fetchStatus[tid] == IcacheWaitResponse && mem_req->isMisalignedFetch() && (mem_req == memReq[tid] || mem_req == anotherMemReq[tid])) && 
        (fetchStatus[tid] != ItlbWait || ((mem_req != anotherMemReq[tid] || mem_req->getVaddr() != anotherMemReq[tid]->getVaddr()) && 
         (mem_req != memReq[tid] || mem_req->getVaddr() != memReq[tid]->getVaddr())))) {
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
            fetchStatus[tid] = NoGoodAddr;
            setAllFetchStalls(StallReason::OtherFetchStall);
            memReq[tid] = NULL;
            anotherMemReq[tid] = NULL;
            return;
        }

        // Build packet here.
        PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
        data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);
        if (mem_req->isMisalignedFetch())
            data_pkt->setSendRightAway();

        DPRINTF(Fetch, "[tid:%i] Fetching data for addr %#x, pc=%#lx\n",
                    tid, mem_req->getVaddr(), fetchPC);

        fetchBuffer[tid].startPC = fetchPC;
        fetchBuffer[tid].valid = false;
        DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

        fetchStats.cacheLines++;

        // Access the cache.
        if (!icachePort.sendTimingReq(data_pkt)) {
            //assert(retryPkt == NULL);
            // assert(retryTid == InvalidThreadID);
            DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

            fetchStatus[tid] = IcacheWaitRetry;
            data_pkt->setRetriedPkt();
            DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                    mem_req->getVaddr());
            setAllFetchStalls(StallReason::IcacheStall);
            retryPkt.push_back(data_pkt);
            retryTid = tid;
            cacheBlocked = true;

        } else {
            DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
            DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache "
                    "response.\n", tid);
            lastIcacheStall[tid] = curTick();
            fetchStatus[tid] = IcacheWaitResponse;
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
                tid, mem_req->getVaddr(), memReq[tid]->getVaddr());
        // Translation faulted, icache request won't be sent.
        memReq[tid] = NULL;
        anotherMemReq[tid] = NULL;

        // Send the fault to commit.  This thread will not do anything
        // until commit handles the fault.  The only other way it can
        // wake up is if a squash comes along and changes the PC.
        const PCStateBase &fetch_pc = *pc[tid];

        DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
        // We will use a nop in ordier to carry the fault.
        DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
                fetch_pc, fetch_pc, false);
        instruction->setVersion(localSquashVer);
        instruction->setNotAnInst();

        instruction->setPredTarg(fetch_pc);
        instruction->fault = fault;
        std::unique_ptr<PCStateBase> next_pc(fetch_pc.clone());
        instruction->staticInst->advancePC(*next_pc);
        set(instruction->predPC, next_pc);

        wroteToTimeBuffer = true;

        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();

        fetchStatus[tid] = TrapPending;
        setAllFetchStalls(StallReason::TrapStall);

        DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
        DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
                tid, fault->name(), *pc[tid]);
    }
    _status = updateFetchStatus();
}

void
Fetch::doSquash(PCStateBase &new_pc, const DynInstPtr squashInst, const InstSeqNum seqNum,
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

    // align PC to 2 bytes
    // This handles cases where PC might be odd due to speculative execution,
    // but no need to throw INST_ADDR_MISALIGNED fault here
    if (new_pc.instAddr() % 2 != 0) {
        // Modify new_pc directly to make it 2-byte aligned
        auto& riscv_pc = new_pc.as<RiscvISA::PCState>();
        riscv_pc.set(new_pc.instAddr() & ~1);
        set(pc[tid], new_pc);
        DPRINTF(Fetch, "[tid:%i] pc is misaligned, aligned to %#lx\n", tid, new_pc.instAddr());
    } else {
        set(pc[tid], new_pc);
    }
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    if (fetchStatus[tid] == IcacheWaitResponse) {
        DPRINTF(Fetch, "[tid:%i] Squashing outstanding Icache miss.\n",
                tid);
        memReq[tid] = NULL;
        anotherMemReq[tid] = NULL;
    } else if (fetchStatus[tid] == ItlbWait) {
        DPRINTF(Fetch, "[tid:%i] Squashing outstanding ITLB miss.\n",
                tid);
        memReq[tid] = NULL;
        anotherMemReq[tid] = NULL;
    }

    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == tid) {
        assert(cacheBlocked);
        for (auto it : retryPkt) {
            delete it;
        }
        retryPkt.clear();
        retryTid = InvalidThreadID;
        cacheBlocked = false;   // clear cache blocked
    }

    if (squashInst && !squashInst->isControl()) {
        // csrrw satp need to flush all fetch targets
        fetchBuffer[tid].valid = false;
    }

    fetchStatus[tid] = Squashing;
    setAllFetchStalls(StallReason::BpStall); // may caused by other stages like load and store

    // Empty fetch queue
    fetchQueue[tid].clear();

    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    // Set usedUpFetchTargets to force getting new FTQ entry after squash
    usedUpFetchTargets = true;
    fetchBuffer[tid].valid = false;  // clear fetch buffer valid

    DPRINTF(Fetch, "[tid:%i] Squash: set usedUpFetchTargets=true, will need new FTQ entry\n", tid);

    ++fetchStats.squashCycles;
}

void
Fetch::flushFetchBuffer()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        fetchBuffer[i].valid = false;
    }
}

Addr
Fetch::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    if (isFTBPred()) {
        return dbpftb->getPreservedReturnAddr(dynInst);
    } else if (isBTBPred()) {
        return dbpbtb->getPreservedReturnAddr(dynInst);
    } else {
        panic("getPreservedReturnAddr not implemented for this bpu");
        return 0;
    }
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
    //Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == Squashing ||
            fetchStatus[tid] == IcacheAccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (fetchStatus[tid] == IcacheAccessComplete) {
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

    // Perform fetch operations and instruction delivery
    fetchAndProcessInstructions(status_change);

    // Handle branch prediction updates
    updateBranchPredictors();
}

bool
Fetch::initializeTickState()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;

    // get the distribution of fetch status
    fetchStats.fetchStatusDist[fetchStatus[0]]++;

    // Check signal updates for all active threads
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

void
Fetch::sendInstructionsToDecode()
{
    // Send instructions enqueued into the fetch queue to decode.
    // Limit rate by fetchWidth.  Stall if decode is stalled.
    unsigned insts_to_decode = 0;
    unsigned available_insts = 0;

    // Count available instructions across all active threads
    for (auto tid : *activeThreads) {
        if (!stalls[tid].decode) {
            available_insts += fetchQueue[tid].size();
        }
    }

    // Pick a random thread to start trying to grab instructions from
    auto tid_itr = activeThreads->begin();
    std::advance(tid_itr,
            random_mt.random<uint8_t>(0, activeThreads->size() - 1));

    // Collect instructions from fetch queues until decode width is reached
    while (available_insts != 0 && insts_to_decode < decodeWidth) {
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

    // Update stall reasons based on fetch/decode status
    updateStallReasons(insts_to_decode, *tid_itr);

    // Intel TopDown method for measuring frontend bubbles
    measureFrontendBubbles(insts_to_decode, *tid_itr);

    // If there was activity this cycle, inform the CPU of it
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    // Reset the number of instructions we've fetched
    numInst = 0;
}

void
Fetch::updateStallReasons(unsigned insts_to_decode, ThreadID tid)
{
    // fetch totally stalled
    if (stalls[tid].decode) {
        // If decode stalled, use decode's stall reason
        setAllFetchStalls(fromDecode->decodeInfo[tid].blockReason);
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
    if (!stalls[tid].decode && !fromCommit->commitInfo[tid].robSquashing) {
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

    if (stalls[tid].decode) {
        fetchStats.decodeStalls++;
    }
}

void
Fetch::updateBranchPredictors()
{
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
    } else if (isBTBPred()) {
        assert(dbpbtb);
        dbpbtb->tick();
        usedUpFetchTargets = !dbpbtb->trySupplyFetchWithTarget(pc[0]->instAddr(), currentFetchTargetInLoop);
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

        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from commit.\n",tid);
        // In any case, squash.
        squash(*fromCommit->commitInfo[tid].pc,
               fromCommit->commitInfo[tid].doneSeqNum,
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
                } else if (isBTBPred()) {
                    dbpbtb->controlSquash(
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
                } else if (isBTBPred()) {
                    dbpbtb->trapSquash(
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
                    } else if (isBTBPred()) {
                        dbpbtb->nonControlSquash(
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
            } else if (isBTBPred()) {
                assert(dbpbtb);
                dbpbtb->update(fromCommit->commitInfo[tid].doneFsqId, tid);
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
                } else if (isBTBPred()) {
                    dbpbtb->controlSquash(
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

    if (checkStall(tid) &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != QuiescePending) {
        DPRINTF(Fetch, "[tid:%i] Setting to blocked\n",tid);

        fetchStatus[tid] = Blocked;

        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        DPRINTF(Fetch, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        fetchStatus[tid] = Running;

        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
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
        } else if (isBTBPred()) {
            DPRINTF(DecoupleBP, "Set instruction %lu with stream id %lu, fetch id %lu\n",
                    instruction->seqNum, dbpbtb->getSupplyingStreamId(), dbpbtb->getSupplyingTargetId());
            instruction->setFsqId(dbpbtb->getSupplyingStreamId());
            instruction->setFtqId(dbpbtb->getSupplyingTargetId());
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

ThreadID
Fetch::selectFetchThread()
{
    ThreadID tid = getFetchingThread();

    assert(!cpu->switchedOut());

    if (tid == InvalidThreadID) {
        // Breaks looping condition in tick()
        threadFetched = numFetchingThreads;

        if (numThreads == 1) {
            profileStall(0);
        }
        return InvalidThreadID;
    }

    return tid;
}

bool
Fetch::checkDecoupledFrontend(ThreadID tid)
{
    if (!isDecoupledFrontend()) {
        return true; // No decoupled frontend to check
    }

    if (isStreamPred()) {
        if (!dbsp->fetchTargetAvailable()) {
            DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
            setAllFetchStalls(StallReason::FTQBubble);
            return false;
        }
    } else if (isFTBPred()) {
        if (!dbpftb->fetchTargetAvailable()) {
            dbpftb->addFtqNotValid();
            DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
            setAllFetchStalls(StallReason::FTQBubble);
            return false;
        }
    } else if (isBTBPred()) {
        if (!dbpbtb->fetchTargetAvailable()) {
            dbpbtb->addFtqNotValid();
            DPRINTF(Fetch, "Skip fetch when FTQ head is not available\n");
            return false;
        }
    }

    return true;
}

bool
Fetch::prepareFetchAddress(ThreadID tid, bool &status_change)
{
    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC - directly use the actual instruction address
    PCStateBase &this_pc = *pc[tid];

    // Handle status transitions and cache access
    if (fetchStatus[tid] == IcacheAccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);
        fetchStatus[tid] = Running;
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
        return true;
    } else if (fetchStatus[tid] == Running) {
        // Check if we need to fetch from icache based on FTQ entry status
        // For RISC-V, we don't need ROM microcode, only check FTQ status and macroop
        if (needNewFTQEntry(tid) && !macroop[tid]) {
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled due to need new FTQ entry\n", tid);
            return true;    // to send icache request in performInstructionFetch!
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
    ThreadID tid = selectFetchThread();
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

    Addr fetch_pc = this_pc.instAddr();

    // Check if fetch buffer is valid and contains this PC
    if (!fetchBuffer[tid].valid) {
        DPRINTF(Fetch, "[tid:%i] Fetch buffer invalid, stalling on ICache\n", tid);
        return StallReason::IcacheStall;
    }

    // Check if the fetch buffer contains enough bytes for this instruction
    // We need at least 4 bytes to decode any RISC-V instruction (including compressed)
    if (fetch_pc < fetchBuffer[tid].startPC ||
        fetch_pc + 4 > fetchBuffer[tid].startPC + fetchBufferSize) {
        DPRINTF(Fetch, "[tid:%i] PC %#x outside fetch buffer range [%#x, %#x), stalling on ICache\n",
                tid, fetch_pc, fetchBuffer[tid].startPC, fetchBuffer[tid].startPC + fetchBufferSize);
        return StallReason::IcacheStall;
    }

    // Supply bytes to decoder - always provide 4 bytes for RISC-V
    auto *dec_ptr = decoder[tid];
    Addr offset_in_buffer = fetch_pc - fetchBuffer[tid].startPC;
    memcpy(dec_ptr->moreBytesPtr(),
           fetchBuffer[tid].data + offset_in_buffer, 4);

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
    DynInstPtr instruction = buildInst(tid, staticInst, curMacroop, pc, *next_pc, true);

    // Special handling for RISC-V vector configuration instructions.
    if (staticInst->isVectorConfig()) {
        waitForVsetvl = dec_ptr->stall();
        DPRINTF(Fetch, "[tid:%i] Vector config instruction, waitForVsetvl=%d\n",
                tid, waitForVsetvl);
    }

    instruction->setVersion(localSquashVer);
    ppFetch->notify(instruction);
    numInst++;

#if TRACING_ON
    if (debug::O3PipeView) {
        instruction->fetchTick = curTick();
    }
#endif

    // Save current PC to next_pc first
    set(next_pc, pc);

    // Handle branch prediction for non-decoupled frontend
    if (!isDecoupledFrontend()) {
        predictedBranch = pc.branching();
    } else { // decoupled frontend
        predictedBranch = lookupAndUpdateNextPC(instruction, *next_pc);
    }

    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Branch detected with PC = %s, target = %s\n",
                instruction->threadNumber, pc, *next_pc);
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

    return predictedBranch;
}

void
Fetch::performInstructionFetch(ThreadID tid)
{
    // Initialize local variables
    PCStateBase &pc_state = *pc[tid];
    StaticInstPtr &curMacroop = macroop[tid];

    // Control flags for main fetch loop
    bool predictedBranch = false;

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to decode.\n", tid);

    // Main instruction fetch loop - process until fetch width or other limits
    StallReason stall = StallReason::NoStall;
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize &&
           !predictedBranch && !ftqEmpty() && !waitForVsetvl) {

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
    if (!needNewFTQEntry(tid)) return;

    Addr ftq_start_pc = isDecoupledFrontend() ?
            getNextFTQStartPC(tid) : pc_state.instAddr();
    if (ftq_start_pc == 0) {
        DPRINTF(Fetch, "[tid:%i] No FTQ entry available for next fetch\n", tid);
        return;
    }
    DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access for new FTQ entry, "
                "starting at PC %#x (original PC %s)\n",
                tid, ftq_start_pc, pc_state);

    fetchCacheLine(ftq_start_pc, tid, pc_state.instAddr());
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

        if (fetchStatus[tid] == Running ||
            fetchStatus[tid] == IcacheAccessComplete ||
            fetchStatus[tid] == Idle) {
            return tid;
        } else {
            return InvalidThreadID;
        }
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

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle) {

            priorityList.erase(pri_iter);
            priorityList.push_back(high_pri);

            return high_pri;
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

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
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

        if (fetchStatus[high_pri] == Running ||
            fetchStatus[high_pri] == IcacheAccessComplete ||
            fetchStatus[high_pri] == Idle)
            return high_pri;
        else
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

bool
Fetch::needNewFTQEntry(ThreadID tid)
{
    // Check if we need a new FTQ entry based on:
    // 1. Used up current FTQ targets (decoupled frontend)
    // 2. Invalid fetch buffer (cache miss or initial state)
    bool need_new = usedUpFetchTargets || !fetchBuffer[tid].valid;

    // Assert consistency: if usedUpFetchTargets=true, fetchBuffer should be invalid
    if (isDecoupledFrontend() && usedUpFetchTargets) {
        assert(!fetchBuffer[tid].valid &&
               "fetchBuffer should be invalid when FTQ entry is exhausted");
    }

    DPRINTF(Fetch, "[tid:%i] needNewFTQEntry: usedUpFetchTargets=%d, "
            "fetchBufferValid=%d, result=%d\n",
            tid, usedUpFetchTargets, fetchBuffer[tid].valid, need_new);

    return need_new;
}

Addr
Fetch::getNextFTQStartPC(ThreadID tid)
{
    assert(isDecoupledFrontend());

    // When we need a new FTQ entry, try to supply fetch with the next target immediately
    if (usedUpFetchTargets) {
        DPRINTF(Fetch, "[tid:%i] usedUpFetchTargets=true, trying to get next FTQ entry\n", tid);

        bool in_loop = false;
        bool got_target = false;

        if (isBTBPred()) {
            got_target = dbpbtb->trySupplyFetchWithTarget(pc[tid]->instAddr(), in_loop);
        } else if (isFTBPred()) {
            got_target = dbpftb->trySupplyFetchWithTarget(pc[tid]->instAddr(), in_loop);
        } else if (isStreamPred()) {
            got_target = dbsp->trySupplyFetchWithTarget(pc[tid]->instAddr());
        }

        if (got_target) {
            DPRINTF(Fetch, "[tid:%i] Successfully got next FTQ entry, resetting usedUpFetchTargets\n", tid);
            usedUpFetchTargets = false;  // Reset flag since we got a new FTQ entry
            // Note: fetchBufferValid[tid] will be set to true later when cache line is fetched
        } else {
            DPRINTF(Fetch, "[tid:%i] Failed to get next FTQ entry, should stall fetch until FTQ available\n", tid);
            // Don't fallback to old address, return 0 to indicate stall needed
            return 0;  // Signal that fetch should stall
        }
    }

    // Now get the current supplying FTQ entry
    if (isBTBPred()) {
        assert(dbpbtb);
        auto& ftq_entry = dbpbtb->getSupplyingFetchTarget();
        Addr start_pc = ftq_entry.startPC;

        // Update fetchBufferPC to align with FTQ entry
        fetchBuffer[tid].startPC = start_pc;

        DPRINTF(Fetch, "[tid:%i] getNextFTQStartPC: FTQ entry startPC=%#x, "
                "endPC=%#x, fetchBufferPC updated to %#x\n",
                tid, start_pc, ftq_entry.endPC, fetchBuffer[tid].startPC);

        return start_pc;
    } else if (isFTBPred()) {
        assert(dbpftb);
        auto& ftq_entry = dbpftb->getSupplyingFetchTarget();
        Addr start_pc = ftq_entry.startPC;
        fetchBuffer[tid].startPC = start_pc;

        DPRINTF(Fetch, "[tid:%i] getNextFTQStartPC: FTB entry startPC=%#x, "
                "endPC=%#x, fetchBufferPC updated to %#x\n",
                tid, start_pc, ftq_entry.endPC, fetchBuffer[tid].startPC);

        return start_pc;
    } else if (isStreamPred()) {
        // For stream predictor, fall back to current fetchBufferPC
        DPRINTF(Fetch, "[tid:%i] getNextFTQStartPC: Stream predictor fallback, "
                "using fetchBufferPC=%#x\n", tid, fetchBuffer[tid].startPC);
        return fetchBuffer[tid].startPC;
    }

    panic("getNextFTQStartPC called with unsupported predictor type");
    return 0;
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
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitResponse) {
        ++fetchStats.icacheStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    } else if (fetchStatus[tid] == ItlbWait) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
                "finish!\n", tid);
    } else if (fetchStatus[tid] == TrapPending) {
        ++fetchStats.pendingTrapStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
                tid);
    } else if (fetchStatus[tid] == QuiescePending) {
        ++fetchStats.pendingQuiesceStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending quiesce "
                "instruction!\n", tid);
    } else if (fetchStatus[tid] == IcacheWaitRetry) {
        ++fetchStats.icacheWaitRetryStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                tid);
    } else if (fetchStatus[tid] == NoGoodAddr) {
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

} // namespace o3
} // namespace gem5
