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

#include "cpu/o3/lsq_unit.hh"

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
#include "cpu/utils.hh"
#include "debug/Activity.hh"
#include "debug/Diff.hh"
#include "debug/Hint.hh"
#include "debug/HtmCpu.hh"
#include "debug/IEW.hh"
#include "debug/LSQUnit.hh"
#include "debug/O3PipeView.hh"
#include "debug/StoreBuffer.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/cur_tick.hh"

namespace gem5
{

namespace o3
{

void
StoreBufferEntry::reset(uint64_t block_vaddr, uint64_t block_paddr, uint64_t offset, uint8_t *datas, uint64_t size,
                        const std::vector<bool> &mask)
{
    std::fill(validMask.begin(), validMask.begin() + offset, false);

    for (int i = 0; i < size; i++) {
        validMask[offset + i] = mask[i];
    }

    std::fill(validMask.begin() + offset + size, validMask.end(), false);
    memcpy(blockDatas.data() + offset, datas, size);

    this->blockVaddr = block_vaddr;
    this->blockPaddr = block_paddr;
    this->sending = false;
    this->request = nullptr;
    this->vice = nullptr;
}

void
StoreBufferEntry::merge(uint64_t offset, uint8_t *datas, uint64_t size, const std::vector<bool> &mask)
{
    assert(offset + size <= validMask.size());
    for (uint64_t i = 0; i < size; ++i) {
        if (mask[i]) {
            blockDatas[offset + i] = datas[i];
            validMask[offset + i] = true;
        }
    }
}

bool
StoreBufferEntry::recordForward(RequestPtr req, LSQ::LSQRequest *lsqreq)
{
    int offset = req->getPaddr() & (validMask.size() - 1);
    // the offset in the split request
    int goffset = req->getVaddr() - lsqreq->mainReq()->getVaddr();
    if (goffset > 0) {
        assert(offset == 0);
    }
    bool full_forward = true;
    for (int i = 0; i < req->getSize(); i++) {
        assert(goffset + i < lsqreq->_size);
        if (vice && vice->validMask[offset + i]) {
            // vice is newer
            assert(vice->blockVaddr == blockVaddr);
            lsqreq->forwardPackets.push_back(
                LSQ::LSQRequest::FWDPacket{.idx = goffset + i, .byte = vice->blockDatas[offset + i]});
        } else if (validMask[offset + i]) {
            lsqreq->forwardPackets.push_back(
                LSQ::LSQRequest::FWDPacket{.idx = goffset + i, .byte = blockDatas[offset + i]});
        } else {
            full_forward = false;
        }
    }

    return full_forward;
}

void
StoreBuffer::setData(std::vector<StoreBufferEntry *> &data_vec)
{
    this->data_vec = data_vec;
    int way = data_vec.size();
    _size = 0;
    lru_index.set_capacity(way);
    free_list.set_capacity(way);
    crossRef.resize(way);
    data_vec.resize(way);
    data_vld.resize(way, false);
    for (uint64_t i = 0; i < way; i++) {
        free_list.push_back(i);
    }
}

bool
StoreBuffer::full()
{
    return free_list.size() == 0;
}

uint64_t
StoreBuffer::size()
{
    return this->_size;
}

uint64_t
StoreBuffer::unsentSize()
{
    return lru_index.size();
}

StoreBufferEntry *
StoreBuffer::getEmpty()
{
    assert(!full());
    uint64_t index = free_list.back();
    free_list.pop_back();
    return data_vec[index];
}

void
StoreBuffer::insert(int index, uint64_t addr)
{
    assert(_size < data_vec.size());
    assert(!data_vld[index]);
    assert(!lru_index.full());
    _size++;
    auto [it, _] = data_map.insert({addr, data_vec[index]});
    crossRef[index] = it;
    data_vld[index] = true;
    lru_index.push_front(index);
}

StoreBufferEntry *
StoreBuffer::get(uint64_t addr)
{
    auto iter = data_map.find(addr);
    if (iter == data_map.end()) {
        return nullptr;
    }
    assert(data_vld[iter->second->index]);
    return iter->second;
}

void
StoreBuffer::update(int index)
{
    assert(std::find(lru_index.begin(), lru_index.end(), index) != lru_index.end());
    lru_index.erase(std::find(lru_index.begin(), lru_index.end(), index));
    lru_index.push_front(index);
}

StoreBufferEntry *
StoreBuffer::getEvict()
{
    assert(lru_index.size() > 0);
    uint64_t index = lru_index.back();
    lru_index.pop_back();
    assert(data_vld[index]);
    return data_vec[index];
}

StoreBufferEntry *
StoreBuffer::createVice(StoreBufferEntry *entry)
{
    _size++;
    auto vice = getEmpty();
    assert(!entry->vice);
    entry->vice = vice;
    data_vld[vice->index] = true;
    // do not insert map and lru_index
    return vice;
}

void
StoreBuffer::release(StoreBufferEntry *entry)
{
    assert(_size > 0);
    _size--;
    int index = entry->index;
    data_vld[index] = false;
    data_map.erase(crossRef[index]);
    assert(std::find(free_list.begin(), free_list.end(), index) == free_list.end());
    free_list.push_back(index);
    if (entry->vice) {
        // make vice regular
        auto vice = entry->vice;
        assert(data_vld[vice->index]);
        auto [it, _] = data_map.insert({vice->blockPaddr, vice});
        crossRef[vice->index] = it;
        lru_index.push_front(vice->index);
    }
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

LSQUnit::WritebackEvent::WritebackEvent(const DynInstPtr &_inst,
        PacketPtr _pkt, LSQUnit *lsq_ptr)
    : Event(Default_Pri, AutoDelete),
      inst(_inst), pkt(_pkt), lsqPtr(lsq_ptr)
{
    assert(_inst->savedRequest);
    _inst->savedRequest->writebackScheduled();
}

void
LSQUnit::WritebackEvent::process()
{
    assert(!lsqPtr->cpu->switchedOut());

    lsqPtr->writeback(inst, pkt);

    assert(inst->savedRequest);
    inst->savedRequest->writebackDone();
    delete pkt;
}

const char *
LSQUnit::WritebackEvent::description() const
{
    return "Store writeback";
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

    assert(!cpu->switchedOut());
    if (!inst->isSquashed()) {
        if (inst->isLoad() || inst->isAtomic()) {
            Addr addr = pkt->getAddr();
            auto [enable_diff, diff_all_states] = cpu->getDiffAllStates();
            if (system->multiCore() && enable_diff && !request->_sbufferBypass &&
                cpu->goldenMemManager()->inPmem(addr)) {
                // check data with golden mem
                uint8_t *golden_data = (uint8_t *)cpu->goldenMemManager()->guestToHost(addr);
                uint8_t *loaded_data = pkt->getPtr<uint8_t>();
                size_t size = pkt->getSize();
                if (memcmp(golden_data, loaded_data, size) == 0) {
                    assert(size == inst->effSize);
                    inst->setGolden(golden_data);
                } else {
                    panic("Data error at addr %#lx, size %d. %s\n",
                        addr, size,
                        goldenDiffStr(loaded_data, golden_data, size).c_str());
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

            writeback(inst, request->mainPacket());
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

LSQUnit::LSQUnit(uint32_t lqEntries, uint32_t sqEntries, uint32_t sbufferEntries, uint32_t sbufferEvictThreshold,
    uint64_t storeBufferInactiveThreshold, uint32_t ldPipeStages, uint32_t stPipeStages)
    : sbufferEvictThreshold(sbufferEvictThreshold),
      sbufferEntries(sbufferEntries),
      storeBufferWritebackInactive(0),
      storeBufferInactiveThreshold(storeBufferInactiveThreshold),
      lsqID(-1),
      storeQueue(sqEntries),
      loadQueue(lqEntries),
      loadPipe(ldPipeStages - 1, 0),
      storePipe(stPipeStages - 1, 0),
      storesToWB(0),
      htmStarts(0),
      htmStops(0),
      lastRetiredHtmUid(0),
      cacheBlockMask(0),
      stalled(false),
      isStoreBlocked(false),
      storeBlockedfromQue(false),
      storeInFlight(false),
      lastClockSQPopEntries(0),
      lastClockLQPopEntries(0),
      stats(nullptr)
{
    // reserve space, we want if sq will be full, sbuffer will start evicting
    sqFullUpperLimit = sqEntries - 4;
    sqFullLowerLimit = sqFullUpperLimit - 4;

    loadPipeSx.resize(ldPipeStages);
    storePipeSx.resize(stPipeStages);

    for (int i = 0; i < ldPipeStages; i++) {
        loadPipeSx[i] = loadPipe.getWire(-i);
    }
    for (int i = 0; i < stPipeStages; i++) {
        storePipeSx[i] = storePipe.getWire(-i);
    }
    assert(ldPipeStages >= 4 && stPipeStages >= 5);
    assert(sqFullLowerLimit > 0);
}

void
LSQUnit::tick()
{
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

    enableStorePrefetchTrain = params.store_prefetch_train;
    std::vector<StoreBufferEntry*> sbufer;
    for (int i = 0; i < sbufferEntries; i++) {
        sbufer.push_back(new StoreBufferEntry(cpu->cacheLineSize(), i));
    }
    storeBuffer.setData(sbufer);

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

    retryPkt = NULL;
    memDepViolator = NULL;

    stalled = false;

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
      ADD_STAT(sbufferCreateVice, statistics::units::Count::get(), "create vice"),
      ADD_STAT(sbufferEvictDuetoFlush, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferEvictDuetoFull, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferEvictDuetoSQFull, statistics::units::Count::get(), ""),
      ADD_STAT(sbufferEvictDuetoTimeout, statistics::units::Count::get(), ""),
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
      ADD_STAT(unitStrideAligned, "Number of vector unitStride 16-byte aligned")
{
    loadToUse
        .init(0, 299, 10)
        .flags(statistics::nozero);
    loadTranslationLat
        .init(0, 299, 10)
        .flags(statistics::nozero);
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

bool
LSQUnit::pipeLineNukeCheck(const DynInstPtr &load_inst, const DynInstPtr &store_inst)
{
    Addr load_eff_addr1 = load_inst->physEffAddr >> depCheckShift;
    Addr load_eff_addr2 = (load_inst->physEffAddr + load_inst->effSize - 1) >> depCheckShift;

    Addr store_eff_addr1 = store_inst->physEffAddr >> depCheckShift;
    Addr store_eff_addr2 = (store_inst->physEffAddr + store_inst->effSize - 1) >> depCheckShift;

    LSQRequest* store_req = store_inst->savedRequest;
    // Dont perform pipe line nuke check for split load
    bool load_is_splited = load_inst->savedRequest && load_inst->savedRequest->isSplit();
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
    if (ld_inst->effAddrValid() &&
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
            request->isCacheBlockHit(invalidate_addr, cacheBlockMask)) {
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

bool
LSQUnit::skipNukeReplay(const DynInstPtr& load_inst)
{
    // if the load_inst has been marked as `Nuke`
    // load will be replayed, so no Raw violation happens.
    if (lsq->enablePipeNukeCheck()) {
        for (int i = 1; i <= 2; i++) {
            // check loadPipe s1 & s2
            auto& stage = loadPipeSx[i];
            for (int j = 0; j < stage->size; j++) {
                if (load_inst == stage->insts[j] && stage->flags[j][LdStFlags::Nuke]) {
                    return true;
                }
            }
        }
    }
    return false;
}

Fault
LSQUnit::checkViolations(typename LoadQueue::iterator& loadIt,
        const DynInstPtr& inst)
{
    Addr inst_eff_addr1 = inst->physEffAddr >> depCheckShift;
    Addr inst_eff_addr2 = (inst->physEffAddr + inst->effSize - 1) >> depCheckShift;

    /** @todo in theory you only need to check an instruction that has executed
     * however, there isn't a good way in the pipeline at the moment to check
     * all instructions that will execute before the store writes back. Thus,
     * like the implementation that came before it, we're overly conservative.
     */
    DPRINTF(LSQUnit, "Checking for violations for store [sn:%lli], addr: %#lx\n",
            inst->seqNum, inst->physEffAddr);
    while (loadIt != loadQueue.end()) {
        DynInstPtr ld_inst = loadIt->instruction();
        if (!ld_inst->effAddrValid() || ld_inst->strictlyOrdered()) {
            ++loadIt;
            continue;
        }

        Addr ld_eff_addr1 = ld_inst->physEffAddr >> depCheckShift;
        Addr ld_eff_addr2 =
            (ld_inst->physEffAddr + ld_inst->effSize - 1) >> depCheckShift;

        DPRINTF(LSQUnit, "Checking for violations for load [sn:%lli], addr: %#lx\n",
                ld_inst->seqNum, ld_inst->physEffAddr);
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
            } else {
                // A load/store incorrectly passed this store.
                // Check if we already have a violator, or if it's newer
                // squash and refetch.
                if (memDepViolator && ld_inst->seqNum > memDepViolator->seqNum)
                    break;

                // if this load has been marked as Nuke, the load will then be replayed
                // So next time this load replaying to pipeline will forward from store correctly
                // And no RAW violation happens
                if (skipNukeReplay(ld_inst)) {
                    ++loadIt;
                    continue;
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

                return std::make_shared<GenericISA::M5PanicFault>(
                    "Detected fault with "
                    "inst [sn:%lli] and [sn:%lli] at address %#x\n",
                    inst->seqNum, ld_inst->seqNum, ld_eff_addr1);
            }
        }

        ++loadIt;
    }
    return NoFault;
}

void
LSQUnit::loadReplayHelper(DynInstPtr inst, LSQRequest* request, bool cacheMiss, bool fastReplay, bool dropReqNow)
{
    // clear state in this instruction
    inst->effAddrValid(false);
    // Reset DTB translation state
    inst->translationStarted(false);
    inst->translationCompleted(false);
    if (cacheMiss) {
        // set it as waiting for dcache refill
        inst->waitingCacheRefill(true);
        // insert to missed load replay queue
        iewStage->cacheMissLdReplay(inst);
    } else if (fastReplay) {
        // insert to replayQ directly, replay at next cycle
        inst->issueQue->retryMem(inst);
    }
    // clear request in loadQueue
    loadQueue[inst->lqIdx].setRequest(nullptr);
    // set replayed flag in pipeline
    setFlagInPipeLine(inst, LdStFlags::Replayed);
    // cancel subsequent dependent insts of this load
    iewStage->loadCancel(inst);
    if (dropReqNow) {
        // discard this request
        request->discard();
        // TODO: is this essential?
        inst->savedRequest = nullptr;
    }

    DPRINTF(LSQUnit, "[sn:%ld]Load Replayed Because of %s, dropReqNow: %d\n", inst->seqNum,
            cacheMiss  ? "Cache Miss" :
            fastReplay ? "Nuke Fast Replay": "Other Reason", dropReqNow);
}

void
LSQUnit::setFlagInPipeLine(DynInstPtr inst, LdStFlags f)
{
    bool found = false;
    if (inst->isLoad()) {
        for (int i = (loadPipeSx.size() - 1); i >= 0; i--) {
            for (int j = 0; j < loadPipeSx[i]->size; j++) {
                if (inst == loadPipeSx[i]->insts[j]) {
                    found = true;
                    (loadPipeSx[i]->flags[j])[f] = true;
                    break;
                }
            }
        }
    } else {
        for (int i = (storePipeSx.size() - 1); i >= 0; i--) {
            for (int j = 0; j < storePipeSx[i]->size; j++) {
                if (inst == storePipeSx[i]->insts[j]) {
                    found = true;
                    (storePipeSx[i]->flags[j])[f] = true;
                    break;
                }
            }
        }
    }

    if (!found) {
        panic("[sn:%ld] Can not found corresponding inst in PipeLine, isLoad: %d\n", inst->seqNum, inst->isLoad());
    }
}

void
LSQUnit::issueToLoadPipe(const DynInstPtr &inst)
{
    // push to loadPipeS0
    assert(loadPipeSx[0]->size < MaxWidth);
    int idx = loadPipeSx[0]->size;

    loadPipeSx[0]->insts[idx] = inst;
    loadPipeSx[0]->flags[idx][LdStFlags::Valid] = true;
    loadPipeSx[0]->size++;

    DPRINTF(LSQUnit, "issueToLoadPipe: [sn:%lli]\n", inst->seqNum);
    dumpLoadPipe();
}

void
LSQUnit::issueToStorePipe(const DynInstPtr &inst)
{
    // push to storePipeS0
    assert(storePipeSx[0]->size < MaxWidth);
    int idx = storePipeSx[0]->size;

    storePipeSx[0]->insts[idx] = inst;
    storePipeSx[0]->flags[idx][LdStFlags::Valid] = true;
    storePipeSx[0]->size++;

    DPRINTF(LSQUnit, "issueToStorePipe: [sn:%lli]\n", inst->seqNum);
    dumpStorePipe();
}

Fault
LSQUnit::loadPipeS0(const DynInstPtr &inst, std::bitset<LdStFlagNum> &flag)
{
    DPRINTF(LSQUnit, "LoadPipeS0: Executing load PC %s, [sn:%lli] flags: %s\n",
            inst->pcState(), inst->seqNum, getLdStFlagStr(flag));
    assert(!inst->isSquashed());

    Fault load_fault = NoFault;
    // Now initiateAcc only does TLB access
    load_fault = inst->initiateAcc();

    return load_fault;
}

Fault
LSQUnit::loadPipeS1(const DynInstPtr &inst, std::bitset<LdStFlagNum> &flag)
{
    DPRINTF(LSQUnit, "LoadPipeS1: Executing load PC %s, [sn:%lli] flags: %s\n",
            inst->pcState(), inst->seqNum, getLdStFlagStr(flag));
    assert(!inst->isSquashed());

    Fault load_fault = inst->getFault();
    LSQRequest* request = inst->savedRequest;

    // normal inst cache access
    if (request && request->isTranslationComplete()) {
        if (request->isMemAccessRequired()) {
            inst->effAddr = request->getVaddr();
            inst->effAddrValid(true);

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

    if (!inst->translationCompleted()) {
        // TLB miss
        iewStage->loadCancel(inst);
    } else {
        DPRINTF(LSQUnit, "LoadPipeS1: load tlb hit [sn:%lli]\n",
                inst->seqNum);
    }

    if (load_fault == NoFault && !inst->readMemAccPredicate()) {
        flag[LdStFlags::readMemAccNotPredicate] = true;
        return NoFault;
    }

    if (inst->isTranslationDelayed() && load_fault == NoFault) {
        return load_fault;
    }

    if (load_fault != NoFault && inst->translationCompleted() &&
            inst->savedRequest->isPartialFault()
            && !inst->savedRequest->isComplete()) {
        assert(inst->savedRequest->isSplit());
        // If we have a partial fault where the mem access is not complete yet
        // then the cache must have been blocked. This load will be re-executed
        // when the cache gets unblocked. We will handle the fault when the
        // mem access is complete.
        return NoFault;
    }

    if (load_fault != NoFault || !inst->readPredicate()) {
        flag[LdStFlags::HasFault] = load_fault != NoFault;
        flag[LdStFlags::readNotPredicate] = !inst->readPredicate();
    } else {
        if (inst->effAddrValid()) {
            // raw violation check (nuke replay)
            for (int i = 0; i < storePipeSx[1]->size; i++) {
                auto& store_inst = storePipeSx[1]->insts[i];
                if (pipeLineNukeCheck(inst, store_inst)) {
                    DPRINTF(LSQUnit, "LoadPipeS1: Nuke replay at s1, [sn:%lli]\n", inst->seqNum);
                    flag[LdStFlags::Nuke] = true;
                    break;
                }
            }
            // rar violation check
            auto it = inst->lqIt;
            ++it;

            if (checkLoads)
                load_fault = checkViolations(it, inst);
        }
    }

    return load_fault;
}

Fault
LSQUnit::loadPipeS2(const DynInstPtr &inst, std::bitset<LdStFlagNum> &flag)
{
    Fault fault = inst->getFault();
    DPRINTF(LSQUnit, "LoadPipeS2: Executing load PC %s, [sn:%lli] flags: %s\n",
            inst->pcState(), inst->seqNum, getLdStFlagStr(flag));
    assert(!inst->isSquashed());
    LSQRequest* request = inst->savedRequest;

    if (flag[LdStFlags::readMemAccNotPredicate]) {
        assert(inst->readPredicate() && fault == NoFault);
        inst->setExecuted();
        inst->completeAcc(nullptr);
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return NoFault;
    }

    // If the instruction faulted or predicated false, then we need to send it
    // along to commit without the instruction completing.
    if (flag[LdStFlags::HasFault] || flag[LdStFlags::readNotPredicate]) {
        // Send this instruction to commit, also make sure iew stage
        // realizes there is activity.  Mark it as executed unless it
        // is a strictly ordered load that needs to hit the head of
        // commit.
        if (flag[LdStFlags::readNotPredicate])
            inst->forwardOldRegs();
        DPRINTF(LSQUnit, "LoadPipeS2: Load [sn:%lli] not executed from %s\n",
                inst->seqNum, (fault != NoFault ? "fault" : "predication"));
        if (!(inst->hasRequest() && inst->strictlyOrdered()) || inst->isAtCommit()) {
            inst->setExecuted();
        }
        iewStage->instToCommit(inst);
        iewStage->activityThisCycle();
        return fault;
    }

    if (flag[LdStFlags::WakeUpEarly]) {
        auto& bus = getLsq()->bus;
        bool busFwdSuccess = bus.find(inst->seqNum) != bus.end();
        if (inst->hasPendingCacheReq() || !busFwdSuccess) {
            // Load has been waken up too early, even no TimingResp at load s2
            // Or load received TimingResp any time at [s1, s2], but can not find data on bus
            // replay this load
            // warn("Tick:%ld Hint & TimingResp not Match, "
            //     "plz check the timing relationship between Hint & TimingResp, sn:%ld\n", curTick(), inst->seqNum);
            loadReplayHelper(
                inst, request,
                true,  // cache miss
                false, // Dont fast replay
                true   // call request->discard() now
            );
            stats.cacheMissReplayEarly++;
        } else {
            // Load received TimingResp any time at [s1, s2], forward from data bus
            DPRINTF(LSQUnit, "[sn:%ld]: Forward from bus at load s2, data: %lx\n",
                    inst->seqNum, *((uint64_t*)(inst->memData)));
            panic_if(bus.size() > lsq->getLQEntries(), "packets on bus should never be greater than LQ size");
            forwardFrmBus(inst, request);
        }
    }

    if (flag[LdStFlags::Replayed] || flag[LdStFlags::LocalAccess]) {
        return fault;
    }

    // raw violation check (nuke replay)
    for (int i = 0; i < storePipeSx[1]->size; i++) {
        auto& store_inst = storePipeSx[1]->insts[i];
        if (pipeLineNukeCheck(inst, store_inst)) {
            DPRINTF(LSQUnit, "LoadPipeS2: Nuke replay at s2, [sn:%lli]\n", inst->seqNum);
            flag[LdStFlags::Nuke] = true;
            break;
        }
    }

    // check if cache hit & get cache response?
    // NOTE: cache miss replay has higher priority than nuke replay!
    if (lsq->enableLdMissReplay() &&
        request && request->isNormalLd() && !flag[LdStFlags::FullForward] && !flag[LdStFlags::CacheHit]) {
        // cannot get cache data at load s2, replay this load
        loadReplayHelper(
            inst, request,
            true,  // cache miss
            false, // Dont fast replay
            false  // call request->discard() later when TimingResp comes
        );
        return fault;
    }

    if (flag[LdStFlags::Nuke]) {
        assert(lsq->enablePipeNukeCheck());
        // replay load if nuke happens
        loadReplayHelper(
            inst, request,
            false, // cache hit
            true,  // fast replay
            true   // call request->discard() now
        );
        stats.pipeRawNukeReplay++;
    } else {
        // no nuke happens, prepare the inst data
        request = inst->savedRequest;
        if (flag[LdStFlags::FullForward]) {
            assert(request);
            PacketPtr pkt = makeFullFwdPkt(inst, request);
            // this load gets full data from sq or sbuffer
            writeback(inst, pkt);
            request->writebackDone();
            delete pkt;
        } else {
            if (lsq->enableLdMissReplay() && request && request->isNormalLd()) {
                // assemble cache & sbuffer forwarded data and completeDataAcess
                request->assemblePackets();
            }
        }
    }

    return fault;
}

Fault
LSQUnit::loadPipeS3(const DynInstPtr &inst, std::bitset<LdStFlagNum> &flag)
{
    Fault fault = inst->getFault();
    DPRINTF(LSQUnit, "LoadPipeS3: Executing load PC %s, [sn:%lli] flags: %s\n",
            inst->pcState(), inst->seqNum, getLdStFlagStr(flag));
    assert(!inst->isSquashed());
    return fault;
}

void
LSQUnit::executeLoadPipeSx()
{
    // TODO: execute operations in each load pipelines
    Fault fault = NoFault;
    for (int i = 0; i < loadPipeSx.size(); i++) {
        auto& stage = loadPipeSx[i];
        for (int j = 0; j < stage->size; j++) {
            auto& inst = stage->insts[j];
            auto& flag = stage->flags[j];
            if (!inst->isSquashed()) {
                switch (i) {
                    case 0:
                        fault = loadPipeS0(inst, flag);
                        break;
                    case 1:
                        iewStage->getScheduler()->specWakeUpFromLoadPipe(inst);
                        // Loads will mark themselves as executed, and their writeback
                        // event adds the instruction to the queue to commit
                        fault = loadPipeS1(inst, flag);

                        if (inst->isTranslationDelayed() && fault == NoFault) {
                            // A hw page table walk is currently going on; the
                            // instruction must be deferred.
                            DPRINTF(LSQUnit, "Execute: Delayed translation, deferring "
                            "load.\n");
                            iewStage->deferMemInst(inst);
                            flag[LdStFlags::Replayed] = true;
                        }
                        iewStage->SquashCheckAfterExe(inst);
                        break;
                    case 2:
                        fault = loadPipeS2(inst, flag);

                        if (inst->isDataPrefetch() || inst->isInstPrefetch()) {
                            inst->fault = NoFault;
                        }
                        break;
                    case 3:
                        fault = loadPipeS3(inst, flag);
                        break;
                    default:
                        panic("unsupported loadpipe length");
                }
            } else {
                DPRINTF(LSQUnit, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                                " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                                inst->seqNum);
                inst->setExecuted();
                inst->setCanCommit();
                flag[LdStFlags::Squashed] = true;
            }
        }
    }
}

Fault
LSQUnit::storePipeS0(const DynInstPtr &inst, std::bitset<LdStFlagNum> &flag)
{
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);
    assert(!inst->isSquashed());

    DPRINTF(LSQUnit, "StorePipeS0: Executing store PC %s [sn:%lli] flags: %s\n",
            inst->pcState(), inst->seqNum, getLdStFlagStr(flag));

    // Now initiateAcc only does TLB access
    Fault store_fault = inst->initiateAcc();

    return store_fault;
}

Fault
LSQUnit::storePipeS1(const DynInstPtr &inst, std::bitset<LdStFlagNum> &flag)
{
    // Make sure that a store exists.
    assert(storeQueue.size() != 0);

    ssize_t store_idx = inst->sqIdx;
    LSQRequest* request = inst->savedRequest;

    DPRINTF(LSQUnit, "StorePipeS1: Executing store PC %s [sn:%lli] flags: %s\n",
            inst->pcState(), inst->seqNum, getLdStFlagStr(flag));

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = inst->lqIt;

    /* This is the place were instructions get the effAddr. */
    if (request && request->isTranslationComplete()) {
        lsq->isMisaligned(inst, request);
        if (request->isMemAccessRequired() && (inst->getFault() == NoFault)) {
            inst->effAddr = request->getVaddr();
            inst->effAddrValid(true);

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

    if (inst->isTranslationDelayed() &&
        store_fault == NoFault)
        return store_fault;

    if (!inst->readPredicate()) {
        DPRINTF(LSQUnit, "StorePipeS1: Store [sn:%lli] not executed from predication\n",
                inst->seqNum);
        inst->forwardOldRegs();
        flag[LdStFlags::readNotPredicate] = true;
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
        DPRINTF(LSQUnit, "StorePipeS1: Fault on Store PC %s, [sn:%lli], Size = 0\n",
                inst->pcState(), inst->seqNum);
        flag[LdStFlags::HasFault] = true;
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
LSQUnit::emptyStorePipeSx(const DynInstPtr &inst, std::bitset<LdStFlagNum> &flag, uint64_t stage)
{
    // empty store pipe stage, Does not perform any operation
    Fault fault = inst->getFault();
    assert(!inst->isSquashed());

    DPRINTF(LSQUnit, "StorePipeS%d: Executing store PC %s [sn:%lli] flags: %s\n",
            stage, inst->pcState(), inst->seqNum, getLdStFlagStr(flag));
    return fault;
}

void
LSQUnit::executeStorePipeSx()
{
    // TODO: execute operations in each store pipelines
    Fault fault = NoFault;
    for (int i = 0; i < storePipeSx.size(); i++) {
        auto& stage = storePipeSx[i];
        for (int j = 0; j < stage->size; j++) {
            auto& inst = stage->insts[j];
            auto& flag = stage->flags[j];
            if (!inst->isSquashed()) {
                switch (i) {
                    case 0:
                        fault = storePipeS0(inst, flag);
                        break;
                    case 1:
                        fault = storePipeS1(inst, flag);
                        if (inst->isTranslationDelayed() && fault == NoFault) {
                            // A hw page table walk is currently going on; the
                            // instruction must be deferred.
                            DPRINTF(LSQUnit, "Execute: Delayed translation, deferring "
                                    "store.\n");
                            iewStage->deferMemInst(inst);
                            flag[LdStFlags::Replayed] = true;
                            continue;
                        }

                        iewStage->notifyExecuted(inst);
                        iewStage->SquashCheckAfterExe(inst);
                        break;
                    case 2:
                    case 3:
                    case 4:
                        fault = emptyStorePipeSx(inst, flag, i);
                        break;
                    default:
                        panic("unsupported storepipe length");
                }
                if (i == (lsq->storeWbStage() - 1)) {
                    // If the store had a fault then it may not have a mem req
                    if (inst->fault != NoFault || !inst->readPredicate() || !inst->isStoreConditional()) {
                        // If the instruction faulted, then we need to send it
                        // along to commit without the instruction completing.
                        // Send this instruction to commit, also make sure iew
                        // stage realizes there is activity.
                        if (!flag[LdStFlags::Replayed]) {
                            inst->sqIt->setStatus(SplitStoreStatus::StaPipeFinish);
                            if (!inst->isSplitStoreAddr()) {
                                inst->sqIt->setStatus(SplitStoreStatus::StdPipeFinish);
                            }
                            if (inst->fault != NoFault) {
                                inst->setExecuted();
                                iewStage->instToCommit(inst);
                            } else if (inst->sqIt->splitStoreFinish()) {
                                iewStage->instToCommit(inst);
                            }
                            iewStage->activityThisCycle();
                        }
                    }
                }
            } else {
                DPRINTF(LSQUnit, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                                " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                                inst->seqNum);
                inst->setExecuted();
                inst->setCanCommit();
                flag[LdStFlags::Squashed] = true;
            }
        }
    }
}

void
LSQUnit::executePipeSx()
{
    executeLoadPipeSx();
    executeStorePipeSx();
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
        iewStage->instToCommit(amo_inst);
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
                inst->printDisassemblyAndResult(cpu->name());
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
            assert(x.instruction()->isSplitStoreAddr() ? x.splitStoreFinish() : true);
            DPRINTF(LSQUnit, "Marking store as able to write back, PC "
                    "%s [sn:%lli]\n",
                    x.instruction()->pcState(),
                    x.instruction()->seqNum);

            x.canWB() = true;

            ++storesToWB;
        }
    }
}

void
LSQUnit::writebackBlockedStore()
{
    assert(isStoreBlocked);

    if (storeBlockedfromQue) {
        storeWBIt->request()->sendPacketToCache();
        if (storeWBIt->request()->isSent()) {
            storePostSend();
        }
    } else {
        assert(blockedsbufferEntry);
        bool success = blockedsbufferEntry->request->sendPacketToCache();
        if (!success) {
            return;
        }
        blockedsbufferEntry->sending = true;
        blockedsbufferEntry = nullptr;
    }
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
            WritebackEvent *wb = new WritebackEvent(inst, new_pkt, this);
            cpu->schedule(wb, curTick() + 1);
            completeStore(storeWBIt);
            if (!storeQueue.empty())
                storeWBIt++;
            else
                storeWBIt = storeQueue.end();
            return true;
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

void
LSQUnit::offloadToStoreBuffer()
{
    if (isStoreBlocked) {
        writebackBlockedStore();
        if (isStoreBlocked) return;
    }
    if (storeBufferFlushing) {
        return;
    }

    // write the committed store to storebuffer
    int offloaded = 0;
    while (storesToWB > 0 &&
           storeWBIt.dereferenceable() &&
           storeWBIt->valid() &&
           storeWBIt->canWB() &&
           offloaded < maxSQoffload) {

        if (storeWBIt->size() == 0) {
            completeStore(storeWBIt);
            storeWBIt++;
            continue;
        }
        if (storeWBIt->instruction()->isDataPrefetch()) {
            storeWBIt++;
            continue;
        }

        assert(!storeWBIt->committed());
        DynInstPtr inst = storeWBIt->instruction();
        LSQRequest *request = storeWBIt->request();

        if (request->mainReq()->isLLSC() ||
            request->mainReq()->isAtomic() ||
            request->mainReq()->isRelease() ||
            request->mainReq()->isStrictlyOrdered() ||
            inst->isStoreConditional()) {
            DPRINTF(StoreBuffer, "Find atomic/SC store [sn:%llu]\n", storeWBIt->instruction()->seqNum);
            if (!(storeWBIt.idx() == storeQueue.head())) {
                DPRINTF(StoreBuffer, "atomic/SC store waiting\n");
                break;
            }
            if (!storeBufferEmpty()) {
                DPRINTF(StoreBuffer, "sbuffer need flush\n");
                flushStoreBuffer();
                break;
            } else {
                DPRINTF(StoreBuffer, "sbuffer finishing flushed\n");
            }
            bool contin = directStoreToCache();
            if (isStoreBlocked) {
                assert(storeBlockedfromQue);
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
            bool all_send = true;
            for (int i = request->_numOutstandingPackets; i < request->_reqs.size(); i++) {
                auto req = request->_reqs[i];
                Addr vaddr = req->getVaddr();
                Addr paddr = req->getPaddr();
                uint64_t offset = vaddr - vbase;
                DPRINTF(LSQUnit, "Spilt store idx %d [sn:%lli] insert into sbuffer\n", i, inst->seqNum);
                assert(offset + req->getSize() <= storeWBIt->size());
                bool success = insertStoreBuffer(vaddr, paddr, (uint8_t *)storeWBIt->data() + offset, req->getSize(),
                                                 req->getByteEnable());
                if (success) {
                    request->_numOutstandingPackets++;
                } else {
                    break;
                }
            }
            if (request->_numOutstandingPackets == request->_reqs.size()) {
                request->_numOutstandingPackets = 0;
                completeStore(storeWBIt, true);
                storeWBIt++;
            } else {
                break;
            }
            offloaded++;
        } else {
            assert(inst->isSplitStoreAddr() ? storeWBIt->splitStoreFinish() : true);
            Addr vaddr = request->getVaddr();
            Addr paddr = request->mainReq()->getPaddr();
            DPRINTF(LSQUnit, "Store [sn:%lli] insert into sbuffer\n", inst->seqNum);
            bool success = insertStoreBuffer(vaddr, paddr, (uint8_t *)storeWBIt->data(), request->_size,
                                             request->mainReq()->getByteEnable());
            if (!success) {
                break;
            }
            // finish once store
            completeStore(storeWBIt, true);
            storeWBIt++;
            offloaded++;
        }
    }
}

bool LSQUnit::insertStoreBuffer(Addr vaddr, Addr paddr, uint8_t* datas, uint64_t size, const std::vector<bool>& mask)
{
    // access range must in a cache block
    assert((vaddr & cacheBlockMask) == ((vaddr + size - 1) & cacheBlockMask));
    Addr blockVaddr = vaddr & cacheBlockMask;
    Addr blockPaddr = paddr & cacheBlockMask;
    Addr offset = paddr & ~cacheBlockMask;
    // check request is not already in the storebuffer
    auto entry = storeBuffer.get(blockPaddr);
    if (entry) {
        if (entry->sending) {
            if (entry->vice) {
                // merge into vice
                entry = entry->vice;
                entry->merge(offset, datas, size, mask);
                DPRINTF(StoreBuffer, "Merging vice entry[%#x] for addr %#x\n",
                        blockPaddr, paddr);
            } else {
                // create vice for sending entry
                if (storeBuffer.full()) {
                    DPRINTF(StoreBuffer, "Insert %#x failed due to sbuffer full\n", paddr);
                    stats.sbufferFull++;
                    return false;
                }
                stats.sbufferCreateVice++;
                auto vice = storeBuffer.createVice(entry);
                vice->reset(blockVaddr, blockPaddr, offset, datas, size, mask);
                DPRINTF(StoreBuffer, "Create new vice entry[%#x] for addr %#x\n",
                        blockPaddr, paddr);
            }
        } else {
            // merge into unsent
            storeBuffer.update(entry->index);
            entry->merge(offset, datas, size, mask);
            DPRINTF(StoreBuffer, "Merging entry[%#x] for addr %#x\n",
                    blockPaddr, paddr);
        }
    } else {
        // create new entry
        if (storeBuffer.full()) {
            stats.sbufferFull++;
            DPRINTF(StoreBuffer, "Insert %#x failed due to sbuffer full\n", paddr);
            return false;
        }
        // insert
        auto entry = storeBuffer.getEmpty();
        entry->reset(blockVaddr, blockPaddr, offset, datas, size, mask);
        storeBuffer.insert(entry->index, blockPaddr);
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
LSQUnit::storeBufferEvictToCache()
{
    if (storeBufferFlushing && storeBuffer.size() == 0) [[unlikely]] {
        assert(storeBuffer.unsentSize() == 0);
        storeBufferFlushing = false;
        cpu->activityThisCycle();
    }

    // write request will stall one cycle
    // so 2 cycle send one write request
    if (lsq->getDcacheWriteStall()) {
        lsq->setDcacheWriteStall(false);
        return;
    }

    if (isStoreBlocked || storeBuffer.unsentSize() == 0) {
        return;
    }

    if (storeQueue.size() > sqFullUpperLimit) {
        sqWillFull = true;
    } else if (storeQueue.size() < sqFullLowerLimit) {
        sqWillFull = false;
    }

    if ((storeBuffer.unsentSize() > sbufferEvictThreshold) ||
        (storeBufferWritebackInactive > storeBufferInactiveThreshold) ||
        (sqWillFull) ||
        storeBufferFlushing) {

        if (storeBufferFlushing) {
            stats.sbufferEvictDuetoFlush++;
            DPRINTF(StoreBuffer, "sbuffer flushing\n");
        } else if (storeBuffer.unsentSize() > sbufferEvictThreshold) {
            stats.sbufferEvictDuetoFull++;
            DPRINTF(StoreBuffer, "sbuffer has reached threshold\n");
        } else if (sqWillFull) {
            stats.sbufferEvictDuetoSQFull++;
            DPRINTF(StoreBuffer, "sbuffer has reached SQ threshold\n");
        } else {
            stats.sbufferEvictDuetoTimeout++;
            DPRINTF(StoreBuffer, "sbuffer has reached timeout\n");
        }

        // evict entry to cache
        auto entry = storeBuffer.getEvict();
        DPRINTF(StoreBuffer, "Evicting sbuffer entry[%#x]\n",
                entry->blockPaddr);

        if (debug::StoreBuffer) {
            DPRINTFR(StoreBuffer, "Dumping sbuffer entry data\n");
            for (int i = 0; i < cacheLineSize(); i++) {
                DPRINTFR(StoreBuffer, "%s%d ", entry->validMask[i] ? "" : "!", (uint32_t)entry->blockDatas[i]);
            }
            DPRINTFR(StoreBuffer, "\n");
        }

        // send packet to cache
        assert(entry->request == nullptr);

        entry->request = new LSQ::SbufferRequest(cpu, this, entry->blockPaddr, entry->blockDatas.data());
        entry->request->addReq(entry->blockVaddr, entry->blockPaddr, entry->validMask);
        entry->request->buildPackets();
        entry->request->sbuffer_entry = entry;
        bool success = entry->request->sendPacketToCache();
        if (!success) {
            blockedsbufferEntry = entry;
            DPRINTF(StoreBuffer, "send packet fail\n");
            return;
        }
        DPRINTF(StoreBuffer, "send packet successed\n");
        entry->sending = true;
        lsq->setDcacheWriteStall(true);
        storeBufferWritebackInactive = 0;
    } else {
        // Timeout
        storeBufferWritebackInactive++;
    }
}

void
LSQUnit::flushStoreBuffer()
{
    storeBufferFlushing = true;
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
LSQUnit::writeback(const DynInstPtr &inst, PacketPtr pkt)
{
    assert(!inst->isSplitStoreAddr());
    iewStage->wakeCPU();
    // Squashed instructions do not need to complete their access.
    if (inst->isSquashed()) {
        assert (!inst->isStore() || inst->isStoreConditional());
        ++stats.ignoredResponses;
        return;
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
                       inst->savedRequest->isPartialFault());

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

    // Need to insert instruction into queue to commit
    iewStage->instToCommit(inst);

    iewStage->activityThisCycle();

    // see if this load changed the PC
    iewStage->checkMisprediction(inst);
}

void
LSQUnit::completeSbufferEvict(PacketPtr pkt)
{
    auto request = dynamic_cast<LSQ::SbufferRequest *>(pkt->senderState);
    if (cpu->goldenMemManager() && cpu->goldenMemManager()->inPmem(request->mainReq()->getPaddr())) {
        Addr paddr = request->mainReq()->getPaddr();
        DPRINTF(LSQUnit, "StoreBuffer writing to golden memory at addr %#x\n", paddr);
        cpu->goldenMemManager()->updateGoldenMem(paddr, request->_data, request->mainReq()->getByteEnable(),
                                                 request->_size);
    }
    storeBuffer.release(request->sbuffer_entry);
    DPRINTF(StoreBuffer, "finish entry[%#x] evict to cache, sbuffer size: %d, unsentsize: %d\n", pkt->getAddr(),
            storeBuffer.size(), storeBuffer.unsentSize());
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

    DPRINTF(LSQUnit, "Completing store [sn:%lli], idx:%i, store head "
            "idx:%i\n",
            store_inst->seqNum, store_idx.idx() - 1, storeQueue.head() - 1);

    if (!from_sbuffer &&
        (!store_inst->isStoreConditional() || store_inst->lockedWriteSuccess()) &&
        cpu->goldenMemManager() &&
        cpu->goldenMemManager()->inPmem(request->mainReq()->getPaddr())) {
        Addr paddr = request->mainReq()->getPaddr();
        if (!store_inst->isAtomic()) {
            DPRINTF(LSQUnit, "Store writing to golden memory at addr %#x, data %#lx, mask %#x, size %d\n",
                    paddr, *((uint64_t *)store_inst->memData), 0xff, request->_size);
            cpu->goldenMemManager()->updateGoldenMem(paddr, store_inst->memData, 0xff,
                                                     request->_size);
        } else {
            uint8_t tmp_data[8];
            memset(tmp_data, 0, 8);
            memcpy(tmp_data, store_inst->memData, request->_size);
            assert(request->req()->getAtomicOpFunctor());

            // read golden memory to get the global latest value before this AMO is executed for further compare
            cpu->goldenMemManager()->readGoldenMem(paddr,
                                                   store_inst->getAmoOldGoldenValuePtr(), request->_size);
            cpu->diffInfo.amoOldGoldenValue = store_inst->getAmoOldGoldenValue();

            // before amo operate on golden memory
            (*(request->req()->getAtomicOpFunctor()))(tmp_data);
            // after amo operate on golden memory

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
LSQUnit::trySendPacket(bool isLoad, PacketPtr data_pkt, bool &bank_conflict, bool &tag_read_fail)
{
    // sbuffer do not call this
    if (lsq->getLastConflictCheckTick() != curTick()) {
        lsq->clearAddresses(curTick());
    }
    bool ret = true;
    bool cache_got_blocked = false;

    LSQRequest *request = dynamic_cast<LSQRequest *>(data_pkt->senderState);
    if (isLoad) {
        bank_conflict = lsq->bankConflictedCheck(data_pkt->req->getVaddr());
    }

    PacketPtr pkt = data_pkt;

    if (!lsq->cacheBlocked() && lsq->cachePortAvailable(isLoad)) {
        if (bank_conflict) {
            ++stats.bankConflictTimes;
            if (!isLoad) {
                assert(request == storeWBIt->request());
                isStoreBlocked = true;
                storeBlockedfromQue = true;
            }
            bank_conflict = true;
            ret = false;
        }
        if (!bank_conflict && !dcachePort->sendTimingReq(data_pkt)) {
            ret = false;
            tag_read_fail = data_pkt->tagReadFail;
            if (!tag_read_fail) {
                cache_got_blocked = true;
            }
        }
    } else {
        ret = false;
    }

    if (ret) {
        if (!isLoad) {
            isStoreBlocked = false;
        }
        lsq->cachePortBusy(isLoad);
        request->packetSent();

        if (isLoad) {
            auto entry = storeBuffer.get(pkt->getAddr() & cacheBlockMask);
            if (entry) {
                DPRINTF(StoreBuffer, "sbuffer entry[%#x] coverage %s\n", entry->blockPaddr, pkt->print());
                if (entry->recordForward(pkt->req, request)) {
                    assert(request->isSplit()); // here must be split request
                    stats.sbufferFullForward++;
                } else if (!request->forwardPackets.empty()) {
                    stats.sbufferPartiForward++;
                }
            }
        }
    } else {
        if (cache_got_blocked) {
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        if (!isLoad) {
            assert(request == storeWBIt->request());
            isStoreBlocked = true;
            storeBlockedfromQue = true;
        }
        request->packetNotSent();
    }
    DPRINTF(LSQUnit,
            "Memory request (pkt: %s) from inst [sn:%llu] was"
            " %ssent (cache is blocked: %d, cache_got_blocked: %d, bank conflict: %d, tag_read_fail: %d)\n",
            data_pkt->print(), request->instruction()->seqNum, ret ? "" : "not ", lsq->cacheBlocked(),
            cache_got_blocked, bank_conflict, tag_read_fail);
    return ret;
}

bool
LSQUnit::sbufferSendPacket(PacketPtr data_pkt)
{
    if (lsq->getLastConflictCheckTick() != curTick()) {
        lsq->clearAddresses(curTick());
    }
    bool ret = true;
    bool cache_got_blocked = false;

    if (!lsq->cacheBlocked() && lsq->cachePortAvailable(false)) {
        if (!dcachePort->sendTimingReq(data_pkt)) {
            ret = false;
            cache_got_blocked = true;
        }
    } else {
        ret = false;
    }

    if (ret) {
        isStoreBlocked = false;
        lsq->cachePortBusy(false);
    } else {
        if (cache_got_blocked) {
            lsq->cacheBlocked(true);
            ++stats.blockedByCache;
        }
        isStoreBlocked = true;
        storeBlockedfromQue = false;
    }
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
LSQUnit::recvRetry()
{
    if (isStoreBlocked) {
        DPRINTF(LSQUnit, "Receiving retry: blocked store\n");
        writebackBlockedStore();
    }
}

void
LSQUnit::dumpLoadPipe()
{
    if (GEM5_UNLIKELY(::gem5::debug::LSQUnit)) {
        DPRINTFN("Dumping LoadPipe:\n");
        for (int i = 0; i < loadPipeSx.size(); i++) {
            DPRINTFN("Load S%d:, size: %d\n", i, loadPipeSx[i]->size);
            for (int j = 0; j < loadPipeSx[i]->size; j++) {
                DPRINTFN("  PC: %s, [tid:%i] [sn:%lli] flags: %s\n",
                        loadPipeSx[i]->insts[j]->pcState(),
                        loadPipeSx[i]->insts[j]->threadNumber,
                        loadPipeSx[i]->insts[j]->seqNum,
                        getLdStFlagStr(loadPipeSx[i]->flags[j])
                );
            }
        }
    }
}

void
LSQUnit::dumpStorePipe()
{
    if (GEM5_UNLIKELY(::gem5::debug::LSQUnit)) {
        DPRINTFN("Dumping StorePipe:\n");
        for (int i = 0; i < storePipeSx.size(); i++) {
            DPRINTFN("Store S%d:, size: %d\n", i, storePipeSx[i]->size);
            for (int j = 0; j < storePipeSx[i]->size; j++) {
                DPRINTFN("  PC: %s, [tid:%i] [sn:%lli] flags: %s\n",
                        storePipeSx[i]->insts[j]->pcState(),
                        storePipeSx[i]->insts[j]->threadNumber,
                        storePipeSx[i]->insts[j]->seqNum,
                        getLdStFlagStr(storePipeSx[i]->flags[j])
                );
            }
        }
    }
}

void
LSQUnit::dumpInsts() const
{
    cprintf("Load store queue: Dumping instructions.\n");
    cprintf("Load queue size: %i\n", loadQueue.size());
    cprintf("Load queue: ");

    for (const auto& e: loadQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
    }
    cprintf("\n");

    cprintf("Store queue size: %i\n", storeQueue.size());
    cprintf("Store queue: ");

    for (const auto& e: storeQueue) {
        const DynInstPtr &inst(e.instruction());
        cprintf("%s.[sn:%llu] ", inst->pcState(), inst->seqNum);
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
LSQUnit::forwardFrmBus(DynInstPtr inst, LSQRequest *request)
{
    // load can get it's data from data bus, actually saved in `inst->memData`
    // So there is no need to access Dcache

    // forward sbuffer again
    auto entry = storeBuffer.get(request->mainReq()->getPaddr() & cacheBlockMask);
    if (entry) {
        if (entry->recordForward(request->mainReq(), request)) {
            assert(request->isSplit()); // here must be split request
            stats.sbufferFullForward++;
        } else {
            stats.sbufferPartiForward++;
        }
    }
    // merge forward result
    request->forward();
    // set as FullForwarded, writeback at load s2
    setFlagInPipeLine(inst, LdStFlags::FullForward);
    stats.busForwardSuccess++;
}

Fault
LSQUnit::read(LSQRequest *request, ssize_t load_idx)
{
    LQEntry& load_entry = loadQueue[load_idx];
    const DynInstPtr& load_inst = load_entry.instruction();

    DPRINTF(LSQUnit, "request: size: %u, Addr: %#lx\n",
            request->mainReq()->getSize(), request->mainReq()->getVaddr());

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

    if (lsq->isMisaligned(load_inst, request)) {
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
        iewStage->rescheduleMemInst(load_inst);
        load_inst->effAddrValid(false);
        setFlagInPipeLine(load_inst, LdStFlags::Replayed);
        ++stats.rescheduledLoads;
        DPRINTF(LSQUnit, "Strictly ordered load [sn:%lli] PC %s\n",
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

    DPRINTF(LSQUnit, "Read called, load idx: %i, store idx: %i, "
            "storeHead: %i addr: %#x%s\n",
            load_idx - 1, load_inst->sqIt._idx, storeQueue.head() - 1,
            request->mainReq()->getPaddr(), request->isSplit() ? " split" :
            "");

    if (squashMark) {
        request->mainReq()->setFirstReqAfterSquash();
        squashMark = false;
    }

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

        WritebackEvent *wb = new WritebackEvent(load_inst, main_pkt, this);
        cpu->schedule(wb, cpu->clockEdge(delay));
        setFlagInPipeLine(load_inst, LdStFlags::LocalAccess);
        return NoFault;
    }

    // Check the SQ for any previous stores that might lead to forwarding
    auto store_it = load_inst->sqIt;
    assert (store_it >= storeWBIt);
    // End once we've reached the top of the LSQ
    while (store_it != storeWBIt && !load_inst->isDataPrefetch()) {
        // Move the index to one younger
        store_it--;
        assert(store_it->valid());
        assert(store_it->instruction()->seqNum < load_inst->seqNum);
        int store_size = store_it->size();

        // Cache maintenance instructions go down via the store
        // path but they carry no data and they shouldn't be
        // considered for forwarding
        if (store_size != 0 && !store_it->instruction()->strictlyOrdered() &&
            !(store_it->request()->mainReq() &&
              store_it->request()->mainReq()->isCacheMaintenance())) {
            assert(store_it->instruction()->effAddrValid());

            // Check if the store data is within the lower and upper bounds of
            // addresses that the request needs.
            auto req_s = request->mainReq()->getPaddr();
            auto req_e = req_s + request->mainReq()->getSize();
            auto st_s = store_it->instruction()->physEffAddr;
            auto st_e = st_s + store_size;

            bool store_has_lower_limit = req_s >= st_s;
            bool store_has_upper_limit = req_e <= st_e;
            bool lower_load_has_store_part = req_s < st_e;
            bool upper_load_has_store_part = req_e > st_s;

            DPRINTF(LSQUnit, "req_s:%x,req_e:%x,st_s:%x,st_e:%x\n", req_s,
                    req_e, st_s, st_e);
            DPRINTF(LSQUnit, "store_size:%x,store_pc:%s,req_size:%x,req_pc:%s\n",
                    store_size, store_it->instruction()->pcState(),
                    request->mainReq()->getSize(),
                    request->instruction()->pcState());

            auto coverage = AddrRangeCoverage::NoAddrRangeCoverage;

            // If the store entry is not atomic (atomic does not have valid
            // data), the store has all of the data needed, and
            // the load is not LLSC, then
            // we can forward data from the store to the load
            if ((!store_it->instruction()->isAtomic() &&
                 store_has_lower_limit && store_has_upper_limit &&
                 !request->mainReq()->isLLSC()) &&
                (!((req_s > req_e) || (st_s > st_e)))) {

                const auto &store_req = store_it->request()->mainReq();
                if (store_it->instruction()->isSplitStoreAddr() && !store_it->canForwardToLoad()) {
                    stats.forwardSTDNotReady++;
                    // insert load inst into replayQ and wait for store data to be ready
                    iewStage->stlfFailLdReplay(load_inst, store_it->instruction()->seqNum);
                    loadReplayHelper(load_inst, request, false, false, true);
                    return NoFault;
                } else {
                    coverage = store_req->isMasked()
                                ? AddrRangeCoverage::PartialAddrRangeCoverage
                                : AddrRangeCoverage::FullAddrRangeCoverage;
                }

            } else if ((!((req_s > req_e) || (st_s > st_e))) &&
                       (
                           // This is the partial store-load forwarding case
                           // where a store has only part of the load's data
                           // and the load isn't LLSC
                           (!request->mainReq()->isLLSC() &&
                            ((store_has_lower_limit &&
                              lower_load_has_store_part) ||
                             (store_has_upper_limit &&
                              upper_load_has_store_part) ||
                             (lower_load_has_store_part &&
                              upper_load_has_store_part))) ||
                           // The load is LLSC, and the store has all or part
                           // of the load's data
                           (request->mainReq()->isLLSC() &&
                            ((store_has_lower_limit ||
                              upper_load_has_store_part) &&
                             (store_has_upper_limit ||
                              lower_load_has_store_part))) ||
                           // The store entry is atomic and has all or part of
                           // the load's data
                           (store_it->instruction()->isAtomic() &&
                            ((store_has_lower_limit ||
                              upper_load_has_store_part) &&
                             (store_has_upper_limit ||
                              lower_load_has_store_part))))) {

                coverage = AddrRangeCoverage::PartialAddrRangeCoverage;
            }

            if (coverage == AddrRangeCoverage::FullAddrRangeCoverage) {
                // Get shift amount for offset into the store's data.
                int shift_amt = request->mainReq()->getPaddr() -
                    store_it->instruction()->physEffAddr;

                // Allocate memory if this is the first time a load is issued.
                if (!load_inst->memData) {
                    load_inst->memData =
                        new uint8_t[request->mainReq()->getSize()];
                }
                if (store_it->isAllZeros())
                    memset(load_inst->memData, 0,
                            request->mainReq()->getSize());
                else{
                    memcpy(load_inst->memData,
                        store_it->data() + shift_amt,
                        request->mainReq()->getSize());

                }

                DPRINTF(LSQUnit, "Forwarding from store idx %i to load to "
                        "addr %#x\n", store_it._idx,
                        request->mainReq()->getVaddr());

                // set FullForward flag, then this load will be written back at s2
                setFlagInPipeLine(load_inst, LdStFlags::FullForward);

                // Don't need to do anything special for split loads.
                ++stats.forwLoads;

                return NoFault;
            } else if (
                    coverage == AddrRangeCoverage::PartialAddrRangeCoverage) {
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
                iewStage->rescheduleMemInst(load_inst);
                load_inst->effAddrValid(false);
                setFlagInPipeLine(load_inst, LdStFlags::Replayed);
                ++stats.rescheduledLoads;

                // Do not generate a writeback event as this instruction is not
                // complete.
                DPRINTF(LSQUnit, "Load-store forwarding mis-match. "
                        "Store idx %i to load addr %#x\n",
                        store_it._idx, request->mainReq()->getVaddr());

                // Must discard the request.
                request->discard();
                load_entry.setRequest(nullptr);
                return NoFault;
            }
        }
    }

    // sbuffer forward
    if (!load_inst->isDataPrefetch() && !request->isSplit()) {
        Addr blk_addr = request->mainReq()->getPaddr() & cacheBlockMask;
        int offset = request->mainReq()->getPaddr() & ~cacheBlockMask;
        auto entry = storeBuffer.get(blk_addr);
        if (entry) {
            if (entry->recordForward(request->mainReq(), request)) {
                // full forward
                // no need to send to cache
                stats.sbufferFullForward++;
                if (!load_inst->memData) {
                    load_inst->memData = new uint8_t[request->mainReq()->getSize()];
                }

                request->forward();

                // set FullForward flag, then this load will be written back at s2
                setFlagInPipeLine(load_inst, LdStFlags::FullForward);

                return NoFault;
            }
            // if not fully forward, need to clear buffer
            request->forwardPackets.clear();
        }
    }


    // If there's no forwarding case, then go access memory
    DPRINTF(LSQUnit, "Doing memory access for inst [sn:%lli] PC %s\n",
            load_inst->seqNum, load_inst->pcState());

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
        // Load has been waken up too early, TimingResp is not present now
        // try waiting TimingResp and forward bus again at load s2
        assert(request->isLoad());
        setFlagInPipeLine(load_inst, LdStFlags::WakeUpEarly);
    } else if (busFwdSuccess) {
        DPRINTF(LSQUnit, "[sn:%ld]: Forward from bus at load s1, data: %lx\n",
                load_inst->seqNum, *((uint64_t*)(load_inst->memData)));
        panic_if(bus.size() > lsq->getLQEntries(), "packets on bus should never be greater than LQ size");
        for (auto ele : bus) {
            DPRINTF(LSQUnit, " bus:[sn:%ld], paddr:%lx\n", ele.first, ele.second);
        }
        // this load can forward data from bus
        forwardFrmBus(load_inst, request);
    } else {
        // if cannot forward from bus, do real cache access
        request->buildPackets();
        // if the cache is not blocked, do cache access
        if (!request->sendPacketToCache()) {
            iewStage->loadCancel(load_inst);
        }
        if (!request->isSent()) {
            iewStage->blockMemInst(load_inst);
            setFlagInPipeLine(load_inst, LdStFlags::Replayed);
        }
    }

    return NoFault;
}

Fault
LSQUnit::write(LSQRequest *request, uint8_t *data, ssize_t store_idx)
{
    auto &entry = storeQueue[store_idx];
    assert(entry.valid());

    DPRINTF(LSQUnit, "Doing write to store idx %i, addr %#x | storeHead:%i, size: %d"
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

} // namespace o3
} // namespace gem5
