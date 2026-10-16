/*
 * Copyright (c) 2011-2012, 2014, 2017-2019, 2021 ARM Limited
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
 * Copyright (c) 2005-2006 The Regents of The University of Michigan
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

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/lsq.hh"
#include "debug/LSQ.hh"

namespace gem5
{

namespace o3
{

MlsUnit::IssueResult
LSQ::issueMatrixMem(const DynInstPtr &inst)
{
    ThreadID tid = inst->threadNumber;
    return thread[tid].issueMatrixMem(inst);
}

bool
LSQ::canAllocateMatrixMem(ThreadID tid, unsigned count) const
{
    return mlsVirtualQueue && mlsVirtualQueue->canAllocate(tid, count);
}

bool
LSQ::hasMatrixMemEntry(const DynInstPtr &inst) const
{
    return mlsVirtualQueue && mlsVirtualQueue->hasEntry(inst);
}

bool
LSQ::allocateMatrixMemEntry(const DynInstPtr &inst)
{
    return mlsVirtualQueue && mlsVirtualQueue->allocate(inst);
}

void
LSQ::scheduleMatrixMemReplay()
{
    if (!mlsReplayQueue) {
        return;
    }
    for (auto tid : *activeThreads) {
        mlsReplayQueue->refreshReady(
            tid,
            [this, tid](const DynInstPtr &inst,
                        MlsReplayQueue::ReplayState &state) {
                return thread[tid].matrixReplayReady(inst, state);
            });

        DynInstPtr replay_inst;
        if (!mlsReplayQueue->scheduleNext(tid, replay_inst)) {
            continue;
        }

        panic_if(!replay_inst || !replay_inst->issueQue,
                 "Matrix replay inst missing issue queue [tid:%i]", tid);
        replay_inst->issueQue->retryMem(replay_inst);
        DPRINTF(LSQ,
                "[tid:%i] Matrix replay scheduled [sn:%llu].\n",
                tid, replay_inst->seqNum);
    }
}

unsigned
LSQ::squashMatrixMem(ThreadID tid, InstSeqNum squash_seq)
{
    if (!mlsVirtualQueue) {
        return 0;
    }
    const unsigned mlsqCanceled = mlsVirtualQueue->squash(tid, squash_seq);
    if (mlsqCanceled != 0) {
        DPRINTF(LSQ,
                "[tid:%i] MlsVirtualQueue squash canceled=%u free=%u.\n",
            tid, mlsqCanceled, mlsVirtualQueue->freeEntries(tid));
    }

    const unsigned replayCanceled = mlsReplayQueue->squash(tid, squash_seq);
    if (replayCanceled != 0) {
        DPRINTF(LSQ,
                "[tid:%i] MlsReplayQueue squash canceled=%u free=%u.\n",
            tid, replayCanceled, mlsReplayQueue->freeEntries(tid));
    }

    return mlsqCanceled + replayCanceled;
}

unsigned
LSQ::retireCommittedMatrixMem(ThreadID tid, InstSeqNum committed_seq)
{
    if (!mlsVirtualQueue) {
        return 0;
    }
    const unsigned retired = mlsVirtualQueue->retireCommitted(tid, committed_seq);
    if (retired != 0) {
        DPRINTF(LSQ,
                "[tid:%i] MlsVirtualQueue retire count=%u free=%u.\n",
            tid, retired, mlsVirtualQueue->freeEntries(tid));
    }
    return retired;
}

MlsUnit::IssueResult
LSQUnit::issueMatrixMem(const DynInstPtr &inst)
{
    panic_if(!mlsUnit.has_value(),
             "Matrix MLS execution helper is not initialized [sn:%llu]",
             inst ? inst->seqNum : 0);
    return mlsUnit->issue(inst);
}

bool
LSQUnit::matrixReplayReady(
    const DynInstPtr &inst, MlsReplayQueue::ReplayState &state) const
{
    panic_if(!mlsUnit.has_value(),
             "Matrix MLS execution helper is not initialized for replay check");
    return mlsUnit->replayReady(inst, state);
}

} // namespace o3
} // namespace gem5
