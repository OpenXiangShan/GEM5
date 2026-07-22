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

LSQUnit::AddrRangeCoverage
LSQUnit::checkStoreLoadForwardingRange(typename StoreQueue::iterator store_it,
                                      LSQRequest *request, const DynInstPtr &load_inst,
                                      int load_req_idx, int store_req_idx)
{
    // Extract address ranges from parameters
    Addr req_s, req_e;
    if (load_req_idx == -1) {
        // Complete load request
        req_s = request->mainReq()->getPaddr();
        req_e = req_s + request->mainReq()->getSize();
    } else {
        // Split load sub-request
        req_s = request->_reqs[load_req_idx]->getPaddr();
        req_e = req_s + request->_reqs[load_req_idx]->getSize();
    }

    Addr st_s, st_e;
    if (store_req_idx == -1) {
        // Complete store request
        st_s = store_it->instruction()->physEffAddr;
        st_e = st_s + store_it->size();
    } else {
        // Split store sub-request
        st_s = store_it->request()->_reqs[store_req_idx]->getPaddr();
        st_e = st_s + store_it->request()->_reqs[store_req_idx]->getSize();
    }

    bool store_is_split = store_it->request() && store_it->request()->isSplit();

    bool store_has_lower_limit = req_s >= st_s;
    bool store_has_upper_limit = req_e <= st_e;
    bool lower_load_has_store_part = req_s < st_e;
    bool upper_load_has_store_part = req_e > st_s;

    DPRINTF(LSQUnit, "load_idx:%d,store_idx:%d req_s:%x,req_e:%x,st_s:%x,st_e:%x\n",
            load_req_idx, store_req_idx, req_s, req_e, st_s, st_e);
    DPRINTF(LSQUnit, "store_size:%x,store_pc:%s,req_size:%x,req_pc:%s\n",
            st_e - st_s, store_it->instruction()->pcState(),
            req_e - req_s, request->instruction()->pcState());

    // Check for complete coverage - store fully contains the load request
    if ((!store_it->instruction()->isAtomic() &&
         store_has_lower_limit && store_has_upper_limit &&
         !request->mainReq()->isLLSC()) &&
        (!((req_s > req_e) || (st_s > st_e)))) {

        const auto &store_req = store_it->request()->mainReq();

        // Check if store data is ready for forwarding
        if (store_it->instruction()->isSplitStoreAddr() && !store_it->canForwardToLoad()) {
            // Store data not ready, need to set STLF replay flag for load
            load_inst->setSTLFReplay();
            return AddrRangeCoverage::NoAddrRangeCoverage;
        } else {
            // Store can forward to load
            // For split stores, always return partial coverage to ensure proper handling
            if (store_is_split) {
                return AddrRangeCoverage::PartialAddrRangeCoverage;
            } else {
                return store_req->isMasked()
                    ? AddrRangeCoverage::PartialAddrRangeCoverage
                    : AddrRangeCoverage::FullAddrRangeCoverage;
            }
        }
    }
    // Check for partial coverage - store and load have some overlap
    else if ((!((req_s > req_e) || (st_s > st_e))) &&
             ((!request->mainReq()->isLLSC() &&
               ((store_has_lower_limit && lower_load_has_store_part) ||
                (store_has_upper_limit && upper_load_has_store_part) ||
                (lower_load_has_store_part && upper_load_has_store_part))) ||
              (request->mainReq()->isLLSC() &&
               ((store_has_lower_limit || upper_load_has_store_part) &&
                (store_has_upper_limit || lower_load_has_store_part))) ||
              (store_it->instruction()->isAtomic() &&
               ((store_has_lower_limit || upper_load_has_store_part) &&
                (store_has_upper_limit || lower_load_has_store_part))))) {

        return AddrRangeCoverage::PartialAddrRangeCoverage;
    }

    // No overlap between store and load addresses
    return AddrRangeCoverage::NoAddrRangeCoverage;
}

void
LSQUnit::SQEntry::setStatus(SplitStoreStatus status)
{
    _addrReady |= status == SplitStoreStatus::AddressReady;
    _dataReady |= status == SplitStoreStatus::DataReady;
    _staFinish |= status == SplitStoreStatus::StaPipeFinish;
    _stdFinish |= status == SplitStoreStatus::StdPipeFinish;
    if (splitStoreFinish()) {
        instruction()->setExecuted();
    } else {
        assert(!instruction()->isExecuted());
    }
}

LSQUnit::WritebackRegEvent::WritebackRegEvent(const DynInstPtr &_inst,
        PacketPtr _pkt, LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), request(_inst->savedRequest), pkt(_pkt), lsqPtr(lsq_ptr)
{
    assert(request);
    request->writebackScheduled();
}

void
LSQUnit::WritebackRegEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    lsqPtr->writebackReg(inst, pkt);

    assert(request);
    request->writebackDone();
    delete pkt;
}

const char *
LSQUnit::WritebackRegEvent::description() const
{
    return "writeback to reg";
}

LSQUnit::bankConflictReplayEvent::bankConflictReplayEvent(LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete), lsqPtr(lsq_ptr)
{
}

void
LSQUnit::bankConflictReplayEvent::process()
{
    lsqPtr->bankConflictReplay();
}

const char *
LSQUnit::bankConflictReplayEvent::description() const
{
    return "bankConflictReplayEvent";
}

LSQUnit::tagReadFailReplayEvent::tagReadFailReplayEvent(LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete), lsqPtr(lsq_ptr)
{
}

void
LSQUnit::tagReadFailReplayEvent::process()
{
    lsqPtr->tagReadFailReplay();
}

const char *
LSQUnit::tagReadFailReplayEvent::description() const
{
    return "tagReadFailReplayEvent";
}

bool
LSQUnit::recvTimingResp(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest *>(pkt->senderState);
    assert(request != nullptr);

    if (request->instruction()) {
        DPRINTF(LSQUnit, "LSQUnit::recvTimingResp [sn:%lu] pkt: %s\n", request->instruction()->seqNum, pkt->print());
    } else {
        DPRINTF(StoreBuffer, "LSQUnit::recvTimingResp sbuffer entry[%#lx]\n",
                dynamic_cast<LSQ::SbufferRequest *>(request)->sbuffer_entry->blockPaddr);
    }
    bool ret = true;
    /* Check that the request is still alive before any further action. */
    if (!request->isReleased()) {
        ret = request->recvTimingResp(pkt);
    } else if (request->instruction()) {
        DPRINTF(LoadPipeline, "LSQUnit::recvTimingResp [sn:%lu] pkt: %s - ignored\n",
                request->instruction()->seqNum, pkt->print());
        request->instruction()->hasPendingCacheReq(false);
        request->instruction()->pendingCacheReq = nullptr;
    }

    return ret;
}

void
LSQUnit::completeDataAccess(PacketPtr pkt)
{
    LSQRequest *request = dynamic_cast<LSQRequest *>(pkt->senderState);
    DynInstPtr inst = request->instruction();

    // hardware transactional memory
    // sanity check
    if (pkt->isHtmTransactional() && !inst->isSquashed()) {
        assert(inst->getHtmTransactionUid() == pkt->getHtmTransactionUid());
    }

    // if in a HTM transaction, it's possible
    // to abort within the cache hierarchy.
    // This is signalled back to the processor
    // through responses to memory requests.
    if (pkt->htmTransactionFailedInCache()) {
        // cannot do this for write requests because
        // they cannot tolerate faults
        const HtmCacheFailure htm_rc =
            pkt->getHtmTransactionFailedInCacheRC();
        if (pkt->isWrite()) {
            DPRINTF(HtmCpu,
                "store notification (ignored) of HTM transaction failure "
                "in cache - addr=0x%lx - rc=%s - htmUid=%d\n",
                pkt->getAddr(), htmFailureToStr(htm_rc),
                pkt->getHtmTransactionUid());
        } else {
            HtmFailureFaultCause fail_reason =
                HtmFailureFaultCause::INVALID;

            if (htm_rc == HtmCacheFailure::FAIL_SELF) {
                fail_reason = HtmFailureFaultCause::SIZE;
            } else if (htm_rc == HtmCacheFailure::FAIL_REMOTE) {
                fail_reason = HtmFailureFaultCause::MEMORY;
            } else if (htm_rc == HtmCacheFailure::FAIL_OTHER) {
                // these are likely loads that were issued out of order
                // they are faulted here, but it's unlikely that these will
                // ever reach the commit head.
                fail_reason = HtmFailureFaultCause::OTHER;
            } else {
                panic("HTM error - unhandled return code from cache (%s)",
                      htmFailureToStr(htm_rc));
            }

            inst->fault =
            std::make_shared<GenericHtmFailureFault>(
                inst->getHtmTransactionUid(),
                fail_reason);

            DPRINTF(HtmCpu,
                "load notification of HTM transaction failure "
                "in cache - pc=%s - addr=0x%lx - "
                "rc=%u - htmUid=%d\n",
                inst->pcState(), pkt->getAddr(),
                htmFailureToStr(htm_rc), pkt->getHtmTransactionUid());
        }
    }

    cpu->ppDataAccessComplete->notify(std::make_pair(inst, pkt));

    // Trace-mode: If this is a load, replace returning data with trace value.
    // KISS: only if metadata exists and value present; DRY: reuse CPU API.
    if (!cpu->switchedOut() && cpu->isTraceMode() && inst->isLoad()) {
        const o3::TraceInstruction* ti = cpu->getTraceInstMetadata(inst->seqNum);
        if (ti && ti->getLoad() && !ti->getLoadValues().empty()) {
            // Use the first value; respect packet size to avoid overruns.
            const auto &vals = ti->getLoadValues();
            const size_t copy_sz = std::min<size_t>(pkt->getSize(), sizeof(uint64_t));
            uint8_t *dst = pkt->getPtr<uint8_t>();
            // Values recorded are per-access; reinterpret as little-endian bytes.
            uint64_t v = vals[0];
            std::memcpy(dst, &v, copy_sz);
            DPRINTF(LoadPipeline, "[sn:%llu] Trace overrides load data to 0x%llx (bytes=%zu)\n",
                    inst->seqNum, (unsigned long long)v, copy_sz);
        }
    }

    assert(!cpu->switchedOut());
    if (!inst->isSquashed()) {
        if (inst->isLoad() || inst->isAtomic()) {
            Addr addr = pkt->getAddr();
            auto [enable_diff, diff_all_states] = cpu->getDiffAllStates();
            if (system->multiContextDifftest() && enable_diff &&
                request->_sbufferBypass &&
                inst->isLoad() &&
                cpu->goldenMemManager()->inPmem(addr)) {
                // A store-forwarded load may legitimately observe a value that
                // is newer than the current shared golden memory snapshot.
                // Keep the observed value on the instruction so difftest can
                // repair the reference state for this hart if needed.
                inst->setGolden(pkt->getPtr<uint8_t>());
            }
            if (system->multiContextDifftest() && enable_diff &&
                !request->_sbufferBypass &&
                cpu->goldenMemManager()->inPmem(addr)) {
                uint8_t *loaded_data = pkt->getPtr<uint8_t>();
                size_t size = pkt->getSize();
                assert(size == inst->effSize);

                if (inst->isAtomic()) {
                    panic_if(size > sizeof(uint64_t),
                             "Unexpected AMO size %zu at addr %#lx\n",
                             size, addr);
                    // Preserve the DUT-observed old value until completeStore()
                    // derives the post-AMO memory image. Keep the actual
                    // response value for difftest, since the request may have
                    // been serialized behind another hart's AMO by the cache.
                    inst->setGolden(loaded_data);
                    std::memcpy(inst->getAmoOldGoldenValuePtr(), loaded_data,
                                size);
                } else {
                    // check data with golden mem
                    uint8_t *golden_data =
                        (uint8_t *)cpu->goldenMemManager()->guestToHost(addr);
                    if (memcmp(golden_data, loaded_data, size) != 0) {
                        DPRINTF(Diff,
                                "[tid:%d] [sn:%llu] Load sees value different from "
                                "current golden memory at addr %#lx, size %d. "
                                "Treating as concurrent update window. %s\n",
                                inst->threadNumber, inst->seqNum, addr, size,
                                goldenDiffStr(loaded_data, golden_data, size).c_str());
                    }
                }
            }
        }

        if (request->needWBToRegister()) {
            // Only loads, store conditionals and atomics perform the writeback
            // after receving the response from the memory
            assert(inst->isLoad() || inst->isStoreConditional() ||
                   inst->isAtomic());

            // hardware transactional memory
            if (pkt->htmTransactionFailedInCache()) {
                request->mainPacket()->setHtmTransactionFailedInCache(
                    pkt->getHtmTransactionFailedInCacheRC() );
            }

            writebackReg(inst, request->mainPacket());
            if (inst->isStore() || inst->isAtomic()) {
                request->writebackDone();
                completeStore(request->instruction()->sqIt);
            }
        } else if (inst->isStore()) {
            // This is a regular store (i.e., not store conditionals and
            // atomics), so it can complete without writing back
            completeStore(request->instruction()->sqIt);
        }
    }
}

LSQUnit::LSQUnit(uint32_t lqEntries, uint32_t sqEntries,
    uint32_t ldPipeStages, uint32_t stPipeStages,
    uint32_t maxRARQEntries, uint32_t maxRAWQEntries, unsigned rarDequeuePerCycle,
    unsigned rawDequeuePerCycle, unsigned loadCompletionWidth, unsigned storeCompletionWidth)
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
      loadPipe(ldPipeStages - 1, 0),
      storePipe(stPipeStages - 1, 0),
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
      stats(nullptr)
{
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
    mptWaitLoads.clear();
    mptWaitStores.clear();

    enableStorePrefetchTrain = params.store_prefetch_train;

    resetState();
}

void
LSQUnit::bankConflictReplay()
{
    iewStage->cacheUnblocked();
}

void
LSQUnit::tagReadFailReplay()
{
    iewStage->cacheUnblocked();
}

void
LSQUnit::bankConflictReplaySchedule()
{
    bankConflictReplayEvent *bk = new bankConflictReplayEvent(this);
    cpu->schedule(bk, cpu->clockEdge(Cycles(1)));
}

void
LSQUnit::tagReadFailReplaySchedule()
{
    tagReadFailReplayEvent *e = new tagReadFailReplayEvent(this);
    cpu->schedule(e, cpu->clockEdge(Cycles(1)));
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
    mptWaitLoads.clear();
    mptWaitStores.clear();

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

LSQUnit::LSQUnitStats::LSQUnitStats(statistics::Group *parent)
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
      ADD_STAT(storeReplayMpt, statistics::units::Count::get(),
               "Number of store MPT wait events at the store pipe replay exit"),
      ADD_STAT(mptLoadWaits, statistics::units::Count::get(),
               "Loads parked waiting for MPT permission"),
      ADD_STAT(mptStoreWaits, statistics::units::Count::get(),
               "Stores parked waiting for MPT permission"),
      ADD_STAT(mptLoadWakeups, statistics::units::Count::get(),
               "Loads woken from the MPT wait queue"),
      ADD_STAT(mptStoreWakeups, statistics::units::Count::get(),
               "Stores woken from the MPT wait queue"),
      ADD_STAT(mptWaitRequestCycles, statistics::units::Cycle::get(),
               "Accumulated request-cycles spent in MPT wait queues"),
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
    // TODO: load-pipe PMU vectors currently assume exactly three load pipes.
    // Extend the vector sizing and IssueQue loadPipeId mapping together if the
    // model grows more load pipes; the replay pipe counters below share this
    // same assumption.
    loadPipeAccepted
        .init(3)
        .flags(statistics::total | statistics::nozero);
    for (int i = 0; i < 3; i++) {
        loadPipeAccepted.subname(i, csprintf("pipe%d", i));
    }
    storePipeAccepted
        .init(MaxPipeWidth)
        .flags(statistics::total | statistics::nozero);
    for (int i = 0; i < MaxPipeWidth; i++) {
        storePipeAccepted.subname(i, csprintf("pipe%d", i));
    }
    loadPipeReplayAccepted
        .init(3)
        .flags(statistics::total | statistics::nozero);
    for (int i = 0; i < 3; i++) {
        loadPipeReplayAccepted.subname(i, csprintf("pipe%d", i));
    }
    loadPipeFastReplayAccepted
        .init(3)
        .flags(statistics::total | statistics::nozero);
    for (int i = 0; i < 3; i++) {
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
    assert(!retryPkt);
    assert(mptWaitLoads.empty());
    assert(mptWaitStores.empty());
}

void
LSQUnit::takeOverFrom()
{
    resetState();
}

void
LSQUnit::insert(const DynInstPtr &inst)
{
    assert(inst->isMemRef());

    assert(inst->isLoad() || inst->isStore() || inst->isAtomic());

    if (inst->isLoad()) {
        insertLoad(inst);
    } else {
        insertStore(inst);
    }

    inst->setInLSQ();
}

void
LSQUnit::insertLoad(const DynInstPtr &load_inst)
{
    assert(!loadQueue.full());
    assert(loadQueue.size() < loadQueue.capacity());

    DPRINTF(LSQUnit, "Inserting load PC %s, idx:%i [sn:%lli]\n",
            load_inst->pcState(), loadQueue.tail(), load_inst->seqNum);

    /* Grow the queue. */
    loadQueue.advance_tail();

    load_inst->sqIt = storeQueue.end();

    assert(!loadQueue.back().valid());
    loadQueue.back().set(load_inst);
    load_inst->lqIdx = loadQueue.tail();
    assert(load_inst->lqIdx > 0);
    load_inst->lqIt = loadQueue.getIterator(load_inst->lqIdx);

    // hardware transactional memory
    // transactional state and nesting depth must be tracked
    // in the in-order part of the core.
    if (load_inst->isHtmStart()) {
        htmStarts++;
        DPRINTF(HtmCpu, ">> htmStarts++ (%d) : htmStops (%d)\n",
                htmStarts, htmStops);

        const int htm_depth = htmStarts - htmStops;
        const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
        auto htm_uid = htm_cpt->getHtmUid();

        // for debugging purposes
        if (!load_inst->inHtmTransactionalState()) {
            htm_uid = htm_cpt->newHtmUid();
            DPRINTF(HtmCpu, "generating new htmUid=%u\n", htm_uid);
            if (htm_depth != 1) {
                DPRINTF(HtmCpu,
                    "unusual HTM transactional depth (%d)"
                    " possibly caused by mispeculation - htmUid=%u\n",
                    htm_depth, htm_uid);
            }
        }
        load_inst->setHtmTransactionalState(htm_uid, htm_depth);
    }

    if (load_inst->isHtmStop()) {
        htmStops++;
        DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops++ (%d)\n",
                htmStarts, htmStops);

        if (htmStops==1 && htmStarts==0) {
            DPRINTF(HtmCpu,
            "htmStops==1 && htmStarts==0. "
            "This generally shouldn't happen "
            "(unless due to misspeculation)\n");
        }
    }
}

void
LSQUnit::insertStore(const DynInstPtr& store_inst)
{
    // Make sure it is not full before inserting an instruction.
    assert(!storeQueue.full());
    assert(storeQueue.size() < storeQueue.capacity());

    DPRINTF(LSQUnit, "Inserting store PC %s, idx:%i [sn:%lli]\n",
            store_inst->pcState(), storeQueue.tail(), store_inst->seqNum);
    storeQueue.advance_tail();

    store_inst->sqIdx = storeQueue.tail();
    store_inst->sqIt = storeQueue.getIterator(store_inst->sqIdx);

    store_inst->lqIdx = loadQueue.tail() + 1;
    assert(store_inst->lqIdx > 0);
    store_inst->lqIt = loadQueue.end();

    storeQueue.back().set(store_inst);
}

LSQUnit::LSQRequest *
LSQUnit::currentLoadRequest(const DynInstPtr &inst)
{
    return (inst && inst->lqIdx >= 0) ? loadQueue[inst->lqIdx].request()
                                      : nullptr;
}

LSQUnit::LSQRequest *
LSQUnit::currentStoreRequest(const DynInstPtr &inst)
{
    return (inst && inst->sqIdx >= 0) ? storeQueue[inst->sqIdx].request()
                                      : nullptr;
}

bool
LSQUnit::splitStoreAddrSquashed(const DynInstPtr &inst)
{
    if (!inst->isSplitStoreData()) {
        return false;
    }

    if (!storeQueue.isValidIdx(inst->sqIdx)) {
        return true;
    }

    auto sq_it = storeQueue.getIterator(inst->sqIdx);
    if (!sq_it->valid()) {
        return true;
    }

    const auto &sta_inst = sq_it->instruction();
    if (!sta_inst || sta_inst->seqNum != inst->seqNum) {
        return true;
    }

    return sta_inst->isSquashed();
}

bool
LSQUnit::pipeLineNukeCheck(const DynInstPtr &load_inst, const DynInstPtr &store_inst)
{
    Addr load_eff_addr1 = load_inst->physEffAddr >> depCheckShift;
    Addr load_eff_addr2 = (load_inst->physEffAddr + load_inst->effSize - 1) >> depCheckShift;

    Addr store_eff_addr1 = store_inst->physEffAddr >> depCheckShift;
    Addr store_eff_addr2 = (store_inst->physEffAddr + store_inst->effSize - 1) >> depCheckShift;

    LSQRequest* store_req = currentStoreRequest(store_inst);
    LSQRequest* load_req = currentLoadRequest(load_inst);
    // Dont perform pipe line nuke check for split load
    bool load_is_splited = load_req && load_req->isSplit();
    bool load_need_check = !load_is_splited && load_inst->effAddrValid() &&
                            (load_inst->lqIt >= store_inst->lqIt);
    bool store_need_check = store_req && store_req->isTranslationComplete() &&
                            store_req->isMemAccessRequired() && (store_inst->getFault() == NoFault);

    if (lsq->enablePipeNukeCheck() && load_need_check && store_need_check) {
        if (load_eff_addr1 <= store_eff_addr2 && store_eff_addr1 <= load_eff_addr2) {
            return true;
        }
    }
    return false;
}

DynInstPtr
LSQUnit::getMemDepViolator()
{
    DynInstPtr temp = memDepViolator;

    memDepViolator = NULL;

    return temp;
}

unsigned
LSQUnit::numFreeLoadEntries()
{
        DPRINTF(LSQUnit, "LQ size: %d, #loads occupied: %d\n",
                loadQueue.capacity(), loadQueue.size());
        return loadQueue.capacity() - loadQueue.size();
}

unsigned
LSQUnit::numFreeStoreEntries()
{
        DPRINTF(LSQUnit, "SQ size: %d, #stores occupied: %d\n",
                storeQueue.capacity(), storeQueue.size());
        return storeQueue.capacity() - storeQueue.size();

}

unsigned
LSQUnit::getAndResetLastClockLQPopEntries(){
    unsigned num = lastClockLQPopEntries;
    lastClockLQPopEntries = 0;
    return num;
}

unsigned
LSQUnit::getAndResetLastClockSQPopEntries(){
    unsigned num = lastClockSQPopEntries;
    lastClockSQPopEntries = 0;
    return num;
}

void
LSQUnit::checkSnoop(PacketPtr pkt)
{
    // Should only ever get invalidations in here
    assert(pkt->isInvalidate());

    DPRINTF(LSQUnit, "Got snoop for address %#x\n", pkt->getAddr());

    for (int x = 0; x < cpu->numContexts(); x++) {
        gem5::ThreadContext *tc = cpu->getContext(x);
        bool no_squash = cpu->thread[x]->noSquashFromTC;
        cpu->thread[x]->noSquashFromTC = true;
        tc->getIsaPtr()->handleLockedSnoop(pkt, cacheBlockMask);
        cpu->thread[x]->noSquashFromTC = no_squash;
    }

    if (loadQueue.empty())
        return;

    auto iter = loadQueue.begin();

    Addr invalidate_addr = pkt->getAddr() & cacheBlockMask;

    DynInstPtr ld_inst = iter->instruction();
    assert(ld_inst);
    LSQRequest *request = iter->request();

    // Check that this snoop didn't just invalidate our lock flag
    if (ld_inst->effAddrValid() && request &&
        request->isCacheBlockHit(invalidate_addr, cacheBlockMask)
        && ld_inst->memReqFlags & Request::LLSC) {
        ld_inst->tcBase()->getIsaPtr()->handleLockedSnoopHit(ld_inst.get());
    }

    bool force_squash = false;

    while (++iter != loadQueue.end()) {
        ld_inst = iter->instruction();
        assert(ld_inst);
        request = iter->request();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered())
            continue;

        DPRINTF(LSQUnit, "-- inst [sn:%lli] to pktAddr:%#x\n",
                    ld_inst->seqNum, invalidate_addr);

        if (force_squash ||
            (request && request->isCacheBlockHit(invalidate_addr, cacheBlockMask))) {
            if (needsTSO) {
                // If we have a TSO system, as all loads must be ordered with
                // all other loads, this load as well as *all* subsequent loads
                // need to be squashed to prevent possible load reordering.
                force_squash = true;
            }
            if (ld_inst->possibleLoadViolation() || force_squash) {
                DPRINTF(LSQUnit, "Conflicting load at addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Mark the load for re-execution
                ld_inst->fault = std::make_shared<ReExec>();
                request->setStateToFault();
            } else {
                DPRINTF(LSQUnit, "HitExternal Snoop for addr %#x [sn:%lli]\n",
                        pkt->getAddr(), ld_inst->seqNum);

                // Make sure that we don't lose a snoop hitting a LOCKED
                // address since the LOCK* flags don't get updated until
                // commit.
                if (ld_inst->memReqFlags & Request::LLSC) {
                    ld_inst->tcBase()->getIsaPtr()->
                        handleLockedSnoopHit(ld_inst.get());
                }

                // If a older load checks this and it's true
                // then we might have missed the snoop
                // in which case we need to invalidate to be sure
                ld_inst->hitExternalSnoop(true);
            }
        }
    }
    return;
}

namespace
{

bool
overlapsVisibleStore(const o3::LSQ::LSQRequest *load_req, Addr store_paddr,
                     const std::vector<bool> &store_byte_enable)
{
    if (!load_req) {
        return false;
    }

    for (size_t req_idx = 0; req_idx < load_req->numReqs(); ++req_idx) {
        const auto req = load_req->req(req_idx);
        if (!req->hasPaddr()) {
            continue;
        }

        const Addr load_start = req->getPaddr();
        const Addr load_end = load_start + req->getSize();
        for (size_t byte_idx = 0; byte_idx < store_byte_enable.size();
             ++byte_idx) {
            if (!store_byte_enable[byte_idx]) {
                continue;
            }

            const Addr byte_addr = store_paddr + byte_idx;
            if (byte_addr >= load_start && byte_addr < load_end) {
                return true;
            }
        }
    }

    return false;
}

} // anonymous namespace

void
LSQUnit::checkLocalStoreVisible(Addr store_paddr,
                                const std::vector<bool> &store_byte_enable)
{
    if (loadQueue.empty()) {
        return;
    }

    const Addr block_addr = store_paddr & cacheBlockMask;
    DynInstPtr oldest_violator = memDepViolator;

    for (auto it = loadQueue.begin(); it != loadQueue.end(); ++it) {
        DynInstPtr ld_inst = it->instruction();
        if (!ld_inst || ld_inst->isSquashed() || ld_inst->needReplay() ||
            !ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            continue;
        }

        LSQRequest *request = it->request();
        // Replay/cancel paths can leave the dyninst carrying a stale
        // savedRequest pointer after the active LQ request has been replaced
        // or dropped. Only the current queue entry request is safe here.
        if (!request || !request->isCacheBlockHit(block_addr, cacheBlockMask)) {
            continue;
        }
        if (!overlapsVisibleStore(request, store_paddr, store_byte_enable)) {
            continue;
        }
        if (ld_inst->memReqFlags & Request::LLSC) {
            ld_inst->tcBase()->getIsaPtr()->handleLockedSnoopHit(ld_inst.get());
        }

        if (ld_inst->isExecuted()) {
            DPRINTF(LSQUnit,
                    "Local visible store ignores already executed load "
                    "[sn:%lli] on addr %#x\n",
                    ld_inst->seqNum, store_paddr);
            continue;
        }

        ld_inst->hitExternalSnoop(true);
        ld_inst->possibleLoadViolation(true);
        DPRINTF(LSQUnit,
                "Local visible store replays not-yet-executed load [sn:%lli] "
                "on addr %#x\n",
                ld_inst->seqNum, store_paddr);
        ld_inst->setNukeReplay();
        loadSetReplay(ld_inst, request, true);
    }

    if (oldest_violator &&
        (!memDepViolator || oldest_violator->seqNum < memDepViolator->seqNum)) {
        memDepViolator = oldest_violator;
        cpu->activityThisCycle();
        iewStage->SquashCheckAfterExe(oldest_violator);
    }
}

Fault
LSQUnit::checkViolations(typename LoadQueue::iterator& loadIt,
        const DynInstPtr& inst)
{
    LSQRequest *inst_request = nullptr;
    if (inst->isLoad()) {
        inst_request = currentLoadRequest(inst);
    } else if (inst->isStore() || inst->isAtomic()) {
        inst_request = currentStoreRequest(inst);
    }

    // Replay/cancel paths can drop the active LSQ request before the
    // instruction is retried. In that window the dyninst may still carry a
    // stale savedRequest pointer, so only the current LSQ entry request is
    // safe to inspect here.
    if (!inst_request) {
        return NoFault;
    }

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    while (loadIt != loadQueue.end()) {
        DynInstPtr ld_inst = loadIt->instruction();
        LSQRequest *load_request = loadIt->request();
        if (!ld_inst || !load_request || !ld_inst->effAddrValid() ||
                ld_inst->strictlyOrdered()) {
            ++loadIt;
            continue;
        }

        // The LQ is ordered oldest to youngest. Once the remaining loads are
        // newer than an already-recorded violator, they cannot improve the
        // squash point for this check.
        if (!inst->isLoad() && memDepViolator &&
                ld_inst->seqNum > memDepViolator->seqNum) {
            return NoFault;
        }

        bool done_checking_load = false;
        for (auto req0 : inst_request->_reqs) {
            Addr inst_eff_addr1 = req0->getPaddr() >> depCheckShift;
            Addr inst_eff_addr2 =
                (req0->getPaddr() + req0->getSize() - 1) >> depCheckShift;

            DPRINTF(LSQUnit, "Checking for violations for [sn:%lli], addr: %#lx\n",
                    inst->seqNum, req0->getPaddr());

            for (auto req1 : load_request->_reqs) {
                Addr ld_eff_addr1 = req1->getPaddr() >> depCheckShift;
                Addr ld_eff_addr2 = (req1->getPaddr() + req1->getSize() - 1) >> depCheckShift;

                DPRINTF(LSQUnit, "Checking for violations for load [sn:%lli], addr: %#lx\n",
                        ld_inst->seqNum, req1->getPaddr());
                if (inst_eff_addr2 >= ld_eff_addr1 && inst_eff_addr1 <= ld_eff_addr2) {
                    if (inst->isLoad()) {
                        // If this load is to the same block as an external snoop
                        // invalidate that we've observed then the load needs to be
                        // squashed as it could have newer data
                        if (ld_inst->hitExternalSnoop()) {
                            if (!memDepViolator ||
                                    ld_inst->seqNum < memDepViolator->seqNum) {
                                DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] "
                                        "and [sn:%lli] at address %#x\n",
                                        inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                                memDepViolator = ld_inst;

                                ++stats.memOrderViolation;
                                ++stats.ldLdViolation;
                                ++stats.loadOrderViolation;

                                return std::make_shared<GenericISA::M5PanicFault>(
                                    "Detected fault with inst [sn:%lli] and "
                                    "[sn:%lli] at address %#x\n",
                                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                            }
                        }

                        // Otherwise, mark the load has a possible load violation and
                        // if we see a snoop before it's commited, we need to squash
                        ld_inst->possibleLoadViolation(true);
                        DPRINTF(LSQUnit, "Found possible load violation at addr: %#x"
                                " between instructions [sn:%lli] and [sn:%lli]\n",
                                inst_eff_addr1, inst->seqNum, ld_inst->seqNum);
                        done_checking_load = true;
                        break;
                    } else {
                        // A load/store incorrectly passed this store.
                        // Check if we already have a violator, or if it's newer
                        // squash and refetch.
                        if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum) {
                            return NoFault;
                        }

                        // if this load has been marked as Nuke, the load will then be replayed
                        // So next time this load replaying to pipeline will forward from store correctly
                        // And no RAW violation happens
                        if (ld_inst->needNukeReplay()) {
                            done_checking_load = true;
                            break;
                        }

                        // If load is at stage 0 while store is at stage 1, no RAW violation
                        // should occur since the load can forward data from store later
                        if (ld_inst->skipRawCheck()) {
                            ++stats.skipRawWhenLoadAtS0;
                            done_checking_load = true;
                            break;
                        }

                        DPRINTF(LSQUnit,
                                "ld_eff_addr1: %#x, ld_eff_addr2: %#x, "
                                "inst_eff_addr1: %#x, inst_eff_addr2: %#x\n",
                                ld_eff_addr1, ld_eff_addr2, inst_eff_addr1,
                                inst_eff_addr2);
                        DPRINTF(LSQUnit, "Detected fault with inst [sn:%lli] and "
                                "[sn:%lli] at address %#x\n",
                                inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                        memDepViolator = ld_inst;

                        ++stats.memOrderViolation;
                        if (inst->isStore() && !countedStLdViolationThisCycle) {
                            ++stats.stLdViolation;
                            countedStLdViolationThisCycle = true;
                        }
                        ++stats.rawMemOrderViolation;
                        if (ld_inst->mdpPredStrictWait) {
                            ++stats.rawViolationMdpStrict;
                        } else if (ld_inst->mdpProducingStores.empty()) {
                            ++stats.rawViolationMdpNoPred;
                        } else if (std::find(ld_inst->mdpProducingStores.begin(),
                                             ld_inst->mdpProducingStores.end(),
                                             inst->seqNum) !=
                                   ld_inst->mdpProducingStores.end()) {
                            ++stats.rawViolationMdpHit;
                        } else {
                            ++stats.rawViolationMdpMiss;
                        }

                        return std::make_shared<GenericISA::M5PanicFault>(
                            "Detected fault with "
                            "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                            inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
                    }
                }
            }
            if (done_checking_load) {
                break;
            }
        }
        ++loadIt;
    }
    return NoFault;
}

void
LSQUnit::loadSetReplay(DynInstPtr inst, LSQRequest* request, bool dropReqNow)
{
    // clear state in this instruction
    inst->effAddrValid(false);
    // Reset DTB translation state
    inst->translationStarted(false);
    inst->translationCompleted(false);
    inst->savedRequest = nullptr;
    // clear request in loadQueue
    loadQueue[inst->lqIdx].setRequest(nullptr);
    if (dropReqNow) {
        request->discard();
    }

    DPRINTF(LoadPipeline, "Load [sn:%ld] set replay, dropReqNow: %d\n", inst->seqNum, dropReqNow);
}

void
LSQUnit::issueToLoadPipe(const DynInstPtr &inst)
{
    // S0 is the single load-pipe entry point. First issues, slow replays, and
    // fast replays all enter here; LoadPipeSource records which path fed the
    // pipe for RTL-aligned stats.
    assert(loadPipeSx[0]->size < MaxPipeWidth);
    panic_if(inst->inPipe(), "load [sn:%llu] is already in pipeline", inst->seqNum);
    inst->beginPipelining();
    inst->setSkipRawCheck();

    int idx = loadPipeSx[0]->size;
    loadPipeSx[0]->insts[idx] = inst;
    loadPipeSx[0]->size++;

    const int load_pipe_id = inst->issueQue->getLoadPipeId();
    panic_if(load_pipe_id < 0,
        "Per-load-pipe PMU stats require dedicated load IQ naming; "
        "unsupported issue queue %s for load [sn:%llu]\n",
        inst->issueQue->getName().c_str(), inst->seqNum);
    stats.loadPipeAccepted[load_pipe_id]++;
    const auto load_pipe_source = inst->getLoadPipeSource();
    if (load_pipe_source == DynInst::LoadPipeSource::ReplayQueue) {
        stats.loadPipeReplayAccepted[load_pipe_id]++;
    } else if (load_pipe_source == DynInst::LoadPipeSource::FastReplay) {
        stats.loadPipeFastReplayAccepted[load_pipe_id]++;
    }

    DPRINTF(LoadPipeline, "issueToLoadPipe: [sn:%llu]\n", inst->seqNum);
}

void
LSQUnit::issueToStorePipe(const DynInstPtr &inst)
{
    // push to storePipeS0
    assert(storePipeSx[0]->size < MaxPipeWidth);
    panic_if(inst->inPipe(), "load [sn:%llu] is already in pipeline", inst->seqNum);
    inst->beginPipelining();

    int idx = storePipeSx[0]->size;
    storePipeSx[0]->insts[idx] = inst;
    storePipeSx[0]->size++;
    stats.storePipeAccepted[idx]++;

    DPRINTF(LSQUnit, "issueToStorePipe: [sn:%lli]\n", inst->seqNum);
}

void
LSQUnit::serviceMptWaitQueues()
{
    stats.mptWaitRequestCycles +=
        mptWaitLoads.size() + mptWaitStores.size();

    auto wakeLoads = [this]() {
        for (auto it = mptWaitLoads.begin();
             it != mptWaitLoads.end() &&
                 loadPipeSx[0]->size < MaxPipeWidth;) {
            const DynInstPtr inst = *it;
            if (inst->isSquashed()) {
                it = mptWaitLoads.erase(it);
                continue;
            }
            if (!inst->translationCompleted()) {
                ++it;
                continue;
            }

            it = mptWaitLoads.erase(it);
            inst->setLoadPipeSource(DynInst::LoadPipeSource::MptWait);
            issueToLoadPipe(inst);
            ++stats.mptLoadWakeups;
        }
    };

    auto wakeStores = [this]() {
        for (auto it = mptWaitStores.begin();
             it != mptWaitStores.end() &&
                 storePipeSx[0]->size < MaxPipeWidth;) {
            const DynInstPtr inst = *it;
            if (inst->isSquashed()) {
                it = mptWaitStores.erase(it);
                continue;
            }
            if (!inst->translationCompleted()) {
                ++it;
                continue;
            }

            it = mptWaitStores.erase(it);
            issueToStorePipe(inst);
            ++stats.mptStoreWakeups;
        }
    };

    wakeLoads();
    wakeStores();
    if (!mptWaitLoads.empty() || !mptWaitStores.empty())
        cpu->activityThisCycle();
}

Fault
LSQUnit::loadDoTranslate(const DynInstPtr &inst)
{
    DPRINTF(LoadPipeline, "loadDoTranslate: load [sn:%llu]\n", inst->seqNum);
    assert(!inst->isSquashed());

    Fault load_fault = NoFault;
    // Now initiateAcc only does TLB access
    load_fault = inst->initiateAcc();

    auto *request = currentLoadRequest(inst);
    if (inst->isTranslationDelayed() && load_fault == NoFault) {
        if (request != nullptr && request->isMptDelayed()) {
            inst->setMptReplay();
            DPRINTF(LoadPipeline, "Load [sn:%llu] waits for MPT\n",
                    inst->seqNum);
        } else {
            inst->setTLBMissReplay();
            DPRINTF(LoadPipeline, "Load [sn:%llu] setTLBMissReplay\n",
                    inst->seqNum);
        }
    }

    if (request && request->isTranslationComplete()) {
        inst->setNormalLd(request->isNormalLd());

        cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::VAddress, inst->effAddr);
        cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::PAddress, inst->physEffAddr);
    }

    return load_fault;
}

Fault
LSQUnit::loadDoSendRequest(const DynInstPtr &inst)
{
    DPRINTF(LoadPipeline, "loadDoSendRequest: load [sn:%lli]\n", inst->seqNum);
    assert(!inst->isSquashed());
    Fault load_fault = inst->getFault();
    LSQRequest* request = currentLoadRequest(inst);

    if (inst->effAddrValid()) {
        // S1 can still catch a same-cycle RAW/nuke against a store already in
        // the store pipe. Replaying here avoids letting the load observe data
        // before the conflicting store has published its address/data state.
        for (int i = 0; i < storePipeSx[1]->size; i++) {
            auto& store_inst = storePipeSx[1]->insts[i];
            if (pipeLineNukeCheck(inst, store_inst)) {
                DPRINTF(LoadPipeline, "Load [sn:%llu] Nuke need replay\n", inst->seqNum);
                ++stats.pipeRawNukeReplay;
                inst->setProducerStorePC(store_inst->pcState().instAddr());
                inst->setNukeReplay();
                return NoFault;
            }
        }
    }

    // normal inst cache access
    if (request && request->isTranslationComplete()) {
        panic_if(request->isMptDelayed(),
                 "Load [sn:%llu] reached cache-send stage before MPT "
                 "permission completed\n", inst->seqNum);
        if (request->isMemAccessRequired()) {
            // read() is the main LSQ access: it may satisfy the load from
            // SQ/SBuffer forwarding, send a DCache request, or leave replay
            // state on the instruction/request for later stages to consume.
            Fault fault;
            fault = read(request, inst->lqIdx);
            // inst->getFault() may have the first-fault of a
            // multi-access split request at this point.
            // Overwrite that only if we got another type of fault
            // (e.g. re-exec).
            if (fault != NoFault) {
                inst->getFault() = fault;
                load_fault = fault;
            }
        } else {
            inst->setMemAccPredicate(false);
            // Commit will have to clean up whatever happened.  Set this
            // instruction as executed.
            inst->setExecuted();
        }
    }

    if (load_fault == NoFault && !inst->readMemAccPredicate()) {
        assert(inst->readPredicate());
        inst->setExecuted();
        inst->completeAcc(nullptr);
        inst->setSkipFollowingPipe();
        DPRINTF(LoadPipeline, "Load [sn:%lli] not executed from readMemAccPredicate\n",
                inst->seqNum);
        return NoFault;
    }

    if (load_fault != NoFault && inst->translationCompleted() &&
            request && request->isPartialFault()
            && !request->isComplete()) {
        assert(request->isSplit());
        // If we have a partial fault where the mem access is not complete yet
        // then the cache must have been blocked. This load will be re-executed
        // when the cache gets unblocked. We will handle the fault when the
        // mem access is complete.
        // not replay or cancel
        return NoFault;
    }

    if (load_fault != NoFault || !inst->readPredicate()) {
        if (!inst->readPredicate())
            inst->forwardOldRegs();

        DPRINTF(LoadPipeline, "Load [sn:%llu] not executed from %s\n",
                inst->seqNum, (load_fault != NoFault ? "fault" : "readPredicate"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) || inst->isAtCommit()) {
            inst->setExecuted();
        }
        inst->setSkipFollowingPipe();
        inst->setCanCommit();
        return load_fault;
    }

    if (inst->needReplay()) { return NoFault; }

    if (inst->effAddrValid()) {
        // rar violation check
        auto it = inst->lqIt;
        ++it;
        if (checkLoads)
            load_fault = checkViolations(it, inst);
    }

    return load_fault;
}

Fault
LSQUnit::loadDoRecvData(const DynInstPtr &inst)
{
    Fault fault = inst->getFault();
    DPRINTF(LoadPipeline, "loadDoRecvData: load [sn:%lli]\n", inst->seqNum);

    assert(!inst->isSquashed());
    LSQRequest* request = currentLoadRequest(inst);
    bool earlyWakeupCacheMissReplay = false;

    // S2 is the load pipe's replay-selection point.  It first checks whether
    // an early-woken load actually has data, then folds cache, forwarding,
    // nuke, and RAW/RAR tracking outcomes into one replay reason or the
    // normal completion path below.
    if (inst->wakeUpEarly()) {
        auto& bus = getLsq()->bus;
        bool busFwdSuccess = bus.find(inst->seqNum) != bus.end();
        if (inst->hasPendingCacheReq() || !busFwdSuccess) {
            // Load has been waken up too early, even no TimingResp at load s2
            // Or load received TimingResp any time at [s1, s2], but can not find data on bus
            // replay this load

            DPRINTF(LoadPipeline, "Load [sn:%ld]: Early wakeup, no data on bus\n",
                    inst->seqNum);
            earlyWakeupCacheMissReplay = true;
        } else {
            // Load received TimingResp any time at [s1, s2], forward from data bus
            DPRINTF(LoadPipeline, "Load [sn:%ld]: Forward from bus at load s2, data: %lx\n",
                    inst->seqNum, *((uint64_t*)(inst->memData)));
            panic_if(bus.size() > lsq->getLQEntries(), "packets on bus should never be greater than LQ size");
            forwardFromBus(inst, request);
        }
    }

    const bool cacheMissReplay =
        earlyWakeupCacheMissReplay ||
        (lsq->enableLdMissReplay() && request && request->isNormalLd() &&
         !inst->fullForward() && !inst->cacheHit());
    const bool bankConflictReplay = inst->isNormalLd() && !request;

    bool nukeReplay = false;
    for (int i = 0; i < storePipeSx[1]->size; i++) {
        auto& store_inst = storePipeSx[1]->insts[i];
        if (pipeLineNukeCheck(inst, store_inst)) {
            nukeReplay = true;
            break;
        }
    }

    const bool trackRAR =
        loadCompletedIdx != loadQueue.tail() && inst->isNormalLd() &&
        inst->lqIt.idx() > loadCompletedIdx + 1;
    const bool rarReplay =
        trackRAR && lsq->logicalFreeRAREntries(lsqID) == 0;
    const bool trackRAW =
        storeCompletedIdx != storeQueue.tail() && inst->isNormalLd() &&
        inst->sqIt.idx() > storeCompletedIdx + 1;
    const bool rawReplay =
        trackRAW && lsq->logicalFreeRAWEntries(lsqID) == 0;

    if (cacheMissReplay) {
        inst->markReplayFlag(LdStReplayType::CacheMissReplay);
    }
    if (bankConflictReplay) {
        inst->markReplayFlag(LdStReplayType::BankConflictReplay);
    }
    if (nukeReplay) {
        inst->markReplayFlag(LdStReplayType::NukeReplay);
    }
    if (rarReplay) {
        inst->markReplayFlag(LdStReplayType::RARReplay);
    }
    if (rawReplay) {
        inst->markReplayFlag(LdStReplayType::RAWReplay);
    }

    // More than one condition can request replay in the same S2 pass.  Keep
    // the priority centralized in DynInst::finalizeReplayTypeFromFlags() so
    // the handoff code below only sees the selected replay owner.
    if (inst->finalizeReplayTypeFromFlags()) {
        switch (*inst->getReplayType()) {
          case LdStReplayType::CacheMissReplay:
            loadSetReplay(inst, request, earlyWakeupCacheMissReplay);
            inst->setWaitingCacheRefill();
            if (earlyWakeupCacheMissReplay) {
                stats.cacheMissReplayEarly++;
            }
            DPRINTF(LoadPipeline, "Load [sn:%llu] setCacheMissReplay\n", inst->seqNum);
            return fault;
          case LdStReplayType::BankConflictReplay:
            loadSetReplay(inst, request, false);
            DPRINTF(LoadPipeline, "Load [sn:%llu] setBankConflictReplay\n", inst->seqNum);
            return fault;
          case LdStReplayType::RARReplay:
            DPRINTF(LSQUnit, "RARQueue full, reschedule [sn:%llu], LoadCompletedItIdx: %d, inst->lqItIdx: %d\n",
                    inst->seqNum, loadCompletedIdx, inst->lqIt._idx);
            stats.RARQueueFull++;
            loadSetReplay(inst, request, true);
            addToRARReplayQueue(inst);
            return fault;
          case LdStReplayType::RAWReplay:
            DPRINTF(LSQUnit, "RAWQueue full, reschedule [sn:%lli], StoreCompletedItIdx: %d, inst->sqItIdx: %d\n",
                    inst->seqNum, storeCompletedIdx, inst->sqIt.idx());
            stats.RAWQueueFull++;
            loadSetReplay(inst, request, true);
            addToRAWReplayQueue(inst);
            return fault;
          case LdStReplayType::NukeReplay:
            DPRINTF(LoadPipeline, "Load [sn:%llu] Nuke need replay\n", inst->seqNum);
            ++stats.pipeRawNukeReplay;
            return fault;
          default:
            panic("Unsupported load replay type selected in s2");
        }
    }

    // A load that completed before older loads/stores became continuous must
    // be tracked for later ordering/conflict checks.  If the tracking queues
    // were full, the replay path above already removed this load from pipe.
    if (trackRAR) {
        auto existingIt = std::find(RARQueue.begin(), RARQueue.end(), inst);
        if (existingIt == RARQueue.end()) {
            RARQueue.push_back(inst);
        }
    }

    if (trackRAW) {
        auto existingIt = std::find(RAWQueue.begin(), RAWQueue.end(), inst);
        if (existingIt == RAWQueue.end()) {
            RAWQueue.push_back(inst);
        }
    }

    // No nuke happens, prepare the inst data
    // assert(request->isNormalLd() ? !request->isAnyOutstandingRequest() : true);
    request = currentLoadRequest(inst);
    // Completion path after replay selection.  Full forwarding can write back
    // immediately; otherwise a normal load assembles cache and forwarded data
    // before completeDataAccess/writeback handling takes over.
    if (inst->fullForward()) {
        DPRINTF(LoadPipeline, "Load [sn:%llu] fullForward\n", inst->seqNum);
        assert(request);
        // forwarding
        request->forward();
        PacketPtr pkt = makeFullFwdPkt(inst, request);
        // this load gets full data from sq or sbuffer
        writebackReg(inst, pkt);
        request->writebackDone();
        delete pkt;
    } else {
        if (lsq->enableLdMissReplay() && request && request->isNormalLd()) {
            // assemble cache & sbuffer forwarded data and completeDataAcess
            DPRINTFR(LoadPipeline, "Load [sn:%llu] assemble packet\n", inst->seqNum);
            request->assemblePackets();
        }
    }

    return fault;
}

Fault
LSQUnit::loadDoWriteback(const DynInstPtr &inst)
{
    DPRINTF(LoadPipeline, "loadDoWriteback: load [sn:%lli]\n", inst->seqNum);
    return NoFault;
}

void
LSQUnit::executeLoadPipeSx()
{
    // This function is both the stage dispatcher and the pipe-exit arbiter.
    // Stage handlers mark instruction/request state; the blocks after the
    // switch decide whether the load leaves the pipe as squashed, replayed, or
    // normally finished.
    Fault fault = NoFault;
    for (int i = 0; i < loadPipeSx.size(); i++) {
        auto& stage = loadPipeSx[i];
        for (int j = 0; j < stage->size; j++) {
            auto& inst = stage->insts[j];
            if (!inst) {
                continue;
            }
            if (i > 0 && inst->skipRawCheck()) {
                inst->clearSkipRawCheck();
            }
            if (inst->isSquashed()) {
                // Terminal path 1: squashed loads leave the pipe without
                // running more stage work.
                DPRINTF(LoadPipeline, "Instruction was squashed. PC: %s, [tid:%i]"
                    " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                    inst->seqNum);
                inst->setExecuted();
                inst->setCanCommit();
                inst = nullptr;
                continue;
            }
            if (!inst->replayOrSkipFollowingPipe()) {
                // Normal path: run the current stage unless an earlier stage
                // has already converted the load into a replay or a
                // skip-rest-of-pipe completion.
                switch (i) {
                    case 0:
                        fault = loadDoTranslate(inst);
                        break;
                    case 1: {
                        fault = loadDoSendRequest(inst);
                        auto *request = currentLoadRequest(inst);
                        if (fault == NoFault &&
                            !inst->replayOrSkipFollowingPipe() &&
                            inst->readPredicate() &&
                            inst->readMemAccPredicate() &&
                            request &&
                            request->isTranslationComplete() &&
                            request->isMemAccessRequired()) {
                            iewStage->getScheduler()->specWakeUpFromLoadPipe(
                                inst);
                        }
                        // Loads will mark themselves as executed, and their
                        // writeback event adds the instruction to the queue
                        // to commit.
                        iewStage->SquashCheckAfterExe(inst);
                        break;
                    }
                    case 2:
                        fault = loadDoRecvData(inst);

                        if (inst->isDataPrefetch() || inst->isInstPrefetch()) {
                            inst->fault = NoFault;
                        }
                        break;
                    case 3:
                        fault = loadDoWriteback(inst);
                        break;
                    default:
                        panic("unsupported loadpipe length");
                }
            }

            // Terminal path 2: replay paths that are resolved before the
            // common loadWhenToReplay point.  These either already inserted
            // the load into a replay queue or must cancel the scheduler view
            // immediately.
            if (inst->needSTLFReplay() || inst->needCacheBlockedReplay() || inst->needRescheduleReplay() ||
                inst->needRAWReplay() || inst->needRARReplay()) {
                DPRINTF(LoadPipeline, "Load [sn:%llu] replayed\n", inst->seqNum);
                // record replay stats
                assert(inst->getReplayType());
                stats.loadReplayEvents[*inst->getReplayType()]++;
                // RTL only increments the main replay_* counters when a load
                // first enters slow replay from the IssueQueue.
                if (inst->getLoadPipeSource() == DynInst::LoadPipeSource::IssueQueue) {
                    stats.loadReplayEventsFromIssueQueue[*inst->getReplayType()]++;
                }

                if (inst->needCacheBlockedReplay()) {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_DcacheStall);
                }
                else if (inst->needRARReplay()) {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_RARReplay);
                }
                else if (inst->needRAWReplay()) {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_RAWReplay);
                }
                else {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_OtherReplay);
                }
                cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::LastReplay, curTick());

                iewStage->loadCancel(inst);
                inst->endPipelining();
                inst = nullptr;
                continue;
            }

            if (i == loadWhenToReplay && inst->needReplay()) [[unlikely]] {
                // Terminal path 3: common replay handoff.  Each replay type
                // has a different owner: IQ retry, cache-miss replay, MDP
                // address wait, TLB deferral, or nuke replay.
                DPRINTF(LoadPipeline, "Load [sn:%llu] replayed\n", inst->seqNum);
                // record replay stats
                assert(inst->getReplayType());
                stats.loadReplayEvents[*inst->getReplayType()]++;
                // Use the load-pipe entry source to detect that first entry
                // and match RTL, which only counts replay_* on first issue.
                if (inst->getLoadPipeSource() == DynInst::LoadPipeSource::IssueQueue) {
                    stats.loadReplayEventsFromIssueQueue[*inst->getReplayType()]++;
                }

                if (inst->needBankConflictReplay()) inst->issueQue->retryMem(inst);
                else if (inst->needMshrArbFailReplay()) inst->issueQue->retryMem(inst);
                else if (inst->needMshrAliasFailReplay()) inst->issueQue->retryMem(inst);
                else if (inst->needHitInWriteBufferReplay()) inst->issueQue->retryMem(inst);
                else if (inst->needCacheMissReplay()) iewStage->cacheMissLdReplay(inst);
                else if (inst->needMdpAddrReplay()) iewStage->mdpAddrReplayPipeDone(inst);
                else if (inst->needNukeReplay()) {
                    if (auto *request = currentLoadRequest(inst); request) {
                        if (inst->cacheHit()) {
                            loadSetReplay(inst, request, true);
                        } else if (inst->hasPendingCacheReq()) {
                            loadSetReplay(inst, request, false);
                        }
                    }
                    inst->issueQue->retryMem(inst);
                }
                else if (inst->needTLBMissReplay())
                    iewStage->deferMemInst(inst);
                else if (inst->needMptReplay()) {
                    mptWaitLoads.push_back(inst);
                    ++stats.mptLoadWaits;
                }


                if (inst->needTLBMissReplay()) {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_TLBMiss);
                }
                else if (inst->needCacheMissReplay()) {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_CacheMiss);
                }
                else if (inst->needBankConflictReplay()) {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_BankConflict);
                }
                else if (inst->needNukeReplay()) {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_Nuke);
                } else {
                    cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::ReplayStr, TT_OtherReplay);
                }
                cpu->perfCCT->updateInstMeta(inst->seqNum, InstDetail::LastReplay, curTick());

                iewStage->loadCancel(inst);
                inst->endPipelining();
                inst = nullptr;
                continue;
            }

            if (i == loadPipeStages - 1 && !inst->needReplay()) {
                // Terminal path 4: only a non-replayed load at the last pipe
                // stage can finish the IEW side and become ready to commit.
                if (inst->isExecuted() &&
                    (inst->isNormalLd() || !inst->readMemAccPredicate())) {
                    iewStage->readyToFinish(inst);
                }
                iewStage->activityThisCycle();
                inst->endPipelining();
                DPRINTF(LoadPipeline, "Load [sn:%llu] ready to finish\n",
                        inst->seqNum);
            }
        }
    }
}

Fault
LSQUnit::storeDoTranslate(const DynInstPtr &inst)
{
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);
    assert(!inst->isSquashed());

    DPRINTF(StorePipeline, "storeDoTranslate: Store [sn:%llu]\n", inst->seqNum);

    // Now initiateAcc only does TLB access
    Fault store_fault = inst->initiateAcc();

    if (inst->isTranslationDelayed() && store_fault == NoFault) {
        auto *request = currentStoreRequest(inst);
        if (request != nullptr && request->isMptDelayed()) {
            inst->setMptReplay();
            DPRINTF(StorePipeline, "Store [sn:%llu] waits for MPT\n",
                    inst->seqNum);
        } else {
            inst->setTLBMissReplay();
        }
    }

    return store_fault;
}

Fault
LSQUnit::storeDoWriteSQ(const DynInstPtr &inst)
{
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);

    ssize_t store_idx = inst->sqIdx;
    LSQRequest* request = inst->savedRequest;

    DPRINTF(StorePipeline, "storeDoWriteSQ: Store [sn:%lli]\n", inst->seqNum);

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = inst->lqIt;

    /* This is the place were instructions get the effAddr. */
    if (request && request->isTranslationComplete()) {
        panic_if(request->isMptDelayed(),
                 "Store [sn:%llu] reached SQ-write stage before MPT "
                 "permission completed\n", inst->seqNum);
        if (request->isMemAccessRequired() && (inst->getFault() == NoFault)) {

            if (cpu->checker) {
                inst->reqToVerify = std::make_shared<Request>(*request->req());
            }
            Fault fault;
            fault = write(request, inst->memData, inst->sqIdx);
            // release temporal data
            delete [] inst->memData;
            inst->memData = nullptr;

            if (fault != NoFault)
                inst->getFault() = fault;
        }
    }

    Fault store_fault = inst->getFault();

    if (!inst->readPredicate()) {
        DPRINTF(StorePipeline, "Store [sn:%lli] not executed from readPredicate\n",
                inst->seqNum);
        inst->forwardOldRegs();
        return store_fault;
    }

    inst->sqIt->setStatus(SplitStoreStatus::AddressReady);
    if (!inst->isSplitStoreAddr()) {
        inst->sqIt->setStatus(SplitStoreStatus::DataReady);
    }

    if (inst->sqIt->canForwardToLoad()) {
        stats.STDReadyFirst++;
    } else {
        stats.STAReadyFirst++;
    }

    if (storeQueue[store_idx].size() == 0) {
        DPRINTF(StorePipeline, "Store [sn:%lli] not executed from storeQueue\n",
                inst->seqNum);
        return store_fault;
    }

    assert(store_fault == NoFault);

    if (inst->isStoreConditional()) {
        // Store conditionals need to set themselves as able to
        // writeback if we haven't had a fault by here.
        storeQueue[store_idx].canWB() = true;

        ++storesToWB;
    } else {
        if (enableStorePrefetchTrain) {
            triggerStorePFTrain(store_idx);
        }
    }

    return checkViolations(loadIt, inst);
}

Fault
LSQUnit::emptyStorePipeSx(const DynInstPtr &inst, uint64_t stage)
{
    // empty store pipe stage, Does not perform any operation
    Fault fault = inst->getFault();
    assert(!inst->isSquashed());

    DPRINTF(StorePipeline, "emptyStorePipeSx: Store [sn:%lli]\n", inst->seqNum);
    return fault;
}

void
LSQUnit::executeStorePipeSx()
{
    Fault fault = NoFault;
    for (int i = 0; i < storePipeSx.size(); i++) {
        auto& stage = storePipeSx[i];
        for (int j = 0; j < stage->size; j++) {
            auto& inst = stage->insts[j];
            if (!inst) {
                continue;
            }

            if (splitStoreAddrSquashed(inst)) {
                inst->setSquashed();
            }

            if (inst->isSquashed()) {
                DPRINTF(StorePipeline, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                    " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                    inst->seqNum);
                inst->setExecuted();
                inst->setCanCommit();
                inst = nullptr;
                continue;
            }

            if (!inst->replayOrSkipFollowingPipe()) {
                switch (i) {
                    case 0:
                        fault = storeDoTranslate(inst);
                        break;
                    case 1:
                        fault = storeDoWriteSQ(inst);

                        iewStage->notifyExecuted(inst);
                        iewStage->SquashCheckAfterExe(inst);
                        break;
                    case 2:
                    case 3:
                    case 4:
                        fault = emptyStorePipeSx(inst, i);
                        break;
                    default:
                        panic("unsupported storepipe length");
                }
            }


            if (i == storeWhenToReplay && inst->needReplay()) [[unlikely]] {
                stats.storeReplayTotal++;
                if (inst->needTLBMissReplay()) {
                    iewStage->deferMemInst(inst);
                    stats.storeReplayTlbMiss++;
                } else if (inst->needMptReplay()) {
                    mptWaitStores.push_back(inst);
                    ++stats.mptStoreWaits;
                    stats.storeReplayMpt++;
                }
                inst->endPipelining();
                inst = nullptr;
                continue;
            }

            if (i == (lsq->storeWbStage() - 1)) {
                // If the store had a fault then it may not have a mem req
                if (inst->fault != NoFault || !inst->readPredicate() || !inst->isStoreConditional()) {
                    // If the instruction faulted, then we need to send it
                    // along to commit without the instruction completing.
                    // Send this instruction to commit, also make sure iew
                    // stage realizes there is activity.
                    if (!inst->needReplay()) {
                        inst->sqIt->setStatus(SplitStoreStatus::StaPipeFinish);
                        if (!inst->isSplitStoreAddr()) {
                            inst->sqIt->setStatus(SplitStoreStatus::StdPipeFinish);
                        }
                        DPRINTF(StorePipeline, "Store [sn:%llu] ready to finish\n",
                                inst->seqNum);
                        if (inst->fault != NoFault) {
                            inst->setExecuted();
                            iewStage->readyToFinish(inst);
                        } else if (inst->sqIt->splitStoreFinish()) {
                            iewStage->readyToFinish(inst);
                        }
                        iewStage->activityThisCycle();
                    }
                }
                inst->endPipelining();
            }
        }
    }
}

void
LSQUnit::executePipeSx()
{
    serviceMptWaitQueues();
    executeLoadPipeSx();
    executeStorePipeSx();
    updateCompletedIdx();
}

bool
LSQUnit::triggerStorePFTrain(int sq_idx)
{
    auto inst = storeQueue[sq_idx].instruction();
    assert(inst->translationCompleted());
    Addr vaddr = inst->effAddr;
    Addr pc = inst->pcState().instAddr();
    // create request
    RequestPtr req =
        std::make_shared<Request>(vaddr, 1, Request::STORE_PF_TRAIN, inst->requestorId(), pc, inst->contextId());
    req->setPaddr(inst->physEffAddr);

    // create packet
    PacketPtr pkt = Packet::createPFtrain(req);

    // send packet
    bool success = dcachePort->sendTimingReq(pkt);
    assert(success); // must be true

    return true;
}

Fault
LSQUnit::executeAmo(const DynInstPtr &amo_inst)
{
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);
    assert(!amo_inst->staticInst->isSplitStoreAddr());

    ssize_t amo_idx = amo_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing AMO PC %s [sn:%lli]\n",
            amo_inst->pcState(), amo_inst->seqNum);

    assert(!amo_inst->isSquashed());

    // Check the recently completed loads to see if any match this amo's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = amo_inst->lqIt;

    Fault amo_fault = amo_inst->initiateAcc();

    if (amo_inst->isTranslationDelayed() && amo_fault == NoFault)
        return amo_fault;

    if (!amo_inst->readPredicate()) {
        DPRINTF(LSQUnit, "AMO [sn:%lli] not executed from predication\n",
                amo_inst->seqNum);
        amo_inst->forwardOldRegs();
        return amo_fault;
    }

    if (storeQueue[amo_idx].size() == 0) {
        DPRINTF(LSQUnit,"Fault on AMO PC %s, [sn:%lli], Size = 0\n",
                amo_inst->pcState(), amo_inst->seqNum);

        // If the amo instruction faulted, then we need to send it along
        // to commit without the instruction completing.
        if (!(amo_inst->hasRequest() && amo_inst->strictlyOrdered()) ||
            amo_inst->isAtCommit()) {
            amo_inst->setExecuted();
        }
        iewStage->readyToFinish(amo_inst);
        iewStage->activityThisCycle();

        return amo_fault;
    }

    assert(amo_fault == NoFault);

    // Atomics need to set themselves as able to writeback if we haven't had a fault by here.
    storeQueue[amo_idx].canWB() = true;
    ++storesToWB;

    return checkViolations(loadIt, amo_inst);
}

void
LSQUnit::commitLoad()
{
    assert(loadQueue.front().valid());

    DynInstPtr inst = loadQueue.front().instruction();

    DPRINTF(LSQUnit, "Committing head load instruction, PC %s, [sn:%lu]\n",
            inst->pcState(), inst->seqNum);

    // Update histogram with memory latency from load
    // Only take latency from load demand that where issued and did not fault
    if (!inst->isInstPrefetch() && !inst->isDataPrefetch()) {
        uint64_t translation_lat = 0;
        if (inst->firstIssue != -1 && inst->translatedTick != -1) {
            translation_lat =
                cpu->ticksToCycles(inst->translatedTick - inst->firstIssue);
            stats.loadTranslationLat.sample(translation_lat);
        }
        if (inst->firstIssue != -1 && inst->lastWakeDependents != -1) {
            auto load_to_use = cpu->ticksToCycles(
                inst->lastWakeDependents - inst->firstIssue);
            stats.loadToUse.sample(load_to_use);
            if (((uint64_t) load_to_use) > 2000) {
                DPRINTF(CommitTrace,
                        "Inst[sn:%lu] load2use = %lu, translation lat = %lu\n",
                        inst->seqNum, load_to_use, translation_lat);
            }
        }
    }

    loadQueue.front().clear();
    loadQueue.pop_front();
    lastClockLQPopEntries++;
}

void
LSQUnit::commitLoads(InstSeqNum &youngest_inst)
{
    assert(loadQueue.size() == 0 || loadQueue.front().valid());

    while (loadQueue.size() != 0 && loadQueue.front().instruction()->seqNum
            <= youngest_inst) {
        commitLoad();
    }
}

void
LSQUnit::commitStores(InstSeqNum &youngest_inst)
{
    assert(storeQueue.size() == 0 || storeQueue.front().valid());

    /* Forward iterate the store queue (age order). */
    for (auto& x : storeQueue) {
        assert(x.valid());
        // Mark any stores that are now committed and have not yet
        // been marked as able to write back.
        if (!x.canWB()) {
            if (x.instruction()->seqNum > youngest_inst) {
                break;
            }
            // Commit can publish a new squash to IEW one cycle after IEW has
            // already received an older doneMemSeqNum. If that stale
            // doneMemSeqNum reaches here in the same cycle that ROB marks this
            // store squashed, do not advance SQ writeback state past the
            // squashed entry; IEW's next-cycle squash will remove it.
            if (x.instruction()->isSquashed()) {
                break;
            }
            if (x.instruction()->isSplitStoreAddr() && !x.splitStoreFinish()) {
                panic("Split store reached commitStores unfinished: tid=%d "
                      "seq=%llu pc=%#lx youngest=%llu canCommit=%d "
                      "executed=%d squashed=%d addrReady=%d dataReady=%d "
                      "staFinish=%d stdFinish=%d canWB=%d completed=%d\n",
                      x.instruction()->threadNumber,
                      static_cast<unsigned long long>(
                          x.instruction()->seqNum),
                      x.instruction()->pcState().instAddr(),
                      static_cast<unsigned long long>(youngest_inst),
                      x.instruction()->readyToCommit(),
                      x.instruction()->isExecuted(),
                      x.instruction()->isSquashed(),
                      x.addrReady(), x.dataReady(),
                      x.staFinish(), x.stdFinish(),
                      x.canWB(), x.completed());
            }
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    x.instruction()->pcState(),
                    x.instruction()->seqNum);

            x.canWB() = true;

            ++storesToWB;
        }
    }
}

bool
LSQUnit::hasStoresToWBBefore(InstSeqNum seq_num) const
{
    if (storesToWB == 0) {
        return false;
    }

    for (auto it = storeQueue.begin(); it != storeQueue.end(); ++it) {
        if (!it->valid() || !it->instruction()) {
            continue;
        }

        const auto &inst = it->instruction();
        if (inst->seqNum >= seq_num) {
            break;
        }

        if (it->canWB() && !it->completed()) {
            return true;
        }
    }

    return false;
}

bool
LSQUnit::writebackBlockedStore()
{
    if (!isStoreBlocked) {
        return false;
    }

    auto *request = storeWBIt->request();
    const auto &inst = storeWBIt->instruction();

    if (request->mainReq()->hasPaddr() &&
        system->multiContextDifftest() && inst->isAtomic() &&
        cpu->goldenMemManager() &&
        cpu->goldenMemManager()->inPmem(request->mainReq()->getPaddr())) {
        uint8_t issue_golden[8] = {};
        panic_if(request->_size > sizeof(issue_golden),
                 "Unexpected AMO size %u at addr %#lx\n",
                 request->_size, request->mainReq()->getPaddr());
        cpu->goldenMemManager()->readGoldenMem(
            request->mainReq()->getPaddr(), issue_golden, request->_size);
        std::memcpy(inst->getAmoOldGoldenValuePtr(), issue_golden,
                    request->_size);
    }

    request->sendPacketToCache();
    if (request->isSent()) {
        storePostSend();
    }
    return isStoreBlocked;
}

bool
LSQUnit::directStoreToCache()
{
    DynInstPtr inst = storeWBIt->instruction();
    LSQRequest* request = storeWBIt->request();

    if ((request->mainReq()->isLLSC() || request->mainReq()->isRelease()) && (storeWBIt.idx() != storeQueue.head())) {
        DPRINTF(LSQUnit,
                "Store idx:%i PC:%s to Addr:%#x "
                "[sn:%lli] is %s%s and not head of the queue\n",
                storeWBIt.idx(), inst->pcState(), request->mainReq()->getPaddr(), inst->seqNum,
                request->mainReq()->isLLSC() ? "SC" : "", request->mainReq()->isRelease() ? "/Release" : "");
        return false;
    }

    assert(!inst->memData);
    inst->memData = new uint8_t[request->_size];

    if (storeWBIt->isAllZeros()) {
        memset(inst->memData, 0, request->_size);
    } else {
        memcpy(inst->memData, storeWBIt->data(), request->_size);
    }

    request->buildPackets();

    bool sc_success = false;

    if (inst->isStoreConditional()) {
        inst->recordResult(false);
        sc_success = inst->tcBase()->getIsaPtr()->handleLockedWrite(inst.get(), request->mainReq(), cacheBlockMask);
        inst->recordResult(true);
        request->packetSent();

        inst->lockedWriteSuccess(sc_success);

        if (!sc_success) {
            request->complete();
            DPRINTF(LSQUnit,
                    "Store conditional [sn:%lli] failed.  "
                    "Instantly completing it.\n",
                    inst->seqNum);
            PacketPtr new_pkt = new Packet(*request->packet());
            WritebackRegEvent *wb = new WritebackRegEvent(inst, new_pkt, this);
            cpu->schedule(wb, curTick() + 1);
            completeStore(storeWBIt);
            if (!storeQueue.empty())
                storeWBIt++;
            else
                storeWBIt = storeQueue.end();
            return true;
        }
    }

    if (request->mainReq()->hasPaddr()) {
        if (system->multiContextDifftest() && inst->isAtomic() &&
            cpu->goldenMemManager() &&
            cpu->goldenMemManager()->inPmem(request->mainReq()->getPaddr())) {
            uint8_t issue_golden[8] = {};
            panic_if(request->_size > sizeof(issue_golden),
                     "Unexpected AMO size %u at addr %#lx\n",
                     request->_size, request->mainReq()->getPaddr());
            cpu->goldenMemManager()->readGoldenMem(
                request->mainReq()->getPaddr(), issue_golden, request->_size);
            std::memcpy(inst->getAmoOldGoldenValuePtr(), issue_golden,
                        request->_size);
        }
    }

    if (request->mainReq()->isLocalAccess()) {
        assert(!inst->isStoreConditional());
        assert(!inst->inHtmTransactionalState());
        gem5::ThreadContext *thread = cpu->tcBase(lsqID);
        PacketPtr main_pkt = new Packet(request->mainReq(), MemCmd::WriteReq);
        main_pkt->dataStatic(inst->memData);
        request->mainReq()->localAccessor(thread, main_pkt);
        delete main_pkt;
        completeStore(storeWBIt);
        storeWBIt++;
        return true;
    }

    request->sendPacketToCache();

    if (request->isSent()) {
        storePostSend();
    } else {
        DPRINTF(LSQUnit, "D-Cache became blocked when writing [sn:%lli], "
                            "will retry later\n",
                            inst->seqNum);
    }

    return true;
}

uint32_t
LSQUnit::countStoreBufferOffloadableEntries(uint32_t max_entries) const
{
    if (max_entries == 0 || isStoreBlocked) {
        return 0;
    }

    uint32_t count = 0;
    int stores_to_wb = storesToWB;
    auto store_wb_it = storeWBIt;

    while (count < max_entries &&
           stores_to_wb > 0 &&
           store_wb_it.dereferenceable() &&
           store_wb_it->valid() &&
           store_wb_it->canWB()) {

        if (store_wb_it->size() == 0 ||
            store_wb_it->instruction()->isDataPrefetch()) {
            ++store_wb_it;
            continue;
        }

        ++count;
        ++store_wb_it;
    }

    return count;
}

void
LSQUnit::offloadToStoreBuffer(uint32_t max_entries, std::vector<bool>& offload_fail)
{
    assert(!lsq->storeBufferBlocked());
    if (isStoreBlocked) return;

    // Zero-sized stores do not consume store-buffer quota, but they still need
    // completeStore() to free SQ entries.
    uint32_t accepted_entries = 0;
    while (storesToWB > 0 &&
           storeWBIt.dereferenceable() &&
           storeWBIt->valid() &&
           storeWBIt->canWB()) {

        if (storeWBIt->size() == 0) {
            completeStore(storeWBIt);
            storeWBIt++;
            continue;
        }
        if (storeWBIt->instruction()->isDataPrefetch()) {
            storeWBIt++;
            continue;
        }

        if (accepted_entries >= max_entries) {
            break;
        }

        assert(!storeWBIt->committed());
        DynInstPtr inst = storeWBIt->instruction();
        LSQRequest *request = storeWBIt->request();

        if (request->mainReq()->isLLSC() ||
            request->mainReq()->isAtomic() ||
            request->mainReq()->isRelease() ||
            request->mainReq()->isStrictlyOrdered() ||
            inst->isStoreConditional()) {
            if (!(storeWBIt.idx() == storeQueue.head())) {
                break;
            }
            if (!storeBufferEmpty(lsqID)) {
                lsq->flushStores(lsqID);
                break;
            }
            bool contin = directStoreToCache();
            if (isStoreBlocked) {
                break;
            }
            if (contin) {
                continue;
            } else {
                break;
            }
        }
        assert(!request->mainReq()->isLocalAccess());

        if (request->isSplit()) {
            Addr vbase = request->_addr;
            for (int i = request->_numOutstandingPackets; i < request->_reqs.size(); i++) {
                auto req = request->_reqs[i];
                Addr vaddr = req->getVaddr();
                Addr paddr = req->getPaddr();
                uint64_t offset = vaddr - vbase;
                DPRINTF(LSQUnit, "Spilt store idx %d [sn:%lli] insert into sbuffer\n", i, inst->seqNum);
                assert(offset + req->getSize() <= storeWBIt->size());
                bool success = insertStoreBuffer(
                    vaddr, paddr, (uint8_t *)storeWBIt->data() + offset,
                    req->getSize(), req->getByteEnable(), inst->seqNum);
                if (success) {
                    request->_numOutstandingPackets++;
                } else {
                    offload_fail[lsqID] = true;
                    break;
                }
            }
            if (request->_numOutstandingPackets == request->_reqs.size()) {
                ++accepted_entries;
                request->_numOutstandingPackets = 0;
                completeStore(storeWBIt, true);
                storeWBIt++;
            } else {
                break;
            }
        } else {
            assert(inst->isSplitStoreAddr() ? storeWBIt->splitStoreFinish() : true);
            Addr vaddr = request->getVaddr();
            Addr paddr = request->mainReq()->getPaddr();
            DPRINTF(LSQUnit, "Store [sn:%lli] insert into sbuffer\n", inst->seqNum);
            bool success = insertStoreBuffer(
                vaddr, paddr, (uint8_t *)storeWBIt->data(), request->_size,
                request->mainReq()->getByteEnable(), inst->seqNum);
            if (!success) {
                offload_fail[lsqID] = true;
                break;
            }
            ++accepted_entries;
            // finish once store
            completeStore(storeWBIt, true);
            storeWBIt++;
        }
    }
}

bool
LSQUnit::insertStoreBuffer(Addr vaddr, Addr paddr, uint8_t* datas,
                           uint64_t size, const std::vector<bool>& mask,
                           InstSeqNum store_seq)
{
    auto &storeBuffer = lsq->getStoreBuffer();
    // access range must in a cache block
    assert((vaddr & cacheBlockMask) == ((vaddr + size - 1) & cacheBlockMask));
    Addr blockVaddr = vaddr & cacheBlockMask;
    Addr blockPaddr = paddr & cacheBlockMask;
    Addr offset = paddr & ~cacheBlockMask;

    // check request is not already in the storebuffer
    auto entry = storeBuffer.get(lsqID, blockPaddr);

    if (entry) {
        if (entry->evictionInProgress()) {
            if (entry->vice) {
                // merge into vice
                stats.sbufferMerge++;
                entry = entry->vice;
                entry->merge(offset, datas, size, mask);
                DPRINTF(StoreBuffer, "Merging vice entry[%#x] for addr %#x\n",
                        blockPaddr, paddr);
            } else {
                // create vice for sending entry
                if (storeBuffer.full(lsqID) || storeBuffer.full()) {
                    DPRINTF(StoreBuffer, "[tid:%u] Insert %#x failed due to sbuffer full\n", lsqID, paddr);
                    stats.sbufferFull++;
                    return false;
                }
                stats.sbufferNewline++;
                stats.sbufferCreateVice++;
                auto vice = storeBuffer.createVice(entry);
                vice->reset(lsqID, store_seq, blockVaddr, blockPaddr, offset,
                            datas, size, mask);
                DPRINTF(StoreBuffer, "Create new vice entry[%#x] for addr %#x\n",
                        blockPaddr, paddr);
            }
        } else {
            // merge into unsent
            stats.sbufferMerge++;
            storeBuffer.update(entry->index);
            entry->merge(offset, datas, size, mask);
            entry->seqNum = std::max(entry->seqNum, store_seq);
            DPRINTF(StoreBuffer, "Merging entry[%#x] for addr %#x\n",
                    blockPaddr, paddr);
        }
    } else {
        // create new entry
        if (storeBuffer.full(lsqID) || storeBuffer.full()) {
            stats.sbufferFull++;
            // lsq->nextStoreBufferInsertTid = lsqID;
            DPRINTF(StoreBuffer, "[tid:%u] Insert %#x failed due to sbuffer full\n", lsqID, paddr);
            return false;
        }
        // insert
        stats.sbufferNewline++;
        auto entry = storeBuffer.getEmpty();
        entry->reset(lsqID, store_seq, blockVaddr, blockPaddr, offset, datas,
                     size, mask);
        storeBuffer.insert(entry);
        DPRINTF(StoreBuffer, "Create new entry[%#x] for addr %#x\n",
                blockPaddr, paddr);
    }
    DPRINTF(
        StoreBuffer,
        "insert %#x to entry[%#x] successed, sbuffer size: %d unsentsize: %d\n",
        paddr, blockPaddr, storeBuffer.size(), storeBuffer.unsentSize());
    return true;
}

void
LSQUnit::squash(const InstSeqNum &squashed_num)
{
    DPRINTF(LSQUnit, "Squashing until [sn:%lli]!"
            "(Loads:%i Stores:%i)\n", squashed_num, loadQueue.size(),
            storeQueue.size());

    squashMark = true;

    while (loadQueue.size() != 0 &&
            loadQueue.back().instruction()->seqNum > squashed_num) {
        DPRINTF(LSQUnit,"Load Instruction PC %s squashed, "
                "[sn:%lli]\n",
                loadQueue.back().instruction()->pcState(),
                loadQueue.back().instruction()->seqNum);

        if (isStalled() && loadQueue.tail() == stallingLoadIdx) {
            stalled = false;
            stallingStoreIsn = 0;
            stallingLoadIdx = 0;
        }

        // hardware transactional memory
        // Squashing instructions can alter the transaction nesting depth
        // and must be corrected before fetching resumes.
        if (loadQueue.back().instruction()->isHtmStart())
        {
            htmStarts = (--htmStarts < 0) ? 0 : htmStarts;
            DPRINTF(HtmCpu, ">> htmStarts-- (%d) : htmStops (%d)\n",
              htmStarts, htmStops);
        }
        if (loadQueue.back().instruction()->isHtmStop())
        {
            htmStops = (--htmStops < 0) ? 0 : htmStops;
            DPRINTF(HtmCpu, ">> htmStarts (%d) : htmStops-- (%d)\n",
              htmStarts, htmStops);
        }

        // Clear the smart pointer to make sure it is decremented.
        loadQueue.back().instruction()->setSquashed();
        loadQueue.back().clear();

        loadQueue.pop_back();
        lastClockLQPopEntries++;
        ++stats.squashedLoads;
    }

    // Squash only removes entries with seqNum > squashed_num. completedIdx
    // is rewound only when it points past squashed_num or is invalid.
    auto rewindCompletedIdx = [&](auto &queue, size_t &completed_idx) {
        if (queue.empty()) {
            completed_idx = queue.head() - 1;
            return;
        }

        if (!queue.isValidIdx(completed_idx)) {
            completed_idx = queue.head() - 1;
            return;
        }

        auto cur_it = queue.getIterator(completed_idx);
        if (cur_it->valid() && cur_it->instruction() &&
            cur_it->instruction()->seqNum <= squashed_num) {
            return;
        }

        for (auto it = queue.end(); it != queue.begin();) {
            --it;
            if (it->valid() && it->instruction() &&
                it->instruction()->seqNum <= squashed_num) {
                completed_idx = it.idx();
                return;
            }
        }
        completed_idx = queue.head() - 1;
    };

    rewindCompletedIdx(loadQueue, loadCompletedIdx);

    for (auto it = inflightLoads.begin(); it != inflightLoads.end();) {
        if ((*it)->instruction()->isSquashed()) {
            it = inflightLoads.erase(it);
        } else {
            ++it;
        }
    }

    // hardware transactional memory
    // scan load queue (from oldest to youngest) for most recent valid htmUid
    auto scan_it = loadQueue.begin();
    uint64_t in_flight_uid = 0;
    while (scan_it != loadQueue.end()) {
        if (scan_it->instruction()->isHtmStart() &&
            !scan_it->instruction()->isSquashed()) {
            in_flight_uid = scan_it->instruction()->getHtmTransactionUid();
            DPRINTF(HtmCpu, "loadQueue[%d]: found valid HtmStart htmUid=%u\n",
                scan_it._idx, in_flight_uid);
        }
        scan_it++;
    }
    // If there's a HtmStart in the pipeline then use its htmUid,
    // otherwise use the most recently committed uid
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    if (htm_cpt) {
        const uint64_t old_local_htm_uid = htm_cpt->getHtmUid();
        uint64_t new_local_htm_uid;
        if (in_flight_uid > 0)
            new_local_htm_uid = in_flight_uid;
        else
            new_local_htm_uid = lastRetiredHtmUid;

        if (old_local_htm_uid != new_local_htm_uid) {
            DPRINTF(HtmCpu, "flush: lastRetiredHtmUid=%u\n",
                lastRetiredHtmUid);
            DPRINTF(HtmCpu, "flush: resetting localHtmUid=%u\n",
                new_local_htm_uid);

            htm_cpt->setHtmUid(new_local_htm_uid);
        }
    }

    if (memDepViolator && squashed_num < memDepViolator->seqNum) {
        memDepViolator = NULL;
    }

    while (storeQueue.size() != 0 &&
           storeQueue.back().instruction()->seqNum > squashed_num) {
        // Instructions marked as can WB are already committed.
        if (storeQueue.back().canWB()) {
            break;
        }

        DPRINTF(LSQUnit,"Store Instruction PC %s squashed, "
                "idx:%i [sn:%lli]\n",
                storeQueue.back().instruction()->pcState(),
                storeQueue.tail(), storeQueue.back().instruction()->seqNum);

        // I don't think this can happen.  It should have been cleared
        // by the stalling load.
        if (isStalled() &&
            storeQueue.back().instruction()->seqNum == stallingStoreIsn) {
            panic("Is stalled should have been cleared by stalling load!\n");
            stalled = false;
            stallingStoreIsn = 0;
        }

        // Clear the smart pointer to make sure it is decremented.
        storeQueue.back().instruction()->setSquashed();

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        storeQueue.back().clear();

        storeQueue.pop_back();
        lastClockSQPopEntries++;
        ++stats.squashedStores;
    }

    auto prevStoreCompletedIdx = storeCompletedIdx;
    // Store side follows the same rewind rule.
    rewindCompletedIdx(storeQueue, storeCompletedIdx);

    if (lsq->enableReplayBasedMDP() && storeCompletedIdx != prevStoreCompletedIdx) {
        iewStage->mdpAddrReplayUpdateStoreCompletedIdx(lsqID, storeCompletedIdx);
    }

    auto RARIt = RARQueue.begin();
    while (RARIt != RARQueue.end()) {
        if ((*RARIt)->seqNum > squashed_num) {
            RARIt = RARQueue.erase(RARIt);
        } else {
            ++RARIt;
        }
    }

    // Clean up replay queues - remove squashed instructions
    while (!RARReplayQueue.empty()) {
        auto inst = RARReplayQueue.front();
        if (inst->seqNum > squashed_num) {
            DPRINTF(LSQUnit, "Removing squashed inst [sn:%llu] from RARReplayQueue\n",
                    inst->seqNum);
            RARReplayQueue.pop_front();
        } else {
            break;
        }
    }

    auto RAWIt = RAWQueue.begin();
    while (RAWIt != RAWQueue.end()) {
        if ((*RAWIt)->seqNum > squashed_num) {
            RAWIt = RAWQueue.erase(RAWIt);
        } else {
            ++RAWIt;
        }
    }

    while (!RAWReplayQueue.empty()) {
        auto inst = RAWReplayQueue.front();
        if (inst->seqNum > squashed_num) {
            DPRINTF(LSQUnit, "Removing squashed inst [sn:%llu] from RAWReplayQueue\n",
                    inst->seqNum);
            RAWReplayQueue.pop_front();
        } else {
            break;
        }
    }

    auto removeSquashedMptWaits = [squashed_num](auto &queue) {
        queue.erase(
            std::remove_if(queue.begin(), queue.end(),
                           [squashed_num](const DynInstPtr &inst) {
                               return !inst || inst->isSquashed() ||
                                      inst->seqNum > squashed_num;
                           }),
            queue.end());
    };
    removeSquashedMptWaits(mptWaitLoads);
    removeSquashedMptWaits(mptWaitStores);

}

uint64_t
LSQUnit::getLatestHtmUid() const
{
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    return htm_cpt->getHtmUid();
}

void
LSQUnit::storePostSend()
{
    if (isStalled() &&
        storeWBIt->instruction()->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    if (!storeWBIt->instruction()->isStoreConditional()) {
        // The store is basically completed at this time. This
        // only works so long as the checker doesn't try to
        // verify the value in memory for stores.
        storeWBIt->instruction()->setCompleted();

        if (cpu->checker) {
            cpu->checker->verify(storeWBIt->instruction());
        }
    }

    if (needsTSO) {
        storeInFlight = true;
    }

    storeWBIt++;
}

void
LSQUnit::writebackReg(const DynInstPtr &inst, PacketPtr pkt)
{
    assert(!inst->isSplitStoreAddr());
    iewStage->wakeCPU();
    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert (!inst->isStore() || inst->isStoreConditional());
        ++stats.ignoredResponses;
        return;
    }

    if (debug::LoadPipeline) {
        uint64_t first_word = 0;
        if (inst->memData) {
            auto copy_size = inst->effSize;
            if (copy_size > sizeof(first_word)) {
                copy_size = sizeof(first_word);
            }
            std::memcpy(&first_word, inst->memData, copy_size);
        }
        DPRINTF(LoadPipeline, "WritebackReg: %s [sn:%lli] data: %#lx\n", enums::OpClassStrings[inst->opClass()],
                inst->seqNum, first_word);
    }


    if (!inst->isExecuted()) {
        inst->setExecuted();

        if (inst->fault == NoFault) {
            // Complete access to copy data to proper place.
            inst->completeAcc(pkt);
        } else {
            // If the instruction has an outstanding fault, we cannot complete
            // the access as this discards the current fault.

            // If we have an outstanding fault, the fault should only be of
            // type ReExec or - in case of a SplitRequest - a partial
            // translation fault

            // Unless it's a hardware transactional memory fault
            auto htm_fault = std::dynamic_pointer_cast<
                GenericHtmFailureFault>(inst->fault);

            if (!htm_fault) {
                assert(dynamic_cast<ReExec*>(inst->fault.get()) != nullptr ||
                       (currentLoadRequest(inst) &&
                        currentLoadRequest(inst)->isPartialFault()));

            } else if (!pkt->htmTransactionFailedInCache()) {
                // Situation in which the instruction has a hardware
                // transactional memory fault but not the packet itself. This
                // can occur with ldp_uop microops since access is spread over
                // multiple packets.
                DPRINTF(HtmCpu,
                        "%s writeback with HTM failure fault, "
                        "however, completing packet is not aware of "
                        "transaction failure. cause=%s htmUid=%u\n",
                        inst->staticInst->getName(),
                        htmFailureToStr(htm_fault->getHtmFailureFaultCause()),
                        htm_fault->getHtmUid());
            }

            DPRINTF(LSQUnit, "Not completing instruction [sn:%lli] access "
                    "due to pending fault.\n", inst->seqNum);
        }
    }

    const bool finish_after_writeback =
        !inst->isNormalLd() || !inst->inPipe();
    if (finish_after_writeback) {
        // Normal loads usually wait for the last pipe stage to enqueue commit.
        // If the response arrives after the load has already drained from the
        // pipe, writeback must finish the instruction here.
        iewStage->readyToFinish(inst);
        iewStage->activityThisCycle();
    }
    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

void
LSQUnit::completeStore(typename StoreQueue::iterator store_idx, bool from_sbuffer)
{
    assert(store_idx->valid());
    store_idx->completed() = true;
    --storesToWB;
    // A bit conservative because a store completion may not free up entries,
    // but hopefully avoids two store completions in one cycle from making
    // the CPU tick twice.
    cpu->wakeCPU();
    cpu->activityThisCycle();

    /* We 'need' a copy here because we may clear the entry from the
     * store queue. */
    DynInstPtr store_inst = store_idx->instruction();
    auto request = store_idx->request();
    // Predicated-off or zero-sized stores can legitimately reach completion
    // without ever materializing a backing memory request.
    const bool has_main_request =
        request && request->numReqs() > 0;
    const bool has_paddr =
        has_main_request && request->mainReq()->hasPaddr();
    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

    if (!from_sbuffer &&
        (!store_inst->isStoreConditional() || store_inst->lockedWriteSuccess()) &&
        has_paddr) {
        if (!store_inst->isAtomic()) {
            cpu->consumeSyncVisibleStoreReplay(lsqID);
        }
        lsq->notifyOtherThreadsStoreVisible(lsqID,
            request->mainReq()->getPaddr(),
            request->mainReq()->getByteEnable());
    }

    if (!from_sbuffer &&
        (!store_inst->isStoreConditional() || store_inst->lockedWriteSuccess()) &&
        cpu->goldenMemManager() &&
        has_paddr &&
        cpu->goldenMemManager()->inPmem(request->mainReq()->getPaddr())) {
        Addr paddr = request->mainReq()->getPaddr();
        if (!store_inst->isAtomic()) {
            DPRINTF(LSQUnit, "Store writing to golden memory at addr %#x, data %#lx, mask %#x, size %d\n",
                    paddr, *((uint64_t *)store_inst->memData), 0xff, request->_size);
            cpu->goldenMemManager()->updateGoldenMem(paddr, store_inst->memData, 0xff,
                                                     request->_size);
        } else {
            uint8_t tmp_data[8];
            memset(tmp_data, 0, sizeof(tmp_data));
            assert(request->req()->getAtomicOpFunctor());

            // The AMO response returns the old memory value. Capture it on the
            // instruction so commit/difftest can use a per-inst golden copy
            // under SMT, but derive the new memory image from the DUT-observed
            // old value captured in goldenData.
            memcpy(tmp_data, store_inst->getGolden(), request->_size);

            (*(request->req()->getAtomicOpFunctor()))(tmp_data);

            DPRINTF(LSQUnit, "AMO writing to golden memory at addr %#x, data %#lx, mask %#x, size %d\n",
                    paddr, *((uint64_t *)(tmp_data)), 0xff, request->_size);
            cpu->goldenMemManager()->updateGoldenMem(paddr, tmp_data, 0xff,
                                                     request->_size);
        }
    }

    if (store_idx == storeQueue.begin()) {
        do {
            storeQueue.front().clear();
            storeQueue.pop_front();
            lastClockSQPopEntries++;
        } while (storeQueue.front().completed() &&
                 !storeQueue.empty());

        iewStage->updateLSQNextCycle = true;
    }

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

#if TRACING_ON
    if (debug::O3PipeView) {
        store_inst->storeTick =
            curTick() - store_inst->fetchTick;
    }
#endif

    if (isStalled() &&
        store_inst->seqNum == stallingStoreIsn) {
        DPRINTF(LSQUnit, "Unstalling, stalling store [sn:%lli] "
                "load idx:%li\n",
                stallingStoreIsn, stallingLoadIdx);
        stalled = false;
        stallingStoreIsn = 0;
        iewStage->replayMemInst(loadQueue[stallingLoadIdx].instruction());
    }

    store_inst->setCompleted();

    if (needsTSO) {
        storeInFlight = false;
    }

    // Tell the checker we've completed this instruction.  Some stores
    // may get reported twice to the checker, but the checker can
    // handle that case.
    // Store conditionals cannot be sent to the checker yet, they have
    // to update the misc registers first which should take place
    // when they commit
    if (cpu->checker &&  !store_inst->isStoreConditional()) {
        cpu->checker->verify(store_inst);
    }
}

bool
LSQUnit::trySendPacket(bool isLoad, PacketPtr data_pkt, bool &bank_conflict, bool &tag_read_fail,
                        bool &mshr_used, bool &mshr_alias_fail, bool &hit_in_write_buffer)
{
    bool ret = true;
    bool cache_got_blocked = false;
    LSQRequest *request = dynamic_cast<LSQRequest *>(data_pkt->senderState);
    if (isLoad) {
        bank_conflict = lsq->loadBankConflictedCheck(data_pkt->req->getVaddr());
    }
    // Record the tick count at the time of sending to let
    // the subsequent cache understand the request's sending time.
    data_pkt->sendTick = curTick();
    PacketPtr pkt = data_pkt;

    auto inst = dynamic_cast<LSQRequest *>(data_pkt->senderState)->instruction();

    DPRINTF(LSQUnit, "Attempting to send packet for inst [sn:%llu], addr: %#x\n",
            inst->seqNum, data_pkt->getAddr());

    if (isLoad) {
        const int fwd_base =
            pkt->req->getVaddr() - request->mainReq()->getVaddr();
        const int fwd_end = fwd_base + pkt->req->getSize();
        assert(fwd_base >= 0);
        for (auto it = request->SBforwardPackets.begin();
             it != request->SBforwardPackets.end();) {
            if (it->idx >= fwd_base && it->idx < fwd_end) {
                it = request->SBforwardPackets.erase(it);
            } else {
                ++it;
            }
        }

        const Addr block_addr = pkt->getAddr() & cacheBlockMask;
        auto entry = lsq->findForwardingStoreBufferEntry(
            block_addr, lsqID, request->instruction()->seqNum);
        if (entry) {
            DPRINTF(StoreBuffer, "sbuffer entry[%#x] coverage %s\n",
                    entry->blockPaddr, pkt->print());
            if (entry->recordForward(
                    pkt->req, request, lsqID,
                    request->instruction()->seqNum)) {
                assert(request->isSplit()); // here must be split request
                stats.sbufferFullForward++;
            } else if (!request->SBforwardPackets.empty()) {
                stats.sbufferPartiForward++;
            }
        }
    }

    if (!lsq->cacheBlocked() && lsq->cachePortAvailable(isLoad)) {
        if (bank_conflict) {
            ++stats.bankConflictTimes;
            if (!isLoad) {
                assert(request == storeWBIt->request());
                isStoreBlocked = true;
            }
            bank_conflict = true;
            ret = false;
        }
        if (!bank_conflict && !dcachePort->sendTimingReq(data_pkt)) {
            ret = false;
            mshr_used = data_pkt->mshrArbFailed();
            mshr_alias_fail = data_pkt->mshrAliasFailed();
            hit_in_write_buffer = data_pkt->isHitInWriteBuffer();
            tag_read_fail = data_pkt->tagReadFail;

            if (!tag_read_fail && !mshr_used && !mshr_alias_fail && !hit_in_write_buffer) {
                cache_got_blocked = true;
            }
        }
    }
    else {
        ret = false;
    }

    if (ret) {
        if (!isLoad) {
            isStoreBlocked = false;
        }
        lsq->cachePortBusy(isLoad);
        request->packetSent();
    } else {
        if (cache_got_blocked) {
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        if (!isLoad) {
            assert(request == storeWBIt->request());
            isStoreBlocked = true;
        }
        request->packetNotSent();
    }
    DPRINTF(LSQUnit,
            "Memory request (pkt: %s) from inst [sn:%llu] was"
            " %ssent (cache is blocked: %d, cache_got_blocked: %d, bank conflict: %d, tag_read_fail: %d,"
            " mshr_used: %d, mshr_alias_fail: %d, hit_in_write_buffer: %d)\n",
            data_pkt->print(), request->instruction()->seqNum, ret ? "" : "not ", lsq->cacheBlocked(),
            cache_got_blocked, bank_conflict, tag_read_fail, mshr_used, mshr_alias_fail, hit_in_write_buffer);
    return ret;
}

void
LSQUnit::startStaleTranslationFlush()
{
    DPRINTF(LSQUnit, "Unit %p marking stale translations %d %d\n", this,
        storeQueue.size(), loadQueue.size());
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest())
            entry.request()->markAsStaleTranslation();
    }
}

bool
LSQUnit::checkStaleTranslations() const
{
    DPRINTF(LSQUnit, "Unit %p checking stale translations\n", this);
    for (auto& entry : storeQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    for (auto& entry : loadQueue) {
        if (entry.valid() && entry.hasRequest()
            && entry.request()->hasStaleTranslation())
            return true;
    }
    DPRINTF(LSQUnit, "Unit %p found no stale translations\n", this);
    return false;
}

void
LSQUnit::updateCompletedIdx()
{
    auto prevStoreCompletedIdx = storeCompletedIdx;
    // Keep completed indices within [head-1, tail]; head-1 means no completed entry.
    if (loadQueue.empty()) {
        loadCompletedIdx = loadQueue.head() - 1;
    } else if (loadCompletedIdx < loadQueue.head() - 1 ||
               loadCompletedIdx > loadQueue.tail()) {
        loadCompletedIdx = loadQueue.head() - 1;
    }
    if (storeQueue.empty()) {
        storeCompletedIdx = storeQueue.head() - 1;
    } else if (storeCompletedIdx < storeQueue.head() - 1 ||
               storeCompletedIdx > storeQueue.tail()) {
        storeCompletedIdx = storeQueue.head() - 1;
    }

    // Advance load completed index (controls RAR queue dequeue rate)
    if (!loadQueue.empty()) {
        for (unsigned i = 0; i < loadCompletionWidth; i++) {
            const int currentIdx = loadCompletedIdx;
            auto loadIt = loadQueue.getIterator(loadCompletedIdx + 1);
            if (loadIt->valid() && loadIt->instruction() &&
                loadIt->instruction()->isExecuted()) {
                loadCompletedIdx++;
                DPRINTF(LSQUnit, "loadCompletedIdx [%d]->[%d]\n", currentIdx,
                        loadCompletedIdx);
            } else {
                break;
            }
        }
    }

    // Advance store completed index (controls RAW queue dequeue rate)
    if (!storeQueue.empty()) {
        for (unsigned i = 0; i < storeCompletionWidth; i++) {
            const int currentIdx = storeCompletedIdx;
            auto storeIt = storeQueue.getIterator(storeCompletedIdx + 1);
            if (storeIt->addrReady() || storeIt->canWB()) {
                storeCompletedIdx++;
                DPRINTF(LSQUnit, "storeCompletedIdx [%d]->[%d]\n", currentIdx,
                        storeCompletedIdx);
            } else {
                break;
            }
        }
    }

    if (lsq->enableReplayBasedMDP() && storeCompletedIdx != prevStoreCompletedIdx) {
        iewStage->mdpAddrReplayUpdateStoreCompletedIdx(lsqID, storeCompletedIdx);
    }

    // Remove completed instructions from RAR and RAW queues
    auto RARIt = RARQueue.begin();
    int RARDequeueCount = 0;
    while (RARIt != RARQueue.end() && RARDequeueCount < rarDequeuePerCycle) {
        if ((*RARIt)->lqIt.idx() <= loadCompletedIdx + 1) {
            RARIt = RARQueue.erase(RARIt);
            RARDequeueCount++;
        } else {
            ++RARIt;
        }
    }

    auto RAWIt = RAWQueue.begin();
    int RAWDequeueCount = 0;
    while (RAWIt != RAWQueue.end() && RAWDequeueCount < rawDequeuePerCycle) {
        if ((*RAWIt)->sqIt.idx() <= storeCompletedIdx + 1) {
            RAWIt = RAWQueue.erase(RAWIt);
            RAWDequeueCount++;
        } else {
            ++RAWIt;
        }
    }

    processReplayQueues();

    if (RARQueue.size() >= maxRARQEntries) {
        ++stats.RARQueueFullCycles;
    }

    if (RAWQueue.size() >= maxRAWQEntries) {
        ++stats.RAWQueueFullCycles;
    }

    // Sample the tracked RAR queue occupancy once per cycle.
    stats.RARQueueAvgEntryNum = RARQueue.size();

    // Sample the tracked RAW queue occupancy once per cycle.
    stats.RAWQueueAvgEntryNum = RAWQueue.size();
}

void
LSQUnit::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
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

PacketPtr
LSQUnit::makeFullFwdPkt(DynInstPtr load_inst, LSQRequest *request)
{
    auto store_it = load_inst->sqIt;
    PacketPtr data_pkt = new Packet(request->mainReq(),
            MemCmd::ReadReq);
    data_pkt->dataStatic(load_inst->memData);

    // hardware transactional memory
    // Store to load forwarding within a transaction
    // This should be okay because the store will be sent to
    // the memory subsystem and subsequently get added to the
    // write set of the transaction. The write set has a stronger
    // property than the read set, so the load doesn't necessarily
    // have to be there.
    assert(!request->mainReq()->isHTMCmd());
    if (load_inst->inHtmTransactionalState()) {
        assert (!storeQueue[store_it._idx].completed());
        assert (
            storeQueue[store_it._idx].instruction()->
                inHtmTransactionalState());
        assert (
            load_inst->getHtmTransactionUid() ==
            storeQueue[store_it._idx].instruction()->
                getHtmTransactionUid());
        data_pkt->setHtmTransactional(
            load_inst->getHtmTransactionUid());
        DPRINTF(HtmCpu, "HTM LD (ST2LDF) "
            "pc=0x%lx - vaddr=0x%lx - "
            "paddr=0x%lx - htmUid=%u\n",
            load_inst->pcState().instAddr(),
            data_pkt->req->hasVaddr() ?
            data_pkt->req->getVaddr() : 0lu,
            data_pkt->getAddr(),
            load_inst->getHtmTransactionUid());
    }

    if (request->isAnyOutstandingRequest()) {
        assert(request->_numOutstandingPackets > 0);
        // There are memory requests packets in flight already.
        // This may happen if the store was not complete the
        // first time this load got executed. Signal the senderSate
        // that response packets should be discarded.
        request->discard();
    }

    return data_pkt;
}

void
LSQUnit::forwardFromBus(DynInstPtr inst, LSQRequest *request)
{
    // load can get it's data from data bus, actually saved in `inst->memData`
    // So there is no need to access Dcache

    inst->setFullForward();
    stats.busForwardSuccess++;
}

Fault
LSQUnit::read(LSQRequest *request, ssize_t load_idx)
{
    LQEntry& load_entry = loadQueue[load_idx];
    const DynInstPtr& load_inst = load_entry.instruction();

    DPRINTF(LoadPipeline, "request: size: %u, Addr: %#lx\n",
            request->mainReq()->getSize(), request->mainReq()->getVaddr());

    // Bookkeeping-only vector alignment stats.  The real access decisions
    // below still use the LSQRequest range/split information.
    Addr addr = request->mainReq()->getVaddr();
    Addr size = request->mainReq()->getSize();
    bool cross16Byte = (addr % 16) + size > 16;
    if (load_inst->isVector() && cross16Byte) {
        if (load_inst->opClass() == enums::VectorUnitStrideLoad) {
            stats.unitStrideCross16Byte++;
        } else {
            stats.nonUnitStrideCross16Byte++;
        }
    }
    if (load_inst->isVector() && !cross16Byte) {
        if (load_inst->opClass() == enums::VectorUnitStrideLoad) {
            stats.unitStrideAligned++;
        }
    }

    if (load_inst->getFault() != NoFault) {
        // If the instruction has an outstanding fault, we cannot complete
        // the access as this discards the current fault.
        DPRINTF(LoadPipeline, "Not completing instruction [sn:%lli] access "
                "due to pending fault.\n", load_inst->seqNum);
        return load_inst->getFault();
    }

    load_entry.setRequest(request);
    assert(load_inst);

    assert(!load_inst->isExecuted());

    // Make sure this isn't a strictly ordered load
    // A bit of a hackish way to get strictly ordered accesses to work
    // only if they're at the head of the LSQ and are ready to commit
    // (at the head of the ROB too).
    if (request->mainReq()->isStrictlyOrdered() &&
        (load_idx != loadQueue.head() || !load_inst->isAtCommit())) {
        // should not enter this
        // Tell IQ/mem dep unit that this instruction will need to be
        // rescheduled eventually
        load_inst->effAddrValid(false);
        load_inst->setCanCommit();
        load_inst->setSkipFollowingPipe();
        load_inst->setRescheduleReplay();
        iewStage->rescheduleMemInst(load_inst);
        ++stats.rescheduledLoads;
        DPRINTF(LoadPipeline, "Strictly ordered load [sn:%lli] PC %s\n",
                load_inst->seqNum, load_inst->pcState());

        // Must delete request now that it wasn't handed off to
        // memory.  This is quite ugly.  @todo: Figure out the proper
        // place to really handle request deletes.
        load_entry.setRequest(nullptr);
        request->discard();
        return std::make_shared<GenericISA::M5PanicFault>(
            "Strictly ordered load [sn:%lli] PC %s\n", load_inst->seqNum,
            load_inst->pcState());
    }

    DPRINTF(LoadPipeline, "Read called, load idx: %i, store idx: %i, "
            "storeHead: %i addr: %#x%s\n",
            load_idx - 1, load_inst->sqIt._idx, storeQueue.head() - 1,
            request->mainReq()->getPaddr(), request->isSplit() ? " split" :
            "");

    // Replay-based MDP gates a load before any forwarding/cache access when
    // the predictor says it must wait for older store addresses.  A replay
    // here means "try this load again after the named store address condition
    // becomes true", not that a real address overlap has been found.
    if (lsq->enableReplayBasedMDP() && request->isNormalLd() &&
        (load_inst->mdpPredStrictWait || !load_inst->mdpProducingStores.empty()) &&
        !request->mainReq()->isLLSC()) {
        if (load_inst->mdpPredStrictWait) {
            // Strict wait: all prior store addresses must be ready.
            if (load_inst->sqIt.idx() > storeCompletedIdx + 1) {
                const size_t required =
                    load_inst->sqIt.idx() ? (load_inst->sqIt.idx() - 1) : 0;
                DPRINTF(LoadPipeline,
                        "Load[sn:%llu] MDP strict wait, storeCompletedIdx=%lu, required>=%lu\n",
                        load_inst->seqNum, storeCompletedIdx, required);
                load_inst->setMdpAddrReplay();
                loadSetReplay(load_inst, request, true);
                iewStage->mdpAddrReplayRegisterStrict(load_inst, required);
                return NoFault;
            }
        } else {
            std::vector<InstSeqNum> wait_stores;
            wait_stores.reserve(load_inst->mdpProducingStores.size());
            for (auto st_it = storeQueue.begin(); st_it != storeQueue.end(); ++st_it) {
                if (!st_it->valid() || !st_it->instruction()) {
                    continue;
                }
                const auto &st_inst = st_it->instruction();
                if (st_inst->seqNum >= load_inst->seqNum) {
                    break;
                }
                if (st_it->addrReady()) {
                    continue;
                }
                if (std::find(load_inst->mdpProducingStores.begin(),
                              load_inst->mdpProducingStores.end(),
                              st_inst->seqNum) != load_inst->mdpProducingStores.end()) {
                    wait_stores.push_back(st_inst->seqNum);
                }
            }

            if (!wait_stores.empty()) {
                DPRINTF(LoadPipeline,
                        "Load[sn:%llu] MDP wait %lu store addrs (replay)\n",
                        load_inst->seqNum, wait_stores.size());
                load_inst->setMdpAddrReplay();
                loadSetReplay(load_inst, request, true);
                iewStage->mdpAddrReplayRegister(load_inst, wait_stores);
                return NoFault;
            }
        }
    }

    if (squashMark) {
        request->mainReq()->setFirstReqAfterSquash();
        squashMark = false;
    }

    // LLSC and local accesses are ISA/device special paths.  They do not use
    // the normal SQ-forwarding -> SBuffer-forwarding -> DCache-send flow.
    if (request->mainReq()->isLLSC()) {
        // Disable recording the result temporarily.  Writing to misc
        // regs normally updates the result, but this is not the
        // desired behavior when handling store conditionals.
        load_inst->recordResult(false);
        load_inst->tcBase()->getIsaPtr()->handleLockedRead(load_inst.get(),
                request->mainReq());
        load_inst->recordResult(true);
    }

    if (request->mainReq()->isLocalAccess()) {
        assert(!load_inst->memData);
        load_inst->memData = new uint8_t[MaxDataBytes];

        gem5::ThreadContext *thread = cpu->tcBase(lsqID);
        PacketPtr main_pkt = new Packet(request->mainReq(), MemCmd::ReadReq);

        main_pkt->dataStatic(load_inst->memData);

        Cycles delay = request->mainReq()->localAccessor(thread, main_pkt);

        WritebackRegEvent *wb = new WritebackRegEvent(load_inst, main_pkt, this);
        cpu->schedule(wb, cpu->clockEdge(delay));
        load_inst->setSkipFollowingPipe();
        DPRINTF(LoadPipeline, "Load [sn:%llu] local access, setSkipFollowingPipe",
                load_inst->seqNum);
        return NoFault;
    }

    // A retry can revisit read() with an LSQRequest that already made cache
    // progress.  Only clear forwarding/snapshot state for a fresh access, so
    // partially sent split requests keep their in-flight packet accounting.
    if (request && !request->hasCachePacketProgress()) {
        request->SBforwardPackets.clear();
        request->SQforwardPackets.clear();
        request->_sbufferBypass = false;
        if (!load_inst->hasPendingCacheReq()) {
            request->_goldenSnapshotCaptured = false;
        }
    }

    // Check older, not-yet-written stores for store-to-load forwarding.
    //
    // Outcomes:
    // - full coverage: record forwarding bytes and mark the load fullForward;
    //   S2 will complete/write back using those bytes.
    // - partial coverage: the load cannot safely complete yet, so reschedule
    //   it and remember the oldest stalled load.
    // - data not ready on an overlapping store: request an STLF replay.
    auto store_it = load_inst->sqIt;
    if (storeWBIt.dereferenceable()) {
        panic_if(store_it < storeWBIt,
                 "[sn:%llu] Load instruction's store index is younger than "
                 "store writeback index",
                 load_inst->seqNum);
    }
    // End once we've reached the top of the LSQ. If storeWBIt is end(), there
    // is no outstanding SQ forwarding window to scan.
    while (storeWBIt.dereferenceable() &&
           store_it != storeWBIt &&
           !load_inst->isDataPrefetch()) {
        // Move the index to one younger
        store_it--;
        assert(store_it->valid());
        assert(store_it->instruction()->seqNum < load_inst->seqNum);
        auto store_req = store_it->request();

        if (store_it->completed()) {
            continue;
        }

        int store_size = store_it->size();

        // Cache maintenance instructions go down via the store
        // path but they carry no data and they shouldn't be
        // considered for forwarding
        if (store_size != 0 && !store_it->instruction()->strictlyOrdered() &&
            !(store_it->request()->mainReq() &&
              store_it->request()->mainReq()->isCacheMaintenance())) {
            assert(store_it->instruction()->effAddrValid());

            auto coverage = AddrRangeCoverage::NoAddrRangeCoverage;

            // Check if store is split (has multiple sub-requests)
            bool store_is_split = store_it->request() && store_it->request()->isSplit();

            // Split vector/scattered requests turn forwarding into a fragment
            // matrix.  Each check below compares one load fragment against one
            // store fragment; any overlap upgrades the whole SQ entry to at
            // least partial coverage.
            if (request->isSplit()) {
                // Case 1: Load is split + Store may be split
                // Check each load sub-request against store (or store sub-requests)
                for (int i = 0; i < request->_reqs.size(); i++) {
                    if (store_is_split) {
                        // Case 1a: Both load and store are split
                        // Check this load sub-request against each store sub-request
                        for (int j = 0; j < store_it->request()->_reqs.size(); j++) {
                            auto sub_coverage =
                                checkStoreLoadForwardingRange(
                                    store_it, request, load_inst, i, j);
                            if (sub_coverage == AddrRangeCoverage::NoAddrRangeCoverage &&
                                load_inst->needSTLFReplay()) {
                                // Handle STLF (Store-to-Load Forwarding) failure
                                stats.forwardSTDNotReady++;
                                iewStage->stlfFailLdReplay(load_inst, store_it->instruction()->seqNum);
                                loadSetReplay(load_inst, request, true);
                                DPRINTF(LoadPipeline, "Load [sn:%llu] setSTLFReplay\n", load_inst->seqNum);
                                return NoFault;
                            }
                            if (sub_coverage != AddrRangeCoverage::NoAddrRangeCoverage) {
                                coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
                                break;
                            }
                        }
                    } else {
                        // Case 1b: Load is split, store is not split
                        auto sub_coverage =
                            checkStoreLoadForwardingRange(
                                store_it, request, load_inst, i, -1);
                        if (sub_coverage == AddrRangeCoverage::NoAddrRangeCoverage &&
                            load_inst->needSTLFReplay()) {
                            // Handle STLF (Store-to-Load Forwarding) failure
                            stats.forwardSTDNotReady++;
                            iewStage->stlfFailLdReplay(load_inst, store_it->instruction()->seqNum);
                            loadSetReplay(load_inst, request, true);
                            DPRINTF(LoadPipeline, "Load [sn:%llu] setSTLFReplay\n", load_inst->seqNum);
                            return NoFault;
                        }
                        if (sub_coverage != AddrRangeCoverage::NoAddrRangeCoverage) {
                            coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
                            break;
                        }
                    }

                    if (coverage != AddrRangeCoverage::NoAddrRangeCoverage) {
                        break;
                    }
                }
            } else {
                // Case 2: Load is not split + Store may be split
                if (store_is_split) {
                    // Case 2a: Load is not split, store is split
                    // Check load against each store sub-request
                    for (int j = 0; j < store_it->request()->_reqs.size(); j++) {
                        auto sub_coverage =
                            checkStoreLoadForwardingRange(
                                store_it, request, load_inst, -1, j);
                        if (sub_coverage == AddrRangeCoverage::NoAddrRangeCoverage &&
                            load_inst->needSTLFReplay()) {
                            // Handle STLF (Store-to-Load Forwarding) failure
                            stats.forwardSTDNotReady++;
                            iewStage->stlfFailLdReplay(load_inst, store_it->instruction()->seqNum);
                            loadSetReplay(load_inst, request, true);
                            DPRINTF(LoadPipeline, "Load [sn:%llu] setSTLFReplay\n", load_inst->seqNum);
                            return NoFault;
                        }
                        if (sub_coverage != AddrRangeCoverage::NoAddrRangeCoverage) {
                            coverage = sub_coverage;
                            break;
                        }
                    }
                } else {
                    // Case 2b: Neither load nor store is split (original case)
                    coverage = checkStoreLoadForwardingRange(store_it, request, load_inst, -1, -1);
                    if (coverage == AddrRangeCoverage::NoAddrRangeCoverage &&
                        load_inst->needSTLFReplay()) {
                        // Handle STLF (Store-to-Load Forwarding) failure
                        stats.forwardSTDNotReady++;
                        iewStage->stlfFailLdReplay(load_inst, store_it->instruction()->seqNum);
                        loadSetReplay(load_inst, request, true);
                        DPRINTF(LoadPipeline, "Load [sn:%llu] setSTLFReplay\n", load_inst->seqNum);
                        return NoFault;
                    }
                }
            }

            if (coverage == AddrRangeCoverage::FullAddrRangeCoverage) {
                // The older store fully covers the load bytes.  Keep the
                // forwarded byte list on the request; loadDoRecvData() will
                // turn this into the final writeback packet.
                // Get shift amount for offset into the store's data.
                int shift_amt = request->mainReq()->getPaddr() -
                    store_it->instruction()->physEffAddr;

                // Allocate memory if this is the first time a load is issued.
                if (!load_inst->memData) {
                    load_inst->memData =
                        new uint8_t[request->mainReq()->getSize()];
                }
                if (store_it->isAllZeros()) {
                    for (int i=0;i<request->mainReq()->getSize();i++) {
                        request->SQforwardPackets.push_back(
                            LSQRequest::FWDPacket{i, 0}
                        );
                    }
                }
                else {
                    for (int i=0;i<request->mainReq()->getSize();i++) {
                        request->SQforwardPackets.push_back(
                            LSQRequest::FWDPacket{i, *(uint8_t*)(store_it->data() + shift_amt + i)}
                        );
                    }
                }

                if (debug::LoadPipeline) {
                    uint64_t first_word = 0;
                    if (load_inst->memData) {
                        auto copy_size = load_inst->effSize;
                        if (copy_size > sizeof(first_word)) {
                            copy_size = sizeof(first_word);
                        }
                        std::memcpy(&first_word, load_inst->memData, copy_size);
                    }
                    DPRINTF(LoadPipeline, "Forwarding from store [sn:%llu] to load [sn:%llu] "
                            "addr %#x, data: %#lx\n", store_it->instruction()->seqNum, load_inst->seqNum,
                            request->mainReq()->getPaddr(), first_word);
                }
                load_inst->setFullForward();

                // Don't need to do anything special for split loads.
                ++stats.forwLoads;
                load_inst->setProducerStorePC(store_it->instruction()->pcState().instAddr());

                return NoFault;
            } else if (
                    coverage == AddrRangeCoverage::PartialAddrRangeCoverage) {
                // Some bytes are covered by the older store and the rest must
                // come from memory.  This model handles that by replaying the
                // load later rather than mixing a partial store forward with a
                // new cache request in the same attempt.
                // If it's already been written back, then don't worry about
                // stalling on it.
                if (store_it->completed()) {
                    panic("Should not check one of these");
                    continue;
                }

                // Must stall load and force it to retry, so long as it's the
                // oldest load that needs to do so.
                if (!stalled ||
                    (stalled &&
                     load_inst->seqNum <
                     loadQueue[stallingLoadIdx].instruction()->seqNum)) {
                    stalled = true;
                    stallingStoreIsn = store_it->instruction()->seqNum;
                    stallingLoadIdx = load_idx;
                }

                // Tell IQ/mem dep unit that this instruction will need to be
                // rescheduled eventually
                load_inst->effAddrValid(false);
                load_inst->setRescheduleReplay();
                iewStage->rescheduleMemInst(load_inst);
                ++stats.rescheduledLoads;

                // Do not generate a writeback event as this instruction is not
                // complete.
                DPRINTF(LoadPipeline, "Load-store forwarding mis-match. "
                        "Store idx %i to load addr %#x\n",
                        store_it._idx, request->mainReq()->getVaddr());

                // Must discard the request.
                request->discard();
                load_entry.setRequest(nullptr);
                return NoFault;
            }
        }
    }

    // If no live SQ entry can satisfy the load, try the SBuffer path.  This is
    // a committed-store forwarding/bypass path, so it is only used for
    // non-split, non-prefetch loads.
    if (!load_inst->isDataPrefetch() && !request->isSplit()) {
        Addr blk_addr = request->mainReq()->getPaddr() & cacheBlockMask;
        auto entry = lsq->findForwardingStoreBufferEntry(
            blk_addr, lsqID, load_inst->seqNum);
        if (entry) {
            if (entry->recordForward(request->mainReq(), request, lsqID,
                                     load_inst->seqNum)) {
                // full forward
                // no need to send to cache
                stats.sbufferFullForward++;
                if (!load_inst->memData) {
                    load_inst->memData = new uint8_t[request->mainReq()->getSize()];
                }

                load_inst->setFullForward();
                if (debug::LoadPipeline) {
                    uint64_t first_word = 0;
                    if (load_inst->memData) {
                        auto copy_size = load_inst->effSize;
                        if (copy_size > sizeof(first_word)) {
                            copy_size = sizeof(first_word);
                        }
                        std::memcpy(&first_word, load_inst->memData, copy_size);
                    }
                    DPRINTF(LoadPipeline, "Load [sn:%llu] forward from sbuffer, data: %lx\n",
                            load_inst->seqNum, first_word);
                }
                return NoFault;
            }
        }
    }

    // Normal memory path after all local forwarding choices failed.  From this
    // point the load either uses an already-returned data-bus response, waits
    // for a pending cache response, or sends a new cache request.
    // Allocate memory if this is the first time a load is issued.
    if (!load_inst->memData) {
        load_inst->memData = new uint8_t[request->mainReq()->getSize()];
    }


    // hardware transactional memory
    if (request->mainReq()->isHTMCmd()) {
        // this is a simple sanity check
        // the Ruby cache controller will set
        // memData to 0x0ul if successful.
        *load_inst->memData = (uint64_t) 0x1ull;
    }

    // For now, load throughput is constrained by the number of
    // load FUs only, and loads do not consume a cache port (only
    // stores do).
    // @todo We should account for cache port contention
    // and arbitrate between loads and stores.
    DPRINTF(Hint, "[sn:%ld] Read\n", load_inst->seqNum);
    auto& bus = getLsq()->bus;
    bool busFwdSuccess = bus.find(load_inst->seqNum) != bus.end();
    if (request->_inst->hasPendingCacheReq()) {
        // A previous send has not produced a TimingResp yet.  Keep the load in
        // pipe and let S2 decide whether the response arrived in time or
        // whether this attempt should replay as a cache miss.
        // Load has been waken up too early, TimingResp is not present now
        // try waiting TimingResp and forward bus again at load s2
        assert(request->isLoad());
        DPRINTF(LoadPipeline, "Load [sn:%ld] setWakeUpEarly\n", load_inst->seqNum);
        load_inst->setWakeUpEarly();
    } else if (busFwdSuccess) {
        DPRINTF(LoadPipeline, "Load [sn:%ld]: Forward from bus, data: %lx\n",
                load_inst->seqNum, *((uint64_t*)load_inst->memData));
        panic_if(bus.size() > lsq->getLQEntries(), "packets on bus should never be greater than LQ size");
        for (auto ele : bus) {
            DPRINTF(LSQUnit, " bus:[sn:%ld], paddr:%lx\n", ele.first, ele.second);
        }
        // this load can forward data from bus
        forwardFromBus(load_inst, request);
    } else {
        // Last resort: build packets and arbitrate for the cache path.  Failure
        // may already have installed a precise replay reason; otherwise the
        // generic cache-blocked replay path owns the retry.
        DPRINTF(LoadPipeline, "Load [sn:%llu] sendPacketToCache\n", load_inst->seqNum);
        // if cannot forward from bus, do real cache access
        bool should_capture_golden =
            system->multiContextDifftest() &&
            cpu->goldenMemManager() &&
            cpu->goldenMemManager()->inPmem(request->mainReq()->getPaddr()) &&
            !request->_goldenSnapshotCaptured;
        request->buildPackets();
        // if the cache is not blocked, do cache access
        request->sendPacketToCache();
        if (request->isSent() && should_capture_golden) {
            uint8_t *issue_golden =
                (uint8_t *)cpu->goldenMemManager()->guestToHost(
                    request->mainReq()->getPaddr());
            load_inst->setGolden(issue_golden);
            request->_goldenSnapshotCaptured = true;
        }
        if (!request->isSent() && !load_inst->needBankConflictReplay() && !load_inst->needMshrArbFailReplay() &&
            !load_inst->needMshrAliasFailReplay() &&!load_inst->needHitInWriteBufferReplay()) {
            const bool partialSplitRequest =
                request->isSplit() && request->_numOutstandingPackets > 0;
            iewStage->blockMemInst(load_inst);
            load_inst->setCacheBlockedReplay();
            DPRINTF(LoadPipeline, "Load [sn:%llu] setCacheBlockedReplay\n", load_inst->seqNum);
            if (partialSplitRequest) {
                // A split load may have already sent one fragment before a
                // later fragment is rejected. Drop the old request so the
                // in-flight fragment response is ignored and the retry starts
                // from a clean request.
                DPRINTF(LoadPipeline,
                        "Load [sn:%llu] discards partially sent split request\n",
                        load_inst->seqNum);
                loadSetReplay(load_inst, request, true);
            }
        }
    }

    return NoFault;
}

Fault
LSQUnit::write(LSQRequest *request, uint8_t *data, ssize_t store_idx)
{
    auto &entry = storeQueue[store_idx];
    assert(entry.valid());

    DPRINTF(StorePipeline, "Doing write to store idx %i, addr %#x | storeHead:%i, size: %d"
            "[sn:%llu]\n",
            store_idx - 1, request->req()->getPaddr(), storeQueue.head() - 1, request->_size,
            entry.instruction()->seqNum);

    entry.setRequest(request);
    unsigned size = request->_size;
    entry.size() = size;
    bool store_no_data =
        request->mainReq()->getFlags() & Request::STORE_NO_DATA;
        entry.isAllZeros() = store_no_data;
    assert(size <= SQEntry::DataSize || store_no_data);

    // copy data into the storeQueue only if the store request has valid data
    if (!(request->req()->getFlags() & Request::CACHE_BLOCK_ZERO) && !request->req()->isCacheMaintenance() &&
        !request->req()->isAtomic() && !entry.instruction()->isSplitStoreAddr()) {
        memcpy(entry.data(), data, size);
    }

    // This function only writes the data to the store queue, so no fault
    // can happen here.
    return NoFault;
}

InstSeqNum
LSQUnit::getLoadHeadSeqNum()
{
    if (loadQueue.front().valid())
        return loadQueue.front().instruction()->seqNum;
    else
        return 0;
}

InstSeqNum
LSQUnit::getStoreHeadSeqNum()
{
    if (storeQueue.front().valid())
        return storeQueue.front().instruction()->seqNum;
    else
        return 0;
}

void
LSQUnit::addToRARReplayQueue(const DynInstPtr &inst)
{
    DPRINTF(LSQUnit, "Adding inst [sn:%llu] to RARReplayQueue\n", inst->seqNum);
    // Record entry time for latency calculation
    inst->RARQueueEntryTick = curTick();

    // Insert in sorted order by seqNum (ascending)
    auto it = std::lower_bound(RARReplayQueue.begin(), RARReplayQueue.end(), inst,
                               [](const DynInstPtr &a, const DynInstPtr &b) { return a->seqNum < b->seqNum; });
    RARReplayQueue.insert(it, inst);
}

void
LSQUnit::addToRAWReplayQueue(const DynInstPtr &inst)
{
    DPRINTF(LSQUnit, "Adding inst [sn:%llu] to RAWReplayQueue\n", inst->seqNum);
    // Record entry time for latency calculation
    inst->RAWQueueEntryTick = curTick();

    // Insert in sorted order by seqNum (ascending)
    auto it = std::lower_bound(RAWReplayQueue.begin(), RAWReplayQueue.end(), inst,
                               [](const DynInstPtr &a, const DynInstPtr &b) { return a->seqNum < b->seqNum; });
    RAWReplayQueue.insert(it, inst);
}

void
LSQUnit::processReplayQueues()
{
    std::vector<DynInstPtr> instsToReplay;

    // Collect remaining RAR instructions that can be completed immediately
    auto RARReplayIt = RARReplayQueue.begin();
    int RARReplayCount = 0;
    while (RARReplayIt != RARReplayQueue.end() && RARReplayCount < rarDequeuePerCycle) {
        if ((*RARReplayIt)->lqIt.idx() <= loadCompletedIdx + 1) {
            DynInstPtr inst = *RARReplayIt;
            instsToReplay.push_back(inst);
            RARReplayIt = RARReplayQueue.erase(RARReplayIt);
            RARReplayCount++;
        } else {
            ++RARReplayIt;
        }
    }

    // Collect remaining RAW instructions that can be completed immediately
    auto RAWReplayIt = RAWReplayQueue.begin();
    int RAWReplayCount = 0;
    while (RAWReplayIt != RAWReplayQueue.end() && RAWReplayCount < rawDequeuePerCycle) {
        if ((*RAWReplayIt)->sqIt.idx() <= storeCompletedIdx + 1) {
            DynInstPtr inst = *RAWReplayIt;
            instsToReplay.push_back(inst);
            RAWReplayIt = RAWReplayQueue.erase(RAWReplayIt);
            RAWReplayCount++;
        } else {
            ++RAWReplayIt;
        }
    }

    // Collect instructions from RAR replay queue when space available
    assert(RARQueue.size() <= maxRARQEntries);
    const int freeRARSize = lsq->logicalFreeRAREntries(lsqID);
    const int maxRARCollect = std::min(freeRARSize, (int)rarDequeuePerCycle - RARReplayCount);
    for (int i = 0; i < maxRARCollect && !RARReplayQueue.empty(); ++i) {
        DynInstPtr inst = RARReplayQueue.front();
        RARReplayQueue.pop_front();
        instsToReplay.push_back(inst);
    }

    // Collect instructions from RAW replay queue when space available
    assert(RAWQueue.size() <= maxRAWQEntries);
    const int freeRAWSize = lsq->logicalFreeRAWEntries(lsqID);
    const int maxRAWCollect = std::min(freeRAWSize, (int)rawDequeuePerCycle - RAWReplayCount);
    for (int i = 0; i < maxRAWCollect && !RAWReplayQueue.empty(); ++i) {
        DynInstPtr inst = RAWReplayQueue.front();
        RAWReplayQueue.pop_front();
        instsToReplay.push_back(inst);
    }

    // Process all collected instructions
    for (const auto& inst : instsToReplay) {
        if (inst->isSquashed()) {
            DPRINTF(LSQUnit, "Removing squashed inst [sn:%llu] from ReplayQueue\n",
                    inst->seqNum);
            continue;
        }

        // Record latency statistics
        bool isRAR = inst->needRARReplay();
        const Tick entryTick = isRAR ? inst->RARQueueEntryTick : inst->RAWQueueEntryTick;
        if (entryTick != (Tick)-1) {
            const Tick latency = curTick() - entryTick;
            const Cycles cycleLatency = cpu->ticksToCycles(latency);
            if (isRAR) {
                stats.RARQueueLatency.sample(cycleLatency);
                stats.RARQueueReplay++;
            } else {
                stats.RAWQueueLatency.sample(cycleLatency);
                stats.RAWQueueReplay++;
            }
        }

        inst->clearReplayType();
        inst->clearNeedReplay();
        inst->issueQue->retryMem(inst);
    }
}

} // namespace o3
} // namespace gem5
