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
LSQUnit::storeSetReplay(const DynInstPtr& inst, LSQRequest* request)
{
    inst->effAddrValid(false);
    inst->translationStarted(false);
    inst->translationCompleted(false);
    inst->savedRequest = nullptr;
    storeQueue[inst->sqIdx].setRequest(nullptr);
    if (inst->memData) {
        delete [] inst->memData;
        inst->memData = nullptr;
    }
    if (request) {
        request->discard();
    }

    DPRINTF(StorePipeline,
            "Store [sn:%llu] resets translation/request for physical SQ replay\n",
            inst->seqNum);
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
