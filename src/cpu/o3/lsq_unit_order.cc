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
                if (cpu->getTea()) {
                    ld_inst->teaSetEvent(TeaEvent::FL_MO);
                }
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
            if (!req0 || !req0->hasPaddr()) {
                continue;
            }
            Addr inst_eff_addr1 = req0->getPaddr() >> depCheckShift;
            Addr inst_eff_addr2 =
                (req0->getPaddr() + req0->getSize() - 1) >> depCheckShift;

            DPRINTF(LSQUnit, "Checking for violations for [sn:%lli], addr: %#lx\n",
                    inst->seqNum, req0->getPaddr());

            for (auto req1 : load_request->_reqs) {
                    if (!req1 || !req1->hasPaddr()) {
                        continue;
                    }
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
                        // Preserve the concrete RAW target for both MDP
                        // implementations. PHAST additionally consumes the
                        // SQ distance and path metadata captured at issue.
                        if (!ld_inst->memDepInfo.violationPending) {
                            ld_inst->memDepInfo.violatingStoreSeqNum =
                                inst->seqNum;
                            ld_inst->memDepInfo.violatingStorePC =
                                inst->pcState().instAddr();
                            ld_inst->memDepInfo.storeQueueDistance =
                                (ld_inst->sqIt - inst->sqIt) - 1;
                        }

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

} // namespace o3
} // namespace gem5
