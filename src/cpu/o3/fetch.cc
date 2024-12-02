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
      retryPkt(NULL),
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
    if (fetchBufferSize > cacheBlkSize)
        fatal("fetch buffer size (%u bytes) is greater than the cache "
              "block size (%u bytes)\n", fetchBufferSize, cacheBlkSize);
    if (cacheBlkSize % fetchBufferSize)
        fatal("cache block (%u bytes) is not a multiple of the "
              "fetch buffer (%u bytes)\n", cacheBlkSize, fetchBufferSize);

    for (int i = 0; i < MaxThreads; i++) {
        fetchStatus[i] = Idle;
        decoder[i] = nullptr;
        pc[i].reset(params.isa[0]->newPCState());
        fetchOffset[i] = 0;
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        memReq[i] = nullptr;
        stalls[i] = {false, false};
        fetchBuffer[i] = NULL;
        fetchBufferPC[i] = 0;
        fetchBufferValid[i] = false;
        lastIcacheStall[i] = 0;
        issuePipelinedIfetch[i] = false;
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
        // Create space to buffer the cache line data,
        // which may not hold the entire cache line.
        fetchBuffer[tid] = new uint8_t[fetchBufferSize];
    }

    // Get the size of an instruction.
    instSize = decoder[0]->moreBytesSize();

    stallReason.resize(fetchWidth, StallReason::NoStall);

    firstDataBuf = new uint8_t[fetchBufferSize];
    secondDataBuf = new uint8_t[fetchBufferSize];
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
             insts / cpu->baseStats.numCycles)
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
    fetchOffset[tid] = 0;
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    memReq[tid] = NULL;
    anotherMemReq[tid] = NULL;
    stalls[tid].decode = false;
    stalls[tid].drain = false;
    fetchBufferPC[tid] = 0;
    fetchBufferValid[tid] = false;
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
        fetchOffset[tid] = 0;
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        memReq[tid] = NULL;
        anotherMemReq[tid] = NULL;

        stalls[tid].decode = false;
        stalls[tid].drain = false;

        fetchBufferPC[tid] = 0;
        fetchBufferValid[tid] = false;

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

    if (pkt->req->isMisalignedFetch() && (pkt->req == memReq[tid] || pkt->req == anotherMemReq[tid])) {
        DPRINTF(Fetch, "[tid:%i] Misaligned pkt receive.\n", tid);
        Addr anotherPC = 0;
        unsigned anotherSize = 0;
        if (pkt->req->getReqNum() == 1) {
            firstPkt[tid] = pkt;
            anotherPC = pkt->req->getVaddr() + 64 - pkt->req->getVaddr() % 64;
            anotherSize = fetchBufferSize - pkt->getSize();
        } else if (pkt->req->getReqNum() == 2) {
            secondPkt[tid] = pkt;
            anotherPC = pkt->req->getVaddr() - 64 + pkt->getSize();
            anotherSize = fetchBufferSize - pkt->getSize();
        }

        if (firstPkt[tid] == nullptr || secondPkt[tid] == nullptr) {
            DPRINTF(Fetch, "[tid:%i] Waiting for %s pkt.\n", tid, 
                    firstPkt[tid] == nullptr ? "first" : "second");
            if (pkt->isRetriedPkt()) {
                DPRINTF(Fetch, "[tid:%i] Retried pkt.\n", tid);
                DPRINTF(Fetch, "[tid:%i] send next pkt, addr: %#x, size: %d\n",
                        tid, pkt->req->getVaddr() + 64 - pkt->req->getVaddr() % 64, 
                        fetchBufferSize - pkt->getSize());
                RequestPtr mem_req = std::make_shared<Request>(
                                    anotherPC, 
                                    anotherSize,
                                    Request::INST_FETCH, cpu->instRequestorId(), pkt->req->getPC(),
                                    cpu->thread[tid]->contextId());

                mem_req->taskId(cpu->taskId());

                mem_req->setMisalignedFetch();

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
            return;
        } else {
            DPRINTF(Fetch, "[tid:%i] Received another pkt addr=%#lx, mem_req addr=%#lx.\n", tid,
                    pkt->getAddr(), pkt->req->getVaddr());

            // Copy two packets data into second packet
            firstPkt[tid]->getData(firstDataBuf);
            secondPkt[tid]->getData(secondDataBuf);
            if (memReq[tid]->getReqNum() == 2) {
                pkt = secondPkt[tid];
            } else {
                pkt = firstPkt[tid];
            }
            pkt->setData(firstDataBuf, 0, 0, firstPkt[tid]->getSize());
            pkt->setData(secondDataBuf, 0, firstPkt[tid]->getSize(), secondPkt[tid]->getSize());
        }
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

    memcpy(fetchBuffer[tid], pkt->getConstPtr<uint8_t>(), fetchBufferSize);
    fetchBufferValid[tid] = true;

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
        delete firstPkt[tid];
        delete secondPkt[tid];
        firstPkt[tid] = nullptr;
        secondPkt[tid] = nullptr;
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
    assert(retryPkt == NULL);
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
            }
        }
        else if (isFTBPred()) {
            std::tie(predict_taken, usedUpFetchTargets) =
                dbpftb->decoupledPredict(
                    inst->staticInst, inst->seqNum, next_pc, tid, currentLoopIter);
            if (usedUpFetchTargets) {
                DPRINTF(DecoupleBP, "Used up fetch targets.\n");
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
    Fault fault = NoFault;

    assert(!cpu->switchedOut());

    // @todo: not sure if these should block translation.
    //AlphaDep
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n",
                tid);
        setAllFetchStalls(StallReason::IcacheStall);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n",
                tid);
        setAllFetchStalls(StallReason::IntStall);
        return false;
    }

    Addr fetchPC = vaddr;
    unsigned fetchSize = fetchBufferSize;

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x, pc=%#lx\n",
            tid, fetchPC, vaddr, pc);

    // Setup the memReq to do a read of the first instruction's address.
    // Set the appropriate read size and flags as well.
    // Build request here.
    if (fetchPC % 64 + fetchBufferSize > 64) {
        fetchMisaligned[tid] = true;

        firstPkt[tid] = nullptr;
        secondPkt[tid] = nullptr;

        fetchSize = 64 - fetchPC % 64;
        RequestPtr mem_req = std::make_shared<Request>(
            fetchPC, fetchSize,
            Request::INST_FETCH, cpu->instRequestorId(), pc,
            cpu->thread[tid]->contextId());

        mem_req->taskId(cpu->taskId());

        memReq[tid] = mem_req;

        anotherMemReq[tid] = mem_req;

        mem_req->setMisalignedFetch();

        DPRINTF(Fetch, "[tid:%i] Fetching first cache line %#x for addr %#x, pc=%#lx\n",
                tid, fetchPC, vaddr, pc);

        // Initiate translation of the icache block
        fetchStatus[tid] = ItlbWait;
        FetchTranslation *trans = new FetchTranslation(this);
        cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                                  trans, BaseMMU::Execute);

        fetchPC += (64 - fetchPC % 64);
        fetchSize = fetchBufferSize - fetchSize;
    } else {
        fetchMisaligned[tid] = false;
    }

    accessInfo[tid] = std::make_pair(vaddr, fetchPC);

    if (fetchMisaligned[tid] && fetchStatus[tid] == IcacheWaitRetry) {
        return true;
    }

    RequestPtr mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    mem_req->taskId(cpu->taskId());

    memReq[tid] = mem_req;

    if (fetchMisaligned[tid]) {
        DPRINTF(Fetch, "[tid:%i] Fetching second cache line %#x for addr %#x, pc=%#lx\n",
                tid, fetchPC, vaddr, pc);
        mem_req->setMisalignedFetch();
        mem_req->setReqNum(2);
    }

    // Initiate translation of the icache block
    fetchStatus[tid] = ItlbWait;
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);
    return true;
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());
    Addr fetchMisalignedPC = mem_req->getVaddr();
    if (mem_req->getReqNum() == 2) {
        fetchMisalignedPC = mem_req->getVaddr() - 64 + mem_req->getSize();
    }
    Addr fetchPC = mem_req->isMisalignedFetch() ? fetchMisalignedPC : mem_req->getVaddr();

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

        fetchBufferPC[tid] = fetchPC;
        fetchBufferValid[tid] = false;
        DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

        fetchStats.cacheLines++;

        // Access the cache.
        if (!icachePort.sendTimingReq(data_pkt)) {
            assert(retryPkt == NULL);
            assert(retryTid == InvalidThreadID);
            DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

            fetchStatus[tid] = IcacheWaitRetry;
            data_pkt->setRetriedPkt();
            DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                    mem_req->getVaddr());
            setAllFetchStalls(StallReason::IcacheStall);
            retryPkt = data_pkt;
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
        if (retryPkt) {
            delete retryPkt;
        }
        retryPkt = NULL;
        retryTid = InvalidThreadID;
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
        fetchBufferValid[i] = false;
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

    for (ThreadID i = 0; i < numThreads; ++i) {
        issuePipelinedIfetch[i] = false;
    }

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
        // Fetch each of the actively fetching threads.
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

    // Issue the next I-cache request if possible.
    for (ThreadID i = 0; i < numThreads; ++i) {
        if (issuePipelinedIfetch[i]) {
            pipelineIcacheAccesses(i);
        }
    }

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

    for (int i = 0;i < toDecode->fetchStallReason.size();i++) {
        if (i < insts_to_decode) {
            toDecode->fetchStallReason[i] = StallReason::NoStall;
        } else if(stalls[*tid_itr].decode) {
            toDecode->fetchStallReason[i] = fromDecode->decodeInfo[*tid_itr].blockReason;
        }
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
        dbpftb->tick();
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
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
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

    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC.
    PCStateBase &this_pc = *pc[tid];

    Addr pc_offset = fetchOffset[tid];
    Addr fetch_addr = (this_pc.instAddr() + pc_offset) & decoder[tid]->pcMask();

    bool in_rom = isRomMicroPC(this_pc.microPC());

    // if (isStreamPred()) {
    //     const auto &ftq_head = dbsp->getSupplyingFetchTarget();

    //     if (enableLoopBuffer && !loopBufferisActive() && ftq_head.taken) {
    //         // don't touch the state when already in loop buf
    //         // look up loop buffer: whether current FTQe is loop
    //         loopBuffer.tryActivateLoop(ftq_head.takenPC, ftq_head.predLoopIteration);
    //     }
    // }

    // If returning from the delay of a cache miss, then update the status
    // to running, otherwise do the cache access.  Possibly move this up
    // to tick() function.
    if (fetchStatus[tid] == IcacheAccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);

        fetchStatus[tid] = Running;
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
    } else if (fetchStatus[tid] == Running) {
        // If buffer is no longer valid or fetch_addr has moved to point
        // to the next cache block, AND we have no remaining ucode
        // from a macro-op, then start fetch from icache.
        if (!(fetchBufferValid[tid] &&
              fetchBufferPC[tid] + fetchBufferSize > fetch_addr && fetchBufferPC[tid] <= fetch_addr) &&
            !in_rom && !macroop[tid] && !currentFetchTargetInLoop) {
            DPRINTF(Fetch, "[tid:%i] Attempting to translate and read "
                    "instruction, starting at PC %s.\n", tid, this_pc);

            fetchCacheLine(fetch_addr, tid, this_pc.instAddr());

            if (fetchStatus[tid] == IcacheWaitResponse)
                ++fetchStats.icacheStallCycles;
            else if (fetchStatus[tid] == ItlbWait)
                ++fetchStats.tlbCycles;
            else
                ++fetchStats.miscStallCycles;
            return;
        } else if (checkInterrupt(this_pc.instAddr()) && !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted and we're not issuing
            // an delayed commit micro-op currently (delayed commit
            // instructions are not interruptable by interrupts, only faults)
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return;
        }
        if (ftqEmpty()) {
            DPRINTF(
                Fetch, "[tid:%i] Fetch is stalled due to ftq empty\n", tid);
        }
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }

        // Status is Idle, so fetch should do nothing.
        return;
    }

    ++fetchStats.cycles;

    std::unique_ptr<PCStateBase> next_pc(this_pc.clone());

    StaticInstPtr staticInst = NULL;
    StaticInstPtr curMacroop = macroop[tid];

    // If the read of the first instruction was successful, then grab the
    // instructions from the rest of the cache line and put them into the
    // queue heading to decode.

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to "
            "decode.\n", tid);

    // Need to keep track of whether or not a predicted branch
    // ended this fetch block.
    bool predictedBranch = false;

    // Need to halt fetch if quiesce instruction detected
    bool quiesce = false;

    // num_insts_per_buffer: number of instruction payloads (usually in 4bytes)
    // in the fetchBuffer. Note that it does not consider RVC or x86's variable
    // inst length. It only indicates the number of 4 byte chunks.
    const unsigned num_insts_per_buffer = fetchBufferSize / instSize;

    // block offset: offset of the fetch_addr in the fetchBuffer/loopBuffer.
    // Note that it is counted with the number of instruction payloads
    // instead of in bytes.
    unsigned blk_offset =
        currentFetchTargetInLoop && enableLoopBuffer
            ? (fetch_addr - loopBuffer->getActiveLoopStart()) / instSize
            : (fetch_addr - fetchBufferPC[tid]) / instSize;

    auto *dec_ptr = decoder[tid];
    const Addr pc_mask = dec_ptr->pcMask();

    auto stallDuetoVset = false;

    // Loop through instruction memory from the cache.
    // Keep issuing while fetchWidth is available and branch is not
    // predicted taken
    StallReason stall = StallReason::NoStall;
    bool exit_loopbuffer_this_cycle = false;
    bool cond_taken_backward = false;
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize &&
           !(predictedBranch && !currentFetchTargetInLoop) && !quiesce &&
           !ftqEmpty() && !exit_loopbuffer_this_cycle && !waitForVsetvl) {
        // We need to process more memory if we aren't going to get a
        // StaticInst from the rom, the current macroop, or what's already
        // in the decoder.
        // insts from loop buffer is decoded, we do not need instruction bytes
        bool need_mem = !in_rom && !curMacroop && !dec_ptr->instReady() && !currentFetchTargetInLoop;
        fetch_addr = (this_pc.instAddr() + pc_offset) & pc_mask;

        if (need_mem) {
            // If buffer is no longer valid or fetch_addr has moved to point
            // to the next cache block then start fetch from icache.
            if (!currentFetchTargetInLoop && !fetchBufferValid[tid]) {
                stall = StallReason::IcacheStall;
                break;
            }

            if (!currentFetchTargetInLoop && blk_offset >= num_insts_per_buffer) {
                // We need to process more memory, but we've run out of the
                // current block.
                stall = StallReason::IcacheStall;
                break;
            }

            // if (loopBuffer->isActive()) {
            //     memcpy(dec_ptr->moreBytesPtr(),
            //             loopBuffer.activePointer + blk_offset * instSize, instSize);
            //     bool run_out_loop_entry = loopBuffer.notifyOffset(blk_offset);
            //     exit_loopbuffer_this_cycle = run_out_loop_entry;
            // } else {
            memcpy(dec_ptr->moreBytesPtr(),
                    fetchBuffer[tid] + blk_offset * instSize, instSize);
            DPRINTF(Fetch, "Supplying fetch from fetchBuffer\n");
            // }

            decoder[tid]->moreBytes(this_pc, fetch_addr);

            if (dec_ptr->needMoreBytes()) {
                blk_offset++;
                fetch_addr += instSize;
                pc_offset += instSize;
            }
        }

        // Extract as many instructions and/or microops as we can from
        // the memory we've processed so far.
        do {
            if (!(curMacroop || in_rom)) {
                if (dec_ptr->instReady() || (isFTBPred() && enableLoopBuffer && currentFetchTargetInLoop)) {
                    if (isFTBPred() && enableLoopBuffer && currentFetchTargetInLoop) {
                        auto instDesc = dbpftb->lb.supplyInst();
                        staticInst = instDesc.inst;
                        dec_ptr->setPCStateWithInstDesc(instDesc.compressed, this_pc);
                        DPRINTF(LoopBuffer, "Supplying inst pc %#lx from loop buffer pc %#lx\n",
                            this_pc.instAddr(), instDesc.pc);
                        assert(this_pc.instAddr() == instDesc.pc);
                    } else {
                        staticInst = dec_ptr->decode(this_pc);
                    }

                    // Increment stat of fetched instructions.
                    ++fetchStats.insts;

                    if (staticInst->isMacroop()) {
                        curMacroop = staticInst;
                    } else {
                        pc_offset = 0;
                    }
                } else {
                    // We need more bytes for this instruction so blkOffset and
                    // pcOffset will be updated
                    break;
                }
            }
            // Whether we're moving to a new macroop because we're at the
            // end of the current one, or the branch predictor incorrectly
            // thinks we are...
            bool newMacro = false;
            if (curMacroop || in_rom) {
                if (in_rom) {
                    staticInst = dec_ptr->fetchRomMicroop(
                            this_pc.microPC(), curMacroop);
                } else {
                    staticInst = curMacroop->fetchMicroop(this_pc.microPC());
                }
                newMacro |= staticInst->isLastMicroop();
            }

            DynInstPtr instruction = buildInst(
                    tid, staticInst, curMacroop, this_pc, *next_pc, true);

            if (staticInst->isVectorConfig()) {
                waitForVsetvl = dec_ptr->stall();
            }

            instruction->setVersion(localSquashVer);

            if (enableLoopBuffer) {
                // record this static inst of current ftq entry
                currentFtqEntryInsts.second.push_back(loopBuffer->genInstDesc(
                    instruction->getInstBytes() == 2, staticInst, this_pc.instAddr()));
            }

            ppFetch->notify(instruction);
            numInst++;

#if TRACING_ON
            if (debug::O3PipeView) {
                instruction->fetchTick = curTick();
                DPRINTF(O3PipeView, "Record fetch for inst sn:%lu\n",
                        instruction->seqNum);
            }
#endif

            set(next_pc, this_pc);

            // If we're branching after this instruction, quit fetching
            // from the same block.
            if (!isDecoupledFrontend()) {
                predictedBranch |= this_pc.branching();
            }
            predictedBranch |= lookupAndUpdateNextPC(instruction, *next_pc);
            if (predictedBranch) {
                DPRINTF(Fetch, "Branch detected with PC = %s\n", this_pc);
            }
            cond_taken_backward = predictedBranch && next_pc->instAddr() < this_pc.instAddr() && staticInst->isCondCtrl();
            if (enableLoopBuffer) {
                if (!predictedBranch && instruction->staticInst->isCondCtrl()) {
                    notTakenBranchEncountered = true;
                }
            }

            newMacro |= this_pc.instAddr() != next_pc->instAddr();

            // Move to the next instruction, unless we have a branch.
            set(this_pc, *next_pc);
            in_rom = isRomMicroPC(this_pc.microPC());

            if (newMacro) {
                fetch_addr = this_pc.instAddr() & pc_mask;
                blk_offset = (fetch_addr - fetchBufferPC[tid]) / instSize;
                pc_offset = 0;
                curMacroop = NULL;
            }

            if (instruction->isQuiesce()) {
                DPRINTF(Fetch,
                        "Quiesce instruction encountered, halting fetch!\n");
                fetchStatus[tid] = QuiescePending;
                status_change = true;
                quiesce = true;
                break;
            }
        } while ((curMacroop || dec_ptr->instReady()) &&
                 numInst < fetchWidth &&
                 fetchQueue[tid].size() < fetchQueueSize);

        // Re-evaluate whether the next instruction to fetch is in micro-op ROM
        // or not.
        in_rom = isRomMicroPC(this_pc.microPC());
    }

    DPRINTF(FetchVerbose, "FetchQue start dumping\n");
    for (auto it : fetchQueue[tid]) {
        DPRINTF(FetchVerbose, "inst: %s\n", it->staticInst->disassemble(it->pcState().instAddr()));
    }

    for (int i = 0;i < fetchWidth;i++) {
        if (i < numInst)
            stallReason[i] = StallReason::NoStall;
        else {
            if (numInst > 0) {
                stallReason[i] = StallReason::FetchFragStall;
            } else if (stall  != StallReason::NoStall) {
                stallReason[i] = stall;
            } else if (stalls[tid].decode && fetchQueue[tid].size() >= fetchQueueSize) {
                stallReason[i] = fromDecode->decodeInfo[tid].blockReason;
            } else {
                stallReason[i] = StallReason::OtherFetchStall;
            }
        }
    }

    if (enableLoopBuffer && isFTBPred()) {
        if (ftqEmpty()) {
            currentLoopIter = 0;
            
            if (!currentFetchTargetInLoop) {
                // try to record static insts of current ftq entry to loop buffer spec entry
                if (cond_taken_backward && currentFtqEntryInsts.second.size() <= loopBuffer->maxLoopInsts) {
                    if (!notTakenBranchEncountered) {
                        DPRINTF(LoopBuffer, "ftq entry ended by backward taken conditional branch, try to record insts in loop buffer, pc %#lx\n",
                            currentFtqEntryInsts.first);
                        loopBuffer->fillSpecLoopBuffer(currentFtqEntryInsts.first, currentFtqEntryInsts.second);
                    } else {
                        DPRINTF(LoopBuffer, "not taken branch encountered in ftq entry, not record insts in loop buffer, pc %#lx\n",
                            currentFtqEntryInsts.first);
                    }
                }
                // try to record new ftq entry
                currentFtqEntryInsts.first = this_pc.instAddr();
                currentFtqEntryInsts.second.clear();
                notTakenBranchEncountered = false;
            }
        }
    }

    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, predicted branch "
                "instruction encountered.\n", tid);
    } else if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth "
                "for this cycle.\n", tid);
    } else if (blk_offset >= fetchBufferSize) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached the end of the"
                "fetch buffer.\n", tid);
    }

    macroop[tid] = curMacroop;
    fetchOffset[tid] = pc_offset;

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    // pipeline a fetch if we're crossing a fetch buffer boundary and not in
    // a state that would preclude fetching
    fetch_addr = (this_pc.instAddr() + pc_offset) & pc_mask;
    Addr fetchBufferBlockPC = fetchBufferAlignPC(fetch_addr);
    issuePipelinedIfetch[tid] = fetchBufferBlockPC != fetchBufferPC[tid] &&
        !currentFetchTargetInLoop &&
        fetchStatus[tid] != IcacheWaitResponse &&
        fetchStatus[tid] != ItlbWait &&
        fetchStatus[tid] != IcacheWaitRetry &&
        fetchStatus[tid] != QuiescePending &&
        !curMacroop;
}

void
Fetch::recvReqRetry()
{
    if (retryPkt != NULL) {
        assert(cacheBlocked);
        assert(retryTid != InvalidThreadID);
        assert(fetchStatus[retryTid] == IcacheWaitRetry);

        if (icachePort.sendTimingReq(retryPkt)) {
            fetchStatus[retryTid] = IcacheWaitResponse;
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify(retryPkt->req);
            retryPkt = NULL;
            retryTid = InvalidThreadID;
            cacheBlocked = false;
        }
    } else {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
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

void
Fetch::pipelineIcacheAccesses(ThreadID tid)
{
    if (!issuePipelinedIfetch[tid]) {
        return;
    }

    // The next PC to access.
    const PCStateBase &this_pc = *pc[tid];

    if (isRomMicroPC(this_pc.microPC())) {
        return;
    }

    Addr pcOffset = fetchOffset[tid];
    Addr fetchAddr = (this_pc.instAddr() + pcOffset) & decoder[tid]->pcMask();

    // Unless buffer already got the block, fetch it from icache.
    if (!(fetchBufferValid[tid] && (fetchBufferPC[tid] + fetchBufferSize > fetchAddr))) {
        DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access, "
                "starting at PC %s.\n", tid, this_pc);

        fetchCacheLine(fetchAddr, tid, this_pc.instAddr());
    }
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
