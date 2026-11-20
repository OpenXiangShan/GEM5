/*
 * Copyright (c) 2011-2012, 2014, 2016, 2017, 2019-2020 ARM Limited
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
 * Copyright (c) 2011 Regents of the University of California
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

#include <cassert>

#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/matrix_cpu_state.hh"
#include "debug/Commit.hh"
#include "matrix/CUTETOP.hh"
#include "matrix/MatrixTypes.hh"
#include "sim/full_system.hh"

namespace gem5
{

namespace o3
{

void
CPU::checkMatrixCheckpoint(ThreadID tid) const
{
    if (tid == 0 && matrixHasLiveState()) {
        panic("Serializing O3 CPU with live matrix register state is "
              "unsupported; checkpoint would lose backend register contents.");
    }

    if (tid == 0 && matrixHasPendingWork()) {
        panic("Serializing O3 CPU with pending or live matrix backend state "
              "is unsupported; drain/checkpoint must not lose matrix "
              "register contents.");
    }
}

void
CPU::checkMatrixDrain() const
{
    if (matrixHasLiveState()) {
        panic("Draining O3 CPU with live matrix register state is unsupported; "
              "checkpoint would lose backend register contents.");
    }
}

void
CPU::checkMatrixSwitchOut() const
{
    panic_if(matrixHasLiveState() || matrixHasPendingWork(),
             "Switching O3 CPU with pending or live Matrix state is "
             "unsupported; CPU switch would lose backend state.");
}

void
CPU::commitMatrixResetToken(const DynInstPtr &inst)
{
    if (const auto &token = inst->matrixData().stagedMatrixTokenReset) {
        commitMatrixResetToken(inst->threadNumber, *token, inst->seqNum);
    }
}

MatrixCpuState::MatrixCpuState() = default;
MatrixCpuState::~MatrixCpuState() = default;

const BaseISA::RegClasses &
CPU::matrixRegClasses(const BaseO3CPUParams &params)
{
    // Configure the architectural count before PhysRegFile and the rename
    // maps retain references to the ISA's register classes.
    for (auto *isa : params.isa) {
        static_cast<TheISA::ISA *>(isa)->setMatrixEnabled(
            FullSystem && params.enableMatrixBackend);
    }
    return params.isa[0]->regClasses();
}

void
CPU::initMatrix(const BaseO3CPUParams &params)
{
    enableMatrixBackend = params.enableMatrixBackend;
    if (!enableMatrixBackend) {
        return;
    }
    fatal_if(!FullSystem || params.numThreads != 1,
             "Matrix requires full-system mode and one hardware thread");
    matrixState = std::make_unique<MatrixCpuState>();
    constexpr unsigned detailedCuteFifoDepth = 8;
    matrix::DetailedCuteBackend::TimingConfig timing_config;
    timing_config.matrixCZeroLoadLatencyCycles =
        params.matrixCZeroLoadLatency;
    matrixState->backend = std::make_unique<matrix::DetailedCuteBackend>(
        detailedCuteFifoDepth,
        matrix::MatrixRegFile::DefaultAbRegCount,
        matrix::MatrixRegFile::DefaultCRegCount,
        timing_config, this);
}

void
CPU::commitMatrixResetToken(ThreadID tid, RegVal token_idx,
                            InstSeqNum seq_num)
{
    assert(token_idx < matrixState->resetSeqs.size());
    DPRINTF(Commit,
            "Matrix reset token commit [tid:%i] [sn:%llu] token=%llu.\n",
            tid, seq_num, token_idx);
    matrixState->resetSeqs[token_idx] = seq_num;
    isa[tid]->resetMatrixToken(token_idx);
}

bool
CPU::matrixTokenReady(ThreadID tid, RegVal token, RegVal threshold) const
{
    return isa[tid]->matrixTokenReady(token, threshold);
}

void
CPU::commitMatrixReleaseToken(ThreadID tid, RegVal token_idx,
                              InstSeqNum seq_num)
{
    assert(token_idx < matrixState->resetSeqs.size());
    const auto reset_seq = matrixState->resetSeqs[token_idx];
    if (reset_seq != 0 && seq_num <= reset_seq) {
        DPRINTF(Commit,
                "Matrix release ignored after newer reset [tid:%i] "
                "[sn:%llu] token=%llu resetSn=%llu.\n",
                tid, seq_num, token_idx, reset_seq);
        return;
    }
    isa[tid]->releaseMatrixToken(token_idx);
    DPRINTF(Commit, "Matrix release [tid:%i] [sn:%llu] token=%llu.\n",
            tid, seq_num, token_idx);
}

void
CPU::serviceMatrixBackend()
{
    if (!matrixState || !matrixState->backend) {
        return;
    }

    if (matrixState->backend->hasWork()) {
        matrixState->backend->step();
    }

    while (matrixState->backend->hasCompletion()) {
        auto completion = matrixState->backend->popCompletion();
        auto it = matrixState->owners.find(completion.seq);
        panic_if(it == matrixState->owners.end(),
                 "Matrix backend completion missing owner [sn:%llu]",
                 completion.seq);

        const auto &request = it->second.request;
        panic_if(completion.status != matrix::CuteCompletionStatus::Success,
                 "Unexpected Matrix backend Unsupported completion "
                 "[tid:%i] [sn:%llu] pc=%#llx kind=%s op=%#llx "
                 "M/N/K=%u/%u/%u lhs/rhs/dst=%u/%u/%u LSU=%ux%u type=%u",
                 it->second.tid, completion.seq, request.pc,
                 requestKindName(request.kind), request.op,
                 request.mma.mtilem, request.mma.mtilen, request.mma.mtilek,
                 unsigned(request.mma.lhsElemType),
                 unsigned(request.mma.rhsElemType),
                 unsigned(request.mma.dstElemType),
                 request.lsu.row, request.lsu.column,
                 unsigned(request.lsu.elemType));

        panic_if(completion.tid != it->second.tid,
                 "Matrix completion thread mismatch [sn:%llu]", completion.seq);
        panic_if(it->second.completion.has_value(),
                 "Duplicate Matrix completion [sn:%llu]", completion.seq);
        if (completion.kind == matrix::CuteRequestKind::Release) {
            for (const auto &[older_seq, older] : matrixState->owners) {
                panic_if(older_seq < completion.seq &&
                         older.request.kind == matrix::CuteRequestKind::Lsu &&
                         older.request.lsu.isStore && !older.completion,
                         "Matrix release completed before older store "
                         "[tid:%u] [sn:%llu] storeSn=%llu", it->second.tid,
                         completion.seq, older_seq);
            }
            commitMatrixReleaseToken(it->second.tid, completion.tokenIdx,
                                     completion.seq);
        }
        it->second.completion = std::move(completion);
    }
    checkMatrixResults();

    if (matrixState->backend->hasWork() || matrixState->backend->hasCompletion() ||
        !matrixState->owners.empty()) {
        activityRec.activity();
    }
}

bool
CPU::tryIssueMatrixAmu(ThreadID tid, MatrixAmuBuffer &buffer)
{
    const auto *front = buffer.peekReady();
    if (!front) {
        return false;
    }
    const auto seq_num = front->seqNum;
    panic_if(!matrixState || !matrixState->backend,
             "Matrix backend request [sn:%llu] reached CPU while "
             "enableMatrixBackend=false", seq_num);
    if (!matrixState->backend->canAccept(front->backendReq)) {
        DPRINTF(Commit,
                "Matrix toAMU proxy blocked [tid:%i] [sn:%llu] "
                "kind=%s backend cannot accept yet.\n",
                tid, seq_num, requestKindName(front->backendReq.kind));
        return false;
    }

    auto entry = buffer.popReady();
    const auto &backend_req = entry.backendReq;
    DPRINTF(Commit,
            "Matrix toAMU proxy fire [tid:%i] [sn:%llu] kind=%s.\n",
            tid, seq_num, requestKindName(backend_req.kind));

    panic_if(matrix::validateMatrixRequest(backend_req) !=
                 matrix::MatrixRequestError::None,
             "Unsupported Matrix backend request after commit "
             "[tid:%i] [sn:%llu] kind=%s op=%#llx",
             tid, seq_num, requestKindName(backend_req.kind), backend_req.op);

    const bool inserted = matrixState->owners.emplace(
        seq_num, MatrixCpuState::Owner{tid, backend_req}).second;
    panic_if(!inserted,
             "Duplicate matrix backend owner [tid:%i] [sn:%llu]",
             tid, seq_num);
    matrixState->backend->submit(backend_req);
    matrixState->checkOrder.push_back(seq_num);

    if (backend_req.kind == matrix::CuteRequestKind::Release) {
        DPRINTF(Commit,
                "Matrix release backend-visible after toAMU proxy "
                "[tid:%i] [sn:%llu] token=%llu.\n",
                tid, seq_num, backend_req.release.tokenIndex);
    } else if (backend_req.kind == matrix::CuteRequestKind::Lsu) {
        DPRINTF(Commit,
                "Matrix LSU backend-visible after toAMU proxy "
                "[tid:%i] [sn:%llu] load=%d store=%d base=%#llx "
                "stride=%#llx row=%#llx column=%#llx.\n",
                tid, seq_num, !backend_req.lsu.isStore,
                backend_req.lsu.isStore,
                static_cast<unsigned long long>(backend_req.lsu.baseAddr),
                static_cast<unsigned long long>(backend_req.lsu.stride),
                static_cast<unsigned long long>(backend_req.lsu.row),
                static_cast<unsigned long long>(backend_req.lsu.column));
    }
    return true;
}

} // namespace o3
} // namespace gem5

namespace gem5::o3
{

bool
CPU::matrixHasLiveState() const
{
    return matrixState && matrixState->backend &&
           matrixState->backend->hasArchitecturalState();
}

bool
CPU::matrixHasPendingWork() const
{
    return matrixState && matrixState->backend &&
           (matrixState->backend->hasWork() ||
            matrixState->backend->hasCompletion() || !matrixState->owners.empty());
}

} // namespace gem5::o3

namespace gem5::o3
{

void
CPU::checkMatrixResults()
{
    auto &state = *matrixState;
    while (!state.checkOrder.empty()) {
        const auto seq = state.checkOrder.front();
        auto it = state.owners.find(seq);
        panic_if(it == state.owners.end(), "Missing Matrix check owner");
        auto &owner = it->second;
        if (!owner.completion) {
            break;
        }
        state.owners.erase(it);
        state.checkOrder.pop_front();
    }
}

} // namespace gem5::o3
