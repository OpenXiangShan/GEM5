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

#include "matrix/CUTETOP.hh"

#include <algorithm>
#include <utility>

#include "base/trace.hh"
#include "debug/MatrixCuteTrace.hh"

namespace gem5
{

namespace matrix
{

namespace
{

constexpr unsigned MatrixTileMn = 8;
constexpr unsigned Int8KGroup = 32;
constexpr unsigned LocalMmuReadResponseMatrixRegChunks = 2;

bool
matrixRegWriteConflictsWithComputeRead(
    const MatrixRegResource::Request &write_request,
    const DecodedFifoEntry &entry)
{
    if (write_request.bank == MatrixBankKind::A ||
        write_request.bank == MatrixBankKind::B) {
        return true;
    }

    if (write_request.bank == MatrixBankKind::C) {
        return (write_request.entry & 1) == (entry.readRegs[2] & 1);
    }

    return false;
}

bool
isInt8TileWritebackMma(const AmuMmaDesc &desc)
{
    const bool int8_tags =
        desc.lhsElemType == MatrixElemType::Int8 &&
        desc.rhsElemType == MatrixElemType::Int8 &&
        desc.dstElemType == MatrixElemType::Int32;
    const bool int8_encodings =
        (desc.types1 == 0 || desc.types1 == 0x4) &&
        (desc.types2 == 0 || desc.types2 == 0x4) &&
        (desc.typed == 0 || desc.typed == 0x2);
    return !desc.isFp && int8_tags && int8_encodings &&
           desc.mtilem % MatrixTileMn == 0 &&
           desc.mtilen % MatrixTileMn == 0 &&
           desc.mtilek % Int8KGroup == 0;
}

unsigned
cdcTileEntry(const AmuMmaDesc &desc, unsigned beat)
{
    const unsigned m_tiles = desc.mtilem / MatrixTileMn;
    const unsigned n_tiles = desc.mtilen / MatrixTileMn;
    return beat % (m_tiles * n_tiles);
}

} // anonymous namespace

// Active runtime path: task slots, task events, and final completions.
void
DetailedCuteBackend::applyWriteFinish(const DecodedFifoEntry &entry,
                                      const CuteCompletion &completion,
                                      MicroTaskKind kind)
{
    if (entry.isLoad) {
        scoreboard.onLoadFinish(entry);
    } else if (entry.isStore) {
        scoreboard.onStoreWriteFinish(entry);
        if (pendingStoreCount > 0) {
            --pendingStoreCount;
        }
        DPRINTF(MatrixCuteTrace,
                "store_write_finish [sn:%llu] unit=%u step=%llu pendingStore=%u status=%u.\n",
                entry.request.seq,
                static_cast<unsigned>(kind),
                static_cast<unsigned long long>(backendStep),
                pendingStoreCount,
                static_cast<unsigned>(completion.status));
    } else if (entry.isMma) {
        scoreboard.onComputeWriteFinishC(entry);
    } else if (entry.isZeroAcc || entry.isZeroTr) {
        scoreboard.onArithFinish(entry);
    }

}

uint64_t
DetailedCuteBackend::finalizeCompletion(const CuteCompletion &completion,
                                        MicroTaskKind kind,
                                        uint64_t issueStep,
                                        uint64_t activeCount)
{
    ++counters.backendCompletion;
    ++counters.microtaskFinish;
    counters.lastMicrotaskLatency = backendStep - issueStep;
    counters.microtaskLatencySum += counters.lastMicrotaskLatency;
    ++counters.microtaskFinishesByKind[static_cast<size_t>(kind)];
    completions.push_back(completion);
    DPRINTF(MatrixCuteTrace,
            "backend completion [sn:%llu] kind=%u pendingStore=%u active=%llu.\n",
            completion.seq,
            static_cast<unsigned>(completion.kind),
            pendingStoreCount,
            static_cast<unsigned long long>(activeCount));
    return counters.lastMicrotaskLatency;
}

void
DetailedCuteBackend::enqueueTaskEvent(const DecodedFifoEntry &entry,
                                      MicroTaskKind microTaskKind,
                                      uint64_t issueStep,
                                      TaskEventKind kind,
                                      CuteCompletion completion)
{
    TaskEvent event;
    event.entry = entry;
    event.microTaskKind = microTaskKind;
    event.kind = kind;
    event.completion = completion;
    event.issueStep = issueStep;
    event.readyStep = backendStep;
    if (entry.isMma &&
        (kind == TaskEventKind::WriteFinish ||
         kind == TaskEventKind::TerminalCompletion)) {
        event.readyStep = backendStep + 1;
    }
    taskEvents.push_back(event);
}

void
DetailedCuteBackend::enqueueTaskEvent(const TaskSlot &task, TaskEventKind kind,
                                      CuteCompletion completion)
{
    enqueueTaskEvent(task.entry, task.microTaskKind, task.issueStep, kind,
                     completion);
}

void
DetailedCuteBackend::enqueueTaskEvent(const ComputeTaskState &task,
                                      TaskEventKind kind,
                                      CuteCompletion completion)
{
    enqueueTaskEvent(task.entry, MicroTaskKind::Compute, task.issueStep, kind,
                     completion);
}

void
DetailedCuteBackend::retireTaskSlot(MicroTaskKind kind, uint64_t seq)
{
    std::optional<TaskSlot> *slot = nullptr;
    switch (kind) {
      case MicroTaskKind::AML:
        slot = &amlTask;
        break;
      case MicroTaskKind::BML:
        slot = &bmlTask;
        break;
      case MicroTaskKind::CML:
        slot = &cmlTask;
        break;
      case MicroTaskKind::Release:
        slot = &releaseTask;
        break;
      case MicroTaskKind::Compute:
      case MicroTaskKind::Count:
        assert(false && "retireTaskSlot called with unexpected kind");
        return;
    }

    assert(slot != nullptr);
    assert(slot->has_value());
    assert(slot->value().entry.request.seq == seq);
    slot->reset();
}

void
DetailedCuteBackend::retireComputeTask(uint64_t seq)
{
    assert(!computeTasks.empty());
    assert(computeTasks.front().entry.request.seq == seq);
    assert(computeTasks.front().terminalIssued);
    assert(computeTasks.front().unitWorkDone);
    computeTasks.pop_front();
}

void
DetailedCuteBackend::traceTaskEvent(const TaskEvent &event) const
{
    DPRINTF(MatrixCuteTrace,
            "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
            event.entry.request.seq,
            static_cast<unsigned>(event.microTaskKind),
            static_cast<unsigned>(event.kind),
            static_cast<unsigned long long>(backendStep));
}

void
DetailedCuteBackend::traceMicrotaskOccupy(const DecodedFifoEntry &entry,
                                          MicroTaskKind kind,
                                          uint8_t stage,
                                          uint64_t issueStep) const
{
    DPRINTF(MatrixCuteTrace,
            "microtask_occupy [sn:%llu] unit=%u stage=%u step=%llu age=%llu.\n",
            entry.request.seq,
            static_cast<unsigned>(kind),
            static_cast<unsigned>(stage),
            static_cast<unsigned long long>(backendStep),
            static_cast<unsigned long long>(backendStep - issueStep));
}

void
DetailedCuteBackend::markComputeUnitOccupied(const ComputeTaskState &task)
{
    ++counters.computeUnitOccupiesByKind[
        static_cast<size_t>(task.activeUnit)];
    DPRINTF(MatrixCuteTrace,
            "compute_unit_occupy [sn:%llu] unit=%u step=%llu age=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(task.activeUnit),
            static_cast<unsigned long long>(backendStep),
            static_cast<unsigned long long>(
                backendStep - task.unitIssueStep));
}

void
DetailedCuteBackend::markComputeUnitIssued(const ComputeTaskState &task,
                                           ComputeUnitKind kind)
{
    ++counters.computeUnitIssuesByKind[static_cast<size_t>(kind)];
    DPRINTF(MatrixCuteTrace,
            "compute_unit_issue [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(kind),
            static_cast<unsigned long long>(backendStep));
}

void
DetailedCuteBackend::markComputeUnitFinished(ComputeTaskState &task,
                                             ComputeUnitKind kind)
{
    ++counters.computeUnitFinishesByKind[static_cast<size_t>(kind)];
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(kind),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
}

void
DetailedCuteBackend::processTaskEvents()
{
    while (!taskEvents.empty()) {
        const auto event = taskEvents.front();
        if (event.readyStep > backendStep) {
            break;
        }
        taskEvents.pop_front();

        switch (event.kind) {
          case TaskEventKind::ReadFinish:
            assert(event.entry.isStore);
            scoreboard.onStoreReadFinish(event.entry);
            break;
          case TaskEventKind::ComputeReadAFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishA(event.entry);
            break;
          case TaskEventKind::ComputeReadBFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishB(event.entry);
            break;
          case TaskEventKind::ComputeReadCFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishC(event.entry);
            break;
          case TaskEventKind::WriteFinish:
            applyWriteFinish(event.entry, event.completion,
                             event.microTaskKind);
            break;
          case TaskEventKind::TerminalCompletion:
            finalizeCompletion(event.completion, event.microTaskKind,
                               event.issueStep, activeTaskCount());
            if (event.entry.isMma) {
                retireComputeTask(event.entry.request.seq);
            } else {
                retireTaskSlot(event.microTaskKind, event.entry.request.seq);
            }
            traceTaskEvent(event);
            DPRINTF(MatrixCuteTrace,
                    "microtask_finish [sn:%llu] unit=%u stage=done step=%llu delta=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(
                        counters.lastMicrotaskLatency));
            continue;
        }
        traceTaskEvent(event);
    }
}

void
DetailedCuteBackend::finishTaskSlot(std::optional<TaskSlot> &slot)
{
    assert(slot.has_value());
    auto &task = slot.value();
    const auto &entry = task.entry;
    const auto completion = entry.isLoad ?
        executeLoadWrite(task) :
        (entry.isStore ? executeStoreWrite(task) :
         executeTaskSlot(task));

    if (entry.isLoad || entry.isStore ||
        entry.isZeroAcc || entry.isZeroTr) {
        if (auto request = matrixRegWriteRequestForTask(task, completion)) {
            pendingMatrixRegWrites.push_back(*request);
        }
        enqueueTaskEvent(task, TaskEventKind::WriteFinish, completion);
    }
    enqueueTaskEvent(task, TaskEventKind::TerminalCompletion, completion);
    task.stage = TaskStage::TerminalPending;
}

void
DetailedCuteBackend::beginComputeUnit(ComputeTaskState &task,
                                      ComputeUnitKind kind)
{
    task.activeUnit = kind;
    task.unitWorkDone = false;
    task.terminalIssued = false;
    task.unitIssueStep = backendStep;
    task.unitOccupancyTraced = false;

    if (kind == ComputeUnitKind::MTE) {
        task.executeCyclesRemaining = computeExecuteLatency(task.entry);
    } else if (kind == ComputeUnitKind::CDC) {
        const unsigned writeback_beats = std::max(
            1U, computeMteTiming(task.entry.request.mma).cdcWriteCycles);
        task.cdcWritebackBeatsTotal = writeback_beats;
        task.cdcWritebackBeatsRemaining = writeback_beats;
        task.cdcWritebackBeatsDone = 0;
        task.cdcTileReadIssued = false;
        task.cdcTileWriteReady = false;
        task.hasCdcTileWriteTensor = false;
        issueCdcTileRead(task);
    }

    if (kind != ComputeUnitKind::None &&
        kind != ComputeUnitKind::Count &&
        kind != ComputeUnitKind::CDC) {
        markComputeUnitIssued(task, kind);
    }
}

void
DetailedCuteBackend::advanceComputeReadC(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensorC = false;
    if (regFile.hasRegister(MatrixBankKind::C, task.entry.readRegs[2])) {
        task.bufferedTensorC =
            regFile.read(MatrixBankKind::C, task.entry.readRegs[2]);
        task.hasBufferedTensorC = true;
    }

    markComputeUnitFinished(task, ComputeUnitKind::CDC);
    task.cdcReadComplete = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadCFinish);
}

bool
DetailedCuteBackend::issueComputeReadFrontend(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    if (task.adcReadIssued || task.bdcReadIssued || task.cdcReadIssued) {
        return false;
    }

    for (const auto &write_request : pendingMatrixRegWrites) {
        if (matrixRegWriteConflictsWithComputeRead(
                write_request, task.entry)) {
            return false;
        }
    }

    std::vector<MatrixRegResource::Request> requests = {
        MatrixRegResource::makeRead(
            MatrixBankKind::A, MatrixRegResource::Client::DataController,
            task.entry.readRegs[0]),
        MatrixRegResource::makeRead(
            MatrixBankKind::B, MatrixRegResource::Client::DataController,
            task.entry.readRegs[1]),
        MatrixRegResource::makeRead(
            MatrixBankKind::C, MatrixRegResource::Client::DataController,
            task.entry.readRegs[2])};
    requests.insert(requests.end(), pendingMatrixRegWrites.begin(),
                    pendingMatrixRegWrites.end());

    const auto grants = matrixRegResource.arbitrate(requests);
    assert(grants.size() == requests.size());
    if (!grants[0].granted || !grants[1].granted || !grants[2].granted) {
        return false;
    }

    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Mma,
        CuteCompletionStatus::Success);
    task.adcReadIssued = true;
    task.bdcReadIssued = true;
    task.cdcReadIssued = true;
    task.unitIssueStep = backendStep;
    task.unitOccupancyTraced = false;

    for (auto kind : {ComputeUnitKind::ADC,
                      ComputeUnitKind::BDC,
                      ComputeUnitKind::CDC}) {
        markComputeUnitIssued(task, kind);
    }

    return true;
}

void
DetailedCuteBackend::advanceComputeReadA(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensorA = false;
    task.bufferedCompletion = makeCompletion(
        task.entry.request.seq, CuteRequestKind::Mma,
        CuteCompletionStatus::Success);

    if (!regFile.hasRegister(MatrixBankKind::A, task.entry.readRegs[0])) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    } else {
        task.bufferedTensorA =
            regFile.read(MatrixBankKind::A, task.entry.readRegs[0]);
        task.hasBufferedTensorA = true;
    }

    markComputeUnitFinished(task, ComputeUnitKind::ADC);
    task.adcReadComplete = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadAFinish);
}

void
DetailedCuteBackend::advanceComputeReadB(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensorB = false;
    const bool has_b =
        regFile.hasRegister(MatrixBankKind::B, task.entry.readRegs[1]);
    if (task.bufferedCompletion.status != CuteCompletionStatus::Unsupported &&
        !has_b) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    } else if (has_b) {
        task.bufferedTensorB =
            regFile.read(MatrixBankKind::B, task.entry.readRegs[1]);
        task.hasBufferedTensorB = true;
    }

    markComputeUnitFinished(task, ComputeUnitKind::BDC);
    task.bdcReadComplete = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadBFinish);
}

void
DetailedCuteBackend::advanceComputeExecute(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    auto finish_mte = [&] {
        markComputeUnitFinished(task, ComputeUnitKind::MTE);
    };
    auto finish_unsupported = [&] {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        finish_mte();
    };

    task.hasBufferedTensor = false;
    if (!computeDatatypeSupported(task.entry.request.mma) ||
        task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !task.hasBufferedTensorA || !task.hasBufferedTensorB) {
        finish_unsupported();
        return;
    }

    if (isInt8TileWritebackMma(task.entry.request.mma)) {
        const auto &desc = task.entry.request.mma;
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Success);
        if (task.bufferedTensorA.elemType != MatrixElemType::Int8 ||
            task.bufferedTensorB.elemType != MatrixElemType::Int8 ||
            task.bufferedTensorA.rows != desc.mtilem ||
            task.bufferedTensorA.cols != desc.mtilek ||
            task.bufferedTensorB.rows != desc.mtilek ||
            task.bufferedTensorB.cols != desc.mtilen ||
            (task.hasBufferedTensorC &&
             (task.bufferedTensorC.elemType != MatrixElemType::Int32 ||
              task.bufferedTensorC.rows != desc.mtilem ||
              task.bufferedTensorC.cols != desc.mtilen))) {
            task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
        }
        markComputeUnitFinished(task, ComputeUnitKind::MTE);
        return;
    }

    MatrixRegFile scratch(regFile.abRegCount(), regFile.cRegCount());
    scratch.write(
        MatrixBankKind::A, task.entry.readRegs[0], task.bufferedTensorA);
    scratch.write(
        MatrixBankKind::B, task.entry.readRegs[1], task.bufferedTensorB);
    if (task.hasBufferedTensorC) {
        scratch.write(
            MatrixBankKind::C, task.entry.readRegs[2],
            task.bufferedTensorC);
    }

    task.bufferedCompletion = executeMma(
        task.entry.request.seq, task.entry.request.mma, scratch);
    if (task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !scratch.hasRegister(MatrixBankKind::C, task.entry.writeRegs[0])) {
        finish_unsupported();
        return;
    }

    task.bufferedTensor =
        scratch.read(MatrixBankKind::C, task.entry.writeRegs[0]);
    task.hasBufferedTensor = true;
    finish_mte();
}

bool
DetailedCuteBackend::issueCdcTileRead(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.activeUnit == ComputeUnitKind::CDC);
    if (task.cdcTileReadIssued || task.cdcTileWriteReady) {
        return false;
    }

    const auto &desc = task.entry.request.mma;
    if (!isInt8TileWritebackMma(desc)) {
        return false;
    }

    std::vector<MatrixRegResource::Request> requests =
        pendingMatrixRegWrites;
    requests.push_back(MatrixRegResource::makeRead(
        MatrixBankKind::C, MatrixRegResource::Client::DataController,
        cdcTileEntry(desc, task.cdcWritebackBeatsDone)));
    const auto grants = matrixRegResource.arbitrate(requests);
    assert(grants.size() == requests.size());
    if (!grants.back().granted) {
        return false;
    }

    task.cdcTileReadIssued = true;
    task.cdcTileReadBeatIndex = task.cdcWritebackBeatsDone;
    return true;
}

bool
DetailedCuteBackend::prepareCdcTileWrite(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.cdcTileReadIssued);
    assert(!task.cdcTileWriteReady);

    if (!matrixRegResource.consumeReadResponse(
            MatrixBankKind::C, MatrixRegResource::Client::DataController)) {
        return false;
    }

    const auto &desc = task.entry.request.mma;
    const unsigned m_tiles = desc.mtilem / MatrixTileMn;
    const unsigned n_tiles = desc.mtilen / MatrixTileMn;
    const unsigned tiles_per_k_group = m_tiles * n_tiles;
    const unsigned addr = cdcTileEntry(desc, task.cdcTileReadBeatIndex);
    const unsigned k_group = task.cdcTileReadBeatIndex / tiles_per_k_group;
    const unsigned m_tile = addr / n_tiles;
    const unsigned n_tile = addr % n_tiles;
    const unsigned k_begin = k_group * Int8KGroup;
    const unsigned k_end = std::min(k_begin + Int8KGroup, desc.mtilek);

    MatrixTensor updated;
    if (regFile.hasRegister(MatrixBankKind::C, task.entry.writeRegs[0])) {
        updated = regFile.read(MatrixBankKind::C, task.entry.writeRegs[0]);
    } else {
        updated.rows = desc.mtilem;
        updated.cols = desc.mtilen;
        updated.elemType = desc.dstElemType;
        updated.elements.assign(
            static_cast<size_t>(updated.rows) * updated.cols, 0);
    }

    if (updated.elemType != MatrixElemType::Int32 ||
        updated.rows != desc.mtilem ||
        updated.cols != desc.mtilen ||
        !task.hasBufferedTensorA || !task.hasBufferedTensorB ||
        task.bufferedTensorA.elemType != MatrixElemType::Int8 ||
        task.bufferedTensorB.elemType != MatrixElemType::Int8 ||
        task.bufferedTensorA.rows != desc.mtilem ||
        task.bufferedTensorA.cols != desc.mtilek ||
        task.bufferedTensorB.rows != desc.mtilek ||
        task.bufferedTensorB.cols != desc.mtilen) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        task.cdcTileReadIssued = false;
        task.cdcTileWriteReady = true;
        task.cdcTileWriteBeatIndex = task.cdcTileReadBeatIndex;
        task.hasCdcTileWriteTensor = false;
        return true;
    }

    for (unsigned mi = 0; mi < MatrixTileMn; ++mi) {
        const unsigned m = m_tile * MatrixTileMn + mi;
        for (unsigned ni = 0; ni < MatrixTileMn; ++ni) {
            const unsigned n = n_tile * MatrixTileMn + ni;
            int64_t acc =
                updated.elements[static_cast<size_t>(m) * updated.cols + n];
            for (unsigned k = k_begin; k < k_end; ++k) {
                const int64_t lhs =
                    task.bufferedTensorA.elements[
                        static_cast<size_t>(m) *
                        task.bufferedTensorA.cols + k];
                const int64_t rhs =
                    task.bufferedTensorB.elements[
                        static_cast<size_t>(k) *
                        task.bufferedTensorB.cols + n];
                acc += lhs * rhs;
            }
            updated.elements[static_cast<size_t>(m) * updated.cols + n] = acc;
        }
    }

    task.cdcTileWriteTensor = std::move(updated);
    task.hasCdcTileWriteTensor = true;
    task.cdcTileReadIssued = false;
    task.cdcTileWriteReady = true;
    task.cdcTileWriteBeatIndex = task.cdcTileReadBeatIndex;
    return true;
}

void
DetailedCuteBackend::advanceComputeWriteback(ComputeTaskState &task)
{
    assert(task.entry.isMma);
    assert(task.activeUnit == ComputeUnitKind::CDC);
    assert(task.cdcWritebackBeatsRemaining != 0);

    const bool int8_tile_writeback =
        isInt8TileWritebackMma(task.entry.request.mma) &&
        task.bufferedCompletion.status == CuteCompletionStatus::Success;

    if (int8_tile_writeback && !task.cdcTileWriteReady) {
        if (task.cdcTileReadIssued) {
            prepareCdcTileWrite(task);
        } else {
            issueCdcTileRead(task);
        }
        if (!task.cdcTileWriteReady) {
            return;
        }
    }

    const auto write_entry = int8_tile_writeback ?
        task.cdcTileWriteBeatIndex : task.entry.writeRegs[0];
    const auto write_request = MatrixRegResource::makeWrite(
        MatrixBankKind::C, MatrixRegResource::Client::DataController,
        write_entry);
    std::vector<MatrixRegResource::Request> requests =
        pendingMatrixRegWrites;

    const bool can_pipeline_next_read =
        int8_tile_writeback &&
        task.cdcWritebackBeatsRemaining > 1 &&
        !task.cdcTileReadIssued;
    if (can_pipeline_next_read) {
        const auto &desc = task.entry.request.mma;
        const unsigned next_beat = task.cdcWritebackBeatsDone + 1;
        requests.push_back(MatrixRegResource::makeRead(
            MatrixBankKind::C, MatrixRegResource::Client::DataController,
            cdcTileEntry(desc, next_beat)));
    }
    requests.push_back(write_request);
    const auto grants = matrixRegResource.arbitrate(requests);
    assert(grants.size() == requests.size());
    if (!grants.back().granted) {
        return;
    }

    if (can_pipeline_next_read) {
        const auto read_grant = grants[grants.size() - 2];
        if (read_grant.granted) {
            task.cdcTileReadIssued = true;
            task.cdcTileReadBeatIndex = task.cdcWritebackBeatsDone + 1;
        }
    }

    if (task.cdcTileWriteReady) {
        if (task.bufferedCompletion.status == CuteCompletionStatus::Success) {
            if (!task.hasCdcTileWriteTensor) {
                task.bufferedCompletion.status =
                    CuteCompletionStatus::Unsupported;
            } else {
                regFile.write(MatrixBankKind::C, task.entry.writeRegs[0],
                              task.cdcTileWriteTensor);
            }
        }
        task.cdcTileWriteReady = false;
        task.hasCdcTileWriteTensor = false;
    }

    --task.cdcWritebackBeatsRemaining;
    ++task.cdcWritebackBeatsDone;
    if (task.cdcWritebackBeatsRemaining != 0) {
        return;
    }

    const auto completion = isInt8TileWritebackMma(task.entry.request.mma) ?
        task.bufferedCompletion : executeComputeWrite(task);
    enqueueTaskEvent(task, TaskEventKind::WriteFinish, completion);
    enqueueTaskEvent(task, TaskEventKind::TerminalCompletion, completion);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::CDC),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
    task.terminalIssued = true;
    task.activeUnit = ComputeUnitKind::None;
}

void
DetailedCuteBackend::advanceTaskSlot(std::optional<TaskSlot> &slot)
{
    if (!slot.has_value()) {
        return;
    }

    auto &task = slot.value();
    if (!task.occupancyTraced && backendStep > task.issueStep) {
        task.occupancyTraced = true;
        ++counters.microtaskOccupy;
        traceMicrotaskOccupy(task.entry, task.microTaskKind,
                             static_cast<uint8_t>(task.stage),
                             task.issueStep);
    }

    switch (task.stage) {
      case TaskStage::Accepted:
        if (task.entry.isStore) {
            task.stage = TaskStage::RegRead;
        } else if (task.entry.isRelease) {
            task.stage = TaskStage::WaitPendingStoreClear;
        } else if (task.entry.isZeroAcc || task.entry.isZeroTr) {
            task.stage = TaskStage::RegWrite;
        } else {
            enqueueLocalMmuBeats(task);
            task.stage = TaskStage::FillPending;
        }
        break;
      case TaskStage::FillPending:
        if (task.entry.isLoad) {
            advanceLoadFill(task);
            if (task.bufferedCompletion.status ==
                    CuteCompletionStatus::Success &&
                (!task.hasBufferedTensor ||
                 task.lsuPendingMatrixRegWriteChunks != 0)) {
                break;
            }
            if (task.bufferedCompletion.status !=
                CuteCompletionStatus::Success) {
                task.stage = TaskStage::RegWrite;
                break;
            }
        }
        if (task.entry.isLoad && !task.hasBufferedTensor) {
            break;
        }
        task.stage = TaskStage::RegWrite;
        break;
      case TaskStage::RegWrite:
      case TaskStage::WaitPendingStoreClear:
        finishTaskSlot(slot);
        break;
      case TaskStage::StorePending:
        if (task.lsuResponsesReceived < task.lsuTotalBeats) {
            break;
        }
        finishTaskSlot(slot);
        break;
      case TaskStage::TerminalPending:
        break;
      case TaskStage::RegRead:
        advanceStoreRead(task);
        if (!task.hasBufferedTensor &&
            task.bufferedCompletion.status == CuteCompletionStatus::Success) {
            break;
        }
        task.stage = TaskStage::StorePending;
        break;
    }
}

void
DetailedCuteBackend::serviceLocalMmuResponses()
{
    for (const auto &response : localMmu.takeReadyResponses()) {
        auto service_slot = [&](std::optional<TaskSlot> &slot) {
            if (!slot.has_value()) {
                return false;
            }
            auto &task = slot.value();
            if (task.entry.request.seq != response.seq ||
                localMmuClient(task) != response.client) {
                return false;
            }

            ++task.lsuResponsesReceived;
            if (!response.isStore) {
                ++counters.localMmuReadResponses;
                const uint32_t base_entry = task.entry.writeRegs[0] +
                    response.beatIndex *
                    LocalMmuReadResponseMatrixRegChunks;
                for (unsigned chunk = 0;
                     chunk < LocalMmuReadResponseMatrixRegChunks;
                     ++chunk) {
                    task.lsuPendingMatrixRegWriteEntries.push_back(
                        base_entry + chunk);
                }
                task.lsuPendingMatrixRegWriteChunks +=
                    LocalMmuReadResponseMatrixRegChunks;
                counters.matrixRegLoaderWriteChunksQueued +=
                    LocalMmuReadResponseMatrixRegChunks;
            } else {
                ++counters.localMmuStoreAcks;
            }
            DPRINTF(MatrixCuteTrace,
                    "local_mmu_response [sn:%llu] unit=%u store=%u "
                    "beat=%u bytes=%u source=%u responses=%u/%u "
                    "pendingFill=%u step=%llu.\n",
                    task.entry.request.seq,
                    static_cast<unsigned>(task.microTaskKind),
                    response.isStore ? 1 : 0,
                    response.beatIndex,
                    response.byteSize,
                    response.sourceId,
                    task.lsuResponsesReceived,
                    task.lsuTotalBeats,
                    task.lsuPendingMatrixRegWriteChunks,
                    static_cast<unsigned long long>(backendStep));
            return true;
        };

        if (service_slot(amlTask) ||
            service_slot(bmlTask) ||
            service_slot(cmlTask)) {
            continue;
        }
    }
}

void
DetailedCuteBackend::advanceComputeTask()
{
    if (computeTasks.empty()) {
        return;
    }

    traceActiveComputeTasks();
    serviceActiveComputeUnits();

    dispatchReadyComputeUnits();
}

void
DetailedCuteBackend::traceActiveComputeTasks()
{
    for (auto &task : computeTasks) {
        if (!task.occupancyTraced && backendStep > task.issueStep) {
            task.occupancyTraced = true;
            ++counters.microtaskOccupy;
            traceMicrotaskOccupy(task.entry, MicroTaskKind::Compute,
                                 static_cast<uint8_t>(task.activeUnit),
                                 task.issueStep);
        }

        if (task.activeUnit != ComputeUnitKind::None &&
            !task.unitOccupancyTraced && backendStep > task.unitIssueStep) {
            task.unitOccupancyTraced = true;
            markComputeUnitOccupied(task);
        }
    }
}

void
DetailedCuteBackend::serviceActiveComputeUnits()
{
    for (auto &task : computeTasks) {
        if (task.adcReadIssued && !task.adcReadComplete &&
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::A,
                MatrixRegResource::Client::DataController)) {
            advanceComputeReadA(task);
        }
        if (task.bdcReadIssued && !task.bdcReadComplete &&
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::B,
                MatrixRegResource::Client::DataController)) {
            advanceComputeReadB(task);
        }
        if (task.cdcReadIssued && !task.cdcReadComplete &&
            matrixRegResource.consumeReadResponse(
                MatrixBankKind::C,
                MatrixRegResource::Client::DataController)) {
            advanceComputeReadC(task);
        }
        if (task.activeUnit == ComputeUnitKind::MTE) {
            if (task.unitWorkDone) {
                continue;
            }
            if (backendStep <= task.unitIssueStep) {
                continue;
            }
            if (task.executeCyclesRemaining > 1) {
                --task.executeCyclesRemaining;
            } else if (task.executeCyclesRemaining == 1) {
                task.executeCyclesRemaining = 0;
                advanceComputeExecute(task);
            }
        }
        if (task.activeUnit == ComputeUnitKind::CDC &&
            !task.unitWorkDone) {
            advanceComputeWriteback(task);
        }
    }
}

void
DetailedCuteBackend::dispatchReadyComputeUnits()
{
    for (auto &task : computeTasks) {
        if (task.terminalIssued) {
            continue;
        }
        if (task.activeUnit == ComputeUnitKind::None &&
            !task.adcReadIssued &&
            computeUnitAvailable(ComputeUnitKind::ADC) &&
            computeUnitAvailable(ComputeUnitKind::BDC) &&
            computeUnitAvailable(ComputeUnitKind::CDC)) {
            issueComputeReadFrontend(task);
        }
    }

    for (auto &task : computeTasks) {
        if (task.terminalIssued) {
            continue;
        }
        if (task.activeUnit == ComputeUnitKind::None &&
            task.adcReadComplete &&
            task.bdcReadComplete &&
            task.cdcReadComplete &&
            computeUnitAvailable(ComputeUnitKind::MTE)) {
            beginComputeUnit(task, ComputeUnitKind::MTE);
        }
    }

    for (auto &task : computeTasks) {
        if (task.terminalIssued) {
            continue;
        }
        if (task.activeUnit == ComputeUnitKind::MTE &&
            task.unitWorkDone &&
            computeUnitAvailable(ComputeUnitKind::CDC)) {
            beginComputeUnit(task, ComputeUnitKind::CDC);
        }
    }
}

void
DetailedCuteBackend::advanceTaskSlots()
{
    advanceTaskSlot(amlTask);
    advanceTaskSlot(bmlTask);
    advanceTaskSlot(cmlTask);
    advanceComputeTask();
    advanceTaskSlot(releaseTask);
}

void
DetailedCuteBackend::processFifoHead()
{
    if (fifo.empty()) {
        return;
    }

    const auto &head = fifo.head();
    DetailedCuteScoreboard::BlockReason sb_reason;
    const bool can_issue = headReady(head, sb_reason);

    if (can_issue) {
        ++counters.fifoDequeue;
        DPRINTF(MatrixCuteTrace,
                "fifo_deq [sn:%llu] kind=%u queued=%llu active=%llu pendingStore=%u.\n",
                head.request.seq,
                static_cast<unsigned>(head.request.kind),
                static_cast<unsigned long long>(fifo.size() - 1),
                static_cast<unsigned long long>(activeTaskCount() + 1),
                pendingStoreCount);
        issueHead(head);
        fifo.dequeue();
        return;
    }

    if (sb_reason != DetailedCuteScoreboard::BlockReason::None) {
        recordScoreboardBlock(sb_reason);
        DPRINTF(MatrixCuteTrace,
                "scoreboard_block [sn:%llu] kind=%u reason=%u pendingStore=%u.\n",
                head.request.seq,
                static_cast<unsigned>(head.request.kind),
                static_cast<unsigned>(sb_reason),
                pendingStoreCount);
        return;
    }

    if (head.isRelease && !releaseReady()) {
        recordFifoBlock(FifoBlockReason::ReleasePendingStore);
        DPRINTF(MatrixCuteTrace,
                "fifo_block [sn:%llu] kind=%u reason=release_pending_store pendingStore=%u.\n",
                head.request.seq,
                static_cast<unsigned>(head.request.kind),
                pendingStoreCount);
        return;
    }

    recordFifoBlock(FifoBlockReason::DownstreamNotAccepting);
    DPRINTF(MatrixCuteTrace,
            "fifo_block [sn:%llu] kind=%u reason=downstream_not_accepting pendingStore=%u.\n",
            head.request.seq,
            static_cast<unsigned>(head.request.kind),
            pendingStoreCount);
}

void
DetailedCuteBackend::step()
{
    ++backendStep;
    matrixRegResource.advanceCycle();
    const auto issued_before = localMmu.issuedCount();
    localMmu.step(backendStep);
    const auto issued_after = localMmu.issuedCount();
    if (issued_after != issued_before) {
        counters.localMmuBeatsIssued += issued_after - issued_before;
        DPRINTF(MatrixCuteTrace,
                "local_mmu_issue step=%llu issued=%llu pending=%llu "
                "outstanding=%llu.\n",
                static_cast<unsigned long long>(backendStep),
                static_cast<unsigned long long>(issued_after - issued_before),
                static_cast<unsigned long long>(localMmu.pendingCount()),
                static_cast<unsigned long long>(localMmu.outstandingCount()));
    }
    serviceLocalMmuResponses();
    pendingMatrixRegWrites.clear();
    memoryBudget = 1;
    processFifoHead();
    advanceTaskSlots();
    processTaskEvents();
}

CuteCompletion
DetailedCuteBackend::popCompletion()
{
    assert(!completions.empty());
    const auto completion = completions.front();
    completions.pop_front();
    return completion;
}

} // namespace matrix
} // namespace gem5
