/*
 * Copyright (c) 2010-2014, 2017-2021 ARM Limited
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

#include <algorithm>
#include <cassert>

#include "arch/generic/debugfaults.hh"
#include "arch/riscv/faults.hh"
#include "base/logging.hh"
#include "base/str.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/golden_global_mem.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/replay_events.hh"
#include "cpu/o3/trace/TraceInstruction.hh"
#include "cpu/utils.hh"
#include "debug/Activity.hh"
#include "debug/Diff.hh"
#include "debug/Hint.hh"
#include "debug/HtmCpu.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/O3PipeView.hh"
#include "debug/StoreBuffer.hh"
#include "debug/StorePipeline.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace o3
{



LSQUnit::LSQUnit(uint32_t lqEntries, uint32_t sqEntries,
    uint32_t physicalSqEntries,
    uint32_t ldPipeStages, uint32_t stPipeStages,
    uint32_t maxRARQEntries, uint32_t maxRAWQEntries, unsigned rarDequeuePerCycle,
    unsigned rawDequeuePerCycle, unsigned loadCompletionWidth,
    unsigned storeCompletionWidth, unsigned loadPipeCount,
    unsigned storePipeCount)
    : numSBufferRequest(0),
      numSingleRequest(0),
      numSplitRequest(0),
      lsqID(-1),
      storeQueue(sqEntries),
      loadQueue(lqEntries),
      // Use head-1 as a sentinel: no completed entry yet; advance must verify head.
      loadCompletedIdx(loadQueue.head() - 1),
      // Use head-1 as a sentinel: no completed entry yet; advance must verify head.
      storeCompletedIdx(storeQueue.head() - 1),
      loadPipeCount(loadPipeCount),
      storePipeCount(storePipeCount),
      loadPipe(ldPipeStages - 1, 0),
      storePipe(stPipeStages - 1, 0),
      physicalSQEntries(physicalSqEntries),
      virtualSQEnabled(physicalSqEntries < sqEntries),
      addrOrDataReadyNums(0),
      storesToWB(0),
      htmStarts(0),
      htmStops(0),
      lastRetiredHtmUid(0),
      cacheBlockMask(0),
      stalled(false),
      isStoreBlocked(false),
      storeInFlight(false),
      lastClockSQPopEntries(0),
      lastClockLQPopEntries(0),
      maxRARQEntries(maxRARQEntries),
      maxRAWQEntries(maxRAWQEntries),
      rarDequeuePerCycle(rarDequeuePerCycle),
      rawDequeuePerCycle(rawDequeuePerCycle),
      loadCompletionWidth(loadCompletionWidth),
      storeCompletionWidth(storeCompletionWidth),
      stats(nullptr, loadPipeCount ? loadPipeCount : 1,
            storePipeCount ? storePipeCount : 1)
{
    fatal_if(loadPipeCount == 0,
             "Scheduler must provide at least one load pipe\n");
    fatal_if(storePipeCount == 0,
             "Scheduler must provide at least one store pipe\n");

    // reserve space, we want if sq will be full, sbuffer will start evicting
    sqFullUpperLimit = sqEntries - 4;

    loadPipeSx.resize(ldPipeStages);
    storePipeSx.resize(stPipeStages);

    for (int i = 0; i < ldPipeStages; i++) {
        loadPipeSx[i] = loadPipe.getWire(-i);
    }
    for (int i = 0; i < stPipeStages; i++) {
        storePipeSx[i] = storePipe.getWire(-i);
    }
    assert(ldPipeStages >= 4 && stPipeStages >= 5);
}

void
LSQUnit::tick()
{
    countedStLdViolationThisCycle = false;
    loadPipe.advance();
    storePipe.advance();
}

void
LSQUnit::init(CPU *cpu_ptr, IEW *iew_ptr, const BaseO3CPUParams &params,
        LSQ *lsq_ptr, unsigned id)
{
    lsqID = id;

    cpu = cpu_ptr;
    iewStage = iew_ptr;

    lsq = lsq_ptr;

    cpu->addStatGroup(csprintf("lsq%i", lsqID).c_str(), &stats);

    DPRINTF(LSQUnit, "Creating LSQUnit%i object.\n",lsqID);

    system = params.system;

    depCheckShift = params.LSQDepCheckShift;
    checkLoads = params.LSQCheckLoads;
    needsTSO = params.needsTSO;

    // Clear RAR/RAW queues
    RARQueue.clear();
    RAWQueue.clear();
    RARReplayQueue.clear();
    RAWReplayQueue.clear();

    enableStorePrefetchTrain = params.store_prefetch_train;

    resetState();
}

void
LSQUnit::resetState()
{
    storesToWB = 0;

    // hardware transactional memory
    // nesting depth
    htmStarts = htmStops = 0;

    storeWBIt = storeQueue.begin();

    // Reset completed indices to head-1 sentinel: no completed entry yet.
    // This forces updateCompletedIdx to verify head before advancing.
    loadCompletedIdx = loadQueue.head() - 1;
    storeCompletedIdx = storeQueue.head() - 1;

    retryPkt = NULL;
    memDepViolator = NULL;

    stalled = false;

    // Clear RAR/RAW queues
    RARQueue.clear();
    RAWQueue.clear();
    RARReplayQueue.clear();
    RAWReplayQueue.clear();

    cacheBlockMask = ~(((uint64_t)cpu->cacheLineSize()) - 1);
}

std::string
LSQUnit::name() const
{
    if (MaxThreads == 1) {
        return iewStage->name() + ".lsq";
    } else {
        return iewStage->name() + ".lsq.thread" + std::to_string(lsqID);
    }
}

LSQUnit::LSQUnitStats::LSQUnitStats(statistics::Group *parent,
                                    unsigned loadPipeCount,
                                    unsigned storePipeCount)
    : statistics::Group(parent),
      ADD_STAT(forwLoads, statistics::units::Count::get(),
               "Number of loads that had data forwarded from stores"),
      ADD_STAT(squashedLoads, statistics::units::Count::get(),
               "Number of loads squashed"),
      ADD_STAT(pipeRawNukeReplay, statistics::units::Count::get(),
               "Number of pipeline detected raw nuke"),
      ADD_STAT(ignoredResponses, statistics::units::Count::get(),
               "Number of memory responses ignored because the instruction is "
               "squashed"),
      ADD_STAT(memOrderViolation, statistics::units::Count::get(),
               "Number of memory ordering violations"),
      ADD_STAT(ldLdViolation, statistics::units::Count::get(),
               "Number of load-load violation events"),
      ADD_STAT(stLdViolation, statistics::units::Count::get(),
               "Number of store-load violation events"),
      ADD_STAT(rawMemOrderViolation, statistics::units::Count::get(),
               "Number of RAW memory ordering violations"),
      ADD_STAT(rawViolationMdpNoPred, statistics::units::Count::get(),
               "Number of RAW violations where replay-based MDP had no producer prediction"),
      ADD_STAT(rawViolationMdpHit, statistics::units::Count::get(),
               "Number of RAW violations where replay-based MDP predicted the violating store"),
      ADD_STAT(rawViolationMdpMiss, statistics::units::Count::get(),
               "Number of RAW violations where replay-based MDP predicted other stores only"),
      ADD_STAT(rawViolationMdpStrict, statistics::units::Count::get(),
               "Number of RAW violations where replay-based MDP used strict wait"),
      ADD_STAT(loadOrderViolation, statistics::units::Count::get(),
               "Number of load-load or snoop ordering violations"),
      ADD_STAT(busForwardSuccess, statistics::units::Count::get(),
               "Number of successfully forwarding from bus"),
      ADD_STAT(cacheMissReplayEarly, statistics::units::Count::get(),
               "Number of early cache miss replay"),
      ADD_STAT(squashedStores, statistics::units::Count::get(),
               "Number of stores squashed"),
      ADD_STAT(rescheduledLoads, statistics::units::Count::get(),
               "Number of loads that were rescheduled"),
      ADD_STAT(bankConflictTimes, statistics::units::Count::get(),
               "Number of bank conflict times"),
      ADD_STAT(busAppendTimes, statistics::units::Count::get(),
               "Number of bus append times"),
      ADD_STAT(blockedByCache, statistics::units::Count::get(),
               "Number of times an access to memory failed due to the cache "
               "being blocked"),
      ADD_STAT(sbufferFull, statistics::units::Count::get(), "blocked cycle"),
      ADD_STAT(sbufferMerge, statistics::units::Count::get(),
               "Number of stores merged into an existing store buffer line"),
      ADD_STAT(sbufferNewline, statistics::units::Count::get(),
               "Number of stores that allocate a new store buffer line"),
      ADD_STAT(sbufferCreateVice, statistics::units::Count::get(), "create vice"),
      ADD_STAT(sbufferFullForward, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferPartiForward, statistics::units::Count::get(), ""),
      ADD_STAT(loadToUse, "Distribution of cycle latency between the "
                "first time a load is issued and its completion"),
      ADD_STAT(loadTranslationLat, "Distribution of cycle latency between the "
                "first time a load is issued and its translation completion"),
      ADD_STAT(forwardSTDNotReady, "Number of load forward but store data not ready"),
      ADD_STAT(STAReadyFirst, "Number of store addr ready first"),
      ADD_STAT(STDReadyFirst, "Number of store data ready first"),
      ADD_STAT(nonUnitStrideCross16Byte, "Number of vector non unitStride cross 16-byte boundary"),
      ADD_STAT(unitStrideCross16Byte, "Number of vector unitStride cross 16-byte boundary"),
      ADD_STAT(unitStrideAligned, "Number of vector unitStride 16-byte aligned"),
      ADD_STAT(skipRawWhenLoadAtS0, "Number of times RAW check skipped because load is at S0"),
      ADD_STAT(RARQueueFull, "Number of times RAR queue was full"),
      ADD_STAT(RARQueueFullCycles, statistics::units::Cycle::get(),
               "Number of cycles that RAR queue is physically full"),
      ADD_STAT(RARQueueReplay, "Number of instructions replayed from RAR queue"),
      ADD_STAT(RARQueueLatency, statistics::units::Cycle::get(), "RAR queue latency distribution"),
      ADD_STAT(RARQueueAvgEntryNum, statistics::units::Count::get(),
               "Average number of entries in RAR queue"),
      ADD_STAT(RAWQueueFull, "Number of times RAW queue was full"),
      ADD_STAT(RAWQueueFullCycles, statistics::units::Cycle::get(),
               "Number of cycles that RAW queue is physically full"),
      ADD_STAT(RAWQueueReplay, "Number of instructions replayed from RAW queue"),
      ADD_STAT(RAWQueueLatency, statistics::units::Cycle::get(), "RAW queue latency distribution"),
      ADD_STAT(RAWQueueAvgEntryNum, statistics::units::Count::get(),
               "Average number of entries in RAW queue"),
      ADD_STAT(loadPipeAccepted, statistics::units::Count::get(),
               "Number of load requests accepted by each load pipe"),
      ADD_STAT(storePipeAccepted, statistics::units::Count::get(),
               "Number of store requests accepted by each store pipe slot"),
      ADD_STAT(storeReplayTotal, statistics::units::Count::get(),
               "Number of store replay events at the store pipe replay exit"),
      ADD_STAT(storeReplayTlbMiss, statistics::units::Count::get(),
               "Number of store TLB miss replay events at the store pipe replay exit"),
      ADD_STAT(storeReplayPhysicalSQFull, statistics::units::Count::get(),
               "Number of post-issue store replays caused by a full physical SQ window"),
      ADD_STAT(storePhysicalSQReplayBlocked, statistics::units::Count::get(),
               "Number of physical-SQ replay checks blocked by the physical SQ window"),
      ADD_STAT(loadPipeReplayAccepted, statistics::units::Count::get(),
               "Number of replayQ load requests accepted by load pipe"),
      ADD_STAT(loadPipeFastReplayAccepted, statistics::units::Count::get(),
               "Number of fast replay load requests accepted by load pipe"),
      ADD_STAT(loadReplayEvents, statistics::units::Count::get(), "event distribution of load replay"),
      ADD_STAT(loadReplayEventsFromIssueQueue, statistics::units::Count::get(),
               "load replay events counted only when the load enters from issue queue")
{
    loadToUse
        .init(0, 299, 10)
        .flags(statistics::nozero);
    loadTranslationLat
        .init(0, 299, 10)
        .flags(statistics::nozero);

    RARQueueLatency
        .init(0, 500, 20)
        .flags(statistics::nozero);

    RAWQueueLatency
        .init(0, 500, 20)
        .flags(statistics::nozero);
    loadPipeAccepted
        .init(loadPipeCount)
        .flags(statistics::total | statistics::nozero);
    for (unsigned i = 0; i < loadPipeCount; i++) {
        loadPipeAccepted.subname(i, csprintf("pipe%d", i));
    }
    storePipeAccepted
        .init(storePipeCount)
        .flags(statistics::total | statistics::nozero);
    for (unsigned i = 0; i < storePipeCount; i++) {
        storePipeAccepted.subname(i, csprintf("pipe%d", i));
    }
    loadPipeReplayAccepted
        .init(loadPipeCount)
        .flags(statistics::total | statistics::nozero);
    for (unsigned i = 0; i < loadPipeCount; i++) {
        loadPipeReplayAccepted.subname(i, csprintf("pipe%d", i));
    }
    loadPipeFastReplayAccepted
        .init(loadPipeCount)
        .flags(statistics::total | statistics::nozero);
    for (unsigned i = 0; i < loadPipeCount; i++) {
        loadPipeFastReplayAccepted.subname(i, csprintf("pipe%d", i));
    }
    loadReplayEvents
        .init(LdStReplayTypeCount)
        .flags(statistics::total);
    for (int i = 0; i < LdStReplayTypeCount; i++) {
        loadReplayEvents.subname(i, load_store_replay_event_str[static_cast<LdStReplayType>(i)]);
    }
    loadReplayEventsFromIssueQueue
        .init(LdStReplayTypeCount)
        .flags(statistics::total);
    for (int i = 0; i < LdStReplayTypeCount; i++) {
        loadReplayEventsFromIssueQueue.subname(
            i, load_store_replay_event_str[static_cast<LdStReplayType>(i)]);
    }
}

void
LSQUnit::setDcachePort(RequestPort *dcache_port)
{
    dcachePort = dcache_port;
}

void
LSQUnit::drainSanityCheck() const
{
    for (int i = 0; i < loadQueue.capacity(); ++i)
        assert(!loadQueue[i].valid());

    assert(storesToWB == 0);
    assert(addrOrDataReadyNums == 0);
    assert(!retryPkt);
}

void
LSQUnit::takeOverFrom()
{
    resetState();
}

void
LSQUnit::executePipeSx()
{
    executeLoadPipeSx();
    executeStorePipeSx();
    updateCompletedIdx();
}

void
LSQUnit::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loadQueue.size());
    cprintf("Load queue: ");

    for (auto it = loadQueue.begin(); it != loadQueue.end(); ++it) {
        if (it->valid()) {
            const DynInstPtr &inst(it->instruction());
            cprintf("idx:%d %s.[sn:%llu] %s\n", it.idx(), inst->pcState(), inst->seqNum,
                    inst->isExecuted() ? "Executed" : "Not Executed");
        }
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", storeQueue.size());
    cprintf("Store queue: ");

    for (auto it = storeQueue.begin(); it != storeQueue.end(); ++it) {
        if (it->valid()) {
            const DynInstPtr &inst(it->instruction());
            cprintf("idx:%d %s.[sn:%llu] %s squashed=%d canWB=%d completed=%d "
                    "dataReady=%d staFinish=%d stdFinish=%d\n",
                    it.idx(), inst->pcState(), inst->seqNum,
                    it->addrReady() ? "AddrReady" : "Not AddrReady",
                    inst->isSquashed(), it->canWB(), it->completed(),
                    it->dataReady(), it->staFinish(), it->stdFinish());
        }
    }

    cprintf("\n");
}

void LSQUnit::schedule(Event& ev, Tick when) { cpu->schedule(ev, when); }

BaseMMU *LSQUnit::getMMUPtr() { return cpu->mmu; }

unsigned int
LSQUnit::cacheLineSize()
{
    return cpu->cacheLineSize();
}

} // namespace o3
} // namespace gem5
