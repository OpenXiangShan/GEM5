/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#include "cpu/o3/free_list.hh"

#include <algorithm>
#include <list>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/FreeList.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

UnifiedFreeList::UnifiedFreeList(const std::string &_my_name,
                                 PhysRegFile *_regFile,
                                 const BaseO3CPUParams &params)
    : _name(_my_name), regFile(_regFile),
      pregPolicy(params.smtPregPolicy),
      donorReservePercent(params.smtPregDonorReservePercent),
      fixedBase(params.smtPregFixedBase),
      numThreads(params.numThreads)
{
    DPRINTF(FreeList, "Creating new free list object.\n");

    panic_if(pregPolicy == SMTQueuePolicy::Threshold,
             "smtPregPolicy=Threshold is not implemented for the Preg free "
             "list; use Partitioned or DynamicBorrowing instead.");

    // Have the register file initialize the free list since it knows
    // about its internal organization
    regFile->initFreeList(this);

    // Capture each class' total capacity now, before any allocation
    // happens; freeLists[i] has just been fully populated by initFreeList.
    for (int i = 0; i <= RMiscRegClass; i++)
        numPhysRegs[i] = freeLists[i].numFreeRegs();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        donor[tid] = false;
        for (int i = 0; i <= RMiscRegClass; i++) {
            maxEntries[tid][i] = (pregPolicy == SMTQueuePolicy::Partitioned)
                ? numPhysRegs[i] / numThreads
                : numPhysRegs[i];
        }
    }
    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        for (int i = 0; i <= RMiscRegClass; i++)
            maxEntries[tid][i] = 0;
    }
}

void
UnifiedFreeList::resetEntries()
{
    if (pregPolicy != SMTQueuePolicy::Partitioned || !activeThreads) {
        return;
    }

    const unsigned active = std::max<size_t>(1, activeThreads->size());
    for (ThreadID tid : *activeThreads) {
        for (int i = 0; i <= RMiscRegClass; i++)
            maxEntries[tid][i] = numPhysRegs[i] / active;
    }
}

unsigned
UnifiedFreeList::activeThreadCount() const
{
    if (!activeThreads || activeThreads->empty())
        return numThreads == 0 ? 1 : numThreads;
    return activeThreads->size();
}

unsigned
UnifiedFreeList::base(RegClassType type) const
{
    if (fixedBase > 0)
        return std::min(fixedBase, numPhysRegs[type] / activeThreadCount());
    return std::max(1u, numPhysRegs[type] / activeThreadCount());
}

unsigned
UnifiedFreeList::donorQuota(RegClassType type) const
{
    unsigned fairShare = numPhysRegs[type] / activeThreadCount();
    return fairShare * donorReservePercent / 100;
}

unsigned
UnifiedFreeList::borrowingLimit(RegClassType type, ThreadID tid) const
{
    unsigned reserved = 0;
    if (activeThreads && !activeThreads->empty()) {
        for (ThreadID other : *activeThreads) {
            if (other == tid)
                continue;
            const unsigned reserve =
                donor[other] ? donorQuota(type) : base(type);
            reserved += std::max(threadUsed[other][type], reserve);
        }
    }
    if (reserved >= numPhysRegs[type])
        return 0;
    return numPhysRegs[type] - reserved;
}

bool
UnifiedFreeList::canAllocate(RegClassType type, ThreadID tid, unsigned n) const
{
    if (n > freeLists[type].numFreeRegs()) {
        return false;
    }

    switch (pregPolicy) {
      case SMTQueuePolicy::DynamicBorrowing:
        return threadUsed[tid][type] + n <= borrowingLimit(type, tid);
      case SMTQueuePolicy::Partitioned:
        return threadUsed[tid][type] + n <= maxEntries[tid][type];
      default:
        // Dynamic: no per-thread cap beyond physical availability, which
        // is already checked above.
        return true;
    }
}

unsigned
UnifiedFreeList::numAllocatable(RegClassType type, ThreadID tid) const
{
    const unsigned free_regs = freeLists[type].numFreeRegs();

    if (pregPolicy == SMTQueuePolicy::Dynamic) {
        return free_regs;
    }

    const unsigned limit = (pregPolicy == SMTQueuePolicy::DynamicBorrowing)
        ? borrowingLimit(type, tid)
        : maxEntries[tid][type];
    const unsigned used = threadUsed[tid][type];
    const unsigned per_thread_limit = used >= limit ? 0 : limit - used;

    return std::min(per_thread_limit, free_regs);
}

bool
UnifiedFreeList::isPerThreadExhausted(RegClassType type, ThreadID tid,
                                       unsigned n) const
{
    if (pregPolicy == SMTQueuePolicy::Dynamic) {
        return false;
    }

    const unsigned free_regs = freeLists[type].numFreeRegs();
    if (free_regs < n) {
        return false;
    }

    const unsigned limit = (pregPolicy == SMTQueuePolicy::DynamicBorrowing)
        ? borrowingLimit(type, tid)
        : maxEntries[tid][type];
    const unsigned used = threadUsed[tid][type];
    const unsigned per_thread_avail = used >= limit ? 0 : limit - used;

    return per_thread_avail < n;
}

} // namespace o3
} // namespace gem5
