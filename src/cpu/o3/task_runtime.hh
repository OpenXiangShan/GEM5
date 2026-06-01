/*
 * Copyright (c) 2026 The Regents of The University of Michigan
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

#ifndef __CPU_O3_TASK_RUNTIME_HH__
#define __CPU_O3_TASK_RUNTIME_HH__

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "sim/core.hh"

namespace gem5
{

class Event;
class System;

namespace o3
{

class CPU;

enum class TaskKind : uint8_t
{
    Strong,
    Weak,
    Merge,
    Barrier,
};

enum class TaskStage : uint8_t
{
    Commit,
    IEW,
    Rename,
    Decode,
    Fetch,
    Event,
    Runtime,
};

enum class TaskLifetime : uint8_t
{
    PreAdvanceDrain,
    CrossTimeBufferAdvance,
};

enum class FutureWavefrontSkipReason : uint8_t
{
    CommitPreview,
    IEWInput,
    IEWPreview,
    RenameInput,
    RenamePreview,
    DecodeInput,
    DecodePreview,
    FetchInput,
    FetchPreview,
    NumReasons,
};

struct TaskOrderKey
{
    Cycles cycle;
    TaskStage stage;
    uint8_t phase;
    ThreadID tid;
    uint64_t localSeq;
};

class TaskRuntimeStats : public statistics::Group
{
  public:
    TaskRuntimeStats(statistics::Group *parent);

    statistics::Scalar workerThreads;
    statistics::Scalar created;
    statistics::Scalar strong;
    statistics::Scalar inlined;
    statistics::Scalar executed;
    statistics::Scalar merged;
    statistics::Scalar stageBarrierWaits;
    statistics::Scalar stageBarrierDeferredTasks;
    statistics::Scalar stageBarrierMaxDeferredTasks;
    statistics::Scalar horizonWaits;
    statistics::Scalar readyQueueSamples;
    statistics::Scalar readyQueueOccupancy;
    statistics::Scalar maxReadyQueueDepth;
    statistics::Scalar readyQueueBackpressureWaits;
    statistics::Scalar readyQueueBackpressureInlineTasks;
    statistics::Scalar inFlightCycleSamples;
    statistics::Scalar inFlightCycles;
    statistics::Scalar wavefrontPlanSamples;
    statistics::Scalar wavefrontPlanEffectiveCycles;
    statistics::Scalar wavefrontPlanTasks;
    statistics::Scalar wavefrontPlanEdges;
    statistics::Scalar wavefrontPlanCriticalPathLen;
    statistics::Scalar wavefrontPlanMaxReadyTasks;
    statistics::Scalar wavefrontPlanReadySlack;
    statistics::Scalar eventHorizonSamples;
    statistics::Scalar eventHorizonCandidateCycles;
    statistics::Scalar eventHorizonCommittableCycles;
    statistics::Scalar eventHorizonLimitedCycles;
    statistics::Scalar eventHorizonBlockedSamples;
    statistics::Scalar eventHorizonSameTickBlocks;
    statistics::Scalar eventHorizonZeroCycleBlocks;
    statistics::Scalar eventHorizonPartialWindowBlocks;
    statistics::Scalar eventHorizonEarlierTickBlocks;
    statistics::Scalar eventHorizonCpuPriorityBlocks;
    statistics::Scalar eventHorizonMaxBlockedOffset;
    statistics::Scalar eventHorizonMaxCommittableCycles;
    statistics::Vector eventHorizonBlockReasons;
    statistics::Vector eventHorizonBlockerTypes;
    statistics::Vector eventHorizonEarlierTickBlockerTypes;
    statistics::Vector eventHorizonCpuPriorityBlockerTypes;
    statistics::Scalar stallSignalWindowSamples;
    statistics::Scalar stallSignalWindowCapacity;
    statistics::Scalar stallSignalWindowValidSlots;
    statistics::Scalar stallSignalWindowMaxValidSlots;
    statistics::Scalar stallSignalWindowEdgesCaptured;
    statistics::Vector stallSignalMerges;
    statistics::Scalar stallSignalInputReads;
    statistics::Scalar stallSignalInputReadFallbacks;
    statistics::Scalar stallSignalFutureReadBlocks;
    statistics::Scalar specPrepared;
    statistics::Scalar specDiscarded;
    statistics::Scalar specThrottled;
    statistics::Vector stageWeakTasks;
    statistics::Vector stageWeakWork;
    statistics::Vector stageWeakMerges;
    statistics::Vector stageInlineTasks;
    statistics::Vector stageTaskRunHostNs;
    statistics::Vector stageTaskMergeHostNs;
    statistics::Scalar timeBufferInputSnapshots;
    statistics::Scalar timeBufferOutputSnapshots;
    statistics::Scalar timeBufferSlotsCaptured;
    statistics::Scalar timeBufferSnapshotWindowSamples;
    statistics::Scalar timeBufferSnapshotWindowCapacity;
    statistics::Scalar timeBufferInputWindowValidFrames;
    statistics::Scalar timeBufferOutputWindowValidFrames;
    statistics::Scalar timeBufferSnapshotWindowMaxValidFrames;
    statistics::Scalar timeBufferStageInputReads;
    statistics::Scalar timeBufferStageInputReadMisses;
    statistics::Scalar timeBufferBackwardSlotReads;
    statistics::Scalar timeBufferBackwardSlotReadMisses;
    statistics::Scalar timeBufferFetchToDecodeSlotReads;
    statistics::Scalar timeBufferFetchToDecodeSlotReadMisses;
    statistics::Scalar timeBufferDecodeToRenameSlotReads;
    statistics::Scalar timeBufferDecodeToRenameSlotReadMisses;
    statistics::Scalar timeBufferRenameToIEWSlotReads;
    statistics::Scalar timeBufferRenameToIEWSlotReadMisses;
    statistics::Scalar timeBufferRenameToCommitSlotReads;
    statistics::Scalar timeBufferRenameToCommitSlotReadMisses;
    statistics::Scalar timeBufferIEWToCommitSlotReads;
    statistics::Scalar timeBufferIEWToCommitSlotReadMisses;
    statistics::Scalar timeBufferFetchBackwardSlotReads;
    statistics::Scalar timeBufferFetchBackwardSlotReadMisses;
    statistics::Scalar timeBufferPrepareMerges;
    statistics::Scalar timeBufferPreparedInstRefs;
    statistics::Scalar timeBufferPreparedControlSignals;
    statistics::Scalar timeBufferPreparedResolvedCFIs;
    statistics::Scalar timeBufferAdvanceWaits;
    statistics::Scalar timeBufferAdvancePendingTasks;
    statistics::Scalar timeBufferAdvanceMaxPendingTasks;
    statistics::Scalar timeBufferAdvanceSafeDeferrals;
    statistics::Scalar timeBufferAdvanceSafeDeferredTasks;
    statistics::Scalar timeBufferAdvanceMaxSafeDeferredTasks;
    statistics::Scalar serialTickEndSafeDeferrals;
    statistics::Scalar serialTickEndSafeDeferredTasks;
    statistics::Scalar serialTickEndMaxSafeDeferredTasks;
    statistics::Scalar futureTimeBufferInputSnapshots;
    statistics::Scalar futureTimeBufferInputSnapshotSlots;
    statistics::Scalar futureTimeBufferPrepareMerges;
    statistics::Scalar futureTimeBufferPrepareSkipped;
    statistics::Scalar futureTimeBufferPrepareReuses;
    statistics::Scalar futureTimeBufferPrepareChecks;
    statistics::Scalar futureTimeBufferPrepareMatches;
    statistics::Scalar futureTimeBufferPrepareMismatches;
    statistics::Scalar futureTimeBufferPrepareStale;
    statistics::Scalar futureTimeBufferPreparedInstRefs;
    statistics::Scalar futureTimeBufferPreparedControlSignals;
    statistics::Scalar futureTimeBufferPreparedResolvedCFIs;
    statistics::Scalar futureWavefrontPrepareProbes;
    statistics::Scalar futureWavefrontPrepareMerges;
    statistics::Scalar futureWavefrontPrepareSkipped;
    statistics::Scalar futureWavefrontPrepareChecks;
    statistics::Scalar futureWavefrontPrepareMatches;
    statistics::Scalar futureWavefrontPrepareMismatches;
    statistics::Scalar futureWavefrontPrepareStale;
    statistics::Scalar futureRenameWavefrontPrepareProbes;
    statistics::Scalar futureRenameWavefrontPrepareMerges;
    statistics::Scalar futureRenameWavefrontPrepareSkipped;
    statistics::Scalar futureRenameWavefrontPrepareChecks;
    statistics::Scalar futureRenameWavefrontPrepareMatches;
    statistics::Scalar futureRenameWavefrontPrepareMismatches;
    statistics::Scalar futureRenameWavefrontPrepareStale;
    statistics::Scalar futureDecodeWavefrontPrepareProbes;
    statistics::Scalar futureDecodeWavefrontPrepareMerges;
    statistics::Scalar futureDecodeWavefrontPrepareSkipped;
    statistics::Scalar futureDecodeWavefrontPrepareChecks;
    statistics::Scalar futureDecodeWavefrontPrepareMatches;
    statistics::Scalar futureDecodeWavefrontPrepareMismatches;
    statistics::Scalar futureDecodeWavefrontPrepareStale;
    statistics::Scalar futureFetchWavefrontPrepareProbes;
    statistics::Scalar futureFetchWavefrontPrepareMerges;
    statistics::Scalar futureFetchWavefrontPrepareSkipped;
    statistics::Scalar futureFetchWavefrontPrepareChecks;
    statistics::Scalar futureFetchWavefrontPrepareMatches;
    statistics::Scalar futureFetchWavefrontPrepareMismatches;
    statistics::Scalar futureFetchWavefrontPrepareStale;
    statistics::Vector futureWavefrontSkipReasons;
    statistics::Scalar workerBusyHostNs;
    statistics::Scalar workerIdleHostNs;
    statistics::Scalar steals;
};

class TaskRuntime
{
  public:
    using TaskFn = std::function<void()>;
    using MergeFn = std::function<void()>;

    struct PipelineStageDelays
    {
        Cycles fetchToDecode;
        Cycles decodeToRename;
        Cycles renameToIEW;
        Cycles renameToCommit;
        Cycles iewToCommit;
    };

    TaskRuntime(CPU *cpu, const System *system);
    ~TaskRuntime();

    bool enabled() const { return enabled_; }
    bool deterministic() const { return deterministic_; }
    bool traceEnabled() const { return trace_; }
    bool selfTestEnabled() const { return selfTest_; }
    bool timeBufferPrepareEnabled() const { return trace_; }
    bool eventPriorityAuditEnabled() const { return eventPriorityAudit_; }

    unsigned workerThreads() const { return workerThreads_; }
    unsigned windowCycles() const { return windowCycles_; }
    unsigned maxInFlightCycles() const { return maxInFlightCycles_; }
    unsigned maxReadyTasks() const { return maxReadyTasks_; }
    unsigned taskMinWork() const { return taskMinWork_; }
    unsigned maxSpecTaskWaste() const { return maxSpecTaskWaste_; }

    void submitWeak(TaskOrderKey order, unsigned estimatedWork, TaskFn run,
                    MergeFn merge = {},
                    TaskLifetime lifetime =
                        TaskLifetime::PreAdvanceDrain);
    template <class Fn>
    void
    runStrong(TaskOrderKey order, Fn &&run)
    {
        if (!enabled_) {
            run();
            return;
        }

        runStrongImpl(order, TaskFn(std::forward<Fn>(run)));
    }

    void waitForOrder(TaskOrderKey barrier);
    void waitForAll();
    void waitForPreAdvance();
    void drain();
    uint64_t pendingTaskCount() const;
    uint64_t pendingPreAdvanceTaskCount() const;

    void onSerialTickBegin(Cycles cycle);
    void onSerialTickEnd(Cycles cycle, bool deferSafeTasks);
    void recordWavefrontPlan(Cycles cycle, PipelineStageDelays delays);
    void recordEventHorizon(Cycles cycle, unsigned candidate_cycles,
                            unsigned committable_cycles,
                            bool has_next_event, Tick next_event_tick,
                            int next_event_priority,
                            unsigned blocked_offset,
                            bool blocked_by_earlier_tick_event,
                            const Event *blocker_event);
    void recordStallSignalWindow(Cycles cycle, unsigned capacity,
                                 unsigned valid_slots,
                                 unsigned edges_captured);
    void recordStallSignalMerge(unsigned edge_index, uint64_t writes);
    void recordStallSignalInputRead(bool hit);
    void recordStallSignalFutureReadBlock();
    void recordTimeBufferSnapshot(bool input, unsigned slots);
    void recordTimeBufferSnapshotWindow(unsigned capacity,
                                        unsigned input_valid_frames,
                                        unsigned output_valid_frames);
    void recordTimeBufferStageInputRead(bool hit);
    void recordTimeBufferBackwardSlotRead(bool hit);
    void recordTimeBufferFetchToDecodeSlotRead(bool hit);
    void recordTimeBufferDecodeToRenameSlotRead(bool hit);
    void recordTimeBufferRenameToIEWSlotRead(bool hit);
    void recordTimeBufferRenameToCommitSlotRead(bool hit);
    void recordTimeBufferIEWToCommitSlotRead(bool hit);
    void recordTimeBufferFetchBackwardSlotRead(bool hit);
    void recordTimeBufferPrepareMerge(uint64_t inst_refs,
                                      uint64_t control_signals,
                                      uint64_t resolved_cfis);
    void recordTimeBufferAdvanceWait(Cycles cycle,
                                     uint64_t pending_pre_advance_tasks,
                                     uint64_t safe_deferred_tasks);
    void recordFutureTimeBufferSnapshot(unsigned slots);
    void recordFutureTimeBufferPrepareMerge(uint64_t inst_refs,
                                            uint64_t control_signals,
                                            uint64_t resolved_cfis);
    void recordFutureTimeBufferPrepareSkipped();
    bool speculativePrepareAllowed() const;
    void recordSpecTaskThrottled();
    void recordSpecTaskDiscarded();
    void recordFutureTimeBufferPrepareReuse();
    void recordFutureTimeBufferPrepareCheck(bool cycle_match,
                                            bool summary_match);
    void recordFutureWavefrontPrepareProbe();
    void recordFutureWavefrontPrepareMerge();
    void recordFutureWavefrontPrepareSkipped();
    void recordFutureWavefrontPrepareCheck(bool cycle_match,
                                           bool latch_match);
    void recordFutureRenameWavefrontPrepareProbe();
    void recordFutureRenameWavefrontPrepareMerge();
    void recordFutureRenameWavefrontPrepareSkipped();
    void recordFutureRenameWavefrontPrepareCheck(bool cycle_match,
                                                 bool latch_match);
    void recordFutureDecodeWavefrontPrepareProbe();
    void recordFutureDecodeWavefrontPrepareMerge();
    void recordFutureDecodeWavefrontPrepareSkipped();
    void recordFutureDecodeWavefrontPrepareCheck(bool cycle_match,
                                                 bool latch_match);
    void recordFutureFetchWavefrontPrepareProbe();
    void recordFutureFetchWavefrontPrepareMerge();
    void recordFutureFetchWavefrontPrepareSkipped();
    void recordFutureFetchWavefrontPrepareCheck(bool cycle_match,
                                                bool output_match);
    void recordFutureWavefrontSkipReason(
            FutureWavefrontSkipReason reason);

    TaskRuntimeStats &stats() { return stats_; }
    const TaskRuntimeStats &stats() const { return stats_; }

  private:
    struct Task;
    struct WavefrontPlanMetrics
    {
        bool valid = false;
        PipelineStageDelays delays;
        unsigned effectiveWindow = 0;
        unsigned tasks = 0;
        unsigned edges = 0;
        unsigned criticalPathLen = 0;
        unsigned maxReadyTasks = 0;
        unsigned readySlack = 0;
    };

    bool shouldInline(unsigned estimatedWork) const;
    void startWorkers();
    void workerLoop(unsigned workerId);
    void stopWorkers();
    void flushWorkerStats();
    void mergeCompletedTasks(std::vector<std::unique_ptr<Task>> &completed);
    void waitForBarrier(TaskOrderKey barrier);
    void runSelfTest();
    void runStrongImpl(TaskOrderKey order, TaskFn run);

    CPU *cpu;
    bool enabled_ = false;
    bool deterministic_ = true;
    bool trace_ = false;
    bool selfTest_ = false;
    bool eventPriorityAudit_ = false;
    unsigned workerThreads_ = 0;
    unsigned windowCycles_ = 1;
    unsigned taskMinWork_ = 0;
    unsigned maxInFlightCycles_ = 1;
    unsigned maxReadyTasks_ = 0;
    unsigned maxSpecTaskWaste_ = 100;

    bool workersStarted_ = false;
    bool stopping_ = false;
    uint64_t nextTaskSeq_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable workAvailable_;
    std::condition_variable workDone_;
    std::deque<Task *> ready_;
    std::vector<std::unique_ptr<Task>> inFlight_;
    std::vector<std::thread> workers_;
    std::atomic<uint64_t> pendingExecutedStats_{0};
    std::atomic<uint64_t> pendingWorkerBusyNs_{0};
    std::atomic<uint64_t> pendingWorkerIdleNs_{0};
    WavefrontPlanMetrics wavefrontPlan_;

    TaskRuntimeStats stats_;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TASK_RUNTIME_HH__
