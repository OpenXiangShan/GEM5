/*
 * Copyright (c) 2010-2013, 2018-2019 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
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

// @todo: Fix the instantaneous communication among all the stages within
// iew.  There's a clear delay between issue and execute, yet backwards
// communication happens simultaneously.

#include "cpu/o3/iew.hh"

#include <algorithm>
#include <cassert>
#include <memory>
#include <queue>

#include "arch/riscv/pcstate.hh"
#include "base/output.hh"
#include "base/stats/info.hh"
#include "config/the_isa.hh"
#include "cpu/checker/cpu.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/op_class.hh"
#include "cpu/timebuf.hh"
#include "debug/Activity.hh"
#include "debug/Counters.hh"
#include "debug/DecoupleBP.hh"
#include "debug/Drain.hh"
#include "debug/IEW.hh"
#include "debug/O3PipeView.hh"
#include "debug/Rename.hh"
#include "params/BaseO3CPU.hh"
#include "sim/core.hh"

namespace gem5
{

namespace o3
{

namespace
{

constexpr unsigned NumIEWFutureInputSkipReasons = 4;
constexpr unsigned NumIEWFutureInputCommitControlReasons = 6;
constexpr unsigned NumIEWFutureInputAllowedCommitProgressReasons = 2;
constexpr unsigned NumIEWDispatchDrainPreviewSkipReasons = 3;
constexpr unsigned NumIEWFutureDispatchPreviewDifferenceReasons = 9;
constexpr unsigned NumIEWDispatchOutputSnapshotFields = 8;
constexpr unsigned NumIEWFutureDispatchOutputPublishabilityReasons = 6;
constexpr unsigned NumIEWFutureRenameLatchPreviewDifferenceReasons = 2;
constexpr unsigned NumIEWFutureDispatchBlockTokenDifferenceFields = 9;
constexpr unsigned
    NumIEWFutureDispatchPreviewDispatchedBeforeBlockDiffDirections = 2;
constexpr unsigned NumIEWFutureDispatchPreviewDrainedDiffDirections = 2;

enum IEWFutureInputSkipReason : uint8_t
{
    IEWFutureInputMissingSnapshot,
    IEWFutureInputNoActiveThreads,
    IEWFutureInputCommitControl,
    IEWFutureInputCommitProgressWithLDSTBlock,
};

enum IEWFutureInputCommitControlReason : uint8_t
{
    IEWFutureInputCommitSquash,
    IEWFutureInputCommitRobSquashing,
    IEWFutureInputCommitDoneSeqNum,
    IEWFutureInputCommitDoneMemSeqNum,
    IEWFutureInputCommitNonSpecSeqNum,
    IEWFutureInputCommitStrictlyOrdered,
};

enum IEWFutureInputAllowedCommitProgressReason : uint8_t
{
    IEWFutureInputAllowedDoneSeqNum,
    IEWFutureInputAllowedDoneMemSeqNum,
};

enum IEWDispatchDrainPreviewSkipReason : uint8_t
{
    IEWDispatchDrainPreviewDispatchQueue,
    IEWDispatchDrainPreviewSplitStore,
    IEWDispatchDrainPreviewNeedsSchedulerOrResource,
};

enum IEWFutureDispatchPreviewDifferenceReason : uint8_t
{
    IEWFutureDispatchPreviewActualMissing,
    IEWFutureDispatchPreviewValid,
    IEWFutureDispatchPreviewTid,
    IEWFutureDispatchPreviewVisibleInsts,
    IEWFutureDispatchPreviewDispatchedBeforeBlock,
    IEWFutureDispatchPreviewDrained,
    IEWFutureDispatchPreviewBlockReason,
    IEWFutureDispatchPreviewSchedulerBlockReason,
    IEWFutureDispatchPreviewOther,
};

enum IEWFutureDispatchPreviewDispatchedBeforeBlockDiffDirection : uint8_t
{
    IEWFutureDispatchPreviewFutureLess,
    IEWFutureDispatchPreviewFutureGreater,
};

enum IEWFutureDispatchPreviewDrainedDiffDirection : uint8_t
{
    IEWFutureDispatchPreviewFutureBlockedActualDrained,
    IEWFutureDispatchPreviewFutureDrainedActualBlocked,
};

enum IEWDispatchOutputSnapshotField : uint8_t
{
    IEWDispatchOutputFixedBufferPops,
    IEWDispatchOutputSquashedPops,
    IEWDispatchOutputIQInserts,
    IEWDispatchOutputLQInserts,
    IEWDispatchOutputSQInserts,
    IEWDispatchOutputNonSpecInserts,
    IEWDispatchOutputBarrierInserts,
    IEWDispatchOutputProducerAdds,
};

enum IEWFutureDispatchOutputPublishability : uint8_t
{
    IEWFutureDispatchOutputActualMissing,
    IEWFutureDispatchOutputPreviewDifferent,
    IEWFutureDispatchOutputOutputDifferent,
    IEWFutureDispatchOutputStableDrained,
    IEWFutureDispatchOutputStableBlockedNoSideEffect,
    IEWFutureDispatchOutputStableBlockedSideEffect,
};

enum IEWFutureRenameLatchPreviewDifferenceReason : uint8_t
{
    IEWFutureRenameLatchPreviewBlock,
    IEWFutureRenameLatchPreviewReason,
};

enum IEWFutureDispatchBlockTokenDifferenceField : uint8_t
{
    IEWFutureDispatchBlockTokenValid,
    IEWFutureDispatchBlockTokenReason,
    IEWFutureDispatchBlockTokenIQIndex,
    IEWFutureDispatchBlockTokenSelector,
    IEWFutureDispatchBlockTokenOpClass,
    IEWFutureDispatchBlockTokenDispSeq,
    IEWFutureDispatchBlockTokenFreeEntries,
    IEWFutureDispatchBlockTokenFreeInports,
    IEWFutureDispatchBlockTokenReplayBlocked,
};

const char *
iewFutureInputSkipReasonName(unsigned reason)
{
    switch (reason) {
      case IEWFutureInputMissingSnapshot:
        return "MissingSnapshot";
      case IEWFutureInputNoActiveThreads:
        return "NoActiveThreads";
      case IEWFutureInputCommitControl:
        return "CommitControl";
      case IEWFutureInputCommitProgressWithLDSTBlock:
        return "CommitProgressWithLDSTBlock";
    }

    return "Unknown";
}

const char *
iewFutureInputCommitControlReasonName(unsigned reason)
{
    switch (reason) {
      case IEWFutureInputCommitSquash:
        return "Squash";
      case IEWFutureInputCommitRobSquashing:
        return "RobSquashing";
      case IEWFutureInputCommitDoneSeqNum:
        return "DoneSeqNum";
      case IEWFutureInputCommitDoneMemSeqNum:
        return "DoneMemSeqNum";
      case IEWFutureInputCommitNonSpecSeqNum:
        return "NonSpecSeqNum";
      case IEWFutureInputCommitStrictlyOrdered:
        return "StrictlyOrdered";
    }

    return "Unknown";
}

const char *
iewFutureInputAllowedCommitProgressReasonName(unsigned reason)
{
    switch (reason) {
      case IEWFutureInputAllowedDoneSeqNum:
        return "DoneSeqNum";
      case IEWFutureInputAllowedDoneMemSeqNum:
        return "DoneMemSeqNum";
    }

    return "Unknown";
}

const char *
iewDispatchDrainPreviewSkipReasonName(unsigned reason)
{
    switch (reason) {
      case IEWDispatchDrainPreviewDispatchQueue:
        return "DispatchQueue";
      case IEWDispatchDrainPreviewSplitStore:
        return "SplitStore";
      case IEWDispatchDrainPreviewNeedsSchedulerOrResource:
        return "NeedsSchedulerOrResource";
    }

    return "Unknown";
}

const char *
iewFutureDispatchPreviewDifferenceReasonName(unsigned reason)
{
    switch (reason) {
      case IEWFutureDispatchPreviewActualMissing:
        return "ActualMissing";
      case IEWFutureDispatchPreviewValid:
        return "Valid";
      case IEWFutureDispatchPreviewTid:
        return "Tid";
      case IEWFutureDispatchPreviewVisibleInsts:
        return "VisibleInsts";
      case IEWFutureDispatchPreviewDispatchedBeforeBlock:
        return "DispatchedBeforeBlock";
      case IEWFutureDispatchPreviewDrained:
        return "Drained";
      case IEWFutureDispatchPreviewBlockReason:
        return "BlockReason";
      case IEWFutureDispatchPreviewSchedulerBlockReason:
        return "SchedulerBlockReason";
      case IEWFutureDispatchPreviewOther:
        return "Other";
    }

    return "Unknown";
}

const char *
iewFutureDispatchPreviewDispatchedBeforeBlockDiffDirectionName(
        unsigned reason)
{
    switch (reason) {
      case IEWFutureDispatchPreviewFutureLess:
        return "FutureLess";
      case IEWFutureDispatchPreviewFutureGreater:
        return "FutureGreater";
    }

    return "Unknown";
}

const char *
iewFutureDispatchPreviewDrainedDiffDirectionName(unsigned direction)
{
    switch (direction) {
      case IEWFutureDispatchPreviewFutureBlockedActualDrained:
        return "FutureBlockedActualDrained";
      case IEWFutureDispatchPreviewFutureDrainedActualBlocked:
        return "FutureDrainedActualBlocked";
    }

    return "Unknown";
}

const char *
iewDispatchOutputSnapshotFieldName(unsigned field)
{
    switch (field) {
      case IEWDispatchOutputFixedBufferPops:
        return "FixedBufferPops";
      case IEWDispatchOutputSquashedPops:
        return "SquashedPops";
      case IEWDispatchOutputIQInserts:
        return "IQInserts";
      case IEWDispatchOutputLQInserts:
        return "LQInserts";
      case IEWDispatchOutputSQInserts:
        return "SQInserts";
      case IEWDispatchOutputNonSpecInserts:
        return "NonSpecInserts";
      case IEWDispatchOutputBarrierInserts:
        return "BarrierInserts";
      case IEWDispatchOutputProducerAdds:
        return "ProducerAdds";
    }

    return "Unknown";
}

const char *
iewFutureDispatchOutputPublishabilityName(unsigned reason)
{
    switch (reason) {
      case IEWFutureDispatchOutputActualMissing:
        return "ActualMissing";
      case IEWFutureDispatchOutputPreviewDifferent:
        return "PreviewDifferent";
      case IEWFutureDispatchOutputOutputDifferent:
        return "OutputDifferent";
      case IEWFutureDispatchOutputStableDrained:
        return "StableDrained";
      case IEWFutureDispatchOutputStableBlockedNoSideEffect:
        return "StableBlockedNoSideEffect";
      case IEWFutureDispatchOutputStableBlockedSideEffect:
        return "StableBlockedSideEffect";
    }

    return "Unknown";
}

const char *
iewFutureRenameLatchPreviewDifferenceReasonName(unsigned reason)
{
    switch (reason) {
      case IEWFutureRenameLatchPreviewBlock:
        return "Block";
      case IEWFutureRenameLatchPreviewReason:
        return "Reason";
    }

    return "Unknown";
}

const char *
iewFutureDispatchBlockTokenDifferenceFieldName(unsigned field)
{
    switch (field) {
      case IEWFutureDispatchBlockTokenValid:
        return "Valid";
      case IEWFutureDispatchBlockTokenReason:
        return "Reason";
      case IEWFutureDispatchBlockTokenIQIndex:
        return "IQIndex";
      case IEWFutureDispatchBlockTokenSelector:
        return "Selector";
      case IEWFutureDispatchBlockTokenOpClass:
        return "OpClass";
      case IEWFutureDispatchBlockTokenDispSeq:
        return "DispSeq";
      case IEWFutureDispatchBlockTokenFreeEntries:
        return "FreeEntries";
      case IEWFutureDispatchBlockTokenFreeInports:
        return "FreeInports";
      case IEWFutureDispatchBlockTokenReplayBlocked:
        return "ReplayBlocked";
    }

    return "Unknown";
}

const char *
iewFuturePreviewSkipReasonName(unsigned reason)
{
    using Reason = IEW::FuturePreviewSkipReason;

    switch (static_cast<Reason>(reason)) {
      case Reason::ActiveDispatch:
        return "ActiveDispatch";
      case Reason::MultipleActive:
        return "MultipleActive";
      case Reason::NumReasons:
        break;
    }

    return "Unknown";
}

const char *
iewFutureActiveDispatchSourceName(unsigned source)
{
    using Source = IEW::FutureActiveDispatchSource;

    switch (static_cast<Source>(source)) {
      case Source::ExistingFixedBuffer:
        return "ExistingFixedBuffer";
      case Source::RenameInput:
        return "RenameInput";
      case Source::Mixed:
        return "Mixed";
      case Source::Unknown:
        return "Unknown";
      case Source::NumSources:
        break;
    }

    return "Unknown";
}

const char *
iewFutureActiveDispatchModeName(unsigned mode)
{
    using Mode = IEW::FutureActiveDispatchMode;

    switch (static_cast<Mode>(mode)) {
      case Mode::DirectIssue:
        return "DirectIssue";
      case Mode::DispatchQueue:
        return "DispatchQueue";
      case Mode::NumModes:
        break;
    }

    return "Unknown";
}

const char *
iewFutureActiveDispatchPreviewOutcomeName(unsigned outcome)
{
    using Outcome = IEW::FutureActiveDispatchPreviewOutcome;

    switch (static_cast<Outcome>(outcome)) {
      case Outcome::Skipped:
        return "Skipped";
      case Outcome::DrainedNoResource:
        return "DrainedNoResource";
      case Outcome::DrainedWithResources:
        return "DrainedWithResources";
      case Outcome::BlockedWithResources:
        return "BlockedWithResources";
      case Outcome::NumOutcomes:
        break;
    }

    return "Unknown";
}

const char *
iewFutureActiveDispatchPreviewBlockReasonName(unsigned reason)
{
    using Reason = IEW::FutureActiveDispatchPreviewBlockReason;

    switch (static_cast<Reason>(reason)) {
      case Reason::BuildInputFailed:
        return "BuildInputFailed";
      case Reason::InvalidPreview:
        return "InvalidPreview";
      case Reason::UnsupportedTokens:
        return "UnsupportedTokens";
      case Reason::SerializeBlocked:
        return "SerializeBlocked";
      case Reason::LQFull:
        return "LQFull";
      case Reason::SQFull:
        return "SQFull";
      case Reason::SchedulerNotReady:
        return "SchedulerNotReady";
      case Reason::NumReasons:
        break;
    }

    return "Unknown";
}

const char *
iewFutureDispatchSchedulerBlockReasonName(unsigned reason)
{
    using Reason = IEW::FutureDispatchSchedulerBlockReason;

    switch (static_cast<Reason>(reason)) {
      case Reason::NoBlock:
        return "NoBlock";
      case Reason::InvalidState:
        return "InvalidState";
      case Reason::InvalidOp:
        return "InvalidOp";
      case Reason::InvalidDispSeq:
        return "InvalidDispSeq";
      case Reason::InvalidSelector:
        return "InvalidSelector";
      case Reason::ReplayBlocked:
        return "ReplayBlocked";
      case Reason::IQFull:
        return "IQFull";
      case Reason::InportFull:
        return "InportFull";
      case Reason::NumReasons:
        break;
    }

    return "Unknown";
}

IEW::FutureDispatchSchedulerBlockReason
iewFutureDispatchSchedulerBlockReason(
        DispatchTokenBlockReason reason)
{
    using IEWReason = IEW::FutureDispatchSchedulerBlockReason;
    using SchedulerReason = DispatchTokenBlockReason;

    switch (reason) {
      case SchedulerReason::NoBlock:
        return IEWReason::NoBlock;
      case SchedulerReason::InvalidState:
        return IEWReason::InvalidState;
      case SchedulerReason::InvalidOp:
        return IEWReason::InvalidOp;
      case SchedulerReason::InvalidDispSeq:
        return IEWReason::InvalidDispSeq;
      case SchedulerReason::InvalidSelector:
        return IEWReason::InvalidSelector;
      case SchedulerReason::ReplayBlocked:
        return IEWReason::ReplayBlocked;
      case SchedulerReason::IQFull:
        return IEWReason::IQFull;
      case SchedulerReason::InportFull:
        return IEWReason::InportFull;
      case SchedulerReason::NumReasons:
        break;
    }

    return IEWReason::NumReasons;
}

} // anonymous namespace

IEW::IEW(CPU *_cpu, const BaseO3CPUParams &params)
    : dqSize(params.numDQEntries),
      issueToExecQueue(params.backComSize, params.forwardComSize),
      valuePred(params.valuePred),
      cpu(_cpu),
      scheduler(params.scheduler),
      instQueue(_cpu, this, params),
      ldstQueue(_cpu, this, params),
      dispWidth(params.dispWidth),
      commitToIEWDelay(params.commitToIEWDelay),
      renameToIEWDelay(params.renameToIEWDelay),
      enableDispatchStage(params.enableDispatchStage),
      renameWidth(params.renameWidth),
      wbNumInst(0),
      wbCycle(0),
      iewToCommitDelay(params.iewToCommitDelay),
      wbWidth(params.wbWidth),
      enableStoreSetTrain(params.enable_storeSet_train),
      numThreads(params.numThreads),
      iewStats(cpu)
{
    if (wbWidth > MaxWidth)
        fatal("wbWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             wbWidth, static_cast<int>(MaxWidth));

    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

    // Setup wire to read instructions coming from issue.
    fromIssue = issueToExecQueue.getWire(0);

    // Instruction queue needs the queue between issue and execute.
    instQueue.setIssueToExecuteQueue(&issueToExecQueue);

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        fetchRedirect[tid] = false;
        serializeOnNextInst[tid] = false;
    }

    assert(renameToIEWDelay == 1);

    updateLSQNextCycle = false;

    skidBufferMax = (renameToIEWDelay + 1) * params.renameWidth * 2;

    dispatchStalls.resize(renameWidth, StallReason::NoStall);

}

std::string
IEW::name() const
{
    return cpu->name() + ".iew";
}

void
IEW::regProbePoints()
{
    ppDispatch = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Dispatch");
    ppMispredict = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Mispredict");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction starts to execute.
     */
    ppExecute = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Execute");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction execution completes and it is marked ready to commit.
     */
    ppToCommit = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "ToCommit");
}

IEW::IEWStats::IEWStats(CPU *cpu)
    : statistics::Group(cpu, "iew"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is idle"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is squashing"),
    ADD_STAT(blockCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is blocking"),
    ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
             "Number of cycles IEW is unblocking"),
    ADD_STAT(dispatchedInsts, statistics::units::Count::get(),
             "Number of instructions dispatched to IQ"),
    ADD_STAT(dispSquashedInsts, statistics::units::Count::get(),
             "Number of squashed instructions skipped by dispatch"),
    ADD_STAT(prepareTasks, statistics::units::Count::get(),
             "Number of IEW prepare tasks submitted"),
    ADD_STAT(prepareMerges, statistics::units::Count::get(),
             "Number of IEW prepare results merged"),
    ADD_STAT(prepareActiveThreads, statistics::units::Count::get(),
             "Accumulated active thread count seen by IEW prepare"),
    ADD_STAT(prepareBlockedThreads, statistics::units::Count::get(),
             "Accumulated blocked thread count seen by IEW prepare"),
    ADD_STAT(prepareInlineEmptyThreads, statistics::units::Count::get(),
             "Accumulated empty-thread count evaluated inline by IEW prepare"),
    ADD_STAT(prepareMultipleActive, statistics::units::Count::get(),
             "Number of times IEW prepare saw multiple active threads"),
    ADD_STAT(futurePrepareProbes, statistics::units::Count::get(),
             "Number of future IEW prepare probes submitted"),
    ADD_STAT(futurePrepareSkipped, statistics::units::Count::get(),
             "Number of future IEW prepare probes skipped"),
    ADD_STAT(futureInputSkipReasons, statistics::units::Count::get(),
             "Breakdown of why future IEW prepare input construction was "
             "not safe"),
    ADD_STAT(futureInputCommitControlReasons,
             statistics::units::Count::get(),
             "Breakdown of which commit control field blocked future IEW "
             "prepare input construction"),
    ADD_STAT(futureInputAllowedCommitProgress,
             statistics::units::Count::get(),
             "Occurrences of commit progress fields accepted by future IEW "
             "prepare input construction"),
    ADD_STAT(futurePreviewSkipReasons,
             statistics::units::Count::get(),
             "Breakdown of why future IEW preview could not safely predict "
             "the IEW-to-Rename latch"),
    ADD_STAT(futureActiveDispatchSources,
             statistics::units::Count::get(),
             "Source of active dispatches that blocked future IEW preview"),
    ADD_STAT(futureActiveDispatchModes,
             statistics::units::Count::get(),
             "Dispatch mode for active dispatches that blocked future IEW "
             "preview"),
    ADD_STAT(futureActiveDispatchPreviewOutcomes,
             statistics::units::Count::get(),
             "Preview outcome for active dispatches seen by future IEW "
             "preview"),
    ADD_STAT(futureActiveDispatchPreviewBlockReasons,
             statistics::units::Count::get(),
             "Block reason for skipped active dispatches seen by future IEW "
             "preview"),
    ADD_STAT(futureActiveDispatchSchedulerBlockReasons,
             statistics::units::Count::get(),
             "Scheduler token reason for future SchedulerNotReady dispatch "
             "previews"),
    ADD_STAT(futureActiveDispatchInsts,
             statistics::units::Count::get(),
             "Visible instruction count for active dispatches seen by "
             "future IEW preview"),
    ADD_STAT(futurePrepareMerges, statistics::units::Count::get(),
             "Number of future IEW prepare results merged"),
    ADD_STAT(futurePrepareReuses, statistics::units::Count::get(),
             "Number of future IEW prepare results reused"),
    ADD_STAT(futurePrepareChecks, statistics::units::Count::get(),
             "Number of future IEW prepare results checked"),
    ADD_STAT(futurePrepareMatches, statistics::units::Count::get(),
             "Number of future IEW prepare checks that matched"),
    ADD_STAT(futurePrepareMismatches, statistics::units::Count::get(),
             "Number of future IEW prepare checks that mismatched"),
    ADD_STAT(futurePrepareStale, statistics::units::Count::get(),
             "Number of stale future IEW prepare results"),
    ADD_STAT(dispatchStatusPrepareTasks, statistics::units::Count::get(),
             "Number of IEW dispatch status prepare tasks submitted"),
    ADD_STAT(dispatchStatusPrepareMerges, statistics::units::Count::get(),
             "Number of IEW dispatch status prepare results merged"),
    ADD_STAT(dispatchStatusPrepareMismatches,
             statistics::units::Count::get(),
             "Number of IEW dispatch status prepare validation mismatches"),
    ADD_STAT(dispatchDrainPreviewProbes, statistics::units::Count::get(),
             "Number of direct-dispatch drain previews built"),
    ADD_STAT(dispatchDrainPreviewSkipped, statistics::units::Count::get(),
             "Number of direct-dispatch drain previews skipped"),
    ADD_STAT(dispatchDrainPreviewSkipReasons,
             statistics::units::Count::get(),
             "Breakdown of why direct-dispatch drain preview was skipped"),
    ADD_STAT(dispatchDrainPreviewMatches, statistics::units::Count::get(),
             "Number of direct-dispatch drain preview checks that matched"),
    ADD_STAT(dispatchDrainPreviewMismatches, statistics::units::Count::get(),
             "Number of direct-dispatch drain preview checks that mismatched"),
    ADD_STAT(dispatchDrainPreviewStallReasonMatches,
             statistics::units::Count::get(),
             "Number of blocked direct-dispatch drain preview stall reasons "
             "that matched"),
    ADD_STAT(dispatchDrainPreviewStallReasonMismatches,
             statistics::units::Count::get(),
             "Number of blocked direct-dispatch drain preview stall reasons "
             "that mismatched"),
    ADD_STAT(dispatchDrainPreviewStallReasonSideEffectSkips,
             statistics::units::Count::get(),
             "Number of blocked direct-dispatch drain preview stall reason "
             "checks skipped after previewed dispatch side effects"),
    ADD_STAT(dispatchOutputSnapshotChecks,
             statistics::units::Count::get(),
             "Number of current-cycle direct-dispatch output snapshots "
             "checked"),
    ADD_STAT(dispatchOutputSnapshotMatches,
             statistics::units::Count::get(),
             "Number of current-cycle direct-dispatch output snapshots "
             "matching actual dispatch side effects"),
    ADD_STAT(dispatchOutputSnapshotMismatches,
             statistics::units::Count::get(),
             "Number of current-cycle direct-dispatch output snapshots "
             "mismatching actual dispatch side effects"),
    ADD_STAT(dispatchOutputSnapshotMismatchFields,
             statistics::units::Count::get(),
             "Fields mismatching in current-cycle dispatch output snapshots"),
    ADD_STAT(futureDispatchPreviewChecks,
             statistics::units::Count::get(),
             "Number of future direct-dispatch previews checked next cycle"),
    ADD_STAT(futureDispatchPreviewMatches,
             statistics::units::Count::get(),
             "Number of future direct-dispatch previews matching "
             "current-cycle preview"),
    ADD_STAT(futureDispatchPreviewDifferences,
             statistics::units::Count::get(),
             "Number of future direct-dispatch previews differing from "
             "current-cycle preview"),
    ADD_STAT(futureDispatchPreviewDifferenceReasons,
             statistics::units::Count::get(),
             "Breakdown of future direct-dispatch preview differences"),
    ADD_STAT(futureDispatchPreviewDispatchedBeforeBlockDiffDirections,
             statistics::units::Count::get(),
             "Direction of dispatched-before-block count differences in "
             "future direct-dispatch preview checks"),
    ADD_STAT(futureDispatchPreviewDrainedDiffDirections,
             statistics::units::Count::get(),
             "Direction of drained/block state differences in future "
             "direct-dispatch preview checks"),
    ADD_STAT(futureDispatchPreviewDispatchedBeforeBlockDelta,
             statistics::units::Count::get(),
             "Absolute dispatched-before-block count difference in future "
             "direct-dispatch preview checks"),
    ADD_STAT(futureDispatchOutputSnapshotChecks,
             statistics::units::Count::get(),
             "Number of future direct-dispatch output snapshots checked "
             "next cycle"),
    ADD_STAT(futureDispatchOutputSnapshotMatches,
             statistics::units::Count::get(),
             "Number of future direct-dispatch output snapshots matching "
             "current-cycle preview"),
    ADD_STAT(futureDispatchOutputSnapshotDifferences,
             statistics::units::Count::get(),
             "Number of future direct-dispatch output snapshots differing "
             "from current-cycle preview"),
    ADD_STAT(futureDispatchOutputSnapshotDifferenceFields,
             statistics::units::Count::get(),
             "Fields differing in future dispatch output snapshots"),
    ADD_STAT(futureDispatchOutputPublishability,
             statistics::units::Count::get(),
             "Publishability classification for checked future dispatch "
             "output snapshots"),
    ADD_STAT(futureDispatchOutputStableBlockedReasons,
             statistics::units::Count::get(),
             "Block reason for stable future blocked dispatch output "
             "snapshots"),
    ADD_STAT(futureDispatchOutputStableBlockedSchedulerReasons,
             statistics::units::Count::get(),
             "Scheduler token reason for stable future SchedulerNotReady "
             "dispatch output snapshots"),
    ADD_STAT(futureDispatchOutputStableBlockedPops,
             statistics::units::Count::get(),
             "Fixedbuffer pops in stable future blocked dispatch output "
             "snapshots"),
    ADD_STAT(futureDispatchOutputPreviewDifferentReasons,
             statistics::units::Count::get(),
             "Expected block reason for future dispatch previews that "
             "differed from the next-cycle current preview"),
    ADD_STAT(futureDispatchOutputPreviewDifferentSchedulerReasons,
             statistics::units::Count::get(),
             "Expected scheduler token reason for future SchedulerNotReady "
             "dispatch previews that differed from the next-cycle current "
             "preview"),
    ADD_STAT(futureDispatchOutputPreviewDifferentPops,
             statistics::units::Count::get(),
             "Expected fixedbuffer pops in future dispatch previews that "
             "differed from the next-cycle current preview"),
    ADD_STAT(futureDispatchBlockTokenChecks,
             statistics::units::Count::get(),
             "Number of future scheduler block token snapshots checked next "
             "cycle"),
    ADD_STAT(futureDispatchBlockTokenMatches,
             statistics::units::Count::get(),
             "Number of future scheduler block token snapshots matching "
             "next-cycle current preview"),
    ADD_STAT(futureDispatchBlockTokenDifferences,
             statistics::units::Count::get(),
             "Number of future scheduler block token snapshots differing "
             "from next-cycle current preview"),
    ADD_STAT(futureDispatchBlockTokenDifferenceFields,
             statistics::units::Count::get(),
             "Fields differing in future scheduler block token snapshots"),
    ADD_STAT(futureDispatchBlockTokenMatchesByPublishability,
             statistics::units::Count::get(),
             "Future scheduler block token snapshot matches by dispatch "
             "output publishability class"),
    ADD_STAT(futureDispatchBlockTokenDifferencesByPublishability,
             statistics::units::Count::get(),
             "Future scheduler block token snapshot differences by dispatch "
             "output publishability class"),
    ADD_STAT(futureRenameLatchPreviewChecks,
             statistics::units::Count::get(),
             "Number of future IEW-to-Rename latch previews checked next "
             "cycle"),
    ADD_STAT(futureRenameLatchPreviewMatches,
             statistics::units::Count::get(),
             "Number of future IEW-to-Rename latch previews matching actual "
             "latch"),
    ADD_STAT(futureRenameLatchPreviewDifferences,
             statistics::units::Count::get(),
             "Number of future IEW-to-Rename latch previews differing from "
             "actual latch"),
    ADD_STAT(futureRenameLatchPreviewDifferenceReasons,
             statistics::units::Count::get(),
             "Breakdown of future IEW-to-Rename latch preview differences"),
    ADD_STAT(futureRenameLatchPreviewMatchesByPublishability,
             statistics::units::Count::get(),
             "Future IEW-to-Rename latch preview matches by dispatch output "
             "publishability class"),
    ADD_STAT(futureRenameLatchPreviewDifferencesByPublishability,
             statistics::units::Count::get(),
             "Future IEW-to-Rename latch preview differences by dispatch "
             "output publishability class"),
    ADD_STAT(futureRenameLatchPreviewStale,
             statistics::units::Count::get(),
             "Number of future IEW-to-Rename latch previews discarded before "
             "checking"),
    ADD_STAT(futureDispatchPreviewStale,
             statistics::units::Count::get(),
             "Number of future direct-dispatch previews discarded before "
             "checking"),
    ADD_STAT(writebackPrepareTasks, statistics::units::Count::get(),
             "Number of IEW writeback prepare tasks submitted"),
    ADD_STAT(writebackPrepareMerges, statistics::units::Count::get(),
             "Number of IEW writeback prepare results merged"),
    ADD_STAT(writebackPrepareNoWork, statistics::units::Count::get(),
             "Number of IEW writeback prepare cycles with no entries"),
    ADD_STAT(writebackPrepareMismatches, statistics::units::Count::get(),
             "Number of IEW writeback prepare validation mismatches"),
    ADD_STAT(dispLoadInsts, statistics::units::Count::get(),
             "Number of dispatched load instructions"),
    ADD_STAT(dispStoreInsts, statistics::units::Count::get(),
             "Number of dispatched store instructions"),
    ADD_STAT(dispNonSpecInsts, statistics::units::Count::get(),
             "Number of dispatched non-speculative instructions"),
    ADD_STAT(iqFullEvents, statistics::units::Count::get(),
             "Number of times the IQ has become full, causing a stall"),
    ADD_STAT(lsqFullEvents, statistics::units::Count::get(),
             "Number of times the LSQ has become full, causing a stall"),
    ADD_STAT(memOrderViolationEvents, statistics::units::Count::get(),
             "Number of memory order violations"),
    ADD_STAT(predictedTakenIncorrect, statistics::units::Count::get(),
             "Number of branches that were predicted taken incorrectly"),
    ADD_STAT(predictedNotTakenIncorrect, statistics::units::Count::get(),
             "Number of branches that were predicted not taken incorrectly"),
    ADD_STAT(branchMispredicts, statistics::units::Count::get(),
             "Number of branch mispredicts detected at execute",
             predictedTakenIncorrect + predictedNotTakenIncorrect),
    ADD_STAT(dispDist, statistics::units::Count::get(),
             "Number of branch mispredicts detected at execute"),
    executedInstStats(cpu),
    ADD_STAT(instsToCommit, statistics::units::Count::get(),
             "Cumulative count of insts sent to commit"),
    ADD_STAT(writebackCount, statistics::units::Count::get(),
             "Cumulative count of insts written-back"),
    ADD_STAT(producerInst, statistics::units::Count::get(),
             "Number of instructions producing a value"),
    ADD_STAT(consumerInst, statistics::units::Count::get(),
             "Number of instructions consuming a value"),
    ADD_STAT(wbRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Insts written-back per cycle"),
    ADD_STAT(wbFanout, statistics::units::Rate<
                statistics::units::Count, statistics::units::Count>::get(),
             "Average fanout of values written-back"),
    ADD_STAT(stallEvents, statistics::units::Count::get(),
             "Number of events the IEW has stalled"),
    ADD_STAT(fetchStallReason, statistics::units::Count::get(),
             "Number of fetch stall reasons each tick (Total)"),
    ADD_STAT(decodeStallReason, statistics::units::Count::get(),
             "Number of decode stall reasons each tick (Total)"),
    ADD_STAT(renameStallReason, statistics::units::Count::get(),
             "Number of rename stall reasons each tick (Total)"),
    ADD_STAT(dispatchStallReason, statistics::units::Count::get(),
             "Number of dispatch stall reasons each tick (Total)")
{
    instsToCommit
        .init(cpu->numThreads)
        .flags(statistics::total);

    writebackCount
        .init(cpu->numThreads)
        .flags(statistics::total);

    producerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    consumerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    wbRate
        .flags(statistics::total);
    wbRate = writebackCount / cpu->baseStats.numCycles;

    wbFanout
        .flags(statistics::total);
    wbFanout = producerInst / consumerInst;

    stallEvents
        .init(StallEventCount)
        .flags(statistics::total);

    dispDist.init(0,10,1).flags(statistics::nozero);

    std::map < StallEvent, const char* > stall_event_str = {
        { CacheMiss, "CacheMiss" },
        { Translation, "Translation" },
        { ROBWalk, "ROBWalk" },
        { IQFull, "IQFull" },
        { LSQFull, "LSQFull" },
        { DispBWFull, "DispBWFull" }
    };

    for (int i = 0; i < StallEventCount; i++) {
        stallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
    }

    futureInputSkipReasons
        .init(NumIEWFutureInputSkipReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumIEWFutureInputSkipReasons; ++i)
        futureInputSkipReasons.subname(i, iewFutureInputSkipReasonName(i));

    futureInputCommitControlReasons
        .init(NumIEWFutureInputCommitControlReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumIEWFutureInputCommitControlReasons; ++i) {
        futureInputCommitControlReasons.subname(
            i, iewFutureInputCommitControlReasonName(i));
    }

    futureInputAllowedCommitProgress
        .init(NumIEWFutureInputAllowedCommitProgressReasons)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureInputAllowedCommitProgressReasons; ++i) {
        futureInputAllowedCommitProgress.subname(
            i, iewFutureInputAllowedCommitProgressReasonName(i));
    }

    futurePreviewSkipReasons
        .init(static_cast<unsigned>(
            IEW::FuturePreviewSkipReason::NumReasons))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FuturePreviewSkipReason::NumReasons); ++i) {
        futurePreviewSkipReasons.subname(
            i, iewFuturePreviewSkipReasonName(i));
    }

    futureActiveDispatchSources
        .init(static_cast<unsigned>(
            IEW::FutureActiveDispatchSource::NumSources))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FutureActiveDispatchSource::NumSources); ++i) {
        futureActiveDispatchSources.subname(
            i, iewFutureActiveDispatchSourceName(i));
    }

    futureActiveDispatchModes
        .init(static_cast<unsigned>(
            IEW::FutureActiveDispatchMode::NumModes))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FutureActiveDispatchMode::NumModes); ++i) {
        futureActiveDispatchModes.subname(
            i, iewFutureActiveDispatchModeName(i));
    }

    futureActiveDispatchPreviewOutcomes
        .init(static_cast<unsigned>(
            IEW::FutureActiveDispatchPreviewOutcome::NumOutcomes))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FutureActiveDispatchPreviewOutcome::NumOutcomes); ++i) {
        futureActiveDispatchPreviewOutcomes.subname(
            i, iewFutureActiveDispatchPreviewOutcomeName(i));
    }

    futureActiveDispatchPreviewBlockReasons
        .init(static_cast<unsigned>(
            IEW::FutureActiveDispatchPreviewBlockReason::NumReasons))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FutureActiveDispatchPreviewBlockReason::NumReasons); ++i) {
        futureActiveDispatchPreviewBlockReasons.subname(
            i, iewFutureActiveDispatchPreviewBlockReasonName(i));
    }

    futureActiveDispatchSchedulerBlockReasons
        .init(static_cast<unsigned>(
            IEW::FutureDispatchSchedulerBlockReason::NumReasons))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FutureDispatchSchedulerBlockReason::NumReasons); ++i) {
        futureActiveDispatchSchedulerBlockReasons.subname(
            i, iewFutureDispatchSchedulerBlockReasonName(i));
    }

    futureActiveDispatchInsts.init(0, MaxWidth + 1, 1)
        .flags(statistics::nozero);

    dispatchDrainPreviewSkipReasons
        .init(NumIEWDispatchDrainPreviewSkipReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumIEWDispatchDrainPreviewSkipReasons; ++i) {
        dispatchDrainPreviewSkipReasons.subname(
            i, iewDispatchDrainPreviewSkipReasonName(i));
    }

    dispatchOutputSnapshotMismatchFields
        .init(NumIEWDispatchOutputSnapshotFields)
        .flags(statistics::total);
    futureDispatchOutputSnapshotDifferenceFields
        .init(NumIEWDispatchOutputSnapshotFields)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumIEWDispatchOutputSnapshotFields; ++i) {
        const char *name = iewDispatchOutputSnapshotFieldName(i);
        dispatchOutputSnapshotMismatchFields.subname(i, name);
        futureDispatchOutputSnapshotDifferenceFields.subname(i, name);
    }

    futureDispatchOutputPublishability
        .init(NumIEWFutureDispatchOutputPublishabilityReasons)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureDispatchOutputPublishabilityReasons; ++i) {
        futureDispatchOutputPublishability.subname(
            i, iewFutureDispatchOutputPublishabilityName(i));
    }

    futureDispatchOutputStableBlockedReasons
        .init(static_cast<unsigned>(
            IEW::FutureActiveDispatchPreviewBlockReason::NumReasons))
        .flags(statistics::total);
    futureDispatchOutputPreviewDifferentReasons
        .init(static_cast<unsigned>(
            IEW::FutureActiveDispatchPreviewBlockReason::NumReasons))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FutureActiveDispatchPreviewBlockReason::NumReasons); ++i) {
        const char *name = iewFutureActiveDispatchPreviewBlockReasonName(i);
        futureDispatchOutputStableBlockedReasons.subname(i, name);
        futureDispatchOutputPreviewDifferentReasons.subname(i, name);
    }

    futureDispatchOutputStableBlockedSchedulerReasons
        .init(static_cast<unsigned>(
            IEW::FutureDispatchSchedulerBlockReason::NumReasons))
        .flags(statistics::total);
    futureDispatchOutputPreviewDifferentSchedulerReasons
        .init(static_cast<unsigned>(
            IEW::FutureDispatchSchedulerBlockReason::NumReasons))
        .flags(statistics::total);
    for (unsigned i = 0;
         i < static_cast<unsigned>(
             IEW::FutureDispatchSchedulerBlockReason::NumReasons); ++i) {
        const char *name = iewFutureDispatchSchedulerBlockReasonName(i);
        futureDispatchOutputStableBlockedSchedulerReasons.subname(i, name);
        futureDispatchOutputPreviewDifferentSchedulerReasons.subname(i, name);
    }

    futureDispatchOutputStableBlockedPops
        .init(0, MaxWidth * 2 + 1, 1)
        .flags(statistics::nozero);
    futureDispatchOutputPreviewDifferentPops
        .init(0, MaxWidth * 2 + 1, 1)
        .flags(statistics::nozero);

    futureDispatchBlockTokenDifferenceFields
        .init(NumIEWFutureDispatchBlockTokenDifferenceFields)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumIEWFutureDispatchBlockTokenDifferenceFields;
         ++i) {
        futureDispatchBlockTokenDifferenceFields.subname(
            i, iewFutureDispatchBlockTokenDifferenceFieldName(i));
    }

    futureDispatchBlockTokenMatchesByPublishability
        .init(NumIEWFutureDispatchOutputPublishabilityReasons)
        .flags(statistics::total);
    futureDispatchBlockTokenDifferencesByPublishability
        .init(NumIEWFutureDispatchOutputPublishabilityReasons)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureDispatchOutputPublishabilityReasons; ++i) {
        const char *name = iewFutureDispatchOutputPublishabilityName(i);
        futureDispatchBlockTokenMatchesByPublishability.subname(i, name);
        futureDispatchBlockTokenDifferencesByPublishability.subname(i, name);
    }

    futureRenameLatchPreviewDifferenceReasons
        .init(NumIEWFutureRenameLatchPreviewDifferenceReasons)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureRenameLatchPreviewDifferenceReasons; ++i) {
        futureRenameLatchPreviewDifferenceReasons.subname(
            i, iewFutureRenameLatchPreviewDifferenceReasonName(i));
    }

    futureRenameLatchPreviewMatchesByPublishability
        .init(NumIEWFutureDispatchOutputPublishabilityReasons)
        .flags(statistics::total);
    futureRenameLatchPreviewDifferencesByPublishability
        .init(NumIEWFutureDispatchOutputPublishabilityReasons)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureDispatchOutputPublishabilityReasons; ++i) {
        const char *name = iewFutureDispatchOutputPublishabilityName(i);
        futureRenameLatchPreviewMatchesByPublishability.subname(i, name);
        futureRenameLatchPreviewDifferencesByPublishability.subname(
            i, name);
    }

    futureDispatchPreviewDifferenceReasons
        .init(NumIEWFutureDispatchPreviewDifferenceReasons)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureDispatchPreviewDifferenceReasons; ++i) {
        futureDispatchPreviewDifferenceReasons.subname(
            i, iewFutureDispatchPreviewDifferenceReasonName(i));
    }

    futureDispatchPreviewDispatchedBeforeBlockDiffDirections
        .init(NumIEWFutureDispatchPreviewDispatchedBeforeBlockDiffDirections)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureDispatchPreviewDispatchedBeforeBlockDiffDirections;
         ++i) {
        futureDispatchPreviewDispatchedBeforeBlockDiffDirections.subname(
            i,
            iewFutureDispatchPreviewDispatchedBeforeBlockDiffDirectionName(
                i));
    }

    futureDispatchPreviewDrainedDiffDirections
        .init(NumIEWFutureDispatchPreviewDrainedDiffDirections)
        .flags(statistics::total);
    for (unsigned i = 0;
         i < NumIEWFutureDispatchPreviewDrainedDiffDirections; ++i) {
        futureDispatchPreviewDrainedDiffDirections.subname(
                i, iewFutureDispatchPreviewDrainedDiffDirectionName(i));
    }

    futureDispatchPreviewDispatchedBeforeBlockDelta
        .init(0, MaxWidth * 2 + 1, 1)
        .flags(statistics::nozero);

    fetchStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    decodeStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    renameStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    dispatchStallReason
            .init(NumStallReasons)
            .flags(statistics::total | statistics::pdf);

    std::map <StallReason, const char*> stallReasonStr = {
        {StallReason::NoStall, "NoStall"},
        {StallReason::IcacheStall, "IcacheStall"},
        {StallReason::ITlbStall, "ITlbStall"},
        {StallReason::DTlbStall, "DTlbStall"},
        {StallReason::BpStall, "BpStall"},
        {StallReason::IntStall, "IntStall"},
        {StallReason::TrapStall, "TrapStall"},
        {StallReason::FetchFragStall, "FetchFragStall"},
        {StallReason::OtherFragStall, "OtherFragStall"},
        {StallReason::SquashStall, "SquashStall"},
        {StallReason::FetchBufferInvalid, "FetchBufferInvalid"},
        {StallReason::InstMisPred, "InstMisPred"},
        {StallReason::InstSquashed, "InstSquashed"},
        {StallReason::SerializeStall, "SerializeStall"},
        {StallReason::VectorLongExecute, "VectorLongExecute"},
        {StallReason::ScalarLongExecute, "ScalarLongExecute"},
        {StallReason::InstNotReady, "InstNotReady"},
        {StallReason::LoadL1Bound, "LoadL1Bound"},
        {StallReason::LoadL2Bound, "LoadL2Bound"},
        {StallReason::LoadL3Bound, "LoadL3Bound"},
        {StallReason::LoadMemBound, "LoadMemBound"},
        {StallReason::StoreL1Bound, "StoreL1Bound"},
        {StallReason::StoreL2Bound, "StoreL2Bound"},
        {StallReason::StoreL3Bound, "StoreL3Bound"},
        {StallReason::StoreMemBound, "StoreMemBound"},
        {StallReason::MemSquashed, "MemSquashed"},
        {StallReason::Atomic,"Atomic"},
        {StallReason::ResumeUnblock, "ResumeUnblock"},
        {StallReason::CommitSquash, "CommitSquash"},
        {StallReason::ROBFull, "ROBFull"},
        {StallReason::RegFull, "RegFull"},
        {StallReason::OtherStall, "OtherStall"},
        {StallReason::OtherFetchStall, "OtherFetchStall"},
        {StallReason::FTQBubble, "FTQBubble"},
        {StallReason::MemDQBandwidth, "MemDQBandwidth"},
        {StallReason::FVDQBandwidth, "FVDQBandwidth"},
        {StallReason::IntDQBandwidth, "IntDQBandwidth"},
        {StallReason::MemNotReady, "MemNotReady"},
        {StallReason::MemCommitRateLimit, "MemCommitRateLimit"},
        {StallReason::OtherMemStall, "OtherMemStall"},
        {StallReason::VectorReadyButNotIssued, "VectorReadyButNotIssued"},
        {StallReason::ScalarReadyButNotIssued, "ScalarReadyButNotIssued"}
    };

    for (int i = 0;i < NumStallReasons;i++) {
        fetchStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        decodeStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        renameStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
        dispatchStallReason.subname(i, stallReasonStr[static_cast<StallReason>(i)]);
    }
}

IEW::IEWStats::ExecutedInstStats::ExecutedInstStats(CPU *cpu)
    : statistics::Group(cpu, "executed_inst"),
    ADD_STAT(numInsts, statistics::units::Count::get(),
             "Number of executed instructions"),
    ADD_STAT(numLoadInsts, statistics::units::Count::get(),
             "Number of load instructions executed"),
    ADD_STAT(numSquashedInsts, statistics::units::Count::get(),
             "Number of squashed instructions skipped in execute"),
    ADD_STAT(numSwp, statistics::units::Count::get(),
             "Number of swp insts executed"),
    ADD_STAT(numNop, statistics::units::Count::get(),
             "Number of nop insts executed"),
    ADD_STAT(numRefs, statistics::units::Count::get(),
             "Number of memory reference insts executed"),
    ADD_STAT(numBranches, statistics::units::Count::get(),
             "Number of branches executed"),
    ADD_STAT(numStoreInsts, statistics::units::Count::get(),
             "Number of stores executed"),
    ADD_STAT(numRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Inst execution rate", numInsts / cpu->baseStats.numCycles)
{
    numLoadInsts
        .init(cpu->numThreads)
        .flags(statistics::total);

    numSwp
        .init(cpu->numThreads)
        .flags(statistics::total);

    numNop
        .init(cpu->numThreads)
        .flags(statistics::total);

    numRefs
        .init(cpu->numThreads)
        .flags(statistics::total);

    numBranches
        .init(cpu->numThreads)
        .flags(statistics::total);

    numStoreInsts
        .flags(statistics::total);
    numStoreInsts = numRefs - numLoadInsts;

    numRate
        .flags(statistics::total);
}

void
IEW::startupStage()
{
    // Initialize the checker's dcache port here
    if (cpu->checker) {
        cpu->checker->setDcachePort(&ldstQueue.getDataPort());
    }

    cpu->activateStage(CPU::IEWIdx);
}

void
IEW::clearStates(ThreadID tid)
{
}

void
IEW::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to read information from time buffer, from commit.
    fromCommit = timeBuffer->getWire(-commitToIEWDelay);

    // Setup wire to write information back to previous stages.
    toRename = timeBuffer->getWire(0);

    toFetch = timeBuffer->getWire(0);

    // Instruction queue also needs main time buffer.
    instQueue.setTimeBuffer(tb_ptr);
}

void
IEW::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to read information from rename queue.
    fromRename = renameQueue->getWire(-renameToIEWDelay);
}

void
IEW::setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)
{
    iewQueue = iq_ptr;

    // Setup wire to write instructions and squash signals to commit.
    // Note: This wire is named "toCommit" (previously "execWB") to clarify its purpose.
    // Since iewToCommitDelay == 1, IEW writes to position [-1] and Commit reads from
    // position [-1] (via fromIEW) in the same cycle, achieving zero-cycle latency for
    // IEW→Commit communication. This allows Commit to immediately arbitrate squash
    // signals from IEW (e.g., branch mispredictions) before forwarding to Fetch.
    toCommit = iewQueue->getWire(0);
}

void
IEW::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;

    ldstQueue.setActiveThreads(at_ptr);
    instQueue.setActiveThreads(at_ptr);
}

void
IEW::setScoreboard(Scoreboard *sb_ptr)
{
    scoreboard = sb_ptr;
}

void
IEW::lvpWakeDependents(const DynInstPtr &inst) {
    assert(inst->numDestRegs() == 1);
    if (enableSelectiveVPFlush) {
        scheduler->specWakeUpFromVP(inst);
        DPRINTF(IEW,"[sn:%llu] vp specWakeUp dependents\n", inst->seqNum);
    } else {
        for (int i = 0; i < inst->numDestRegs(); i++) {
            auto dest = inst->renamedDestIdx(i);
            if (dest->isFixedMapping()) {
                continue;
            }
            scheduler->setAllScoreBoard(dest);
            DPRINTF(IEW,"[sn:%llu] vp set scoreboard to true\n", inst->seqNum);
        }
    }
}

bool
IEW::isDrained() const
{
    bool drained = ldstQueue.isDrained() && instQueue.isDrained();
    if (!drained) {
        DPRINTF(Drain, "LDSTQ or IQ not drained.\n");
        return false;
    }

    for (int i=0;i<numThreads;i++) {
        if (!fixedbuffer[i].empty()) {
            DPRINTF(Drain, "%i: Insts not empty.\n", i);
            drained = false;
            break;
        }
    }

    return drained;
}

void
IEW::drainSanityCheck() const
{
    assert(isDrained());

    instQueue.drainSanityCheck();
    ldstQueue.drainSanityCheck();
}

void
IEW::takeOverFrom()
{
    // Reset all state.
    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

    instQueue.takeOverFrom();
    ldstQueue.takeOverFrom();

    startupStage();
    cpu->activityThisCycle();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        fetchRedirect[tid] = false;
    }

    updateLSQNextCycle = false;

    for (int i = 0; i < issueToExecQueue.getSize(); ++i) {
        issueToExecQueue.advance();
    }
}

void
IEW::setRenameStall(ThreadID tid, bool block, StallReason reason)
{
    if (stallSignalBank) {
        stallSignalBank->set(StallSignalEdge::IEWToRename, tid, block,
                             reason);
    } else {
        stallSig->blockRename[tid] = block;
        stallSig->renameBlockReason[tid] = reason;
    }
    cpu->getTaskRuntime().recordStallSignalMerge(
            static_cast<unsigned>(StallSignalEdge::IEWToRename), 1);
}

void
IEW::setRenameBlock(ThreadID tid, bool block)
{
    if (stallSignalBank) {
        stallSignalBank->setBlock(StallSignalEdge::IEWToRename, tid, block);
    } else {
        stallSig->blockRename[tid] = block;
    }
    cpu->getTaskRuntime().recordStallSignalMerge(
            static_cast<unsigned>(StallSignalEdge::IEWToRename), 1);
}

IEW::IEWPrepareInput
IEW::buildIEWPrepareInput(Cycles cycle,
                          const StallSignalLatch *commit_to_iew_override,
                          const RenameStruct *snapshot_rename,
                          bool reset_lsq_pop_entries)
{
    IEWPrepareInput input;
    input.cycle = cycle;
    input.numThreads = numThreads;
    input.dispatchStageEnabled = enableDispatchStage;

    const StallSignalLatch *commit_to_iew =
        commit_to_iew_override ? commit_to_iew_override :
        (stallSignalBank ?
        &cpu->stallSignalSnapshotOrCurrent(
                cycle, StallSignalEdge::CommitToIEW) :
        nullptr);

    for (int tid = 0; tid < numThreads; ++tid) {
        input.fixedbufferSize[tid] = fixedbuffer[tid].size();
        for (const auto &inst : fixedbuffer[tid]) {
            if (inst && inst->isSquashed())
                ++input.fixedbufferSquashedInsts[tid];
        }
        if (snapshot_rename && snapshot_rename->size > 0) {
            const int renamed_insts =
                std::min(snapshot_rename->size, MaxWidth);
            for (int i = 0; i < renamed_insts; ++i) {
                const DynInstPtr &inst = snapshot_rename->insts[i];
                if (inst && !inst->isSquashed() &&
                    inst->threadNumber == tid) {
                    ++input.renameInputInsts[tid];
                }
            }
        }
        const bool fixedbuffer_empty =
            input.fixedbufferSize[tid] == 0 &&
            input.renameInputInsts[tid] == 0;
        input.fixedbufferEmpty[tid] = fixedbuffer_empty;
        input.commitToIEW.block[tid] =
            commit_to_iew ? commit_to_iew->block[tid] :
                            stallSig->blockIEW[tid];
        input.commitToIEW.reason[tid] =
            commit_to_iew ? commit_to_iew->reason[tid] :
                            stallSig->iewBlockReason[tid];
        input.ldstCanInsert[tid] =
            canInsertLDSTQue(tid, reset_lsq_pop_entries);
        input.ldstBlockReason[tid] = StallReason::NoStall;
        if (!input.ldstCanInsert[tid]) {
            input.ldstBlockReason[tid] =
                checkDispatchStall(tid, NumDQ, nullptr, -1);
            if (input.ldstBlockReason[tid] == StallReason::NoStall)
                input.ldstBlockReason[tid] = StallReason::OtherStall;
        }
    }

    return input;
}

IEW::IEWThreadPrepareResult
IEW::prepareIEWThreadControl(const IEWPrepareInput &input,
                             ThreadID tid) const
{
    IEWThreadPrepareResult result;
    result.cycle = input.cycle;
    result.tid = tid;

    const bool commit_block = input.commitToIEW.block[tid];
    const bool ldst_block = !input.ldstCanInsert[tid];
    const bool block = commit_block || ldst_block;
    const bool active = !block && !input.fixedbufferEmpty[tid];
    StallReason block_reason = StallReason::NoStall;

    if (commit_block) {
        block_reason = input.commitToIEW.reason[tid];
    } else if (ldst_block) {
        block_reason = input.ldstBlockReason[tid];
    }

    result.commitBlock = commit_block;
    result.ldstBlock = ldst_block;
    result.block = block;
    result.active = active;
    result.blocked = block;
    result.renameBlock = block;
    result.renameBlockReason = block ? block_reason : StallReason::NoStall;

    return result;
}

IEW::IEWPrepareResult
IEW::combineIEWThreadPrepareResults(
        const IEWPrepareInput &input,
        const IEWThreadPrepareResults &thread_results) const
{
    IEWPrepareResult result;
    result.cycle = input.cycle;

    for (int tid = 0; tid < input.numThreads; ++tid) {
        const auto &thread_result = thread_results.byThread[tid];

        result.commitBlock[tid] = thread_result.commitBlock;
        result.ldstBlock[tid] = thread_result.ldstBlock;
        result.block[tid] = thread_result.block;
        result.active[tid] = thread_result.active;
        result.renameBlock[tid] = thread_result.renameBlock;
        result.renameBlockReason[tid] = thread_result.renameBlockReason;

        if (thread_result.active) {
            ++result.activeThreads;
            if (result.selectedTid == InvalidThreadID) {
                result.selectedTid = tid;
            } else {
                result.multipleActive = true;
                result.renameBlock[result.selectedTid] = true;
                result.renameBlock[tid] = true;
            }
        } else if (thread_result.blocked) {
            ++result.blockedThreads;
        }
    }

    return result;
}

IEW::IEWPrepareResult
IEW::prepareIEWControl(const IEWPrepareInput &input) const
{
    IEWThreadPrepareResults thread_results;
    for (int tid = 0; tid < input.numThreads; ++tid) {
        thread_results.byThread[tid] =
            prepareIEWThreadControl(input, static_cast<ThreadID>(tid));
    }

    return combineIEWThreadPrepareResults(input, thread_results);
}

void
IEW::mergeIEWPrepareResult(const IEWPrepareResult &result,
                           bool countPrepareStats)
{
    lastPrepareResult = result;

    if (countPrepareStats) {
        iewStats.prepareMerges++;
        iewStats.prepareActiveThreads += result.activeThreads;
        iewStats.prepareBlockedThreads += result.blockedThreads;
        if (result.multipleActive)
            iewStats.prepareMultipleActive++;
    }
}

bool
IEW::samePrepareResult(const IEWPrepareResult &lhs,
                       const IEWPrepareResult &rhs) const
{
    if (lhs.cycle != rhs.cycle ||
        lhs.selectedTid != rhs.selectedTid ||
        lhs.activeThreads != rhs.activeThreads ||
        lhs.blockedThreads != rhs.blockedThreads ||
        lhs.multipleActive != rhs.multipleActive) {
        return false;
    }

    for (int tid = 0; tid < numThreads; ++tid) {
        if (lhs.commitBlock[tid] != rhs.commitBlock[tid] ||
            lhs.ldstBlock[tid] != rhs.ldstBlock[tid] ||
            lhs.block[tid] != rhs.block[tid] ||
            lhs.active[tid] != rhs.active[tid] ||
            lhs.renameBlock[tid] != rhs.renameBlock[tid] ||
            lhs.renameBlockReason[tid] != rhs.renameBlockReason[tid]) {
            return false;
        }
    }

    return true;
}

IEW::IEWPrepareResult
IEW::runIEWPrepare(Cycles cycle)
{
    auto input = std::make_shared<IEWPrepareInput>(
            buildIEWPrepareInput(cycle));
    auto result = std::make_shared<IEWPrepareResult>();

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled()) {
        *result = prepareIEWControl(*input);
        mergeIEWPrepareResult(*result, false);
        return *result;
    }

    if (pendingFuturePrepare.valid) {
        if (pendingFuturePrepare.result.cycle == cycle) {
            *result = pendingFuturePrepare.result;
            pendingFuturePrepare.valid = false;
            iewStats.futurePrepareReuses++;
            mergeIEWPrepareResult(*result, true);

            if (runtime.selfTestEnabled()) {
                const IEWPrepareResult expected = prepareIEWControl(*input);
                iewStats.futurePrepareChecks++;
                if (samePrepareResult(*result, expected)) {
                    iewStats.futurePrepareMatches++;
                } else {
                    iewStats.futurePrepareMismatches++;
                }
            }

            return lastPrepareResult;
        }

        iewStats.futurePrepareChecks++;
        iewStats.futurePrepareStale++;
        pendingFuturePrepare.valid = false;
    }

    iewStats.prepareTasks++;
    auto thread_results = std::make_shared<IEWThreadPrepareResults>();
    for (int tid = 0; tid < input->numThreads; ++tid) {
        if (input->fixedbufferEmpty[tid]) {
            thread_results->byThread[tid] =
                prepareIEWThreadControl(
                        *input, static_cast<ThreadID>(tid));
            iewStats.prepareInlineEmptyThreads++;
            continue;
        }

        const TaskOrderKey thread_order{
            cycle, TaskStage::IEW, 1, InvalidThreadID,
            static_cast<uint64_t>(tid)};
        runtime.submitWeak(
                thread_order,
                1,
                [this, input, thread_results, tid] {
                    thread_results->byThread[tid] =
                        prepareIEWThreadControl(
                                *input, static_cast<ThreadID>(tid));
                });
    }

    const TaskOrderKey merge_order{
        cycle, TaskStage::IEW, 1, InvalidThreadID,
        static_cast<uint64_t>(input->numThreads)};
    runtime.submitWeak(
            merge_order,
            0,
            [] {},
            [this, input, thread_results, result] {
                *result = combineIEWThreadPrepareResults(
                        *input, *thread_results);
                mergeIEWPrepareResult(*result, true);
            });
    runtime.waitForOrder(merge_order);

    return lastPrepareResult;
}

bool
IEW::buildFutureRenameLatchInput(Cycles cycle,
                                 const StallSignalLatch &commit_to_iew,
                                 const RenameStruct *snapshot_rename,
                                 const TimeStruct *snapshot_commit,
                                 IEWPrepareInput &input)
{
    if (!snapshot_rename || !snapshot_commit) {
        iewStats.futureInputSkipReasons[IEWFutureInputMissingSnapshot]++;
        return false;
    }
    if (activeThreads->empty()) {
        iewStats.futureInputSkipReasons[IEWFutureInputNoActiveThreads]++;
        return false;
    }

    for (ThreadID tid : *activeThreads) {
        const auto &commit_info = snapshot_commit->commitInfo[tid];
        unsigned reason = NumIEWFutureInputCommitControlReasons;
        if (commit_info.squash) {
            reason = IEWFutureInputCommitSquash;
        } else if (commit_info.robSquashing) {
            reason = IEWFutureInputCommitRobSquashing;
        } else if (commit_info.strictlyOrdered) {
            reason = IEWFutureInputCommitStrictlyOrdered;
        } else if (commit_info.nonSpecSeqNum != 0) {
            reason = IEWFutureInputCommitNonSpecSeqNum;
        }
        if (reason != NumIEWFutureInputCommitControlReasons) {
            iewStats.futureInputSkipReasons[IEWFutureInputCommitControl]++;
            iewStats.futureInputCommitControlReasons[reason]++;
            return false;
        }
    }

    input = buildIEWPrepareInput(
            cycle, &commit_to_iew, snapshot_rename, false);

    unsigned accepted_done_seq = 0;
    unsigned accepted_done_mem_seq = 0;
    for (ThreadID tid : *activeThreads) {
        const auto &commit_info = snapshot_commit->commitInfo[tid];
        if (commit_info.doneSeqNum != 0 || commit_info.doneMemSeqNum != 0) {
            // Commit progress is applied after prepare in IEW::tick().
            // It is only unsafe here when the current LDST block reason can
            // still change before the next-cycle self-test consumes it.
            if (!input.ldstCanInsert[tid]) {
                iewStats.futureInputSkipReasons[
                    IEWFutureInputCommitProgressWithLDSTBlock]++;
                return false;
            }
            if (commit_info.doneSeqNum != 0)
                ++accepted_done_seq;
            if (commit_info.doneMemSeqNum != 0)
                ++accepted_done_mem_seq;
        }
    }
    iewStats.futureInputAllowedCommitProgress[
        IEWFutureInputAllowedDoneSeqNum] += accepted_done_seq;
    iewStats.futureInputAllowedCommitProgress[
        IEWFutureInputAllowedDoneMemSeqNum] += accepted_done_mem_seq;
    return true;
}

bool
IEW::previewFutureRenameLatch(const IEWPrepareInput &input,
                              StallSignalLatch &iew_to_rename,
                              IEWPrepareResult *prepare_result) const
{
    const IEWPrepareResult result = prepareIEWControl(input);
    if (prepare_result)
        *prepare_result = result;

    if (result.selectedTid != InvalidThreadID &&
        !futureActiveDispatchDrainsWithoutResources(input, result)) {
        return false;
    }

    iew_to_rename.clear();
    for (int tid = 0; tid < numThreads; ++tid) {
        iew_to_rename.block[tid] = result.renameBlock[tid];
        iew_to_rename.reason[tid] = result.renameBlockReason[tid];
    }

    return true;
}

bool
IEW::previewFutureRenameLatch(
        const IEWPrepareInput &input,
        const RenameStruct *snapshot_rename,
        const TimeStruct *snapshot_commit,
        StallSignalLatch &iew_to_rename,
        IEWPrepareResult *prepare_result,
        FutureActiveDispatchPreviewOutcome *dispatch_outcome,
        FutureActiveDispatchPreviewBlockReason *dispatch_block_reason,
        FutureDispatchCandidateProfile *dispatch_profile)
{
    const IEWPrepareResult result = prepareIEWControl(input);
    if (prepare_result)
        *prepare_result = result;
    if (dispatch_outcome) {
        *dispatch_outcome =
            FutureActiveDispatchPreviewOutcome::NumOutcomes;
    }
    if (dispatch_block_reason) {
        *dispatch_block_reason =
            FutureActiveDispatchPreviewBlockReason::NumReasons;
    }
    if (dispatch_profile)
        *dispatch_profile = FutureDispatchCandidateProfile();

    bool active_dispatch_drained = true;
    if (result.selectedTid != InvalidThreadID) {
        if (futureActiveDispatchDrainsWithoutResources(input, result)) {
            if (dispatch_outcome) {
                *dispatch_outcome =
                    FutureActiveDispatchPreviewOutcome::DrainedNoResource;
            }
        } else {
            FutureDispatchPreviewInput dispatch_input;
            if (!buildFutureDispatchPreviewInput(
                        input, result, snapshot_rename, snapshot_commit,
                        dispatch_input)) {
                if (dispatch_block_reason) {
                    *dispatch_block_reason =
                        FutureActiveDispatchPreviewBlockReason::
                        BuildInputFailed;
                }
                return false;
            }

            const DispatchDrainPreviewResult dispatch_result =
                previewFutureDirectDispatchDrain(dispatch_input);
            if (dispatch_profile) {
                dispatch_profile->valid = dispatch_result.valid;
                dispatch_profile->drained = dispatch_result.drained;
                dispatch_profile->fixedBufferPops =
                    dispatch_result.output.fixedBufferPops;
                dispatch_profile->dispatchedBeforeBlock =
                    dispatch_result.dispatchedBeforeBlock;
                dispatch_profile->blockReason =
                    dispatch_result.blockReason;
                dispatch_profile->schedulerBlockReason =
                    dispatch_result.schedulerBlockReason;
            }
            if (!dispatch_result.valid) {
                if (dispatch_block_reason) {
                    *dispatch_block_reason =
                        dispatch_result.blockReason ==
                        FutureActiveDispatchPreviewBlockReason::NumReasons ?
                        FutureActiveDispatchPreviewBlockReason::
                        InvalidPreview :
                        dispatch_result.blockReason;
                }
                return false;
            }
            setPendingFutureDispatchPreview(dispatch_result);
            StallSignalLatch predicted_iew_to_rename;
            predicted_iew_to_rename.clear();
            for (int tid = 0; tid < numThreads; ++tid) {
                predicted_iew_to_rename.block[tid] =
                    result.renameBlock[tid];
                predicted_iew_to_rename.reason[tid] =
                    result.renameBlockReason[tid];
            }
            if (!dispatch_result.drained) {
                predicted_iew_to_rename.block[result.selectedTid] = true;
            }
            setPendingFutureRenameLatchPreview(
                    input.cycle, predicted_iew_to_rename);
            iew_to_rename = predicted_iew_to_rename;
            if (!dispatch_result.drained) {
                if (dispatch_result.dispatchedBeforeBlock == 0 &&
                    dispatch_result.blockReason ==
                    FutureActiveDispatchPreviewBlockReason::
                    SerializeBlocked) {
                    active_dispatch_drained = false;
                    if (dispatch_outcome) {
                        *dispatch_outcome =
                            FutureActiveDispatchPreviewOutcome::
                            BlockedWithResources;
                    }
                } else if (dispatch_result.blockReason ==
                           FutureActiveDispatchPreviewBlockReason::
                           SchedulerNotReady) {
                    active_dispatch_drained = false;
                    if (dispatch_outcome) {
                        *dispatch_outcome =
                            FutureActiveDispatchPreviewOutcome::
                            BlockedWithResources;
                    }
                } else {
                    if (dispatch_result.blockReason ==
                        FutureActiveDispatchPreviewBlockReason::
                        SchedulerNotReady) {
                        const unsigned reason = static_cast<unsigned>(
                                dispatch_result.schedulerBlockReason);
                        if (reason < static_cast<unsigned>(
                                FutureDispatchSchedulerBlockReason::
                                NumReasons)) {
                            iewStats.futureActiveDispatchSchedulerBlockReasons[
                                reason]++;
                        }
                    }
                    if (dispatch_block_reason) {
                        *dispatch_block_reason =
                            dispatch_result.blockReason ==
                            FutureActiveDispatchPreviewBlockReason::NumReasons ?
                            FutureActiveDispatchPreviewBlockReason::
                            InvalidPreview :
                            dispatch_result.blockReason;
                    }
                    return false;
                }
            } else {
                if (dispatch_outcome) {
                    *dispatch_outcome =
                        FutureActiveDispatchPreviewOutcome::
                        DrainedWithResources;
                }
            }
        }
    }

    iew_to_rename.clear();
    for (int tid = 0; tid < numThreads; ++tid) {
        iew_to_rename.block[tid] = result.renameBlock[tid];
        iew_to_rename.reason[tid] = result.renameBlockReason[tid];
    }

    if (result.selectedTid != InvalidThreadID && !active_dispatch_drained)
        iew_to_rename.block[result.selectedTid] = true;

    return true;
}

IEW::IEWPrepareResult
IEW::previewFuturePrepare(const IEWPrepareInput &input) const
{
    return prepareIEWControl(input);
}

IEW::FuturePreviewSkipReason
IEW::futurePreviewSkipReason(const IEWPrepareResult &result)
{
    if (result.multipleActive)
        return FuturePreviewSkipReason::MultipleActive;
    return FuturePreviewSkipReason::ActiveDispatch;
}

IEW::FutureActiveDispatchSource
IEW::futureActiveDispatchSource(const IEWPrepareInput &input,
                                const IEWPrepareResult &result)
{
    if (result.selectedTid == InvalidThreadID)
        return FutureActiveDispatchSource::Unknown;

    const ThreadID tid = result.selectedTid;
    const bool has_existing = input.fixedbufferSize[tid] != 0;
    const bool has_rename = input.renameInputInsts[tid] != 0;
    if (has_existing && has_rename)
        return FutureActiveDispatchSource::Mixed;
    if (has_existing)
        return FutureActiveDispatchSource::ExistingFixedBuffer;
    if (has_rename)
        return FutureActiveDispatchSource::RenameInput;
    return FutureActiveDispatchSource::Unknown;
}

IEW::FutureActiveDispatchMode
IEW::futureActiveDispatchMode(const IEWPrepareInput &input)
{
    return input.dispatchStageEnabled ?
        FutureActiveDispatchMode::DispatchQueue :
        FutureActiveDispatchMode::DirectIssue;
}

bool
IEW::futureActiveDispatchDrainsWithoutResources(
        const IEWPrepareInput &input, const IEWPrepareResult &result)
{
    if (result.selectedTid == InvalidThreadID || result.multipleActive)
        return false;

    const ThreadID tid = result.selectedTid;
    if (tid >= input.numThreads)
        return false;

    return !input.dispatchStageEnabled &&
        input.renameInputInsts[tid] == 0 &&
        input.fixedbufferSize[tid] != 0 &&
        input.fixedbufferSquashedInsts[tid] == input.fixedbufferSize[tid];
}

bool
IEW::buildFutureDispatchPreviewInput(
        const IEWPrepareInput &input,
        const IEWPrepareResult &result,
        const RenameStruct *snapshot_rename,
        const TimeStruct *snapshot_commit,
        FutureDispatchPreviewInput &preview_input)
{
    preview_input = FutureDispatchPreviewInput();

    if (result.selectedTid == InvalidThreadID || result.multipleActive ||
        input.dispatchStageEnabled || !snapshot_commit) {
        return false;
    }

    const ThreadID tid = result.selectedTid;
    if (tid >= input.numThreads)
        return false;

    preview_input.valid = true;
    preview_input.cycle = input.cycle;
    preview_input.tid = tid;
    preview_input.freeLQEntries = ldstQueue.numFreeLoadEntries(tid);
    preview_input.freeSQEntries = ldstQueue.numFreeStoreEntries(tid);
    preview_input.serializeNext = serializeOnNextInst[tid];
    preview_input.robHeadSeqNum =
        snapshot_commit->commitInfo[tid].robheadSeqNum;

    auto append_inst = [this, &preview_input](
            const DynInstPtr &inst, bool squash_by_version) -> bool
    {
        if (!inst)
            return true;

        if (preview_input.entries >= MaxFutureDispatchPreviewEntries)
            return false;

        auto &entry = preview_input.insts[preview_input.entries++];
        entry.valid = true;
        entry.squashed = inst->isSquashed() || squash_by_version;
        entry.splitStoreAddr = inst->staticInst->isSplitStoreAddr();
        entry.atomic = inst->isAtomic();
        entry.load = inst->isLoad();
        entry.store = inst->isStore();
        entry.storeConditional = inst->isStoreConditional();
        entry.readBarrier = inst->isReadBarrier();
        entry.writeBarrier = inst->isWriteBarrier();
        entry.nop = inst->isNop();
        entry.eliminated = inst->isEliminated();
        entry.nonSpeculative = inst->isNonSpeculative();
        entry.serializeBefore = inst->isSerializeBefore();
        entry.serializeAfter = inst->isSerializeAfter();
        entry.opClass = inst->opClass();
        entry.seqNum = inst->seqNum;
        return true;
    };

    for (const auto &inst : fixedbuffer[tid]) {
        if (!append_inst(inst, false))
            return false;
    }

    if (snapshot_rename && snapshot_rename->size > 0) {
        const DynInstPtr &first_inst = snapshot_rename->insts[0];
        if (first_inst && first_inst->threadNumber == tid) {
            if (!fixedbuffer[tid].empty())
                return false;

            for (int i = 0; i < snapshot_rename->size; ++i) {
                const DynInstPtr &inst = snapshot_rename->insts[i];
                if (!inst)
                    continue;
                if (inst->threadNumber != tid)
                    return false;
                const bool squash_by_version =
                    localSquashVer.largerThan(inst->getVersion());
                if (squash_by_version)
                    continue;
                if (!append_inst(inst, false))
                    return false;
            }
        } else if (input.renameInputInsts[tid] != 0) {
            return false;
        }
    } else if (input.renameInputInsts[tid] != 0) {
        return false;
    }

    return true;
}

bool
IEW::previewFutureRenameLatch(Cycles cycle,
                              const StallSignalLatch &commit_to_iew,
                              const RenameStruct *snapshot_rename,
                              const TimeStruct *snapshot_commit,
                              StallSignalLatch &iew_to_rename,
                              IEWPrepareResult *prepare_result)
{
    IEWPrepareInput input;
    if (!buildFutureRenameLatchInput(
                cycle, commit_to_iew, snapshot_rename, snapshot_commit,
                input)) {
        return false;
    }

    return previewFutureRenameLatch(input, snapshot_rename, snapshot_commit,
                                    iew_to_rename, prepare_result);
}

void
IEW::recordFuturePrepareProbe()
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    iewStats.futurePrepareProbes++;
}

void
IEW::recordFuturePrepareSkipped()
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    iewStats.futurePrepareSkipped++;
}

void
IEW::recordFuturePreviewSkipped(FuturePreviewSkipReason reason)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    const unsigned index = static_cast<unsigned>(reason);
    if (index >= static_cast<unsigned>(FuturePreviewSkipReason::NumReasons))
        return;
    iewStats.futurePreviewSkipReasons[index]++;
}

void
IEW::recordFutureActiveDispatchPreviewSkipped(
        const IEWPrepareInput &input, const IEWPrepareResult &result,
        FutureActiveDispatchPreviewBlockReason block_reason)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    recordFuturePreviewSkipped(futurePreviewSkipReason(result));

    if (futurePreviewSkipReason(result) !=
        FuturePreviewSkipReason::ActiveDispatch) {
        return;
    }

    const unsigned source = static_cast<unsigned>(
            futureActiveDispatchSource(input, result));
    if (source < static_cast<unsigned>(
            FutureActiveDispatchSource::NumSources)) {
        iewStats.futureActiveDispatchSources[source]++;
    }

    const unsigned mode = static_cast<unsigned>(
            futureActiveDispatchMode(input));
    if (mode < static_cast<unsigned>(FutureActiveDispatchMode::NumModes))
        iewStats.futureActiveDispatchModes[mode]++;

    const ThreadID tid = result.selectedTid;
    if (tid < input.numThreads) {
        iewStats.futureActiveDispatchPreviewOutcomes[
            static_cast<unsigned>(
                FutureActiveDispatchPreviewOutcome::Skipped)]++;
        const unsigned reason = static_cast<unsigned>(block_reason);
        if (reason < static_cast<unsigned>(
                FutureActiveDispatchPreviewBlockReason::NumReasons)) {
            iewStats.futureActiveDispatchPreviewBlockReasons[reason]++;
        }
        iewStats.futureActiveDispatchInsts.sample(
            input.fixedbufferSize[tid] + input.renameInputInsts[tid]);
    }
}

void
IEW::recordFutureActiveDispatchPreviewAccepted(
        const IEWPrepareInput &input, const IEWPrepareResult &result,
        FutureActiveDispatchPreviewOutcome outcome)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    if (outcome == FutureActiveDispatchPreviewOutcome::NumOutcomes)
        return;

    const ThreadID tid = result.selectedTid;
    if (tid >= input.numThreads)
        return;

    iewStats.futureActiveDispatchPreviewOutcomes[
        static_cast<unsigned>(outcome)]++;
    iewStats.futureActiveDispatchInsts.sample(
        input.fixedbufferSize[tid] + input.renameInputInsts[tid]);
}

void
IEW::setPendingFuturePrepare(const IEWPrepareResult &result)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    if (pendingFuturePrepare.valid)
        iewStats.futurePrepareStale++;

    pendingFuturePrepare.result = result;
    pendingFuturePrepare.valid = true;
    iewStats.futurePrepareMerges++;
}

IEW::DispatchHeadSnapshot
IEW::snapshotDispatchHead(const DynInstPtr &inst) const
{
    DispatchHeadSnapshot snapshot;
    if (!inst)
        return snapshot;

    snapshot.valid = true;
    snapshot.squashed = inst->isSquashed();
    snapshot.committed = inst->isCommitted();
    snapshot.atomic = inst->isAtomic();
    snapshot.storeConditional = inst->isStoreConditional();
    snapshot.load = inst->isLoad();
    snapshot.store = inst->isStore();
    snapshot.vector = inst->isVector();
    snapshot.nonSpeculative = inst->isNonSpeculative();
    snapshot.readyToIssue = inst->readyToIssue();
    snapshot.issued = inst->isIssued();
    snapshot.translationStarted = inst->translationStarted();
    snapshot.translationCompleted = inst->translationCompleted();
    snapshot.hasPendingCacheReq = inst->hasPendingCacheReq();
    snapshot.readyTickUnset = inst->readyTick == -1;
    snapshot.firstIssueSet = inst->firstIssue != -1;

    if (snapshot.hasPendingCacheReq) {
        fatal_if(!inst->pendingCacheReq,
                 "Instruction reports a pending cache request but has no "
                 "request object.");
        snapshot.pendingCacheDepth = inst->pendingCacheReq->mainReq()->depth;
    }

    return snapshot;
}

IEW::DispatchStatusPrepareInput
IEW::buildDispatchStatusPrepareInput(Cycles cycle, ThreadID tid)
{
    DispatchStatusPrepareInput input;
    input.cycle = cycle;
    input.tid = tid;
    input.lqEmpty = ldstQueue.lqEmpty();
    input.sqEmpty = ldstQueue.sqEmpty();
    input.lqFull = ldstQueue.lqFull(tid);
    input.sqFull = ldstQueue.sqFull(tid);

    const DynInstPtr &rob_head = rob->readHeadInst(tid);
    if (rob_head != rob->dummyInst)
        input.robHead = snapshotDispatchHead(rob_head);

    if (!input.lqEmpty)
        input.lqHead = snapshotDispatchHead(
                ldstQueue.getLSQHeadInst(tid, true));
    if (!input.sqEmpty)
        input.sqHead = snapshotDispatchHead(
                ldstQueue.getLSQHeadInst(tid, false));

    return input;
}

StallReason
IEW::checkLoadStoreSnapshot(const DispatchHeadSnapshot &inst) const
{
    fatal_if(!inst.valid,
             "IEW dispatch status prepare expected a valid LSQ/ROB head.");

    if (inst.squashed) {
        return StallReason::MemSquashed;
    }
    if (inst.committed) {
        return StallReason::MemCommitRateLimit;
    }
    if (inst.atomic || inst.storeConditional) {
        return StallReason::Atomic;
    }
    if (!inst.readyToIssue) {
        return StallReason::MemNotReady;
    }
    fatal_if(!(inst.load || inst.store),
             "IEW dispatch status prepare saw a non-memory head in the "
             "memory-stall classifier.");

    if (inst.issued && inst.translationStarted &&
        !inst.translationCompleted) {
        return StallReason::DTlbStall;
    }

    const bool in_flight = inst.issued && inst.hasPendingCacheReq;
    const bool lsu_stall = inst.issued && !inst.hasPendingCacheReq;
    const int depth = inst.pendingCacheDepth;

    assert(depth < 5);
    const bool in_l1 = depth == 0;
    const bool in_l2 = depth == 1;
    const bool in_l3 = depth == 2;
    const bool other_stall = depth == -1;
    const bool in_mem = !(in_l1 || in_l2 || in_l3 || other_stall);

    if (in_flight && in_l1) {
        return inst.load ? StallReason::LoadL1Bound :
                           StallReason::StoreL1Bound;
    } else if (in_flight && in_l2) {
        return inst.load ? StallReason::LoadL2Bound :
                           StallReason::StoreL2Bound;
    } else if (in_flight && in_l3) {
        return inst.load ? StallReason::LoadL3Bound :
                           StallReason::StoreL3Bound;
    } else if (in_flight && in_mem) {
        return inst.load ? StallReason::LoadMemBound :
                           StallReason::StoreMemBound;
    } else if (in_flight && other_stall) {
        return StallReason::OtherMemStall;
    }

    if (lsu_stall) {
        return inst.load ? StallReason::LoadL1Bound :
                           StallReason::StoreL1Bound;
    }
    return StallReason::OtherMemStall;
}

StallReason
IEW::checkDispatchHeadSnapshot(
        const DispatchStatusPrepareInput &input) const
{
    const auto &head = input.robHead;
    if (!head.valid)
        return StallReason::NoStall;

    if (head.readyTickUnset) {
        DPRINTF(Counters,
                "IEW: [tid:%i] Dispatch: Instruction not ready. "
                "nonSpeculative:%d\n",
                input.tid, head.nonSpeculative);
        if (head.nonSpeculative) {
            return StallReason::SerializeStall;
        } else if (head.load && input.lqFull) {
            return checkLoadStoreSnapshot(input.lqHead);
        } else if ((head.store || head.atomic) && input.sqFull) {
            return checkLoadStoreSnapshot(input.sqHead);
        } else {
            return StallReason::InstNotReady;
        }
    }

    if (head.load || head.store || head.atomic) {
        return checkLoadStoreSnapshot(head);
    }

    if (head.firstIssueSet) {
        return head.vector ? StallReason::VectorLongExecute :
                             StallReason::ScalarLongExecute;
    }
    return head.vector ? StallReason::VectorReadyButNotIssued :
                         StallReason::ScalarReadyButNotIssued;
}

IEW::DispatchStatusPrepareResult
IEW::prepareDispatchStatusControl(
        const DispatchStatusPrepareInput &input) const
{
    DispatchStatusPrepareResult result;
    result.cycle = input.cycle;
    result.tid = input.tid;
    result.robHeadStallReason = checkDispatchHeadSnapshot(input);
    result.lqHeadStallReason =
        input.lqEmpty ? StallReason::NoStall :
                        checkLoadStoreSnapshot(input.lqHead);
    result.sqHeadStallReason =
        input.sqEmpty ? StallReason::NoStall :
                        checkLoadStoreSnapshot(input.sqHead);
    return result;
}

IEW::DispatchStatusPrepareResult
IEW::runDispatchStatusPrepare(Cycles cycle, ThreadID tid)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled()) {
        DispatchStatusPrepareResult result;
        result.cycle = cycle;
        result.tid = tid;
        result.robHeadStallReason =
            checkDispatchStall(tid, NumDQ, nullptr, -1);
        result.lqHeadStallReason =
            ldstQueue.lqEmpty() ? StallReason::NoStall :
                                  checkLSQStall(tid, true);
        result.sqHeadStallReason =
            ldstQueue.sqEmpty() ? StallReason::NoStall :
                                  checkLSQStall(tid, false);
        return result;
    }

    auto input = std::make_shared<DispatchStatusPrepareInput>(
            buildDispatchStatusPrepareInput(cycle, tid));
    auto result = std::make_shared<DispatchStatusPrepareResult>();

    iewStats.dispatchStatusPrepareTasks++;
    const TaskOrderKey order{cycle, TaskStage::IEW, 2, tid, 0};
    runtime.submitWeak(
            order,
            3,
            [this, input, result] {
                *result = prepareDispatchStatusControl(*input);
            },
            [this, tid, result] {
                iewStats.dispatchStatusPrepareMerges++;
                verifyDispatchStatusPrepareResult(tid, *result);
            });
    runtime.waitForOrder(order);

    return *result;
}

void
IEW::verifyDispatchStatusPrepareResult(
        ThreadID tid, const DispatchStatusPrepareResult &result)
{
    DispatchStatusPrepareResult expected;
    expected.cycle = result.cycle;
    expected.tid = tid;
    expected.robHeadStallReason =
        checkDispatchStall(tid, NumDQ, nullptr, -1);
    expected.lqHeadStallReason =
        ldstQueue.lqEmpty() ? StallReason::NoStall :
                              checkLSQStall(tid, true);
    expected.sqHeadStallReason =
        ldstQueue.sqEmpty() ? StallReason::NoStall :
                              checkLSQStall(tid, false);

    if (result.tid != expected.tid ||
        result.robHeadStallReason != expected.robHeadStallReason ||
        result.lqHeadStallReason != expected.lqHeadStallReason ||
        result.sqHeadStallReason != expected.sqHeadStallReason) {
        iewStats.dispatchStatusPrepareMismatches++;
        panic("IEW dispatch status prepare mismatch for tid %i: "
              "prepared rob/lq/sq=%i/%i/%i expected=%i/%i/%i",
              tid, result.robHeadStallReason, result.lqHeadStallReason,
              result.sqHeadStallReason, expected.robHeadStallReason,
              expected.lqHeadStallReason, expected.sqHeadStallReason);
    }
}

IEW::DispatchDrainPreviewResult
IEW::previewDirectDispatchDrain(Cycles cycle, ThreadID tid,
                                const TimeStruct *commit_input)
{
    DispatchDrainPreviewResult result;
    result.cycle = cycle;
    result.tid = tid;

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return result;

    iewStats.dispatchDrainPreviewProbes++;

    if (enableDispatchStage) {
        iewStats.dispatchDrainPreviewSkipped++;
        iewStats.dispatchDrainPreviewSkipReasons[
            IEWDispatchDrainPreviewDispatchQueue]++;
        return result;
    }

    const auto &insts_to_dispatch = fixedbuffer[tid];
    result.visibleInsts = insts_to_dispatch.size();

    auto token_state = scheduler->buildDispatchTokenState();
    if (!token_state.supported) {
        result.blockReason =
            FutureActiveDispatchPreviewBlockReason::UnsupportedTokens;
        iewStats.dispatchDrainPreviewSkipped++;
        iewStats.dispatchDrainPreviewSkipReasons[
            IEWDispatchDrainPreviewNeedsSchedulerOrResource]++;
        return result;
    }

    unsigned free_lq_entries = ldstQueue.numFreeLoadEntries(tid);
    unsigned free_sq_entries = ldstQueue.numFreeStoreEntries(tid);
    bool serialize_next = serializeOnNextInst[tid];
    int disp_seq = -1;

    for (size_t i = 0; i < insts_to_dispatch.size(); ++i) {
        const auto &inst = insts_to_dispatch[i];
        disp_seq++;

        if (inst->isSquashed()) {
            result.output.fixedBufferPops++;
            result.output.squashedPops++;
            continue;
        }

        const bool skip_serialize =
            commit_input &&
            commit_input->commitInfo[tid].robheadSeqNum >= inst->seqNum;
        const bool serialize_before =
            inst->isSerializeBefore() || serialize_next;
        serialize_next = false;

        if (serialize_before && !skip_serialize) {
            result.drained = false;
            result.valid = true;
            result.stallReason = checkDispatchStall(tid, NumDQ, inst,
                                                    disp_seq);
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::SerializeBlocked;
            return result;
        }

        if (inst->isStoreConditional() || inst->isSerializeAfter())
            serialize_next = true;

        const bool needs_lq = inst->isLoad();
        const bool needs_sq = inst->isAtomic() || inst->isStore();

        if (needs_lq && free_lq_entries == 0) {
            result.drained = false;
            result.valid = true;
            result.stallReason = checkDispatchStall(tid, NumDQ, inst,
                                                    disp_seq);
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::LQFull;
            return result;
        }

        if (needs_sq && free_sq_entries == 0) {
            result.drained = false;
            result.valid = true;
            result.stallReason = checkDispatchStall(tid, NumDQ, inst,
                                                    disp_seq);
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::SQFull;
            return result;
        }

        if (!scheduler->dryRunDispatchReady(token_state, inst, disp_seq,
                                            false)) {
            const auto block_token =
                scheduler->dryRunDispatchBlockSnapshot(
                    token_state, inst->opClass(),
                    inst->staticInst->isSplitStoreAddr(), disp_seq);
            result.drained = false;
            result.valid = true;
            result.stallReason = checkDispatchStall(tid, NumDQ, inst,
                                                    disp_seq);
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::SchedulerNotReady;
            result.schedulerBlockReason =
                iewFutureDispatchSchedulerBlockReason(block_token.reason);
            result.schedulerBlockToken.valid = block_token.valid;
            result.schedulerBlockToken.reason =
                result.schedulerBlockReason;
            result.schedulerBlockToken.iqIndex = block_token.iqIndex;
            result.schedulerBlockToken.selector = block_token.selector;
            result.schedulerBlockToken.opClass = block_token.opClass;
            result.schedulerBlockToken.dispSeq = block_token.dispSeq;
            result.schedulerBlockToken.freeEntries =
                block_token.freeEntries;
            result.schedulerBlockToken.freeInports =
                block_token.freeInports;
            result.schedulerBlockToken.replayBlocked =
                block_token.replayBlocked;
            return result;
        }

        if (!inst->isNop() && !inst->isEliminated())
            result.output.producerAdds++;

        bool add_to_iq = false;
        if (inst->isAtomic()) {
            free_sq_entries--;
            result.output.sqInserts++;
            result.output.nonSpecInserts++;
        } else if (inst->isLoad()) {
            free_lq_entries--;
            result.output.lqInserts++;
            add_to_iq = true;
        } else if (inst->isStore()) {
            free_sq_entries--;
            result.output.sqInserts++;
            if (inst->isStoreConditional())
                result.output.nonSpecInserts++;
            add_to_iq = !inst->isStoreConditional();
        } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
            result.output.barrierInserts++;
            add_to_iq = false;
        } else if (inst->isNop() || inst->isEliminated()) {
            add_to_iq = false;
        } else {
            add_to_iq = true;
        }

        if (add_to_iq && inst->isNonSpeculative()) {
            result.output.nonSpecInserts++;
            add_to_iq = false;
        }

        if (add_to_iq) {
            result.output.iqInserts++;
            scheduler->dryRunDispatchReady(token_state, inst, disp_seq,
                                           true);
        }
        result.dispatchedBeforeBlock++;
        result.output.fixedBufferPops++;
    }

    result.drained = true;
    result.valid = true;
    return result;
}

IEW::DispatchDrainPreviewResult
IEW::previewFutureDirectDispatchDrain(
        const FutureDispatchPreviewInput &input) const
{
    DispatchDrainPreviewResult result;
    result.cycle = input.cycle;
    result.tid = input.tid;
    result.visibleInsts = input.entries;

    if (!input.valid || input.tid == InvalidThreadID)
        return result;

    std::vector<OpClass> op_classes;
    std::vector<OpClass> extra_sorted_ops;
    op_classes.reserve(input.entries);
    extra_sorted_ops.reserve(input.entries);
    for (unsigned i = 0; i < input.entries; ++i) {
        const auto &entry = input.insts[i];
        if (!entry.valid)
            continue;
        op_classes.push_back(entry.opClass);
        if (entry.splitStoreAddr)
            extra_sorted_ops.push_back(StoreDataOp);
    }

    auto token_state = scheduler->buildLookaheadDispatchTokenState(
            op_classes, extra_sorted_ops, true);
    if (!token_state.supported) {
        result.blockReason =
            FutureActiveDispatchPreviewBlockReason::UnsupportedTokens;
        return result;
    }

    unsigned free_lq_entries = input.freeLQEntries;
    unsigned free_sq_entries = input.freeSQEntries;
    bool serialize_next = input.serializeNext;
    int disp_seq = -1;

    for (unsigned i = 0; i < input.entries; ++i) {
        const auto &entry = input.insts[i];
        if (!entry.valid)
            continue;

        disp_seq++;
        if (entry.squashed) {
            result.output.fixedBufferPops++;
            result.output.squashedPops++;
            continue;
        }

        const bool skip_serialize = input.robHeadSeqNum >= entry.seqNum;
        const bool serialize_before =
            entry.serializeBefore || serialize_next;
        serialize_next = false;

        if (serialize_before && !skip_serialize) {
            result.drained = false;
            result.valid = true;
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::SerializeBlocked;
            return result;
        }

        if (entry.storeConditional || entry.serializeAfter)
            serialize_next = true;

        const bool needs_lq = entry.load;
        const bool needs_sq = entry.atomic || entry.store;

        if (needs_lq && free_lq_entries == 0) {
            result.drained = false;
            result.valid = true;
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::LQFull;
            return result;
        }

        if (needs_sq && free_sq_entries == 0) {
            result.drained = false;
            result.valid = true;
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::SQFull;
            return result;
        }

        if (!scheduler->dryRunDispatchReady(
                    token_state, entry.opClass, entry.splitStoreAddr,
                    disp_seq, false)) {
            const auto block_token =
                scheduler->dryRunDispatchBlockSnapshot(
                    token_state, entry.opClass, entry.splitStoreAddr,
                    disp_seq);
            result.drained = false;
            result.valid = true;
            result.blockReason =
                FutureActiveDispatchPreviewBlockReason::SchedulerNotReady;
            result.schedulerBlockReason =
                iewFutureDispatchSchedulerBlockReason(block_token.reason);
            result.schedulerBlockToken.valid = block_token.valid;
            result.schedulerBlockToken.reason =
                result.schedulerBlockReason;
            result.schedulerBlockToken.iqIndex = block_token.iqIndex;
            result.schedulerBlockToken.selector = block_token.selector;
            result.schedulerBlockToken.opClass = block_token.opClass;
            result.schedulerBlockToken.dispSeq = block_token.dispSeq;
            result.schedulerBlockToken.freeEntries =
                block_token.freeEntries;
            result.schedulerBlockToken.freeInports =
                block_token.freeInports;
            result.schedulerBlockToken.replayBlocked =
                block_token.replayBlocked;
            return result;
        }

        if (!entry.nop && !entry.eliminated)
            result.output.producerAdds++;

        bool add_to_iq = false;
        if (entry.atomic) {
            free_sq_entries--;
            result.output.sqInserts++;
            result.output.nonSpecInserts++;
        } else if (entry.load) {
            free_lq_entries--;
            result.output.lqInserts++;
            add_to_iq = true;
        } else if (entry.store) {
            free_sq_entries--;
            result.output.sqInserts++;
            if (entry.storeConditional)
                result.output.nonSpecInserts++;
            add_to_iq = !entry.storeConditional;
        } else if (entry.readBarrier || entry.writeBarrier) {
            result.output.barrierInserts++;
            add_to_iq = false;
        } else if (entry.nop || entry.eliminated) {
            add_to_iq = false;
        } else {
            add_to_iq = true;
        }

        if (add_to_iq && entry.nonSpeculative) {
            result.output.nonSpecInserts++;
            add_to_iq = false;
        }

        if (add_to_iq) {
            result.output.iqInserts++;
            scheduler->dryRunDispatchReady(
                    token_state, entry.opClass, entry.splitStoreAddr,
                    disp_seq, true);
        }
        result.dispatchedBeforeBlock++;
        result.output.fixedBufferPops++;
    }

    result.drained = true;
    result.valid = true;
    return result;
}

void
IEW::verifyDirectDispatchDrainPreview(
        const DispatchDrainPreviewResult &result, ThreadID tid)
{
    if (!result.valid)
        return;

    const bool actual_drained = fixedbuffer[tid].empty();
    if (result.drained == actual_drained) {
        iewStats.dispatchDrainPreviewMatches++;
        if (!result.drained) {
            if (result.dispatchedBeforeBlock > 0) {
                iewStats.dispatchDrainPreviewStallReasonSideEffectSkips++;
                if (cpu->getTaskRuntime().traceEnabled()) {
                    DPRINTF(IEW,
                            "Direct-dispatch stall reason preview skipped "
                            "cycle=%llu tid=%u predicted=%i actual=%i "
                            "visibleInsts=%u dispatchedBeforeBlock=%u "
                            "remaining=%llu\n",
                            static_cast<unsigned long long>(result.cycle),
                            tid, result.stallReason, blockReason,
                            result.visibleInsts, result.dispatchedBeforeBlock,
                            static_cast<unsigned long long>(
                                fixedbuffer[tid].size()));
                }
            } else if (result.stallReason == blockReason) {
                iewStats.dispatchDrainPreviewStallReasonMatches++;
            } else {
                iewStats.dispatchDrainPreviewStallReasonMismatches++;
                if (cpu->getTaskRuntime().traceEnabled()) {
                    DPRINTF(IEW,
                            "Direct-dispatch stall reason preview mismatch "
                            "cycle=%llu tid=%u predicted=%i actual=%i "
                            "visibleInsts=%u remaining=%llu\n",
                            static_cast<unsigned long long>(result.cycle),
                            tid, result.stallReason, blockReason,
                            result.visibleInsts,
                            static_cast<unsigned long long>(
                                fixedbuffer[tid].size()));
                }
            }
        }
    } else {
        iewStats.dispatchDrainPreviewMismatches++;
        if (cpu->getTaskRuntime().traceEnabled()) {
            DPRINTF(IEW,
                    "Direct-dispatch drain preview mismatch cycle=%llu "
                    "tid=%u predictedDrained=%i actualDrained=%i "
                    "visibleInsts=%u remaining=%llu\n",
                    static_cast<unsigned long long>(result.cycle), tid,
                    result.drained, actual_drained, result.visibleInsts,
                    static_cast<unsigned long long>(fixedbuffer[tid].size()));
        }
    }
}

bool
IEW::sameDispatchOutputSnapshot(
        const DispatchDrainPreviewResult::OutputSnapshot &lhs,
        const DispatchDrainPreviewResult::OutputSnapshot &rhs)
{
    return lhs.fixedBufferPops == rhs.fixedBufferPops &&
           lhs.squashedPops == rhs.squashedPops &&
           lhs.iqInserts == rhs.iqInserts &&
           lhs.lqInserts == rhs.lqInserts &&
           lhs.sqInserts == rhs.sqInserts &&
           lhs.nonSpecInserts == rhs.nonSpecInserts &&
           lhs.barrierInserts == rhs.barrierInserts &&
           lhs.producerAdds == rhs.producerAdds;
}

void
IEW::recordDispatchOutputSnapshotFieldDifferences(
        const DispatchDrainPreviewResult::OutputSnapshot &expected,
        const DispatchDrainPreviewResult::OutputSnapshot &actual,
        statistics::Vector &fields)
{
    if (expected.fixedBufferPops != actual.fixedBufferPops)
        fields[IEWDispatchOutputFixedBufferPops]++;
    if (expected.squashedPops != actual.squashedPops)
        fields[IEWDispatchOutputSquashedPops]++;
    if (expected.iqInserts != actual.iqInserts)
        fields[IEWDispatchOutputIQInserts]++;
    if (expected.lqInserts != actual.lqInserts)
        fields[IEWDispatchOutputLQInserts]++;
    if (expected.sqInserts != actual.sqInserts)
        fields[IEWDispatchOutputSQInserts]++;
    if (expected.nonSpecInserts != actual.nonSpecInserts)
        fields[IEWDispatchOutputNonSpecInserts]++;
    if (expected.barrierInserts != actual.barrierInserts)
        fields[IEWDispatchOutputBarrierInserts]++;
    if (expected.producerAdds != actual.producerAdds)
        fields[IEWDispatchOutputProducerAdds]++;
}

unsigned
IEW::futureDispatchOutputPublishabilityReason(
        const DispatchDrainPreviewResult &expected,
        const DispatchDrainPreviewResult *actual) const
{
    if (!actual || !actual->valid)
        return IEWFutureDispatchOutputActualMissing;

    if (!sameDispatchDrainPreview(expected, *actual))
        return IEWFutureDispatchOutputPreviewDifferent;

    if (!sameDispatchOutputSnapshot(expected.output, actual->output))
        return IEWFutureDispatchOutputOutputDifferent;

    if (expected.drained)
        return IEWFutureDispatchOutputStableDrained;

    return expected.output.fixedBufferPops == 0 ?
        IEWFutureDispatchOutputStableBlockedNoSideEffect :
        IEWFutureDispatchOutputStableBlockedSideEffect;
}

void
IEW::recordFutureDispatchOutputPublishability(
        const DispatchDrainPreviewResult &expected,
        const DispatchDrainPreviewResult *actual)
{
    const unsigned publishability =
        futureDispatchOutputPublishabilityReason(expected, actual);
    if (publishability >= NumIEWFutureDispatchOutputPublishabilityReasons)
        return;

    iewStats.futureDispatchOutputPublishability[publishability]++;

    const unsigned reason = static_cast<unsigned>(expected.blockReason);
    const bool valid_block_reason =
        reason < static_cast<unsigned>(
            FutureActiveDispatchPreviewBlockReason::NumReasons);
    const unsigned scheduler_reason =
        static_cast<unsigned>(expected.schedulerBlockReason);
    const bool valid_scheduler_reason =
        scheduler_reason < static_cast<unsigned>(
            FutureDispatchSchedulerBlockReason::NumReasons);

    if (publishability == IEWFutureDispatchOutputPreviewDifferent) {
        if (valid_block_reason) {
            iewStats.futureDispatchOutputPreviewDifferentReasons[reason]++;
        }
        if (expected.blockReason ==
            FutureActiveDispatchPreviewBlockReason::SchedulerNotReady &&
            valid_scheduler_reason) {
            iewStats.futureDispatchOutputPreviewDifferentSchedulerReasons[
                scheduler_reason]++;
        }
        iewStats.futureDispatchOutputPreviewDifferentPops.sample(
                expected.output.fixedBufferPops);
        return;
    }

    if (publishability != IEWFutureDispatchOutputStableBlockedNoSideEffect &&
        publishability != IEWFutureDispatchOutputStableBlockedSideEffect) {
        return;
    }

    if (valid_block_reason)
        iewStats.futureDispatchOutputStableBlockedReasons[reason]++;

    if (expected.blockReason ==
        FutureActiveDispatchPreviewBlockReason::SchedulerNotReady &&
        valid_scheduler_reason) {
        iewStats.futureDispatchOutputStableBlockedSchedulerReasons[
            scheduler_reason]++;
    }
    iewStats.futureDispatchOutputStableBlockedPops.sample(
            expected.output.fixedBufferPops);
}

bool
IEW::sameDispatchBlockTokenSnapshot(
        const DispatchDrainPreviewResult::BlockTokenSnapshot &lhs,
        const DispatchDrainPreviewResult::BlockTokenSnapshot &rhs)
{
    return lhs.valid == rhs.valid &&
           lhs.reason == rhs.reason &&
           lhs.iqIndex == rhs.iqIndex &&
           lhs.selector == rhs.selector &&
           lhs.opClass == rhs.opClass &&
           lhs.dispSeq == rhs.dispSeq &&
           lhs.freeEntries == rhs.freeEntries &&
           lhs.freeInports == rhs.freeInports &&
           lhs.replayBlocked == rhs.replayBlocked;
}

void
IEW::recordDispatchBlockTokenDifferenceFields(
        const DispatchDrainPreviewResult::BlockTokenSnapshot &expected,
        const DispatchDrainPreviewResult::BlockTokenSnapshot &actual)
{
    if (expected.valid != actual.valid)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenValid]++;
    if (expected.reason != actual.reason)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenReason]++;
    if (expected.iqIndex != actual.iqIndex)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenIQIndex]++;
    if (expected.selector != actual.selector)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenSelector]++;
    if (expected.opClass != actual.opClass)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenOpClass]++;
    if (expected.dispSeq != actual.dispSeq)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenDispSeq]++;
    if (expected.freeEntries != actual.freeEntries)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenFreeEntries]++;
    if (expected.freeInports != actual.freeInports)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenFreeInports]++;
    if (expected.replayBlocked != actual.replayBlocked)
        iewStats.futureDispatchBlockTokenDifferenceFields[
            IEWFutureDispatchBlockTokenReplayBlocked]++;
}

void
IEW::recordFutureDispatchBlockTokenCheck(
        const DispatchDrainPreviewResult &expected,
        const DispatchDrainPreviewResult *actual,
        unsigned dispatch_publishability_reason)
{
    DispatchDrainPreviewResult::BlockTokenSnapshot missing_actual;
    const auto &actual_token =
        actual ? actual->schedulerBlockToken : missing_actual;

    if (!expected.schedulerBlockToken.valid && !actual_token.valid)
        return;

    iewStats.futureDispatchBlockTokenChecks++;
    if (sameDispatchBlockTokenSnapshot(
                expected.schedulerBlockToken, actual_token)) {
        iewStats.futureDispatchBlockTokenMatches++;
        if (dispatch_publishability_reason <
            NumIEWFutureDispatchOutputPublishabilityReasons) {
            iewStats.futureDispatchBlockTokenMatchesByPublishability[
                dispatch_publishability_reason]++;
        }
    } else {
        iewStats.futureDispatchBlockTokenDifferences++;
        if (dispatch_publishability_reason <
            NumIEWFutureDispatchOutputPublishabilityReasons) {
            iewStats.futureDispatchBlockTokenDifferencesByPublishability[
                dispatch_publishability_reason]++;
        }
        recordDispatchBlockTokenDifferenceFields(
                expected.schedulerBlockToken, actual_token);
    }
}

void
IEW::verifyDirectDispatchOutputSnapshot(
        const DispatchDrainPreviewResult &result,
        const DispatchDrainPreviewResult::OutputSnapshot &actual,
        ThreadID tid)
{
    if (!result.valid)
        return;

    iewStats.dispatchOutputSnapshotChecks++;
    if (sameDispatchOutputSnapshot(result.output, actual)) {
        iewStats.dispatchOutputSnapshotMatches++;
    } else {
        iewStats.dispatchOutputSnapshotMismatches++;
        recordDispatchOutputSnapshotFieldDifferences(
                result.output, actual,
                iewStats.dispatchOutputSnapshotMismatchFields);
        if (cpu->getTaskRuntime().traceEnabled()) {
            DPRINTF(IEW,
                    "Direct-dispatch output snapshot mismatch cycle=%llu "
                    "tid=%u expected(pop=%u squashed=%u iq=%u lq=%u "
                    "sq=%u nonSpec=%u barrier=%u prod=%u) actual(pop=%u "
                    "squashed=%u iq=%u lq=%u sq=%u nonSpec=%u barrier=%u "
                    "prod=%u)\n",
                    static_cast<unsigned long long>(result.cycle), tid,
                    result.output.fixedBufferPops,
                    result.output.squashedPops,
                    result.output.iqInserts,
                    result.output.lqInserts,
                    result.output.sqInserts,
                    result.output.nonSpecInserts,
                    result.output.barrierInserts,
                    result.output.producerAdds,
                    actual.fixedBufferPops,
                    actual.squashedPops,
                    actual.iqInserts,
                    actual.lqInserts,
                    actual.sqInserts,
                    actual.nonSpecInserts,
                    actual.barrierInserts,
                    actual.producerAdds);
        }
    }
}

void
IEW::setPendingFutureDispatchPreview(
        const DispatchDrainPreviewResult &result)
{
    if (!result.valid)
        return;

    pendingFutureDispatchPreview.result = result;
    pendingFutureDispatchPreview.valid = true;
}

void
IEW::setPendingFutureRenameLatchPreview(
        Cycles cycle, const StallSignalLatch &latch)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    if (pendingFutureRenameLatchPreview.valid &&
        pendingFutureRenameLatchPreview.cycle != cycle) {
        iewStats.futureRenameLatchPreviewStale++;
    }

    pendingFutureRenameLatchPreview.cycle = cycle;
    pendingFutureRenameLatchPreview.latch = latch;
    pendingFutureRenameLatchPreview.valid = true;
}

bool
IEW::sameRenameLatchPreview(const StallSignalLatch &lhs,
                            const StallSignalLatch &rhs) const
{
    for (int tid = 0; tid < numThreads; ++tid) {
        if (lhs.block[tid] != rhs.block[tid] ||
            lhs.reason[tid] != rhs.reason[tid]) {
            return false;
        }
    }
    return true;
}

void
IEW::recordRenameLatchPreviewDifferences(
        const StallSignalLatch &expected,
        const StallSignalLatch &actual)
{
    bool block_diff = false;
    bool reason_diff = false;
    for (int tid = 0; tid < numThreads; ++tid) {
        block_diff = block_diff || expected.block[tid] != actual.block[tid];
        reason_diff = reason_diff ||
                      expected.reason[tid] != actual.reason[tid];
    }
    if (block_diff) {
        iewStats.futureRenameLatchPreviewDifferenceReasons[
            IEWFutureRenameLatchPreviewBlock]++;
    }
    if (reason_diff) {
        iewStats.futureRenameLatchPreviewDifferenceReasons[
            IEWFutureRenameLatchPreviewReason]++;
    }
}

void
IEW::verifyPendingFutureRenameLatchPreview(
        unsigned dispatch_publishability_reason)
{
    if (!pendingFutureRenameLatchPreview.valid)
        return;

    const Cycles cycle = cpu->curCycle();
    if (pendingFutureRenameLatchPreview.cycle != cycle) {
        iewStats.futureRenameLatchPreviewStale++;
        pendingFutureRenameLatchPreview.valid = false;
        return;
    }

    StallSignalLatch legacy_actual;
    const StallSignalLatch *actual = nullptr;
    if (stallSignalBank) {
        actual = &cpu->stallSignalSnapshotOrCurrent(
                cycle, StallSignalEdge::IEWToRename);
    } else {
        legacy_actual.clear();
        for (int tid = 0; tid < numThreads; ++tid) {
            legacy_actual.block[tid] = stallSig->blockRename[tid];
            legacy_actual.reason[tid] = stallSig->renameBlockReason[tid];
        }
        actual = &legacy_actual;
    }

    iewStats.futureRenameLatchPreviewChecks++;
    if (sameRenameLatchPreview(
                pendingFutureRenameLatchPreview.latch, *actual)) {
        iewStats.futureRenameLatchPreviewMatches++;
        if (dispatch_publishability_reason <
            NumIEWFutureDispatchOutputPublishabilityReasons) {
            iewStats.futureRenameLatchPreviewMatchesByPublishability[
                dispatch_publishability_reason]++;
        }
    } else {
        iewStats.futureRenameLatchPreviewDifferences++;
        if (dispatch_publishability_reason <
            NumIEWFutureDispatchOutputPublishabilityReasons) {
            iewStats.futureRenameLatchPreviewDifferencesByPublishability[
                dispatch_publishability_reason]++;
        }
        recordRenameLatchPreviewDifferences(
                pendingFutureRenameLatchPreview.latch, *actual);
        if (cpu->getTaskRuntime().traceEnabled()) {
            DPRINTF(IEW,
                    "Future IEW-to-Rename latch preview difference "
                    "cycle=%llu\n",
                    static_cast<unsigned long long>(cycle));
        }
    }
    pendingFutureRenameLatchPreview.valid = false;
}

bool
IEW::sameDispatchDrainPreview(const DispatchDrainPreviewResult &lhs,
                              const DispatchDrainPreviewResult &rhs) const
{
    return futureDispatchPreviewDifferenceReason(lhs, &rhs) ==
           NumIEWFutureDispatchPreviewDifferenceReasons;
}

unsigned
IEW::futureDispatchPreviewDifferenceReason(
        const DispatchDrainPreviewResult &expected,
        const DispatchDrainPreviewResult *actual) const
{
    if (!actual)
        return IEWFutureDispatchPreviewActualMissing;
    if (expected.valid != actual->valid ||
        expected.cycle != actual->cycle) {
        return IEWFutureDispatchPreviewValid;
    }
    if (expected.tid != actual->tid)
        return IEWFutureDispatchPreviewTid;
    if (expected.visibleInsts != actual->visibleInsts)
        return IEWFutureDispatchPreviewVisibleInsts;
    if (expected.dispatchedBeforeBlock != actual->dispatchedBeforeBlock)
        return IEWFutureDispatchPreviewDispatchedBeforeBlock;
    if (expected.drained != actual->drained)
        return IEWFutureDispatchPreviewDrained;
    if (expected.blockReason != actual->blockReason)
        return IEWFutureDispatchPreviewBlockReason;
    if (expected.schedulerBlockReason != actual->schedulerBlockReason)
        return IEWFutureDispatchPreviewSchedulerBlockReason;

    return NumIEWFutureDispatchPreviewDifferenceReasons;
}

void
IEW::verifyPendingFutureDispatchPreview(
        const DispatchDrainPreviewResult *actual)
{
    if (!pendingFutureDispatchPreview.valid)
        return;

    const Cycles cycle = cpu->curCycle();
    if (pendingFutureDispatchPreview.result.cycle != cycle) {
        iewStats.futureDispatchPreviewStale++;
        pendingFutureDispatchPreview.valid = false;
        return;
    }

    iewStats.futureDispatchPreviewChecks++;
    const unsigned publishability =
        futureDispatchOutputPublishabilityReason(
                pendingFutureDispatchPreview.result, actual);
    recordFutureDispatchOutputPublishability(
            pendingFutureDispatchPreview.result, actual);
    recordFutureDispatchBlockTokenCheck(
            pendingFutureDispatchPreview.result, actual, publishability);
    if (actual && actual->valid) {
        iewStats.futureDispatchOutputSnapshotChecks++;
        if (sameDispatchOutputSnapshot(
                    pendingFutureDispatchPreview.result.output,
                    actual->output)) {
            iewStats.futureDispatchOutputSnapshotMatches++;
        } else {
            iewStats.futureDispatchOutputSnapshotDifferences++;
            recordDispatchOutputSnapshotFieldDifferences(
                    pendingFutureDispatchPreview.result.output,
                    actual->output,
                    iewStats.futureDispatchOutputSnapshotDifferenceFields);
        }
    }
    if (actual && actual->valid &&
        sameDispatchDrainPreview(
                pendingFutureDispatchPreview.result, *actual)) {
        iewStats.futureDispatchPreviewMatches++;
    } else {
        iewStats.futureDispatchPreviewDifferences++;
        const unsigned reason =
            futureDispatchPreviewDifferenceReason(
                    pendingFutureDispatchPreview.result, actual);
        if (reason < NumIEWFutureDispatchPreviewDifferenceReasons)
            iewStats.futureDispatchPreviewDifferenceReasons[reason]++;
        const auto &expected = pendingFutureDispatchPreview.result;
        if (actual && actual->valid && expected.valid &&
            expected.drained != actual->drained) {
            if (!expected.drained && actual->drained) {
                iewStats.futureDispatchPreviewDrainedDiffDirections[
                    IEWFutureDispatchPreviewFutureBlockedActualDrained]++;
            } else {
                iewStats.futureDispatchPreviewDrainedDiffDirections[
                    IEWFutureDispatchPreviewFutureDrainedActualBlocked]++;
            }
        }
        if (reason == IEWFutureDispatchPreviewDispatchedBeforeBlock &&
            actual) {
            const auto expected_count =
                expected.dispatchedBeforeBlock;
            const auto actual_count = actual->dispatchedBeforeBlock;
            const unsigned delta = expected_count > actual_count ?
                expected_count - actual_count : actual_count - expected_count;
            if (expected_count < actual_count) {
                iewStats
                    .futureDispatchPreviewDispatchedBeforeBlockDiffDirections[
                        IEWFutureDispatchPreviewFutureLess]++;
            } else {
                iewStats
                    .futureDispatchPreviewDispatchedBeforeBlockDiffDirections[
                        IEWFutureDispatchPreviewFutureGreater]++;
            }
            iewStats.futureDispatchPreviewDispatchedBeforeBlockDelta.sample(
                    delta);
        }
        if (cpu->getTaskRuntime().traceEnabled()) {
            if (actual) {
                DPRINTF(IEW,
                        "Future dispatch preview difference cycle=%llu "
                        "expected(tid=%i valid=%i drained=%i block=%u "
                        "sched=%u insts=%u before=%u) actual(tid=%i "
                        "valid=%i drained=%i block=%u sched=%u insts=%u "
                        "before=%u)\n",
                        static_cast<unsigned long long>(cycle),
                        expected.tid, expected.valid, expected.drained,
                        static_cast<unsigned>(expected.blockReason),
                        static_cast<unsigned>(
                            expected.schedulerBlockReason),
                        expected.visibleInsts,
                        expected.dispatchedBeforeBlock,
                        actual->tid, actual->valid, actual->drained,
                        static_cast<unsigned>(actual->blockReason),
                        static_cast<unsigned>(
                            actual->schedulerBlockReason),
                        actual->visibleInsts,
                        actual->dispatchedBeforeBlock);
            } else {
                DPRINTF(IEW,
                        "Future dispatch preview difference cycle=%llu "
                        "expected active tid=%i but actual had no dispatch\n",
                        static_cast<unsigned long long>(cycle),
                        expected.tid);
            }
        }
    }
    pendingFutureDispatchPreview.valid = false;
}

IEW::WritebackPrepareInput
IEW::buildWritebackPrepareInput(Cycles cycle) const
{
    WritebackPrepareInput input;
    input.cycle = cycle;
    input.width = wbWidth;

    for (unsigned inst_num = 0; inst_num < wbWidth; ++inst_num) {
        const DynInstPtr &inst = toCommit->insts[inst_num];
        if (!inst)
            break;

        auto &entry = input.entries[inst_num];
        entry.valid = true;
        entry.tid = inst->threadNumber;
        entry.loadWithSavedRequest = inst->savedRequest && inst->isLoad();
        entry.wakeEligible = !inst->isSquashed() && inst->isExecuted() &&
                             inst->getFault() == NoFault;
        input.validInsts++;
    }

    return input;
}

IEW::WritebackPrepareResult
IEW::prepareWritebackControl(const WritebackPrepareInput &input) const
{
    WritebackPrepareResult result;
    result.cycle = input.cycle;

    for (unsigned inst_num = 0; inst_num < input.validInsts; ++inst_num) {
        const auto &entry = input.entries[inst_num];
        fatal_if(!entry.valid,
                 "IEW writeback prepare expected a valid entry at slot %u.",
                 inst_num);
        fatal_if(entry.tid == InvalidThreadID || entry.tid >= MaxThreads,
                 "IEW writeback prepare saw invalid thread id %i.",
                 entry.tid);
        result.tid[inst_num] = entry.tid;
        result.loadWithSavedRequest[inst_num] =
            entry.loadWithSavedRequest;
        result.wakeEligible[inst_num] = entry.wakeEligible;
        result.instsToCommit[entry.tid]++;
        result.validInsts++;
    }

    return result;
}

IEW::WritebackPrepareResult
IEW::runWritebackPrepare(Cycles cycle)
{
    WritebackPrepareInput input = buildWritebackPrepareInput(cycle);

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return prepareWritebackControl(input);

    if (input.validInsts == 0) {
        iewStats.writebackPrepareNoWork++;
        return prepareWritebackControl(input);
    }

    iewStats.writebackPrepareTasks++;
    const TaskOrderKey order{cycle, TaskStage::IEW, 4, InvalidThreadID, 0};
    auto input_ptr = std::make_shared<WritebackPrepareInput>(input);
    auto result = std::make_shared<WritebackPrepareResult>();
    runtime.submitWeak(
            order,
            std::max(1u, input_ptr->validInsts),
            [this, input_ptr, result] {
                *result = prepareWritebackControl(*input_ptr);
            },
            [this, result] {
                iewStats.writebackPrepareMerges++;
                verifyWritebackPrepareResult(*result);
            });
    runtime.waitForOrder(order);

    return *result;
}

void
IEW::verifyWritebackPrepareResult(
        const WritebackPrepareResult &result)
{
    const WritebackPrepareResult expected =
        prepareWritebackControl(buildWritebackPrepareInput(result.cycle));

    auto mismatch = [&] {
        if (result.validInsts != expected.validInsts)
            return true;
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            if (result.instsToCommit[tid] != expected.instsToCommit[tid])
                return true;
        }
        for (unsigned inst_num = 0; inst_num < expected.validInsts;
             ++inst_num) {
            if (result.tid[inst_num] != expected.tid[inst_num] ||
                result.loadWithSavedRequest[inst_num] !=
                    expected.loadWithSavedRequest[inst_num] ||
                result.wakeEligible[inst_num] !=
                    expected.wakeEligible[inst_num]) {
                return true;
            }
        }
        return false;
    };

    if (mismatch()) {
        iewStats.writebackPrepareMismatches++;
        panic("IEW writeback prepare mismatch: prepared valid=%u expected=%u",
              result.validInsts, expected.validInsts);
    }
}

const RenameStruct *
IEW::renameInput(Cycles cycle) const
{
    const int rename_to_iew_offset = -static_cast<int>(
            static_cast<uint64_t>(renameToIEWDelay));
    const RenameStruct *snapshot =
        cpu->pipelineInputRenameToIEW(cycle, rename_to_iew_offset);
    return snapshot ? snapshot : &(*fromRename);
}

const TimeStruct *
IEW::commitInput(Cycles cycle) const
{
    const int commit_to_iew_offset = -static_cast<int>(
            static_cast<uint64_t>(commitToIEWDelay));
    const TimeStruct *snapshot =
        cpu->pipelineInputBackward(cycle, commit_to_iew_offset);
    return snapshot ? snapshot : &(*fromCommit);
}

void
IEW::squash(ThreadID tid, const TimeStruct *commit_input)
{
    DPRINTF(IEW, "[tid:%i] Squashing all instructions.\n", tid);

    for (auto& dp : dispQue) {
        for (auto& it : dp) {
            if (it->seqNum > commit_input->commitInfo[tid].doneSeqNum) {
                it->setSquashed();
            }
        }
    }

    // Tell the IQ to start squashing.
    instQueue.squash(tid);

    // Tell the LDSTQ to start squashing.
    ldstQueue.squash(commit_input->commitInfo[tid].doneSeqNum, tid);
    updatedQueues = true;

    fixedbuffer[tid].clear();

    setRenameBlock(tid, true);

    // Clear the skid buffer in case it has any data in it.
    DPRINTF(IEW,
            "Removing skidbuffer instructions until "
            "[sn:%llu] [tid:%i]\n",
            commit_input->commitInfo[tid].doneSeqNum, tid);
}

void
IEW::squashDueToBranch(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] [sn:%llu] Squashing from a specific instruction,"
            " PC: %s "
            "\n", tid, inst->seqNum, inst->pcState() );

    if (!toCommit->squash[tid] || inst->seqNum < toCommit->squashedSeqNum[tid]) {
        toCommit->squash[tid] = true;
        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->squashedTargetId[tid] = inst->getFtqId();
        toCommit->squashedLoopIter[tid] = inst->getLoopIteration();
        toCommit->branchTaken[tid] = inst->pcState().branching();

        set(toCommit->pc[tid], inst->pcState());
        inst->staticInst->advancePC(*toCommit->pc[tid]);

        toCommit->mispredictInst[tid] = inst;
        toCommit->includeSquashInst[tid] = false;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "Branch misprediction (pc=%#lx) set target "
                "id to %lu, loop iter to %u\n",
                toCommit->pc[tid]->instAddr(),
                toCommit->squashedTargetId[tid],
                toCommit->squashedLoopIter[tid]);
    }

    setRenameBlock(tid, true);
}

void
IEW::squashDueToMemOrder(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Memory violation, squashing violator and younger "
            "insts, PC: %s [sn:%llu].\n", tid, inst->pcState(), inst->seqNum);
    // Need to include inst->seqNum in the following comparison to cover the
    // corner case when a branch misprediction and a memory violation for the
    // same instruction (e.g. load PC) are detected in the same cycle.  In this
    // case the memory violator should take precedence over the branch
    // misprediction because it requires the violator itself to be included in
    // the squash.
    if (!toCommit->squash[tid] || inst->seqNum <= toCommit->squashedSeqNum[tid]) {
        toCommit->squash[tid] = true;

        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->squashedTargetId[tid] = inst->getFtqId();
        toCommit->squashedLoopIter[tid] = inst->getLoopIteration();
        set(toCommit->pc[tid], inst->pcState());
        toCommit->mispredictInst[tid] = NULL;

        // Must include the memory violator in the squash.
        toCommit->includeSquashInst[tid] = true;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "Memory violation (pc=%#lx) set target id "
                "to %lu, loop iter to %u\n",
                toCommit->pc[tid]->instAddr(),
                toCommit->squashedTargetId[tid],
                toCommit->squashedLoopIter[tid]);
    }

    setRenameBlock(tid, true);
}

void
IEW::squashDueToValuePrediction(const DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] value prediction error, squashing violator and younger "
            "insts, PC: %s [sn:%llu].\n",
            tid, inst->pcState(), inst->seqNum);
    if (!toCommit->squash[tid] || inst->seqNum < toCommit->squashedSeqNum[tid]) {
        toCommit->squash[tid] = true;

        toCommit->valuePredictionError[tid] = true;
        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->squashedTargetId[tid] = inst->getFtqId();
        toCommit->squashedLoopIter[tid] = inst->getLoopIteration();
        set(toCommit->pc[tid], inst->pcState());

        // advance pc to next instruction
        inst->staticInst->advancePC(*toCommit->pc[tid]);

        toCommit->mispredictInst[tid] = NULL;

        // Even speculatively executed value prediction instructions cannot
        // be squashed after obtaining a correct result.
        toCommit->includeSquashInst[tid] = false;

        wroteToTimeBuffer = true;

        DPRINTF(DecoupleBP,
                "value prediction error (pc=%#lx) set target id "
                "to %lu, loop iter to %u\n",
                toCommit->pc[tid]->instAddr(),
                toCommit->squashedTargetId[tid],
                toCommit->squashedLoopIter[tid]);
    }

    setRenameBlock(tid, true);
}

void
IEW::wakeDependents(const DynInstPtr& inst)
{
    instQueue.wakeDependents(inst);
}

void
IEW::rescheduleMemInst(const DynInstPtr& inst)
{
    instQueue.rescheduleMemInst(inst);
}

void
IEW::replayMemInst(const DynInstPtr& inst)
{
    instQueue.replayMemInst(inst);
}

void
IEW::blockMemInst(const DynInstPtr& inst)
{
    instQueue.blockMemInst(inst);
}

void
IEW::cacheMissLdReplay(const DynInstPtr& inst)
{
    instQueue.cacheMissLdReplay(inst);
}

void
IEW::cacheUnblocked()
{
    instQueue.cacheUnblocked();
}

void
IEW::readyToFinish(const DynInstPtr& inst)
{
    // This function should not be called after writebackInsts in a
    // single cycle.  That will cause problems with an instruction
    // being added to the queue to commit without being processed by
    // writebackInsts prior to being sent to commit.

    // First check the time slot that this instruction will write
    // to.  If there are free write ports at the time, then go ahead
    // and write the instruction to that time.  If there are not,
    // keep looking back to see where's the first time there's a
    // free slot.
    while ((*iewQueue)[wbCycle].insts[wbNumInst]) {
        ++wbNumInst;
        if (wbNumInst == wbWidth) {
            ++wbCycle;
            wbNumInst = 0;
        }
    }

#if TRACING_ON
    if (debug::O3PipeView) {
        inst->completeTick = curTick() - inst->fetchTick;
    }
#endif

    scheduler->bypassWriteback(inst);
    inst->completionTick = curTick();

    ThreadID tid = inst->threadNumber;
    if (inst->vpMisprediction) {
        if (!enableSelectiveVPFlush) {
            if (!fetchRedirect[tid] || !toCommit->squash[tid] ||
                toCommit->squashedSeqNum[tid] > inst->seqNum) {
                fetchRedirect[tid] = true;
                squashDueToValuePrediction(inst, tid);
            }
        } else {
            // VP selective flush: cancel data-dependent consumers when possible.
            // If any dependent consumer is already issued, fallback to squash.
            DPRINTF(IEW, "[sn:%llu] VP misprediction detected, "
                    "selective cancel via loadCancel\n", inst->seqNum);
            bool needSquashFallback = scheduler->loadCancel(inst);
            if (needSquashFallback) {
                DPRINTF(IEW, "[sn:%llu] VP fallback to squash due to issued dependent\n",
                        inst->seqNum);
                if (!fetchRedirect[tid] || !toCommit->squash[tid] ||
                    toCommit->squashedSeqNum[tid] > inst->seqNum) {
                    fetchRedirect[tid] = true;
                    squashDueToValuePrediction(inst, tid);
                }
            } else {
                // Writeback with real value to re-wake consumers
                scheduler->writebackWakeup(inst);
            }
        }
    }

    DPRINTF(IEW, "Current wb cycle: %i, width: %i, numInst: %i\nwbActual:%i\n",
            wbCycle, wbWidth, wbNumInst, wbCycle * wbWidth + wbNumInst);
    // Add finished instruction to queue to commit.
    (*iewQueue)[wbCycle].insts[wbNumInst] = inst;
    (*iewQueue)[wbCycle].size++;
}

void
IEW::updateActivate()
{
    bool any_unblocking = false;

    // If there are no ready instructions waiting to be scheduled by the IQ,
    // and there's no stores waiting to write back, and dispatch is not
    // unblocking, then there is no internal activity for the IEW stage.
    instQueue.iqIOStats.intInstQueueReads++;
    if (_status == Active && !instQueue.hasReadyInsts() &&
        !ldstQueue.willWB() && !any_unblocking) {
        DPRINTF(IEW, "IEW switching to idle\n");

        deactivateStage();

        _status = Inactive;
    } else if (_status == Inactive && (instQueue.hasReadyInsts() ||
                                       ldstQueue.willWB() ||
                                       any_unblocking)) {
        // Otherwise there is internal activity.  Set to active.
        DPRINTF(IEW, "IEW switching to active\n");

        activateStage();

        _status = Active;
    }
}

bool
IEW::checkSerialize(const DynInstPtr& inst,
                    const TimeStruct *commit_input)
{
    ThreadID tid = inst->threadNumber;
    bool skipserialize =
        commit_input->commitInfo[tid].robheadSeqNum >= inst->seqNum;

    if (serializeOnNextInst[tid]) {
        inst->setSerializeBefore();
        serializeOnNextInst[tid] = false;
    }

    if (inst->isSerializeBefore() && !skipserialize) {
        return true;
    } else if (inst->isStoreConditional() || inst->isSerializeAfter()) {
        serializeOnNextInst[tid] = true;
        return false;
    }

    return false;
}

void
IEW::checkSquash(const TimeStruct *commit_input)
{
    // Check if there's a squash signal, squash if there is
    // Check stall signals, block if there is.
    // If status was Blocked
    //     if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    for (int i = 0; i < numThreads; i++) {
        if (commit_input->commitInfo[i].squash) {
            squash(i, commit_input);
            localSquashVer.update(
                    commit_input->commitInfo[i].squashVersion.getVersion());
            DPRINTF(IEW, "Updating squash version to %u\n", localSquashVer.getVersion());

            fetchRedirect[i] = false;
            iewStats.stallEvents[ROBWalk]++;
            setAllStalls(StallReason::CommitSquash);
            return;
        }

        if (commit_input->commitInfo[i].robSquashing) {
            DPRINTF(IEW, "[tid:%i] ROB is still squashing.\n", i);

            wroteToTimeBuffer = true;
            iewStats.stallEvents[ROBWalk]++;
            setAllStalls(StallReason::CommitSquash);
        }
    }
}

void
IEW::moveInstsToBuffer(const RenameStruct *rename_input)
{
    int insts_from_rename = rename_input->size;
    if (insts_from_rename == 0) {
        DPRINTF(IEW, "No instructions from rename to move to buffer.\n");
        return;
    }
    ThreadID tid = rename_input->insts[0]->threadNumber;
    assert(fixedbuffer[tid].empty());
    for (int i = 0; i < insts_from_rename; ++i) {
        const DynInstPtr &inst = rename_input->insts[i];
        assert(inst->threadNumber == tid);
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();
        } else {
            fixedbuffer[tid].push_back(inst);
        }
    }
}

void
IEW::wakeCPU()
{
    cpu->wakeCPU();
}

void
IEW::activityThisCycle()
{
    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();
}

void
IEW::activateStage()
{
    DPRINTF(Activity, "Activating stage.\n");
    cpu->activateStage(CPU::IEWIdx);
}

void
IEW::deactivateStage()
{
    DPRINTF(Activity, "Deactivating stage.\n");
    cpu->deactivateStage(CPU::IEWIdx);
}

bool
IEW::canInsertLDSTQue(ThreadID tid, bool reset_lsq_pop_entries)
{
    int freeLQEntries = ldstQueue.getFreeLQEntries(tid);
    int freeSQEntries = ldstQueue.getFreeSQEntries(tid);

    int lastClockLQPopEntries = reset_lsq_pop_entries ?
        ldstQueue.getAndResetLastLQPopEntries(tid) :
        ldstQueue.peekLastLQPopEntries(tid);
    int lastClockSQPopEntries = reset_lsq_pop_entries ?
        ldstQueue.getAndResetLastSQPopEntries(tid) :
        ldstQueue.peekLastSQPopEntries(tid);
    if (freeLQEntries >= renameWidth + lastClockLQPopEntries &&
        freeSQEntries >= renameWidth + lastClockSQPopEntries) {
        return true;
    }
    return false;
}

void
IEW::dispatchInsts(const RenameStruct *rename_input,
                   const TimeStruct *commit_input)
{
    if (enableDispatchStage) {
        dispatchInstFromDispQue();
    }

    // check threads stall & status
    const IEWPrepareResult prepare = runIEWPrepare(cpu->curCycle());
    ThreadID tid = prepare.selectedTid;
    for (int i = 0; i < numThreads; i++) {
        setRenameStall(i, prepare.renameBlock[i],
                       prepare.renameBlockReason[i]);
    }
    if (prepare.multipleActive) {
        DPRINTF(IEW,
                "Multiple active threads detected, blocking all threads\n");
    }

    if (tid != InvalidThreadID) {
        DPRINTF(IEW,"Processing [tid:%i]\n",tid);

        DispatchDrainPreviewResult dispatch_drain_preview;

        // dispatch to IQ
        if (enableDispatchStage) {
            dispatch_drain_preview =
                previewDirectDispatchDrain(cpu->curCycle(), tid, commit_input);
            classifyInstToDispQue(tid, rename_input, commit_input);
        } else {
            dispatch_drain_preview =
                dispatchInstFromRename(tid, rename_input, commit_input);
        }
        // check stall again
        if (!fixedbuffer[tid].empty()) {
            setRenameBlock(tid, true);
            DPRINTF(IEW, "Dispatch bandwidth full, blocking thread %i\n", tid);
        }
        const unsigned dispatch_publishability =
            pendingFutureDispatchPreview.valid &&
            pendingFutureDispatchPreview.result.cycle == cpu->curCycle() ?
            futureDispatchOutputPublishabilityReason(
                    pendingFutureDispatchPreview.result,
                    &dispatch_drain_preview) :
            NumIEWFutureDispatchOutputPublishabilityReasons;
        verifyPendingFutureDispatchPreview(&dispatch_drain_preview);
        verifyPendingFutureRenameLatchPreview(dispatch_publishability);
        verifyDirectDispatchDrainPreview(dispatch_drain_preview, tid);

        const DispatchStatusPrepareResult dispatch_status =
            runDispatchStatusPrepare(cpu->curCycle(), tid);
        toRename->iewInfo[tid].robHeadStallReason =
            dispatch_status.robHeadStallReason;
        toRename->iewInfo[tid].lqHeadStallReason =
            dispatch_status.lqHeadStallReason;
        toRename->iewInfo[tid].sqHeadStallReason =
            dispatch_status.sqHeadStallReason;
        toRename->iewInfo[tid].blockReason = blockReason;
    } else {
        const unsigned dispatch_publishability =
            pendingFutureDispatchPreview.valid &&
            pendingFutureDispatchPreview.result.cycle == cpu->curCycle() ?
            futureDispatchOutputPublishabilityReason(
                    pendingFutureDispatchPreview.result, nullptr) :
            NumIEWFutureDispatchOutputPublishabilityReasons;
        verifyPendingFutureDispatchPreview(nullptr);
        verifyPendingFutureRenameLatchPreview(dispatch_publishability);
    }
}

IEW::DispatchDrainPreviewResult
IEW::dispatchInstFromRename(ThreadID tid,
                            const RenameStruct *rename_input,
                            const TimeStruct *commit_input)
{
    DynInstPtr inst;

    auto &insts_to_dispatch = fixedbuffer[tid];

    bool emptyROB = commit_input->commitInfo[tid].emptyROB;

    int insts_to_add = insts_to_dispatch.size();
    std::queue<StallReason> dispatch_stalls;
    StallReason breakDispatch = StallReason::NoStall;

    unsigned dispatched = 0;
    int disp_seq = -1;

    scheduler->lookahead(insts_to_dispatch);
    const DispatchDrainPreviewResult dispatch_drain_preview =
        previewDirectDispatchDrain(cpu->curCycle(), tid, commit_input);
    DispatchDrainPreviewResult::OutputSnapshot actual_dispatch_output;
    while (!insts_to_dispatch.empty()) {
        bool add_to_iq = false;
        auto &inst = insts_to_dispatch.front();
        disp_seq++;
        int ins = cpu->cpuStats.committedInsts.total();
        if (cpu->hasHintDownStream() && ins % 10000 == 1) {
            cpu->hintDownStream->notifyIns(ins);
        }

        if (inst->isSquashed()) {
            ++iewStats.dispSquashedInsts;
            actual_dispatch_output.fixedBufferPops++;
            actual_dispatch_output.squashedPops++;
            insts_to_dispatch.pop_front();

            dispatch_stalls.push(StallReason::InstSquashed);
            continue;
        }

        if (checkSerialize(inst, commit_input)) {
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Dispatch: Serialize instruction encountered.\n", tid, inst->seqNum);
            dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }

        // Check LSQ if inst is LD/ST
        if ((inst->isAtomic() && ldstQueue.sqFull(tid)) || (inst->isLoad() && ldstQueue.lqFull(tid)) ||
            (inst->isStore() && ldstQueue.sqFull(tid))) {
            DPRINTF(IEW, "[tid:%i] Dispatch: %s has become full.\n", tid, inst->isLoad() ? "LQ" : "SQ");

            iewStats.stallEvents[LSQFull]++;

            ++iewStats.lsqFullEvents;
            dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }

        if (!scheduler->ready(inst, disp_seq)) {
            DPRINTF(IEW, "[tid:%i] Dispatch: IQ is full or bwFull.\n", tid);
            iewStats.stallEvents[IQFull]++;
            ++iewStats.iqFullEvents;

            dispatch_stalls.push(checkDispatchStall(tid, NumDQ, inst, disp_seq));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }

        const int numHtmStarts = ldstQueue.numHtmStarts(tid);
        const int numHtmStops = ldstQueue.numHtmStops(tid);
        const int htmDepth = numHtmStarts - numHtmStops;
        if (htmDepth > 0) {
            inst->setHtmTransactionalState(ldstQueue.getLatestHtmUid(tid), htmDepth);
        } else {
            inst->clearHtmTransactionalState();
        }

        if (!inst->isNop() && !inst->isEliminated()) {
            scheduler->addProducer(inst);
            actual_dispatch_output.producerAdds++;
        }

        if (inst->isAtomic()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Memory instruction "
                    "encountered, adding to LSQ.\n",
                    tid);
            ++iewStats.dispStoreInsts;
            ++iewStats.dispNonSpecInsts;

            ldstQueue.insertStore(inst);
            actual_dispatch_output.sqInserts++;
            inst->setCanCommit();
            instQueue.insertNonSpec(inst);
            actual_dispatch_output.nonSpecInserts++;
            add_to_iq = false;
        } else if (inst->isLoad()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Memory instruction "
                    "encountered, adding to LSQ.\n",
                    tid);
            ++iewStats.dispLoadInsts;

            ldstQueue.insertLoad(inst);
            actual_dispatch_output.lqInserts++;
            add_to_iq = true;
            if (valuePred && inst->vpSupported && inst->vpResult.speculative) {
                lvpWakeDependents(inst);
            }
        } else if (inst->isStore()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Memory instruction "
                    "encountered, adding to LSQ.\n",
                    tid);
            ++iewStats.dispStoreInsts;

            ldstQueue.insertStore(inst);
            actual_dispatch_output.sqInserts++;
            if (inst->isStoreConditional()) {
                ++iewStats.dispNonSpecInsts;
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                actual_dispatch_output.nonSpecInserts++;
                add_to_iq = false;
            } else {
                add_to_iq = true;
            }
        } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
            inst->setCanCommit();
            instQueue.insertBarrier(inst);
            actual_dispatch_output.barrierInserts++;
            add_to_iq = false;
        } else if (inst->isNop() || inst->isEliminated()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Nop instruction [sn:%llu] encountered, "
                    "skipping.\n",
                    tid, inst->seqNum);
            inst->setIssued();
            inst->setExecuted();
            inst->setCanCommit();
            iewStats.executedInstStats.numNop[tid]++;
            add_to_iq = false;
        } else {
            assert(!inst->isExecuted());
            add_to_iq = true;
        }

        if (add_to_iq && inst->isNonSpeculative()) {
            DPRINTF(IEW,
                    "[tid:%i] Dispatch: Nonspeculative instruction "
                    "encountered, skipping.\n",
                    tid);
            inst->setCanCommit();
            instQueue.insertNonSpec(inst);
            actual_dispatch_output.nonSpecInserts++;
            add_to_iq = false;
        }

        if (add_to_iq) {
            instQueue.insert(inst, disp_seq);
            actual_dispatch_output.iqInserts++;
        }
        ppDispatch->notify(inst);

        ++iewStats.dispatchedInsts;

        insts_to_dispatch.pop_front();
        actual_dispatch_output.fixedBufferPops++;
        dispatched++;
    }
    verifyDirectDispatchOutputSnapshot(
            dispatch_drain_preview, actual_dispatch_output, tid);

    iewStats.dispDist.sample(dispatched);

    if (!dispatch_stalls.empty()) {
        setAllStalls(dispatch_stalls.front());
        dispatch_stalls.pop();
    } else if (breakDispatch != StallReason::NoStall) {
        setAllStalls(breakDispatch);
    } else {
        // no totally stall, pass rename stall
        // assert(dispatched != 0);
        for (int i = 0; i < dispatchStalls.size(); i++) {
            if (i < dispatched) {   // dispatch success, no stall
                dispatchStalls.at(i) = StallReason::NoStall;
            } else {    // dispatch no insts, pass rename stall
                if (rename_input->renameStallReason.size() == 0) {    // initialize, no stall
                    dispatchStalls.at(i) = StallReason::NoStall;
                } else {    // not dispatch initialize, pass rename stall
                    dispatchStalls.at(i) =
                        rename_input->renameStallReason.at(i);
                }
            }
        }
    }


    for (int i = 0;i < dispatchStalls.size();i++) {
        DPRINTF(IEW,"[tid:%i] dispatchStalls[%d]=%d\n", tid, i, dispatchStalls.at(i));
    }

    if (!insts_to_dispatch.empty()) {
        DPRINTF(IEW,"[tid:%i] Dispatch: Bandwidth Full. Blocking.\n", tid);

        iewStats.stallEvents[DispBWFull]++;
    }

    return dispatch_drain_preview;
}

void
IEW::classifyInstToDispQue(ThreadID tid,
                           const RenameStruct *rename_input,
                           const TimeStruct *commit_input)
{
    auto &insts_to_dispatch = fixedbuffer[tid];

    bool emptyROB = commit_input->commitInfo[tid].emptyROB;

    int insts_to_add = insts_to_dispatch.size();
    std::queue<StallReason> dispatch_stalls;
    StallReason breakDispatch = StallReason::NoStall;
    unsigned dispatched = 0;
    while (!insts_to_dispatch.empty()) {
        auto& inst = insts_to_dispatch.front();
        int ins = cpu->cpuStats.committedInsts.total();
        if (cpu->hasHintDownStream() && ins % 10000 == 1) {
            cpu->hintDownStream->notifyIns(ins);
        }
        int id = getInstDQType(inst);
        if (dispQue[id].size() < dqSize[id]) {
            if (inst->isSquashed()) {
                ++iewStats.dispSquashedInsts;
                insts_to_dispatch.pop_front();

                dispatch_stalls.push(StallReason::InstSquashed);
                continue;
            }

            if (checkSerialize(inst, commit_input)) {
                DPRINTF(IEW, "[tid:%i] [sn:%llu] Dispatch: Serialize instruction encountered.\n", tid, inst->seqNum);
                break;
            }

            // hardware transactional memory
            // CPU needs to track transactional state in program order.
            const int numHtmStarts = ldstQueue.numHtmStarts(tid);
            const int numHtmStops = ldstQueue.numHtmStops(tid);
            const int htmDepth = numHtmStarts - numHtmStops;
            if (htmDepth > 0) {
                inst->setHtmTransactionalState(ldstQueue.getLatestHtmUid(tid),
                                                htmDepth);
            } else {
                inst->clearHtmTransactionalState();
            }

            if (inst->isAtomic()) {
                ++iewStats.dispStoreInsts;
                ++iewStats.dispNonSpecInsts;
            } else if (inst->isLoad()) {
                ++iewStats.dispLoadInsts;
            } else if (inst->isStore()) {
                ++iewStats.dispStoreInsts;
                if (inst->isStoreConditional()) {
                    ++iewStats.dispNonSpecInsts;
                }
            }
            ++iewStats.dispatchedInsts;
            dispQue[id].push_back(inst);

            if (!inst->isNop() && !inst->isEliminated()) {
                scheduler->addProducer(inst);
            }

            inst->enterDQTick = curTick();
            cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDispQue);

            if (valuePred && inst->vpSupported && inst->vpResult.speculative) {
                lvpWakeDependents(inst);
            }

            insts_to_dispatch.pop_front();
            dispatched++;
        } else {
            dispatch_stalls.push(checkDispatchStall(tid, id, inst, -1));
            breakDispatch = dispatch_stalls.back();
            blockReason = breakDispatch;
            break;
        }
    }

    if (!dispatch_stalls.empty()) {
        setAllStalls(dispatch_stalls.front());
        dispatch_stalls.pop();
    } else if (breakDispatch != StallReason::NoStall) {
        setAllStalls(breakDispatch);
    } else {
        // no totally stall, pass rename stall
        // assert(dispatched != 0);
        for (int i = 0; i < dispatchStalls.size(); i++) {
            if (i < dispatched) {   // dispatch success, no stall
                dispatchStalls.at(i) = StallReason::NoStall;
            } else {    // dispatch no insts, pass rename stall
                if (rename_input->renameStallReason.size() == 0) {    // initialize, no stall
                    dispatchStalls.at(i) = StallReason::NoStall;
                } else {    // not dispatch initialize, pass rename stall
                    dispatchStalls.at(i) =
                        rename_input->renameStallReason.at(i);
                }
            }
        }
    }


    for (int i = 0;i < dispatchStalls.size();i++) {
        DPRINTF(IEW,"[tid:%i] dispatchStalls[%d]=%d\n", tid, i, dispatchStalls.at(i));
    }

    if (!insts_to_dispatch.empty()) {
        DPRINTF(IEW,"[tid:%i] Dispatch: Bandwidth Full. Blocking.\n", tid);
        iewStats.stallEvents[DispBWFull]++;
    }
}

void
IEW::dispatchInstFromDispQue()
{
    DynInstPtr inst;
    int dis_num_inst = 0;

    for (int i = 0; i < NumDQ; i++) {
        int dispatched = 0;
        int disp_seq = -1;
        scheduler->lookahead(dispQue[i]);
        while (!dispQue[i].empty() && dispatched < dispWidth[i]) {
            inst = dispQue[i].front();
            ThreadID tid = inst->threadNumber;
            disp_seq++;

            // Check for squashed instructions.
            if (inst->isSquashed()) {
                DPRINTF(IEW, "[tid:%i] [sn:%llu] Dispatch: Squashed instruction encountered, "
                        "not adding to IQ.\n", tid, inst->seqNum);

                dispQue[i].pop_front();
                continue;
            }

            // Check for ready conditions.(ready: !full && !bwFull )
            if (!scheduler->ready(inst, disp_seq)) {
                DPRINTF(IEW, "[tid:%i] Dispatch: IQ is full or bwFull.\n", tid);

                iewStats.stallEvents[IQFull]++;
                ++iewStats.iqFullEvents;
                break;
            }

            // Check LSQ if inst is LD/ST
            if ((inst->isAtomic() && ldstQueue.sqFull(tid)) ||
                (inst->isLoad() && ldstQueue.lqFull(tid)) ||
                (inst->isStore() && ldstQueue.sqFull(tid))) {
                DPRINTF(IEW, "[tid:%i] Dispatch: %s has become full.\n",tid,
                        inst->isLoad() ? "LQ" : "SQ");

                iewStats.stallEvents[LSQFull]++;

                ++iewStats.lsqFullEvents;
                break;
            }

            bool add_to_iq = false;
            // Otherwise issue the instruction just fine.
            if (inst->isAtomic()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in store queue
                ldstQueue.insertStore(inst);

                // AMOs need to be set as "canCommit()"
                // so that commit can process them when they reach the
                // head of commit.
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);
                add_to_iq = false;
            } else if (inst->isLoad()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in load queue
                ldstQueue.insertLoad(inst);

                add_to_iq = true;

            } else if (inst->isStore()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Memory instruction "
                        "encountered, adding to LSQ.\n", tid);

                // allocate entry in store queue
                ldstQueue.insertStore(inst);

                if (inst->isStoreConditional()) {
                    // Store conditionals need to be set as "canCommit()"
                    // so that commit can process them when they reach the
                    // head of commit.
                    // @todo: This is somewhat specific to Alpha.
                    inst->setCanCommit();
                    instQueue.insertNonSpec(inst);
                    add_to_iq = false;

                } else {
                    add_to_iq = true;
                }
            } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
                // Same as non-speculative stores.
                inst->setCanCommit();
                instQueue.insertBarrier(inst);
                add_to_iq = false;
            } else if (inst->isNop() || inst->isEliminated()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Nop instruction [sn:%llu] encountered, "
                        "skipping.\n", tid, inst->seqNum);

                inst->setIssued();
                inst->setExecuted();
                inst->setCanCommit();

                iewStats.executedInstStats.numNop[tid]++;

                add_to_iq = false;
            } else {
                assert(!inst->isExecuted());
                add_to_iq = true;
            }

            if (add_to_iq && inst->isNonSpeculative()) {
                DPRINTF(IEW, "[tid:%i] Dispatch: Nonspeculative instruction "
                        "encountered, skipping.\n", tid);

                // Same as non-speculative stores.
                inst->setCanCommit();

                // Specifically insert it as nonspeculative.
                instQueue.insertNonSpec(inst);

                add_to_iq = false;
            }

            // If the instruction queue is not full, then add the
            // instruction.
            if (add_to_iq) {
                DPRINTF(IEW, "[tid:%i] Dispatch: [sn:%llu] dispatched to IQ.\n", tid, inst->seqNum);
                instQueue.insert(inst, disp_seq);
            }
            ++dis_num_inst;

            inst->exitDQTick = curTick();

    #if TRACING_ON
            inst->dispatchTick = curTick() - inst->fetchTick;
    #endif
            ppDispatch->notify(inst);

            dispQue[i].pop_front();
            dispatched++;
        }
    }
    iewStats.dispDist.sample(dis_num_inst);
}

void
IEW::printAvailableInsts()
{
    int inst = 0;

    std::cout << "Available Instructions: ";

    while (fromIssue->insts[inst]) {

        if (inst%3==0) std::cout << "\n\t";

        std::cout << "PC: " << fromIssue->insts[inst]->pcState()
             << " TN: " << fromIssue->insts[inst]->threadNumber
             << " SN: " << fromIssue->insts[inst]->seqNum << " | ";

        inst++;

    }

    std::cout << "\n";
}

void
IEW::SquashCheckAfterExe(DynInstPtr inst)
{
    ThreadID tid = inst->threadNumber;

    if (inst->isControl()) {
        auto &resolved_cfis = toFetch->iewInfo[tid].resolvedCFIs;
        TimeStruct::IewComm::ResolvedCFIEntry entry;
        entry.ftqId = inst->getFtqId();
        entry.pc = inst->getPC();
        resolved_cfis.push_back(entry);
    }

    if (!fetchRedirect[tid] ||
        !toCommit->squash[tid] ||
        toCommit->squashedSeqNum[tid] > inst->seqNum) {

        // Prevent testing for misprediction on load instructions,
        // that have not been executed.
        bool loadNotExecuted = !inst->isExecuted() && inst->isLoad();

        if (cpu->isTraceMode() && inst->hasTraceBranchInfo()) {
            std::unique_ptr<PCStateBase> new_pc(inst->pcState().clone());
            new_pc->as<RiscvISA::PCState>().npc(inst->traceBranchNextPC());
            inst->pcState(*new_pc);
        }

        if (inst->mispredicted() && !loadNotExecuted) {
            fetchRedirect[tid] = true;

            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Branch mispredict detected.\n",
                    tid, inst->seqNum);
            DPRINTF(IEW, "[tid:%i] [sn:%llu] "
                    "Predicted target was PC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg());
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Redirecting fetch to PC: %s\n",
                    tid, inst->seqNum, inst->pcState());
            // If incorrect, then signal the ROB that it must be squashed.
            squashDueToBranch(inst, tid);

            ppMispredict->notify(inst);

            if (inst->readPredTaken()) {
                iewStats.predictedTakenIncorrect++;
            } else {
                iewStats.predictedNotTakenIncorrect++;
            }
        } else if (ldstQueue.violation(tid)) {
            assert(inst->isMemRef());
            // If there was an ordering violation, then get the
            // DynInst that caused the violation.  Note that this
            // clears the violation signal.
            DynInstPtr violator;
            violator = ldstQueue.getMemDepViolator(tid);

            DPRINTF(IEW, "LDSTQ detected a violation. Violator PC: %s "
                    "[sn:%lli], inst PC: %s [sn:%lli]. Addr is: %#x.\n",
                    violator->pcState(), violator->seqNum,
                    inst->pcState(), inst->seqNum, inst->physEffAddr);

            fetchRedirect[tid] = true;

            // Tell the instruction queue that a violation has occured.
            if (enableStoreSetTrain) {
                instQueue.violation(inst, violator);
            }
            violator->setProducerStorePC(inst->pcState().instAddr());

            // Squash.
            squashDueToMemOrder(violator, tid);

            ++iewStats.memOrderViolationEvents;
        }
    } else {
        // Reset any state associated with redirects that will not
        // be used.
        if (ldstQueue.violation(tid)) {
            assert(inst->isMemRef());

            DynInstPtr violator = ldstQueue.getMemDepViolator(tid);

            DPRINTF(IEW, "LDSTQ detected a violation.  Violator PC: "
                    "%s, inst PC: %s.  Addr is: %#x.\n",
                    violator->pcState(), inst->pcState(),
                    inst->physEffAddr);
            DPRINTF(IEW, "Violation will not be handled because "
                    "already squashing\n");

            ++iewStats.memOrderViolationEvents;
        }
    }
}

void
IEW::executeInsts()
{
    wbNumInst = 0;
    wbCycle = 0;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;
        fetchRedirect[tid] = false;
    }

    // Uncomment this if you want to see all available instructions.
    // @todo This doesn't actually work anymore, we should fix it.
//    printAvailableInsts();

    // Clear resolvedFSQId and resolvedInstPC since they are already handled in frontend
    ThreadID tid = *activeThreads->begin();
    toFetch->iewInfo[tid].resolvedCFIs.clear();

    // Execute/writeback any instructions that are available.
    int insts_to_execute = fromIssue->size;
    fromIssue->size = 0;
    int inst_num = 0;
    for (; inst_num < insts_to_execute;
          ++inst_num) {

        DPRINTF(IEW, "Execute: Executing instructions from IQ.\n");

        DynInstPtr inst = instQueue.getInstToExecute();

        // Notify potential listeners that this instruction has started
        // executing
        ppExecute->notify(inst);

        // Check if the instruction is squashed; if so then skip it
        if (inst->isSquashed()) {
            DPRINTF(IEW, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                         " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                         inst->seqNum);

            // Consider this instruction executed so that commit can go
            // ahead and retire the instruction.
            inst->setExecuted();

            // Not sure if I should set this here or just let commit try to
            // commit any squashed instructions.  I like the latter a bit more.
            inst->setCanCommit();

            // avoid "not a load cancel" for using the squashed instruction's data
            scheduler->bypassWriteback(inst);

            ++iewStats.executedInstStats.numSquashedInsts;

            continue;
        }

        Fault fault = NoFault;

        // Execute instruction.
        // Note that if the instruction faults, it will be handled
        // at the commit stage.
        if (inst->isMemRef()) {
            DPRINTF(IEW, "Execute: Calculating address for memory "
                    "reference.\n");

            // Tell the LDSTQ to execute this instruction (if it is a load).
            if (inst->isAtomic()) {
                // AMOs are treated like store requests
                fault = ldstQueue.executeAmo(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "store.\n");
                    deferMemInst(inst);
                    continue;
                }
            } else if (inst->isLoad()) {
                // add this load inst to loadpipe S0.
                ldstQueue.issueToLoadPipe(inst);
            } else if (inst->isStore()) {
                // add this store inst to storepipe S0.
                ldstQueue.issueToStorePipe(inst);

                // Store conditionals will mark themselves as
                // executed, and their writeback event will add the
                // instruction to the queue to commit.
            } else {
                panic("Unexpected memory type!\n");
            }

        } else {
            // If the instruction has already faulted, then skip executing it.
            // Such case can happen when it faulted during ITLB translation.
            // If we execute the instruction (even if it's a nop) the fault
            // will be replaced and we will lose it.
            if (inst->getFault() == NoFault) {
                inst->execute();
                if (!inst->readPredicate())
                    inst->forwardOldRegs();
            }

            if (!inst->isSplitStoreData()) {
                inst->setExecuted();
                readyToFinish(inst);
            } else {
                DPRINTF(IEW, "Execute: Split store data, [sn:%lli]\n", inst->seqNum);
                // STD is ready, wake up corresponding load if any
                instQueue.resolveSTLFFailInst(inst->seqNum);
                if (inst->sqIt->splitStoreFinish()) {
                    readyToFinish(inst->sqIt->instruction());
                }
            }
        }

        updateExeInstStats(inst);

        // Check if branch prediction was correct, if not then we need
        // to tell commit to squash in flight instructions.  Only
        // handle this if there hasn't already been something that
        // redirects fetch in this group of instructions.

        // This probably needs to prioritize the redirects if a different
        // scheduler is used.  Currently the scheduler schedules the oldest
        // instruction first, so the branch resolution order will be correct.
        if (!(inst->isLoad() || inst->isStore() || inst->isSplitStoreData())) {
            // because Load/Store become pipeline execution ,Load/Store will
            // call this in `lsq_unit.cc` after execution
            SquashCheckAfterExe(inst);
        }
    }

    ldstQueue.executePipeSx();

    // Update and record activity if we processed any instructions.
    if (inst_num) {
        if (exeStatus == Idle) {
            exeStatus = Running;
        }

        updatedQueues = true;

        cpu->activityThisCycle();
    }

    // Need to reset this in case a writeback event needs to write into the
    // iew queue.  That way the writeback event will write into the correct
    // spot in the queue.
    wbNumInst = 0;

}

void
IEW::writebackInsts()
{
    // Loop through the head of the time buffer and wake any
    // dependents.  These instructions are about to write back.  Also
    // mark scoreboard that this instruction is finally complete.
    // Either have IEW have direct access to scoreboard, or have this
    // as part of backwards communication.

    const WritebackPrepareResult prepare =
        runWritebackPrepare(cpu->curCycle());
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        iewStats.instsToCommit[tid] += prepare.instsToCommit[tid];
    }

    for (unsigned inst_num = 0; inst_num < prepare.validInsts; ++inst_num) {
        DynInstPtr inst = toCommit->insts[inst_num];
        fatal_if(!inst,
                 "IEW writeback prepare expected an instruction at slot %u.",
                 inst_num);
        ThreadID tid = prepare.tid[inst_num];
        fatal_if(inst->threadNumber != tid,
                 "IEW writeback prepare tid mismatch at slot %u: "
                 "prepared %i, current %i.",
                 inst_num, tid, inst->threadNumber);

        if (prepare.loadWithSavedRequest[inst_num]) {
            inst->pf_source = inst->savedRequest->mainReq()->getPFSource();
        }

        DPRINTF(IEW, "Sending instructions to commit, [sn:%lli] PC %s.\n",
                inst->seqNum, inst->pcState());

        // Notify potential listeners that execution is complete for this
        // instruction.
        ppToCommit->notify(inst);

        // Some instructions will be sent to commit without having
        // executed because they need commit to handle them.
        // E.g. Strictly ordered loads have not actually executed when they
        // are first sent to commit.  Instead commit must tell the LSQ
        // when it's ready to execute the strictly ordered load.
        if (prepare.wakeEligible[inst_num]) {

            scheduler->writebackWakeup(inst);
            int dependents = instQueue.wakeDependents(inst);

            for (int i = 0; i < inst->numDestRegs(); i++) {
                // Mark register as ready if not pinned
                if (inst->renamedDestIdx(i)->
                        getNumPinnedWritesToComplete() == 0) {
                    DPRINTF(IEW,"Setting Destination Register %i (%s)\n",
                            inst->renamedDestIdx(i)->index(),
                            inst->renamedDestIdx(i)->className());
                    scoreboard->setReg(inst->renamedDestIdx(i));
                }
            }

            if (dependents) {
                iewStats.producerInst[tid]++;
                iewStats.consumerInst[tid]+= dependents;
            }
            iewStats.writebackCount[tid]++;
        }
    }
}

void
IEW::tick()
{
    const RenameStruct *rename_input = renameInput(cpu->curCycle());
    const TimeStruct *commit_input = commitInput(cpu->curCycle());

    blockReason = StallReason::NoStall;
    for (int i = 0;i < rename_input->fetchStallReason.size();i++) {
        iewStats.fetchStallReason[rename_input->fetchStallReason[i]]++;
    }
    for (int i = 0;i < rename_input->decodeStallReason.size();i++) {
        iewStats.decodeStallReason[rename_input->decodeStallReason[i]]++;
    }
    for (int i = 0;i < rename_input->renameStallReason.size();i++) {
        iewStats.renameStallReason[rename_input->renameStallReason[i]]++;
    }

    wbNumInst = 0;
    wbCycle = 0;

    wroteToTimeBuffer = false;
    updatedQueues = false;

    scheduler->tick();
    ldstQueue.tick();

    // dispatch
    moveInstsToBuffer(rename_input);
    checkSquash(commit_input);
    dispatchInsts(rename_input, commit_input);

    for (int i = 0;i < dispatchStalls.size();i++) {
        iewStats.dispatchStallReason[dispatchStalls[i]]++;
    }

    // update the LSQ and scheduler before we check for ready instructions to execute
    ldstQueue.processWriteback();
    if (exeStatus != Squashing) {
        instQueue.scheduleReadyInsts();

        executeInsts();

        writebackInsts();
    }
    scheduler->issueAndSelect();

    bool broadcast_free_entries = false;

    if (updatedQueues || exeStatus == Running || updateLSQNextCycle) {
        exeStatus = Idle;
        updateLSQNextCycle = false;

        broadcast_free_entries = true;
    }

    // Writeback any stores using any leftover bandwidth.

    // Check the committed load/store signals to see if there's a load
    // or store to commit.  Also check if it's being told to execute a
    // nonspeculative instruction.
    // This is pretty inefficient...

    auto threads = activeThreads->begin();
    while (threads != activeThreads->end()) {
        ThreadID tid = (*threads++);

        DPRINTF(IEW,"Commit processing [tid:%i]\n",tid);

        if (commit_input->commitInfo[tid].doneMemSeqNum != 0 &&
            !commit_input->commitInfo[tid].squash &&
            !commit_input->commitInfo[tid].robSquashing) {

            // Marks some of the entries in the store queue as canWB and
            // they will be moved to the store buffer when appropriate.
            InstSeqNum done_mem_seq =
                commit_input->commitInfo[tid].doneMemSeqNum;
            ldstQueue.commitStores(done_mem_seq, tid);
            updateLSQNextCycle = true;
        }

        // Update structures based on instructions committed.
        if (commit_input->commitInfo[tid].doneSeqNum != 0 &&
            !commit_input->commitInfo[tid].squash &&
            !commit_input->commitInfo[tid].robSquashing) {

            InstSeqNum done_seq = commit_input->commitInfo[tid].doneSeqNum;
            ldstQueue.commitLoads(done_seq, tid);
            updateLSQNextCycle = true;

            instQueue.commit(done_seq, tid);
        }

        if (commit_input->commitInfo[tid].nonSpecSeqNum != 0) {

            //DPRINTF(IEW,"NonspecInst from thread %i",tid);
            if (commit_input->commitInfo[tid].strictlyOrdered) {
                instQueue.replayMemInst(
                    commit_input->commitInfo[tid].strictlyOrderedLoad);
                commit_input->commitInfo[tid].strictlyOrderedLoad
                    ->setAtCommit();
            } else {
                instQueue.scheduleNonSpec(
                    commit_input->commitInfo[tid].nonSpecSeqNum);
            }
        }

        if (broadcast_free_entries) {
            wroteToTimeBuffer = true;
        }
    }

    DPRINTF(IEW,"LQ has %i free entries. SQ has %i free entries.\n",
            ldstQueue.numFreeLoadEntries(), ldstQueue.numFreeStoreEntries());

    updateActivate();

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
IEW::updateExeInstStats(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    iewStats.executedInstStats.numInsts++;

    //
    //  Control operations
    //
    if (inst->isControl())
        iewStats.executedInstStats.numBranches[tid]++;

    //
    //  Memory operations
    //
    if (inst->isMemRef()) {
        iewStats.executedInstStats.numRefs[tid]++;

        if (inst->isLoad()) {
            iewStats.executedInstStats.numLoadInsts[tid]++;
        }
    }
}

void
IEW::checkMisprediction(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    if (!fetchRedirect[tid] ||
        !toCommit->squash[tid] ||
        toCommit->squashedSeqNum[tid] > inst->seqNum) {
        // In trace mode, override npc with trace nextPC so that
        // mispredicted() and squash use standard advancePC logic.
        if (cpu->isTraceMode() && inst->hasTraceBranchInfo()) {
            std::unique_ptr<PCStateBase> new_pc(inst->pcState().clone());
            new_pc->as<RiscvISA::PCState>().npc(inst->traceBranchNextPC());
            inst->pcState(*new_pc);
        }

        if (inst->mispredicted()) {
            fetchRedirect[tid] = true;

            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Branch mispredict detected.\n",
                    tid, inst->seqNum);
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Predicted target was PC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg());
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Redirecting fetch to PC: %s\n",
                    tid, inst->seqNum, inst->pcState());
            // If incorrect, then signal the ROB that it must be squashed.
            squashDueToBranch(inst, tid);

            if (inst->readPredTaken()) {
                iewStats.predictedTakenIncorrect++;
            } else {
                iewStats.predictedNotTakenIncorrect++;
            }
        }
    }
}

void
IEW::loadCancel(const DynInstPtr &inst)
{
    scheduler->loadCancel(inst);
}

void
IEW::stlfFailLdReplay(const DynInstPtr &inst, const InstSeqNum &store_seq_num)
{
    instQueue.stlfFailLdReplay(inst, store_seq_num);
}

void
IEW::mdpAddrReplayRegister(const DynInstPtr &inst,
                           const std::vector<InstSeqNum> &store_seq_nums)
{
    instQueue.mdpAddrReplayRegister(inst, store_seq_nums);
}

void
IEW::mdpAddrReplayRegisterStrict(const DynInstPtr &inst,
                                 size_t required_store_completed_idx)
{
    instQueue.mdpAddrReplayRegisterStrict(inst, required_store_completed_idx);
}

void
IEW::mdpAddrReplayPipeDone(const DynInstPtr &inst)
{
    instQueue.mdpAddrReplayPipeDone(inst);
}

void
IEW::mdpAddrReplayUpdateStoreCompletedIdx(ThreadID tid,
                                          size_t store_completed_idx)
{
    instQueue.mdpAddrReplayUpdateStoreCompletedIdx(tid, store_completed_idx);
}

uint32_t
IEW::getIQInsts()
{
    return scheduler->getIQInsts();
}

void
IEW::setAllStalls(StallReason dispatchStall)
{
    for (int i = 0;i < dispatchStalls.size();i++) {
        dispatchStalls.at(i) = dispatchStall;
    }
}

StallReason
IEW::checkLoadStoreInst(DynInstPtr inst)
{
    if (inst->isSquashed()) {
        return StallReason::MemSquashed;
    }
    if (inst->isCommitted()) {
        return StallReason::MemCommitRateLimit;
    }
    if (inst->isAtomic() || inst->isStoreConditional()) {
        return StallReason::Atomic;
    }
    if (!inst->readyToIssue()){
        return StallReason::MemNotReady;
    }
    assert(inst->isLoad() || inst->isStore());

    if (inst->isIssued() && inst->translationStarted() && !inst->translationCompleted()) {
        return StallReason::DTlbStall;
    }

    bool inFlight = inst->isIssued() && inst->hasPendingCacheReq();
    bool lsuStall = inst->isIssued() && !inst->hasPendingCacheReq();
    //Level of the cache hierachy where this request was responded to
    //e.g. 0:in l1, 1:in l2
    int depth=-1;
    if (inFlight) {
        assert(inst->pendingCacheReq);
        depth = inst->pendingCacheReq->mainReq()->depth;
    }
    assert(depth < 5);
    bool in_l1 = depth == 0;
    bool in_l2 = depth == 1;
    bool in_l3 = depth == 2;
    bool other_stall = depth == -1;
    // maybe soc does not have l3cache
    // so we can not use in_mem = depth==3
    bool in_mem = !(in_l1 ||  in_l2 || in_l3 || other_stall);
    if (inFlight && in_l1) {
        return inst->isLoad() ? StallReason::LoadL1Bound : StallReason::StoreL1Bound;
    } else if (inFlight && in_l2) {
        return inst->isLoad() ? StallReason::LoadL2Bound : StallReason::StoreL2Bound;
    } else if (inFlight && in_l3) {
        return inst->isLoad() ? StallReason::LoadL3Bound : StallReason::StoreL3Bound;
    } else if (inFlight && in_mem) {
        return inst->isLoad() ? StallReason::LoadMemBound : StallReason::StoreMemBound;
    } else if (inFlight && other_stall) {
        return StallReason::OtherMemStall;
    }

    if (lsuStall) {
        return inst->isLoad() ? StallReason::LoadL1Bound : StallReason::StoreL1Bound;
    } else {
        return StallReason::OtherMemStall;
    }
}

StallReason
IEW::dqTypeToReason(DQType dq_type)
{
    switch (dq_type) {
        case DQType::IntDQ:
            return StallReason::IntDQBandwidth;
        case DQType::MemDQ:
            return StallReason::MemDQBandwidth;
        case DQType::FVDQ:
            return StallReason::FVDQBandwidth;
        default:
            panic("Unknown DQType");
    }
}

IEW::DQType
IEW::getInstDQType(const DynInstPtr &inst)
{
    if (inst->isMemRef() || inst->isReadBarrier() || inst->isWriteBarrier() || inst->isNonSpeculative()) {
        return MemDQ;
    }
    // FIX: fcvt_s_w (Int2Fp) reads INT register, needs INT read port -> should go to IntDQ
    if (inst->opClass() == Int2FpOp) {
        return IntDQ;
    }
    if (inst->isFloating() || inst->isVector()) {
        return FVDQ;
    }
    return IntDQ;
}

StallReason
IEW::checkDispatchStall(ThreadID tid, int dq_stall, const DynInstPtr &dispatch_inst, int disp_seq) {
    DynInstPtr head_inst = rob->readHeadInst(tid);
    if (head_inst == rob->dummyInst) {
        if (dq_stall != NumDQ) {
            // this call is from dispatch to classify the reason why an instr cannot be dispatched
            return dqTypeToReason(static_cast<DQType>(dq_stall));
        } else {  // this call is to tell rename the stall status
            return StallReason::NoStall;
        }
    }

    if (dq_stall != NumDQ && disp_seq >= 0) {
        // get dq head inst
        assert(dispQue[dq_stall].size());
        auto &dq_head = dispQue[dq_stall].front();
        bool ready = !scheduler->ready(dq_head, disp_seq);
        if (getInstDQType(dispatch_inst) == getInstDQType(dq_head) && !ready) {
            return dqTypeToReason(static_cast<DQType>(dq_stall));
        }

        if (dispatch_inst->isStore() && !ldstQueue.sqFull()) {
            // store cannot be dispatched while sq is not full
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isLoad() && !(ldstQueue.lqFull() || rob->isFull() || !ready)) {
            return StallReason::MemDQBandwidth;
        }
        if (dispatch_inst->isAtCommit() && !(ldstQueue.lqFull() || ldstQueue.sqFull())) {
            return StallReason::MemDQBandwidth;
        }

        if ((dispatch_inst->isFloating() || dispatch_inst->isVector()) &&
            !(rob->isFull() || !ready)) {
            return StallReason::FVDQBandwidth;
        }

        if (dispatch_inst->isInteger() && !(rob->isFull() || !ready)) {
            return StallReason::IntDQBandwidth;
        }
    }

    assert(head_inst);

    if (head_inst->readyTick == -1) {
        DPRINTF(Counters, "IEW: [tid:%i] [sn:%llu] "
                "Dispatch: Instruction not ready. nonSpeculative:%d\n",
                tid, head_inst->seqNum, head_inst->isNonSpeculative());
        if (head_inst->isNonSpeculative()) {
            return StallReason::SerializeStall;
        } else if (head_inst->isLoad() && ldstQueue.lqFull(tid)) {
            return checkLSQStall(tid, true);
        } else if ((head_inst->isStore() || head_inst->isAtomic()) && ldstQueue.sqFull(tid)) {
            return checkLSQStall(tid, false);
        } else {
            return StallReason::InstNotReady;
        }
    }

    if (head_inst->isLoad() || head_inst->isStore() || head_inst->isAtomic()) {
        return checkLoadStoreInst(head_inst);
    } else {
        if (head_inst->firstIssue != -1) {
            if (head_inst->isVector()) {
                return StallReason::VectorLongExecute;
            } else {
                return StallReason::ScalarLongExecute;
            }
        } else {
            if (head_inst->isVector()) {
                return StallReason::VectorReadyButNotIssued;
            } else {
                return StallReason::ScalarReadyButNotIssued;
            }
        }
    }

    return StallReason::OtherStall;
}

StallReason
IEW::checkLSQStall(ThreadID tid, bool isLoad)
{
    DynInstPtr head_inst = ldstQueue.getLSQHeadInst(tid, isLoad);
    return checkLoadStoreInst(head_inst);
}

void
IEW::setRob(ROB *rob)
{
    this->rob = rob;
}

} // namespace o3
} // namespace gem5
