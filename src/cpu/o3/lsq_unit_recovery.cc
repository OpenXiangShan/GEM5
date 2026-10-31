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

        // A deferred MDP violation only applies while this dynamic load
        // remains on the architecturally valid path.
        loadQueue.back().instruction()->memDepInfo.violationPending = false;

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
        recordAddrOrDataDequeue(storeQueue.back().instruction());
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

}

uint64_t
LSQUnit::getLatestHtmUid() const
{
    const auto& htm_cpt = cpu->tcBase(lsqID)->getHtmCheckpointPtr();
    return htm_cpt->getHtmUid();
}

} // namespace o3
} // namespace gem5
