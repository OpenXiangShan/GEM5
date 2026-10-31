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
        // Record response facts before per-request replay or completion.
        if (const auto &inst = request->instruction()) {
            inst->cacheAccessDepth = std::max(
                inst->cacheAccessDepth, pkt->cacheAccessDepth);
            if (auto *tea = cpu->getTea();
                tea && !request->mainReq()->isUncacheable()) {
                tea->cacheMiss(inst, pkt->cacheAccessDepth);
            }
        }
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
    // Record cache access depth for MLP predictor
    // Use max to avoid overwrite by replay packet (depth=0)
    inst->cacheAccessDepth = std::max(inst->cacheAccessDepth,
                                       pkt->cacheAccessDepth);
    if (cpu->getTea() && pkt->cacheAccessDepth > 0) {
        inst->teaSetEvent(TeaEvent::ST_L1);
    }
    DPRINTF(LSQUnit, "[sn:%llu] completeDataAccess: depth=%d (pkt=%d), addr=%#lx\n",
            inst->seqNum, inst->cacheAccessDepth, pkt->cacheAccessDepth,
            pkt->getAddr());


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

        // adjust golden_mask according to the request->_size
        // request->_size = 8 , mask = 0xff
        // request->_size = 16 , mask = 0xffff
        uint64_t golden_mask = (request->_size >= 64) ? ~0ULL :  ((1ULL << request->_size) - 1);
        if (!store_inst->isAtomic()) {
            DPRINTF(LSQUnit, "Store writing to golden memory at addr %#x, data %#lx, mask %#x, size %d\n",
                    paddr, *((uint64_t *)store_inst->memData), golden_mask, request->_size);
            cpu->goldenMemManager()->updateGoldenMem(paddr, store_inst->memData, golden_mask,
                                                     request->_size);
        } else {
            uint8_t tmp_data[16];
            memset(tmp_data, 0, sizeof(tmp_data));
            assert(request->req()->getAtomicOpFunctor());

            // The AMO response returns the old memory value. Capture it on the
            // instruction so commit/difftest can use a per-inst golden copy
            // under SMT, but derive the new memory image from the DUT-observed
            // old value captured in goldenData.
            memcpy(tmp_data, store_inst->getGolden(), request->_size);

            (*(request->req()->getAtomicOpFunctor()))(tmp_data);

            DPRINTF(LSQUnit, "AMO writing to golden memory at addr %#x, data %#lx, mask %#x, size %d\n",
                    paddr, *((uint64_t *)(tmp_data)), golden_mask, request->_size);
            cpu->goldenMemManager()->updateGoldenMem(paddr, tmp_data, golden_mask,
                                                     request->_size);
        }
    }

    if (store_idx == storeQueue.begin()) {
        do {
            recordAddrOrDataDequeue(storeQueue.front().instruction());
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

} // namespace o3
} // namespace gem5
