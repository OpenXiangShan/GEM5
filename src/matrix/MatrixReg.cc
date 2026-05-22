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

#include <cassert>

#include "matrix/CUTETOP.hh"

namespace gem5
{

namespace matrix
{

namespace
{

CuteCompletion
execRelease(uint64_t seq, const AmuReleaseDesc &desc)
{
    CuteCompletion completion =
        makeCompletion(seq, CuteRequestKind::Release,
                       CuteCompletionStatus::Success);
    completion.hasTokenRelease = true;
    completion.tokenIdx = desc.tokenIndex;
    return completion;
}

} // anonymous namespace

CuteCompletion
DetailedCuteBackend::executeArith(uint64_t seq, const AmuArithDesc &desc,
                                  MatrixRegFile &state)
{
    switch (desc.op) {
      case MatrixArithOpcode::Zero:
        state.zero(desc.bank, desc.reg, desc.rows, desc.cols, desc.elemType);
        return makeCompletion(seq, CuteRequestKind::Arith,
                              CuteCompletionStatus::Success);
    }

    return makeCompletion(seq, CuteRequestKind::Arith,
                          CuteCompletionStatus::Unsupported);
}

// Active regfile helpers and backend writeback paths.
MatrixBankKind
DetailedCuteBackend::destBank(const DecodedFifoEntry &entry) const
{
    if (entry.isMma || entry.isZeroAcc) {
        return MatrixBankKind::C;
    }
    if (entry.isZeroTr) {
        return MatrixBankKind::A;
    }
    if (entry.isLoad) {
        if (entry.request.lsu.isAcc) {
            return MatrixBankKind::C;
        }
        return entry.request.lsu.isB ? MatrixBankKind::B : MatrixBankKind::A;
    }
    return MatrixBankKind::C;
}

CuteCompletion
DetailedCuteBackend::executeTaskSlot(const TaskSlot &task)
{
    const auto &entry = task.entry;
    if (entry.isZeroAcc || entry.isZeroTr) {
        return executeArith(entry.request.seq, entry.request.arith, regFile);
    }

    assert(entry.isRelease);
    return execRelease(entry.request.seq, entry.request.release);
}

CuteCompletion
DetailedCuteBackend::executeLoadWrite(TaskSlot &task)
{
    assert(task.entry.isLoad);

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return task.bufferedCompletion;
    }

    if (!task.hasBufferedTensor) {
        return makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Unsupported);
    }

    regFile.write(
        destBank(task.entry), task.entry.writeRegs[0], task.bufferedTensor);
    return makeCompletion(
        task.entry.request.seq, CuteRequestKind::Lsu,
        CuteCompletionStatus::Success);
}

std::optional<MatrixRegResource::Request>
DetailedCuteBackend::matrixRegWriteRequestForTask(
    const TaskSlot &task, const CuteCompletion &completion) const
{
    if (completion.status != CuteCompletionStatus::Success) {
        return std::nullopt;
    }

    if (task.entry.isZeroAcc || task.entry.isZeroTr) {
        return MatrixRegResource::makeWrite(
            destBank(task.entry), MatrixRegResource::Client::MemoryLoader,
            task.entry.writeRegs[0]);
    }

    return std::nullopt;
}

CuteCompletion
DetailedCuteBackend::executeStoreWrite(const TaskSlot &task)
{
    assert(task.entry.isStore);

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return task.bufferedCompletion;
    }

    if (!task.hasBufferedTensor) {
        return makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Unsupported);
    }

    if (!memory->storeTile(task.entry.request.lsu, task.bufferedTensor)) {
        return makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Unsupported);
    }

    return makeCompletion(
        task.entry.request.seq, CuteRequestKind::Lsu,
        CuteCompletionStatus::Success);
}

CuteCompletion
DetailedCuteBackend::executeComputeWrite(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return task.bufferedCompletion;
    }

    if (!task.hasBufferedTensor) {
        return makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
    }

    regFile.write(
        MatrixBankKind::C, task.entry.writeRegs[0], task.bufferedTensor);
    return makeCompletion(
        task.entry.request.seq, CuteRequestKind::Mma,
        CuteCompletionStatus::Success);
}

} // namespace matrix
} // namespace gem5
