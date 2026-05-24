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

#include "base/trace.hh"
#include "debug/MatrixCuteTrace.hh"
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
DetailedCuteBackend::enqueueTaskEvent(const TaskSlot &task, TaskEventKind kind,
                                      CuteCompletion completion)
{
    TaskEvent event;
    event.entry = task.entry;
    event.microTaskKind = task.microTaskKind;
    event.kind = kind;
    event.completion = completion;
    event.issueStep = task.issueStep;
    event.readyStep = backendStep;
    if (task.entry.isMma &&
        (kind == TaskEventKind::WriteFinish ||
         kind == TaskEventKind::TerminalCompletion)) {
        event.readyStep = backendStep + 1;
    }
    taskEvents.push_back(event);
}

void
DetailedCuteBackend::enqueueTaskEvent(const ComputeTaskState &task,
                                      TaskEventKind kind,
                                      CuteCompletion completion)
{
    TaskEvent event;
    event.entry = task.entry;
    event.microTaskKind = MicroTaskKind::Compute;
    event.kind = kind;
    event.completion = completion;
    event.issueStep = task.issueStep;
    event.readyStep = backendStep;
    if (task.entry.isMma &&
        (kind == TaskEventKind::WriteFinish ||
         kind == TaskEventKind::TerminalCompletion)) {
        event.readyStep = backendStep + 1;
    }
    taskEvents.push_back(event);
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
    assert(computeTasks.front().activeUnit == ComputeUnitKind::CDC);
    computeTasks.pop_front();
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
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::ComputeReadAFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishA(event.entry);
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::ComputeReadBFinish:
            assert(event.entry.isMma);
            scoreboard.onComputeReadFinishB(event.entry);
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::WriteFinish:
            applyWriteFinish(event.entry, event.completion,
                             event.microTaskKind);
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            break;
          case TaskEventKind::TerminalCompletion:
            finalizeCompletion(event.completion, event.microTaskKind,
                               event.issueStep,
                               activeTaskCount());
            if (event.entry.isMma) {
                retireComputeTask(event.entry.request.seq);
            } else {
                retireTaskSlot(event.microTaskKind, event.entry.request.seq);
            }
            DPRINTF(MatrixCuteTrace,
                    "task_event [sn:%llu] unit=%u event=%u step=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned>(event.kind),
                    static_cast<unsigned long long>(backendStep));
            DPRINTF(MatrixCuteTrace,
                    "microtask_finish [sn:%llu] unit=%u stage=done step=%llu delta=%llu.\n",
                    event.entry.request.seq,
                    static_cast<unsigned>(event.microTaskKind),
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(
                        counters.lastMicrotaskLatency));
            break;
        }
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

    switch (kind) {
      case ComputeUnitKind::ADC:
        break;
      case ComputeUnitKind::BDC:
        break;
      case ComputeUnitKind::MTE:
        task.executeCyclesRemaining = computeExecuteLatency(task.entry);
        break;
      case ComputeUnitKind::CDC:
        break;
      case ComputeUnitKind::None:
      case ComputeUnitKind::Count:
        break;
    }
    if (kind != ComputeUnitKind::None &&
        kind != ComputeUnitKind::Count) {
        recordComputeUnitIssue(kind);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_issue [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(kind),
                static_cast<unsigned long long>(backendStep));
    }
}

void
DetailedCuteBackend::recordComputeUnitIssue(ComputeUnitKind kind)
{
    ++counters.computeUnitIssuesByKind[static_cast<size_t>(kind)];
}

void
DetailedCuteBackend::recordComputeUnitOccupy(ComputeUnitKind kind)
{
    ++counters.computeUnitOccupiesByKind[static_cast<size_t>(kind)];
}

void
DetailedCuteBackend::recordComputeUnitFinish(ComputeUnitKind kind)
{
    ++counters.computeUnitFinishesByKind[static_cast<size_t>(kind)];
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

    recordComputeUnitFinish(ComputeUnitKind::ADC);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::ADC),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadAFinish);
}

void
DetailedCuteBackend::advanceComputeReadB(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensorB = false;
    if (task.bufferedCompletion.status != CuteCompletionStatus::Unsupported &&
        !regFile.hasRegister(MatrixBankKind::B, task.entry.readRegs[1])) {
        task.bufferedCompletion.status = CuteCompletionStatus::Unsupported;
    } else if (regFile.hasRegister(MatrixBankKind::B, task.entry.readRegs[1])) {
        task.bufferedTensorB =
            regFile.read(MatrixBankKind::B, task.entry.readRegs[1]);
        task.hasBufferedTensorB = true;
    }

    recordComputeUnitFinish(ComputeUnitKind::BDC);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::BDC),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
    enqueueTaskEvent(task, TaskEventKind::ComputeReadBFinish);
}

void
DetailedCuteBackend::advanceComputeExecute(ComputeTaskState &task)
{
    assert(task.entry.isMma);

    task.hasBufferedTensor = false;
    if (!computeDatatypeSupported(task.entry.request.mma) ||
        task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !task.hasBufferedTensorA || !task.hasBufferedTensorB) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        recordComputeUnitFinish(ComputeUnitKind::MTE);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(ComputeUnitKind::MTE),
                static_cast<unsigned long long>(backendStep));
        task.unitWorkDone = true;
        return;
    }

    MatrixRegFile scratch(regFile.abRegCount(), regFile.cRegCount());
    scratch.write(
        MatrixBankKind::A, task.entry.readRegs[0], task.bufferedTensorA);
    scratch.write(
        MatrixBankKind::B, task.entry.readRegs[1], task.bufferedTensorB);
    if (regFile.hasRegister(MatrixBankKind::C, task.entry.readRegs[2])) {
        scratch.write(
            MatrixBankKind::C, task.entry.readRegs[2],
            regFile.read(MatrixBankKind::C, task.entry.readRegs[2]));
    }

    task.bufferedCompletion = executeMma(
        task.entry.request.seq, task.entry.request.mma, scratch);
    if (task.bufferedCompletion.status != CuteCompletionStatus::Success ||
        !scratch.hasRegister(MatrixBankKind::C, task.entry.writeRegs[0])) {
        task.bufferedCompletion = makeCompletion(
            task.entry.request.seq, CuteRequestKind::Mma,
            CuteCompletionStatus::Unsupported);
        recordComputeUnitFinish(ComputeUnitKind::MTE);
        DPRINTF(MatrixCuteTrace,
                "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(ComputeUnitKind::MTE),
                static_cast<unsigned long long>(backendStep));
        task.unitWorkDone = true;
        return;
    }

    task.bufferedTensor =
        scratch.read(MatrixBankKind::C, task.entry.writeRegs[0]);
    task.hasBufferedTensor = true;
    recordComputeUnitFinish(ComputeUnitKind::MTE);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::MTE),
            static_cast<unsigned long long>(backendStep));
    task.unitWorkDone = true;
}

void
DetailedCuteBackend::finishComputeTask()
{
    assert(!computeTasks.empty());
    auto &task = computeTasks.front();
    const auto completion = executeComputeWrite(task);
    enqueueTaskEvent(task, TaskEventKind::WriteFinish, completion);
    enqueueTaskEvent(task, TaskEventKind::TerminalCompletion, completion);
    recordComputeUnitFinish(ComputeUnitKind::CDC);
    DPRINTF(MatrixCuteTrace,
            "compute_unit_finish [sn:%llu] unit=%u step=%llu.\n",
            task.entry.request.seq,
            static_cast<unsigned>(ComputeUnitKind::CDC),
            static_cast<unsigned long long>(backendStep));
    task.terminalIssued = true;
}

bool
DetailedCuteBackend::computeTaskFinishedMte(const ComputeTaskState &task) const
{
    return task.activeUnit == ComputeUnitKind::MTE && task.unitWorkDone;
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
        DPRINTF(MatrixCuteTrace,
                "microtask_occupy [sn:%llu] unit=%u stage=%u step=%llu age=%llu.\n",
                task.entry.request.seq,
                static_cast<unsigned>(task.microTaskKind),
                static_cast<unsigned>(task.stage),
                static_cast<unsigned long long>(backendStep),
                static_cast<unsigned long long>(backendStep - task.issueStep));
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
            task.stage = TaskStage::MemReq;
        }
        break;
      case TaskStage::MemReq:
        task.stage = TaskStage::FillPending;
        break;
      case TaskStage::FillPending:
        if (task.entry.isLoad) {
            advanceLoadFill(task);
            if (task.bufferedCompletion.status ==
                    CuteCompletionStatus::Success &&
                !task.hasBufferedTensor) {
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
      case TaskStage::StorePending:
      case TaskStage::WaitPendingStoreClear:
        finishTaskSlot(slot);
        break;
      case TaskStage::TerminalPending:
        break;
      case TaskStage::RegRead:
        advanceStoreRead(task);
        if (!task.hasBufferedTensor && task.bufferedCompletion.status == CuteCompletionStatus::Success) {
            break;
        }
        task.stage = TaskStage::StorePending;
        break;
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

    if (!computeTasks.empty() &&
        computeTasks.front().activeUnit == ComputeUnitKind::CDC &&
        computeTasks.front().unitWorkDone &&
        !computeTasks.front().terminalIssued) {
        finishComputeTask();
    }

    dispatchReadyComputeUnits();
}

void
DetailedCuteBackend::traceActiveComputeTasks()
{
    for (auto &task : computeTasks) {
        if (!task.occupancyTraced && backendStep > task.issueStep) {
            task.occupancyTraced = true;
            ++counters.microtaskOccupy;
            DPRINTF(MatrixCuteTrace,
                    "microtask_occupy [sn:%llu] unit=%u stage=%u step=%llu age=%llu.\n",
                    task.entry.request.seq,
                    static_cast<unsigned>(MicroTaskKind::Compute),
                    static_cast<unsigned>(task.activeUnit),
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(backendStep - task.issueStep));
        }

        if (task.activeUnit != ComputeUnitKind::None &&
            !task.unitOccupancyTraced && backendStep > task.unitIssueStep) {
            task.unitOccupancyTraced = true;
            recordComputeUnitOccupy(task.activeUnit);
            DPRINTF(MatrixCuteTrace,
                    "compute_unit_occupy [sn:%llu] unit=%u step=%llu age=%llu.\n",
                    task.entry.request.seq,
                    static_cast<unsigned>(task.activeUnit),
                    static_cast<unsigned long long>(backendStep),
                    static_cast<unsigned long long>(
                        backendStep - task.unitIssueStep));
        }
    }
}

void
DetailedCuteBackend::serviceActiveComputeUnits()
{
    for (auto &task : computeTasks) {
        if (task.activeUnit == ComputeUnitKind::ADC &&
            !task.unitWorkDone &&
            backendStep > task.unitIssueStep) {
            advanceComputeReadA(task);
        }
        if (task.activeUnit == ComputeUnitKind::BDC &&
            !task.unitWorkDone &&
            backendStep > task.unitIssueStep) {
            advanceComputeReadB(task);
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
            !task.unitWorkDone &&
            backendStep > task.unitIssueStep) {
            task.unitWorkDone = true;
        }
    }
}

void
DetailedCuteBackend::dispatchReadyComputeUnits()
{
    for (auto &task : computeTasks) {
        if (task.activeUnit == ComputeUnitKind::None &&
            computeUnitAvailable(ComputeUnitKind::ADC)) {
            beginComputeUnit(task, ComputeUnitKind::ADC);
        }
    }

    for (auto &task : computeTasks) {
        if (task.activeUnit == ComputeUnitKind::ADC &&
            task.unitWorkDone &&
            computeUnitAvailable(ComputeUnitKind::BDC)) {
            beginComputeUnit(task, ComputeUnitKind::BDC);
        }
    }

    for (auto &task : computeTasks) {
        if (task.activeUnit == ComputeUnitKind::BDC &&
            task.unitWorkDone &&
            computeUnitAvailable(ComputeUnitKind::MTE)) {
            beginComputeUnit(task, ComputeUnitKind::MTE);
        }
    }

    for (auto &task : computeTasks) {
        if (computeTaskFinishedMte(task) &&
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
