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
LSQUnit::issueToLoadPipe(const DynInstPtr &inst)
{
    // S0 is shared by all load issue sources. First issues, slow replays, and
    // fast replays all enter here; LoadPipeSource records which path fed the
    // pipe for RTL-aligned stats.
    panic_if(loadPipeSx[0]->size >= loadPipeCount,
             "load pipe S0 exceeds configured pipe count (%u)\n",
             loadPipeCount);
    panic_if(inst->inPipe(), "load [sn:%llu] is already in pipeline", inst->seqNum);
    inst->beginPipelining();
    inst->setSkipRawCheck();

    loadPipeSx[0]->insts.push_back(inst);
    loadPipeSx[0]->size = static_cast<int>(loadPipeSx[0]->insts.size());
    // Order S0 by age so older loads have higher priority when S1 performs
    // the bank-conflict check.
    std::stable_sort(
        loadPipeSx[0]->insts.begin(),
        loadPipeSx[0]->insts.end(),
        [](const DynInstPtr &lhs, const DynInstPtr &rhs) {
            return lhs->seqNum < rhs->seqNum;
        });

    const int load_pipe_id = inst->issueQue->getLoadPipeId();
    panic_if(load_pipe_id < 0 ||
                 static_cast<unsigned>(load_pipe_id) >= loadPipeCount,
        "Per-load-pipe PMU stats require a scheduler-assigned load pipe ID; "
        "unsupported or out-of-range issue queue %s for load [sn:%llu] "
        "(id=%d, count=%u)\n",
        inst->issueQue->getName().c_str(), inst->seqNum, load_pipe_id,
        loadPipeCount);
    stats.loadPipeAccepted[load_pipe_id]++;
    const auto load_pipe_source = inst->getLoadPipeSource();
    if (load_pipe_source == DynInst::LoadPipeSource::ReplayQueue) {
        stats.loadPipeReplayAccepted[load_pipe_id]++;
    } else if (load_pipe_source == DynInst::LoadPipeSource::FastReplay) {
        stats.loadPipeFastReplayAccepted[load_pipe_id]++;
    }

    DPRINTF(LoadPipeline, "issueToLoadPipe: [sn:%llu]\n", inst->seqNum);
}

Fault
LSQUnit::loadDoTranslate(const DynInstPtr &inst)
{
    DPRINTF(LoadPipeline, "loadDoTranslate: load [sn:%llu]\n", inst->seqNum);
    assert(!inst->isSquashed());

    Fault load_fault = NoFault;
    // Now initiateAcc only does TLB access
    load_fault = inst->initiateAcc();

    if (inst->isTranslationDelayed() && load_fault == NoFault) {
        inst->setTLBMissReplay();
        if (cpu->getTea()) {
            inst->teaSetEvent(TeaEvent::ST_TLB);
        }
        DPRINTF(LoadPipeline, "Load [sn:%llu] setTLBMissReplay\n", inst->seqNum);
    }

    if (auto *request = currentLoadRequest(inst);
        request && request->isTranslationComplete()) {
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

    const bool cacheMiss = lsq->enableLdMissReplay() && request &&
        request->isNormalLd() && !inst->fullForward() && !inst->cacheHit();
    const bool cacheMissReplay = earlyWakeupCacheMissReplay || cacheMiss;
    // Record the observed miss even if another replay reason wins priority.
    // An early wakeup without data alone does not establish an L1 miss.
    if (cpu->getTea() && cacheMiss) {
        inst->teaSetEvent(TeaEvent::ST_L1);
    }
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
                else if (inst->needTLBMissReplay()) iewStage->deferMemInst(inst);


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

} // namespace o3
} // namespace gem5
