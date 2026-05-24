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

#ifndef __MATRIX_DETAILED_CUTE_BACKEND_HH__
#define __MATRIX_DETAILED_CUTE_BACKEND_HH__

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "matrix/decoded_fifo.hh"
#include "matrix/detailed_cute_scoreboard.hh"
#include "matrix/matrix_backend.hh"
#include "matrix/matrix_memory_adapter.hh"
#include "matrix/matrix_regfile.hh"

namespace gem5
{

namespace matrix
{

// Detailed CUTE active state: keep this header lean and direct.
class DetailedCuteBackend : public MatrixBackend
{
  public:
    enum class MicroTaskKind : uint8_t
    {
        AML = 0,
        BML,
        CML,
        Compute,
        Release,
        Count
    };

    enum class ComputeUnitKind : uint8_t
    {
        None = 0,
        ADC,
        BDC,
        MTE,
        CDC,
        Count
    };

    enum class FifoBlockReason : uint8_t
    {
        ReleasePendingStore = 0,
        DownstreamNotAccepting = 1,
        Count = 2
    };

    struct TraceCounters
    {
        uint64_t fifoEnqueue = 0;
        uint64_t fifoDequeue = 0;
        uint64_t fifoBlock = 0;
        uint64_t scoreboardBlock = 0;
        uint64_t backendCompletion = 0;
        uint64_t microtaskIssue = 0;
        uint64_t microtaskOccupy = 0;
        uint64_t microtaskFinish = 0;
        uint64_t microtaskLatencySum = 0;
        uint64_t lastMicrotaskLatency = 0;
        std::array<uint64_t,
            static_cast<size_t>(DetailedCuteScoreboard::BlockReason::Count)>
            scoreboardBlockReasons = {};
        std::array<uint64_t, static_cast<size_t>(FifoBlockReason::Count)>
            fifoBlockReasons = {};
        std::array<uint64_t, static_cast<size_t>(MicroTaskKind::Count)>
            microtaskIssuesByKind = {};
        std::array<uint64_t, static_cast<size_t>(MicroTaskKind::Count)>
            microtaskFinishesByKind = {};
        std::array<uint64_t, static_cast<size_t>(ComputeUnitKind::Count)>
            computeUnitIssuesByKind = {};
        std::array<uint64_t, static_cast<size_t>(ComputeUnitKind::Count)>
            computeUnitOccupiesByKind = {};
        std::array<uint64_t, static_cast<size_t>(ComputeUnitKind::Count)>
            computeUnitFinishesByKind = {};
    };

    struct MteTiming
    {
        unsigned tensorMn = 0;
        unsigned tensorK = 0;
        unsigned matrixMn = 0;
        unsigned reduceWidthBytes = 0;
        unsigned resultWidthBytes = 0;
        unsigned aBytesPerBeat = 0;
        unsigned bBytesPerBeat = 0;
        unsigned cBytesPerBeat = 0;
        unsigned dBytesPerBeat = 0;
        unsigned acceptedInputBeats = 0;
        unsigned adcReadCycles = 0;
        unsigned bdcReadCycles = 0;
        unsigned mteAcceptedInputBeats = 0;
        unsigned fReduceTailCycles = 0;
        unsigned cdcWriteCycles = 0;
        unsigned terminalHandshakeCycles = 0;
        unsigned totalCompletionCycles = 0;
        bool supported = false;
    };

    explicit DetailedCuteBackend(
        std::unique_ptr<MatrixMemoryAdapter> memory_adapter =
            std::make_unique<NullMatrixMemoryAdapter>(),
        size_t fifo_depth = 8,
        size_t ab_reg_count = MatrixRegFile::DefaultAbRegCount,
        size_t c_reg_count = MatrixRegFile::DefaultCRegCount);

    bool canAccept(const CuteRequest &req) const override;
    void submit(const CuteRequest &req) override;
    bool hasWork() const override
    {
        return !fifo.empty() || !taskEvents.empty() || activeTaskCount() != 0;
    }
    void step() override;

    bool hasCompletion() const override { return !completions.empty(); }
    CuteCompletion popCompletion() override;
    bool hasArchitecturalState() const override
    {
        return regFile.hasAllocatedState();
    }

    // Canonical backend-visible matrix state accessor.
    const MatrixRegFile &matrixState() const { return regFile; }
    MatrixRegFile &matrixState() { return regFile; }
    size_t queueDepth() const { return fifo.depth(); }
    const std::string &name() const { return backendName; }
    const TraceCounters &traceCounters() const { return counters; }

  private:
    friend class DetailedCuteBackendTestProbe;

    enum class TaskStage : uint8_t
    {
        Accepted = 0,
        MemReq,
        FillPending,
        RegWrite,
        RegRead,
        StorePending,
        WaitPendingStoreClear,
        TerminalPending
    };

    enum class TaskEventKind : uint8_t
    {
        ReadFinish = 0,
        ComputeReadAFinish,
        ComputeReadBFinish,
        WriteFinish,
        TerminalCompletion
    };

    struct TaskSlot
    {
        DecodedFifoEntry entry = {};
        MicroTaskKind microTaskKind = MicroTaskKind::Release;
        TaskStage stage = TaskStage::Accepted;
        MatrixTensor bufferedTensor = {};
        MatrixTensor bufferedTensorA = {};
        MatrixTensor bufferedTensorB = {};
        CuteCompletion bufferedCompletion = {};
        bool hasBufferedTensor = false;
        bool hasBufferedTensorA = false;
        bool hasBufferedTensorB = false;
        unsigned stageCyclesRemaining = 0;
        uint64_t issueStep = 0;
        bool occupancyTraced = false;
    };

    struct ComputeTaskState
    {
        DecodedFifoEntry entry = {};
        MatrixTensor bufferedTensor = {};
        MatrixTensor bufferedTensorA = {};
        MatrixTensor bufferedTensorB = {};
        CuteCompletion bufferedCompletion = {};
        bool hasBufferedTensor = false;
        bool hasBufferedTensorA = false;
        bool hasBufferedTensorB = false;
        ComputeUnitKind activeUnit = ComputeUnitKind::None;
        bool unitWorkDone = false;
        unsigned executeCyclesRemaining = 0;
        uint64_t issueStep = 0;
        uint64_t unitIssueStep = 0;
        bool occupancyTraced = false;
        bool unitOccupancyTraced = false;
        bool terminalIssued = false;
    };

    struct TaskEvent
    {
        DecodedFifoEntry entry = {};
        MicroTaskKind microTaskKind = MicroTaskKind::Release;
        TaskEventKind kind = TaskEventKind::TerminalCompletion;
        CuteCompletion completion = {};
        uint64_t issueStep = 0;
        uint64_t readyStep = 0;
    };

    bool canDownstreamAccept(const DecodedFifoEntry &entry) const;
    bool amlReady() const { return !amlTask.has_value(); }
    bool bmlReady() const { return !bmlTask.has_value(); }
    bool cmlReady() const { return !cmlTask.has_value(); }
    bool loadPathReady(const DecodedFifoEntry &entry) const;
    bool storePathReady(const DecodedFifoEntry &entry) const;
    bool computePathReady(const DecodedFifoEntry &entry) const;
    bool arithPathReady(const DecodedFifoEntry &entry) const;
    bool releasePathReady(const DecodedFifoEntry &entry) const;
    bool computeUnitBusyForTest(ComputeUnitKind kind) const;
    ComputeUnitKind activeComputeUnitForTest() const;
    bool computeUnitAvailable(ComputeUnitKind kind) const;
    CuteCompletion executeEntry(const DecodedFifoEntry &entry);
    bool releaseReady() const
    {
        if (pendingStoreCount != 0) {
            return false;
        }
        return !amlTask.has_value() &&
               !bmlTask.has_value() &&
               !cmlTask.has_value() &&
               computeTasks.empty() &&
               taskEvents.empty();
    }
    bool headReady(const DecodedFifoEntry &entry,
                   DetailedCuteScoreboard::BlockReason &reason) const;
    void issueHead(const DecodedFifoEntry &entry);
    void processFifoHead();
    void advanceTaskSlots();
    void advanceTaskSlot(std::optional<TaskSlot> &slot);
    void advanceComputeTask();
    void traceActiveComputeTasks();
    void serviceActiveComputeUnits();
    void dispatchReadyComputeUnits();
    void advanceLoadFill(TaskSlot &task);
    void advanceStoreRead(TaskSlot &task);
    void advanceComputeReadA(ComputeTaskState &task);
    void advanceComputeReadB(ComputeTaskState &task);
    void advanceComputeExecute(ComputeTaskState &task);
    void enqueueTaskEvent(const TaskSlot &task, TaskEventKind kind,
                          CuteCompletion completion = {});
    void enqueueTaskEvent(const ComputeTaskState &task, TaskEventKind kind,
                          CuteCompletion completion = {});
    void applyWriteFinish(const DecodedFifoEntry &entry,
                          const CuteCompletion &completion,
                          MicroTaskKind kind);
    void retireTaskSlot(MicroTaskKind kind, uint64_t seq);
    void retireComputeTask(uint64_t seq);
    uint64_t finalizeCompletion(const CuteCompletion &completion,
                                MicroTaskKind kind,
                                uint64_t issueStep,
                                uint64_t activeCount);
    void processTaskEvents();
    void dispatchTask(const DecodedFifoEntry &entry);
    CuteCompletion executeTaskSlot(const TaskSlot &task);
    CuteCompletion executeLoadWrite(TaskSlot &task);
    CuteCompletion executeStoreWrite(const TaskSlot &task);
    CuteCompletion executeComputeWrite(ComputeTaskState &task);
    MteTiming computeMteTiming(const AmuMmaDesc &desc) const;
    unsigned computeExecuteLatency(const DecodedFifoEntry &entry) const;
    bool computeDatatypeSupported(const AmuMmaDesc &desc) const;
    void finishTaskSlot(std::optional<TaskSlot> &slot);
    void finishComputeTask();
    void beginComputeUnit(ComputeTaskState &task, ComputeUnitKind kind);
    CuteCompletion executeLsu(uint64_t seq, const AmuLsuDesc &desc,
                              MatrixRegFile &state,
                              MatrixMemoryAdapter &memory);
    CuteCompletion executeMma(uint64_t seq, const AmuMmaDesc &desc,
                              MatrixRegFile &state);
    CuteCompletion executeArith(uint64_t seq, const AmuArithDesc &desc,
                                MatrixRegFile &state);
    void recordComputeUnitIssue(ComputeUnitKind kind);
    void recordComputeUnitOccupy(ComputeUnitKind kind);
    void recordComputeUnitFinish(ComputeUnitKind kind);
    bool computeTaskFinishedMte(const ComputeTaskState &task) const;
    MicroTaskKind microTaskKindForEntry(const DecodedFifoEntry &entry) const;
    bool microTaskAvailable(MicroTaskKind kind) const;
    bool useMemoryBudget();
    MatrixBankKind destBank(const DecodedFifoEntry &entry) const;
    void recordScoreboardBlock(DetailedCuteScoreboard::BlockReason reason);
    void recordFifoBlock(FifoBlockReason reason);
    size_t activeTaskCount() const;
    bool loadRuntimeTile(const AmuLsuDesc &desc, MatrixTensor &out_tensor) const;
    bool storeRuntimeTile(const AmuLsuDesc &desc,
                          const MatrixTensor &tensor) const;

  private:
    DecodedFifo fifo;
    MatrixRegFile regFile;
    std::unique_ptr<MatrixMemoryAdapter> memory;
    DetailedCuteScoreboard scoreboard;
    std::optional<TaskSlot> amlTask;
    std::optional<TaskSlot> bmlTask;
    std::optional<TaskSlot> cmlTask;
    std::deque<ComputeTaskState> computeTasks;
    std::optional<TaskSlot> releaseTask;
    std::deque<TaskEvent> taskEvents;
    std::deque<CuteCompletion> completions;
    unsigned pendingStoreCount = 0;
    unsigned memoryBudget = 1;
    uint64_t backendStep = 0;
    std::string backendName = "DetailedCuteBackend";
    TraceCounters counters = {};
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_DETAILED_CUTE_BACKEND_HH__
