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

#include "base/trace.hh"
#include "debug/MatrixCuteTrace.hh"
#include "matrix/CUTETOP.hh"

namespace gem5
{

namespace matrix
{

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

unsigned
DetailedCuteBackend::lsuBeatCount(const AmuLsuDesc &desc) const
{
    const size_t bytes = lsuPayloadByteCount(desc);
    return bytes == 0 ? 0 : static_cast<unsigned>((bytes + 63) / 64);
}

LocalMmuModel::Client
DetailedCuteBackend::localMmuClient(const TaskSlot &task) const
{
    switch (task.microTaskKind) {
      case MicroTaskKind::AML:
        return LocalMmuModel::Client::AML;
      case MicroTaskKind::BML:
        return LocalMmuModel::Client::BML;
      case MicroTaskKind::CML:
        return LocalMmuModel::Client::CML;
      case MicroTaskKind::Compute:
      case MicroTaskKind::Release:
      case MicroTaskKind::Count:
        break;
    }

    return LocalMmuModel::Client::CML;
}

bool
DetailedCuteBackend::enqueueLocalMmuBeats(TaskSlot &task)
{
    if (task.lsuBeatsEnqueued) {
        return true;
    }

    const auto &desc = task.entry.request.lsu;
    const unsigned beats = lsuBeatCount(desc);
    if (beats == 0) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Unsupported);
        task.lsuBeatsEnqueued = true;
        return false;
    }

    const size_t payload_bytes = lsuPayloadByteCount(desc);
    for (unsigned beat = 0; beat < beats; ++beat) {
        const size_t offset = static_cast<size_t>(beat) * 64;
        const LocalMmuModel::Request request{
            task.entry.request.seq,
            localMmuClient(task),
            task.entry.isStore,
            beat,
            static_cast<uint32_t>(std::min<size_t>(64, payload_bytes - offset))
        };
        if (!localMmu.enqueue(request)) {
            task.bufferedCompletion = makeCompletion(
                task.entry.request.seq, CuteRequestKind::Lsu,
                CuteCompletionStatus::Unsupported);
            task.lsuBeatsEnqueued = true;
            return false;
        }
        if (task.entry.isStore) {
            ++counters.localMmuStoreBeatsEnqueued;
        } else {
            ++counters.localMmuLoadBeatsEnqueued;
        }
        DPRINTF(MatrixCuteTrace,
                "local_mmu_enqueue [sn:%llu] unit=%u store=%u "
                "beat=%u/%u bytes=%u pending=%llu outstanding=%llu "
                "step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(task.microTaskKind),
                task.entry.isStore ? 1 : 0,
                beat,
                beats,
                request.byteSize,
                static_cast<unsigned long long>(localMmu.pendingCount()),
                static_cast<unsigned long long>(localMmu.outstandingCount()),
                static_cast<unsigned long long>(backendStep));
    }

    task.lsuTotalBeats = beats;
    task.lsuBeatsEnqueued = true;
    return true;
}

void
DetailedCuteBackend::serviceLsuMatrixRegWriteChunk(TaskSlot &task)
{
    if (!task.entry.isLoad || task.lsuPendingMatrixRegWriteChunks == 0) {
        return;
    }
    assert(!task.lsuPendingMatrixRegWriteEntries.empty());

    const auto write_request = MatrixRegResource::makeWrite(
        destBank(task.entry), MatrixRegResource::Client::MemoryLoader,
        task.lsuPendingMatrixRegWriteEntries.front());
    pendingMatrixRegWrites.push_back(write_request);

    const auto grants = matrixRegResource.arbitrate({write_request});
    assert(grants.size() == 1);
    if (!grants[0].granted) {
        ++counters.matrixRegLoaderWriteChunksStalled;
        DPRINTF(MatrixCuteTrace,
                "matrix_reg_loader_write_stall [sn:%llu] unit=%u "
                "bank=%u entry=%u pending=%u done=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(task.microTaskKind),
                static_cast<unsigned>(write_request.bank),
                write_request.entry,
                task.lsuPendingMatrixRegWriteChunks,
                task.lsuMatrixRegWriteChunksDone,
                static_cast<unsigned long long>(backendStep));
        return;
    }

    --task.lsuPendingMatrixRegWriteChunks;
    ++task.lsuMatrixRegWriteChunksDone;
    task.lsuPendingMatrixRegWriteEntries.pop_front();
    ++counters.matrixRegLoaderWriteChunksGranted;
    DPRINTF(MatrixCuteTrace,
            "matrix_reg_loader_write_grant [sn:%llu] unit=%u "
            "bank=%u entry=%u pending=%u done=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(task.microTaskKind),
            static_cast<unsigned>(write_request.bank),
            write_request.entry,
            task.lsuPendingMatrixRegWriteChunks,
            task.lsuMatrixRegWriteChunksDone,
            static_cast<unsigned long long>(backendStep));
}

void
DetailedCuteBackend::advanceLoadFill(TaskSlot &task)
{
    assert(task.entry.isLoad);

    if (!task.lsuBeatsEnqueued && !enqueueLocalMmuBeats(task)) {
        return;
    }

    if (task.bufferedCompletion.status != CuteCompletionStatus::Success) {
        return;
    }

    if (task.lsuResponsesReceived < task.lsuTotalBeats) {
        return;
    }

    if (!task.lsuFunctionalDone) {
        task.hasBufferedTensor = false;
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Lsu,
            CuteCompletionStatus::Success);

        MatrixTensor tensor;
        if (!memory->loadTile(task.entry.request.lsu, tensor)) {
            task.bufferedCompletion.status =
                CuteCompletionStatus::Unsupported;
            task.lsuPendingMatrixRegWriteChunks = 0;
            task.lsuPendingMatrixRegWriteEntries.clear();
            task.lsuFunctionalDone = true;
            return;
        }

        task.bufferedTensor = std::move(tensor);
        task.hasBufferedTensor = true;
        task.lsuFunctionalDone = true;
    }

    serviceLsuMatrixRegWriteChunk(task);
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

    enqueueLocalMmuBeats(task);

    enqueueTaskEvent(task, TaskEventKind::ReadFinish);
}

} // namespace matrix
} // namespace gem5
