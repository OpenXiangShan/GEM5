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

bool
LSQUnit::storeQueueWriteReady(const DynInstPtr &inst) const
{
    assert(inst);
    if (!virtualSQEnabled) {
        return true;
    }

    assert(inst->sqIdx >= 0);
    const size_t store_idx = static_cast<size_t>(inst->sqIdx);
    assert(storeQueue.isValidIdx(store_idx));
    assert(store_idx >= storeQueue.head());
    assert(physicalSQEntries <= storeQueue.capacity());

    return store_idx - storeQueue.head() < physicalSQEntries;
}

void
LSQUnit::recordAddrOrDataReady(const DynInstPtr &inst)
{
    assert(inst);
    assert(inst->sqIdx >= 0);
    auto &entry = storeQueue[inst->sqIdx];
    assert(entry.valid());
    assert(entry.addrReady() || entry.dataReady());

    if (!entry.addrOrDataReadyCounted()) {
        entry.addrOrDataReadyCounted(true);
        ++addrOrDataReadyNums;
        assert(addrOrDataReadyNums <= physicalSQEntries);
        DPRINTF(StorePipeline,
                "SQ addr/data ready count increments to %u: [sn:%llu] "
                "sqIdx=%llu physical=%u\n",
                addrOrDataReadyNums, inst->seqNum,
                static_cast<unsigned long long>(inst->sqIdx),
                physicalSQEntries);
    }
}

void
LSQUnit::recordAddrOrDataDequeue(const DynInstPtr &inst)
{
    assert(inst);
    assert(inst->sqIdx >= 0);
    auto &entry = storeQueue[inst->sqIdx];
    assert(entry.valid());

    if (entry.addrOrDataReadyCounted()) {
        assert(addrOrDataReadyNums > 0);
        entry.addrOrDataReadyCounted(false);
        --addrOrDataReadyNums;
        DPRINTF(StorePipeline,
                "SQ addr/data ready count decrements to %u: [sn:%llu] "
                "sqIdx=%llu\n",
                addrOrDataReadyNums, inst->seqNum,
                static_cast<unsigned long long>(inst->sqIdx));
    }
}

void
LSQUnit::recordPhysicalSQReplayBlocked(const DynInstPtr &inst)
{
    assert(!storeQueueWriteReady(inst));
    ++stats.storePhysicalSQReplayBlocked;
    DPRINTF(StorePipeline,
            "Store [sn:%llu] waits before replay: sqIdx=%llu head=%llu "
            "physical=%u\n",
            inst->seqNum, static_cast<unsigned long long>(inst->sqIdx),
            static_cast<unsigned long long>(storeQueue.head()),
            physicalSQEntries);
}

void
LSQUnit::recordStoreQueueReplay(const DynInstPtr &inst)
{
    ++stats.storeReplayPhysicalSQFull;
    DPRINTF(StorePipeline,
            "Store [sn:%llu] replays: sqIdx=%llu head=%llu physical=%u\n",
            inst->seqNum, static_cast<unsigned long long>(inst->sqIdx),
            static_cast<unsigned long long>(storeQueue.head()),
            physicalSQEntries);
}

void
LSQUnit::commitLoad()
{
    assert(loadQueue.front().valid());

    DynInstPtr inst = loadQueue.front().instruction();

    DPRINTF(LSQUnit, "Committing head load instruction, PC %s, [sn:%lu]\n",
            inst->pcState(), inst->seqNum);

    if (inst->memDepInfo.predicted) {
        iewStage->instQueue.commit(inst);
    }

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
