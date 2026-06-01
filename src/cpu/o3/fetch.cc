/*
 * Copyright (c) 2010-2014 ARM Limited
 * Copyright (c) 2012-2013 AMD
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

#include "cpu/o3/fetch.hh"

#include <algorithm>
#include <cstring>
#include <list>
#include <map>
#include <memory>
#include <queue>

#include "arch/generic/tlb.hh"
#include "arch/riscv/decoder.hh"
#include "arch/riscv/pcstate.hh"
#include "base/debug_helper.hh"
#include "base/random.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/base.hh"
#include "cpu/exetrace.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/trace/TraceFetch.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/Fetch.hh"
#include "debug/FetchFault.hh"
#include "debug/FetchVerbose.hh"
#include "debug/O3CPU.hh"
#include "debug/O3PipeView.hh"
#include "debug/TraceReader.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

namespace
{

const char *
futureDecodeQueueInputSkipReasonName(
        Fetch::FutureDecodeQueueInputSkipReason reason)
{
    using Reason = Fetch::FutureDecodeQueueInputSkipReason;
    switch (reason) {
      case Reason::MissingSnapshot:
        return "MissingSnapshot";
      case Reason::NoActiveThreads:
        return "NoActiveThreads";
      case Reason::CommitControl:
        return "CommitControl";
      case Reason::DecodeControl:
        return "DecodeControl";
      case Reason::AllBlockedNoTid:
        return "AllBlockedNoTid";
      case Reason::FetchQueueNotReady:
        return "FetchQueueNotReady";
      case Reason::MissingInst:
        return "MissingInst";
      case Reason::NumReasons:
        break;
    }

    return "Unknown";
}

const char *
futureQueueNotReadyOutcomeName(Fetch::FutureQueueNotReadyOutcome outcome)
{
    using Outcome = Fetch::FutureQueueNotReadyOutcome;
    switch (outcome) {
      case Outcome::NoSupplyStillNotReady:
        return "NoSupplyStillNotReady";
      case Outcome::PartialSupply:
        return "PartialSupply";
      case Outcome::FilledToWidth:
        return "FilledToWidth";
      case Outcome::Blocked:
        return "Blocked";
      case Outcome::QueueShrank:
        return "QueueShrank";
      case Outcome::Stale:
        return "Stale";
      case Outcome::NumOutcomes:
        break;
    }

    return "Unknown";
}

const char *
futureQueueNotReadyStateName(Fetch::FutureQueueNotReadyState state)
{
    using State = Fetch::FutureQueueNotReadyState;
    switch (state) {
      case State::DecodeBlocked:
        return "DecodeBlocked";
      case State::FrontendNotReady:
        return "FrontendNotReady";
      case State::CacheAccessComplete:
        return "CacheAccessComplete";
      case State::CachePending:
        return "CachePending";
      case State::FetchNotRunning:
        return "FetchNotRunning";
      case State::WaitForVsetvl:
        return "WaitForVsetvl";
      case State::InterruptBlocked:
        return "InterruptBlocked";
      case State::FetchControlNotReady:
        return "FetchControlNotReady";
      case State::ReadyBuffered:
        return "ReadyBuffered";
      case State::ReadyNeedsCacheLine:
        return "ReadyNeedsCacheLine";
      case State::ReadyOther:
        return "ReadyOther";
      case State::NumStates:
        break;
    }

    return "Unknown";
}

const char *
futureToDecodePrepareMismatchReasonName(
        Fetch::FutureToDecodePrepareMismatchReason reason)
{
    using Reason = Fetch::FutureToDecodePrepareMismatchReason;
    switch (reason) {
      case Reason::Cycle:
        return "Cycle";
      case Reason::SelectedTid:
        return "SelectedTid";
      case Reason::AllThreadsBlocked:
        return "AllThreadsBlocked";
      case Reason::SelectedBlocked:
        return "SelectedBlocked";
      case Reason::InstsToDecode:
        return "InstsToDecode";
      case Reason::FetchBubbles:
        return "FetchBubbles";
      case Reason::FetchBubblesMax:
        return "FetchBubblesMax";
      case Reason::DecodeStalls:
        return "DecodeStalls";
      case Reason::WroteToTimeBuffer:
        return "WroteToTimeBuffer";
      case Reason::StallReason:
        return "StallReason";
      case Reason::NumReasons:
        break;
    }

    return "Unknown";
}

} // namespace

Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
        RequestPort(_cpu->name() + ".icache_port", _cpu), fetch(_fetch)
{}


Fetch::Fetch(CPU *_cpu, const BaseO3CPUParams &params)
    : fetchPolicy(params.smtFetchPolicy),
      cpu(_cpu),
      branchPred(nullptr),
      resolveQueueSize(params.resolveQueueSize),
      decodeToFetchDelay(params.decodeToFetchDelay),
      renameToFetchDelay(params.renameToFetchDelay),
      iewToFetchDelay(params.iewToFetchDelay),
      commitToFetchDelay(params.commitToFetchDelay),
      fetchWidth(params.fetchWidth),
      decodeWidth(params.decodeWidth),
      retryPkt(),
      retryTid(InvalidThreadID),
      cacheBlkSize(cpu->cacheLineSize()),
      fetchBufferSize(params.fetchBufferSize),
      fetchQueueSize(params.fetchQueueSize),
      numThreads(params.numThreads),
      numFetchingThreads(params.smtNumFetchingThreads),
      icachePort(this, _cpu),
      finishTranslationEvent(this), fetchStats(_cpu, this),
      valuePred(params.valuePred)
{
    if (numThreads > MaxThreads)
        fatal("numThreads (%d) is larger than compiled limit (%d),\n"
              "\tincrease MaxThreads in src/cpu/o3/limits.hh\n",
              numThreads, static_cast<int>(MaxThreads));
    if (fetchWidth > MaxWidth)
        fatal("fetchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             fetchWidth, static_cast<int>(MaxWidth));

    for (int i = 0; i < MaxThreads; i++) {
        setThreadStatus(i, Idle);
        decoder[i] = nullptr;
        threads[i].fetchpc.reset(params.isa[0]->newPCState());
        macroop[i] = nullptr;
        delayedCommit[i] = false;
        lastIcacheStall[i] = 0;
    }

    branchPred = params.branchPred;

    // This fetch implementation only supports the decoupled frontend with the
    // decoupled BTB predictor. Fail fast to avoid silently using legacy paths.
    assert(branchPred);
    assert(branchPred->isDecoupled());
    assert(branchPred->isBTB());

    dbpbtb =
        dynamic_cast<branch_prediction::btb_pred::DecoupledBPUWithBTB*>(
            branchPred);
    assert(dbpbtb);
    dbpbtb->setCpu(_cpu);

    assert(params.decoder.size());
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        decoder[tid] = params.decoder[tid];
        // Set the size and allocate data for each fetch buffer instance
        threads[tid].size = fetchBufferSize;
        threads[tid].data = new uint8_t[fetchBufferSize];
    }

    // Get the size of an instruction.
    // stallReason size should be the same as decodeWidth,renameWidth,dispWidth
    stallReason.resize(decodeWidth, StallReason::NoStall);

    traceFetch = std::make_unique<TraceFetch>(*this, params);

    if (isTraceMode() && traceFetch && !traceFetch->allowDecoupledFrontend()) {
        fatal("Trace mode requires allowDecoupledFrontend=true for decoupled+BTB-only fetch\n");
    }
}

Fetch::~Fetch() = default;

bool
Fetch::isTraceMode() const
{
    return traceFetch && traceFetch->enabled();
}

bool
Fetch::isTraceEOF() const
{
    return traceFetch && traceFetch->isEOF();
}

std::string Fetch::name() const { return cpu->name() + ".fetch"; }

void
Fetch::regProbePoints()
{
    ppFetch = new ProbePointArg<DynInstPtr>(cpu->getProbeManager(), "Fetch");
    ppFetchRequestSent = new ProbePointArg<RequestPtr>(cpu->getProbeManager(),
                                                       "FetchRequest");

}

Fetch::FetchStatGroup::FetchStatGroup(CPU *cpu, Fetch *fetch)
    : statistics::Group(cpu, "fetch"),
    ADD_STAT(icacheStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch is stalled on an Icache miss"),
    ADD_STAT(insts, statistics::units::Count::get(),
             "Number of instructions fetch has processed"),
    ADD_STAT(targetPrepareTasks, statistics::units::Count::get(),
             "Number of fetch target prepare tasks submitted"),
    ADD_STAT(targetPrepareMerges, statistics::units::Count::get(),
             "Number of fetch target prepare results merged"),
    ADD_STAT(targetPrepareNoTarget, statistics::units::Count::get(),
             "Number of fetch target prepare cycles with no selectable "
             "target"),
    ADD_STAT(targetPrepareMismatches, statistics::units::Count::get(),
             "Number of mismatches between prepared and applied target tid"),
    ADD_STAT(prepareTasks, statistics::units::Count::get(),
             "Number of fetch prepare tasks submitted"),
    ADD_STAT(prepareMerges, statistics::units::Count::get(),
             "Number of fetch prepare results merged"),
    ADD_STAT(prepareFrontendReady, statistics::units::Count::get(),
             "Number of fetch prepare tasks with a ready frontend target"),
    ADD_STAT(prepareReadyToFetch, statistics::units::Count::get(),
             "Number of fetch prepare tasks that allowed instruction fetch"),
    ADD_STAT(prepareInterruptBlocked, statistics::units::Count::get(),
             "Number of fetch prepare tasks blocked by interrupts"),
    ADD_STAT(toDecodePrepareTasks, statistics::units::Count::get(),
             "Number of fetch-to-decode prepare tasks submitted"),
    ADD_STAT(toDecodePrepareMerges, statistics::units::Count::get(),
             "Number of fetch-to-decode prepare results merged"),
    ADD_STAT(toDecodePrepareAllBlocked, statistics::units::Count::get(),
             "Number of fetch-to-decode prepare cycles with all threads "
             "blocked"),
    ADD_STAT(futureToDecodePrepareProbes, statistics::units::Count::get(),
             "Number of future fetch-to-decode prepare probes submitted"),
    ADD_STAT(futureToDecodePrepareSkipped, statistics::units::Count::get(),
             "Number of future fetch-to-decode prepare probes skipped"),
    ADD_STAT(futureInputSkipReasons, statistics::units::Count::get(),
             "Breakdown of why future fetch-to-decode input construction "
             "was skipped"),
    ADD_STAT(futureInputQueueNotReadyOutcomes,
             statistics::units::Count::get(),
             "Next-cycle outcome for future fetch inputs skipped because "
             "fetchQueue was short"),
    ADD_STAT(futureInputQueueNotReadyStates,
             statistics::units::Count::get(),
             "Candidate fetch-side state for future fetch inputs skipped "
             "because fetchQueue was short"),
    ADD_STAT(futureInputQueueNotReadyAcceptedStates,
             statistics::units::Count::get(),
             "Candidate fetch-side state for short-queue future fetch inputs "
             "accepted as stable"),
    ADD_STAT(futureInputQueueNotReadyAcceptedSizes,
             statistics::units::Count::get(),
             "Visible fetch queue size for short-queue future fetch inputs "
             "accepted as stable"),
    ADD_STAT(futureInputQueueNotReadyAcceptedStallReasons,
             statistics::units::Count::get(),
             "Frozen stall reason for short-queue future fetch inputs "
             "accepted as stable"),
    ADD_STAT(futureInputQueueNotReadyStateOutcomes,
             statistics::units::Count::get(),
             "Next-cycle outcome for future fetch inputs skipped because "
             "fetchQueue was short, grouped by candidate fetch-side state"),
    ADD_STAT(futureInputQueueNotReadyCandidateInsts,
             statistics::units::Count::get(),
             "Fetch queue entries visible when a future queue-not-ready "
             "input was skipped"),
    ADD_STAT(futureInputQueueNotReadyActualInsts,
             statistics::units::Count::get(),
             "Fetch queue entries visible at the next-cycle owner prepare "
             "check for queue-not-ready candidates"),
    ADD_STAT(futureToDecodePrepareMerges, statistics::units::Count::get(),
             "Number of future fetch-to-decode prepare results made pending"),
    ADD_STAT(futureToDecodePrepareReuses, statistics::units::Count::get(),
             "Number of fetch-to-decode prepares reused from future work"),
    ADD_STAT(futureToDecodePrepareChecks, statistics::units::Count::get(),
             "Number of future fetch-to-decode prepare validation checks"),
    ADD_STAT(futureToDecodePrepareMatches, statistics::units::Count::get(),
             "Number of future fetch-to-decode prepare validation matches"),
    ADD_STAT(futureToDecodePrepareMismatches, statistics::units::Count::get(),
             "Number of future fetch-to-decode prepare validation "
             "mismatches"),
    ADD_STAT(futureToDecodePrepareMismatchReasons,
             statistics::units::Count::get(),
             "Field breakdown for future fetch-to-decode prepare "
             "validation mismatches"),
    ADD_STAT(futureToDecodePrepareStale, statistics::units::Count::get(),
             "Number of stale future fetch-to-decode prepare results "
             "discarded"),
    ADD_STAT(resolvePrepareTasks, statistics::units::Count::get(),
             "Number of fetch resolve incoming-CFI prepare tasks submitted"),
    ADD_STAT(resolvePrepareMerges, statistics::units::Count::get(),
             "Number of fetch resolve incoming-CFI prepare results merged"),
    ADD_STAT(resolvePrepareNoIncoming, statistics::units::Count::get(),
             "Number of fetch resolve incoming-CFI prepare cycles without "
             "incoming CFIs"),
    ADD_STAT(resolvePrepareQueueFull, statistics::units::Count::get(),
             "Number of fetch resolve incoming-CFI prepare tasks seeing a "
             "full queue"),
    ADD_STAT(resolveDequeuePrepareTasks, statistics::units::Count::get(),
             "Number of fetch resolve dequeue prepare tasks submitted"),
    ADD_STAT(resolveDequeuePrepareMerges, statistics::units::Count::get(),
             "Number of fetch resolve dequeue prepare results merged"),
    ADD_STAT(resolveDequeuePrepareNoWork, statistics::units::Count::get(),
             "Number of fetch resolve dequeue prepare cycles without an old "
             "front entry"),
    ADD_STAT(resolveDequeuePrepareMismatches,
             statistics::units::Count::get(),
             "Number of fetch resolve dequeue prepare validation mismatches"),
    ADD_STAT(resolveDequeuePrepareCFIs, statistics::units::Count::get(),
             "Number of resolved CFI PCs carried by resolve dequeue prepare"),
    ADD_STAT(branches, statistics::units::Count::get(),
             "Number of branches that fetch encountered"),
    ADD_STAT(predictedBranches, statistics::units::Count::get(),
             "Number of branches that fetch has predicted taken"),
    ADD_STAT(cycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has run and was not squashing or "
             "blocked"),
    ADD_STAT(squashCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent squashing"),
    ADD_STAT(tlbCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting for tlb"),
    ADD_STAT(idleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle"),
    ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked"),
    ADD_STAT(miscStallCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on interrupts, or bad "
             "addresses, or out of MSHRs"),
    ADD_STAT(pendingDrainCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent waiting on pipes to drain"),
    ADD_STAT(noActiveThreadStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to no active thread to fetch from"),
    ADD_STAT(pendingTrapStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending traps"),
    ADD_STAT(pendingQuiesceStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to pending quiesce instructions"),
    ADD_STAT(icacheWaitRetryStallCycles, statistics::units::Cycle::get(),
             "Number of stall cycles due to full MSHR"),
    ADD_STAT(cacheLines, statistics::units::Count::get(),
             "Number of cache lines fetched"),
    ADD_STAT(icacheSquashes, statistics::units::Count::get(),
             "Number of outstanding Icache misses that were squashed"),
    ADD_STAT(tlbSquashes, statistics::units::Count::get(),
             "Number of outstanding ITLB misses that were squashed"),
    ADD_STAT(nisnDist, statistics::units::Count::get(),
             "Number of instructions fetched each cycle (Total)"),
    ADD_STAT(idleRate, statistics::units::Ratio::get(),
             "Ratio of cycles fetch was idle",
             idleCycles / cpu->baseStats.numCycles),
    ADD_STAT(branchRate, statistics::units::Ratio::get(),
             "Number of branch fetches per cycle",
             branches / cpu->baseStats.numCycles),
    ADD_STAT(rate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of inst fetches per cycle",
             insts / cpu->baseStats.numCycles),
    ADD_STAT(fetchStatusDist, statistics::units::Count::get(),
             "Distribution of fetch status"),
    ADD_STAT(decodeStalls, statistics::units::Count::get(),
             "Number of decode stalls"),
    ADD_STAT(decodeStallRate, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Number of decode stalls per cycle",
             decodeStalls / cpu->baseStats.numCycles),
    ADD_STAT(fetchBubbles, statistics::units::Count::get(),
             "Unutilized issue-pipeline slots while there is no backend-stall"),
    ADD_STAT(fetchBubbles_max, statistics::units::Count::get(),
             "Cycles that fetch 0 instruction while there is no backend-stall"),
    ADD_STAT(frontendBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Bound",
             fetchBubbles / (cpu->baseStats.numCycles * fetch->decodeWidth)),
    ADD_STAT(frontendLatencyBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Latency Bound",
             fetchBubbles_max / cpu->baseStats.numCycles),
    ADD_STAT(frontendBandwidthBound, statistics::units::Rate<
                    statistics::units::Count, statistics::units::Cycle>::get(),
             "Frontend Bandwidth Bound",
             frontendBound - frontendLatencyBound),
    ADD_STAT(resolveQueueFullEvents, statistics::units::Count::get(),
             "Number of events the resolve queue becomes full"),
    ADD_STAT(resolveEnqueueFailEvent, statistics::units::Count::get(),
             "Number of times an entry could not be enqueued to the resolve queue"),
    ADD_STAT(resolveDequeueCount, statistics::units::Count::get(),
             "Number of times an entry is dequeued from the resolve queue"),
    ADD_STAT(resolveEnqueueCount, statistics::units::Count::get(),
             "Number of times an entry is enqueued to the resolve queue"),
    ADD_STAT(resolveQueueOccupancy, statistics::units::Count::get(),
             "Number of entries in the resolve queue"),
    ADD_STAT(traceMetaStores, statistics::units::Count::get(),
             "Number of stored trace metadata records (seqNum -> traceInst)"),
    ADD_STAT(traceMetaCleanupSquashCalls, statistics::units::Count::get(),
             "Number of times cleanup was called due to squash/rollback"),
    ADD_STAT(traceMetaCleanupSquashEntries, statistics::units::Count::get(),
             "Total entries erased by squash/rollback cleanups"),
    ADD_STAT(traceMetaCleanupCommitCalls, statistics::units::Count::get(),
             "Number of times cleanup was called on successful commit")
{
        icacheStallCycles
            .prereq(icacheStallCycles);
        insts
            .prereq(insts);
        targetPrepareTasks
            .prereq(targetPrepareTasks);
        targetPrepareMerges
            .prereq(targetPrepareMerges);
        targetPrepareNoTarget
            .prereq(targetPrepareNoTarget);
        targetPrepareMismatches
            .prereq(targetPrepareMismatches);
        prepareTasks
            .prereq(prepareTasks);
        prepareMerges
            .prereq(prepareMerges);
        prepareFrontendReady
            .prereq(prepareFrontendReady);
        prepareReadyToFetch
            .prereq(prepareReadyToFetch);
        prepareInterruptBlocked
            .prereq(prepareInterruptBlocked);
        toDecodePrepareTasks
            .prereq(toDecodePrepareTasks);
        toDecodePrepareMerges
            .prereq(toDecodePrepareMerges);
        toDecodePrepareAllBlocked
            .prereq(toDecodePrepareAllBlocked);
        futureToDecodePrepareProbes
            .prereq(futureToDecodePrepareProbes);
        futureToDecodePrepareSkipped
            .prereq(futureToDecodePrepareSkipped);
        futureInputSkipReasons
            .init(static_cast<unsigned>(
                    FutureDecodeQueueInputSkipReason::NumReasons))
            .flags(statistics::total);
        for (unsigned i = 0;
             i < static_cast<unsigned>(
                    FutureDecodeQueueInputSkipReason::NumReasons);
             ++i) {
            futureInputSkipReasons.subname(
                    i,
                    futureDecodeQueueInputSkipReasonName(
                        static_cast<FutureDecodeQueueInputSkipReason>(i)));
        }
        futureInputQueueNotReadyOutcomes
            .init(static_cast<unsigned>(
                    FutureQueueNotReadyOutcome::NumOutcomes))
            .flags(statistics::total);
        for (unsigned i = 0;
             i < static_cast<unsigned>(
                    FutureQueueNotReadyOutcome::NumOutcomes);
             ++i) {
            futureInputQueueNotReadyOutcomes.subname(
                    i,
                    futureQueueNotReadyOutcomeName(
                        static_cast<FutureQueueNotReadyOutcome>(i)));
        }
        futureInputQueueNotReadyStates
            .init(static_cast<unsigned>(
                    FutureQueueNotReadyState::NumStates))
            .flags(statistics::total);
        for (unsigned i = 0;
             i < static_cast<unsigned>(
                    FutureQueueNotReadyState::NumStates);
             ++i) {
            futureInputQueueNotReadyStates.subname(
                    i,
                    futureQueueNotReadyStateName(
                        static_cast<FutureQueueNotReadyState>(i)));
        }
        futureInputQueueNotReadyAcceptedStates
            .init(static_cast<unsigned>(
                    FutureQueueNotReadyState::NumStates))
            .flags(statistics::total);
        for (unsigned i = 0;
             i < static_cast<unsigned>(
                    FutureQueueNotReadyState::NumStates);
             ++i) {
            futureInputQueueNotReadyAcceptedStates.subname(
                    i,
                    futureQueueNotReadyStateName(
                        static_cast<FutureQueueNotReadyState>(i)));
        }
        futureInputQueueNotReadyAcceptedSizes
            .init(MaxWidth + 1)
            .flags(statistics::total);
        futureInputQueueNotReadyAcceptedStallReasons
            .init(NumStallReasons)
            .flags(statistics::total);
        futureInputQueueNotReadyStateOutcomes
            .init(static_cast<unsigned>(
                    FutureQueueNotReadyState::NumStates),
                  static_cast<unsigned>(
                    FutureQueueNotReadyOutcome::NumOutcomes))
            .flags(statistics::total);
        for (unsigned state = 0;
             state < static_cast<unsigned>(
                    FutureQueueNotReadyState::NumStates);
             ++state) {
            futureInputQueueNotReadyStateOutcomes.subname(
                    state,
                    futureQueueNotReadyStateName(
                        static_cast<FutureQueueNotReadyState>(state)));
            for (unsigned outcome = 0;
                 outcome < static_cast<unsigned>(
                    FutureQueueNotReadyOutcome::NumOutcomes);
                 ++outcome) {
                futureInputQueueNotReadyStateOutcomes.ysubname(
                        outcome,
                        futureQueueNotReadyOutcomeName(
                            static_cast<FutureQueueNotReadyOutcome>(
                                outcome)));
            }
        }
        futureInputQueueNotReadyCandidateInsts
            .prereq(futureInputQueueNotReadyCandidateInsts);
        futureInputQueueNotReadyActualInsts
            .prereq(futureInputQueueNotReadyActualInsts);
        futureToDecodePrepareMerges
            .prereq(futureToDecodePrepareMerges);
        futureToDecodePrepareReuses
            .prereq(futureToDecodePrepareReuses);
        futureToDecodePrepareChecks
            .prereq(futureToDecodePrepareChecks);
        futureToDecodePrepareMatches
            .prereq(futureToDecodePrepareMatches);
        futureToDecodePrepareMismatches
            .prereq(futureToDecodePrepareMismatches);
        futureToDecodePrepareMismatchReasons
            .init(static_cast<unsigned>(
                    FutureToDecodePrepareMismatchReason::NumReasons))
            .flags(statistics::total);
        for (unsigned i = 0;
             i < static_cast<unsigned>(
                    FutureToDecodePrepareMismatchReason::NumReasons);
             ++i) {
            futureToDecodePrepareMismatchReasons.subname(
                    i,
                    futureToDecodePrepareMismatchReasonName(
                        static_cast<FutureToDecodePrepareMismatchReason>(
                            i)));
        }
        futureToDecodePrepareStale
            .prereq(futureToDecodePrepareStale);
        resolvePrepareTasks
            .prereq(resolvePrepareTasks);
        resolvePrepareMerges
            .prereq(resolvePrepareMerges);
        resolvePrepareNoIncoming
            .prereq(resolvePrepareNoIncoming);
        resolvePrepareQueueFull
            .prereq(resolvePrepareQueueFull);
        resolveDequeuePrepareTasks
            .prereq(resolveDequeuePrepareTasks);
        resolveDequeuePrepareMerges
            .prereq(resolveDequeuePrepareMerges);
        resolveDequeuePrepareNoWork
            .prereq(resolveDequeuePrepareNoWork);
        resolveDequeuePrepareMismatches
            .prereq(resolveDequeuePrepareMismatches);
        resolveDequeuePrepareCFIs
            .prereq(resolveDequeuePrepareCFIs);
        branches
            .prereq(branches);
        predictedBranches
            .prereq(predictedBranches);
        cycles
            .prereq(cycles);
        squashCycles
            .prereq(squashCycles);
        tlbCycles
            .prereq(tlbCycles);
        idleCycles
            .prereq(idleCycles);
        blockedCycles
            .prereq(blockedCycles);
        cacheLines
            .prereq(cacheLines);
        miscStallCycles
            .prereq(miscStallCycles);
        pendingDrainCycles
            .prereq(pendingDrainCycles);
        noActiveThreadStallCycles
            .prereq(noActiveThreadStallCycles);
        pendingTrapStallCycles
            .prereq(pendingTrapStallCycles);
        pendingQuiesceStallCycles
            .prereq(pendingQuiesceStallCycles);
        icacheWaitRetryStallCycles
            .prereq(icacheWaitRetryStallCycles);
        icacheSquashes
            .prereq(icacheSquashes);
        tlbSquashes
            .prereq(tlbSquashes);
        nisnDist
            .init(/* base value */ 0,
              /* last value */ fetch->fetchWidth,
              /* bucket size */ 1)
            .flags(statistics::pdf);
        idleRate
            .prereq(idleRate);
        branchRate
            .flags(statistics::total);
        rate
            .flags(statistics::total);
        fetchStatusDist
            .init(NumFetchStatus)
            .flags(statistics::pdf | statistics::total);

        for (int i = 0; i < NumFetchStatus; i++) {
            fetchStatusDist.subname(i, fetch->fetchStatusStr[static_cast<Fetch::ThreadStatus>(i)]);
        }
        decodeStalls
            .prereq(decodeStalls);
        decodeStallRate
            .flags(statistics::total);
        fetchBubbles
            .prereq(fetchBubbles);
        fetchBubbles_max
            .prereq(fetchBubbles_max);
        frontendBound
            .flags(statistics::total);
        frontendLatencyBound
            .flags(statistics::total);
        frontendBandwidthBound
            .flags(statistics::total);
        resolveEnqueueCount
            .init(1, 8, 1);
        resolveQueueOccupancy
            .init(0, 32, 1);
        traceMetaStores
            .prereq(traceMetaStores);
        traceMetaCleanupSquashCalls
            .prereq(traceMetaCleanupSquashCalls);
        traceMetaCleanupSquashEntries
            .prereq(traceMetaCleanupSquashEntries);
        traceMetaCleanupCommitCalls
            .prereq(traceMetaCleanupCommitCalls);
}
void
Fetch::setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer)
{
    timeBuffer = time_buffer;

    // Create wires to get information from proper places in time buffer.
    fromDecode = timeBuffer->getWire(-decodeToFetchDelay);
    fromRename = timeBuffer->getWire(-renameToFetchDelay);
    fromIEW = timeBuffer->getWire(-iewToFetchDelay);
    fromCommit = timeBuffer->getWire(-commitToFetchDelay);
}

void
Fetch::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Fetch::setFetchQueue(TimeBuffer<FetchStruct> *ftb_ptr)
{
    // Create wire to write information to proper place in fetch time buf.
    toDecode = ftb_ptr->getWire(0);

    // initialize to toDecode stall vector
    toDecode->fetchStallReason = stallReason;
}

void
Fetch::startupStage()
{
    assert(priorityList.empty());
    resetStage();

    // Fetch needs to start fetching instructions at the very beginning,
    // so it must start up in active state.
    switchToActive();

    if (isTraceMode() && !traceFetch->initTraceMode()) {
        fatal("Failed to initialize trace mode\n");
    }
}

void
Fetch::clearStates(ThreadID tid)
{
    setThreadStatus(tid, Running);
    set(threads[tid].fetchpc, cpu->pcState(tid));
    macroop[tid] = NULL;
    delayedCommit[tid] = false;
    threads[tid].cacheReq.reset();
    threads[tid].reset();
    fetchQueue[tid].clear();

    // TODO not sure what to do with priorityList for now
    // priorityList.push_back(tid);
}

void
Fetch::resetStage()
{
    numInst = 0;
    interruptPending = false;
    cacheBlocked = false;

    priorityList.clear();

    // Setup PC and nextPC with initial state.
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        setThreadStatus(tid, Running);
        set(threads[tid].fetchpc, cpu->pcState(tid));
        macroop[tid] = NULL;

        delayedCommit[tid] = false;
        threads[tid].cacheReq.reset();

        threads[tid].reset();
        ftqEntryFetchedInsts[tid] = 0;

        fetchQueue[tid].clear();

        priorityList.push_back(tid);
    }

    wroteToTimeBuffer = false;
    _status = Inactive;

    if (traceFetch) {
        traceFetch->resetStage();
    }

    assert(dbpbtb);
    dbpbtb->resetPC(threads[0].fetchpc->instAddr());
}

bool
Fetch::handleMultiCacheLineFetch(Addr vaddr, ThreadID tid, Addr pc)
{
    DPRINTF(Fetch, "[tid:%i] Handling multi-cacheline fetch for addr %#x, pc=%#lx\n", tid, vaddr, pc);
    // Transition to WaitingCache state when initiating cache access
    setThreadStatus(tid, WaitingCache);

    // Reset cache request state for this thread
    threads[tid].cacheReq.reset();
    threads[tid].cacheReq.baseAddr = vaddr;
    threads[tid].cacheReq.totalSize = fetchBufferSize;

    Addr fetchPC = vaddr;
    unsigned fetchSize = cacheBlkSize - fetchPC % cacheBlkSize;  // Size for first cache line

    DPRINTF(Fetch, "[tid:%i] Creating first cache line request: addr=%#x, size=%d\n",
            tid, fetchPC, fetchSize);

    // Create and send first request (tail of first cache line)
    RequestPtr first_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    first_mem_req->taskId(cpu->taskId());
    first_mem_req->setMisalignedFetch();
    first_mem_req->setReqNum(1);

    threads[tid].cacheReq.addRequest(first_mem_req); // packet will be created later

    // Initiate translation for first request
    updateCacheRequestStatusByRequest(tid, first_mem_req, TlbWait);
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans = new FetchTranslation(this);
    cpu->mmu->translateTiming(first_mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);

    // Prepare second request (head of second cache line)
    fetchPC += fetchSize;  // Move to start of next cache line
    assert(fetchPC % cacheBlkSize == 0);
    fetchSize = fetchBufferSize - fetchSize;  // Remaining size

    DPRINTF(Fetch, "[tid:%i] Creating second cache line request: addr=%#x, size=%d\n",
            tid, fetchPC, fetchSize);

    // Create and send second request
    RequestPtr second_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    second_mem_req->taskId(cpu->taskId());
    second_mem_req->setMisalignedFetch();
    second_mem_req->setReqNum(2);

    threads[tid].cacheReq.addRequest(second_mem_req);  // Add second request to cache request

    DPRINTF(Fetch, "[tid:%i] Initiating translation for second cache line\n", tid);

    // Always initiate translation for second request, regardless of first request status
    updateCacheRequestStatusByRequest(tid, second_mem_req, TlbWait);
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans2 = new FetchTranslation(this);
    cpu->mmu->translateTiming(second_mem_req, cpu->thread[tid]->getTC(),
                              trans2, BaseMMU::Execute);
    return true;
}

bool
Fetch::processMultiCacheLineCompletion(ThreadID tid, PacketPtr pkt)
{
    DPRINTF(Fetch, "[tid:%i] Processing dual cacheline fetch completion for addr %#lx.\n",
            tid, pkt->getAddr());

    // Mark this packet as completed in the cache request (this also stores the packet)
    bool found_packet = threads[tid].cacheReq.markCompletedAndStorePacket(pkt);
    if (!found_packet) {
        DPRINTF(Fetch, "[tid:%i] Packet doesn't match current requests, deleting pkt %#lx\n",
                tid, pkt->getAddr());
        DPRINTF(Fetch, "[tid:%i] Expected requests: ", tid);
        for (size_t i = 0; i < threads[tid].cacheReq.requests.size(); i++) {
            DPRINTF(Fetch, "req[%d]=0x%lx ", i, threads[tid].cacheReq.requests[i]->getVaddr());
        }
        DPRINTF(Fetch, "\n");
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] Packet successfully matched and stored. Current status: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Check if we're still waiting for other packets
    if (!threads[tid].cacheReq.allCompleted()) {
        DPRINTF(Fetch, "[tid:%i] Waiting for remaining packets. Completed: %d, Total: %d\n",
                tid, threads[tid].cacheReq.completedPackets, threads[tid].cacheReq.packets.size());

        // Note: retry is handled completely by the standard gem5 recvReqRetry mechanism
        // No need to handle retry here to avoid duplicate packet sending

        return false;  // Return false to indicate we're still waiting
    }

    // All packets have arrived - merge them directly into fetchBuffer
    DPRINTF(Fetch, "[tid:%i] All packets arrived, merging data into fetchBuffer.\n", tid);

    // Find the packets by request number
    PacketPtr firstPkt = nullptr;
    PacketPtr secondPkt = nullptr;

    for (size_t i = 0; i < threads[tid].cacheReq.packets.size(); i++) {
        if (threads[tid].cacheReq.requests[i]->getReqNum() == 1) {
            firstPkt = threads[tid].cacheReq.packets[i];
        } else if (threads[tid].cacheReq.requests[i]->getReqNum() == 2) {
            secondPkt = threads[tid].cacheReq.packets[i];
        }
    }

    assert(firstPkt && secondPkt);

    // Copy merged data directly into fetchBuffer
    memcpy(threads[tid].data, firstPkt->getConstPtr<uint8_t>(), firstPkt->getSize());
    memcpy(threads[tid].data + firstPkt->getSize(), secondPkt->getConstPtr<uint8_t>(), secondPkt->getSize());
    threads[tid].valid = true;

    // Clean up the packets
    delete firstPkt;
    delete secondPkt;

    DPRINTF(Fetch, "[tid:%i] Dual cacheline fetch completion processed successfully.\n", tid);
    return true;
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());
    assert(pkt->req->isMisalignedFetch() && "Only multi-cacheline fetch is supported");

    bool allCompleted = processMultiCacheLineCompletion(tid, pkt);
    // If we're still waiting for another packet, return early
    if (!allCompleted) {
        return;
    }

    // Check if this completion should be processed
    // Either thread is waiting for cache, or cache just completed
    CacheRequestStatus cacheStatus = threads[tid].cacheReq.getOverallStatus();
    if (!hasPendingCacheRequests(tid) && cacheStatus != AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Thread not waiting for cache and no completion, ignoring\n", tid);
        ++fetchStats.icacheSquashes;
        return;
    }

    // Data has been merged into fetchBuffer, we can proceed
    DPRINTF(Fetch, "[tid:%i] All misaligned packets received and merged.\n", tid);

    assert(!cpu->switchedOut());

    // Trace 按需消费：不在 icache 完成时写入 trace 指令码，避免批量消费。
    if (isTraceMode()) {
        DPRINTF(TraceReader,
                "[TRACE] Icache completion: keep timing only; no trace bytes injection\n");
    }

    // Verify fetchBufferPC alignment with the supplying FSQ entry.
    if (threads[tid].valid && dbpbtb->ftqHasFetching(0)) {
        const auto &stream = dbpbtb->ftqFetchingTarget(0);
        if (threads[tid].startPC != stream.startPC) {
            panic("fetchBufferPC %#x should be aligned with FSQ startPC %#x",
                  threads[tid].startPC, stream.startPC);
        }
    }

    // Wake up the CPU (if it went to sleep and was waiting on
    // this completion event).
    cpu->wakeCPU();

    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n",
            tid);

    switchToActive();

    // Transition from WaitingCache back to Running when cache access completes
    setThreadStatus(tid, Running);
}

void
Fetch::drainResume()
{
}

void
Fetch::drainSanityCheck() const
{
    assert(isDrained());
    assert(retryPkt.size() == 0);
    assert(retryTid == InvalidThreadID);
    assert(!cacheBlocked);
    assert(!interruptPending);

    for (ThreadID i = 0; i < numThreads; ++i) {
        assert(threads[i].cacheReq.packets.empty());
        assert(fetchStatus[i] == Idle);
    }

    branchPred->drainSanityCheck();
}

bool
Fetch::isDrained() const
{
    /* Make sure that threads are either idle of that the commit stage
     * has signaled that draining has completed by setting the drain
     * stall flag. This effectively forces the pipeline to be disabled
     * until the whole system is drained (simulation may continue to
     * drain other components).
     */
    for (ThreadID i = 0; i < numThreads; ++i) {
        // Verify fetch queues are drained
        if (!fetchQueue[i].empty())
            return false;

        // Return false if not idle or drain stalled
        if (fetchStatus[i] != Idle) {
            return false;
        }
    }

    /* The pipeline might start up again in the middle of the drain
     * cycle if the finish translation event is scheduled, so make
     * sure that's not the case.
     */
    return !finishTranslationEvent.scheduled();
}

void
Fetch::takeOverFrom()
{
    assert(cpu->getInstPort().isConnected());
    resetStage();

}

void
Fetch::drainStall(ThreadID tid)
{
}

void
Fetch::wakeFromQuiesce()
{
    DPRINTF(Fetch, "Waking up from quiesce\n");
    // Hopefully this is safe
    // @todo: Allow other threads to wake from quiesce.
    setThreadStatus(0, Running);
}

void
Fetch::switchToActive()
{
    if (_status == Inactive) {
        DPRINTF(Activity, "Activating stage.\n");

        cpu->activateStage(CPU::FetchIdx);

        _status = Active;
    }
}

void
Fetch::switchToInactive()
{
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);

        _status = Inactive;
    }
}

void
Fetch::deactivateThread(ThreadID tid)
{
    // Update priority list
    auto thread_it = std::find(priorityList.begin(), priorityList.end(), tid);
    if (thread_it != priorityList.end()) {
        priorityList.erase(thread_it);
    }
}

bool
Fetch::lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc)
{
    // Do branch prediction check here.
    // A bit of a misnomer...next_PC is actually the current PC until
    // this function updates it.
    bool predict_taken = false;

    // Decoupled+BTB-only: compute next PC directly from the supplying FSQ entry.
    ThreadID tid = inst->threadNumber;
    assert(dbpbtb);
    assert(dbpbtb->ftqHasFetching(0));
    const auto &stream = dbpbtb->ftqFetchingTarget(tid);

    const Addr curr_pc = next_pc.instAddr();
    assert(stream.startPC <= curr_pc && curr_pc < stream.predEndPC);

    bool run_out = false;

    // Taken when the current PC matches the predicted control PC.
    predict_taken = stream.predTaken && (curr_pc == stream.predBranchInfo.pc);
    if (predict_taken) {
        auto &rpc = next_pc.as<GenericISA::PCStateWithNext>();
        rpc.pc(stream.predBranchInfo.target);
        rpc.npc(stream.predBranchInfo.target + 4);
        rpc.uReset();
        run_out = true;
    } else if (inst->staticInst->isMicroop()) {
        // Microops must advance uPC explicitly; they do not rely on decoder NPC.
        inst->staticInst->advancePC(next_pc);
        run_out = next_pc.instAddr() >= stream.predEndPC;
    } else {
        // Sequential fetch: decoder already computed npc with correct inst size.
        auto &rpc = next_pc.as<RiscvISA::PCState>();
        const Addr fall_thru = rpc.npc();
        rpc.pc(fall_thru);
        // Placeholder; decoder will overwrite npc on the next decode.
        rpc.npc(fall_thru + 4);
        rpc.uReset();
        run_out = fall_thru >= stream.predEndPC;
    }

    // Track how many dynamic instructions were fetched for this (legacy) FTQ/FSQ entry.
    ftqEntryFetchedInsts[tid]++;
    if (run_out) {
        dbpbtb->consumeFetchTarget(ftqEntryFetchedInsts[tid], tid);
        ftqEntryFetchedInsts[tid] = 0;
        threads[tid].valid = false;
        DPRINTF(DecoupleBP, "Used up fetch targets.\n");
    }

    inst->setLoopIteration(currentLoopIter);

    // For decoupled frontend, the instruction type is predicted with BTB
    if (!predict_taken) {
        inst->setPredTarg(next_pc);
        inst->setPredTaken(false);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x predicted to be taken to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    DPRINTF(Fetch, "[tid:%i] [sn:%llu] Branch at PC %#x "
            "predicted to go to %s\n",
            tid, inst->seqNum, inst->pcState().instAddr(), next_pc);
    inst->setPredTarg(next_pc);
    inst->setPredTaken(predict_taken);

    ++fetchStats.branches;

    if (predict_taken) {
        ++fetchStats.predictedBranches;
    }

    return predict_taken;
}

bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc)
{
    assert(!cpu->switchedOut());

    // Check for blocking conditions
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n", tid);
        setAllFetchStalls(StallReason::IcacheStall);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n", tid);
        setAllFetchStalls(StallReason::IntStall);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x, pc=%#lx\n",
            tid, vaddr, vaddr, pc);

    // With 66-byte fetchBufferSize, we always need to access 2 cache lines
    return handleMultiCacheLineFetch(vaddr, tid, pc);
}

bool
Fetch::validateTranslationRequest(ThreadID tid, const RequestPtr &mem_req)
{
    // Check if this request belongs to current cache request
    bool isExpectedReq = false;
    for (size_t i = 0; i < threads[tid].cacheReq.requests.size(); i++) {
        if (mem_req == threads[tid].cacheReq.requests[i]) {
            isExpectedReq = true;
            break;
        }
    }

    // Check if request should be processed using new state system
    if (!isExpectedReq || !hasPendingCacheRequests(tid)) {
        DPRINTF(Fetch, "[tid:%i] Ignoring translation completed after squash or unexpected request\n", tid);
        DPRINTF(Fetch, "[tid:%i] Ignoring req addr=%#lx\n", tid, mem_req->getVaddr());
        ++fetchStats.tlbSquashes;
        return false;
    }

    return true;
}

void
Fetch::handleSuccessfulTranslation(ThreadID tid, const RequestPtr &mem_req, Addr fetchPC)
{
    // Check that we're not going off into random memory
    if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
        DPRINTF(Fetch, "Address %#x is outside of physical memory, stopping fetch, %lu\n",
                mem_req->getPaddr(), curTick());

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed);
        setAllFetchStalls(StallReason::OtherFetchStall);
        // Note: Don't reset here, let the caller handle cleanup based on overall status
        return;
    }

    // Build packet here.
    PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
    data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);
    // All requests are multi-cacheline, always set send right away
    data_pkt->setSendRightAway();

    DPRINTF(Fetch, "[tid:%i] Fetching data for addr %#x, pc=%#lx\n",
                tid, mem_req->getVaddr(), fetchPC);

    threads[tid].startPC = fetchPC;
    threads[tid].valid = false;
    DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

    fetchStats.cacheLines++;

    // Access the cache.
    if (!icachePort.sendTimingReq(data_pkt)) {
        DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitRetry);
        data_pkt->setRetriedPkt();
        DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                mem_req->getVaddr());
        setAllFetchStalls(StallReason::IcacheStall);
        retryPkt.push_back(data_pkt);
        retryTid = tid;
        cacheBlocked = true;
    } else {
        DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
        DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache response.\n", tid);
        lastIcacheStall[tid] = curTick();

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitResponse);
        setAllFetchStalls(StallReason::IcacheStall);
        // Notify Fetch Request probe when a packet containing a fetch request is successfully sent
        ppFetchRequestSent->notify(mem_req);
    }
}

void
Fetch::handleTranslationFault(ThreadID tid, const RequestPtr &mem_req, const Fault &fault)
{
    DPRINTF(FetchFault, "fault, mem_req.addr=%#lx\n", mem_req->getVaddr());

    // Don't send an instruction to decode if we can't handle it.
    if (!(numInst < fetchWidth) || !(fetchQueue[tid].size() < fetchQueueSize)) {
        if (finishTranslationEvent.scheduled() && finishTranslationEvent.getReq() != mem_req) {
            DPRINTF(FetchFault, "fault, finishTranslationEvent.getReq().addr=%#lx, mem_req.addr=%#lx\n",
                    finishTranslationEvent.getReq()->getVaddr(), mem_req->getVaddr());
            return;
        }
        assert(!finishTranslationEvent.scheduled());
        finishTranslationEvent.setFault(fault);
        finishTranslationEvent.setReq(mem_req);
        cpu->schedule(finishTranslationEvent, cpu->clockEdge(Cycles(1)));
        return;
    }

    DPRINTF(Fetch, "[tid:%i] Got back req with addr %#x but expected base addr %#x\n",
            tid, mem_req->getVaddr(), threads[tid].cacheReq.baseAddr);

    // Update new cache request status system
    updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed);

    // Translation faulted, icache request won't be sent.
    threads[tid].cacheReq.reset();

    // Send the fault to commit.  This thread will not do anything
    // until commit handles the fault.  The only other way it can
    // wake up is if a squash comes along and changes the PC.
    const PCStateBase &fetch_pc = *threads[tid].fetchpc;

    DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
    // We will use a nop in order to carry the fault.
    DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
            fetch_pc, fetch_pc, false);
    instruction->setVersion(localSquashVer);
    instruction->setNotAnInst();

    instruction->setPredTarg(fetch_pc);
    instruction->fault = fault;
    std::unique_ptr<PCStateBase> next_pc(fetch_pc.clone());
    instruction->staticInst->advancePC(*next_pc);
    set(instruction->predPC, next_pc);

    wroteToTimeBuffer = true;

    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();

    setThreadStatus(tid, TrapPending);
    setAllFetchStalls(StallReason::TrapStall);

    DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
    DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
            tid, fault->name(), *threads[tid].fetchpc);
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());

    // For multi-cacheline fetch, use the stored base address
    // Both requests should use the same fetchBufferPC
    Addr fetchPC = threads[tid].cacheReq.baseAddr;

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    DPRINTF(Fetch, "[tid:%i] Translation completed for addr %#lx\n",
            tid, mem_req->getVaddr());

    // Validate if this request should be processed
    if (!validateTranslationRequest(tid, mem_req)) {
        return;
    }

    // Handle translation result
    if (fault == NoFault) {
        handleSuccessfulTranslation(tid, mem_req, fetchPC);
    } else {
        handleTranslationFault(tid, mem_req, fault);
    }

    _status = updateFetchStatus();
}

void
Fetch::doSquash(PCStateBase &new_pc, const DynInstPtr squashInst, const InstSeqNum seqNum,
        ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing, setting PC to: %s. seqNum: %lu\n",
            tid, new_pc, seqNum);
    if (squashInst) {
        DPRINTF(Fetch, "[tid:%i] Squash caused by inst at PC: %s, seqNum: %lu\n",
                tid, squashInst->pcState(), squashInst->seqNum);
    }

    // restore vtype
    uint8_t restored_vtype = cpu->readMiscReg(RiscvISA::MISCREG_VTYPE, tid);
    for (auto& it : cpu->instList) {
        if (!it->isSquashed() &&
            it->seqNum <= seqNum &&
            it->staticInst->isVectorConfig()) {
            auto vset = static_cast<RiscvISA::VConfOp*>(it->staticInst.get());
            if (vset->vtypeIsImm) {
                restored_vtype = vset->earlyVtype;
            }
        }
    }
    decoder[tid]->as<RiscvISA::Decoder>().setVtype(restored_vtype);

    // align PC to 2 bytes
    // This handles cases where PC might be odd due to speculative execution,
    // but no need to throw INST_ADDR_MISALIGNED fault here
    if (new_pc.instAddr() % 2 != 0) {
        // Modify new_pc directly to make it 2-byte aligned
        auto& riscv_pc = new_pc.as<RiscvISA::PCState>();
        riscv_pc.set(new_pc.instAddr() & ~1);
        set(threads[tid].fetchpc, new_pc);
        DPRINTF(Fetch, "[tid:%i] pc is misaligned, aligned to %#lx\n", tid, new_pc.instAddr());
    } else {
        set(threads[tid].fetchpc, new_pc);
    }
    if (squashInst && squashInst->pcState().instAddr() == new_pc.instAddr())
        macroop[tid] = squashInst->macroop;
    else
        macroop[tid] = NULL;
    decoder[tid]->reset();

    // Clear the icache miss if it's outstanding.
    DPRINTF(Fetch, "[tid:%i] Squash: clear cacheReq, current fetchStatus[tid]=%d\n", tid, fetchStatus[tid]);

    // Cancel all active cache requests in new status system
    threads[tid].cacheReq.cancelAllRequests();
    DPRINTF(Fetch, "[tid:%i] Squash: cancelled all cache requests, status: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Reset the cache request after cancelling
    threads[tid].cacheReq.reset();

    // Get rid of the retrying packet if it was from this thread.
    if (retryTid == tid) {
        assert(cacheBlocked);
        for (auto it : retryPkt) {
            delete it;
        }
        retryPkt.clear();
        retryTid = InvalidThreadID;
        cacheBlocked = false;   // clear cache blocked
    }

    if (squashInst && !squashInst->isControl()) {
        // csrrw satp need to flush all fetch targets
        threads[tid].valid = false;
    }

    setThreadStatus(tid, Squashing);
    setAllFetchStalls(StallReason::BpStall); // may caused by other stages like load and store

    // Empty fetch queue
    fetchQueue[tid].clear();

    // microops are being squashed, it is not known wheather the
    // youngest non-squashed microop was  marked delayed commit
    // or not. Setting the flag to true ensures that the
    // interrupts are not handled when they cannot be, though
    // some opportunities to handle interrupts may be missed.
    delayedCommit[tid] = true;

    // Force a new I-cache request for the next FTQ head after squash.
    threads[tid].valid = false;
    ftqEntryFetchedInsts[tid] = 0;

    if (traceFetch) {
        traceFetch->handleTraceSquash(tid, new_pc, squashInst, seqNum);
    }

    ++fetchStats.squashCycles;
}

void
Fetch::flushFetchBuffer()
{
    for (ThreadID i = 0; i < numThreads; ++i) {
        threads[i].valid = false;
    }
}

Addr
Fetch::getPreservedReturnAddr(const DynInstPtr &dynInst)
{
    assert(dbpbtb);
    return dbpbtb->getPreservedReturnAddr(dynInst);
}

void
Fetch::squashFromDecode(PCStateBase &new_pc, const DynInstPtr squashInst,
        const InstSeqNum seq_num, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squashing from decode.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);

    // Tell the CPU to remove any instructions that are in flight between
    // fetch and decode.
    cpu->removeInstsUntil(seq_num, tid);
}

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    //Check Running
    std::list<ThreadID>::iterator act_tid = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (act_tid != end) {
        ThreadID tid = *act_tid++;

        if (canFetchInstructions(tid) || fetchStatus[tid] == Squashing ||
            threads[tid].cacheReq.getOverallStatus() == AccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
                    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                            "completion\n",tid);
                }

                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

void
Fetch::squash(PCStateBase &new_pc, const InstSeqNum seq_num,
        DynInstPtr squashInst, ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] Squash from commit.\n", tid);

    doSquash(new_pc, squashInst, seq_num, tid);
    assert(new_pc.instAddr() % 2 == 0 && "squash PC should be 2-byte aligned");

    // Tell the CPU to remove any instructions that are not in the ROB.
    cpu->removeInstsNotInROB(tid);
}

void
Fetch::tick()
{
    const FetchBackwardInput input = backwardInput(cpu->curCycle());

    // Initialize state for this tick cycle
    bool status_change = initializeTickState(input);

    // Simple decoupled+BTB ordering:
    // - first consume incoming squashes/redirects (in initializeTickState())
    // - then advance predictor pipeline + try to supply an FTQ head
    // - then run fetch using the supplied FTQ entry (if any)
    assert(dbpbtb);
    dbpbtb->tick();

    // Perform fetch operations and instruction delivery
    fetchAndProcessInstructions(status_change, input);
}

bool
Fetch::buildFutureDecodeQueueInput(
        Cycles cycle, const StallSignalLatch &decode_to_fetch,
        const TimeStruct *snapshot_decode,
        const TimeStruct *snapshot_commit,
        FutureDecodeQueueInput &input,
        FutureDecodeQueueInputSkipInfo *skip_info) const
{
    auto skip = [skip_info, cycle, this, &decode_to_fetch](
            FutureDecodeQueueInputSkipReason reason,
            ThreadID tid = InvalidThreadID,
            unsigned queue_size = 0) {
        if (skip_info) {
            skip_info->cycle = cycle;
            skip_info->reason = reason;
            skip_info->tid = tid;
            skip_info->queueSize = queue_size;
            skip_info->decodeWidth =
                std::min<unsigned>(decodeWidth, MaxWidth);
            skip_info->queueState = FutureQueueNotReadyState::NumStates;
            if (reason == FutureDecodeQueueInputSkipReason::FetchQueueNotReady
                && tid != InvalidThreadID && tid < numThreads) {
                const FetchPrepareInput fetch_input =
                    buildFetchPrepareInput(cycle, tid);
                const FetchPrepareResult fetch_result =
                    prepareFetchControl(fetch_input);
                skip_info->queueState =
                    classifyFutureQueueNotReadyState(
                            fetch_input, fetch_result,
                            decode_to_fetch.block[tid]);
            }
        }
        return false;
    };

    if (!snapshot_decode || !snapshot_commit)
        return skip(FutureDecodeQueueInputSkipReason::MissingSnapshot);

    if (activeThreads->empty())
        return skip(FutureDecodeQueueInputSkipReason::NoActiveThreads);

    for (ThreadID tid : *activeThreads) {
        if (snapshot_commit->commitInfo[tid].squash)
            return skip(FutureDecodeQueueInputSkipReason::CommitControl);
        if (snapshot_decode->decodeInfo[tid].squash)
            return skip(FutureDecodeQueueInputSkipReason::DecodeControl);
    }

    input = FutureDecodeQueueInput{};
    input.cycle = cycle;
    input.numThreads = numThreads;
    input.decodeWidth = decodeWidth;
    input.decodeToFetch = decode_to_fetch;

    ThreadID blocked_tid = InvalidThreadID;
    bool all_threads_blocked = true;
    for (int tid = 0; tid < numThreads; ++tid) {
        if (!decode_to_fetch.block[tid])
            all_threads_blocked = false;
        else if (blocked_tid == InvalidThreadID)
            blocked_tid = tid;
    }

    const unsigned width = std::min<unsigned>(decodeWidth, MaxWidth);
    if (all_threads_blocked) {
        if (blocked_tid == InvalidThreadID)
            return skip(FutureDecodeQueueInputSkipReason::AllBlockedNoTid);
        input.allThreadsBlocked = true;
        input.blockedTid = blocked_tid;
        return true;
    }

    const ThreadID tid = 0; // Preserve the current non-SMT send policy.
    const auto queue_size = fetchQueue[tid].size();
    input.selectedTid = tid;
    input.selectedCommitRobSquashing =
        snapshot_commit->commitInfo[tid].robSquashing;
    if (queue_size < width) {
        FetchPrepareInput fetch_input;
        FetchPrepareResult fetch_result;
        const bool decode_blocked = decode_to_fetch.block[tid];
        const auto queue_state = [&] {
            fetch_input = buildFetchPrepareInput(cycle, tid);
            fetch_result = prepareFetchControl(fetch_input);
            return classifyFutureQueueNotReadyState(
                    fetch_input, fetch_result, decode_blocked);
        }();
        if (queue_state != FutureQueueNotReadyState::FrontendNotReady) {
            return skip(FutureDecodeQueueInputSkipReason::FetchQueueNotReady,
                        tid, queue_size);
        }
        const StallReason visible_stall =
            !stallReason.empty() ? stallReason[0] : StallReason::NoStall;
        if (queue_size == 0 &&
            (visible_stall != StallReason::OtherFetchStall ||
             fetch_input.cacheStatus != CacheIdle)) {
            return skip(FutureDecodeQueueInputSkipReason::FetchQueueNotReady,
                        tid, queue_size);
        }

        input.acceptedShortQueue = true;
        input.shortQueueState = queue_state;
        input.instSeqNumCount = queue_size;
        for (unsigned i = 0; i < MaxWidth; ++i)
            input.shortQueueCurrentStallReason[i] =
                i < stallReason.size() ? stallReason[i] : StallReason::NoStall;
        auto it = fetchQueue[tid].begin();
        for (unsigned i = 0; i < queue_size; ++i, ++it) {
            if (it == fetchQueue[tid].end() || !(*it))
                return skip(FutureDecodeQueueInputSkipReason::MissingInst);
            input.instSeqNums[i] = (*it)->seqNum;
        }
        return true;
    }

    input.instSeqNumCount = width;
    auto it = fetchQueue[tid].begin();
    for (unsigned i = 0; i < width; ++i, ++it) {
        if (it == fetchQueue[tid].end() || !(*it))
            return skip(FutureDecodeQueueInputSkipReason::MissingInst);
        input.instSeqNums[i] = (*it)->seqNum;
    }

    return true;
}

bool
Fetch::previewFutureDecodeQueue(
        const FutureDecodeQueueInput &input,
        unsigned &size,
        std::vector<StallReason> &reasons,
        std::vector<InstSeqNum> &inst_seq_nums,
        FetchToDecodePrepareResult *prepare_result) const
{
    const unsigned width = std::min<unsigned>(input.decodeWidth, MaxWidth);
    FetchToDecodePrepareResult prepare;
    prepare.cycle = input.cycle;

    if (input.allThreadsBlocked) {
        if (input.blockedTid == InvalidThreadID)
            return false;
        size = 0;
        reasons.assign(width, input.decodeToFetch.reason[input.blockedTid]);
        inst_seq_nums.clear();
        prepare.allThreadsBlocked = true;
        for (int i = 0; i < width; ++i) {
            prepare.stallReason[i] =
                input.decodeToFetch.reason[input.blockedTid];
        }
        for (int tid = 0; tid < input.numThreads; ++tid) {
            if (input.decodeToFetch.block[tid])
                prepare.decodeStalls++;
        }
        if (prepare_result)
            *prepare_result = prepare;
        return true;
    }

    const ThreadID tid = input.selectedTid;
    if (tid >= input.numThreads ||
        (!input.acceptedShortQueue && input.instSeqNumCount < width)) {
        return false;
    }

    size = input.acceptedShortQueue ? input.instSeqNumCount : width;
    prepare.selectedTid = tid;
    prepare.selectedBlocked = input.decodeToFetch.block[tid];
    prepare.instsToDecode = size;
    prepare.wroteToTimeBuffer = size != 0;
    if (prepare.selectedBlocked)
        prepare.decodeStalls++;

    if (input.decodeToFetch.block[tid]) {
        reasons.assign(width, input.decodeToFetch.reason[tid]);
        for (int i = 0; i < width; ++i)
            prepare.stallReason[i] = input.decodeToFetch.reason[tid];
    } else {
        for (int i = 0; i < width; ++i) {
            if (i < size) {
                prepare.stallReason[i] = StallReason::NoStall;
            } else if (size == 0) {
                const StallReason reason =
                    input.shortQueueCurrentStallReason[0] !=
                    StallReason::NoStall ?
                    input.shortQueueCurrentStallReason[0] :
                    StallReason::OtherFetchStall;
                prepare.stallReason[i] = reason;
            } else {
                prepare.stallReason[i] = StallReason::FetchFragStall;
            }
        }
        reasons.assign(prepare.stallReason,
                       prepare.stallReason + width);
    }

    if (!input.decodeToFetch.block[tid] && !input.selectedCommitRobSquashing) {
        const int unused_slots =
            static_cast<int>(input.decodeWidth) - static_cast<int>(size);
        if (unused_slots > 0) {
            prepare.fetchBubbles += unused_slots;
            if (unused_slots == static_cast<int>(input.decodeWidth))
                prepare.fetchBubblesMax++;
        }
    }

    inst_seq_nums.clear();
    inst_seq_nums.reserve(size);
    for (unsigned i = 0; i < size; ++i)
        inst_seq_nums.push_back(input.instSeqNums[i]);

    if (prepare_result)
        *prepare_result = prepare;

    return true;
}

bool
Fetch::previewFutureDecodeQueue(Cycles cycle,
                                const StallSignalLatch &decode_to_fetch,
                                const TimeStruct *snapshot_decode,
                                const TimeStruct *snapshot_commit,
                                unsigned &size,
                                std::vector<StallReason> &reasons,
                                std::vector<InstSeqNum> &inst_seq_nums,
                                FetchToDecodePrepareResult *prepare_result)
                                const
{
    FutureDecodeQueueInput input;
    if (!buildFutureDecodeQueueInput(
                cycle, decode_to_fetch, snapshot_decode, snapshot_commit,
                input)) {
        return false;
    }

    return previewFutureDecodeQueue(
            input, size, reasons, inst_seq_nums, prepare_result);
}

Fetch::FetchBackwardInput
Fetch::backwardInput(Cycles cycle) const
{
    FetchBackwardInput input;
    const int decode_to_fetch_offset = -static_cast<int>(
            static_cast<uint64_t>(decodeToFetchDelay));
    const int iew_to_fetch_offset = -static_cast<int>(
            static_cast<uint64_t>(iewToFetchDelay));
    const int commit_to_fetch_offset = -static_cast<int>(
            static_cast<uint64_t>(commitToFetchDelay));

    input.decode = cpu->pipelineInputFetchBackward(
            cycle, decode_to_fetch_offset);
    input.iew = cpu->pipelineInputFetchBackward(cycle, iew_to_fetch_offset);
    input.commit = cpu->pipelineInputFetchBackward(
            cycle, commit_to_fetch_offset);

    if (!input.decode)
        input.decode = &(*fromDecode);
    if (!input.iew)
        input.iew = &(*fromIEW);
    if (!input.commit)
        input.commit = &(*fromCommit);

    return input;
}

bool
Fetch::initializeTickState(const FetchBackwardInput &input)
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;
    setAllFetchStalls(StallReason::NoStall);

    // get the distribution of fetch status
    fetchStats.fetchStatusDist[fetchStatus[0]]++;

    // Check signal updates for all active threads
    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid, input);
        status_change =  status_change || updated_status;
    }

    DPRINTF(Fetch, "Running stage.\n");

    if (input.commit->commitInfo[0].emptyROB) {
        waitForVsetvl = false;
    }

    return status_change;
}

void
Fetch::fetchAndProcessInstructions(bool status_change,
                                   const FetchBackwardInput &input)
{
    // Fetch instructions from active threads
    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        // Fetch each of the actively fetching threads.
        fetch(status_change);
    }

    // Pass stall reasons to decode stage
    toDecode->fetchStallReason = stallReason;

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }

    // Handle interrupt processing in full system mode
    handleInterrupts(input);

    // Send instructions to decode stage, update stall reasons and measure frontend bubbles.
    sendInstructionsToDecode(input);
}

void
Fetch::handleInterrupts(const FetchBackwardInput &input)
{
    if (FullSystem) {
        if (input.commit->commitInfo[0].interruptPending) {
            DPRINTF(Fetch, "Set interrupt pending.\n");
            interruptPending = true;
        }

        if (input.commit->commitInfo[0].clearInterrupt) {
            DPRINTF(Fetch, "Clear interrupt pending.\n");
            interruptPending = false;
        }
    }
}

bool
Fetch::fetchBlocked(ThreadID tid) const
{
    if (stallSignalBank) {
        return cpu->stallSignalSnapshotOrCurrent(
            cpu->curCycle(), StallSignalEdge::DecodeToFetch)
            .block[tid];
    }

    return stallSig->blockFetch[tid];
}

StallReason
Fetch::fetchBlockedReason(ThreadID tid) const
{
    if (stallSignalBank) {
        return cpu->stallSignalSnapshotOrCurrent(
            cpu->curCycle(), StallSignalEdge::DecodeToFetch)
            .reason[tid];
    }

    return stallSig->fetchBlockReason[tid];
}

void
Fetch::sendInstructionsToDecode(const FetchBackwardInput &input)
{
    if (cpu->getTaskRuntime().enabled()) {
        const auto prepare =
            runFetchToDecodePrepare(cpu->curCycle(), input);
        applyFetchToDecodePrepareResult(prepare);
        return;
    }

    // Reset the number of instructions we've fetched
    numInst = 0;

    bool any_thread_active = false;
    for (int i = 0; i < numThreads; i++) {
        if (!fetchBlocked(i)) {
            any_thread_active = true;
            break;
        }
    }
    if (!any_thread_active) {
        // All threads are blocked, no instructions to send
        ThreadID blocked_tid = InvalidThreadID;
        for (int i = 0; i < numThreads; i++) {
            if (fetchBlocked(i)) {
                blocked_tid = i;
                break;
            }
        }

        if (blocked_tid != InvalidThreadID) {
            setAllFetchStalls(fetchBlockedReason(blocked_tid));
        }

        toDecode->fetchStallReason = stallReason;

        for (int i = 0; i < numThreads; i++) {
            measureFrontendBubbles(0, i, input);
        }
        return;
    }

    ThreadID tid = 0; // TODO: smt support

    // fetch totally stalled
    if (fetchBlocked(tid)) {
        // If decode stalled, use decode's stall reason
        DPRINTF(Fetch, "[tid:%i] Fetch stalled\n", tid);
        setAllFetchStalls(fetchBlockedReason(tid));
    }

    int insts_to_decode = 0;
    auto& insts = fetchQueue[tid];
    while (!insts.empty() && insts_to_decode < decodeWidth) {
        const auto& inst = insts.front();
        toDecode->insts[toDecode->size++] = inst;
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                "from fetch queue. Fetch queue size: %i.\n",
                tid, inst->seqNum, insts.size());

        wroteToTimeBuffer = true;
        insts.pop_front();
        insts_to_decode++;
    }

    // Update stall reasons based on fetch/decode status
    updateStallReasons(insts_to_decode, tid);

    // Intel TopDown method for measuring frontend bubbles
    measureFrontendBubbles(insts_to_decode, tid, input);

    // If there was activity this cycle, inform the CPU of it
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

Fetch::FetchToDecodePrepareInput
Fetch::buildFetchToDecodePrepareInput(
        Cycles cycle, const FetchBackwardInput &input) const
{
    FetchToDecodePrepareInput prepare_input;
    prepare_input.cycle = cycle;
    prepare_input.numThreads = numThreads;
    prepare_input.decodeWidth = decodeWidth;

    for (int i = 0; i < MaxWidth; ++i) {
        prepare_input.currentStallReason[i] =
            i < stallReason.size() ? stallReason[i] : StallReason::NoStall;
    }

    for (int tid = 0; tid < numThreads; ++tid) {
        prepare_input.blocked[tid] = fetchBlocked(tid);
        prepare_input.blockReason[tid] = fetchBlockedReason(tid);
        prepare_input.fetchQueueSize[tid] = fetchQueue[tid].size();
        prepare_input.commitRobSquashing[tid] =
            input.commit->commitInfo[tid].robSquashing;
    }

    return prepare_input;
}

Fetch::FetchToDecodePrepareResult
Fetch::prepareFetchToDecodeControl(
        const FetchToDecodePrepareInput &input) const
{
    FetchToDecodePrepareResult result;
    result.cycle = input.cycle;

    const unsigned width = std::min<unsigned>(input.decodeWidth, MaxWidth);
    for (int i = 0; i < MaxWidth; ++i) {
        result.stallReason[i] = i < width ?
            input.currentStallReason[i] : StallReason::NoStall;
    }

    auto measure_bubbles = [&input, &result](ThreadID tid,
                                             unsigned insts_to_decode) {
        if (!input.blocked[tid] && !input.commitRobSquashing[tid]) {
            const int unused_slots =
                static_cast<int>(input.decodeWidth) -
                static_cast<int>(insts_to_decode);
            if (unused_slots > 0) {
                result.fetchBubbles += unused_slots;
                if (unused_slots == static_cast<int>(input.decodeWidth))
                    result.fetchBubblesMax++;
            }
        }

        if (input.blocked[tid])
            result.decodeStalls++;
    };

    bool any_thread_active = false;
    for (int tid = 0; tid < input.numThreads; ++tid) {
        if (!input.blocked[tid]) {
            any_thread_active = true;
            break;
        }
    }

    if (!any_thread_active) {
        result.allThreadsBlocked = true;
        ThreadID blocked_tid = InvalidThreadID;
        for (int tid = 0; tid < input.numThreads; ++tid) {
            if (input.blocked[tid]) {
                blocked_tid = tid;
                break;
            }
        }

        if (blocked_tid != InvalidThreadID) {
            for (int i = 0; i < width; ++i)
                result.stallReason[i] = input.blockReason[blocked_tid];
        }

        for (int tid = 0; tid < input.numThreads; ++tid)
            measure_bubbles(tid, 0);
        return result;
    }

    result.selectedTid = 0; // Preserve the current non-SMT send policy.
    const ThreadID tid = result.selectedTid;
    result.selectedBlocked = input.blocked[tid];
    result.instsToDecode = std::min<unsigned>(
            input.fetchQueueSize[tid], width);
    result.wroteToTimeBuffer = result.instsToDecode != 0;

    if (input.blocked[tid]) {
        for (int i = 0; i < width; ++i)
            result.stallReason[i] = input.blockReason[tid];
    } else if (result.instsToDecode == 0) {
        const StallReason reason = width > 0 &&
            input.currentStallReason[0] != StallReason::NoStall ?
            input.currentStallReason[0] : StallReason::OtherFetchStall;
        for (int i = 0; i < width; ++i)
            result.stallReason[i] = reason;
    } else {
        for (int i = 0; i < width; ++i) {
            result.stallReason[i] = i < result.instsToDecode ?
                StallReason::NoStall : StallReason::FetchFragStall;
        }
    }

    measure_bubbles(tid, result.instsToDecode);
    return result;
}

bool
Fetch::sameFetchToDecodePrepareResult(
        const FetchToDecodePrepareResult &lhs,
        const FetchToDecodePrepareResult &rhs) const
{
    if (lhs.cycle != rhs.cycle ||
        lhs.selectedTid != rhs.selectedTid ||
        lhs.allThreadsBlocked != rhs.allThreadsBlocked ||
        lhs.selectedBlocked != rhs.selectedBlocked ||
        lhs.instsToDecode != rhs.instsToDecode ||
        lhs.fetchBubbles != rhs.fetchBubbles ||
        lhs.fetchBubblesMax != rhs.fetchBubblesMax ||
        lhs.decodeStalls != rhs.decodeStalls ||
        lhs.wroteToTimeBuffer != rhs.wroteToTimeBuffer) {
        return false;
    }

    for (int i = 0; i < MaxWidth; ++i) {
        if (lhs.stallReason[i] != rhs.stallReason[i])
            return false;
    }

    return true;
}

Fetch::FetchToDecodePrepareResult
Fetch::runFetchToDecodePrepare(Cycles cycle, const FetchBackwardInput &input)
{
    auto prepare_input = std::make_shared<FetchToDecodePrepareInput>(
            buildFetchToDecodePrepareInput(cycle, input));
    auto result = std::make_shared<FetchToDecodePrepareResult>();

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return prepareFetchToDecodeControl(*prepare_input);

    recordFutureQueueNotReadyOutcome(cycle, *prepare_input);

    if (pendingFutureToDecodePrepare.valid) {
        if (pendingFutureToDecodePrepare.result.cycle == cycle) {
            *result = pendingFutureToDecodePrepare.result;
            pendingFutureToDecodePrepare.valid = false;
            fetchStats.futureToDecodePrepareReuses++;

            if (runtime.selfTestEnabled()) {
                const FetchToDecodePrepareResult expected =
                    prepareFetchToDecodeControl(*prepare_input);
                fetchStats.futureToDecodePrepareChecks++;
                if (sameFetchToDecodePrepareResult(*result, expected)) {
                    fetchStats.futureToDecodePrepareMatches++;
                } else {
                    fetchStats.futureToDecodePrepareMismatches++;
                    recordFutureToDecodePrepareMismatchReasons(
                            expected, *result);
                }
            }

            return *result;
        }

        fetchStats.futureToDecodePrepareChecks++;
        fetchStats.futureToDecodePrepareStale++;
        pendingFutureToDecodePrepare.valid = false;
    }

    bool all_threads_blocked = true;
    for (int tid = 0; tid < prepare_input->numThreads; ++tid) {
        if (!prepare_input->blocked[tid]) {
            all_threads_blocked = false;
            break;
        }
    }

    if (all_threads_blocked) {
        fetchStats.toDecodePrepareAllBlocked++;
        return prepareFetchToDecodeControl(*prepare_input);
    }

    fetchStats.toDecodePrepareTasks++;
    const TaskOrderKey order{
        cycle, TaskStage::Fetch, 3, InvalidThreadID, 0};
    runtime.submitWeak(
            order,
            std::max(1u, prepare_input->numThreads +
                         prepare_input->decodeWidth),
            [this, prepare_input, result] {
                *result = prepareFetchToDecodeControl(*prepare_input);
            },
            [this, result] {
                fetchStats.toDecodePrepareMerges++;
                if (result->allThreadsBlocked)
                    fetchStats.toDecodePrepareAllBlocked++;
            });
    runtime.waitForOrder(order);

    return *result;
}

void
Fetch::recordFutureToDecodePrepareProbe()
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    fetchStats.futureToDecodePrepareProbes++;
}

void
Fetch::recordFutureToDecodePrepareSkipped()
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    fetchStats.futureToDecodePrepareSkipped++;
}

void
Fetch::recordFutureToDecodePrepareMismatchReasons(
        const FetchToDecodePrepareResult &expected,
        const FetchToDecodePrepareResult &actual)
{
    auto record = [this](FutureToDecodePrepareMismatchReason reason) {
        fetchStats.futureToDecodePrepareMismatchReasons[
            static_cast<unsigned>(reason)]++;
    };

    if (actual.cycle != expected.cycle)
        record(FutureToDecodePrepareMismatchReason::Cycle);
    if (actual.selectedTid != expected.selectedTid)
        record(FutureToDecodePrepareMismatchReason::SelectedTid);
    if (actual.allThreadsBlocked != expected.allThreadsBlocked)
        record(FutureToDecodePrepareMismatchReason::AllThreadsBlocked);
    if (actual.selectedBlocked != expected.selectedBlocked)
        record(FutureToDecodePrepareMismatchReason::SelectedBlocked);
    if (actual.instsToDecode != expected.instsToDecode)
        record(FutureToDecodePrepareMismatchReason::InstsToDecode);
    if (actual.fetchBubbles != expected.fetchBubbles)
        record(FutureToDecodePrepareMismatchReason::FetchBubbles);
    if (actual.fetchBubblesMax != expected.fetchBubblesMax)
        record(FutureToDecodePrepareMismatchReason::FetchBubblesMax);
    if (actual.decodeStalls != expected.decodeStalls)
        record(FutureToDecodePrepareMismatchReason::DecodeStalls);
    if (actual.wroteToTimeBuffer != expected.wroteToTimeBuffer)
        record(FutureToDecodePrepareMismatchReason::WroteToTimeBuffer);

    for (int i = 0; i < MaxWidth; ++i) {
        if (actual.stallReason[i] != expected.stallReason[i]) {
            record(FutureToDecodePrepareMismatchReason::StallReason);
            break;
        }
    }
}

void
Fetch::recordFutureDecodeQueueInputAccepted(
        const FutureDecodeQueueInput &input)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled() || !input.acceptedShortQueue)
        return;

    const auto state_index = static_cast<unsigned>(input.shortQueueState);
    if (state_index < static_cast<unsigned>(
                FutureQueueNotReadyState::NumStates)) {
        fetchStats.futureInputQueueNotReadyAcceptedStates[state_index]++;
    }
    fetchStats.futureInputQueueNotReadyAcceptedSizes[
        std::min<unsigned>(input.instSeqNumCount, MaxWidth)]++;
    const StallReason reason = input.shortQueueCurrentStallReason[0];
    if (reason < NumStallReasons)
        fetchStats.futureInputQueueNotReadyAcceptedStallReasons[reason]++;
}

void
Fetch::recordFutureQueueNotReadyOutcomeCount(
        FutureQueueNotReadyState state,
        FutureQueueNotReadyOutcome outcome)
{
    const auto state_index = static_cast<unsigned>(state);
    const auto outcome_index = static_cast<unsigned>(outcome);
    if (state_index >= static_cast<unsigned>(
                FutureQueueNotReadyState::NumStates) ||
        outcome_index >= static_cast<unsigned>(
                FutureQueueNotReadyOutcome::NumOutcomes)) {
        return;
    }

    fetchStats.futureInputQueueNotReadyStateOutcomes
        [state_index][outcome_index]++;
}

void
Fetch::recordFutureDecodeQueueInputSkipped(
        const FutureDecodeQueueInputSkipInfo &skip_info)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    const auto index = static_cast<unsigned>(skip_info.reason);
    if (index >= static_cast<unsigned>(
                FutureDecodeQueueInputSkipReason::NumReasons)) {
        return;
    }

    fetchStats.futureInputSkipReasons[index]++;

    if (skip_info.reason !=
            FutureDecodeQueueInputSkipReason::FetchQueueNotReady) {
        return;
    }

    const auto state_index = static_cast<unsigned>(skip_info.queueState);
    if (state_index < static_cast<unsigned>(
                FutureQueueNotReadyState::NumStates)) {
        fetchStats.futureInputQueueNotReadyStates[state_index]++;
    }

    if (pendingFutureQueueNotReady.valid) {
        fetchStats.futureInputQueueNotReadyOutcomes[
            static_cast<unsigned>(FutureQueueNotReadyOutcome::Stale)]++;
        recordFutureQueueNotReadyOutcomeCount(
                pendingFutureQueueNotReady.state,
                FutureQueueNotReadyOutcome::Stale);
    }

    pendingFutureQueueNotReady.valid = true;
    pendingFutureQueueNotReady.cycle = skip_info.cycle;
    pendingFutureQueueNotReady.tid = skip_info.tid;
    pendingFutureQueueNotReady.queueSize = skip_info.queueSize;
    pendingFutureQueueNotReady.decodeWidth = skip_info.decodeWidth;
    pendingFutureQueueNotReady.state = skip_info.queueState;
}

void
Fetch::recordFutureQueueNotReadyOutcome(
        Cycles cycle, const FetchToDecodePrepareInput &input)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled() || !pendingFutureQueueNotReady.valid)
        return;

    if (pendingFutureQueueNotReady.cycle != cycle) {
        fetchStats.futureInputQueueNotReadyOutcomes[
            static_cast<unsigned>(FutureQueueNotReadyOutcome::Stale)]++;
        recordFutureQueueNotReadyOutcomeCount(
                pendingFutureQueueNotReady.state,
                FutureQueueNotReadyOutcome::Stale);
        pendingFutureQueueNotReady.valid = false;
        return;
    }

    const ThreadID tid = pendingFutureQueueNotReady.tid;
    FutureQueueNotReadyOutcome outcome = FutureQueueNotReadyOutcome::Stale;
    const unsigned candidate_size = pendingFutureQueueNotReady.queueSize;
    unsigned actual_size = 0;

    if (tid != InvalidThreadID && tid < input.numThreads) {
        actual_size = input.fetchQueueSize[tid];
        const unsigned width =
            std::min<unsigned>(input.decodeWidth, MaxWidth);
        if (input.blocked[tid]) {
            outcome = FutureQueueNotReadyOutcome::Blocked;
        } else if (actual_size < candidate_size) {
            outcome = FutureQueueNotReadyOutcome::QueueShrank;
        } else if (actual_size >= width) {
            outcome = FutureQueueNotReadyOutcome::FilledToWidth;
        } else if (actual_size > candidate_size) {
            outcome = FutureQueueNotReadyOutcome::PartialSupply;
        } else {
            outcome = FutureQueueNotReadyOutcome::NoSupplyStillNotReady;
        }
    }

    fetchStats.futureInputQueueNotReadyCandidateInsts += candidate_size;
    fetchStats.futureInputQueueNotReadyActualInsts += actual_size;
    fetchStats.futureInputQueueNotReadyOutcomes[
        static_cast<unsigned>(outcome)]++;
    recordFutureQueueNotReadyOutcomeCount(
            pendingFutureQueueNotReady.state, outcome);
    pendingFutureQueueNotReady.valid = false;
}

void
Fetch::setPendingFutureToDecodePrepare(
        const FetchToDecodePrepareResult &result)
{
    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return;

    if (pendingFutureToDecodePrepare.valid)
        fetchStats.futureToDecodePrepareStale++;

    pendingFutureToDecodePrepare.result = result;
    pendingFutureToDecodePrepare.valid = true;
    fetchStats.futureToDecodePrepareMerges++;
}

void
Fetch::applyFetchToDecodePrepareResult(
        const FetchToDecodePrepareResult &result)
{
    // Reset the number of instructions we've fetched.
    numInst = 0;

    const unsigned width = std::min<unsigned>(decodeWidth, MaxWidth);
    for (int i = 0; i < width; ++i)
        stallReason[i] = result.stallReason[i];
    toDecode->fetchStallReason = stallReason;

    fetchStats.fetchBubbles += result.fetchBubbles;
    fetchStats.fetchBubbles_max += result.fetchBubblesMax;
    fetchStats.decodeStalls += result.decodeStalls;

    if (result.allThreadsBlocked)
        return;

    const ThreadID tid = result.selectedTid;
    fatal_if(tid >= numThreads,
             "Fetch-to-decode prepare selected invalid tid %i.", tid);
    if (result.selectedBlocked)
        DPRINTF(Fetch, "[tid:%i] Fetch stalled\n", tid);
    fatal_if(fetchQueue[tid].size() < result.instsToDecode,
             "Fetch queue for tid %i changed during fetch-to-decode "
             "prepare: prepared %u entries, current size %zu.",
             tid, result.instsToDecode, fetchQueue[tid].size());

    unsigned insts_to_decode = 0;
    auto &insts = fetchQueue[tid];
    while (!insts.empty() && insts_to_decode < result.instsToDecode) {
        const auto &inst = insts.front();
        toDecode->insts[toDecode->size++] = inst;
        DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                "from fetch queue. Fetch queue size: %i.\n",
                tid, inst->seqNum, insts.size());

        wroteToTimeBuffer = true;
        insts.pop_front();
        insts_to_decode++;
    }

    fatal_if(insts_to_decode != result.instsToDecode,
             "Fetch-to-decode prepare sent %u insts but prepared %u.",
             insts_to_decode, result.instsToDecode);

    if (result.wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
Fetch::updateStallReasons(unsigned insts_to_decode, ThreadID tid)
{
    if (fetchBlocked(tid)) {
        setAllFetchStalls(fetchBlockedReason(tid));
    } else if (insts_to_decode == 0) {
        // fetch stalled
        if (stallReason[0] != StallReason::NoStall) {
            // previously set stall reason
            setAllFetchStalls(stallReason[0]);
        } else {
            setAllFetchStalls(StallReason::OtherFetchStall);
        }
    } else {
        // fetch partially stalled or no stall
        for (int i = 0; i < stallReason.size(); i++) {
            if (i < insts_to_decode)
                stallReason[i] = StallReason::NoStall;
            else {
                stallReason[i] = StallReason::FetchFragStall;
            }
        }
    }

    toDecode->fetchStallReason = stallReason;
}

void
Fetch::measureFrontendBubbles(unsigned insts_to_decode, ThreadID tid,
                              const FetchBackwardInput &input)
{
    // Intel TopDown method for measuring frontend bubbles
    // Count unutilized issue slots when backend is not stalled (decode not stalled)
    // For N-wide machine, if frontend supplies 0 instructions:
    // - fetchBubbles += N (count total empty slots)
    // - fetchBubbles_max += 1 (count occurrence of all slots being empty)
    if (!fetchBlocked(tid) && !input.commit->commitInfo[tid].robSquashing) {
        // backend not stalled
        int unused_slots = decodeWidth - insts_to_decode;
        if (unused_slots > 0) {
            // has empty slots
            fetchStats.fetchBubbles += unused_slots; // add number of empty slots
            if (unused_slots == decodeWidth) {
                // all slots empty, insts_to_decode == 0
                fetchStats.fetchBubbles_max++; // count max bubble occurrence
            }
        }
    }

    if (fetchBlocked(tid)) {
        fetchStats.decodeStalls++;
    }
}

bool
Fetch::checkSignalsAndUpdate(ThreadID tid, const FetchBackwardInput &input)
{
    // Check squash signals from commit.
    bool commitSquashed = handleCommitSignals(tid, input.commit);

    handleIEWSignals(tid, input.iew);

    if (commitSquashed) {
        return true;
    }

    if (handleDecodeSquash(tid, input.decode)) {
        return true;
    }

    if (fetchStatus[tid] == Blocked ||
        fetchStatus[tid] == Squashing) {
        // Switch status to running if fetch isn't being told to block or
        // squash this cycle.
        DPRINTF(Fetch, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        setThreadStatus(tid, Running);

        return true;
    }

    // Handle WaitingCache state: check if cache request is complete
    if (fetchStatus[tid] == WaitingCache &&
        threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
        // Cache access completed, transition to Running
        setThreadStatus(tid, Running);
        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause fetch to change its status.  Fetch remains the same as before.
    return false;
}

void
Fetch::handleIEWSignals(ThreadID tid, const TimeStruct *iew_input)
{
    // Currently resolve stage training is a btb-only feature
    if (!isBTBPred()) {
        return;
    }

    const bool had_pending_resolve = !resolveQueue.empty();
    const ResolvePrepareResult prepare =
        runResolvePrepare(cpu->curCycle(), tid, iew_input);
    applyResolvePrepareResult(prepare);

    // Process only entries that were already pending before this cycle.
    // This preserves a cycle of separation between IEW producing resolved CFIs
    // and fetch consuming them as predictor resolved updates.
    const ResolveDequeuePrepareResult dequeue_prepare =
        runResolveDequeuePrepare(cpu->curCycle(), had_pending_resolve);
    if (dequeue_prepare.processFront) {
        fatal_if(resolveQueue.empty(),
                 "Resolve dequeue prepare expected a front entry.");
        fatal_if(resolveQueue.front().resolvedFTQId !=
                     dequeue_prepare.streamId,
                 "Resolve dequeue prepare stream mismatch: prepared %llu, "
                 "front %llu.",
                 static_cast<unsigned long long>(
                         dequeue_prepare.streamId),
                 static_cast<unsigned long long>(
                         resolveQueue.front().resolvedFTQId));
        fatal_if(resolveQueue.front().tid != dequeue_prepare.tid,
                 "Resolve dequeue prepare tid mismatch: prepared %i, "
                 "front %i.",
                 dequeue_prepare.tid, resolveQueue.front().tid);
        unsigned int stream_id = dequeue_prepare.streamId;
        const ThreadID resolve_tid = dequeue_prepare.tid;
        dbpbtb->prepareResolveUpdateEntries(stream_id, resolve_tid);
        for (const auto resolvedInstPC :
             dequeue_prepare.resolvedInstPC) {
            dbpbtb->markCFIResolved(stream_id, resolvedInstPC,
                                    resolve_tid);
        }
        bool success = dbpbtb->resolveUpdate(stream_id, resolve_tid);
        if (success) {
            dbpbtb->notifyResolveSuccess();
            resolveQueue.pop_front();
            fetchStats.resolveDequeueCount++;
        } else {
            dbpbtb->notifyResolveFailure(resolve_tid);
        }
    }
}

Fetch::ResolvePrepareInput
Fetch::buildResolvePrepareInput(
        Cycles cycle, ThreadID tid, const TimeStruct *iew_input) const
{
    ResolvePrepareInput input;
    input.cycle = cycle;
    input.resolveQueueSize = resolveQueueSize;
    input.queuedEntries.reserve(resolveQueue.size());
    for (const auto &entry : resolveQueue) {
        ResolvePrepareInput::QueuedEntry queued;
        queued.tid = entry.tid;
        queued.ftqId = entry.resolvedFTQId;
        input.queuedEntries.push_back(queued);
    }

    const auto &incoming = iew_input->iewInfo[tid].resolvedCFIs;
    input.incoming.reserve(incoming.size());
    for (const auto &resolved : incoming) {
        ResolvePrepareInput::IncomingCFI cfi;
        cfi.tid = tid;
        cfi.ftqId = resolved.ftqId;
        cfi.pc = resolved.pc;
        input.incoming.push_back(cfi);
    }

    return input;
}

Fetch::ResolveDequeuePrepareInput
Fetch::buildResolveDequeuePrepareInput(
        Cycles cycle, bool had_pending_before_enqueue) const
{
    ResolveDequeuePrepareInput input;
    input.cycle = cycle;
    input.hadPendingBeforeEnqueue = had_pending_before_enqueue;

    if (had_pending_before_enqueue && !resolveQueue.empty()) {
        const auto &entry = resolveQueue.front();
        input.frontValid = true;
        input.tid = entry.tid;
        input.streamId = entry.resolvedFTQId;
        input.resolvedInstPC = entry.resolvedInstPC;
    }

    return input;
}

Fetch::ResolveDequeuePrepareResult
Fetch::prepareResolveDequeueControl(
        const ResolveDequeuePrepareInput &input) const
{
    ResolveDequeuePrepareResult result;
    result.cycle = input.cycle;
    if (!input.hadPendingBeforeEnqueue || !input.frontValid)
        return result;

    result.processFront = true;
    result.tid = input.tid;
    result.streamId = input.streamId;
    result.resolvedInstPC = input.resolvedInstPC;
    return result;
}

Fetch::ResolveDequeuePrepareResult
Fetch::runResolveDequeuePrepare(
        Cycles cycle, bool had_pending_before_enqueue)
{
    ResolveDequeuePrepareInput input =
        buildResolveDequeuePrepareInput(cycle, had_pending_before_enqueue);

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return prepareResolveDequeueControl(input);

    if (!input.hadPendingBeforeEnqueue || !input.frontValid) {
        fetchStats.resolveDequeuePrepareNoWork++;
        return prepareResolveDequeueControl(input);
    }

    fetchStats.resolveDequeuePrepareTasks++;
    const TaskOrderKey order{cycle, TaskStage::Fetch, 1, InvalidThreadID, 0};
    auto input_ptr = std::make_shared<ResolveDequeuePrepareInput>(input);
    auto result = std::make_shared<ResolveDequeuePrepareResult>();
    runtime.submitWeak(
            order,
            std::max(1u, static_cast<unsigned>(
                    input_ptr->resolvedInstPC.size() + 1)),
            [this, input_ptr, result] {
                *result = prepareResolveDequeueControl(*input_ptr);
            },
            [this, had_pending_before_enqueue, result] {
                fetchStats.resolveDequeuePrepareMerges++;
                fetchStats.resolveDequeuePrepareCFIs +=
                    result->resolvedInstPC.size();
                verifyResolveDequeuePrepareResult(
                        had_pending_before_enqueue, *result);
            });
    runtime.waitForOrder(order);

    return *result;
}

void
Fetch::verifyResolveDequeuePrepareResult(
        bool had_pending_before_enqueue,
        const ResolveDequeuePrepareResult &result)
{
    const ResolveDequeuePrepareResult expected =
        prepareResolveDequeueControl(buildResolveDequeuePrepareInput(
                result.cycle, had_pending_before_enqueue));

    auto mismatch = [&] {
        if (result.processFront != expected.processFront ||
            result.tid != expected.tid ||
            result.streamId != expected.streamId ||
            result.resolvedInstPC.size() !=
                expected.resolvedInstPC.size()) {
            return true;
        }
        for (size_t i = 0; i < expected.resolvedInstPC.size(); ++i) {
            if (result.resolvedInstPC[i] != expected.resolvedInstPC[i])
                return true;
        }
        return false;
    };

    if (mismatch()) {
        fetchStats.resolveDequeuePrepareMismatches++;
        panic("Fetch resolve dequeue prepare mismatch: prepared "
              "process=%d tid=%i stream=%llu cfi=%zu, expected process=%d "
              "tid=%i stream=%llu cfi=%zu",
              result.processFront,
              result.tid,
              static_cast<unsigned long long>(result.streamId),
              result.resolvedInstPC.size(),
              expected.processFront,
              expected.tid,
              static_cast<unsigned long long>(expected.streamId),
              expected.resolvedInstPC.size());
    }
}

Fetch::ResolvePrepareResult
Fetch::prepareResolveControl(const ResolvePrepareInput &input) const
{
    ResolvePrepareResult result;
    result.cycle = input.cycle;
    result.occupancyAfterEnqueue = input.queuedEntries.size();

    if (input.resolveQueueSize &&
        input.queuedEntries.size() > input.resolveQueueSize - 4) {
        result.queueFull = true;
        result.enqueueFailCount = input.incoming.size();
        return result;
    }

    std::vector<ResolvePrepareInput::QueuedEntry> queued_entries =
        input.queuedEntries;
    result.appendOps.reserve(input.incoming.size());
    for (const auto &resolved : input.incoming) {
        bool found = false;
        unsigned queue_index = 0;
        for (unsigned i = 0; i < queued_entries.size(); ++i) {
            if (queued_entries[i].tid == resolved.tid &&
                queued_entries[i].ftqId == resolved.ftqId) {
                found = true;
                queue_index = i;
                break;
            }
        }

        ResolvePrepareResult::AppendOp op;
        op.tid = resolved.tid;
        op.ftqId = resolved.ftqId;
        op.pc = resolved.pc;
        op.queueIndex = queue_index;
        if (!found) {
            op.createNewEntry = true;
            op.queueIndex = queued_entries.size();
            ResolvePrepareInput::QueuedEntry queued;
            queued.tid = resolved.tid;
            queued.ftqId = resolved.ftqId;
            queued_entries.push_back(queued);
            result.enqueueCount++;
        }
        result.appendOps.push_back(op);
    }

    result.occupancyAfterEnqueue = queued_entries.size();
    return result;
}

Fetch::ResolvePrepareResult
Fetch::runResolvePrepare(Cycles cycle, ThreadID tid,
                         const TimeStruct *iew_input)
{
    auto input = std::make_shared<ResolvePrepareInput>(
            buildResolvePrepareInput(cycle, tid, iew_input));
    auto result = std::make_shared<ResolvePrepareResult>();

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return prepareResolveControl(*input);

    if (input->incoming.empty()) {
        fetchStats.resolvePrepareNoIncoming++;
        return prepareResolveControl(*input);
    }

    fetchStats.resolvePrepareTasks++;
    const TaskOrderKey order{cycle, TaskStage::Fetch, 0, InvalidThreadID, 0};
    runtime.submitWeak(
            order,
            std::max(1u,
                static_cast<unsigned>(input->incoming.size() +
                                      input->queuedEntries.size())),
            [this, input, result] {
                *result = prepareResolveControl(*input);
            },
            [this, result] {
                fetchStats.resolvePrepareMerges++;
                if (result->queueFull)
                    fetchStats.resolvePrepareQueueFull++;
            });
    runtime.waitForOrder(order);

    return *result;
}

void
Fetch::applyResolvePrepareResult(const ResolvePrepareResult &result)
{
    if (result.queueFull) {
        fetchStats.resolveQueueFullEvents++;
        fetchStats.resolveEnqueueFailEvent += result.enqueueFailCount;
    } else {
        for (const auto &op : result.appendOps) {
            if (op.createNewEntry) {
                fatal_if(op.queueIndex != resolveQueue.size(),
                         "Resolve prepare expected to create queue index %u "
                         "but queue size is %zu.",
                         op.queueIndex, resolveQueue.size());
                ResolveQueueEntry new_entry;
                new_entry.tid = op.tid;
                new_entry.resolvedFTQId = op.ftqId;
                resolveQueue.push_back(std::move(new_entry));
            }

            fatal_if(op.queueIndex >= resolveQueue.size(),
                     "Resolve prepare append index %u is outside queue size "
                     "%zu.",
                     op.queueIndex, resolveQueue.size());
            auto &entry = resolveQueue[op.queueIndex];
            fatal_if(entry.tid != op.tid,
                     "Resolve prepare append tid mismatch: op %i, "
                     "queue %i.",
                     op.tid, entry.tid);
            fatal_if(entry.resolvedFTQId != op.ftqId,
                     "Resolve prepare append FTQ id mismatch: op %llu, "
                     "queue %llu.",
                     static_cast<unsigned long long>(op.ftqId),
                     static_cast<unsigned long long>(entry.resolvedFTQId));
            entry.resolvedInstPC.push_back(op.pc);
        }
        fetchStats.resolveEnqueueCount.sample(result.enqueueCount);
    }

    fetchStats.resolveQueueOccupancy.sample(resolveQueue.size());
    fatal_if(resolveQueue.size() != result.occupancyAfterEnqueue,
             "Resolve prepare occupancy mismatch: expected %llu, got %zu.",
             static_cast<unsigned long long>(result.occupancyAfterEnqueue),
             resolveQueue.size());
}

bool
Fetch::handleCommitSignals(ThreadID tid, const TimeStruct *commit_input)
{
    // Check squash signals from commit.
    if (!commit_input->commitInfo[tid].squash) {
        if (commit_input->commitInfo[tid].doneFtqId) {
            DPRINTF(DecoupleBP, "Commit stream Id: %lu\n",
                    commit_input->commitInfo[tid].doneFtqId);
            assert(dbpbtb);
            dbpbtb->commit(commit_input->commitInfo[tid].doneFtqId, tid);
        }
        return false;
    }

    // Check squash signals from commit.
    DPRINTF(Fetch,
            "[tid:%i] Squashing instructions due to squash "
            "from commit.\n",
            tid);

        InstSeqNum squash_seq = commit_input->commitInfo[tid].doneSeqNum;
        DynInstPtr squash_inst = commit_input->commitInfo[tid].squashInst;
        if (commit_input->commitInfo[tid].isTrapSquash &&
            commit_input->commitInfo[tid].traceTrapSkipInst) {
            squash_seq = commit_input->commitInfo[tid].traceTrapSeqNum;
            squash_inst = nullptr;
            DPRINTF(Fetch,
                    "[tid:%i] Trap squash with trace ctrl-flow fault: rollback seq=%llu (skip head)\n",
                    tid, static_cast<unsigned long long>(squash_seq));
        }

    // In any case, squash.
    squash(*commit_input->commitInfo[tid].pc, squash_seq,
           squash_inst, tid);

    localSquashVer.update(
            commit_input->commitInfo[tid].squashVersion.getVersion());
    DPRINTF(Fetch, "Updating squash version to %u\n", localSquashVer.getVersion());

    auto mispred_inst = commit_input->commitInfo[tid].mispredictInst;

    if (mispred_inst) {
        DPRINTF(Fetch, "Use mispred inst to redirect, treating as control squash\n");
        const auto corr_pc =
            commit_input->commitInfo[tid].pc->as<RiscvISA::PCState>();
        assert(dbpbtb);
        dbpbtb->controlSquash(mispred_inst->getFtqId(), mispred_inst->pcState(),
                              corr_pc, mispred_inst->staticInst,
                              mispred_inst->getInstBytes(),
                              commit_input->commitInfo[tid].branchTaken,
                              mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(), true);
    } else if (commit_input->commitInfo[tid].isTrapSquash) {
        DPRINTF(Fetch, "Treating as trap squash\n", tid);
        const auto trap_pc =
            commit_input->commitInfo[tid].pc->as<RiscvISA::PCState>();
        assert(dbpbtb);
        dbpbtb->trapSquash(
                commit_input->commitInfo[tid].squashedTargetId,
                commit_input->commitInfo[tid].committedPC,
                trap_pc, tid,
                commit_input->commitInfo[tid].squashedLoopIter);
    } else {
        if (commit_input->commitInfo[tid].pc &&
            commit_input->commitInfo[tid].squashedTargetId != 0) {
            DPRINTF(Fetch, "Squash with stream id and target id from IEW\n");
            const auto nc_pc =
                commit_input->commitInfo[tid].pc->as<RiscvISA::PCState>();
            assert(dbpbtb);
            dbpbtb->nonControlSquash(
                    commit_input->commitInfo[tid].squashedTargetId, nc_pc,
                    0, tid,
                    commit_input->commitInfo[tid].squashedLoopIter);
        } else {
            DPRINTF(Fetch, "Dont squash dbq because no meaningful stream\n");
        }
    }

    return true;
}

bool
Fetch::handleDecodeSquash(ThreadID tid, const TimeStruct *decode_input)
{
    // Check squash signals from decode.
    if (decode_input->decodeInfo[tid].squash) {
        DPRINTF(Fetch, "[tid:%i] Squashing instructions due to squash "
                "from decode.\n",tid);

        auto mispred_inst = decode_input->decodeInfo[tid].mispredictInst;
        if (decode_input->decodeInfo[tid].branchMispredict) {
            assert(dbpbtb);
            const auto next_pc =
                decode_input->decodeInfo[tid].nextPC->as<RiscvISA::PCState>();
            dbpbtb->controlSquash(
                mispred_inst->getFtqId(),
                mispred_inst->pcState(),
                next_pc,
                mispred_inst->staticInst, mispred_inst->getInstBytes(),
                decode_input->decodeInfo[tid].branchTaken,
                mispred_inst->seqNum, tid, mispred_inst->getLoopIteration(),
                false);
        } else {
            warn("Unexpected non-control squash from decode.\n");
        }

        if (fetchStatus[tid] != Squashing) {

            DPRINTF(Fetch, "Squashing from decode with PC = %s\n",
                *decode_input->decodeInfo[tid].nextPC);
            // Squash unless we're already squashing
            squashFromDecode(*decode_input->decodeInfo[tid].nextPC,
                             decode_input->decodeInfo[tid].squashInst,
                             decode_input->decodeInfo[tid].doneSeqNum,
                             tid);

            return true;
        }
    }

    return false;
}

DynInstPtr
Fetch::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    DynInst::Arrays arrays;
    arrays.numSrcs = staticInst->numSrcRegs();
    arrays.numDests = staticInst->numDestRegs();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, staticInst, curMacroop, this_pc, next_pc, seq, cpu);

    cpu->perfCCT->createMeta(instruction);
    cpu->perfCCT->updateInstPos(instruction->seqNum, PerfRecord::AtFetch);

    instruction->setTid(tid);

    instruction->setThreadState(cpu->thread[tid]);

    DPRINTF(Fetch, "[tid:%i] Instruction PC %s created [sn:%lli].\n",
            tid, this_pc, seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->disassemble(this_pc.instAddr()));

    DPRINTF(Fetch, "Is nop: %i, is move: %i\n", instruction->isNop(),
            instruction->isMov());
    assert(dbpbtb);
    DPRINTF(DecoupleBP, "Set instruction %lu with fetch id %lu\n",
            instruction->seqNum, dbpbtb->ftqHeadId(0));
    instruction->setFtqId(dbpbtb->ftqHeadId(0));

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, this_pc, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Write the instruction to the first slot in the queue
    // that heads to decode.
    assert(numInst < fetchWidth);
    fetchQueue[tid].push_back(instruction);
    assert(fetchQueue[tid].size() <= fetchQueueSize);
    DPRINTF(Fetch, "[tid:%i] Fetch queue entry created (%i/%i).\n",
            tid, fetchQueue[tid].size(), fetchQueueSize);
    //toDecode->insts[toDecode->size++] = instruction;

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    instruction->fallThruPC = this_pc.getFallThruPC();

    return instruction;
}

bool
Fetch::checkDecoupledFrontend(ThreadID tid)
{
    assert(dbpbtb);
    if (!isTraceMode() && !dbpbtb->ftqHasFetching(tid)) {
        dbpbtb->addFtqNotValid();
        DPRINTF(Fetch, "Skip fetch when FSQ head is not available\n");
        setAllFetchStalls(StallReason::FTQBubble);
        return false;
    }
    return true;
}

Fetch::FetchTargetPrepareInput
Fetch::buildFetchTargetPrepareInput(Cycles cycle) const
{
    FetchTargetPrepareInput input;
    input.cycle = cycle;
    input.numThreads = numThreads;

    for (int tid = 0; tid < MaxThreads; ++tid)
        input.targetTid[tid] = InvalidThreadID;

    if (!dbpbtb)
        return input;

    input.roundRobinStart = dbpbtb->ftqRoundRobinStart();
    for (int tid = 0; tid < numThreads; ++tid) {
        input.hasTarget[tid] = dbpbtb->ftqHasFetching(tid);
        if (input.hasTarget[tid]) {
            input.anyTarget = true;
            input.targetTid[tid] = dbpbtb->ftqFetchingTargetTid(tid);
        }
    }

    return input;
}

Fetch::FetchTargetPrepareResult
Fetch::prepareFetchTargetControl(
        const FetchTargetPrepareInput &input) const
{
    FetchTargetPrepareResult result;
    result.cycle = input.cycle;

    if (input.numThreads == 0)
        return result;

    for (int i = input.roundRobinStart;
         i < input.numThreads + input.roundRobinStart; ++i) {
        const ThreadID tid = i % input.numThreads;
        if (input.hasTarget[tid]) {
            result.selectedTid = input.targetTid[tid];
            result.foundTarget = true;
            return result;
        }
    }

    return result;
}

Fetch::FetchTargetPrepareResult
Fetch::runFetchTargetPrepare(Cycles cycle)
{
    auto input = std::make_shared<FetchTargetPrepareInput>(
            buildFetchTargetPrepareInput(cycle));
    auto result = std::make_shared<FetchTargetPrepareResult>();

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled())
        return prepareFetchTargetControl(*input);

    if (!input->anyTarget) {
        fetchStats.targetPrepareNoTarget++;
        return prepareFetchTargetControl(*input);
    }

    fetchStats.targetPrepareTasks++;
    const TaskOrderKey order{
        cycle, TaskStage::Fetch, 1, InvalidThreadID, 0};
    runtime.submitWeak(
            order,
            1,
            [this, input, result] {
                *result = prepareFetchTargetControl(*input);
            },
            [this, result] {
                fetchStats.targetPrepareMerges++;
            });
    runtime.waitForOrder(order);

    return *result;
}

Fetch::FetchPrepareInput
Fetch::buildFetchPrepareInput(Cycles cycle, ThreadID tid) const
{
    FetchPrepareInput input;
    input.cycle = cycle;
    input.tid = tid;
    input.traceMode = isTraceMode();
    input.ftqHasFetching = dbpbtb && dbpbtb->ftqHasFetching(tid);
    input.canFetch = canFetchInstructions(tid);
    input.macroopValid = static_cast<bool>(macroop[tid]);
    input.fetchBufferValid = threads[tid].valid;
    input.waitForVsetvl = waitForVsetvl;
    input.interruptPending = interruptPending;
    input.delayedCommit = delayedCommit[tid];
    input.fetchStatus = fetchStatus[tid];
    input.cacheStatus = threads[tid].cacheReq.getOverallStatus();
    input.fetchPC = threads[tid].fetchpc->instAddr();
    return input;
}

Fetch::FetchPrepareResult
Fetch::prepareFetchControl(const FetchPrepareInput &input) const
{
    FetchPrepareResult result;
    result.cycle = input.cycle;
    result.tid = input.tid;
    result.frontendReady = input.traceMode || input.ftqHasFetching;

    if (!result.frontendReady) {
        result.stallReason = StallReason::FTQBubble;
        return result;
    }

    result.cacheAccessComplete =
        input.cacheStatus == AccessComplete;
    if (result.cacheAccessComplete) {
        result.statusChange = true;
        result.readyToFetch = true;
        result.canFetch = true;
        return result;
    }

    result.canFetch = input.canFetch;
    if (input.canFetch) {
        if (!input.macroopValid && !input.fetchBufferValid) {
            result.readyToFetch = true;
        } else if (input.interruptPending && !input.delayedCommit) {
            result.interruptBlocked = true;
            return result;
        } else {
            result.readyToFetch = true;
        }
        return result;
    }

    result.idle = input.fetchStatus == Idle;
    return result;
}

Fetch::FutureQueueNotReadyState
Fetch::classifyFutureQueueNotReadyState(
        const FetchPrepareInput &input,
        const FetchPrepareResult &result,
        bool decode_blocked) const
{
    using State = FutureQueueNotReadyState;

    if (decode_blocked)
        return State::DecodeBlocked;

    if (!result.frontendReady)
        return State::FrontendNotReady;

    if (input.cacheStatus == AccessComplete)
        return State::CacheAccessComplete;

    if (input.cacheStatus == TlbWait ||
        input.cacheStatus == CacheWaitResponse ||
        input.cacheStatus == CacheWaitRetry) {
        return State::CachePending;
    }

    if (input.fetchStatus != Running)
        return State::FetchNotRunning;

    if (input.waitForVsetvl)
        return State::WaitForVsetvl;

    if (result.interruptBlocked)
        return State::InterruptBlocked;

    if (!result.readyToFetch)
        return State::FetchControlNotReady;

    if (input.macroopValid || input.fetchBufferValid)
        return State::ReadyBuffered;

    if (result.canFetch)
        return State::ReadyNeedsCacheLine;

    return State::ReadyOther;
}

void
Fetch::mergeFetchPrepareResult(const FetchPrepareResult &result,
                               bool countPrepareStats)
{
    lastPrepareResult = result;

    if (countPrepareStats) {
        fetchStats.prepareMerges++;
        if (result.frontendReady)
            fetchStats.prepareFrontendReady++;
        if (result.readyToFetch)
            fetchStats.prepareReadyToFetch++;
        if (result.interruptBlocked)
            fetchStats.prepareInterruptBlocked++;
    }
}

Fetch::FetchPrepareResult
Fetch::runFetchPrepare(Cycles cycle, ThreadID tid)
{
    auto input = std::make_shared<FetchPrepareInput>(
            buildFetchPrepareInput(cycle, tid));
    auto result = std::make_shared<FetchPrepareResult>();

    auto &runtime = cpu->getTaskRuntime();
    if (!runtime.enabled()) {
        *result = prepareFetchControl(*input);
        mergeFetchPrepareResult(*result, false);
        return *result;
    }

    fetchStats.prepareTasks++;
    const TaskOrderKey order{cycle, TaskStage::Fetch, 2, tid, 0};
    runtime.submitWeak(
            order,
            1,
            [this, input, result] {
                *result = prepareFetchControl(*input);
            },
            [this, result] {
                mergeFetchPrepareResult(*result, true);
            });
    runtime.waitForOrder(order);

    return lastPrepareResult;
}

bool
Fetch::prepareFetchAddress(ThreadID tid, bool &status_change)
{
    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC - directly use the actual instruction address
    PCStateBase &this_pc = *threads[tid].fetchpc;

    // Handle status transitions and cache access
    if (threads[tid].cacheReq.getOverallStatus() == AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);
        setThreadStatus(tid, Running);
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
        return true;
    } else if (canFetchInstructions(tid)) {
        // If the decoder needs bytes, performInstructionFetch() will issue an
        // I-cache request via sendNextCacheRequest().
        if (!macroop[tid] && !threads[tid].valid) {
            return true;
        } else if (checkInterrupt(this_pc.instAddr()) && !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return false;
        }
        return true;
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }
        // Status is Idle, so fetch should do nothing.
        return false;
    }
}

void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    const FetchTargetPrepareResult target_prepare =
        runFetchTargetPrepare(cpu->curCycle());
    auto tid = target_prepare.selectedTid;

    if (tid == InvalidThreadID) {
        return;
    }

    const FetchPrepareResult prepare =
        runFetchPrepare(cpu->curCycle(), tid);
    const auto selected_tid = dbpbtb->getTargetTid();
    if (selected_tid != tid) {
        fetchStats.targetPrepareMismatches++;
        panic("Fetch target prepare mismatch: prepared tid %i, "
              "selected tid %i", tid, selected_tid);
    }

    if (!prepare.frontendReady) {
        dbpbtb->addFtqNotValid();
        DPRINTF(Fetch, "Skip fetch when FSQ head is not available\n");
        setAllFetchStalls(prepare.stallReason);
        return;
    }

    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    if (prepare.cacheAccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);
        setThreadStatus(tid, Running);
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
    } else if (prepare.canFetch) {
        if (prepare.interruptBlocked) {
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return;
        }
    } else {
        if (prepare.idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }
        return;
    }

    ++fetchStats.cycles;

    performInstructionFetch(tid);
}

StallReason
Fetch::checkMemoryNeeds(ThreadID tid, const PCStateBase &this_pc,
                        const StaticInstPtr &curMacroop)
{
    // If we are in the middle of a macro-op, the decoder does not need
    // more memory bytes. It will continue processing the existing instruction.
    if (curMacroop) {
        return StallReason::NoStall;
    }

    // Trace 按需消费：在 decode 前逐条供给解码器。
    if (isTraceMode()) {
        assert(traceFetch);
        return traceFetch->checkMemoryNeeds(tid, this_pc);
    }

    Addr fetch_pc = this_pc.instAddr();

    // Check if fetch buffer is valid and contains this PC
    if (!threads[tid].valid) {
        DPRINTF(Fetch, "[tid:%i] Fetch buffer invalid, stalling on ICache\n", tid);
        return StallReason::IcacheStall;
    }

    // Check if the fetch buffer contains enough bytes for this instruction
    // We need at least 4 bytes to decode any RISC-V instruction (including compressed)
    if (fetch_pc < threads[tid].startPC ||
        fetch_pc + 4 > threads[tid].startPC + fetchBufferSize) {
        DPRINTF(Fetch, "[tid:%i] PC %#x outside fetch buffer range [%#x, %#x), stalling on ICache\n",
                tid, fetch_pc, threads[tid].startPC, threads[tid].startPC + fetchBufferSize);
        return StallReason::IcacheStall;
    }

    // Supply bytes to decoder - always provide 4 bytes for RISC-V
    auto *dec_ptr = decoder[tid];
    Addr offset_in_buffer = fetch_pc - threads[tid].startPC;
    memcpy(dec_ptr->moreBytesPtr(), threads[tid].data + offset_in_buffer, 4);

    DPRINTF(Fetch, "[tid:%i] Supplying 4 bytes from fetchBuffer at PC %#x (offset %d)\n",
            tid, fetch_pc, offset_in_buffer);

    // Call decoder with the actual instruction PC
    decoder[tid]->moreBytes(this_pc, fetch_pc);

    return StallReason::NoStall;
}

bool
Fetch::processSingleInstruction(ThreadID tid, PCStateBase &pc,
                               StaticInstPtr &curMacroop)
{
    auto *dec_ptr = decoder[tid];
    bool predictedBranch = false;
    bool newMacroop = false;

    // Create a copy of the current PC state to calculate the next PC.
    std::unique_ptr<PCStateBase> next_pc(pc.clone());

    // Decode the instruction, handling macro-op transitions.
    StaticInstPtr staticInst = nullptr;
    if (!curMacroop) {
        // Decode a new instruction if not currently in a macro-op.
        staticInst = dec_ptr->decode(pc);
        ++fetchStats.insts;

        if (staticInst->isMacroop()) {
            curMacroop = staticInst;
            DPRINTF(Fetch, "[tid:%i] Macroop instruction decoded\n", tid);
        }
    }
    if (curMacroop) {
        // Fetch the next micro-op from the current macro-op.
        staticInst = curMacroop->fetchMicroop(pc.microPC());
        DPRINTF(Fetch, "[tid:%i] Fetched macroop microop\n", tid);
        // Check if this is the last micro-op.
        newMacroop = staticInst->isLastMicroop();
    }

    // Build the dynamic instruction and add it to the fetch queue
    DynInstPtr instruction =
        buildInst(tid, staticInst, curMacroop, pc, *next_pc, true);

    o3::TraceInstruction traceForThisInst;
    if (isTraceMode()) {
        assert(traceFetch);
        traceFetch->bindPendingTraceMetadata(tid, instruction, pc, traceForThisInst);
    }

    // Special handling for RISC-V vector configuration instructions.
    if (staticInst->isVectorConfig()) {
        waitForVsetvl = dec_ptr->stall();
        DPRINTF(Fetch, "[tid:%i] Vector config instruction, waitForVsetvl=%d\n",
                tid, waitForVsetvl);
    }

    instruction->setVersion(localSquashVer);
    ppFetch->notify(instruction);
    numInst++;

#if TRACING_ON
    if (debug::O3PipeView) {
        instruction->fetchTick = curTick();
    }
#endif

    // Save current PC to next_pc first
    set(next_pc, pc);

    // Handle branch prediction and update next_pc for both modes
    predictedBranch = lookupAndUpdateNextPC(instruction, *next_pc);

    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Branch detected with PC = %s, target = %s\n",
                instruction->threadNumber, pc, *next_pc);
    }

    if (isTraceMode()) {
        assert(traceFetch);
        traceFetch->postBranchPredict(tid, instruction, traceForThisInst, pc, *next_pc, predictedBranch);
    }

    // A new macro-op also begins if the PC changes discontinuously.
    newMacroop |= pc.instAddr() != next_pc->instAddr();
    if (newMacroop) {
        curMacroop = NULL;
        DPRINTF(Fetch, "[tid:%i] New macroop transition, PC=%s\n",
                tid, pc);
    }

    // Update the main PC state for the next instruction.
    set(pc, *next_pc);

    // Do the value prediction
    if (valuePred && instruction->canLVP()) {
        valuepred::VPPredMetaData* vpPredMetaData = valuepred::VPDataStructFactory::
                                                        buildPredMetaData(valuePred->getValuePredictorType());

        vpPredMetaData->pc = instruction->getPC();
        vpPredMetaData->seq_no = instruction->seqNum;
        instruction->vpResult = valuePred->valuePredict(vpPredMetaData);
        delete vpPredMetaData;
    }

    return predictedBranch;
}

void
Fetch::performInstructionFetch(ThreadID tid)
{
    if (isTraceMode()) {
        assert(traceFetch);
        if (traceFetch->maybeStallFetch(tid)) {
            return;
        }
    }

    // Initialize local variables
    PCStateBase &pc_state = *threads[tid].fetchpc;
    StaticInstPtr &curMacroop = macroop[tid];

    // Control flags for main fetch loop
    bool predictedBranch = false;

    DPRINTF(Fetch, "[tid:%i] Adding instructions to queue to decode.\n", tid);

    // Main instruction fetch loop - process until fetch width or other limits
    // For decoupled frontend (including trace mode), check FTQ availability
    StallReason stall = StallReason::NoStall;
    while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize &&
           !predictedBranch && !ftqEmpty(tid) && !waitForVsetvl) {

        // Check memory needs and supply bytes to decoder if required
        stall = checkMemoryNeeds(tid, pc_state, curMacroop);
        if (stall != StallReason::NoStall) {
            break;
        }

        // Inner loop: extract as many instructions as possible from buffered
        // memory. This is primarily for macro-op instructions, which decode
        // into multiple micro-ops.
        do {
            // Process a single instruction, from decoding to PC update.
            predictedBranch = processSingleInstruction(tid, pc_state, curMacroop);

        } while (curMacroop &&
                 numInst < fetchWidth &&
                 fetchQueue[tid].size() < fetchQueueSize);
    }

    // Debug output for fetch queue contents
    DPRINTF(FetchVerbose, "FetchQue start dumping\n");
    for (auto it : fetchQueue[tid]) {
        DPRINTF(FetchVerbose, "inst: %s\n", it->staticInst->disassemble(it->pcState().instAddr()));
    }

    // Handle stall conditions and update statistics
    if (stall != StallReason::NoStall) {
        setAllFetchStalls(stall);
    }

    // Log why fetch stopped
    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, predicted branch instruction encountered.\n", tid);
    } else if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth for this cycle.\n", tid);
    } else if (stall != StallReason::NoStall) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, stalled due to %s.\n", tid,
                stall == StallReason::IcacheStall ? "ICache" : "other reasons");
    } else {
        DPRINTF(Fetch, "[tid:%i] Done fetching, no more instructions to fetch.\n", tid);
    }

    // Update persistent state
    macroop[tid] = curMacroop;

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    assert(fetchStatus[tid] == Running && "Fetch should be running");
    sendNextCacheRequest(tid, pc_state);
}

void
Fetch::sendNextCacheRequest(ThreadID tid, const PCStateBase &pc_state) {
    if (threads[tid].valid) {
        return;
    }

    if (ftqEmpty(tid)) {
        DPRINTF(Fetch, "[tid:%i] No FSQ entry available for next fetch\n", tid);
        return;
    }

    assert(dbpbtb);
    const auto &stream = dbpbtb->ftqFetchingTarget(tid);
    const Addr start_pc = stream.startPC;
    threads[tid].startPC = start_pc;

    DPRINTF(Fetch, "[tid:%i] Issuing a pipelined I-cache access for new FSQ entry, "
                  "starting at PC %#x (endPC %#x; original PC %s)\n",
            tid, start_pc, stream.predEndPC, pc_state);
    fetchCacheLine(start_pc, tid, pc_state.instAddr());
}

void
Fetch::recvReqRetry()
{
    if (retryPkt.size() == 0) {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
        return;
    }
    assert(cacheBlocked);
    assert(retryTid != InvalidThreadID);
    // Note: In multi-cacheline fetch, overall status may not be CacheWaitRetry
    // if some requests have progressed while others still need retry.
    // The presence of retryPkt itself indicates retry is needed.

    for (auto it = retryPkt.begin(); it != retryPkt.end();) {
        if (icachePort.sendTimingReq(*it)) {
            // Use new cache state management with specific RequestPtr
            updateCacheRequestStatusByRequest(retryTid, (*it)->req, CacheWaitResponse);
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify((*it)->req);
            it = retryPkt.erase(it);
        } else {
            it++;
        }
    }

    if (retryPkt.size() == 0) {
        retryTid = InvalidThreadID;
        cacheBlocked = false;
    }
}

void
Fetch::profileStall(ThreadID tid)
{
    DPRINTF(Fetch,"There are no more threads available to fetch from.\n");

    // @todo Per-thread stats

    if (activeThreads->empty()) {
        ++fetchStats.noActiveThreadStallCycles;
        DPRINTF(Fetch, "Fetch has no active thread!\n");
    } else if (fetchStatus[tid] == Blocked) {
        ++fetchStats.blockedCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is blocked!\n", tid);
    } else if (fetchStatus[tid] == Squashing) {
        ++fetchStats.squashCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is squashing!\n", tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == CacheWaitResponse) {
        ++fetchStats.icacheStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting cache response!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == TlbWait) {
        ++fetchStats.tlbCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting ITLB walk to "
                "finish!\n", tid);
    } else if (fetchStatus[tid] == TrapPending) {
        ++fetchStats.pendingTrapStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for a pending trap!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == CacheWaitRetry) {
        ++fetchStats.icacheWaitRetryStallCycles;
        DPRINTF(Fetch, "[tid:%i] Fetch is waiting for an I-cache retry!\n",
                tid);
    } else if (threads[tid].cacheReq.getOverallStatus() == AccessFailed) {
            DPRINTF(Fetch, "[tid:%i] Fetch predicted non-executable address\n",
                    tid);
    } else {
        DPRINTF(Fetch, "[tid:%i] Unexpected fetch stall reason "
            "(Status: %i)\n",
            tid, fetchStatus[tid]);
    }
}

void
Fetch::setAllFetchStalls(StallReason stall)
{
    for (int i = 0; i < stallReason.size(); i++) {
        stallReason[i] = stall;
    }
}


bool
Fetch::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));

    DPRINTF(Fetch, "received pkt addr=%#lx, req addr=%#lx\n", pkt->getAddr(),
            pkt->req->getVaddr());

    fetch->processCacheCompletion(pkt);

    return true;
}

void
Fetch::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

bool
Fetch::canFetchInstructions(ThreadID tid) const
{
    // Thread must be in Running state
    if (fetchStatus[tid] != Running) {
        return false;  // Covers Idle, Squashing, Blocked, TrapPending
    }

    // Cache must be ready for new requests or have completed data
    CacheRequestStatus cacheStatus = threads[tid].cacheReq.getOverallStatus();
    return (cacheStatus == CacheIdle || cacheStatus == AccessComplete);
}

bool
Fetch::hasPendingCacheRequests(ThreadID tid) const
{
    // Check for any active cache operations (excluding terminal states)
    CacheRequestStatus overallStatus = threads[tid].cacheReq.getOverallStatus();
    return (overallStatus == TlbWait ||
            overallStatus == CacheWaitResponse ||
            overallStatus == CacheWaitRetry);
}

void
Fetch::setThreadStatus(ThreadID tid, ThreadStatus status)
{
    assert(tid < MaxThreads);

    ThreadStatus oldStatus = fetchStatus[tid];
    fetchStatus[tid] = status;
    DPRINTF(Fetch, "[tid:%d] setThreadStatus: %s -> %s\n", tid, fetchStatusStr[oldStatus], fetchStatusStr[status]);
}

void
Fetch::updateCacheRequestStatus(ThreadID tid, size_t reqIndex,
                               CacheRequestStatus status)
{
    assert(tid < MaxThreads);
    assert(reqIndex < threads[tid].cacheReq.requestStatus.size());

    DPRINTF(Fetch, "[tid:%d] updateCacheRequestStatus[%d]: %d -> %d\n",
            tid, reqIndex, threads[tid].cacheReq.requestStatus[reqIndex], status);

    threads[tid].cacheReq.requestStatus[reqIndex] = status;
}

void
Fetch::updateCacheRequestStatusByRequest(ThreadID tid, const RequestPtr& req,
                                        CacheRequestStatus status)
{
    assert(tid < MaxThreads);

    size_t reqIndex = threads[tid].cacheReq.findRequestIndex(req);
    if (reqIndex != SIZE_MAX) {
        updateCacheRequestStatus(tid, reqIndex, status);
    } else {
        warn("Cannot find req %#x for status update to %d\n", req->getVaddr(), status);
    }
}

void
Fetch::cancelAllCacheRequests(ThreadID tid)
{
    assert(tid < MaxThreads);

    DPRINTF(Fetch, "[tid:%d] cancelAllCacheRequests: status before cancel: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

    // Cancel all cache requests
    threads[tid].cacheReq.cancelAllRequests();

    DPRINTF(Fetch, "[tid:%d] cancelAllCacheRequests: status after cancel: %s\n",
            tid, threads[tid].cacheReq.getStatusSummary().c_str());

}


const o3::TraceInstruction*
Fetch::getTraceInstMetadata(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->getTraceInstMetadata(seqNum) : nullptr;
}

bool
Fetch::isTraceInstruction(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->isTraceInstruction(seqNum) : false;
}

void
Fetch::cleanupTraceMetadataOnCommit(InstSeqNum seqNum)
{
    if (traceFetch) {
        traceFetch->cleanupTraceMetadataOnCommit(seqNum);
    }
}

uint64_t
Fetch::findTraceIndexForSeqNum(InstSeqNum seqNum) const
{
    return traceFetch ? traceFetch->findTraceIndexForSeqNum(seqNum) : 0;
}

bool
Fetch::lookupTraceIndexForSeqNum(InstSeqNum seqNum, uint64_t &index) const
{
    if (!traceFetch) {
        index = 0;
        return false;
    }
    return traceFetch->lookupTraceIndexForSeqNum(seqNum, index);
}

Addr
Fetch::getTracePCByIndex(uint64_t index)
{
    return traceFetch ? traceFetch->getTracePCByIndex(index) : 0;
}

} // namespace o3
} // namespace gem5
