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


#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/matrix_cpu_state.hh"
#include "debug/Commit.hh"
#include "matrix/CUTETOP.hh"
#include "matrix/MatrixRequestValidator.hh"

namespace gem5
{

namespace o3
{

namespace
{

const char *
requestKindName(matrix::CuteRequestKind kind)
{
    switch (kind) {
      case matrix::CuteRequestKind::Lsu:
        return "lsu";
      case matrix::CuteRequestKind::Mma:
        return "mma";
      case matrix::CuteRequestKind::Arith:
        return "arith";
      case matrix::CuteRequestKind::Release:
        return "release";
    }

    return "unknown";
}

} // anonymous namespace

const BaseISA::RegClasses &
CPU::matrixRegClasses(const BaseO3CPUParams &params)
{
    // Configure the architectural count before PhysRegFile and the rename
    // maps retain references to the ISA's register classes.
    if (!params.enableMatrixBackend) {
        for (auto *isa : params.isa) {
            static_cast<TheISA::ISA *>(isa)->useNonMatrixRegClasses();
        }
    }
    return params.isa[0]->regClasses();
}

void
CPU::initMatrix(const BaseO3CPUParams &params)
{
    enableMatrixBackend = params.enableMatrixBackend;
    enableMatrixMemPort = params.enableMatrixMemPort;
    if (!enableMatrixBackend && !enableMatrixMemPort) {
        return;
    }
    matrixState = std::make_unique<MatrixCpuState>();
    if (enableMatrixMemPort) {
        matrixState->memoryPort = std::make_unique<MatrixMemPort>(
            name() + ".matrix_mem_port", this, params.matrixRmwAcquire);
    }
    matrixState->shadowTokens.assign(
        numThreads,
        std::vector<RegVal>(TheISA::ISA::MatrixTokenCount, 0));
    matrixState->resetSeqs.assign(
        numThreads,
        std::vector<InstSeqNum>(TheISA::ISA::MatrixTokenCount, 0));
    if (enableMatrixBackend) {
        constexpr unsigned detailedCuteFifoDepth = 8;
        matrix::DetailedCuteBackend::TimingConfig timing_config;
        timing_config.matrixCZeroLoadLatencyCycles =
            params.matrixCZeroLoadLatency;
        auto detailed_backend = std::make_unique<matrix::DetailedCuteBackend>(
            detailedCuteFifoDepth,
            matrix::MatrixRegFile::DefaultAbRegCount,
            matrix::MatrixRegFile::DefaultCRegCount,
            timing_config,
            this);
        if (enableMatrixMemPort) {
            detailed_backend->setTimingMemoryAdapter(matrixState->memoryPort.get());
        }
        matrixState->backend = std::move(detailed_backend);
    }
}

void
CPU::commitMatrixResetToken(ThreadID tid, RegVal token_idx,
                            InstSeqNum seq_num)
{
    assert(tid < matrixState->shadowTokens.size());
    assert(token_idx < matrixState->shadowTokens[tid].size());
    DPRINTF(Commit,
            "Matrix reset token commit [tid:%i] [sn:%llu] token=%llu.\n",
            tid, seq_num, token_idx);
    matrixState->shadowTokens[tid][token_idx] = 0;
    matrixState->resetSeqs[tid][token_idx] = seq_num;
    isa[tid]->resetMatrixToken(token_idx);
}

void
CPU::releaseMatrixShadowToken(ThreadID tid, RegVal token_idx)
{
    assert(tid < matrixState->shadowTokens.size());
    assert(token_idx < matrixState->shadowTokens[tid].size());
    matrixState->shadowTokens[tid][token_idx]++;
}

bool
CPU::matrixShadowTokenReady(ThreadID tid, RegVal token_idx,
                            RegVal threshold) const
{
    assert(tid < matrixState->shadowTokens.size());
    assert(token_idx < matrixState->shadowTokens[tid].size());
    return matrixState->shadowTokens[tid][token_idx] >= threshold;
}

void
CPU::commitMatrixReleaseToken(ThreadID tid, RegVal token_idx,
                              InstSeqNum seq_num)
{
    assert(tid < matrixState->shadowTokens.size());
    assert(token_idx < matrixState->shadowTokens[tid].size());
    const auto reset_seq = matrixState->resetSeqs[tid][token_idx];
    if (reset_seq != 0 && seq_num <= reset_seq) {
        DPRINTF(Commit,
                "Matrix release ignored after newer reset [tid:%i] "
                "[sn:%llu] token=%llu resetSn=%llu.\n",
                tid, seq_num, token_idx, reset_seq);
        return;
    }
    DPRINTF(Commit,
            "Matrix release temporary token update [tid:%i] [sn:%llu] "
            "token=%llu value=%#x.\n",
            tid, seq_num, token_idx, matrixState->shadowTokens[tid][token_idx] + 1);
    matrixState->shadowTokens[tid][token_idx]++;
    isa[tid]->releaseMatrixToken(token_idx);
}

bool
CPU::canAcceptMatrixBackendReq(const matrix::CuteRequest &req,
                               InstSeqNum seq_num)
{
    if (!matrixState || !matrixState->backend) {
        panic("Matrix backend request [sn:%llu] reached CPU while "
              "enableMatrixBackend=false", seq_num);
    }

    return matrixState->backend->canAccept(req);
}

bool
CPU::submitMatrixBackendReq(ThreadID tid, const matrix::CuteRequest &req,
                            InstSeqNum seq_num)
{
    if (!matrixState || !matrixState->backend) {
        panic("Matrix backend request [sn:%llu] reached CPU while "
              "enableMatrixBackend=false", seq_num);
    }

    panic_if(matrix::validateMatrixRequest(req) !=
                 matrix::MatrixRequestError::None,
             "Unsupported Matrix backend request after commit "
             "[tid:%i] [sn:%llu] kind=%s op=%#llx",
             tid, seq_num, requestKindName(req.kind), req.op);

    if (!matrixState->backend->canAccept(req)) {
        return false;
    }

    const bool inserted = matrixState->owners.emplace(
        seq_num, MatrixCpuState::Owner{tid, req}).second;
    panic_if(!inserted,
             "Duplicate matrix backend owner [tid:%i] [sn:%llu]",
             tid, seq_num);
    auto submit_req = req;
    if (submit_req.kind == matrix::CuteRequestKind::Lsu) {
        submit_req.lsu.tc = tcBase(tid);
    }
    matrixState->backend->submit(submit_req);
    return true;
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

        if (completion.status == matrix::CuteCompletionStatus::Success &&
            completion.kind == matrix::CuteRequestKind::Release &&
            completion.hasTokenRelease) {
            commitMatrixReleaseToken(
                it->second.tid, completion.tokenIdx, completion.seq);
        }
        matrixState->owners.erase(it);
    }

    if (matrixState->backend->hasWork() || matrixState->backend->hasCompletion() ||
        !matrixState->owners.empty()) {
        activityRec.activity();
    }
}

void
CPU::consumeMatrixAmuProxy(ThreadID tid,
                           const o3::MatrixAmuBuffer::Entry &entry)
{
    const auto &backend_req = entry.backendReq;
    const auto seq_num = entry.seqNum;
    DPRINTF(Commit,
            "Matrix toAMU proxy fire [tid:%i] [sn:%llu] kind=%s.\n",
            tid, seq_num, requestKindName(backend_req.kind));

    const bool submitted = submitMatrixBackendReq(tid, backend_req, seq_num);
    panic_if(!submitted,
             "Matrix backend rejected %s request after toAMU proxy "
             "[tid:%i] [sn:%llu]",
             requestKindName(backend_req.kind), tid, seq_num);

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

void
CPU::syncMatrixTokens()
{
    if (!matrixState) {
        return;
    }
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        for (RegVal token = 0; token < TheISA::ISA::MatrixTokenCount; ++token) {
            matrixState->shadowTokens[tid][token] = isa[tid]->matrixTokenValue(token);
            matrixState->resetSeqs[tid][token] = 0;
        }
    }
}

} // namespace gem5::o3
