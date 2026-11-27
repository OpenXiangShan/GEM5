/*
 * Copyright (c) 2010-2011, 2021 ARM Limited
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
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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

/*
 * Copyright (c) 2012 ARM Limited
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

/*
 * Copyright (c) 2026
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

#include "cpu/o3/matrix_pipeline.hh"

#include <algorithm>
#include <cassert>
#include <utility>

#include "arch/riscv/faults.hh"
#include "arch/riscv/matrix.hh"
#include "base/logging.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/rob.hh"
#include "debug/Commit.hh"
#include "matrix/MatrixTypes.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

void
DynInst::initMatrixInstInfo()
{
    if (!cpu->isMatrixBackendEnabled()) {
        return;
    }
    if (const auto *inst = dynamic_cast<const RiscvISA::MatrixStaticInst *>(
            staticInst.get())) {
        matrixState.matrixInst = inst->matrixStaticInfo();
    } else if (const auto *inst = dynamic_cast<const RiscvISA::MatrixCSROp *>(
                   staticInst.get())) {
        matrixState.matrixInst = inst->matrixStaticInfo();
    }
}

void
DynInst::freezeMatrixConfig()
{
    if (matrixState.configFrozen) {
        return;
    }
    for (unsigned i = 0; i < matrixState.config.size(); ++i) {
        matrixState.config[i] = readMiscReg(RiscvISA::MISCREG_MCFG0 + i);
    }
    matrixState.configFrozen = true;
}

RegVal
DynInst::readMatrixConfig(unsigned index)
{
    panic_if(index >= 8, "Invalid internal Matrix config index %u", index);
    freezeMatrixConfig();
    return matrixState.config[index];
}

bool
DynInst::stageMatrixRequest(const matrix::CuteRequest &request)
{
    matrixState.request = request;
    matrixState.request->seq = seqNum;
    matrixState.request->tid = threadNumber;
    matrixState.request->pc = pcState().instAddr();
    return true;
}

bool
DynInst::stageMatrixTokenReset(RegVal token_idx)
{
    matrixState.stagedMatrixTokenReset = token_idx;
    return true;
}

bool
DynInst::readStagedMatrixTokenReady(RegVal token_idx, RegVal threshold,
                                  bool &ready)
{
    ready = cpu->matrixTokenReady(threadNumber, token_idx, threshold);
    return true;
}

bool
ROB::canAllocateMatrixAmu(ThreadID tid,
    const boost::circular_buffer<DynInstPtr> &insts, unsigned rename_width)
{
    if (!matrixAmuBuffers) {
        return true;
    }
    // The AMU tracks individual instructions, including members of a ROB group.
    const unsigned count = std::min<unsigned>(insts.size(), rename_width);
    unsigned demand = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto &inst = insts[i];
        if (shouldTrackMatrixAmu(inst) && !inst->isSquashed()) {
            ++demand;
        }
    }
    const unsigned available = matrixAmuBuffers[tid].numFreeEntries();
    DPRINTF(Commit, "Thread %i: AMU shadowFree %u shadowDemand %u\n",
            tid, available, demand);
    return available >= demand;
}

void
ROB::issueMatrixAmu()
{
    if (cpu->isMatrixBackendEnabled()) {
        for (ThreadID tid : *activeThreads) {
            if (cpu->tryIssueMatrixAmu(tid, matrixAmuBuffers[tid])) {
                break;
            }
        }
    }
}

bool
ROB::shouldTrackMatrixAmu(const DynInstPtr &inst) const
{
    return cpu->isMatrixBackendEnabled() && inst && inst->isMatrixInst() &&
           inst->matrixData().matrixNeedAmuCtrl();
}

void
ROB::insertMatrixAmuEntry(const DynInstPtr &inst)
{
    if (!shouldTrackMatrixAmu(inst)) {
        return;
    }

    matrixAmuBuffers[inst->threadNumber].allocate(inst->seqNum);
}

void
ROB::noteMatrixAmuWriteback(const DynInstPtr &inst)
{
    if (!shouldTrackMatrixAmu(inst)) {
        return;
    }

    const auto &request = inst->matrixData().request;
    if (inst->getFault() == NoFault && request &&
        matrix::validateMatrixRequest(*request) !=
            matrix::MatrixRequestError::None) {
        const auto *riscv_inst = dynamic_cast<const RiscvISA::RiscvStaticInst *>(
            inst->staticInst.get());
        panic_if(!riscv_inst, "Matrix request lacks RISC-V static inst");
        inst->getFault() = std::make_shared<RiscvISA::IllegalInstFault>(
            "unsupported matrix request", riscv_inst->machInst);
    }

    matrixAmuBuffers[inst->threadNumber].noteWriteback(
        inst->seqNum, inst->getFault() == NoFault && request ? &*request :
                                                             nullptr);
}

void
ROB::noteMatrixAmuCommit(const DynInstPtr &inst)
{
    if (!shouldTrackMatrixAmu(inst)) {
        return;
    }

    matrixAmuBuffers[inst->threadNumber].noteCommit(
        inst->seqNum);
}

void
ROB::squashMatrixAmuEntry(ThreadID tid, InstSeqNum seq_num)
{
    if (!cpu->isMatrixBackendEnabled()) {
        return;
    }
    matrixAmuBuffers[tid].squash(seq_num);
}

MatrixAmuBuffer::MatrixAmuBuffer(unsigned capacity)
    : capacity_(capacity)
{
}

void
MatrixAmuBuffer::reset(unsigned capacity)
{
    capacity_ = capacity;
    entries_.clear();
}

MatrixAmuBuffer::Entry *
MatrixAmuBuffer::find(InstSeqNum seq_num)
{
    for (auto &entry : entries_) {
        if (entry.seqNum == seq_num) {
            return &entry;
        }
    }

    return nullptr;
}

void
MatrixAmuBuffer::cleanupFront()
{
    while (!entries_.empty() &&
           entries_.front().state == Entry::State::Retired) {
        entries_.pop_front();
    }
}

void
MatrixAmuBuffer::allocate(InstSeqNum seq_num)
{
    panic_if(capacity_ == 0,
             "Matrix AMU buffer capacity is zero at alloc [sn:%llu]",
             seq_num);
    panic_if(entries_.size() >= capacity_,
             "Matrix AMU buffer overflow at alloc [sn:%llu] used=%llu "
             "capacity=%u",
             seq_num, static_cast<unsigned long long>(entries_.size()),
             capacity_);
    entries_.emplace_back();
    entries_.back().seqNum = seq_num;
}

void
MatrixAmuBuffer::noteWriteback(InstSeqNum seq_num,
                              const matrix::CuteRequest *backend_req)
{
    auto *entry = find(seq_num);
    panic_if(!entry, "Matrix AMU entry missing at writeback [sn:%llu]",
             seq_num);

    if (entry->state == Entry::State::NoRequest) {
        return;
    }

    assert(entry->state == Entry::State::Allocated ||
           entry->state == Entry::State::WrittenBack);
    entry->state = backend_req ? Entry::State::WrittenBack :
                                 Entry::State::NoRequest;
    if (backend_req) {
        entry->backendReq = *backend_req;
    }
}

void
MatrixAmuBuffer::noteCommit(InstSeqNum seq_num)
{
    auto *entry = find(seq_num);
    panic_if(!entry, "Matrix AMU entry missing at commit [sn:%llu]",
             seq_num);
    assert(entry->state == Entry::State::WrittenBack ||
           entry->state == Entry::State::NoRequest);
    entry->state = entry->state == Entry::State::NoRequest ?
        Entry::State::Retired : Entry::State::Ready;
    cleanupFront();
}

const MatrixAmuBuffer::Entry *
MatrixAmuBuffer::peekReady()
{
    cleanupFront();

    // In-order commit makes the oldest live entry the only fire candidate.
    return !entries_.empty() && entries_.front().state == Entry::State::Ready ?
        &entries_.front() : nullptr;
}

MatrixAmuBuffer::Entry
MatrixAmuBuffer::popReady()
{
    assert(peekReady());
    auto entry = std::move(entries_.front());
    entries_.pop_front();
    cleanupFront();
    return entry;
}

void
MatrixAmuBuffer::squash(InstSeqNum seq_num)
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->seqNum == seq_num) {
            entries_.erase(it);
            cleanupFront();
            return;
        }
    }
}

unsigned
MatrixAmuBuffer::numFreeEntries()
{
    cleanupFront();
    if (capacity_ <= entries_.size()) {
        return 0;
    }
    return capacity_ - entries_.size();
}

} // namespace o3
} // namespace gem5
