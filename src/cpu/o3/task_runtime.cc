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

#include "cpu/o3/task_runtime.hh"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "debug/TaskGraph.hh"
#include "debug/TaskSched.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

namespace
{

constexpr unsigned TaskRuntimeSelfTestTasks = 4;
constexpr unsigned DefaultTaskParallelWorkerThreads = 2;
constexpr unsigned NumTaskStages = 7;
constexpr unsigned NumPipelineTaskStages = 5;
constexpr unsigned NumEventHorizonBlockReasons = 8;
constexpr unsigned NumEventHorizonBlockerTypes = 17;
constexpr unsigned NumStallSignalEdges = 4;
constexpr unsigned NumFutureWavefrontSkipReasons =
    static_cast<unsigned>(FutureWavefrontSkipReason::NumReasons);
using HostClock = std::chrono::steady_clock;

enum PipelineStageIndex : uint8_t
{
    PipelineCommit,
    PipelineIEW,
    PipelineRename,
    PipelineDecode,
    PipelineFetch,
};

enum EventHorizonBlockReasonIndex : uint8_t
{
    HorizonEarlierTick,
    HorizonEarlyPriority,
    HorizonCpuSwitch,
    HorizonDelayedWriteback,
    HorizonDefault,
    HorizonDvfsSerialize,
    HorizonCpuTick,
    HorizonOtherCpuPriority,
};

enum EventHorizonBlockerTypeIndex : uint8_t
{
    HorizonBlockerMemoryController,
    HorizonBlockerL1Cache,
    HorizonBlockerL2Slice,
    HorizonBlockerL2Wrapper,
    HorizonBlockerL2Other,
    HorizonBlockerL3MemSidePort,
    HorizonBlockerL3Cache,
    HorizonBlockerL1Prefetcher,
    HorizonBlockerL2WrapperPrefetcher,
    HorizonBlockerL2SlicePrefetcher,
    HorizonBlockerL2OtherPrefetcher,
    HorizonBlockerL3Prefetcher,
    HorizonBlockerOtherPrefetcher,
    HorizonBlockerInterconnect,
    HorizonBlockerCPU,
    HorizonBlockerDevice,
    HorizonBlockerOther,
};

const char *
eventHorizonBlockReasonName(unsigned reason)
{
    switch (reason) {
      case HorizonEarlierTick:
        return "EarlierTick";
      case HorizonEarlyPriority:
        return "EarlyPriority";
      case HorizonCpuSwitch:
        return "CpuSwitch";
      case HorizonDelayedWriteback:
        return "DelayedWriteback";
      case HorizonDefault:
        return "Default";
      case HorizonDvfsSerialize:
        return "DvfsSerialize";
      case HorizonCpuTick:
        return "CpuTick";
      case HorizonOtherCpuPriority:
        return "OtherCpuPriority";
    }

    return "Unknown";
}

const char *
eventHorizonBlockerTypeName(unsigned type)
{
    switch (type) {
      case HorizonBlockerMemoryController:
        return "MemoryController";
      case HorizonBlockerL1Cache:
        return "L1Cache";
      case HorizonBlockerL2Slice:
        return "L2Slice";
      case HorizonBlockerL2Wrapper:
        return "L2Wrapper";
      case HorizonBlockerL2Other:
        return "L2Other";
      case HorizonBlockerL3MemSidePort:
        return "L3MemSidePort";
      case HorizonBlockerL3Cache:
        return "L3Cache";
      case HorizonBlockerL1Prefetcher:
        return "L1Prefetcher";
      case HorizonBlockerL2WrapperPrefetcher:
        return "L2WrapperPrefetcher";
      case HorizonBlockerL2SlicePrefetcher:
        return "L2SlicePrefetcher";
      case HorizonBlockerL2OtherPrefetcher:
        return "L2OtherPrefetcher";
      case HorizonBlockerL3Prefetcher:
        return "L3Prefetcher";
      case HorizonBlockerOtherPrefetcher:
        return "OtherPrefetcher";
      case HorizonBlockerInterconnect:
        return "Interconnect";
      case HorizonBlockerCPU:
        return "CPU";
      case HorizonBlockerDevice:
        return "Device";
      case HorizonBlockerOther:
        return "Other";
    }

    return "Unknown";
}

const char *
stallSignalEdgeName(unsigned edge)
{
    switch (edge) {
      case 0:
        return "CommitToIEW";
      case 1:
        return "IEWToRename";
      case 2:
        return "RenameToDecode";
      case 3:
        return "DecodeToFetch";
    }

    return "Unknown";
}

const char *
futureWavefrontSkipReasonName(unsigned reason)
{
    switch (static_cast<FutureWavefrontSkipReason>(reason)) {
      case FutureWavefrontSkipReason::CommitPreview:
        return "CommitPreview";
      case FutureWavefrontSkipReason::IEWInput:
        return "IEWInput";
      case FutureWavefrontSkipReason::IEWPreview:
        return "IEWPreview";
      case FutureWavefrontSkipReason::RenameInput:
        return "RenameInput";
      case FutureWavefrontSkipReason::RenamePreview:
        return "RenamePreview";
      case FutureWavefrontSkipReason::DecodeInput:
        return "DecodeInput";
      case FutureWavefrontSkipReason::DecodePreview:
        return "DecodePreview";
      case FutureWavefrontSkipReason::FetchInput:
        return "FetchInput";
      case FutureWavefrontSkipReason::FetchPreview:
        return "FetchPreview";
      case FutureWavefrontSkipReason::NumReasons:
        break;
    }

    return "Unknown";
}

unsigned
eventHorizonBlockReason(bool blocked_by_earlier_tick_event,
                        int next_event_priority)
{
    if (blocked_by_earlier_tick_event)
        return HorizonEarlierTick;
    if (next_event_priority < Event::CPU_Switch_Pri)
        return HorizonEarlyPriority;
    if (next_event_priority == Event::CPU_Switch_Pri)
        return HorizonCpuSwitch;
    if (next_event_priority == Event::Delayed_Writeback_Pri)
        return HorizonDelayedWriteback;
    if (next_event_priority == Event::Default_Pri)
        return HorizonDefault;
    if (next_event_priority == Event::DVFS_Update_Pri ||
        next_event_priority == Event::Serialize_Pri) {
        return HorizonDvfsSerialize;
    }
    if (next_event_priority == Event::CPU_Tick_Pri)
        return HorizonCpuTick;

    return HorizonOtherCpuPriority;
}

unsigned
eventHorizonBlockerType(const Event *event)
{
    if (!event)
        return HorizonBlockerOther;

    const std::string name = event->name();
    if (name.find("mem_ctrl") != std::string::npos ||
        name.find("mem_ctrls") != std::string::npos) {
        return HorizonBlockerMemoryController;
    }
    if (name.find("prefetcher") != std::string::npos) {
        if (name.find(".dcache") != std::string::npos ||
            name.find(".icache") != std::string::npos) {
            return HorizonBlockerL1Prefetcher;
        }
        if (name.find("l2_wrappers.slices") != std::string::npos)
            return HorizonBlockerL2SlicePrefetcher;
        if (name.find("l2_wrappers") != std::string::npos)
            return HorizonBlockerL2WrapperPrefetcher;
        if (name.find(".l2") != std::string::npos ||
            name.find("l2_") != std::string::npos) {
            return HorizonBlockerL2OtherPrefetcher;
        }
        if (name.find(".l3") != std::string::npos ||
            name.find("l3_") != std::string::npos) {
            return HorizonBlockerL3Prefetcher;
        }
        return HorizonBlockerOtherPrefetcher;
    }
    if (name.find(".dcache") != std::string::npos ||
        name.find(".icache") != std::string::npos) {
        return HorizonBlockerL1Cache;
    }
    if (name.find("l2_wrappers.slices") != std::string::npos)
        return HorizonBlockerL2Slice;
    if (name.find("l2_wrappers") != std::string::npos)
        return HorizonBlockerL2Wrapper;
    if (name.find(".l2") != std::string::npos ||
        name.find("l2_") != std::string::npos) {
        return HorizonBlockerL2Other;
    }
    if (name.find("l3.mem_side_port") != std::string::npos ||
        name.find("l3.mem_side") != std::string::npos) {
        return HorizonBlockerL3MemSidePort;
    }
    if (name.find(".l3") != std::string::npos ||
        name.find("l3_") != std::string::npos) {
        return HorizonBlockerL3Cache;
    }
    if (name.find("bus") != std::string::npos ||
        name.find("xbar") != std::string::npos ||
        name.find("crossbar") != std::string::npos) {
        return HorizonBlockerInterconnect;
    }
    if (name.find(".cpu") != std::string::npos ||
        name.find("cpu.") != std::string::npos) {
        return HorizonBlockerCPU;
    }
    if (name.find("uart") != std::string::npos ||
        name.find("plic") != std::string::npos ||
        name.find("clint") != std::string::npos ||
        name.find(".device") != std::string::npos) {
        return HorizonBlockerDevice;
    }

    return HorizonBlockerOther;
}

const char *
taskStageName(TaskStage stage)
{
    switch (stage) {
      case TaskStage::Commit:
        return "Commit";
      case TaskStage::IEW:
        return "IEW";
      case TaskStage::Rename:
        return "Rename";
      case TaskStage::Decode:
        return "Decode";
      case TaskStage::Fetch:
        return "Fetch";
      case TaskStage::Event:
        return "Event";
      case TaskStage::Runtime:
        return "Runtime";
    }

    return "Unknown";
}

unsigned
taskStageIndex(TaskStage stage)
{
    const unsigned index = static_cast<unsigned>(stage);
    fatal_if(index >= NumTaskStages, "Unknown task stage index %u.", index);
    return index;
}

bool
taskOrderLess(const TaskOrderKey &lhs, const TaskOrderKey &rhs)
{
    if (lhs.cycle != rhs.cycle)
        return lhs.cycle < rhs.cycle;
    if (lhs.stage != rhs.stage)
        return static_cast<uint8_t>(lhs.stage) <
               static_cast<uint8_t>(rhs.stage);
    if (lhs.phase != rhs.phase)
        return lhs.phase < rhs.phase;
    if (lhs.tid != rhs.tid)
        return lhs.tid < rhs.tid;
    return lhs.localSeq < rhs.localSeq;
}

bool
taskOrderLessOrEqual(const TaskOrderKey &lhs, const TaskOrderKey &rhs)
{
    return !taskOrderLess(rhs, lhs);
}

unsigned
pipelineNodeIndex(unsigned cycle, PipelineStageIndex stage)
{
    return cycle * NumPipelineTaskStages + static_cast<unsigned>(stage);
}

} // anonymous namespace

struct TaskRuntime::Task
{
    uint64_t seq = 0;
    TaskOrderKey order;
    TaskFn run;
    MergeFn merge;
    uint64_t runHostNs = 0;
    TaskLifetime lifetime = TaskLifetime::PreAdvanceDrain;
    bool done = false;
    std::exception_ptr exception;
};

TaskRuntimeStats::TaskRuntimeStats(statistics::Group *parent)
    : statistics::Group(parent, "taskRuntime"),
      ADD_STAT(workerThreads, statistics::units::Count::get(),
               "Resolved number of host worker threads used by the "
               "task-parallel runtime"),
      ADD_STAT(created, statistics::units::Count::get(),
               "Number of task-runtime tasks created"),
      ADD_STAT(strong, statistics::units::Count::get(),
               "Number of task-runtime strong tasks executed on the owner "
               "event thread"),
      ADD_STAT(inlined, statistics::units::Count::get(),
               "Number of task-runtime tasks executed inline"),
      ADD_STAT(executed, statistics::units::Count::get(),
               "Number of task-runtime tasks executed by workers"),
      ADD_STAT(merged, statistics::units::Count::get(),
               "Number of task-runtime task results merged"),
      ADD_STAT(stageBarrierWaits, statistics::units::Count::get(),
               "Number of waits on stage-order barriers"),
      ADD_STAT(stageBarrierDeferredTasks, statistics::units::Count::get(),
               "Accumulated weak tasks left pending across stage-order "
               "barriers because their task order is later than the barrier"),
      ADD_STAT(stageBarrierMaxDeferredTasks, statistics::units::Count::get(),
               "Maximum weak tasks left pending across one stage-order "
               "barrier"),
      ADD_STAT(horizonWaits, statistics::units::Count::get(),
               "Number of waits on the event/task horizon"),
      ADD_STAT(readyQueueSamples, statistics::units::Count::get(),
               "Number of ready-queue occupancy samples"),
      ADD_STAT(readyQueueOccupancy, statistics::units::Count::get(),
               "Accumulated ready-queue occupancy across samples"),
      ADD_STAT(maxReadyQueueDepth, statistics::units::Count::get(),
               "Maximum task-runtime ready queue depth"),
      ADD_STAT(readyQueueBackpressureWaits, statistics::units::Count::get(),
               "Number of task submissions that waited because "
               "maxReadyTasks capped the ready queue"),
      ADD_STAT(readyQueueBackpressureInlineTasks,
               statistics::units::Count::get(),
               "Number of tasks run inline after ready-queue "
               "backpressure"),
      ADD_STAT(inFlightCycleSamples, statistics::units::Count::get(),
               "Number of in-flight cycle window samples"),
      ADD_STAT(inFlightCycles, statistics::units::Cycle::get(),
               "Accumulated in-flight task window cycles"),
      ADD_STAT(wavefrontPlanSamples, statistics::units::Count::get(),
               "Number of static wavefront DAG planning samples"),
      ADD_STAT(wavefrontPlanEffectiveCycles, statistics::units::Cycle::get(),
               "Accumulated effective cycles in static wavefront DAG "
               "planning windows"),
      ADD_STAT(wavefrontPlanTasks, statistics::units::Count::get(),
               "Number of pipeline stage tasks represented in static "
               "wavefront DAG planning windows"),
      ADD_STAT(wavefrontPlanEdges, statistics::units::Count::get(),
               "Number of dependency edges represented in static wavefront "
               "DAG planning windows"),
      ADD_STAT(wavefrontPlanCriticalPathLen, statistics::units::Count::get(),
               "Accumulated ASAP critical-path length of static wavefront "
               "DAG planning windows"),
      ADD_STAT(wavefrontPlanMaxReadyTasks, statistics::units::Count::get(),
               "Maximum ready-set width observed in static wavefront DAG "
               "planning windows"),
      ADD_STAT(wavefrontPlanReadySlack, statistics::units::Count::get(),
               "Accumulated ready tasks beyond one per ASAP wavefront step"),
      ADD_STAT(eventHorizonSamples, statistics::units::Count::get(),
               "Number of event-horizon planning samples"),
      ADD_STAT(eventHorizonCandidateCycles, statistics::units::Cycle::get(),
               "Accumulated candidate CPU cycles before applying the event "
               "horizon"),
      ADD_STAT(eventHorizonCommittableCycles, statistics::units::Cycle::get(),
               "Accumulated CPU cycles that could be committed without "
               "crossing the current EventQueue horizon"),
      ADD_STAT(eventHorizonLimitedCycles, statistics::units::Cycle::get(),
               "Accumulated candidate CPU cycles blocked by the current "
               "EventQueue horizon"),
      ADD_STAT(eventHorizonBlockedSamples, statistics::units::Count::get(),
               "Number of samples where the EventQueue horizon limited the "
               "candidate CPU window"),
      ADD_STAT(eventHorizonSameTickBlocks, statistics::units::Count::get(),
               "Number of event-horizon samples blocked by a same-tick "
               "event that must run before any future CPU cycle"),
      ADD_STAT(eventHorizonZeroCycleBlocks, statistics::units::Count::get(),
               "Number of event-horizon samples where no future CPU cycle "
               "could be committed"),
      ADD_STAT(eventHorizonPartialWindowBlocks,
               statistics::units::Count::get(),
               "Number of event-horizon samples where at least one future "
               "CPU cycle was committable but the candidate window was "
               "still truncated"),
      ADD_STAT(eventHorizonEarlierTickBlocks,
               statistics::units::Count::get(),
               "Number of event-horizon samples blocked by an event before "
               "the first non-committable future CPU tick"),
      ADD_STAT(eventHorizonCpuPriorityBlocks,
               statistics::units::Count::get(),
               "Number of event-horizon samples blocked by an event at the "
               "same future CPU tick with priority not later than CPU_Tick"),
      ADD_STAT(eventHorizonMaxBlockedOffset,
               statistics::units::Count::get(),
               "Maximum future-cycle offset at which the EventQueue horizon "
               "first blocked the candidate CPU window"),
      ADD_STAT(eventHorizonMaxCommittableCycles,
               statistics::units::Cycle::get(),
               "Maximum CPU cycles that could be committed in one sample "
               "without crossing the current EventQueue horizon"),
      ADD_STAT(eventHorizonBlockReasons, statistics::units::Count::get(),
               "EventQueue horizon block reason by priority class"),
      ADD_STAT(eventHorizonBlockerTypes, statistics::units::Count::get(),
               "EventQueue horizon blocker event type. This is populated "
               "only when event-priority audit is enabled"),
      ADD_STAT(eventHorizonEarlierTickBlockerTypes,
               statistics::units::Count::get(),
               "EventQueue horizon blocker event type for blockers at a tick "
               "earlier than the first blocked future CPU tick. This is "
               "populated only when event-priority audit is enabled"),
      ADD_STAT(eventHorizonCpuPriorityBlockerTypes,
               statistics::units::Count::get(),
               "EventQueue horizon blocker event type for blockers at a "
               "future CPU tick with priority not later than CPU_Tick. This "
               "is populated only when event-priority audit is enabled"),
      ADD_STAT(stallSignalWindowSamples, statistics::units::Count::get(),
               "Number of per-cycle stall-signal window samples"),
      ADD_STAT(stallSignalWindowCapacity, statistics::units::Count::get(),
               "Accumulated configured per-cycle stall-signal window "
               "capacity"),
      ADD_STAT(stallSignalWindowValidSlots, statistics::units::Count::get(),
               "Accumulated valid per-cycle stall-signal slots"),
      ADD_STAT(stallSignalWindowMaxValidSlots, statistics::units::Count::get(),
               "Maximum valid per-cycle stall-signal slots in the window"),
      ADD_STAT(stallSignalWindowEdgesCaptured, statistics::units::Count::get(),
               "Number of stall-signal edge latches captured into the "
               "per-cycle window"),
      ADD_STAT(stallSignalMerges, statistics::units::Count::get(),
               "Number of typed stall-signal edge writes merged by the "
               "owner stage, grouped by edge"),
      ADD_STAT(stallSignalInputReads, statistics::units::Count::get(),
               "Number of stage input reads from the per-cycle stall-signal "
               "snapshot window"),
      ADD_STAT(stallSignalInputReadFallbacks,
               statistics::units::Count::get(),
               "Number of stage input stall-signal reads that fell back to "
               "the current latch"),
      ADD_STAT(stallSignalFutureReadBlocks,
               statistics::units::Count::get(),
               "Number of future or non-current stall-signal snapshot reads "
               "blocked by a missing cycle slot"),
      ADD_STAT(specPrepared, statistics::units::Count::get(),
               "Number of speculatively prepared tasks"),
      ADD_STAT(specDiscarded, statistics::units::Count::get(),
               "Number of speculatively prepared tasks discarded"),
      ADD_STAT(specThrottled, statistics::units::Count::get(),
               "Number of ticks where speculative prepare was disabled by "
               "the configured waste threshold"),
      ADD_STAT(stageWeakTasks, statistics::units::Count::get(),
               "Number of weak tasks submitted by task stage"),
      ADD_STAT(stageWeakWork, statistics::units::Count::get(),
               "Estimated weak-task work submitted by task stage"),
      ADD_STAT(stageWeakMerges, statistics::units::Count::get(),
               "Number of weak task merges by task stage"),
      ADD_STAT(stageInlineTasks, statistics::units::Count::get(),
               "Number of weak tasks executed inline by task stage"),
      ADD_STAT(stageTaskRunHostNs, statistics::units::Count::get(),
               "Host nanoseconds spent running weak task functions by task "
               "stage, including worker and inline execution"),
      ADD_STAT(stageTaskMergeHostNs, statistics::units::Count::get(),
               "Host nanoseconds spent applying weak task merge functions "
               "by task stage on the owner thread"),
      ADD_STAT(timeBufferInputSnapshots, statistics::units::Count::get(),
               "Number of TimeBuffer input snapshot frames captured"),
      ADD_STAT(timeBufferOutputSnapshots, statistics::units::Count::get(),
               "Number of TimeBuffer output snapshot frames captured"),
      ADD_STAT(timeBufferSlotsCaptured, statistics::units::Count::get(),
               "Number of TimeBuffer slots copied into task snapshots"),
      ADD_STAT(timeBufferSnapshotWindowSamples,
               statistics::units::Count::get(),
               "Number of per-cycle TimeBuffer snapshot window samples"),
      ADD_STAT(timeBufferSnapshotWindowCapacity,
               statistics::units::Count::get(),
               "Accumulated configured TimeBuffer snapshot window capacity"),
      ADD_STAT(timeBufferInputWindowValidFrames,
               statistics::units::Count::get(),
               "Accumulated valid input frames in the TimeBuffer snapshot "
               "window"),
      ADD_STAT(timeBufferOutputWindowValidFrames,
               statistics::units::Count::get(),
               "Accumulated valid output frames in the TimeBuffer snapshot "
               "window"),
      ADD_STAT(timeBufferSnapshotWindowMaxValidFrames,
               statistics::units::Count::get(),
               "Maximum valid frames observed in either TimeBuffer snapshot "
               "window"),
      ADD_STAT(timeBufferStageInputReads, statistics::units::Count::get(),
               "Number of stage reads from the TimeBuffer input "
               "snapshot frame"),
      ADD_STAT(timeBufferStageInputReadMisses,
               statistics::units::Count::get(),
               "Number of stage TimeBuffer input snapshot reads "
               "that fell back to live wires"),
      ADD_STAT(timeBufferBackwardSlotReads,
               statistics::units::Count::get(),
               "Number of stage prepare reads from TimeBuffer backward "
               "snapshot slots"),
      ADD_STAT(timeBufferBackwardSlotReadMisses,
               statistics::units::Count::get(),
               "Number of stage prepare TimeBuffer backward slot reads "
               "that fell back to live wires"),
      ADD_STAT(timeBufferFetchToDecodeSlotReads,
               statistics::units::Count::get(),
               "Number of Decode reads from TimeBuffer Fetch-to-Decode "
               "input snapshot slots"),
      ADD_STAT(timeBufferFetchToDecodeSlotReadMisses,
               statistics::units::Count::get(),
               "Number of Decode Fetch-to-Decode input snapshot slot reads "
               "that fell back to live wires"),
      ADD_STAT(timeBufferDecodeToRenameSlotReads,
               statistics::units::Count::get(),
               "Number of Rename reads from TimeBuffer Decode-to-Rename "
               "input snapshot slots"),
      ADD_STAT(timeBufferDecodeToRenameSlotReadMisses,
               statistics::units::Count::get(),
               "Number of Rename Decode-to-Rename input snapshot slot reads "
               "that fell back to live wires"),
      ADD_STAT(timeBufferRenameToIEWSlotReads,
               statistics::units::Count::get(),
               "Number of IEW reads from TimeBuffer Rename-to-IEW input "
               "snapshot slots"),
      ADD_STAT(timeBufferRenameToIEWSlotReadMisses,
               statistics::units::Count::get(),
               "Number of IEW Rename-to-IEW input snapshot slot reads "
               "that fell back to live wires"),
      ADD_STAT(timeBufferRenameToCommitSlotReads,
               statistics::units::Count::get(),
               "Number of Commit reads from TimeBuffer Rename-to-Commit "
               "input snapshot slots"),
      ADD_STAT(timeBufferRenameToCommitSlotReadMisses,
               statistics::units::Count::get(),
               "Number of Commit Rename-to-Commit input snapshot slot reads "
               "that fell back to live wires"),
      ADD_STAT(timeBufferIEWToCommitSlotReads,
               statistics::units::Count::get(),
               "Number of Commit reads from TimeBuffer IEW-to-Commit "
               "input snapshot slots"),
      ADD_STAT(timeBufferIEWToCommitSlotReadMisses,
               statistics::units::Count::get(),
               "Number of Commit IEW-to-Commit input snapshot slot reads "
               "that fell back to live wires"),
      ADD_STAT(timeBufferFetchBackwardSlotReads,
               statistics::units::Count::get(),
               "Number of Fetch reads from TimeBuffer backward input "
               "snapshot slots"),
      ADD_STAT(timeBufferFetchBackwardSlotReadMisses,
               statistics::units::Count::get(),
               "Number of Fetch TimeBuffer backward input snapshot slot "
               "reads that fell back to live wires"),
      ADD_STAT(timeBufferPrepareMerges, statistics::units::Count::get(),
               "Number of TimeBuffer snapshot prepare results merged"),
      ADD_STAT(timeBufferPreparedInstRefs, statistics::units::Count::get(),
               "Number of instruction references observed by TimeBuffer "
               "snapshot prepare tasks"),
      ADD_STAT(timeBufferPreparedControlSignals,
               statistics::units::Count::get(),
               "Number of control/squash signals observed by TimeBuffer "
               "snapshot prepare tasks"),
      ADD_STAT(timeBufferPreparedResolvedCFIs,
               statistics::units::Count::get(),
               "Number of resolved CFIs observed by TimeBuffer snapshot "
               "prepare tasks"),
      ADD_STAT(timeBufferAdvanceWaits, statistics::units::Count::get(),
               "Number of pre-advance TimeBuffer lifetime barriers that "
               "found outstanding pre-advance-drain weak tasks"),
      ADD_STAT(timeBufferAdvancePendingTasks,
               statistics::units::Count::get(),
               "Accumulated pre-advance-drain weak tasks observed before "
               "TimeBuffer advance lifetime barriers"),
      ADD_STAT(timeBufferAdvanceMaxPendingTasks,
               statistics::units::Count::get(),
               "Maximum pre-advance-drain weak tasks observed before one "
               "TimeBuffer advance lifetime barrier"),
      ADD_STAT(timeBufferAdvanceSafeDeferrals,
               statistics::units::Count::get(),
               "Number of TimeBuffer advances that left cross-advance-safe "
               "weak tasks for a later owner merge"),
      ADD_STAT(timeBufferAdvanceSafeDeferredTasks,
               statistics::units::Count::get(),
               "Accumulated cross-advance-safe weak tasks left past "
               "TimeBuffer advance"),
      ADD_STAT(timeBufferAdvanceMaxSafeDeferredTasks,
               statistics::units::Count::get(),
               "Maximum cross-advance-safe weak tasks left past one "
               "TimeBuffer advance"),
      ADD_STAT(serialTickEndSafeDeferrals,
               statistics::units::Count::get(),
               "Number of CPU tick callbacks that returned with "
               "cross-advance-safe weak tasks still pending for the next "
               "CPU tick"),
      ADD_STAT(serialTickEndSafeDeferredTasks,
               statistics::units::Count::get(),
               "Accumulated cross-advance-safe weak tasks left pending "
               "across CPU tick callback boundaries"),
      ADD_STAT(serialTickEndMaxSafeDeferredTasks,
               statistics::units::Count::get(),
               "Maximum cross-advance-safe weak tasks left pending across "
               "one CPU tick callback boundary"),
      ADD_STAT(futureTimeBufferInputSnapshots,
               statistics::units::Count::get(),
               "Number of horizon-gated future-cycle TimeBuffer input "
               "snapshots captured for read-only prepare"),
      ADD_STAT(futureTimeBufferInputSnapshotSlots,
               statistics::units::Count::get(),
               "Number of TimeBuffer slots copied into future-cycle input "
               "snapshots"),
      ADD_STAT(futureTimeBufferPrepareMerges,
               statistics::units::Count::get(),
               "Number of future-cycle TimeBuffer read-only prepare results "
               "merged"),
      ADD_STAT(futureTimeBufferPrepareSkipped,
               statistics::units::Count::get(),
               "Number of future-cycle TimeBuffer prepares skipped because "
               "the event horizon or CPU schedule did not allow them"),
      ADD_STAT(futureTimeBufferPrepareReuses,
               statistics::units::Count::get(),
               "Number of current input summaries reused from a matching "
               "future-cycle TimeBuffer prepare result"),
      ADD_STAT(futureTimeBufferPrepareChecks,
               statistics::units::Count::get(),
               "Number of future-cycle TimeBuffer prepare results checked "
               "against the next real input snapshot summary"),
      ADD_STAT(futureTimeBufferPrepareMatches,
               statistics::units::Count::get(),
               "Number of future-cycle TimeBuffer prepare results matching "
               "the next real input snapshot summary"),
      ADD_STAT(futureTimeBufferPrepareMismatches,
               statistics::units::Count::get(),
               "Number of future-cycle TimeBuffer prepare results that "
               "matched the expected cycle but not the next input summary"),
      ADD_STAT(futureTimeBufferPrepareStale,
               statistics::units::Count::get(),
               "Number of future-cycle TimeBuffer prepare results that did "
               "not match the next CPU cycle"),
      ADD_STAT(futureTimeBufferPreparedInstRefs,
               statistics::units::Count::get(),
               "Number of instruction references observed by future-cycle "
               "TimeBuffer prepare tasks"),
      ADD_STAT(futureTimeBufferPreparedControlSignals,
               statistics::units::Count::get(),
               "Number of control/squash signals observed by future-cycle "
               "TimeBuffer prepare tasks"),
      ADD_STAT(futureTimeBufferPreparedResolvedCFIs,
               statistics::units::Count::get(),
               "Number of resolved CFIs observed by future-cycle TimeBuffer "
               "prepare tasks"),
      ADD_STAT(futureWavefrontPrepareProbes,
               statistics::units::Count::get(),
               "Number of horizon-gated future Commit-to-IEW wavefront "
               "prepare probes submitted"),
      ADD_STAT(futureWavefrontPrepareMerges,
               statistics::units::Count::get(),
               "Number of future Commit-to-IEW wavefront latch predictions "
               "merged on the owner thread"),
      ADD_STAT(futureWavefrontPrepareSkipped,
               statistics::units::Count::get(),
               "Number of future Commit-to-IEW wavefront probes skipped "
               "because the read-only inputs were not safe to predict"),
      ADD_STAT(futureWavefrontPrepareChecks,
               statistics::units::Count::get(),
               "Number of future Commit-to-IEW wavefront predictions "
               "checked against real stall latches"),
      ADD_STAT(futureWavefrontPrepareMatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-IEW wavefront predictions that "
               "matched real stall latches"),
      ADD_STAT(futureWavefrontPrepareMismatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-IEW wavefront predictions that "
               "matched the expected cycle but not the real stall latches"),
      ADD_STAT(futureWavefrontPrepareStale,
               statistics::units::Count::get(),
               "Number of future Commit-to-IEW wavefront predictions that "
               "did not match the next CPU cycle"),
      ADD_STAT(futureRenameWavefrontPrepareProbes,
               statistics::units::Count::get(),
               "Number of horizon-gated future Commit-to-Rename wavefront "
               "prepare probes submitted"),
      ADD_STAT(futureRenameWavefrontPrepareMerges,
               statistics::units::Count::get(),
               "Number of future Commit-to-Rename wavefront latch "
               "predictions merged on the owner thread"),
      ADD_STAT(futureRenameWavefrontPrepareSkipped,
               statistics::units::Count::get(),
               "Number of future Commit-to-Rename wavefront probes skipped "
               "because the read-only inputs were not safe to predict"),
      ADD_STAT(futureRenameWavefrontPrepareChecks,
               statistics::units::Count::get(),
               "Number of future Commit-to-Rename wavefront predictions "
               "checked against real stall latches"),
      ADD_STAT(futureRenameWavefrontPrepareMatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-Rename wavefront predictions "
               "that matched real stall latches"),
      ADD_STAT(futureRenameWavefrontPrepareMismatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-Rename wavefront predictions "
               "that matched the expected cycle but not the real stall "
               "latches"),
      ADD_STAT(futureRenameWavefrontPrepareStale,
               statistics::units::Count::get(),
               "Number of future Commit-to-Rename wavefront predictions "
               "that did not match the next CPU cycle"),
      ADD_STAT(futureDecodeWavefrontPrepareProbes,
               statistics::units::Count::get(),
               "Number of horizon-gated future Commit-to-Decode wavefront "
               "prepare probes submitted"),
      ADD_STAT(futureDecodeWavefrontPrepareMerges,
               statistics::units::Count::get(),
               "Number of future Commit-to-Decode wavefront latch "
               "predictions merged on the owner thread"),
      ADD_STAT(futureDecodeWavefrontPrepareSkipped,
               statistics::units::Count::get(),
               "Number of future Commit-to-Decode wavefront probes skipped "
               "because the read-only inputs were not safe to predict"),
      ADD_STAT(futureDecodeWavefrontPrepareChecks,
               statistics::units::Count::get(),
               "Number of future Commit-to-Decode wavefront predictions "
               "checked against real stall latches"),
      ADD_STAT(futureDecodeWavefrontPrepareMatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-Decode wavefront predictions "
               "that matched real stall latches"),
      ADD_STAT(futureDecodeWavefrontPrepareMismatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-Decode wavefront predictions "
               "that matched the expected cycle but not the real stall "
               "latches"),
      ADD_STAT(futureDecodeWavefrontPrepareStale,
               statistics::units::Count::get(),
               "Number of future Commit-to-Decode wavefront predictions "
               "that did not match the next CPU cycle"),
      ADD_STAT(futureFetchWavefrontPrepareProbes,
               statistics::units::Count::get(),
               "Number of horizon-gated future Commit-to-Fetch wavefront "
               "prepare probes submitted"),
      ADD_STAT(futureFetchWavefrontPrepareMerges,
               statistics::units::Count::get(),
               "Number of future Commit-to-Fetch wavefront output "
               "predictions merged on the owner thread"),
      ADD_STAT(futureFetchWavefrontPrepareSkipped,
               statistics::units::Count::get(),
               "Number of future Commit-to-Fetch wavefront probes skipped "
               "because the read-only inputs were not safe to predict"),
      ADD_STAT(futureFetchWavefrontPrepareChecks,
               statistics::units::Count::get(),
               "Number of future Commit-to-Fetch wavefront predictions "
               "checked against real Fetch-to-Decode output"),
      ADD_STAT(futureFetchWavefrontPrepareMatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-Fetch wavefront predictions "
               "that matched real Fetch-to-Decode output"),
      ADD_STAT(futureFetchWavefrontPrepareMismatches,
               statistics::units::Count::get(),
               "Number of future Commit-to-Fetch wavefront predictions "
               "that matched the expected cycle but not the real "
               "Fetch-to-Decode output"),
      ADD_STAT(futureFetchWavefrontPrepareStale,
               statistics::units::Count::get(),
               "Number of future Commit-to-Fetch wavefront predictions "
               "that did not match the next CPU cycle"),
      ADD_STAT(futureWavefrontSkipReasons,
               statistics::units::Count::get(),
               "Breakdown of why horizon-gated future wavefront prepare "
               "probes could not be safely predicted"),
      ADD_STAT(workerBusyHostNs, statistics::units::Count::get(),
               "Host nanoseconds spent executing task-runtime work on "
               "workers"),
      ADD_STAT(workerIdleHostNs, statistics::units::Count::get(),
               "Host nanoseconds spent waiting for task-runtime work on "
               "workers"),
      ADD_STAT(steals, statistics::units::Count::get(),
               "Number of task-runtime work stealing operations")
{
    using namespace statistics;

    workerThreads.prereq(workerThreads);
    stageWeakTasks
        .init(NumTaskStages)
        .flags(total);
    stageWeakWork
        .init(NumTaskStages)
        .flags(total);
    stageWeakMerges
        .init(NumTaskStages)
        .flags(total);
    stageInlineTasks
        .init(NumTaskStages)
        .flags(total);
    stageTaskRunHostNs
        .init(NumTaskStages)
        .flags(total);
    stageTaskMergeHostNs
        .init(NumTaskStages)
        .flags(total);
    for (unsigned i = 0; i < NumTaskStages; ++i) {
        const char *name = taskStageName(static_cast<TaskStage>(i));
        stageWeakTasks.subname(i, name);
        stageWeakWork.subname(i, name);
        stageWeakMerges.subname(i, name);
        stageInlineTasks.subname(i, name);
        stageTaskRunHostNs.subname(i, name);
        stageTaskMergeHostNs.subname(i, name);
    }
    eventHorizonBlockReasons
        .init(NumEventHorizonBlockReasons)
        .flags(total);
    for (unsigned i = 0; i < NumEventHorizonBlockReasons; ++i)
        eventHorizonBlockReasons.subname(i, eventHorizonBlockReasonName(i));
    eventHorizonBlockerTypes
        .init(NumEventHorizonBlockerTypes)
        .flags(total);
    eventHorizonEarlierTickBlockerTypes
        .init(NumEventHorizonBlockerTypes)
        .flags(total);
    eventHorizonCpuPriorityBlockerTypes
        .init(NumEventHorizonBlockerTypes)
        .flags(total);
    for (unsigned i = 0; i < NumEventHorizonBlockerTypes; ++i) {
        const char *name = eventHorizonBlockerTypeName(i);
        eventHorizonBlockerTypes.subname(i, name);
        eventHorizonEarlierTickBlockerTypes.subname(i, name);
        eventHorizonCpuPriorityBlockerTypes.subname(i, name);
    }
    stallSignalMerges
        .init(NumStallSignalEdges)
        .flags(total);
    for (unsigned i = 0; i < NumStallSignalEdges; ++i)
        stallSignalMerges.subname(i, stallSignalEdgeName(i));
    futureWavefrontSkipReasons
        .init(NumFutureWavefrontSkipReasons)
        .flags(total);
    for (unsigned i = 0; i < NumFutureWavefrontSkipReasons; ++i)
        futureWavefrontSkipReasons.subname(
                i, futureWavefrontSkipReasonName(i));

    created.prereq(created);
    strong.prereq(strong);
    inlined.prereq(inlined);
    executed.prereq(executed);
    merged.prereq(merged);
    stageBarrierWaits.prereq(stageBarrierWaits);
    stageBarrierDeferredTasks.prereq(stageBarrierDeferredTasks);
    stageBarrierMaxDeferredTasks.prereq(stageBarrierMaxDeferredTasks);
    horizonWaits.prereq(horizonWaits);
    readyQueueSamples.prereq(readyQueueSamples);
    readyQueueOccupancy.prereq(readyQueueOccupancy);
    maxReadyQueueDepth.prereq(maxReadyQueueDepth);
    readyQueueBackpressureWaits.prereq(readyQueueBackpressureWaits);
    readyQueueBackpressureInlineTasks.prereq(
            readyQueueBackpressureInlineTasks);
    inFlightCycleSamples.prereq(inFlightCycleSamples);
    inFlightCycles.prereq(inFlightCycles);
    wavefrontPlanSamples.prereq(wavefrontPlanSamples);
    wavefrontPlanEffectiveCycles.prereq(wavefrontPlanEffectiveCycles);
    wavefrontPlanTasks.prereq(wavefrontPlanTasks);
    wavefrontPlanEdges.prereq(wavefrontPlanEdges);
    wavefrontPlanCriticalPathLen.prereq(wavefrontPlanCriticalPathLen);
    wavefrontPlanMaxReadyTasks.prereq(wavefrontPlanMaxReadyTasks);
    wavefrontPlanReadySlack.prereq(wavefrontPlanReadySlack);
    eventHorizonSamples.prereq(eventHorizonSamples);
    eventHorizonCandidateCycles.prereq(eventHorizonCandidateCycles);
    eventHorizonCommittableCycles.prereq(eventHorizonCommittableCycles);
    eventHorizonLimitedCycles.prereq(eventHorizonLimitedCycles);
    eventHorizonBlockedSamples.prereq(eventHorizonBlockedSamples);
    eventHorizonSameTickBlocks.prereq(eventHorizonSameTickBlocks);
    eventHorizonZeroCycleBlocks.prereq(eventHorizonZeroCycleBlocks);
    eventHorizonPartialWindowBlocks.prereq(eventHorizonPartialWindowBlocks);
    eventHorizonEarlierTickBlocks.prereq(eventHorizonEarlierTickBlocks);
    eventHorizonCpuPriorityBlocks.prereq(eventHorizonCpuPriorityBlocks);
    eventHorizonMaxBlockedOffset.prereq(eventHorizonMaxBlockedOffset);
    eventHorizonMaxCommittableCycles.prereq(
            eventHorizonMaxCommittableCycles);
    eventHorizonBlockReasons.prereq(eventHorizonBlockReasons);
    eventHorizonBlockerTypes.prereq(eventHorizonBlockerTypes);
    eventHorizonEarlierTickBlockerTypes.prereq(
            eventHorizonEarlierTickBlockerTypes);
    eventHorizonCpuPriorityBlockerTypes.prereq(
            eventHorizonCpuPriorityBlockerTypes);
    stallSignalWindowSamples.prereq(stallSignalWindowSamples);
    stallSignalWindowCapacity.prereq(stallSignalWindowCapacity);
    stallSignalWindowValidSlots.prereq(stallSignalWindowValidSlots);
    stallSignalWindowMaxValidSlots.prereq(stallSignalWindowMaxValidSlots);
    stallSignalWindowEdgesCaptured.prereq(stallSignalWindowEdgesCaptured);
    /*
     * Keep this vector unconditionally displayed. Vector self-prereq is not
     * reliable here because this tree's VectorBase::zero() treats a vector
     * with every element non-zero as zero.
     */
    stallSignalInputReads.prereq(stallSignalInputReads);
    stallSignalInputReadFallbacks.prereq(stallSignalInputReadFallbacks);
    stallSignalFutureReadBlocks.prereq(stallSignalFutureReadBlocks);
    specPrepared.prereq(specPrepared);
    specDiscarded.prereq(specDiscarded);
    specThrottled.prereq(specThrottled);
    stageTaskRunHostNs.prereq(stageTaskRunHostNs);
    stageTaskMergeHostNs.prereq(stageTaskMergeHostNs);
    timeBufferInputSnapshots.prereq(timeBufferInputSnapshots);
    timeBufferOutputSnapshots.prereq(timeBufferOutputSnapshots);
    timeBufferSlotsCaptured.prereq(timeBufferSlotsCaptured);
    timeBufferSnapshotWindowSamples.prereq(timeBufferSnapshotWindowSamples);
    timeBufferSnapshotWindowCapacity.prereq(timeBufferSnapshotWindowCapacity);
    timeBufferInputWindowValidFrames.prereq(timeBufferInputWindowValidFrames);
    timeBufferOutputWindowValidFrames.prereq(
            timeBufferOutputWindowValidFrames);
    timeBufferSnapshotWindowMaxValidFrames.prereq(
            timeBufferSnapshotWindowMaxValidFrames);
    timeBufferStageInputReads.prereq(timeBufferStageInputReads);
    timeBufferStageInputReadMisses.prereq(timeBufferStageInputReadMisses);
    timeBufferBackwardSlotReads.prereq(timeBufferBackwardSlotReads);
    timeBufferBackwardSlotReadMisses.prereq(
            timeBufferBackwardSlotReadMisses);
    timeBufferFetchToDecodeSlotReads.prereq(
            timeBufferFetchToDecodeSlotReads);
    timeBufferFetchToDecodeSlotReadMisses.prereq(
            timeBufferFetchToDecodeSlotReadMisses);
    timeBufferDecodeToRenameSlotReads.prereq(
            timeBufferDecodeToRenameSlotReads);
    timeBufferDecodeToRenameSlotReadMisses.prereq(
            timeBufferDecodeToRenameSlotReadMisses);
    timeBufferRenameToIEWSlotReads.prereq(
            timeBufferRenameToIEWSlotReads);
    timeBufferRenameToIEWSlotReadMisses.prereq(
            timeBufferRenameToIEWSlotReadMisses);
    timeBufferRenameToCommitSlotReads.prereq(
            timeBufferRenameToCommitSlotReads);
    timeBufferRenameToCommitSlotReadMisses.prereq(
            timeBufferRenameToCommitSlotReadMisses);
    timeBufferIEWToCommitSlotReads.prereq(
            timeBufferIEWToCommitSlotReads);
    timeBufferIEWToCommitSlotReadMisses.prereq(
            timeBufferIEWToCommitSlotReadMisses);
    timeBufferFetchBackwardSlotReads.prereq(
            timeBufferFetchBackwardSlotReads);
    timeBufferFetchBackwardSlotReadMisses.prereq(
            timeBufferFetchBackwardSlotReadMisses);
    timeBufferPrepareMerges.prereq(timeBufferPrepareMerges);
    timeBufferPreparedInstRefs.prereq(timeBufferPreparedInstRefs);
    timeBufferPreparedControlSignals.prereq(
            timeBufferPreparedControlSignals);
    timeBufferPreparedResolvedCFIs.prereq(timeBufferPreparedResolvedCFIs);
    timeBufferAdvanceWaits.prereq(timeBufferAdvanceWaits);
    timeBufferAdvancePendingTasks.prereq(timeBufferAdvancePendingTasks);
    timeBufferAdvanceMaxPendingTasks.prereq(
            timeBufferAdvanceMaxPendingTasks);
    timeBufferAdvanceSafeDeferrals.prereq(
            timeBufferAdvanceSafeDeferrals);
    timeBufferAdvanceSafeDeferredTasks.prereq(
            timeBufferAdvanceSafeDeferredTasks);
    timeBufferAdvanceMaxSafeDeferredTasks.prereq(
            timeBufferAdvanceMaxSafeDeferredTasks);
    serialTickEndSafeDeferrals.prereq(serialTickEndSafeDeferrals);
    serialTickEndSafeDeferredTasks.prereq(serialTickEndSafeDeferredTasks);
    serialTickEndMaxSafeDeferredTasks.prereq(
            serialTickEndMaxSafeDeferredTasks);
    futureTimeBufferInputSnapshots.prereq(futureTimeBufferInputSnapshots);
    futureTimeBufferInputSnapshotSlots.prereq(
            futureTimeBufferInputSnapshotSlots);
    futureTimeBufferPrepareMerges.prereq(futureTimeBufferPrepareMerges);
    futureTimeBufferPrepareSkipped.prereq(futureTimeBufferPrepareSkipped);
    futureTimeBufferPrepareReuses.prereq(futureTimeBufferPrepareReuses);
    futureTimeBufferPrepareChecks.prereq(futureTimeBufferPrepareChecks);
    futureTimeBufferPrepareMatches.prereq(futureTimeBufferPrepareMatches);
    futureTimeBufferPrepareMismatches.prereq(
            futureTimeBufferPrepareMismatches);
    futureTimeBufferPrepareStale.prereq(futureTimeBufferPrepareStale);
    futureTimeBufferPreparedInstRefs.prereq(
            futureTimeBufferPreparedInstRefs);
    futureTimeBufferPreparedControlSignals.prereq(
            futureTimeBufferPreparedControlSignals);
    futureTimeBufferPreparedResolvedCFIs.prereq(
            futureTimeBufferPreparedResolvedCFIs);
    futureWavefrontPrepareProbes.prereq(futureWavefrontPrepareProbes);
    futureWavefrontPrepareMerges.prereq(futureWavefrontPrepareMerges);
    futureWavefrontPrepareSkipped.prereq(futureWavefrontPrepareSkipped);
    futureWavefrontPrepareChecks.prereq(futureWavefrontPrepareChecks);
    futureWavefrontPrepareMatches.prereq(futureWavefrontPrepareMatches);
    futureWavefrontPrepareMismatches.prereq(
            futureWavefrontPrepareMismatches);
    futureWavefrontPrepareStale.prereq(futureWavefrontPrepareStale);
    futureRenameWavefrontPrepareProbes.prereq(
            futureRenameWavefrontPrepareProbes);
    futureRenameWavefrontPrepareMerges.prereq(
            futureRenameWavefrontPrepareMerges);
    futureRenameWavefrontPrepareSkipped.prereq(
            futureRenameWavefrontPrepareSkipped);
    futureRenameWavefrontPrepareChecks.prereq(
            futureRenameWavefrontPrepareChecks);
    futureRenameWavefrontPrepareMatches.prereq(
            futureRenameWavefrontPrepareMatches);
    futureRenameWavefrontPrepareMismatches.prereq(
            futureRenameWavefrontPrepareMismatches);
    futureRenameWavefrontPrepareStale.prereq(
            futureRenameWavefrontPrepareStale);
    futureDecodeWavefrontPrepareProbes.prereq(
            futureDecodeWavefrontPrepareProbes);
    futureDecodeWavefrontPrepareMerges.prereq(
            futureDecodeWavefrontPrepareMerges);
    futureDecodeWavefrontPrepareSkipped.prereq(
            futureDecodeWavefrontPrepareSkipped);
    futureDecodeWavefrontPrepareChecks.prereq(
            futureDecodeWavefrontPrepareChecks);
    futureDecodeWavefrontPrepareMatches.prereq(
            futureDecodeWavefrontPrepareMatches);
    futureDecodeWavefrontPrepareMismatches.prereq(
            futureDecodeWavefrontPrepareMismatches);
    futureDecodeWavefrontPrepareStale.prereq(
            futureDecodeWavefrontPrepareStale);
    futureFetchWavefrontPrepareProbes.prereq(
            futureFetchWavefrontPrepareProbes);
    futureFetchWavefrontPrepareMerges.prereq(
            futureFetchWavefrontPrepareMerges);
    futureFetchWavefrontPrepareSkipped.prereq(
            futureFetchWavefrontPrepareSkipped);
    futureFetchWavefrontPrepareChecks.prereq(
            futureFetchWavefrontPrepareChecks);
    futureFetchWavefrontPrepareMatches.prereq(
            futureFetchWavefrontPrepareMatches);
    futureFetchWavefrontPrepareMismatches.prereq(
            futureFetchWavefrontPrepareMismatches);
    futureFetchWavefrontPrepareStale.prereq(
            futureFetchWavefrontPrepareStale);
    futureWavefrontSkipReasons.prereq(futureWavefrontSkipReasons);
    workerBusyHostNs.prereq(workerBusyHostNs);
    workerIdleHostNs.prereq(workerIdleHostNs);
    steals.prereq(steals);
}

TaskRuntime::TaskRuntime(CPU *_cpu, const System *system)
    : cpu(_cpu),
      stats_(cpu)
{
    fatal_if(!system, "TaskRuntime requires a parent System.");

    const auto &config = system->taskParallelConfig();
    enabled_ = config.enableTaskParallelSim;
    deterministic_ = config.taskDeterministic;
    trace_ = config.taskTrace;
    selfTest_ = config.taskRuntimeSelfTest;
    eventPriorityAudit_ = config.eventPriorityAudit;
    workerThreads_ = config.taskParallelThreads;
    windowCycles_ = config.taskWindowCycles;
    taskMinWork_ = config.taskMinWork;
    maxInFlightCycles_ = config.maxInFlightCycles;
    maxReadyTasks_ = config.maxReadyTasks;
    maxSpecTaskWaste_ = config.maxSpecTaskWaste;

    if (enabled_ && workerThreads_ == 0) {
        const auto host_threads = std::thread::hardware_concurrency();
        const unsigned auto_threads = host_threads > 1 ? host_threads - 1 : 1;
        workerThreads_ = std::min(DefaultTaskParallelWorkerThreads,
                                  auto_threads);
    }
    stats_.workerThreads = workerThreads_;

    if (eventPriorityAudit_)
        setEventPriorityAudit(true);

    fatal_if(enabled_ && windowCycles_ == 0,
            "taskWindowCycles must be greater than zero when task parallel "
            "simulation is enabled.");
    fatal_if(enabled_ && maxInFlightCycles_ == 0,
            "maxInFlightCycles must be greater than zero when task parallel "
            "simulation is enabled.");
    fatal_if(selfTest_ && !enabled_,
            "taskRuntimeSelfTest requires task parallel simulation.");
    fatal_if(selfTest_ && !deterministic_,
            "taskRuntimeSelfTest requires deterministic task merging.");
    fatal_if(selfTest_ && maxReadyTasks_ != 0 &&
            maxReadyTasks_ < TaskRuntimeSelfTestTasks,
            "taskRuntimeSelfTest requires maxReadyTasks to be zero or at "
            "least %u.", TaskRuntimeSelfTestTasks);

    if (selfTest_)
        runSelfTest();
}

TaskRuntime::~TaskRuntime()
{
    try {
        drain();
    } catch (...) {
        warn("Discarding an exception while destroying TaskRuntime.");
    }
    stopWorkers();
}

bool
TaskRuntime::shouldInline(unsigned estimatedWork) const
{
    return !enabled_ || workerThreads_ == 0 || estimatedWork < taskMinWork_;
}

void
TaskRuntime::startWorkers()
{
    if (workersStarted_ || workerThreads_ == 0)
        return;

    workersStarted_ = true;
    workers_.reserve(workerThreads_);
    for (unsigned i = 0; i < workerThreads_; ++i)
        workers_.emplace_back(&TaskRuntime::workerLoop, this, i);
}

void
TaskRuntime::stopWorkers()
{
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
    }

    workAvailable_.notify_all();

    for (auto &worker : workers_) {
        if (worker.joinable())
            worker.join();
    }

    workers_.clear();
}

void
TaskRuntime::workerLoop([[maybe_unused]] unsigned workerId)
{
    while (true) {
        Task *task = nullptr;
        auto idle_begin = HostClock::now();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            workAvailable_.wait(lock, [this] {
                return stopping_ || !ready_.empty();
            });

            const auto idle_end = HostClock::now();
            const auto idle_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(idle_end - idle_begin).count();
            pendingWorkerIdleNs_.fetch_add(
                    static_cast<uint64_t>(idle_ns),
                    std::memory_order_relaxed);

            if (stopping_ && ready_.empty())
                return;

            task = ready_.front();
            ready_.pop_front();
        }

        const auto busy_begin = HostClock::now();
        try {
            task->run();
        } catch (...) {
            task->exception = std::current_exception();
        }
        const auto busy_end = HostClock::now();
        const auto busy_ns = std::chrono::duration_cast<
                std::chrono::nanoseconds>(busy_end - busy_begin).count();
        task->runHostNs = static_cast<uint64_t>(busy_ns);

        pendingExecutedStats_.fetch_add(1, std::memory_order_relaxed);
        pendingWorkerBusyNs_.fetch_add(
                static_cast<uint64_t>(busy_ns),
                std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> guard(mutex_);
            task->done = true;
        }

        workDone_.notify_all();
    }
}

void
TaskRuntime::runSelfTest()
{
    std::vector<unsigned> ran(TaskRuntimeSelfTestTasks, 0);
    std::vector<unsigned> merged;
    merged.reserve(TaskRuntimeSelfTestTasks);

    for (unsigned i = 0; i < TaskRuntimeSelfTestTasks; ++i) {
        const unsigned order = TaskRuntimeSelfTestTasks - 1 - i;
        TaskOrderKey key{
            Cycles(0),
            TaskStage::Runtime,
            0,
            0,
            order,
        };

        submitWeak(key, taskMinWork_, [&, i] {
            ran[i] = i + 1;
        }, [&, order] {
            merged.push_back(order);
        });
    }

    waitForAll();

    fatal_if(merged.size() != TaskRuntimeSelfTestTasks,
            "Task runtime self-test merged %llu tasks, expected %u.",
            static_cast<unsigned long long>(merged.size()),
            TaskRuntimeSelfTestTasks);

    for (unsigned i = 0; i < TaskRuntimeSelfTestTasks; ++i) {
        fatal_if(ran[i] != i + 1,
                "Task runtime self-test task %u did not execute.", i);
        fatal_if(merged[i] != i,
                "Task runtime self-test merge order mismatch at index %u: "
                "got %u.", i, merged[i]);
    }

    DPRINTF(TaskSched, "Task runtime self-test passed with %u workers\n",
            workerThreads_);
}

void
TaskRuntime::flushWorkerStats()
{
    const auto executed =
        pendingExecutedStats_.exchange(0, std::memory_order_relaxed);
    if (executed)
        stats_.executed += executed;

    const auto busy_ns =
        pendingWorkerBusyNs_.exchange(0, std::memory_order_relaxed);
    if (busy_ns)
        stats_.workerBusyHostNs += busy_ns;

    const auto idle_ns =
        pendingWorkerIdleNs_.exchange(0, std::memory_order_relaxed);
    if (idle_ns)
        stats_.workerIdleHostNs += idle_ns;
}

void
TaskRuntime::submitWeak(TaskOrderKey order, unsigned estimatedWork,
                        TaskFn run, MergeFn merge, TaskLifetime lifetime)
{
    fatal_if(!run, "TaskRuntime::submitWeak requires a run function.");

    stats_.created++;
    const unsigned stage_index = taskStageIndex(order.stage);
    stats_.stageWeakTasks[stage_index]++;
    stats_.stageWeakWork[stage_index] += estimatedWork;

    if (shouldInline(estimatedWork)) {
        waitForBarrier(order);
        const auto run_begin = HostClock::now();
        run();
        const auto run_end = HostClock::now();
        const auto run_ns = std::chrono::duration_cast<
                std::chrono::nanoseconds>(run_end - run_begin).count();
        stats_.stageTaskRunHostNs[stage_index] +=
                static_cast<uint64_t>(run_ns);
        stats_.inlined++;
        stats_.stageInlineTasks[stage_index]++;
        if (merge) {
            const auto merge_begin = HostClock::now();
            merge();
            const auto merge_end = HostClock::now();
            const auto merge_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                            merge_end - merge_begin).count();
            stats_.stageTaskMergeHostNs[stage_index] +=
                    static_cast<uint64_t>(merge_ns);
            stats_.merged++;
            stats_.stageWeakMerges[stage_index]++;
        }
        return;
    }

    startWorkers();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (maxReadyTasks_ != 0 && ready_.size() >= maxReadyTasks_) {
            stats_.readyQueueBackpressureWaits++;
            lock.unlock();
            waitForAll();
            stats_.readyQueueBackpressureInlineTasks++;
            const auto run_begin = HostClock::now();
            run();
            const auto run_end = HostClock::now();
            const auto run_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                            run_end - run_begin).count();
            stats_.stageTaskRunHostNs[stage_index] +=
                    static_cast<uint64_t>(run_ns);
            stats_.inlined++;
            stats_.stageInlineTasks[stage_index]++;
            if (merge) {
                const auto merge_begin = HostClock::now();
                merge();
                const auto merge_end = HostClock::now();
                const auto merge_ns = std::chrono::duration_cast<
                        std::chrono::nanoseconds>(
                                merge_end - merge_begin).count();
                stats_.stageTaskMergeHostNs[stage_index] +=
                        static_cast<uint64_t>(merge_ns);
                stats_.merged++;
                stats_.stageWeakMerges[stage_index]++;
            }
            return;
        }

        auto task = std::make_unique<Task>();
        task->seq = nextTaskSeq_++;
        task->order = order;
        task->run = std::move(run);
        task->merge = std::move(merge);
        task->lifetime = lifetime;

        ready_.push_back(task.get());
        inFlight_.push_back(std::move(task));

        stats_.readyQueueSamples++;
        const auto ready_depth = ready_.size();
        stats_.readyQueueOccupancy += ready_depth;
        if (ready_depth > stats_.maxReadyQueueDepth.value())
            stats_.maxReadyQueueDepth = ready_depth;

        if (trace_) {
            DPRINTF(TaskSched,
                    "Queued weak task seq=%llu cycle=%llu stage=%u phase=%u "
                    "tid=%i localSeq=%llu ready=%llu\n",
                    inFlight_.back()->seq, order.cycle,
                    static_cast<unsigned>(order.stage), order.phase,
                    order.tid, order.localSeq,
                    static_cast<unsigned long long>(ready_.size()));
        }
    }

    workAvailable_.notify_one();
}

void
TaskRuntime::runStrongImpl(TaskOrderKey order, TaskFn run)
{
    fatal_if(!run, "TaskRuntime::runStrong requires a run function.");

    stats_.created++;
    stats_.stageBarrierWaits++;

    if (trace_) {
        DPRINTF(TaskGraph,
                "Strong task ready cycle=%llu stage=%s phase=%u tid=%i "
                "localSeq=%llu\n",
                order.cycle, taskStageName(order.stage), order.phase,
                order.tid, order.localSeq);
    }

    waitForBarrier(order);

    if (trace_) {
        DPRINTF(TaskSched,
                "Strong task begin cycle=%llu stage=%s phase=%u tid=%i "
                "localSeq=%llu\n",
                order.cycle, taskStageName(order.stage), order.phase,
                order.tid, order.localSeq);
    }

    run();
    stats_.strong++;

    if (trace_) {
        DPRINTF(TaskSched,
                "Strong task end cycle=%llu stage=%s phase=%u tid=%i "
                "localSeq=%llu\n",
                order.cycle, taskStageName(order.stage), order.phase,
                order.tid, order.localSeq);
    }
}

void
TaskRuntime::mergeCompletedTasks(
        std::vector<std::unique_ptr<Task>> &completed)
{
    if (completed.empty())
        return;

    if (deterministic_) {
        std::stable_sort(completed.begin(), completed.end(),
                [] (const auto &lhs, const auto &rhs) {
                    if (taskOrderLess(lhs->order, rhs->order))
                        return true;
                    if (taskOrderLess(rhs->order, lhs->order))
                        return false;
                    return lhs->seq < rhs->seq;
                });
    }

    for (const auto &task : completed) {
        if (task->exception)
            std::rethrow_exception(task->exception);
    }

    for (const auto &task : completed) {
        const unsigned stage_index = taskStageIndex(task->order.stage);
        stats_.stageTaskRunHostNs[stage_index] += task->runHostNs;
        if (task->merge) {
            const auto merge_begin = HostClock::now();
            task->merge();
            const auto merge_end = HostClock::now();
            const auto merge_ns = std::chrono::duration_cast<
                    std::chrono::nanoseconds>(
                            merge_end - merge_begin).count();
            stats_.stageTaskMergeHostNs[stage_index] +=
                    static_cast<uint64_t>(merge_ns);
            stats_.merged++;
            stats_.stageWeakMerges[stage_index]++;
        }
    }
}

void
TaskRuntime::waitForBarrier(TaskOrderKey barrier)
{
    std::vector<std::unique_ptr<Task>> completed;
    uint64_t deferred_tasks = 0;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (inFlight_.empty()) {
            flushWorkerStats();
            return;
        }

        auto blocks_barrier = [&barrier] (const auto &task) {
            return taskOrderLessOrEqual(task->order, barrier);
        };

        const bool has_blocking_task =
            std::any_of(inFlight_.begin(), inFlight_.end(), blocks_barrier);
        if (has_blocking_task) {
            workDone_.wait(lock, [this, &blocks_barrier] {
                return std::all_of(inFlight_.begin(), inFlight_.end(),
                        [&blocks_barrier] (const auto &task) {
                            return !blocks_barrier(task) || task->done;
                        });
            });

            for (auto it = inFlight_.begin(); it != inFlight_.end();) {
                if (blocks_barrier(*it)) {
                    completed.push_back(std::move(*it));
                    it = inFlight_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        deferred_tasks = inFlight_.size();
    }

    flushWorkerStats();

    if (deferred_tasks) {
        stats_.stageBarrierDeferredTasks += deferred_tasks;
        if (deferred_tasks > stats_.stageBarrierMaxDeferredTasks.value())
            stats_.stageBarrierMaxDeferredTasks = deferred_tasks;
    }

    mergeCompletedTasks(completed);
}

void
TaskRuntime::waitForOrder(TaskOrderKey barrier)
{
    if (!enabled_)
        return;

    stats_.stageBarrierWaits++;
    waitForBarrier(barrier);
}

void
TaskRuntime::waitForAll()
{
    std::vector<std::unique_ptr<Task>> completed;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (inFlight_.empty()) {
            flushWorkerStats();
            return;
        }

        workDone_.wait(lock, [this] {
            return std::all_of(inFlight_.begin(), inFlight_.end(),
                    [] (const auto &task) { return task->done; });
        });

        completed.swap(inFlight_);
    }

    flushWorkerStats();
    mergeCompletedTasks(completed);
}

void
TaskRuntime::waitForPreAdvance()
{
    std::vector<std::unique_ptr<Task>> completed;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (inFlight_.empty()) {
            flushWorkerStats();
            return;
        }

        auto must_drain = [] (const auto &task) {
            return task->lifetime == TaskLifetime::PreAdvanceDrain;
        };

        const bool has_pre_advance_task =
            std::any_of(inFlight_.begin(), inFlight_.end(), must_drain);
        if (has_pre_advance_task) {
            workDone_.wait(lock, [this, &must_drain] {
                return std::all_of(inFlight_.begin(), inFlight_.end(),
                        [&must_drain] (const auto &task) {
                            return !must_drain(task) || task->done;
                        });
            });

            for (auto it = inFlight_.begin(); it != inFlight_.end();) {
                if (must_drain(*it)) {
                    completed.push_back(std::move(*it));
                    it = inFlight_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    flushWorkerStats();
    mergeCompletedTasks(completed);
}

uint64_t
TaskRuntime::pendingTaskCount() const
{
    if (!enabled_)
        return 0;

    std::unique_lock<std::mutex> lock(mutex_);
    return inFlight_.size();
}

uint64_t
TaskRuntime::pendingPreAdvanceTaskCount() const
{
    if (!enabled_)
        return 0;

    std::unique_lock<std::mutex> lock(mutex_);
    return std::count_if(inFlight_.begin(), inFlight_.end(),
            [] (const auto &task) {
                return task->lifetime == TaskLifetime::PreAdvanceDrain;
            });
}

void
TaskRuntime::drain()
{
    waitForAll();
}

void
TaskRuntime::onSerialTickBegin(Cycles cycle)
{
    if (!enabled_)
        return;

    stats_.workerThreads = workerThreads_;
    flushWorkerStats();
    stats_.inFlightCycleSamples++;
    stats_.inFlightCycles += 1;

    if (!trace_)
        return;

    DPRINTF(TaskSched,
            "Task runtime serial tick begin: cycle=%llu inFlight=1 "
            "window=%u workers=%u\n",
            cycle, windowCycles_, workerThreads_);
}

void
TaskRuntime::recordWavefrontPlan(Cycles cycle, PipelineStageDelays delays)
{
    if (!enabled_)
        return;

    const unsigned effective_window =
        std::min(windowCycles_, maxInFlightCycles_);

    const bool same_delays =
        static_cast<uint64_t>(wavefrontPlan_.delays.fetchToDecode) ==
            static_cast<uint64_t>(delays.fetchToDecode) &&
        static_cast<uint64_t>(wavefrontPlan_.delays.decodeToRename) ==
            static_cast<uint64_t>(delays.decodeToRename) &&
        static_cast<uint64_t>(wavefrontPlan_.delays.renameToIEW) ==
            static_cast<uint64_t>(delays.renameToIEW) &&
        static_cast<uint64_t>(wavefrontPlan_.delays.renameToCommit) ==
            static_cast<uint64_t>(delays.renameToCommit) &&
        static_cast<uint64_t>(wavefrontPlan_.delays.iewToCommit) ==
            static_cast<uint64_t>(delays.iewToCommit);

    if (!wavefrontPlan_.valid ||
        wavefrontPlan_.effectiveWindow != effective_window ||
        !same_delays) {
        WavefrontPlanMetrics metrics;
        metrics.valid = true;
        metrics.delays = delays;
        metrics.effectiveWindow = effective_window;
        metrics.tasks = effective_window * NumPipelineTaskStages;

        std::vector<std::vector<unsigned>> successors(metrics.tasks);
        std::vector<unsigned> indegree(metrics.tasks, 0);

        auto add_edge = [&] (unsigned src_cycle, PipelineStageIndex src_stage,
                             unsigned dst_cycle, PipelineStageIndex dst_stage)
        {
            if (src_cycle >= effective_window || dst_cycle >= effective_window)
                return;

            const unsigned src = pipelineNodeIndex(src_cycle, src_stage);
            const unsigned dst = pipelineNodeIndex(dst_cycle, dst_stage);
            successors[src].push_back(dst);
            indegree[dst]++;
            metrics.edges++;
        };

        for (unsigned c = 0; c < effective_window; ++c) {
            add_edge(c, PipelineCommit, c, PipelineIEW);
            add_edge(c, PipelineIEW, c, PipelineRename);
            add_edge(c, PipelineRename, c, PipelineDecode);
            add_edge(c, PipelineDecode, c, PipelineFetch);
        }

        auto add_delay_edges = [&] (PipelineStageIndex src_stage,
                                    PipelineStageIndex dst_stage,
                                    Cycles delay)
        {
            const unsigned cycle_delay = static_cast<uint64_t>(delay);
            if (cycle_delay == 0)
                return;

            for (unsigned c = 0; c + cycle_delay < effective_window; ++c)
                add_edge(c, src_stage, c + cycle_delay, dst_stage);
        };

        add_delay_edges(PipelineIEW, PipelineCommit, delays.iewToCommit);
        add_delay_edges(PipelineRename, PipelineIEW, delays.renameToIEW);
        add_delay_edges(PipelineRename, PipelineCommit,
                        delays.renameToCommit);
        add_delay_edges(PipelineDecode, PipelineRename,
                        delays.decodeToRename);
        add_delay_edges(PipelineFetch, PipelineDecode, delays.fetchToDecode);

        std::vector<unsigned> ready;
        ready.reserve(metrics.tasks);
        for (unsigned node = 0; node < metrics.tasks; ++node) {
            if (indegree[node] == 0)
                ready.push_back(node);
        }

        unsigned visited = 0;
        while (!ready.empty()) {
            const unsigned ready_width = ready.size();
            metrics.criticalPathLen++;
            metrics.maxReadyTasks =
                std::max(metrics.maxReadyTasks, ready_width);
            if (ready_width > 1)
                metrics.readySlack += ready_width - 1;

            std::vector<unsigned> next_ready;
            next_ready.reserve(metrics.tasks);
            for (unsigned node : ready) {
                visited++;
                for (unsigned succ : successors[node]) {
                    assert(indegree[succ] > 0);
                    indegree[succ]--;
                    if (indegree[succ] == 0)
                        next_ready.push_back(succ);
                }
            }

            ready.swap(next_ready);
        }

        fatal_if(visited != metrics.tasks,
                "Static wavefront DAG planning found a cycle: visited %u "
                "of %u tasks.", visited, metrics.tasks);

        wavefrontPlan_ = metrics;
    }

    stats_.wavefrontPlanSamples++;
    stats_.wavefrontPlanEffectiveCycles += wavefrontPlan_.effectiveWindow;
    stats_.wavefrontPlanTasks += wavefrontPlan_.tasks;
    stats_.wavefrontPlanEdges += wavefrontPlan_.edges;
    stats_.wavefrontPlanCriticalPathLen +=
        wavefrontPlan_.criticalPathLen;
    if (wavefrontPlan_.maxReadyTasks >
        stats_.wavefrontPlanMaxReadyTasks.value()) {
        stats_.wavefrontPlanMaxReadyTasks = wavefrontPlan_.maxReadyTasks;
    }
    stats_.wavefrontPlanReadySlack += wavefrontPlan_.readySlack;

    if (trace_) {
        DPRINTF(TaskGraph,
                "Wavefront plan cycle=%llu effectiveWindow=%u tasks=%u "
                "edges=%u criticalPath=%u maxReady=%u readySlack=%u "
                "delays(f2d=%llu,d2r=%llu,r2i=%llu,r2c=%llu,i2c=%llu)\n",
                cycle, wavefrontPlan_.effectiveWindow, wavefrontPlan_.tasks,
                wavefrontPlan_.edges, wavefrontPlan_.criticalPathLen,
                wavefrontPlan_.maxReadyTasks, wavefrontPlan_.readySlack,
                delays.fetchToDecode, delays.decodeToRename,
                delays.renameToIEW, delays.renameToCommit,
                delays.iewToCommit);
    }
}

void
TaskRuntime::recordEventHorizon(Cycles cycle, unsigned candidate_cycles,
                                unsigned committable_cycles,
                                bool has_next_event, Tick next_event_tick,
                                int next_event_priority,
                                unsigned blocked_offset,
                                bool blocked_by_earlier_tick_event,
                                const Event *blocker_event)
{
    if (!enabled_)
        return;

    fatal_if(committable_cycles > candidate_cycles,
            "Event horizon committable cycles %u exceed candidate cycles %u.",
            committable_cycles, candidate_cycles);
    fatal_if(blocked_offset > candidate_cycles,
            "Event horizon blocked offset %u exceed candidate cycles %u.",
            blocked_offset, candidate_cycles);
    fatal_if(blocked_offset == 0 && committable_cycles < candidate_cycles,
            "Event horizon blocked window without a blocked offset.");
    fatal_if(blocked_offset != 0 && committable_cycles == candidate_cycles,
            "Event horizon blocked offset %u without a truncated window.",
            blocked_offset);

    stats_.eventHorizonSamples++;
    stats_.eventHorizonCandidateCycles += candidate_cycles;
    stats_.eventHorizonCommittableCycles += committable_cycles;
    stats_.eventHorizonLimitedCycles +=
        candidate_cycles - committable_cycles;
    if (committable_cycles < candidate_cycles) {
        stats_.eventHorizonBlockedSamples++;
        if (has_next_event && next_event_tick == curTick())
            stats_.eventHorizonSameTickBlocks++;
        if (committable_cycles == 0)
            stats_.eventHorizonZeroCycleBlocks++;
        else
            stats_.eventHorizonPartialWindowBlocks++;
        if (blocked_by_earlier_tick_event)
            stats_.eventHorizonEarlierTickBlocks++;
        else
            stats_.eventHorizonCpuPriorityBlocks++;
        if (blocked_offset >
            stats_.eventHorizonMaxBlockedOffset.value()) {
            stats_.eventHorizonMaxBlockedOffset = blocked_offset;
        }
        stats_.eventHorizonBlockReasons[
            eventHorizonBlockReason(blocked_by_earlier_tick_event,
                                    next_event_priority)]++;
        if (eventPriorityAudit_) {
            const unsigned blocker_type =
                eventHorizonBlockerType(blocker_event);
            stats_.eventHorizonBlockerTypes[
                blocker_type]++;
            if (blocked_by_earlier_tick_event) {
                stats_.eventHorizonEarlierTickBlockerTypes[
                    blocker_type]++;
            } else {
                stats_.eventHorizonCpuPriorityBlockerTypes[
                    blocker_type]++;
            }
        }
    }
    if (committable_cycles >
        stats_.eventHorizonMaxCommittableCycles.value()) {
        stats_.eventHorizonMaxCommittableCycles = committable_cycles;
    }

    if (!trace_)
        return;

    if (has_next_event) {
        DPRINTF(TaskGraph,
                "Event horizon cycle=%llu candidate=%u committable=%u "
                "limited=%u blockedOffset=%u blockByEarlierTick=%i "
                "nextTick=%llu nextPriority=%d\n",
                cycle, candidate_cycles, committable_cycles,
                candidate_cycles - committable_cycles, blocked_offset,
                blocked_by_earlier_tick_event,
                next_event_tick, next_event_priority);
    } else {
        DPRINTF(TaskGraph,
                "Event horizon cycle=%llu candidate=%u committable=%u "
                "limited=%u nextTick=none\n",
                cycle, candidate_cycles, committable_cycles,
                candidate_cycles - committable_cycles);
    }
}

void
TaskRuntime::recordStallSignalWindow(Cycles cycle, unsigned capacity,
                                     unsigned valid_slots,
                                     unsigned edges_captured)
{
    if (!enabled_)
        return;

    stats_.stallSignalWindowSamples++;
    stats_.stallSignalWindowCapacity += capacity;
    stats_.stallSignalWindowValidSlots += valid_slots;
    if (valid_slots > stats_.stallSignalWindowMaxValidSlots.value())
        stats_.stallSignalWindowMaxValidSlots = valid_slots;
    stats_.stallSignalWindowEdgesCaptured += edges_captured;

    if (!trace_)
        return;

    DPRINTF(TaskGraph,
            "Stall signal window cycle=%llu capacity=%u valid=%u "
            "edgesCaptured=%u\n",
            cycle, capacity, valid_slots, edges_captured);
}

void
TaskRuntime::recordStallSignalMerge(unsigned edge_index, uint64_t writes)
{
    if (!enabled_ || writes == 0)
        return;

    if (edge_index >= NumStallSignalEdges)
        return;

    stats_.stallSignalMerges[edge_index] += writes;
}

void
TaskRuntime::recordStallSignalInputRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.stallSignalInputReads++;
    if (!hit)
        stats_.stallSignalInputReadFallbacks++;
}

void
TaskRuntime::recordStallSignalFutureReadBlock()
{
    if (!enabled_)
        return;

    stats_.stallSignalFutureReadBlocks++;
}

void
TaskRuntime::onSerialTickEnd(Cycles cycle, bool deferSafeTasks)
{
    if (!enabled_)
        return;

    if (!deferSafeTasks) {
        waitForAll();
    } else {
        uint64_t deferred_tasks = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            for (const auto &task : inFlight_) {
                fatal_if(task->lifetime !=
                         TaskLifetime::CrossTimeBufferAdvance,
                         "Unsafe task left pending across CPU tick end: "
                         "cycle=%llu taskCycle=%llu stage=%u phase=%u "
                         "tid=%i seq=%llu.",
                         static_cast<unsigned long long>(cycle),
                         static_cast<unsigned long long>(task->order.cycle),
                         static_cast<unsigned>(task->order.stage),
                         static_cast<unsigned>(task->order.phase),
                         task->order.tid,
                         static_cast<unsigned long long>(
                                 task->order.localSeq));
            }
            deferred_tasks = inFlight_.size();
        }

        flushWorkerStats();
        if (deferred_tasks != 0) {
            stats_.serialTickEndSafeDeferrals++;
            stats_.serialTickEndSafeDeferredTasks += deferred_tasks;
            if (deferred_tasks >
                stats_.serialTickEndMaxSafeDeferredTasks.value()) {
                stats_.serialTickEndMaxSafeDeferredTasks = deferred_tasks;
            }
        }
    }

    if (!trace_)
        return;

    DPRINTF(TaskSched,
            "Task runtime serial tick end: cycle=%llu "
            "deferSafeTasks=%i\n",
            cycle, deferSafeTasks);
}

void
TaskRuntime::recordTimeBufferSnapshot(bool input, unsigned slots)
{
    if (!enabled_)
        return;

    if (input)
        stats_.timeBufferInputSnapshots++;
    else
        stats_.timeBufferOutputSnapshots++;

    stats_.timeBufferSlotsCaptured += slots;
}

void
TaskRuntime::recordTimeBufferSnapshotWindow(unsigned capacity,
                                            unsigned input_valid_frames,
                                            unsigned output_valid_frames)
{
    if (!enabled_)
        return;

    stats_.timeBufferSnapshotWindowSamples++;
    stats_.timeBufferSnapshotWindowCapacity += capacity;
    stats_.timeBufferInputWindowValidFrames += input_valid_frames;
    stats_.timeBufferOutputWindowValidFrames += output_valid_frames;

    const unsigned max_valid =
        std::max(input_valid_frames, output_valid_frames);
    if (max_valid > stats_.timeBufferSnapshotWindowMaxValidFrames.value())
        stats_.timeBufferSnapshotWindowMaxValidFrames = max_valid;
}

void
TaskRuntime::recordTimeBufferStageInputRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferStageInputReads++;
    if (!hit)
        stats_.timeBufferStageInputReadMisses++;
}

void
TaskRuntime::recordTimeBufferBackwardSlotRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferBackwardSlotReads++;
    if (!hit)
        stats_.timeBufferBackwardSlotReadMisses++;
}

void
TaskRuntime::recordTimeBufferFetchToDecodeSlotRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferFetchToDecodeSlotReads++;
    if (!hit)
        stats_.timeBufferFetchToDecodeSlotReadMisses++;
}

void
TaskRuntime::recordTimeBufferDecodeToRenameSlotRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferDecodeToRenameSlotReads++;
    if (!hit)
        stats_.timeBufferDecodeToRenameSlotReadMisses++;
}

void
TaskRuntime::recordTimeBufferRenameToIEWSlotRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferRenameToIEWSlotReads++;
    if (!hit)
        stats_.timeBufferRenameToIEWSlotReadMisses++;
}

void
TaskRuntime::recordTimeBufferRenameToCommitSlotRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferRenameToCommitSlotReads++;
    if (!hit)
        stats_.timeBufferRenameToCommitSlotReadMisses++;
}

void
TaskRuntime::recordTimeBufferIEWToCommitSlotRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferIEWToCommitSlotReads++;
    if (!hit)
        stats_.timeBufferIEWToCommitSlotReadMisses++;
}

void
TaskRuntime::recordTimeBufferFetchBackwardSlotRead(bool hit)
{
    if (!enabled_)
        return;

    stats_.timeBufferFetchBackwardSlotReads++;
    if (!hit)
        stats_.timeBufferFetchBackwardSlotReadMisses++;
}

void
TaskRuntime::recordTimeBufferPrepareMerge(uint64_t inst_refs,
                                          uint64_t control_signals,
                                          uint64_t resolved_cfis)
{
    if (!enabled_)
        return;

    stats_.specPrepared++;
    stats_.timeBufferPrepareMerges++;
    stats_.timeBufferPreparedInstRefs += inst_refs;
    stats_.timeBufferPreparedControlSignals += control_signals;
    stats_.timeBufferPreparedResolvedCFIs += resolved_cfis;
}

void
TaskRuntime::recordTimeBufferAdvanceWait(Cycles cycle,
                                         uint64_t pending_pre_advance_tasks,
                                         uint64_t safe_deferred_tasks)
{
    if (!enabled_)
        return;

    if (pending_pre_advance_tasks != 0) {
        stats_.timeBufferAdvanceWaits++;
        stats_.timeBufferAdvancePendingTasks += pending_pre_advance_tasks;
        if (pending_pre_advance_tasks >
            stats_.timeBufferAdvanceMaxPendingTasks.value()) {
            stats_.timeBufferAdvanceMaxPendingTasks =
                pending_pre_advance_tasks;
        }
    }

    if (safe_deferred_tasks != 0) {
        stats_.timeBufferAdvanceSafeDeferrals++;
        stats_.timeBufferAdvanceSafeDeferredTasks += safe_deferred_tasks;
        if (safe_deferred_tasks >
            stats_.timeBufferAdvanceMaxSafeDeferredTasks.value()) {
            stats_.timeBufferAdvanceMaxSafeDeferredTasks =
                safe_deferred_tasks;
        }
    }

    if (trace_) {
        DPRINTF(TaskGraph,
                "TimeBuffer advance lifetime barrier cycle=%llu "
                "pendingPreAdvance=%llu safeDeferred=%llu\n",
                cycle,
                static_cast<unsigned long long>(
                        pending_pre_advance_tasks),
                static_cast<unsigned long long>(safe_deferred_tasks));
    }
}

void
TaskRuntime::recordFutureTimeBufferSnapshot(unsigned slots)
{
    if (!enabled_)
        return;

    stats_.futureTimeBufferInputSnapshots++;
    stats_.futureTimeBufferInputSnapshotSlots += slots;
}

void
TaskRuntime::recordFutureTimeBufferPrepareMerge(uint64_t inst_refs,
                                                uint64_t control_signals,
                                                uint64_t resolved_cfis)
{
    if (!enabled_)
        return;

    stats_.specPrepared++;
    stats_.futureTimeBufferPrepareMerges++;
    stats_.futureTimeBufferPreparedInstRefs += inst_refs;
    stats_.futureTimeBufferPreparedControlSignals += control_signals;
    stats_.futureTimeBufferPreparedResolvedCFIs += resolved_cfis;
}

void
TaskRuntime::recordFutureTimeBufferPrepareSkipped()
{
    if (!enabled_)
        return;

    stats_.futureTimeBufferPrepareSkipped++;
}

bool
TaskRuntime::speculativePrepareAllowed() const
{
    if (!enabled_ || maxSpecTaskWaste_ >= 100)
        return true;

    const uint64_t prepared =
        static_cast<uint64_t>(stats_.specPrepared.value());
    const uint64_t discarded =
        static_cast<uint64_t>(stats_.specDiscarded.value());
    const uint64_t total = prepared + discarded;
    if (total == 0)
        return true;

    return static_cast<long double>(discarded) * 100.0L <=
           static_cast<long double>(total) * maxSpecTaskWaste_;
}

void
TaskRuntime::recordSpecTaskThrottled()
{
    if (!enabled_)
        return;

    stats_.specThrottled++;
}

void
TaskRuntime::recordSpecTaskDiscarded()
{
    if (!enabled_)
        return;

    stats_.specDiscarded++;
}

void
TaskRuntime::recordFutureTimeBufferPrepareReuse()
{
    if (!enabled_)
        return;

    stats_.futureTimeBufferPrepareReuses++;
}

void
TaskRuntime::recordFutureTimeBufferPrepareCheck(bool cycle_match,
                                                bool summary_match)
{
    if (!enabled_)
        return;

    stats_.futureTimeBufferPrepareChecks++;
    if (!cycle_match) {
        stats_.futureTimeBufferPrepareStale++;
    } else if (summary_match) {
        stats_.futureTimeBufferPrepareMatches++;
    } else {
        stats_.futureTimeBufferPrepareMismatches++;
    }
}

void
TaskRuntime::recordFutureWavefrontPrepareProbe()
{
    if (!enabled_)
        return;

    stats_.futureWavefrontPrepareProbes++;
}

void
TaskRuntime::recordFutureWavefrontPrepareMerge()
{
    if (!enabled_)
        return;

    stats_.specPrepared++;
    stats_.futureWavefrontPrepareMerges++;
}

void
TaskRuntime::recordFutureWavefrontPrepareSkipped()
{
    if (!enabled_)
        return;

    stats_.futureWavefrontPrepareSkipped++;
}

void
TaskRuntime::recordFutureWavefrontPrepareCheck(bool cycle_match,
                                               bool latch_match)
{
    if (!enabled_)
        return;

    stats_.futureWavefrontPrepareChecks++;
    if (!cycle_match) {
        stats_.futureWavefrontPrepareStale++;
    } else if (latch_match) {
        stats_.futureWavefrontPrepareMatches++;
    } else {
        stats_.futureWavefrontPrepareMismatches++;
    }
}

void
TaskRuntime::recordFutureRenameWavefrontPrepareProbe()
{
    if (!enabled_)
        return;

    stats_.futureRenameWavefrontPrepareProbes++;
}

void
TaskRuntime::recordFutureRenameWavefrontPrepareMerge()
{
    if (!enabled_)
        return;

    stats_.specPrepared++;
    stats_.futureRenameWavefrontPrepareMerges++;
}

void
TaskRuntime::recordFutureRenameWavefrontPrepareSkipped()
{
    if (!enabled_)
        return;

    stats_.futureRenameWavefrontPrepareSkipped++;
}

void
TaskRuntime::recordFutureRenameWavefrontPrepareCheck(bool cycle_match,
                                                     bool latch_match)
{
    if (!enabled_)
        return;

    stats_.futureRenameWavefrontPrepareChecks++;
    if (!cycle_match) {
        stats_.futureRenameWavefrontPrepareStale++;
    } else if (latch_match) {
        stats_.futureRenameWavefrontPrepareMatches++;
    } else {
        stats_.futureRenameWavefrontPrepareMismatches++;
    }
}

void
TaskRuntime::recordFutureDecodeWavefrontPrepareProbe()
{
    if (!enabled_)
        return;

    stats_.futureDecodeWavefrontPrepareProbes++;
}

void
TaskRuntime::recordFutureDecodeWavefrontPrepareMerge()
{
    if (!enabled_)
        return;

    stats_.specPrepared++;
    stats_.futureDecodeWavefrontPrepareMerges++;
}

void
TaskRuntime::recordFutureDecodeWavefrontPrepareSkipped()
{
    if (!enabled_)
        return;

    stats_.futureDecodeWavefrontPrepareSkipped++;
}

void
TaskRuntime::recordFutureDecodeWavefrontPrepareCheck(bool cycle_match,
                                                     bool latch_match)
{
    if (!enabled_)
        return;

    stats_.futureDecodeWavefrontPrepareChecks++;
    if (!cycle_match) {
        stats_.futureDecodeWavefrontPrepareStale++;
    } else if (latch_match) {
        stats_.futureDecodeWavefrontPrepareMatches++;
    } else {
        stats_.futureDecodeWavefrontPrepareMismatches++;
    }
}

void
TaskRuntime::recordFutureFetchWavefrontPrepareProbe()
{
    if (!enabled_)
        return;

    stats_.futureFetchWavefrontPrepareProbes++;
}

void
TaskRuntime::recordFutureFetchWavefrontPrepareMerge()
{
    if (!enabled_)
        return;

    stats_.specPrepared++;
    stats_.futureFetchWavefrontPrepareMerges++;
}

void
TaskRuntime::recordFutureFetchWavefrontPrepareSkipped()
{
    if (!enabled_)
        return;

    stats_.futureFetchWavefrontPrepareSkipped++;
}

void
TaskRuntime::recordFutureWavefrontSkipReason(
        FutureWavefrontSkipReason reason)
{
    if (!enabled_)
        return;

    const auto index = static_cast<unsigned>(reason);
    if (index >= NumFutureWavefrontSkipReasons)
        return;

    stats_.futureWavefrontSkipReasons[index]++;
}

void
TaskRuntime::recordFutureFetchWavefrontPrepareCheck(bool cycle_match,
                                                    bool output_match)
{
    if (!enabled_)
        return;

    stats_.futureFetchWavefrontPrepareChecks++;
    if (!cycle_match) {
        stats_.futureFetchWavefrontPrepareStale++;
    } else if (output_match) {
        stats_.futureFetchWavefrontPrepareMatches++;
    } else {
        stats_.futureFetchWavefrontPrepareMismatches++;
    }
}

} // namespace o3
} // namespace gem5
