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

#include <utility>

#include "matrix/detailed_cute_backend.hh"

namespace gem5
{

namespace matrix
{

namespace
{

CuteCompletion
makeCompletion(uint64_t seq, CuteRequestKind kind, CuteCompletionStatus status)
{
    CuteCompletion completion;
    completion.seq = seq;
    completion.kind = kind;
    completion.status = status;
    return completion;
}

} // anonymous namespace

CuteCompletion
DetailedCuteBackend::executeLsu(uint64_t seq, const AmuLsuDesc &desc,
                                MatrixRegFile &state,
                                MatrixMemoryAdapter &memory)
{
    MatrixBankKind bank = MatrixBankKind::C;
    if (desc.isAcc) {
        bank = MatrixBankKind::C;
    } else if (desc.isA) {
        bank = MatrixBankKind::A;
    } else if (desc.isB) {
        bank = MatrixBankKind::B;
    } else {
        return makeCompletion(seq, CuteRequestKind::Lsu,
                              CuteCompletionStatus::Unsupported);
    }

    if (!desc.isStore) {
        MatrixTensor tensor;
        if (!memory.loadTile(desc, tensor)) {
            return makeCompletion(
            seq, CuteRequestKind::Lsu, CuteCompletionStatus::Unsupported);
        }
        state.write(bank, desc.ms, tensor);
        return makeCompletion(
            seq, CuteRequestKind::Lsu, CuteCompletionStatus::Success);
    }

    if (!state.hasRegister(bank, desc.ms)) {
        return makeCompletion(
            seq, CuteRequestKind::Lsu, CuteCompletionStatus::Unsupported);
    }

    const auto &tensor = state.read(bank, desc.ms);
    if (!memory.storeTile(desc, tensor)) {
        return makeCompletion(
            seq, CuteRequestKind::Lsu, CuteCompletionStatus::Unsupported);
    }

    return makeCompletion(
        seq, CuteRequestKind::Lsu, CuteCompletionStatus::Success);
}

// Active AML/BML/CML data path: load/store snapshots and memory budget.
bool
DetailedCuteBackend::useMemoryBudget()
{
    if (memoryBudget == 0) {
        return false;
    }
    --memoryBudget;
    return true;
}

void
DetailedCuteBackend::advanceLoadFill(TaskSlot &task)
{
    assert(task.entry.isLoad);

    if (!useMemoryBudget()) {
        return;
    }

    task.hasBufferedTensor = false;
    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Lsu,
        CuteCompletionStatus::Success);

    MatrixTensor tensor;
    if (!memory->loadTile(task.entry.request.lsu, tensor)) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
        return;
    }

    task.bufferedTensor = std::move(tensor);
    task.hasBufferedTensor = true;
}

void
DetailedCuteBackend::advanceStoreRead(TaskSlot &task)
{
    assert(task.entry.isStore);

    if (!useMemoryBudget()) {
        return;
    }

    task.hasBufferedTensor = false;
    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Lsu,
        CuteCompletionStatus::Success);

    if (!regFile.hasRegister(MatrixBankKind::C, task.entry.readRegs[0])) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    } else {
        task.bufferedTensor =
            regFile.read(MatrixBankKind::C, task.entry.readRegs[0]);
        task.hasBufferedTensor = true;
    }

    enqueueTaskEvent(task, TaskEventKind::ReadFinish);
}

} // namespace matrix
} // namespace gem5
