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



void
LSQUnit::issueToStorePipe(const DynInstPtr &inst)
{
    // S0 is shared by all store issue sources in the current cycle.
    panic_if(storePipeSx[0]->size >= storePipeCount,
             "store pipe S0 exceeds scheduler-derived pipe count (%u)\n",
             storePipeCount);
    panic_if(inst->inPipe(), "load [sn:%llu] is already in pipeline", inst->seqNum);
    inst->beginPipelining();

    const int idx = storePipeSx[0]->size;
    storePipeSx[0]->insts.push_back(inst);
    storePipeSx[0]->size = static_cast<int>(storePipeSx[0]->insts.size());
    stats.storePipeAccepted[idx]++;

    DPRINTF(LSQUnit, "issueToStorePipe: [sn:%lli]\n", inst->seqNum);
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
        inst->setTLBMissReplay();
        if (cpu->getTea()) {
            inst->teaSetEvent(TeaEvent::ST_TLB);
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

    // If no space left in the physical SQ,
    // then defer the STA operation, will be replayed by IQ.
    if (!storeQueueWriteReady(inst)) {
        inst->setPhysicalSQFullReplay();
        storeSetReplay(inst, request);
        return NoFault;
    }
    assert(storeQueueWriteReady(inst));

    // Check the recently completed loads to see if any match this store's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = inst->lqIt;

    /* This is the place were instructions get the effAddr. */
    if (request && request->isTranslationComplete()) {
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
    recordAddrOrDataReady(inst);

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

                        if (!inst->needReplay()) {
                            iewStage->notifyExecuted(inst);
                            iewStage->SquashCheckAfterExe(inst);
                        }
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
                } else if (inst->needPhysicalSQFullReplay()) {
                    iewStage->instQueue.deferPhysicalSQFullReplay(inst);
                } else {
                    warn("Unsupported store replay type for [sn:%llu]",
                          inst->seqNum);
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
    assert(storeQueueWriteReady(amo_inst));

    ssize_t amo_idx = amo_inst->sqIdx;

    DPRINTF(LSQUnit, "Executing AMO PC %s [sn:%lli]\n",
            amo_inst->pcState(), amo_inst->seqNum);

    assert(!amo_inst->isSquashed());

    // Check the recently completed loads to see if any match this amo's
    // address.  If so, then we have a memory ordering violation.
    typename LoadQueue::iterator loadIt = amo_inst->lqIt;

    Fault amo_fault = amo_inst->initiateAcc();

    if (amo_inst->isTranslationDelayed() && amo_fault == NoFault) {
        if (cpu->getTea()) {
            amo_inst->teaSetEvent(TeaEvent::ST_TLB);
        }
        return amo_fault;
    }

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

    // AMOs materialize the address/data request directly rather than through
    // STA/STD, but still occupy one valid SQ entry for the counter invariant.
    storeQueue[amo_idx].setStatus(SplitStoreStatus::AddressReady);
    storeQueue[amo_idx].setStatus(SplitStoreStatus::DataReady);
    recordAddrOrDataReady(amo_inst);

    // Atomics need to set themselves as able to writeback if we haven't had a fault by here.
    storeQueue[amo_idx].canWB() = true;
    ++storesToWB;

    return checkViolations(loadIt, amo_inst);
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
        uint8_t issue_golden[16] = {};
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
            uint8_t issue_golden[16] = {};
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
            // Cache-block zero requests can exceed the SQ data array size.
            // Materialize their data only when handing them to the sbuffer.
            std::vector<uint8_t> zeros;
            auto *data = reinterpret_cast<uint8_t *>(storeWBIt->data());
            if (storeWBIt->isAllZeros()) {
                zeros.resize(request->_size, 0);
                data = zeros.data();
            }
            bool success = insertStoreBuffer(
                vaddr, paddr, data, request->_size,
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

} // namespace o3
} // namespace gem5
