/*
 * Copyright (c) 2010-2012, 2014-2019 ARM Limited
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

#include "cpu/o3/rename.hh"

#include <algorithm>
#include <list>
#include <memory>

#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/reg_class.hh"
#include "debug/Activity.hh"
#include "debug/O3PipeView.hh"
#include "debug/Rename.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

namespace
{

constexpr unsigned NumRenameFutureInputSkipReasons = 4;
constexpr unsigned NumRenameFutureInputCommitControlReasons = 3;
constexpr unsigned NumRenameFuturePreviewSkipReasons = 2;
constexpr unsigned NumRenameFuturePrepareMismatchReasons = 14;
constexpr unsigned NumRenameFutureCandidateSchedulerReasons = 8;
constexpr unsigned NumRenameFutureCandidateExpectedPopBuckets = 9;
constexpr unsigned NumRenameFutureCandidateInputDifferenceFields = 10;
constexpr unsigned NumRenameFutureCandidateInputStabilityReasons = 4;
constexpr unsigned NumRenameFutureCandidateIEWBlockDiffDirections = 4;

enum RenameFutureInputSkipReason : uint8_t
{
    RenameFutureInputMissingSnapshot,
    RenameFutureInputNoActiveThreads,
    RenameFutureInputCommitControl,
    RenameFutureInputReleaseSeqNotReady,
};

enum RenameFutureInputCommitControlReason : uint8_t
{
    RenameFutureInputCommitSquash,
    RenameFutureInputCommitRobSquashing,
    RenameFutureInputCommitDoneSeqNum,
};

enum RenameFuturePreviewSkipReason : uint8_t
{
    RenameFuturePreviewActiveRename,
    RenameFuturePreviewMultipleActive,
};

enum RenameFuturePrepareMismatchReason : uint8_t
{
    RenameFuturePrepareMismatchCycle,
    RenameFuturePrepareMismatchSelectedTid,
    RenameFuturePrepareMismatchBlockedTid,
    RenameFuturePrepareMismatchActiveThreads,
    RenameFuturePrepareMismatchBlockedThreads,
    RenameFuturePrepareMismatchRegFullEvents,
    RenameFuturePrepareMismatchMultipleActive,
    RenameFuturePrepareMismatchThreadActive,
    RenameFuturePrepareMismatchThreadDecodeBlock,
    RenameFuturePrepareMismatchThreadDecodeBlockReason,
    RenameFuturePrepareMismatchThreadCanRename,
    RenameFuturePrepareMismatchThreadIEWBlock,
    RenameFuturePrepareMismatchThreadBlock,
    RenameFuturePrepareMismatchThreadRenameBlockReason,
};

enum RenameFutureCandidateInputDifferenceField : uint8_t
{
    RenameFutureCandidateInputNumThreads,
    RenameFutureCandidateInputFixedbufferEmpty,
    RenameFutureCandidateInputFixedbufferSize,
    RenameFutureCandidateInputDemandPhyRegs,
    RenameFutureCandidateInputFreePhyRegs,
    RenameFutureCandidateInputIEWBlock,
    RenameFutureCandidateInputIEWReason,
    RenameFutureCandidateInputRobHeadStall,
    RenameFutureCandidateInputLQHeadStall,
    RenameFutureCandidateInputSQHeadStall,
};

enum RenameFutureCandidateInputStabilityReason : uint8_t
{
    RenameFutureCandidatePrepareMatchInputMatch,
    RenameFutureCandidatePrepareMatchInputDiff,
    RenameFutureCandidatePrepareMismatchInputMatch,
    RenameFutureCandidatePrepareMismatchInputDiff,
};

enum RenameFutureCandidateIEWBlockDiffDirection : uint8_t
{
    RenameFutureCandidateIEWBlockMatchFalseToTrue,
    RenameFutureCandidateIEWBlockMatchTrueToFalse,
    RenameFutureCandidateIEWBlockMismatchFalseToTrue,
    RenameFutureCandidateIEWBlockMismatchTrueToFalse,
};

const char *
renameFutureInputSkipReasonName(unsigned reason)
{
    switch (reason) {
      case RenameFutureInputMissingSnapshot:
        return "MissingSnapshot";
      case RenameFutureInputNoActiveThreads:
        return "NoActiveThreads";
      case RenameFutureInputCommitControl:
        return "CommitControl";
      case RenameFutureInputReleaseSeqNotReady:
        return "ReleaseSeqNotReady";
    }

    return "Unknown";
}

const char *
renameFutureInputCommitControlReasonName(unsigned reason)
{
    switch (reason) {
      case RenameFutureInputCommitSquash:
        return "Squash";
      case RenameFutureInputCommitRobSquashing:
        return "RobSquashing";
      case RenameFutureInputCommitDoneSeqNum:
        return "DoneSeqNum";
    }

    return "Unknown";
}

const char *
renameFuturePreviewSkipReasonName(unsigned reason)
{
    switch (reason) {
      case RenameFuturePreviewActiveRename:
        return "ActiveRename";
      case RenameFuturePreviewMultipleActive:
        return "MultipleActive";
    }

    return "Unknown";
}

const char *
renameFuturePrepareMismatchReasonName(unsigned reason)
{
    switch (reason) {
      case RenameFuturePrepareMismatchCycle:
        return "Cycle";
      case RenameFuturePrepareMismatchSelectedTid:
        return "SelectedTid";
      case RenameFuturePrepareMismatchBlockedTid:
        return "BlockedTid";
      case RenameFuturePrepareMismatchActiveThreads:
        return "ActiveThreads";
      case RenameFuturePrepareMismatchBlockedThreads:
        return "BlockedThreads";
      case RenameFuturePrepareMismatchRegFullEvents:
        return "RegFullEvents";
      case RenameFuturePrepareMismatchMultipleActive:
        return "MultipleActive";
      case RenameFuturePrepareMismatchThreadActive:
        return "ThreadActive";
      case RenameFuturePrepareMismatchThreadDecodeBlock:
        return "ThreadDecodeBlock";
      case RenameFuturePrepareMismatchThreadDecodeBlockReason:
        return "ThreadDecodeBlockReason";
      case RenameFuturePrepareMismatchThreadCanRename:
        return "ThreadCanRename";
      case RenameFuturePrepareMismatchThreadIEWBlock:
        return "ThreadIEWBlock";
      case RenameFuturePrepareMismatchThreadBlock:
        return "ThreadBlock";
      case RenameFuturePrepareMismatchThreadRenameBlockReason:
        return "ThreadRenameBlockReason";
    }

    return "Unknown";
}

const char *
renameFutureCandidateSchedulerReasonName(unsigned reason)
{
    switch (reason) {
      case 0:
        return "NoBlock";
      case 1:
        return "InvalidState";
      case 2:
        return "InvalidOp";
      case 3:
        return "InvalidDispSeq";
      case 4:
        return "InvalidSelector";
      case 5:
        return "ReplayBlocked";
      case 6:
        return "IQFull";
      case 7:
        return "InportFull";
    }

    return "Unknown";
}

const char *
renameFutureCandidateExpectedPopName(unsigned pops)
{
    switch (pops) {
      case 0:
        return "0";
      case 1:
        return "1";
      case 2:
        return "2";
      case 3:
        return "3";
      case 4:
        return "4";
      case 5:
        return "5";
      case 6:
        return "6";
      case 7:
        return "7";
      case 8:
        return "8plus";
    }

    return "Unknown";
}

const char *
renameFutureCandidateInputDifferenceFieldName(unsigned field)
{
    switch (field) {
      case RenameFutureCandidateInputNumThreads:
        return "NumThreads";
      case RenameFutureCandidateInputFixedbufferEmpty:
        return "FixedbufferEmpty";
      case RenameFutureCandidateInputFixedbufferSize:
        return "FixedbufferSize";
      case RenameFutureCandidateInputDemandPhyRegs:
        return "DemandPhyRegs";
      case RenameFutureCandidateInputFreePhyRegs:
        return "FreePhyRegs";
      case RenameFutureCandidateInputIEWBlock:
        return "IEWBlock";
      case RenameFutureCandidateInputIEWReason:
        return "IEWReason";
      case RenameFutureCandidateInputRobHeadStall:
        return "RobHeadStall";
      case RenameFutureCandidateInputLQHeadStall:
        return "LQHeadStall";
      case RenameFutureCandidateInputSQHeadStall:
        return "SQHeadStall";
    }

    return "Unknown";
}

const char *
renameFutureCandidateInputStabilityReasonName(unsigned reason)
{
    switch (reason) {
      case RenameFutureCandidatePrepareMatchInputMatch:
        return "PrepareMatchInputMatch";
      case RenameFutureCandidatePrepareMatchInputDiff:
        return "PrepareMatchInputDiff";
      case RenameFutureCandidatePrepareMismatchInputMatch:
        return "PrepareMismatchInputMatch";
      case RenameFutureCandidatePrepareMismatchInputDiff:
        return "PrepareMismatchInputDiff";
    }

    return "Unknown";
}

const char *
renameFutureCandidateIEWBlockDiffDirectionName(unsigned direction)
{
    switch (direction) {
      case RenameFutureCandidateIEWBlockMatchFalseToTrue:
        return "PrepareMatchCandidateFalseActualTrue";
      case RenameFutureCandidateIEWBlockMatchTrueToFalse:
        return "PrepareMatchCandidateTrueActualFalse";
      case RenameFutureCandidateIEWBlockMismatchFalseToTrue:
        return "PrepareMismatchCandidateFalseActualTrue";
      case RenameFutureCandidateIEWBlockMismatchTrueToFalse:
        return "PrepareMismatchCandidateTrueActualFalse";
    }

    return "Unknown";
}

} // namespace

Rename::Rename(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      iewToRenameDelay(params.iewToRenameDelay),
      decodeToRenameDelay(params.decodeToRenameDelay),
      commitToRenameDelay(params.commitToRenameDelay),
      renameWidth(params.renameWidth),
      releaseWidth(params.phyregReleaseWidth),
      numThreads(params.numThreads),
      stats(_cpu),
      valuePred(params.valuePred),
        enableSelectiveVPFlush(params.enableSelectiveVPFlush)
{
    if (renameWidth > MaxWidth)
        fatal("renameWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             renameWidth, static_cast<int>(MaxWidth));

    for (uint32_t tid = 0; tid < MaxThreads; tid++) {
        fixedbuffer[tid] = boost::circular_buffer<DynInstPtr>(renameWidth);
        renameMap[tid] = nullptr;
        stalls[tid] = {false, false};
    }

    assert(decodeToRenameDelay == 1);

    renameStalls.resize(renameWidth, StallReason::NoStall);
}

std::string
Rename::name() const
{
    return cpu->name() + ".rename";
}

Rename::RenameStats::RenameStats(statistics::Group *parent)
    : statistics::Group(parent, "rename"),
      ADD_STAT(squashCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is squashing"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is idle"),
      ADD_STAT(blockCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is blocking"),
      ADD_STAT(serializeStallCycles, statistics::units::Cycle::get(),
               "count of cycles rename stalled for serializing inst"),
      ADD_STAT(runCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is running"),
      ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is unblocking"),
      ADD_STAT(renamedInsts, statistics::units::Count::get(),
               "Number of instructions processed by rename"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions processed by rename"),
      ADD_STAT(prepareTasks, statistics::units::Count::get(),
               "Number of rename prepare tasks submitted"),
      ADD_STAT(prepareMerges, statistics::units::Count::get(),
               "Number of rename prepare results merged"),
      ADD_STAT(prepareActiveThreads, statistics::units::Count::get(),
               "Accumulated active thread count seen by rename prepare"),
      ADD_STAT(prepareBlockedThreads, statistics::units::Count::get(),
               "Accumulated blocked thread count seen by rename prepare"),
      ADD_STAT(prepareInactiveThreads, statistics::units::Count::get(),
               "Accumulated inactive thread count seen by rename prepare"),
      ADD_STAT(prepareMultipleActive, statistics::units::Count::get(),
               "Number of times rename prepare saw multiple active threads"),
      ADD_STAT(futurePrepareProbes, statistics::units::Count::get(),
               "Number of future rename prepare probes submitted"),
      ADD_STAT(futurePrepareSkipped, statistics::units::Count::get(),
               "Number of future rename prepare probes skipped"),
      ADD_STAT(futureInputSkipReasons, statistics::units::Count::get(),
               "Breakdown of why future rename input construction was "
               "skipped"),
      ADD_STAT(futureInputCommitControlReasons,
               statistics::units::Count::get(),
               "Breakdown of which commit control field blocked future "
               "rename input construction"),
      ADD_STAT(futurePreviewSkipReasons,
               statistics::units::Count::get(),
               "Breakdown of why future Rename-to-Decode latch preview "
               "failed"),
      ADD_STAT(futureInputVirtualReleaseSteps,
               statistics::units::Count::get(),
               "Number of future rename inputs with projected phys-reg "
               "release deltas"),
      ADD_STAT(futureInputVirtualReleaseRegs,
               statistics::units::Count::get(),
               "Number of phys regs virtually added to future rename input "
               "tokens"),
      ADD_STAT(futurePrepareMerges, statistics::units::Count::get(),
               "Number of future rename prepare results made pending"),
      ADD_STAT(futurePrepareReuses, statistics::units::Count::get(),
               "Number of rename prepares reused from future work"),
      ADD_STAT(futurePrepareChecks, statistics::units::Count::get(),
               "Number of future rename prepare validation checks"),
      ADD_STAT(futurePrepareMatches, statistics::units::Count::get(),
               "Number of future rename prepare validation matches"),
      ADD_STAT(futurePrepareMismatches, statistics::units::Count::get(),
               "Number of future rename prepare validation mismatches"),
      ADD_STAT(futurePrepareMismatchReasons,
               statistics::units::Count::get(),
               "Breakdown of future rename prepare validation mismatches"),
      ADD_STAT(futurePrepareStale, statistics::units::Count::get(),
               "Number of stale future rename prepare results discarded"),
      ADD_STAT(futureCandidatePrepareChecks,
               statistics::units::Count::get(),
               "Number of diagnostic candidate future rename prepares "
               "checked"),
      ADD_STAT(futureCandidatePrepareMatches,
               statistics::units::Count::get(),
               "Number of diagnostic candidate future rename prepares "
               "matching current rename prepare"),
      ADD_STAT(futureCandidatePrepareMismatches,
               statistics::units::Count::get(),
               "Number of diagnostic candidate future rename prepares "
               "mismatching current rename prepare"),
      ADD_STAT(futureCandidatePrepareMismatchReasons,
               statistics::units::Count::get(),
               "Breakdown of diagnostic candidate future rename prepare "
               "mismatches"),
      ADD_STAT(futureCandidatePrepareStale,
               statistics::units::Count::get(),
               "Number of stale diagnostic candidate future rename prepares "
               "discarded"),
      ADD_STAT(futureCandidatePrepareMatchesBySchedulerReason,
               statistics::units::Count::get(),
               "Diagnostic candidate future rename prepare matches by "
               "scheduler block reason"),
      ADD_STAT(futureCandidatePrepareMismatchesBySchedulerReason,
               statistics::units::Count::get(),
               "Diagnostic candidate future rename prepare mismatches by "
               "scheduler block reason"),
      ADD_STAT(futureCandidatePrepareMatchesByExpectedPops,
               statistics::units::Count::get(),
               "Diagnostic candidate future rename prepare matches by "
               "expected fixedbuffer pops"),
      ADD_STAT(futureCandidatePrepareMismatchesByExpectedPops,
               statistics::units::Count::get(),
               "Diagnostic candidate future rename prepare mismatches by "
               "expected fixedbuffer pops"),
      ADD_STAT(futureCandidatePrepareInputStability,
               statistics::units::Count::get(),
               "Diagnostic candidate future rename prepare result stability "
               "crossed with input stability"),
      ADD_STAT(futureCandidateIEWBlockDiffDirections,
               statistics::units::Count::get(),
               "Direction of diagnostic candidate/current IEW-to-Rename "
               "block input differences"),
      ADD_STAT(futureCandidateInputChecks,
               statistics::units::Count::get(),
               "Number of diagnostic candidate future rename prepare inputs "
               "checked"),
      ADD_STAT(futureCandidateInputMatches,
               statistics::units::Count::get(),
               "Number of diagnostic candidate future rename prepare inputs "
               "matching current input"),
      ADD_STAT(futureCandidateInputDifferences,
               statistics::units::Count::get(),
               "Number of diagnostic candidate future rename prepare inputs "
               "differing from current input"),
      ADD_STAT(futureCandidateInputDifferenceFields,
               statistics::units::Count::get(),
               "Fields differing in diagnostic candidate future rename "
               "prepare inputs"),
      ADD_STAT(futureCandidateInputMatchDifferenceFields,
               statistics::units::Count::get(),
               "Input fields differing in diagnostic candidates whose "
               "prepare result still matched"),
      ADD_STAT(futureCandidateInputMismatchDifferenceFields,
               statistics::units::Count::get(),
               "Input fields differing in diagnostic candidates whose "
               "prepare result mismatched"),
      ADD_STAT(ROBFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to ROB full"),
      ADD_STAT(IQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to IQ full"),
      ADD_STAT(LQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to LQ full" ),
      ADD_STAT(SQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to SQ full"),
      ADD_STAT(fullRegistersEvents, statistics::units::Count::get(),
               "Number of times there has been no free registers"),
      ADD_STAT(renamedOperands, statistics::units::Count::get(),
               "Number of destination operands rename has renamed"),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of register rename lookups that rename has made"),
      ADD_STAT(intLookups, statistics::units::Count::get(),
               "Number of integer rename lookups"),
      ADD_STAT(fpLookups, statistics::units::Count::get(),
               "Number of floating rename lookups"),
      ADD_STAT(vecLookups, statistics::units::Count::get(),
               "Number of vector rename lookups"),
      ADD_STAT(vecPredLookups, statistics::units::Count::get(),
               "Number of vector predicate rename lookups"),
      ADD_STAT(committedMaps, statistics::units::Count::get(),
               "Number of HB maps that are committed"),
      ADD_STAT(undoneMaps, statistics::units::Count::get(),
               "Number of HB maps that are undone due to squashing"),
      ADD_STAT(serializing, statistics::units::Count::get(),
               "count of serializing insts renamed"),
      ADD_STAT(tempSerializing, statistics::units::Count::get(),
               "count of temporary serializing insts renamed"),
      ADD_STAT(skidInsts, statistics::units::Count::get(),
               "count of insts added to the skid buffer"),
      ADD_STAT(moveEliminated, statistics::units::Count::get(),
               "count of insts eliminated by move elimination"),
      ADD_STAT(constantFolded, statistics::units::Count::get(),
               "count of insts eliminated by constant folding"),
      ADD_STAT(stallEvents, statistics::units::Count::get(),
               "count of stall events")
{
    squashCycles.prereq(squashCycles);
    idleCycles.prereq(idleCycles);
    blockCycles.prereq(blockCycles);
    serializeStallCycles.flags(statistics::total);
    runCycles.prereq(idleCycles);
    unblockCycles.prereq(unblockCycles);

    renamedInsts.prereq(renamedInsts);
    squashedInsts.prereq(squashedInsts);
    prepareTasks.prereq(prepareTasks);
    prepareMerges.prereq(prepareMerges);
    prepareActiveThreads.prereq(prepareActiveThreads);
    prepareBlockedThreads.prereq(prepareBlockedThreads);
    prepareInactiveThreads.prereq(prepareInactiveThreads);
    prepareMultipleActive.prereq(prepareMultipleActive);
    futurePrepareProbes.prereq(futurePrepareProbes);
    futurePrepareSkipped.prereq(futurePrepareSkipped);
    futureInputSkipReasons
        .init(NumRenameFutureInputSkipReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFutureInputSkipReasons; ++i) {
        futureInputSkipReasons.subname(i, renameFutureInputSkipReasonName(i));
    }
    futureInputCommitControlReasons
        .init(NumRenameFutureInputCommitControlReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFutureInputCommitControlReasons; ++i) {
        futureInputCommitControlReasons.subname(
                i, renameFutureInputCommitControlReasonName(i));
    }
    futurePreviewSkipReasons
        .init(NumRenameFuturePreviewSkipReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFuturePreviewSkipReasons; ++i) {
        futurePreviewSkipReasons.subname(
                i, renameFuturePreviewSkipReasonName(i));
    }
    futureInputVirtualReleaseSteps.prereq(futureInputVirtualReleaseSteps);
    futureInputVirtualReleaseRegs.prereq(futureInputVirtualReleaseRegs);
    futurePrepareMerges.prereq(futurePrepareMerges);
    futurePrepareReuses.prereq(futurePrepareReuses);
    futurePrepareChecks.prereq(futurePrepareChecks);
    futurePrepareMatches.prereq(futurePrepareMatches);
    futurePrepareMismatches.prereq(futurePrepareMismatches);
    futurePrepareMismatchReasons
        .init(NumRenameFuturePrepareMismatchReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFuturePrepareMismatchReasons; ++i) {
        futurePrepareMismatchReasons.subname(
                i, renameFuturePrepareMismatchReasonName(i));
    }
    futurePrepareStale.prereq(futurePrepareStale);
    futureCandidatePrepareChecks.prereq(futureCandidatePrepareChecks);
    futureCandidatePrepareMatches.prereq(futureCandidatePrepareMatches);
    futureCandidatePrepareMismatches.prereq(
            futureCandidatePrepareMismatches);
    futureCandidatePrepareMismatchReasons
        .init(NumRenameFuturePrepareMismatchReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFuturePrepareMismatchReasons; ++i) {
        futureCandidatePrepareMismatchReasons.subname(
                i, renameFuturePrepareMismatchReasonName(i));
    }
    futureCandidatePrepareStale.prereq(futureCandidatePrepareStale);
    futureCandidatePrepareMatchesBySchedulerReason
        .init(NumRenameFutureCandidateSchedulerReasons)
        .flags(statistics::total);
    futureCandidatePrepareMismatchesBySchedulerReason
        .init(NumRenameFutureCandidateSchedulerReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFutureCandidateSchedulerReasons; ++i) {
        const char *name = renameFutureCandidateSchedulerReasonName(i);
        futureCandidatePrepareMatchesBySchedulerReason.subname(i, name);
        futureCandidatePrepareMismatchesBySchedulerReason.subname(i, name);
    }
    futureCandidatePrepareMatchesByExpectedPops
        .init(NumRenameFutureCandidateExpectedPopBuckets)
        .flags(statistics::total);
    futureCandidatePrepareMismatchesByExpectedPops
        .init(NumRenameFutureCandidateExpectedPopBuckets)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFutureCandidateExpectedPopBuckets; ++i) {
        const char *name = renameFutureCandidateExpectedPopName(i);
        futureCandidatePrepareMatchesByExpectedPops.subname(i, name);
        futureCandidatePrepareMismatchesByExpectedPops.subname(i, name);
    }
    futureCandidatePrepareInputStability
        .init(NumRenameFutureCandidateInputStabilityReasons)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFutureCandidateInputStabilityReasons;
         ++i) {
        futureCandidatePrepareInputStability.subname(
                i, renameFutureCandidateInputStabilityReasonName(i));
    }
    futureCandidateIEWBlockDiffDirections
        .init(NumRenameFutureCandidateIEWBlockDiffDirections)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFutureCandidateIEWBlockDiffDirections;
         ++i) {
        futureCandidateIEWBlockDiffDirections.subname(
                i, renameFutureCandidateIEWBlockDiffDirectionName(i));
    }
    futureCandidateInputChecks.prereq(futureCandidateInputChecks);
    futureCandidateInputMatches.prereq(futureCandidateInputMatches);
    futureCandidateInputDifferences.prereq(futureCandidateInputDifferences);
    futureCandidateInputDifferenceFields
        .init(NumRenameFutureCandidateInputDifferenceFields)
        .flags(statistics::total);
    futureCandidateInputMatchDifferenceFields
        .init(NumRenameFutureCandidateInputDifferenceFields)
        .flags(statistics::total);
    futureCandidateInputMismatchDifferenceFields
        .init(NumRenameFutureCandidateInputDifferenceFields)
        .flags(statistics::total);
    for (unsigned i = 0; i < NumRenameFutureCandidateInputDifferenceFields;
         ++i) {
        const char *name = renameFutureCandidateInputDifferenceFieldName(i);
        futureCandidateInputDifferenceFields.subname(i, name);
        futureCandidateInputMatchDifferenceFields.subname(i, name);
        futureCandidateInputMismatchDifferenceFields.subname(i, name);
    }

    ROBFullEvents.prereq(ROBFullEvents);
    IQFullEvents.prereq(IQFullEvents);
    LQFullEvents.prereq(LQFullEvents);
    SQFullEvents.prereq(SQFullEvents);
    fullRegistersEvents.prereq(fullRegistersEvents);

    renamedOperands.prereq(renamedOperands);
    lookups.prereq(lookups);
    intLookups.prereq(intLookups);
    fpLookups.prereq(fpLookups);
    vecLookups.prereq(vecLookups);
    vecPredLookups.prereq(vecPredLookups);

    committedMaps.prereq(committedMaps);
    undoneMaps.prereq(undoneMaps);
    serializing.flags(statistics::total);
    tempSerializing.flags(statistics::total);
    skidInsts.flags(statistics::total);
    moveEliminated.flags(statistics::total);
    constantFolded.flags(statistics::total);

    stallEvents.init(StallEventCount).flags(statistics::total);
    std::map < StallEvent, const char* > stall_event_str = {
        { ROBWalk, "ROBWalk"},
        { IEWStall, "IEWStall"},
        { ROBFull, "ROBFull"},
        { IQFull, "IQFull"},
        { LSQFull, "LSQFull"},
        { RegFull, "RegFull"},
        { SerializeInst, "SerializeInst"},
        { BWFull, "BWFull"},
    };

    for (int i = 0; i < StallEventCount; i++) {
        stallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
    }
}

void
Rename::regProbePoints()
{
    ppRename = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Rename");
    ppSquashInRename = new ProbePointArg<SeqNumRegPair>(cpu->getProbeManager(),
                                                        "SquashInRename");
}

void
Rename::setDecodeStall(ThreadID tid, bool block, StallReason reason)
{
    if (stallSignalBank) {
        stallSignalBank->set(StallSignalEdge::RenameToDecode, tid, block,
                             reason);
    } else {
        stallSig->blockDecode[tid] = block;
        stallSig->decodeBlockReason[tid] = reason;
    }
    cpu->getTaskRuntime().recordStallSignalMerge(
            static_cast<unsigned>(StallSignalEdge::RenameToDecode), 1);
}

void
Rename::setDecodeBlock(ThreadID tid, bool block)
{
    if (stallSignalBank) {
        stallSignalBank->setBlock(StallSignalEdge::RenameToDecode, tid,
                                  block);
    } else {
        stallSig->blockDecode[tid] = block;
    }
    cpu->getTaskRuntime().recordStallSignalMerge(
            static_cast<unsigned>(StallSignalEdge::RenameToDecode), 1);
}

Rename::RenamePrepareInput
Rename::buildRenamePrepareInput(
        Cycles cycle,
        const StallSignalLatch *iew_to_rename_override,
        const DecodeStruct *snapshot_decode,
        const TimeStruct *snapshot_iew) const
{
    RenamePrepareInput input;
    input.cycle = cycle;
    input.numThreads = numThreads;

    const StallSignalLatch *iew_to_rename =
        iew_to_rename_override ? iew_to_rename_override :
        (stallSignalBank ?
        &cpu->stallSignalSnapshotOrCurrent(
                cycle, StallSignalEdge::IEWToRename) :
        nullptr);
    const TimeStruct *iew_input =
        snapshot_iew ? snapshot_iew : iewInput(cycle);

    for (int tid = 0; tid < numThreads; ++tid) {
        const auto &insts_to_rename = fixedbuffer[tid];
        const auto *iew_info = &iew_input->iewInfo[tid];
        input.fixedbufferSize[tid] = insts_to_rename.size();
        input.iewToRename.block[tid] =
            iew_to_rename ? iew_to_rename->block[tid] :
                            stallSig->blockRename[tid];
        input.iewToRename.reason[tid] =
            iew_to_rename ? iew_to_rename->reason[tid] :
                            stallSig->renameBlockReason[tid];
        input.robHeadStallReason[tid] = iew_info->robHeadStallReason;
        input.lqHeadStallReason[tid] = iew_info->lqHeadStallReason;
        input.sqHeadStallReason[tid] = iew_info->sqHeadStallReason;

        for (int i = 0; i < NumRenameRegClasses; ++i) {
            input.freePhyRegs[tid][i] =
                renameMap[tid]->numFreeEntries(static_cast<RegClassType>(i));
        }

        for (int i = 0; i < input.fixedbufferSize[tid]; ++i) {
            const DynInstPtr &inst = insts_to_rename.at(i);
            if (inst->isSquashed())
                continue;

            for (int j = 0; j < NumRenameRegClasses; ++j) {
                input.demandPhyRegs[tid][j] +=
                    inst->numDestRegs(static_cast<RegClassType>(j));
            }
        }
    }

    if (snapshot_decode && snapshot_decode->size > 0) {
        const int decoded_insts = std::min(snapshot_decode->size, MaxWidth);
        for (int i = 0; i < decoded_insts; ++i) {
            const DynInstPtr &inst = snapshot_decode->insts[i];
            if (!inst || inst->isSquashed())
                continue;

            if (localSquashVer.largerThan(inst->getVersion()))
                continue;

            const ThreadID tid = inst->threadNumber;
            if (tid >= numThreads)
                continue;

            input.fixedbufferSize[tid]++;
            for (int j = 0; j < NumRenameRegClasses; ++j) {
                input.demandPhyRegs[tid][j] +=
                    inst->numDestRegs(static_cast<RegClassType>(j));
            }
        }
    }

    for (int tid = 0; tid < numThreads; ++tid) {
        input.fixedbufferEmpty[tid] = input.fixedbufferSize[tid] == 0;

        for (int i = 0; i < NumRenameRegClasses; ++i) {
            switch (i) {
              case IntRegClass:
              case FloatRegClass:
              case VecRegClass:
              case RMiscRegClass:
                input.demandPhyRegs[tid][i] =
                    std::max(input.demandPhyRegs[tid][i],
                             static_cast<int>(renameWidth));
                break;
              default:
                break;
            }
        }
    }

    return input;
}

Rename::RenameThreadPrepareResult
Rename::prepareRenameThreadControl(const RenamePrepareInput &input,
                                   ThreadID tid) const
{
    RenameThreadPrepareResult result;
    result.cycle = input.cycle;
    result.tid = tid;

    bool can_rename = true;
    for (int i = 0; i < NumRenameRegClasses; ++i) {
        if (input.demandPhyRegs[tid][i] > input.freePhyRegs[tid][i]) {
            can_rename = false;
            break;
        }
    }

    const bool iew_block = input.iewToRename.block[tid];
    const bool block = iew_block || !can_rename;
    const bool active = !block && !input.fixedbufferEmpty[tid];
    StallReason block_reason = StallReason::NoStall;

    if (iew_block) {
        block_reason = input.iewToRename.reason[tid];
    } else if (!can_rename) {
        block_reason = input.robHeadStallReason[tid];
        if (block_reason == StallReason::NoStall)
            block_reason = input.lqHeadStallReason[tid];
        if (block_reason == StallReason::NoStall)
            block_reason = input.sqHeadStallReason[tid];
        if (block_reason == StallReason::NoStall) {
            block_reason = StallReason::RegFull;
            ++result.regFullEvents;
        }
    }

    result.canRename = can_rename;
    result.iewBlock = iew_block;
    result.block = block;
    result.active = active;
    result.renameBlockReason = block_reason;

    const bool decode_block = block && !input.fixedbufferEmpty[tid];
    result.blocked = decode_block;
    result.decodeBlock = decode_block;
    result.decodeBlockReason =
        decode_block ? block_reason : StallReason::NoStall;

    return result;
}

Rename::RenamePrepareResult
Rename::combineRenameThreadPrepareResults(
        const RenamePrepareInput &input,
        const RenameThreadPrepareResults &thread_results) const
{
    RenamePrepareResult result;
    result.cycle = input.cycle;

    for (int tid = 0; tid < input.numThreads; ++tid) {
        const auto &thread_result = thread_results.byThread[tid];

        result.canRename[tid] = thread_result.canRename;
        result.iewBlock[tid] = thread_result.iewBlock;
        result.block[tid] = thread_result.block;
        result.active[tid] = thread_result.active;
        result.renameBlockReason[tid] = thread_result.renameBlockReason;
        result.decodeBlock[tid] = thread_result.decodeBlock;
        result.decodeBlockReason[tid] = thread_result.decodeBlockReason;
        result.regFullEvents += thread_result.regFullEvents;

        if (thread_result.active) {
            ++result.activeThreads;
            if (result.selectedTid == InvalidThreadID) {
                result.selectedTid = tid;
            } else {
                result.multipleActive = true;
                result.decodeBlock[result.selectedTid] = true;
                result.decodeBlock[tid] = true;
            }
        } else if (thread_result.blocked) {
            ++result.blockedThreads;
            if (result.blockedTid == InvalidThreadID)
                result.blockedTid = tid;
        }
    }

    return result;
}

Rename::RenamePrepareResult
Rename::prepareRenameControl(const RenamePrepareInput &input) const
{
    RenameThreadPrepareResults thread_results;
    for (int tid = 0; tid < input.numThreads; ++tid) {
        thread_results.byThread[tid] =
            prepareRenameThreadControl(input, static_cast<ThreadID>(tid));
    }

    return combineRenameThreadPrepareResults(input, thread_results);
}

unsigned
Rename::applyFutureReleaseDeltas(RenamePrepareInput &input) const
{
    if (releaseSeq >= finalCommitSeq || releaseWidth == 0)
        return 0;

    const InstSeqNum target_seq =
        releaseSeq + releaseWidth < finalCommitSeq ?
        releaseSeq + releaseWidth : finalCommitSeq;
    if (target_seq == releaseSeq)
        return 0;

    int free_delta[NumRenameRegClasses] = {};
    std::vector<std::pair<PhysRegIdPtr, unsigned>> local_release_counts;

    auto note_release = [&local_release_counts](PhysRegIdPtr reg) {
        for (auto &entry : local_release_counts) {
            if (entry.first == reg) {
                ++entry.second;
                return entry.second;
            }
        }

        local_release_counts.emplace_back(reg, 1);
        return 1u;
    };

    unsigned released_regs = 0;
    for (ThreadID tid : *activeThreads) {
        for (auto hb_it = historyBuffer[tid].rbegin();
             hb_it != historyBuffer[tid].rend(); ++hb_it) {
            if (hb_it->instSeqNum > target_seq)
                break;

            PhysRegIdPtr reg = hb_it->prevPhysReg.PhyReg();
            if (hb_it->newPhysReg.PhyReg() == reg ||
                !reg || reg->getRef() == 0 ||
                reg->classValue() == InvalidRegClass) {
                continue;
            }

            const unsigned reg_class =
                static_cast<unsigned>(reg->classValue());
            if (reg_class >= NumRenameRegClasses)
                continue;

            const unsigned releases_seen = note_release(reg);
            if (releases_seen == reg->getRef()) {
                ++free_delta[reg_class];
                ++released_regs;
            }
        }
    }

    if (released_regs == 0)
        return 0;

    for (int tid = 0; tid < input.numThreads; ++tid) {
        for (unsigned reg_class = 0; reg_class < NumRenameRegClasses;
             ++reg_class) {
            input.freePhyRegs[tid][reg_class] += free_delta[reg_class];
        }
    }

    return released_regs;
}

void
Rename::mergeRenamePrepareResult(const RenamePrepareResult &result,
                                 bool countPrepareStats)
{
    lastPrepareResult = result;

    if (countPrepareStats) {
        stats.prepareMerges++;
        stats.prepareActiveThreads += result.activeThreads;
        stats.prepareBlockedThreads += result.blockedThreads;
        if (result.multipleActive)
            stats.prepareMultipleActive++;
    }

    for (unsigned i = 0; i < result.regFullEvents; ++i) {
        ++stats.fullRegistersEvents;
        stats.stallEvents[RegFull]++;
    }
}

bool
Rename::samePrepareResult(const RenamePrepareResult &lhs,
                          const RenamePrepareResult &rhs) const
{
    return futurePrepareMismatchReason(lhs, rhs) ==
           NumRenameFuturePrepareMismatchReasons;
}

unsigned
Rename::futurePrepareMismatchReason(const RenamePrepareResult &lhs,
                                     const RenamePrepareResult &rhs) const
{
    if (lhs.cycle != rhs.cycle ||
        lhs.selectedTid != rhs.selectedTid)
        return lhs.cycle != rhs.cycle ?
            RenameFuturePrepareMismatchCycle :
            RenameFuturePrepareMismatchSelectedTid;
    if (lhs.blockedTid != rhs.blockedTid)
        return RenameFuturePrepareMismatchBlockedTid;
    if (lhs.activeThreads != rhs.activeThreads)
        return RenameFuturePrepareMismatchActiveThreads;
    if (lhs.blockedThreads != rhs.blockedThreads)
        return RenameFuturePrepareMismatchBlockedThreads;
    if (lhs.regFullEvents != rhs.regFullEvents)
        return RenameFuturePrepareMismatchRegFullEvents;
    if (lhs.multipleActive != rhs.multipleActive)
        return RenameFuturePrepareMismatchMultipleActive;

    for (int tid = 0; tid < numThreads; ++tid) {
        if (lhs.active[tid] != rhs.active[tid])
            return RenameFuturePrepareMismatchThreadActive;
        if (lhs.decodeBlock[tid] != rhs.decodeBlock[tid])
            return RenameFuturePrepareMismatchThreadDecodeBlock;
        if (lhs.decodeBlockReason[tid] != rhs.decodeBlockReason[tid])
            return RenameFuturePrepareMismatchThreadDecodeBlockReason;

        const bool thread_observes_rename_control =
            lhs.active[tid] || rhs.active[tid] ||
            lhs.decodeBlock[tid] || rhs.decodeBlock[tid];
        // Empty rename input does not consume the upstream IEW block latch;
        // only the generated Decode backpressure and selected work matter.
        if (!thread_observes_rename_control)
            continue;

        if (lhs.canRename[tid] != rhs.canRename[tid])
            return RenameFuturePrepareMismatchThreadCanRename;
        if (lhs.iewBlock[tid] != rhs.iewBlock[tid])
            return RenameFuturePrepareMismatchThreadIEWBlock;
        if (lhs.block[tid] != rhs.block[tid])
            return RenameFuturePrepareMismatchThreadBlock;
        if (lhs.renameBlockReason[tid] != rhs.renameBlockReason[tid])
            return RenameFuturePrepareMismatchThreadRenameBlockReason;
    }

    return NumRenameFuturePrepareMismatchReasons;
}

bool
Rename::sameFutureCandidateInput(
        const RenamePrepareInput &lhs,
        const RenamePrepareInput &rhs) const
{
    if (lhs.numThreads != rhs.numThreads)
        return false;

    for (int tid = 0; tid < numThreads; ++tid) {
        if (lhs.fixedbufferEmpty[tid] != rhs.fixedbufferEmpty[tid] ||
            lhs.fixedbufferSize[tid] != rhs.fixedbufferSize[tid] ||
            lhs.iewToRename.block[tid] != rhs.iewToRename.block[tid] ||
            lhs.iewToRename.reason[tid] != rhs.iewToRename.reason[tid] ||
            lhs.robHeadStallReason[tid] != rhs.robHeadStallReason[tid] ||
            lhs.lqHeadStallReason[tid] != rhs.lqHeadStallReason[tid] ||
            lhs.sqHeadStallReason[tid] != rhs.sqHeadStallReason[tid]) {
            return false;
        }

        for (int reg_class = 0; reg_class < NumRenameRegClasses;
             ++reg_class) {
            if (lhs.demandPhyRegs[tid][reg_class] !=
                    rhs.demandPhyRegs[tid][reg_class] ||
                lhs.freePhyRegs[tid][reg_class] !=
                    rhs.freePhyRegs[tid][reg_class]) {
                return false;
            }
        }
    }

    return true;
}

void
Rename::recordFutureCandidateInputDifferenceFields(
        const RenamePrepareInput &expected,
        const RenamePrepareInput &actual,
        statistics::Vector &fields) const
{
    if (expected.numThreads != actual.numThreads)
        fields[RenameFutureCandidateInputNumThreads]++;

    bool fixed_empty = false;
    bool fixed_size = false;
    bool demand = false;
    bool free = false;
    bool iew_block = false;
    bool iew_reason = false;
    bool rob_head = false;
    bool lq_head = false;
    bool sq_head = false;

    for (int tid = 0; tid < numThreads; ++tid) {
        fixed_empty = fixed_empty ||
            expected.fixedbufferEmpty[tid] != actual.fixedbufferEmpty[tid];
        fixed_size = fixed_size ||
            expected.fixedbufferSize[tid] != actual.fixedbufferSize[tid];
        iew_block = iew_block ||
            expected.iewToRename.block[tid] !=
                actual.iewToRename.block[tid];
        iew_reason = iew_reason ||
            expected.iewToRename.reason[tid] !=
                actual.iewToRename.reason[tid];
        rob_head = rob_head ||
            expected.robHeadStallReason[tid] !=
                actual.robHeadStallReason[tid];
        lq_head = lq_head ||
            expected.lqHeadStallReason[tid] !=
                actual.lqHeadStallReason[tid];
        sq_head = sq_head ||
            expected.sqHeadStallReason[tid] !=
                actual.sqHeadStallReason[tid];

        for (int reg_class = 0; reg_class < NumRenameRegClasses;
             ++reg_class) {
            demand = demand ||
                expected.demandPhyRegs[tid][reg_class] !=
                    actual.demandPhyRegs[tid][reg_class];
            free = free ||
                expected.freePhyRegs[tid][reg_class] !=
                    actual.freePhyRegs[tid][reg_class];
        }
    }

    if (fixed_empty)
        fields[RenameFutureCandidateInputFixedbufferEmpty]++;
    if (fixed_size)
        fields[RenameFutureCandidateInputFixedbufferSize]++;
    if (demand)
        fields[RenameFutureCandidateInputDemandPhyRegs]++;
    if (free)
        fields[RenameFutureCandidateInputFreePhyRegs]++;
    if (iew_block)
        fields[RenameFutureCandidateInputIEWBlock]++;
    if (iew_reason)
        fields[RenameFutureCandidateInputIEWReason]++;
    if (rob_head)
        fields[RenameFutureCandidateInputRobHeadStall]++;
    if (lq_head)
        fields[RenameFutureCandidateInputLQHeadStall]++;
    if (sq_head)
        fields[RenameFutureCandidateInputSQHeadStall]++;
}

void
Rename::recordFutureCandidateIEWBlockDiffDirections(
        const RenamePrepareInput &expected,
        const RenamePrepareInput &actual,
        bool prepare_match)
{
    bool false_to_true = false;
    bool true_to_false = false;

    for (int tid = 0; tid < numThreads; ++tid) {
        if (!expected.iewToRename.block[tid] &&
            actual.iewToRename.block[tid]) {
            false_to_true = true;
        } else if (expected.iewToRename.block[tid] &&
                   !actual.iewToRename.block[tid]) {
            true_to_false = true;
        }
    }

    if (prepare_match) {
        if (false_to_true) {
            stats.futureCandidateIEWBlockDiffDirections[
                RenameFutureCandidateIEWBlockMatchFalseToTrue]++;
        }
        if (true_to_false) {
            stats.futureCandidateIEWBlockDiffDirections[
                RenameFutureCandidateIEWBlockMatchTrueToFalse]++;
        }
    } else {
        if (false_to_true) {
            stats.futureCandidateIEWBlockDiffDirections[
                RenameFutureCandidateIEWBlockMismatchFalseToTrue]++;
        }
        if (true_to_false) {
            stats.futureCandidateIEWBlockDiffDirections[
                RenameFutureCandidateIEWBlockMismatchTrueToFalse]++;
        }
    }
}

Rename::RenamePrepareResult
Rename::runRenamePrepare(Cycles cycle)
{
    auto input = std::make_shared<RenamePrepareInput>(
            buildRenamePrepareInput(cycle));
    auto result = std::make_shared<RenamePrepareResult>();

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled()) {
        *result = prepareRenameControl(*input);
        mergeRenamePrepareResult(*result, false);
        return *result;
    }

    if (pendingFutureCandidatePrepare.valid) {
        if (pendingFutureCandidatePrepare.result.cycle == cycle) {
            const RenamePrepareResult expected =
                prepareRenameControl(*input);
            const auto &profile = pendingFutureCandidatePrepare.profile;
            const unsigned scheduler_reason = profile.schedulerReason;
            const unsigned pop_bucket = std::min(
                    profile.fixedBufferPops,
                    NumRenameFutureCandidateExpectedPopBuckets - 1);
            const bool input_match = sameFutureCandidateInput(
                    pendingFutureCandidatePrepare.input, *input);
            stats.futureCandidatePrepareChecks++;
            stats.futureCandidateInputChecks++;
            if (input_match) {
                stats.futureCandidateInputMatches++;
            } else {
                stats.futureCandidateInputDifferences++;
                recordFutureCandidateInputDifferenceFields(
                        pendingFutureCandidatePrepare.input, *input,
                        stats.futureCandidateInputDifferenceFields);
            }
            const bool prepare_match = samePrepareResult(
                    pendingFutureCandidatePrepare.result, expected);
            const unsigned input_stability_reason = prepare_match ?
                (input_match ?
                 RenameFutureCandidatePrepareMatchInputMatch :
                 RenameFutureCandidatePrepareMatchInputDiff) :
                (input_match ?
                 RenameFutureCandidatePrepareMismatchInputMatch :
                 RenameFutureCandidatePrepareMismatchInputDiff);
            stats.futureCandidatePrepareInputStability[
                input_stability_reason]++;
            if (prepare_match) {
                stats.futureCandidatePrepareMatches++;
                if (scheduler_reason <
                    NumRenameFutureCandidateSchedulerReasons) {
                    stats.futureCandidatePrepareMatchesBySchedulerReason[
                        scheduler_reason]++;
                }
                stats.futureCandidatePrepareMatchesByExpectedPops[
                    pop_bucket]++;
                if (!input_match) {
                    recordFutureCandidateInputDifferenceFields(
                            pendingFutureCandidatePrepare.input, *input,
                            stats.futureCandidateInputMatchDifferenceFields);
                    recordFutureCandidateIEWBlockDiffDirections(
                            pendingFutureCandidatePrepare.input, *input,
                            true);
                }
            } else {
                stats.futureCandidatePrepareMismatches++;
                const unsigned reason =
                    futurePrepareMismatchReason(
                            pendingFutureCandidatePrepare.result, expected);
                if (reason < NumRenameFuturePrepareMismatchReasons) {
                    stats.futureCandidatePrepareMismatchReasons[reason]++;
                }
                if (scheduler_reason <
                    NumRenameFutureCandidateSchedulerReasons) {
                    stats.futureCandidatePrepareMismatchesBySchedulerReason[
                        scheduler_reason]++;
                }
                stats.futureCandidatePrepareMismatchesByExpectedPops[
                    pop_bucket]++;
                if (!input_match) {
                    recordFutureCandidateInputDifferenceFields(
                            pendingFutureCandidatePrepare.input, *input,
                            stats.futureCandidateInputMismatchDifferenceFields);
                    recordFutureCandidateIEWBlockDiffDirections(
                            pendingFutureCandidatePrepare.input, *input,
                            false);
                }
            }
        } else {
            stats.futureCandidatePrepareChecks++;
            stats.futureCandidatePrepareStale++;
        }
        pendingFutureCandidatePrepare.valid = false;
    }

    if (pendingFuturePrepare.valid) {
        if (pendingFuturePrepare.result.cycle == cycle) {
            *result = pendingFuturePrepare.result;
            pendingFuturePrepare.valid = false;
            stats.futurePrepareReuses++;
            mergeRenamePrepareResult(*result, true);

            if (runtime.selfTestEnabled()) {
                const RenamePrepareResult expected =
                    prepareRenameControl(*input);
                stats.futurePrepareChecks++;
                if (samePrepareResult(*result, expected)) {
                    stats.futurePrepareMatches++;
                } else {
                    stats.futurePrepareMismatches++;
                    const unsigned reason =
                        futurePrepareMismatchReason(*result, expected);
                    if (reason < NumRenameFuturePrepareMismatchReasons)
                        stats.futurePrepareMismatchReasons[reason]++;
                }
            }

            return lastPrepareResult;
        }

        stats.futurePrepareChecks++;
        stats.futurePrepareStale++;
        pendingFuturePrepare.valid = false;
    }

    stats.prepareTasks++;
    auto thread_results = std::make_shared<RenameThreadPrepareResults>();
    for (int tid = 0; tid < input->numThreads; ++tid) {
        if (input->fixedbufferEmpty[tid]) {
            thread_results->byThread[tid] =
                prepareRenameThreadControl(
                        *input, static_cast<ThreadID>(tid));
            stats.prepareInactiveThreads++;
            continue;
        }

        const TaskOrderKey thread_order{
            cycle, TaskStage::Rename, 1, InvalidThreadID,
            static_cast<uint64_t>(tid)};
        runtime.submitWeak(
                thread_order,
                renameWidth,
                [this, input, thread_results, tid] {
                    thread_results->byThread[tid] =
                        prepareRenameThreadControl(
                                *input, static_cast<ThreadID>(tid));
                });
    }

    const TaskOrderKey merge_order{
        cycle, TaskStage::Rename, 1, InvalidThreadID,
        static_cast<uint64_t>(input->numThreads)};
    runtime.submitWeak(
            merge_order,
            0,
            [] {},
            [this, input, thread_results, result] {
                *result = combineRenameThreadPrepareResults(
                        *input, *thread_results);
                mergeRenamePrepareResult(*result, true);
            });
    runtime.waitForOrder(merge_order);

    return lastPrepareResult;
}

bool
Rename::buildFutureDecodeLatchInput(Cycles cycle,
                                    const StallSignalLatch &iew_to_rename,
                                    const DecodeStruct *snapshot_decode,
                                    const TimeStruct *snapshot_iew,
                                    const TimeStruct *snapshot_commit,
                                    RenamePrepareInput &input,
                                    bool count_stats)
{
    if (!snapshot_decode || !snapshot_iew || !snapshot_commit) {
        if (count_stats)
            stats.futureInputSkipReasons[RenameFutureInputMissingSnapshot]++;
        return false;
    }

    if (activeThreads->empty()) {
        if (count_stats)
            stats.futureInputSkipReasons[RenameFutureInputNoActiveThreads]++;
        return false;
    }

    for (ThreadID tid : *activeThreads) {
        const auto &commit_info = snapshot_commit->commitInfo[tid];
        unsigned reason = NumRenameFutureInputCommitControlReasons;
        if (commit_info.squash) {
            reason = RenameFutureInputCommitSquash;
        } else if (commit_info.robSquashing) {
            reason = RenameFutureInputCommitRobSquashing;
        } else if (commit_info.doneSeqNum != 0) {
            reason = RenameFutureInputCommitDoneSeqNum;
        }

        if (reason != NumRenameFutureInputCommitControlReasons) {
            if (count_stats) {
                stats.futureInputSkipReasons[RenameFutureInputCommitControl]++;
                stats.futureInputCommitControlReasons[reason]++;
            }
            return false;
        }
    }

    input = buildRenamePrepareInput(
            cycle, &iew_to_rename, snapshot_decode, snapshot_iew);
    if (releaseSeq != finalCommitSeq) {
        const unsigned released_regs = applyFutureReleaseDeltas(input);
        if (count_stats && released_regs != 0) {
            stats.futureInputVirtualReleaseSteps++;
            stats.futureInputVirtualReleaseRegs += released_regs;
        }
    }
    return true;
}

bool
Rename::previewFutureDecodeLatch(const RenamePrepareInput &input,
                                 StallSignalLatch &rename_to_decode,
                                 RenamePrepareResult *prepare_result) const
{
    const RenamePrepareResult result = prepareRenameControl(input);
    if (prepare_result)
        *prepare_result = result;

    if (result.selectedTid != InvalidThreadID)
        return false;

    rename_to_decode.clear();
    for (int tid = 0; tid < numThreads; ++tid) {
        rename_to_decode.block[tid] = result.decodeBlock[tid];
        rename_to_decode.reason[tid] = result.decodeBlockReason[tid];
    }

    return true;
}

bool
Rename::previewFutureDecodeLatch(Cycles cycle,
                                 const StallSignalLatch &iew_to_rename,
                                 const DecodeStruct *snapshot_decode,
                                 const TimeStruct *snapshot_iew,
                                 const TimeStruct *snapshot_commit,
                                 StallSignalLatch &rename_to_decode,
                                 RenamePrepareResult *prepare_result)
{
    RenamePrepareInput input;
    if (!buildFutureDecodeLatchInput(
                cycle, iew_to_rename, snapshot_decode, snapshot_iew,
                snapshot_commit, input)) {
        return false;
    }

    return previewFutureDecodeLatch(input, rename_to_decode, prepare_result);
}

Rename::RenamePrepareResult
Rename::previewFuturePrepare(const RenamePrepareInput &input) const
{
    return prepareRenameControl(input);
}

void
Rename::recordFuturePreviewSkipped(const RenamePrepareResult &result)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    if (result.multipleActive) {
        stats.futurePreviewSkipReasons[RenameFuturePreviewMultipleActive]++;
    } else {
        stats.futurePreviewSkipReasons[RenameFuturePreviewActiveRename]++;
    }
}

void
Rename::recordFuturePrepareProbe()
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    stats.futurePrepareProbes++;
}

void
Rename::recordFuturePrepareSkipped()
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    stats.futurePrepareSkipped++;
}

void
Rename::setPendingFuturePrepare(const RenamePrepareResult &result)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    if (pendingFuturePrepare.valid)
        stats.futurePrepareStale++;

    pendingFuturePrepare.result = result;
    pendingFuturePrepare.valid = true;
    stats.futurePrepareMerges++;
}

void
Rename::setPendingFutureCandidatePrepare(
        const RenamePrepareResult &result,
        const FutureCandidatePrepareProfile &profile,
        const RenamePrepareInput &input)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    if (pendingFutureCandidatePrepare.valid &&
        pendingFutureCandidatePrepare.result.cycle != result.cycle) {
        stats.futureCandidatePrepareStale++;
    }

    pendingFutureCandidatePrepare.result = result;
    pendingFutureCandidatePrepare.profile = profile;
    pendingFutureCandidatePrepare.input = input;
    pendingFutureCandidatePrepare.valid = true;
}

void
Rename::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to read information from time buffer, from IEW stage.
    fromIEW = timeBuffer->getWire(-iewToRenameDelay);

    // Setup wire to read infromation from time buffer, from commit stage.
    fromCommit = timeBuffer->getWire(-commitToRenameDelay);

    // Setup wire to write information to previous stages.
    toDecode = timeBuffer->getWire(0);
}

void
Rename::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to write information to future stages.
    toIEW = renameQueue->getWire(0);
}

void
Rename::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // Setup wire to get information from decode.
    fromDecode = decodeQueue->getWire(-decodeToRenameDelay);
}

void
Rename::startupStage()
{
    resetStage();
}

void
Rename::clearStates(ThreadID tid)
{
    stalls[tid].iew = false;
}

void
Rename::resetStage()
{
    _status = Inactive;
    resumeUnblocking = false;

    // Grab the number of free entries directly from the stages.
    for (ThreadID tid = 0; tid < numThreads; tid++) {

        stalls[tid].iew = false;
    }
}

void
Rename::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}


void
Rename::setRenameMap(UnifiedRenameMap rm_ptr[MaxThreads])
{
    for (ThreadID tid = 0; tid < numThreads; tid++)
        renameMap[tid] = &rm_ptr[tid];
}

void
Rename::setFreeList(UnifiedFreeList *fl_ptr)
{
    freeList = fl_ptr;
}

void
Rename::setScoreboard(Scoreboard *_scoreboard)
{
    scoreboard = _scoreboard;
}

bool
Rename::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!historyBuffer[tid].empty() ||
            !fixedbuffer[tid].empty())
            return false;
    }
    return true;
}

void
Rename::takeOverFrom()
{
    resetStage();
}

void
Rename::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        assert(historyBuffer[tid].empty());
        assert(fixedbuffer[tid].empty());
    }
}

void
Rename::squash(const InstSeqNum &squash_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] [squash sn:%llu] Squashing instructions.\n",
        tid,squash_seq_num);

    fixedbuffer[tid].clear();

    doSquash(squash_seq_num, tid);
}

void
Rename::tick()
{
    const DecodeStruct *decode_input = decodeInput(cpu->curCycle());
    const TimeStruct *iew_input = iewInput(cpu->curCycle());
    const TimeStruct *commit_input = commitInput(cpu->curCycle());

    toIEW->fetchStallReason = decode_input->fetchStallReason;
    toIEW->decodeStallReason = decode_input->decodeStallReason;

    wroteToTimeBuffer = false;
    blockReason = StallReason::NoStall;
    setAllStalls(StallReason::NoStall);

    moveInstsToBuffer(decode_input);

    checkSquash(commit_input);

    releasePhysRegs(commit_input);

    const RenamePrepareResult prepare = runRenamePrepare(cpu->curCycle());
    ThreadID tid = prepare.selectedTid;
    ThreadID blocked_tid = prepare.blockedTid;
    for (int i = 0; i < numThreads; i++) {
        DPRINTF(Rename,
                "[tid:%i] blockRename: %i, canRename: %i, block: %i, "
                "active: %i\n",
                i, prepare.iewBlock[i], prepare.canRename[i],
                prepare.block[i], prepare.active[i]);
        setDecodeStall(i, prepare.decodeBlock[i],
                       prepare.decodeBlockReason[i]);
        toDecode->renameInfo[i].blockReason =
            prepare.decodeBlockReason[i];
    }
    if (prepare.multipleActive) {
        DPRINTF(Rename,
                "Multiple active threads detected, blocking all threads\n");
    }

    if (tid == InvalidThreadID) {
        // all threads are stalled, no need to process
        if (blocked_tid != InvalidThreadID) {
            const StallReason decode_block_reason =
                prepare.decodeBlockReason[blocked_tid];
            setAllStalls(decode_block_reason);
            blockReason = decode_block_reason;
        }
        toIEW->renameStallReason = renameStalls;
        updateActivate();
        return;
    }
    DPRINTF(Rename, "Processing [tid:%i]\n", tid);

    renameInsts(tid, iew_input);
    if (prepare.iewBlock[tid]) {
        setAllStalls(prepare.renameBlockReason[tid]);
    } else if (toIEW->size > 0 && renameStalls[0] == StallReason::NoStall) {
        for (int i = 0; i < renameStalls.size(); i++) {
            if (i < toIEW->size) {
                renameStalls.at(i) = StallReason::NoStall;
            } else {
                renameStalls.at(i) = decode_input->decodeStallReason.at(i);
            }
        }
    }

    const bool decode_block =
        stallSignalBank ?
        cpu->stallSignalSnapshotOrCurrent(
            cpu->curCycle(), StallSignalEdge::RenameToDecode)
            .block[tid] :
        stallSig->blockDecode[tid];
    const StallReason decode_block_reason =
        decode_block ? blockReason : StallReason::NoStall;
    setDecodeStall(tid, decode_block, decode_block_reason);
    toDecode->renameInfo[tid].blockReason = decode_block_reason;

    toIEW->renameStallReason = renameStalls;

    updateActivate();

    if (wroteToTimeBuffer || releaseSeq < finalCommitSeq) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
Rename::releasePhysRegs(const TimeStruct *commit_input)
{
    assert(commit_input);

    // Release physical registers up to releaseWidth
    auto threads = activeThreads->begin();
    if (releaseSeq + releaseWidth < finalCommitSeq) {
        releaseSeq += releaseWidth;
    } else {
        releaseSeq = finalCommitSeq;
    }
    while (threads != activeThreads->end()) {
        ThreadID tid = *threads++;

        removeFromHistory(releaseSeq, tid);
        // If we committed this cycle then doneSeqNum will be > 0
        if (commit_input->commitInfo[tid].doneSeqNum != 0 &&
            !commit_input->commitInfo[tid].squash) {

            finalCommitSeq = commit_input->commitInfo[tid].doneSeqNum;
            releaseSeq = historyBuffer->empty() ? 0 : historyBuffer[tid].back().instSeqNum;
        }
    }
}

bool
Rename::canRename(ThreadID tid)
{
    std::vector<int> demand_phy_regs(RMiscRegClass + 1, 0);
    auto& insts_to_rename = fixedbuffer[tid];
    int num_insts = insts_to_rename.size();

    // calculate physical registers needed by these `num_insts` instructions
    for (int i = 0; i < num_insts; i++) {
        DynInstPtr inst = insts_to_rename.at(i);
        if (inst->isSquashed()) {
            continue;
        }

        for (int j = 0; j < RMiscRegClass + 1 ; j++) {
            demand_phy_regs[j] += inst->numDestRegs((RegClassType)j);
        }
    }

    // if total demand registers are less than renameWidth,
    // then set it to renameWidth.
    // In actual hardware, due to timing constraints,
    // we can only evaluate whether rename can be down in the worst-case scenario.
    for (int i = 0; i < RMiscRegClass + 1; i++) {
        switch (i) {
            case IntRegClass:
            case FloatRegClass:
            case VecRegClass:
            case RMiscRegClass:
                demand_phy_regs[i] = std::max(demand_phy_regs[i], (int)renameWidth);
                break;
            default:
                break;
        }
    }

    // check if the demand registers can be satisfied or not
    for (int i = 0; i < RMiscRegClass + 1; i++) {
        if (demand_phy_regs[i] > renameMap[tid]->numFreeEntries((RegClassType)i)) {
            DPRINTF(Rename,
                    "[tid:%i] Cannot rename because demand for %s physical "
                    "registers is %i, but only %i are free.\n",
                    tid, RegId((RegClassType)i, 0).className(), demand_phy_regs[i],
                    renameMap[tid]->numFreeEntries((RegClassType)i));
            return false;
        }
    }
    return true;
}

void
Rename::renameInsts(ThreadID tid, const TimeStruct *iew_input)
{
    assert(iew_input);

    // Instructions can be either in the skid buffer or the queue of
    // instructions coming from decode, depending on the status.
    auto& insts_to_rename = fixedbuffer[tid];
    int insts_available = insts_to_rename.size();

    int renamed_insts = 0;
    int toIEWIndex = 0;

    std::queue<StallReason> rename_stalls;

    StallReason breakRename = StallReason::NoStall;
    while (insts_available > 0) {

        assert(!insts_to_rename.empty());

        DynInstPtr inst = insts_to_rename.front();

        insts_to_rename.pop_front();

        if (inst->isSquashed()) {
            DPRINTF(Rename, "[sn:%llu] instruction  with PC %s is squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;

            // Decrement how many instructions are available.
            --insts_available;

            rename_stalls.push(StallReason::InstSquashed);

            continue;
        }

        assert(renameMap[tid]->canRename(inst));

        DPRINTF(Rename, "[tid:%i] [sn:%llu] Renaming instruction with PC %s.\n",
            tid, inst->seqNum, inst->pcState());

        renameSrcRegs(inst, inst->threadNumber);

        renameDestRegs(inst, inst->threadNumber);

        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtRename);

        ++renamed_insts;
        // Notify potential listeners that source and destination registers for
        // this instruction have been renamed.
        ppRename->notify(inst);

        // Put instruction in rename queue.
        toIEW->insts[toIEWIndex] = inst;
        ++(toIEW->size);

        // Increment which instruction we're on.
        ++toIEWIndex;

        // Decrement how many instructions are available.
        --insts_available;
    }

    // Check if there's any instructions left that haven't yet been renamed.
    // If so then block.
    if (!fixedbuffer[tid].empty()) {
        setDecodeBlock(tid, true);
        if (breakRename == StallReason::NoStall) {
            breakRename = checkRenameStallFromIEW(tid, iew_input);
            if (breakRename == StallReason::NoStall) {
                breakRename = StallReason::RegFull;
                ++stats.fullRegistersEvents;
                stats.stallEvents[RegFull]++;
            }
        }
        blockReason = breakRename;
        DPRINTF(Rename, "[tid:%i] Stalling because there are still instructions to "
                "rename.\n", tid);
    }

    if (!rename_stalls.empty()) {
        setAllStalls(rename_stalls.front());
        rename_stalls.pop();
    } else if (breakRename != StallReason::NoStall) {
        setAllStalls(breakRename);
    }
    stats.renamedInsts += renamed_insts;

    // If we wrote to the time buffer, record this.
    if (toIEWIndex) {
        wroteToTimeBuffer = true;
    }
}

void
Rename::moveInstsToBuffer(const DecodeStruct *decode_input)
{
    assert(decode_input);

    int insts_from_decode = decode_input->size;
    if (insts_from_decode == 0) {
        return;
    }
    ThreadID tid = decode_input->insts[0]->threadNumber;
    for (int i = 0; i < insts_from_decode; ++i) {
        const DynInstPtr &inst = decode_input->insts[i];
        assert(inst->threadNumber == tid);
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();
        } else {
            assert(!fixedbuffer[tid].full());
            fixedbuffer[tid].push_back(inst);
        }

#if TRACING_ON
        if (debug::O3PipeView) {
            inst->renameTick = curTick() - inst->fetchTick;
        }
#endif
    }
}

const DecodeStruct *
Rename::decodeInput(Cycles cycle) const
{
    const int decode_to_rename_offset = -static_cast<int>(
            static_cast<uint64_t>(decodeToRenameDelay));
    const DecodeStruct *snapshot =
        cpu->pipelineInputDecodeToRename(cycle, decode_to_rename_offset);
    return snapshot ? snapshot : &(*fromDecode);
}

const TimeStruct *
Rename::iewInput(Cycles cycle) const
{
    const int iew_to_rename_offset = -static_cast<int>(
            static_cast<uint64_t>(iewToRenameDelay));
    const TimeStruct *snapshot =
        cpu->pipelineInputBackward(cycle, iew_to_rename_offset);
    return snapshot ? snapshot : &(*fromIEW);
}

const TimeStruct *
Rename::commitInput(Cycles cycle) const
{
    const int commit_to_rename_offset = -static_cast<int>(
            static_cast<uint64_t>(commitToRenameDelay));
    const TimeStruct *snapshot =
        cpu->pipelineInputBackward(cycle, commit_to_rename_offset);
    return snapshot ? snapshot : &(*fromCommit);
}

void
Rename::checkSquash(const TimeStruct *commit_input)
{
    assert(commit_input);

    for (int i = 0; i < numThreads; i++) {
        if (commit_input->commitInfo[i].squash) {
            DPRINTF(Rename, "[tid:%i] Squashing instructions due to squash from "
                    "commit.\n", i);

            squash(commit_input->commitInfo[i].doneSeqNum, i);

            localSquashVer.update(
                    commit_input->commitInfo[i].squashVersion.getVersion());
            DPRINTF(Rename, "Updating squash version to %u\n",
                    localSquashVer.getVersion());
        }
    }
}

void
Rename::updateActivate()
{
    bool any_unblocking = true;

    // Rename will have activity if it's unblocking.
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::RenameIdx);
        }
    } else {
        // If it's not unblocking, then rename will not have any internal
        // activity.  Switch it to inactive.
        if (_status == Active) {
            _status = Inactive;
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::RenameIdx);
        }
    }
}

void
Rename::tryFreePReg(PhysRegIdPtr preg)
{
    const auto preg_idx = preg->flatIndex();
    if (preg->getRef() == 0 || preg->classValue() == InvalidRegClass) {
        return;
    }

    preg->decRef();
    if (preg->getRef() == 0) {
        // Put the renamed physical register back on the free list.
        DPRINTF(Rename, "Really free up p%i on squash with ref=%i\n", preg_idx,
                preg->getRef());
        freeList->addReg(preg);
    } else {
        DPRINTF(Rename, "Not to free up p%i on squash for ref=%i\n",
                preg->flatIndex(), preg->getRef());
    }
}

void
Rename::doSquash(const InstSeqNum &squashed_seq_num, ThreadID tid)
{
    auto hb_it = historyBuffer[tid].begin();

    // After a syscall squashes everything, the history buffer may be empty
    // but the ROB may still be squashing instructions.
    // Go through the most recent instructions, undoing the mappings
    // they did and freeing up the registers.
    while (!historyBuffer[tid].empty() &&
           hb_it->instSeqNum > squashed_seq_num) {
        assert(hb_it != historyBuffer[tid].end());

        DPRINTF(Rename,
                "[tid:%i] Removing history entry with sequence "
                "number %i (archReg: %d, newPhysReg: %s, prevPhysReg: %s).\n",
                tid, hb_it->instSeqNum, hb_it->archReg.index(),
                hb_it->newPhysReg.toString(),
                hb_it->prevPhysReg.toString());

        // Undo the rename mapping only if it was really a change.
        // Special regs that are not really renamed (like misc regs
        // and the zero reg) can be recognized because the new mapping
        // is the same as the old one.  While it would be merely a
        // waste of time to update the rename table, we definitely
        // don't want to put these on the free list.
        if (hb_it->newPhysReg != hb_it->prevPhysReg) {
            // Tell the rename map to set the architected register to the
            // previous physical register that it was renamed to.
            renameMap[tid]->setEntry(hb_it->archReg, hb_it->prevPhysReg);
            if (hb_it->newPhysReg.PhyReg() != hb_it->prevPhysReg.PhyReg()) {
                tryFreePReg(hb_it->newPhysReg.PhyReg());
            }
        }

        // Notify potential listeners that the register mapping needs to be
        // removed because the instruction it was mapped to got squashed. Note
        // that this is done before hb_it is incremented.
        ppSquashInRename->notify(std::make_pair(hb_it->instSeqNum,
                                                hb_it->newPhysReg.PhyReg()));

        historyBuffer[tid].erase(hb_it++);

        ++stats.undoneMaps;
    }
}

void
Rename::removeFromHistory(InstSeqNum inst_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Removing a committed instruction from the "
            "history buffer %u (size=%i), until [sn:%llu].\n",
            tid, tid, historyBuffer[tid].size(), inst_seq_num);

    auto hb_it = historyBuffer[tid].end();

    --hb_it;

    if (historyBuffer[tid].empty()) {
        DPRINTF(Rename, "[tid:%i] History buffer is empty.\n", tid);
        return;
    } else if (hb_it->instSeqNum > inst_seq_num) {
        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Old sequence number encountered. "
                "Ensure that a syscall happened recently.\n",
                tid,inst_seq_num);
        return;
    }

    // Commit all the renames up until (and including) the committed sequence
    // number. Some or even all of the committed instructions may not have
    // rename histories if they did not have destination registers that were
    // renamed.
    while (!historyBuffer[tid].empty() &&
           hb_it != historyBuffer[tid].end() &&
           hb_it->instSeqNum <= inst_seq_num) {

        DPRINTF(Rename,
                "[tid:%i] try to free up older rename of reg %s (%s), "
                "[sn:%llu].\n",
                tid, hb_it->prevPhysReg.toString(),
                hb_it->prevPhysReg.PhyReg()->className(),
                hb_it->instSeqNum);


        // Don't free special phys regs like misc and zero regs, which
        // can be recognized because the new mapping is the same as
        // the old one.
        if (hb_it->newPhysReg.PhyReg() != hb_it->prevPhysReg.PhyReg()) {
            tryFreePReg(hb_it->prevPhysReg.PhyReg());
        }

        ++stats.committedMaps;

        historyBuffer[tid].erase(hb_it--);
    }
}

void
Rename::renameSrcRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_src_regs = inst->numSrcRegs();

    // Get the architectual register numbers from the source and
    // operands, and redirect them to the right physical register.
    for (int src_idx = 0; src_idx < num_src_regs; src_idx++) {
        const RegId& src_reg = inst->srcRegIdx(src_idx);
        VirtRegId renamed_reg;

        renamed_reg = map->lookup(tc->flattenRegId(src_reg));
        switch (src_reg.classValue()) {
          case InvalidRegClass:
            break;
          case IntRegClass:
            stats.intLookups++;
            break;
          case FloatRegClass:
            stats.fpLookups++;
            break;
          case VecRegClass:
          case VecElemClass:
            stats.vecLookups++;
            break;
          case VecPredRegClass:
            stats.vecPredLookups++;
            break;
          case CCRegClass:
          case RMiscRegClass:
          case MiscRegClass:
            break;

          default:
            panic("Invalid register class: %d.", src_reg.classValue());
        }

        DPRINTF(Rename,
                "[tid:%i] "
                "Looking up %s arch reg x%i, got %s\n",
                tid, src_reg.className(), src_reg.index(),
                renamed_reg.toString());

        inst->renameSrcReg(src_idx, renamed_reg);

        // See if the register is ready or not.
        if (scoreboard->getReg(renamed_reg.PhyReg())) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is ready.\n",
                    tid, renamed_reg.PhyReg()->index(), renamed_reg.PhyReg()->flatIndex(),
                    renamed_reg.PhyReg()->className());

            inst->markSrcRegReady(src_idx);
        } else {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is not ready.\n",
                    tid, renamed_reg.PhyReg()->index(), renamed_reg.PhyReg()->flatIndex(),
                    renamed_reg.PhyReg()->className());
        }

        ++stats.lookups;
    }
}

void
Rename::renameDestRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_dest_regs = inst->numDestRegs();

    // Rename the destination registers.
    for (int dest_idx = 0; dest_idx < num_dest_regs; dest_idx++) {
        const RegId& dest_reg = inst->destRegIdx(dest_idx);
        UnifiedRenameMap::RenameInfo rename_result;

        RegId flat_dest_regid = tc->flattenRegId(dest_reg);
        flat_dest_regid.setNumPinnedWrites(dest_reg.getNumPinnedWrites());

        VirtRegId bypass_reg;
        bool inc_ref_of_last_dest_phy_reg = false;
        if (cpu->enableMoveElimination && inst->isMov()) {
            // Move elimination
            bypass_reg =
                map->lookup(tc->flattenRegId(inst->srcRegIdx(0)));
            DPRINTF(Rename, "Find the last reg p%i renamed for mv x%i, x%i\n",
                    bypass_reg.PhyReg()->flatIndex(), dest_reg.index(),
                    inst->srcRegIdx(0).index());
            inc_ref_of_last_dest_phy_reg = true;
            inst->setEmptyMov();
            DPRINTF(Rename, "[sn:%llu] Inst is nop: %i, is move: %i\n", inst->seqNum, inst->isNop(),
                    inst->isMov());
            stats.moveEliminated++;
        } else if (inst->isAddImm() &&
            (cpu->enableConstantFolding || (cpu->enableMovImmElimination && inst->srcRegIdx(0).isZeroReg()))) {
            // Constant folding
            bypass_reg =
                map->lookup(tc->flattenRegId(inst->srcRegIdx(0)));

            if (!bypass_reg.IEOper() || bypass_reg.IEOper()->type == IEOperand::Type::ADD) {
                if (bypass_reg.IEOper()) {
                    IEOperPtr ie_op = new IEOperand(IEOperand::Type::ADD,
                        bypass_reg.IEOper()->imm + inst->staticInst->getImm());
                    bypass_reg.setIEOper(ie_op);
                } else {
                    IEOperPtr ie_op = new IEOperand(IEOperand::Type::ADD, inst->staticInst->getImm());
                    bypass_reg.setIEOper(ie_op);
                }
                inc_ref_of_last_dest_phy_reg = true;

                inst->setConstantFolded();
                DPRINTF(Rename, "[sn:%llu] Inst constant folded, virtRegId: %s\n", inst->seqNum, bypass_reg.toString());
                stats.constantFolded++;
            }
        }

        rename_result = map->rename(flat_dest_regid, bypass_reg);

        inst->flattenedDestIdx(dest_idx, flat_dest_regid);

        if (!inc_ref_of_last_dest_phy_reg) {
            scoreboard->unsetReg(rename_result.first.PhyReg());
        }

        DPRINTF(Rename, "[tid:%i] %s arch reg x%i (%s) from %s to %s.\n", tid,
                inc_ref_of_last_dest_phy_reg ? "Mov" : "Rename",
                dest_reg.index(), dest_reg.className(),
                rename_result.second.toString(),
                rename_result.first.toString());

        // Record the rename information so that a history can be kept.
        RenameHistory hb_entry(inst->seqNum, flat_dest_regid,
                               rename_result.first,
                               rename_result.second);

        historyBuffer[tid].push_front(hb_entry);

        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Adding instruction to history buffer (size=%i).\n",
                tid,(*historyBuffer[tid].begin()).instSeqNum,
                historyBuffer[tid].size());

        // Tell the instruction to rename the appropriate destination
        // register (dest_idx) to the new physical register
        // (rename_result.first), and record the previous physical
        // register that the same logical register was renamed to
        // (rename_result.second).
        inst->renameDestReg(dest_idx,
                            rename_result.first,
                            rename_result.second);

        ++stats.renamedOperands;

        if (valuePred) {
            if (num_dest_regs != 1 || !inst->canLVP()) {
                inst->vpSupported = false;
                inst->vpResult.speculative = false;
                inst->vpResult.value = 0xdeadbeefULL;
                continue;
            }

            inst->vpSupported = true;
            if (inst->vpResult.speculative) {
                if (!enableSelectiveVPFlush) {
                    // old behavior: let back-to-back rename consumers see ready
                    scoreboard->setReg(rename_result.first.PhyReg());
                }
                inst->setRegOperand(inst->staticInst.get(), 0, inst->vpResult.value);
                // must pop result here
                inst->popResult();
                DPRINTF(Rename,
                        "Rename-Stage instruction[%s] generate "
                        "prediction value."
                        "seq num: %lu pc: %lX "
                        "prediction value: %lu \n",
                        inst->staticInst->disassemble(inst->getPC()),
                        inst->seqNum, inst->getPC(), inst->vpResult.value);
            }
        }
    }
}

void
Rename::incrFullStat(const FullSource &source)
{
    switch (source) {
      case ROB:
        ++stats.ROBFullEvents;
        stats.stallEvents[ROBFull]++;
        break;
      case IQ:
        ++stats.IQFullEvents;
        stats.stallEvents[IQFull]++;
        break;
      case LQ:
        ++stats.LQFullEvents;
        stats.stallEvents[LSQFull]++;
        break;
      case SQ:
        ++stats.SQFullEvents;
        stats.stallEvents[LSQFull]++;
        break;
      default:
        panic("Rename full stall stat should be incremented for a reason!");
        break;
    }
}

void
Rename::dumpHistory()
{
    std::list<RenameHistory>::iterator buf_it;

    for (ThreadID tid = 0; tid < numThreads; tid++) {

        buf_it = historyBuffer[tid].begin();

        while (buf_it != historyBuffer[tid].end()) {
            cprintf("Seq num: %i\nArch reg[%s]: %i New phys reg:"
                    " %i[%s] Old phys reg: %i[%s]\n",
                    (*buf_it).instSeqNum,
                    (*buf_it).archReg.className(),
                    (*buf_it).archReg.index(),
                    (*buf_it).newPhysReg.PhyReg()->index(),
                    (*buf_it).newPhysReg.PhyReg()->className(),
                    (*buf_it).prevPhysReg.PhyReg()->index(),
                    (*buf_it).prevPhysReg.PhyReg()->className());

            buf_it++;
        }
    }
}

void
Rename::setAllStalls(StallReason renameStall)
{
    for (int i = 0;i < renameStalls.size();i++) {
        renameStalls.at(i) = renameStall;
    }
}

StallReason
Rename::checkRenameStallFromIEW(ThreadID tid, const TimeStruct *iew_input)
{
    assert(iew_input);

    StallReason robHeadStallReason =
        iew_input->iewInfo[tid].robHeadStallReason;
    if (robHeadStallReason != StallReason::NoStall) {
        return robHeadStallReason;
    }

    StallReason lqHeadStallReason =
        iew_input->iewInfo[tid].lqHeadStallReason;
    if (lqHeadStallReason != StallReason::NoStall) {
        return lqHeadStallReason;
    }

    StallReason sqHeadStallReason =
        iew_input->iewInfo[tid].sqHeadStallReason;
    if (sqHeadStallReason != StallReason::NoStall) {
        return sqHeadStallReason;
    }

    return StallReason::NoStall;
}

} // namespace o3
} // namespace gem5
