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

    /*
     * A split load can have a PartialFault: an earlier fragment translated
     * successfully while a later fragment faulted.  The translated prefix
     * must still be sent to memory.  Keep the architectural fault attached
     * to the instruction, but do not block the successful fragments here.
     */
    const bool partial_fault_load =
        request->isSplit() && request->isPartialFault();

    if (load_inst->getFault() != NoFault &&
        !partial_fault_load) {
        // A full translation fault has no valid memory fragment to access.
        DPRINTF(LoadPipeline,
                "Not completing instruction [sn:%lli] access "
                "due to pending fault.\n",
                load_inst->seqNum);
        return load_inst->getFault();
    }

    if (partial_fault_load) {
        DPRINTF(LoadPipeline,
                "Continuing partial-fault split load [sn:%lli]; "
                "sending successfully translated prefix fragments.\n",
                load_inst->seqNum);
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
            auto rememberPredStore = [&](const DynInstPtr &st_inst) {
                if (!load_inst->memDepInfo.predicted) {
                    return;
                }
                auto &info = load_inst->memDepInfo;
                if (info.predStoreSizes.first != 0 &&
                    info.predStoreAddrs.first == st_inst->effAddr &&
                    info.predStoreSizes.first == st_inst->effSize) {
                    return;
                }
                if (info.predStoreSizes.second != 0 &&
                    info.predStoreAddrs.second == st_inst->effAddr &&
                    info.predStoreSizes.second == st_inst->effSize) {
                    return;
                }
                if (info.predStoreSizes.first == 0) {
                    info.predStoreAddrs.first = st_inst->effAddr;
                    info.predStoreSizes.first = st_inst->effSize;
                } else if (info.predStoreSizes.second == 0) {
                    info.predStoreAddrs.second = st_inst->effAddr;
                    info.predStoreSizes.second = st_inst->effSize;
                }
            };

            for (auto st_it = storeQueue.begin(); st_it != storeQueue.end(); ++st_it) {
                if (!st_it->valid() || !st_it->instruction()) {
                    continue;
                }
                const auto &st_inst = st_it->instruction();
                if (st_inst->seqNum >= load_inst->seqNum) {
                    break;
                }

                const bool predicted_store =
                    std::find(load_inst->mdpProducingStores.begin(),
                              load_inst->mdpProducingStores.end(),
                              st_inst->seqNum) !=
                    load_inst->mdpProducingStores.end();
                if (!predicted_store) {
                    continue;
                }
                if (st_it->addrReady()) {
                    rememberPredStore(st_inst);
                    continue;
                }
                wait_stores.push_back(st_inst->seqNum);
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
                load_inst->memDepInfo.forwardedFrom =
                    store_it->instruction()->seqNum;

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
            blk_addr, lsqID);
        if (entry) {
            if (entry->recordForward(request->mainReq(), request, lsqID)) {
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

} // namespace o3
} // namespace gem5
