/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

#include "mem/cache/prefetch/bop.hh"

#include <algorithm>

#include "base/stats/group.hh"
#include "debug/BOPOffsets.hh"
#include "debug/BOPPrefetcher.hh"
#include "mem/cache/base.hh"
#include "params/BOPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

unsigned int
BOP::pcValidationKindIndex(PCValidationKind kind)
{
    switch (kind) {
      case PCValidationKind::Generic:
        return 0;
      case PCValidationKind::Large:
        return 1;
      case PCValidationKind::Small:
        return 2;
    }
    panic("Invalid BOP PC validation kind\n");
    return 0;
}

const char *
BOP::pcValidationKindName(PCValidationKind kind)
{
    switch (kind) {
      case PCValidationKind::Generic:
        return "generic";
      case PCValidationKind::Large:
        return "large";
      case PCValidationKind::Small:
        return "small";
    }
    panic("Invalid BOP PC validation kind\n");
    return "invalid";
}

BOP::PCValidationConfidenceTable::PCValidationConfidenceTable(
    unsigned int entries, unsigned int tag_bits, unsigned int counter_bits,
    unsigned int initial_confidence, unsigned int medium_threshold,
    unsigned int high_threshold, unsigned int hit_increment,
    unsigned int medium_sample_period, unsigned int miss_decay_period,
    unsigned int low_entry_miss_streak_threshold, unsigned int epoch_bits,
    bool enable_global_coverage_guard,
    unsigned int global_unused_threshold,
    unsigned int global_min_resolved_coverage_shift)
    : entries(entries),
      sets(entries >= PC_VALIDATION_ASSOCIATIVITY
               ? entries / PC_VALIDATION_ASSOCIATIVITY : 0),
      setBits(entries >= PC_VALIDATION_ASSOCIATIVITY
                  ? floorLog2(entries / PC_VALIDATION_ASSOCIATIVITY) : 0),
      tagBits(tag_bits),
      tagMask(tag_bits > 0 && tag_bits < sizeof(Addr) * 8
                  ? (static_cast<Addr>(1) << tag_bits) - 1 : 0),
      counterMax(counter_bits > 0 && counter_bits <= 8
                     ? (1U << counter_bits) - 1 : 0),
      initialConfidence(initial_confidence),
      mediumThreshold(medium_threshold),
      highThreshold(high_threshold),
      hitIncrement(hit_increment),
      mediumSamplePeriod(medium_sample_period),
      missDecayPeriod(miss_decay_period),
      lowEntryMissStreakThreshold(low_entry_miss_streak_threshold),
      epochMask(epoch_bits > 0 && epoch_bits <= 8
                    ? (1U << epoch_bits) - 1 : 0),
      globalCoverageGuardEnabled(enable_global_coverage_guard),
      globalUnusedThreshold(global_unused_threshold),
      globalMinResolvedCoverageShift(global_min_resolved_coverage_shift),
      table(entries),
      plruState(sets)
{
    if (!isPowerOf2(entries)) {
        fatal("BOP PC validation entries must be a power of two\n");
    }
    if (entries < PC_VALIDATION_ASSOCIATIVITY) {
        fatal("BOP PC validation entries must be at least %u for %u-way PLRU\n",
              PC_VALIDATION_ASSOCIATIVITY, PC_VALIDATION_ASSOCIATIVITY);
    }
    if (tagBits == 0 || tagBits >= sizeof(Addr) * 8) {
        fatal("BOP PC validation tag bits must be in [1, %zu)\n",
              sizeof(Addr) * 8);
    }
    if (counter_bits == 0 || counter_bits > 8) {
        fatal("BOP PC validation counter bits must be in [1, 8]\n");
    }
    if (epoch_bits == 0 || epoch_bits > 8) {
        fatal("BOP PC validation epoch bits must be in [1, 8]\n");
    }
    if (initialConfidence > counterMax ||
        mediumThreshold > highThreshold || highThreshold > counterMax) {
        fatal("Invalid BOP PC validation confidence thresholds\n");
    }
    if (!isPowerOf2(mediumSamplePeriod) ||
        !isPowerOf2(missDecayPeriod)) {
        fatal("BOP PC validation sample periods must be powers of two\n");
    }
    if (lowEntryMissStreakThreshold > 3) {
        fatal("BOP PC validation low-entry miss-streak threshold must be "
              "in [0, 3]\n");
    }
    if (globalCoverageGuardEnabled && globalUnusedThreshold > 255) {
        fatal("BOP global coverage unused threshold must be in [0, 255]\n");
    }
    if (globalCoverageGuardEnabled &&
        globalMinResolvedCoverageShift >= sizeof(unsigned int) * 8) {
        fatal("BOP global resolved coverage shift is too large\n");
    }
}

Addr
BOP::PCValidationConfidenceTable::foldedPC(Addr pc) const
{
    // RISC-V instructions are at least 2-byte aligned. Fold non-adjacent PC
    // bits before splitting the compact signature into index and partial tag.
    Addr signature = pc >> 1;
    signature ^= signature >> 7;
    signature ^= signature >> 13;
    signature ^= signature >> 27;
    return signature;
}

Addr
BOP::PCValidationConfidenceTable::signature(
    Addr pc, PCValidationKind kind) const
{
    Addr sig = foldedPC(pc);
    sig ^= static_cast<Addr>(pcValidationKindIndex(kind)) *
           0x9e3779b97f4a7c15ULL;
    sig ^= sig >> 11;
    sig ^= sig >> 23;
    return sig;
}

BOP::PCValidationConfidenceTable::Entry &
BOP::PCValidationConfidenceTable::entryAt(
    unsigned int set, unsigned int way)
{
    assert(set < sets);
    assert(way < PC_VALIDATION_ASSOCIATIVITY);
    return table[set * PC_VALIDATION_ASSOCIATIVITY + way];
}

const BOP::PCValidationConfidenceTable::Entry &
BOP::PCValidationConfidenceTable::entryAt(
    unsigned int set, unsigned int way) const
{
    assert(set < sets);
    assert(way < PC_VALIDATION_ASSOCIATIVITY);
    return table[set * PC_VALIDATION_ASSOCIATIVITY + way];
}

unsigned int
BOP::PCValidationConfidenceTable::plruVictim(unsigned int set) const
{
    assert(set < sets);
    const uint8_t state = plruState[set] & 0x7;
    if ((state & 0x1) == 0) {
        return (state & 0x2) == 0 ? 0 : 1;
    }
    return (state & 0x4) == 0 ? 2 : 3;
}

void
BOP::PCValidationConfidenceTable::touchPLRU(
    unsigned int set, unsigned int way)
{
    assert(set < sets);
    assert(way < PC_VALIDATION_ASSOCIATIVITY);

    uint8_t &state = plruState[set];
    switch (way) {
      case 0:
        state |= 0x1;
        state |= 0x2;
        break;
      case 1:
        state |= 0x1;
        state &= ~0x2;
        break;
      case 2:
        state &= ~0x1;
        state |= 0x4;
        break;
      case 3:
        state &= ~0x1;
        state &= ~0x4;
        break;
      default:
        panic("Invalid BOP PC validation PLRU way\n");
    }
}

bool
BOP::PCValidationConfidenceTable::sample(
    Addr pc, PCValidationKind kind, Addr line, unsigned int period,
    Addr salt) const
{
    assert(isPowerOf2(period));

    const unsigned int kind_index = pcValidationKindIndex(kind);
    Addr sig = signature(pc, kind) ^ line ^ salt ^ currentEpoch[kind_index];
    sig ^= sig >> 9;
    sig ^= sig >> 17;
    sig ^= sig >> 29;
    return (sig & (period - 1)) == 0;
}

BOP::PCValidationConfidenceTable::LookupResult
BOP::PCValidationConfidenceTable::lookup(Addr pc, PCValidationKind kind)
{
    const Addr sig = signature(pc, kind);
    const unsigned int set = sig & (sets - 1);
    const Addr tag = (sig >> setBits) & tagMask;
    const unsigned int kind_index = pcValidationKindIndex(kind);

    LookupResult result;
    result.set = set;
    result.tag = tag;
    result.kind = kind;

    unsigned int way = PC_VALIDATION_ASSOCIATIVITY;
    for (unsigned int candidate = 0;
         candidate < PC_VALIDATION_ASSOCIATIVITY; candidate++) {
        Entry &entry = entryAt(set, candidate);
        if (entry.valid && entry.tag == tag) {
            way = candidate;
            result.entryHit = true;
            break;
        }
    }

    if (!result.entryHit) {
        for (unsigned int candidate = 0;
             candidate < PC_VALIDATION_ASSOCIATIVITY; candidate++) {
            if (!entryAt(set, candidate).valid) {
                way = candidate;
                break;
            }
        }
        if (way == PC_VALIDATION_ASSOCIATIVITY) {
            way = plruVictim(set);
        }
    }

    result.way = way;
    result.index = set * PC_VALIDATION_ASSOCIATIVITY + way;
    Entry &entry = entryAt(set, way);
    result.replaced = !result.entryHit && entry.valid;

    if (!result.entryHit) {
        entry.valid = true;
        entry.tag = tag;
        entry.confidence = initialConfidence;
        entry.lowEntryMissStreak = 0;
        entry.epoch = currentEpoch[kind_index];
    } else if (entry.epoch != currentEpoch[kind_index]) {
        entry.confidence = initialConfidence;
        entry.lowEntryMissStreak = 0;
        entry.epoch = currentEpoch[kind_index];
        result.epochReset = true;
    }
    touchPLRU(set, way);

    result.confidence = entry.confidence;
    result.lowEntryMissStreak = entry.lowEntryMissStreak;
    result.epoch = entry.epoch;
    if (entry.confidence >= highThreshold) {
        result.state = PCConfidenceState::High;
    } else if (entry.confidence >= mediumThreshold) {
        result.state = PCConfidenceState::Medium;
    } else {
        result.state = PCConfidenceState::Low;
    }
    return result;
}

bool
BOP::PCValidationConfidenceTable::sampleMediumIssue(
    Addr pc, PCValidationKind kind, Addr line) const
{
    return sample(pc, kind, line, mediumSamplePeriod, 0x9e37);
}

void
BOP::PCValidationConfidenceTable::resetGlobalBypassPolicy()
{
    globalOutcomeWindowResolved = 0;
    globalOutcomeWindowUnused = 0;
    globalIssuedWindowIssued = 0;
    globalUnusedEwma = GLOBAL_UNUSED_EWMA_INITIAL;
    globalChecksSinceOutcome = 0;
    globalBypassPCValidation = true;
}

bool
BOP::PCValidationConfidenceTable::notePCValidationMiss()
{
    if (!globalCoverageGuardEnabled || !globalBypassPCValidation) {
        return false;
    }

    globalChecksSinceOutcome++;
    if (globalChecksSinceOutcome < GLOBAL_IDLE_RESET_CHECKS) {
        return false;
    }

    resetGlobalBypassPolicy();
    return true;
}

bool
BOP::PCValidationConfidenceTable::bypassPCValidationActive() const
{
    return globalCoverageGuardEnabled && globalBypassPCValidation;
}

BOP::PCValidationConfidenceTable::GlobalTraceState
BOP::PCValidationConfidenceTable::traceState() const
{
    GlobalTraceState state;
    state.enabled = globalCoverageGuardEnabled;
    state.bypassActive = bypassPCValidationActive();
    state.unusedEwma = globalUnusedEwma;
    state.outcomeWindowResolved = globalOutcomeWindowResolved;
    state.outcomeWindowUnused = globalOutcomeWindowUnused;
    state.issuedWindowIssued = globalIssuedWindowIssued;
    state.checksSinceOutcome = globalChecksSinceOutcome;
    return state;
}

void
BOP::PCValidationConfidenceTable::noteGlobalBOPIssued()
{
    if (!globalCoverageGuardEnabled) {
        return;
    }

    globalIssuedWindowIssued++;
}

BOP::PCValidationConfidenceTable::GlobalOutcomeResult
BOP::PCValidationConfidenceTable::noteGlobalBOPOutcome(bool useful)
{
    GlobalOutcomeResult result;
    result.enabled = globalCoverageGuardEnabled;
    if (!globalCoverageGuardEnabled) {
        return result;
    }

    globalChecksSinceOutcome = 0;
    globalOutcomeWindowResolved++;
    if (!useful) {
        globalOutcomeWindowUnused++;
    }
    if (globalOutcomeWindowResolved != GLOBAL_OUTCOME_WINDOW_SIZE) {
        return result;
    }

    const unsigned int issued_window = globalIssuedWindowIssued;
    const bool coverage_gate_enabled =
        globalMinResolvedCoverageShift != 0;
    const uint64_t resolved_scaled =
        static_cast<uint64_t>(GLOBAL_OUTCOME_WINDOW_SIZE) <<
        globalMinResolvedCoverageShift;
    const bool resolved_coverage_good =
        !coverage_gate_enabled || issued_window == 0 ||
        resolved_scaled >= issued_window;
    const unsigned int resolved_coverage_q08 = issued_window == 0 ? 255 :
        static_cast<unsigned int>(std::min<uint64_t>(
            255,
            (static_cast<uint64_t>(GLOBAL_OUTCOME_WINDOW_SIZE) * 255) /
            issued_window));

    const unsigned int bucket_unused_q08 =
        (globalOutcomeWindowUnused * 255) >> GLOBAL_OUTCOME_WINDOW_SHIFT;
    const int delta = static_cast<int>(bucket_unused_q08) -
                      static_cast<int>(globalUnusedEwma);
    int step = delta / static_cast<int>(1U << GLOBAL_EWMA_SHIFT);
    if (step == 0 && delta != 0) {
        step = delta > 0 ? 1 : -1;
    }
    globalUnusedEwma = std::clamp(
        static_cast<int>(globalUnusedEwma) + step,
        0, 255);
    globalOutcomeWindowResolved = 0;
    globalOutcomeWindowUnused = 0;
    globalIssuedWindowIssued = 0;

    const bool was_bypassing = globalBypassPCValidation;
    const bool unused_quality_good = globalUnusedEwma <= globalUnusedThreshold;
    globalBypassPCValidation =
        unused_quality_good && resolved_coverage_good;

    result.ewmaUpdated = true;
    result.bypassModeEntered = !was_bypassing && globalBypassPCValidation;
    result.bypassModeExited = was_bypassing && !globalBypassPCValidation;
    result.resolvedCoverageGood = resolved_coverage_good;
    result.bypassBlockedByLowCoverage =
        unused_quality_good && !resolved_coverage_good;
    result.unusedEwma = globalUnusedEwma;
    result.issuedWindowIssued = issued_window;
    result.resolvedCoverageQ08 = resolved_coverage_q08;
    return result;
}

void
BOP::PCValidationConfidenceTable::submitValidation(
    const LookupResult &lookup, Addr pc, Addr trigger_line,
    bool validation_hit)
{
    PendingUpdate &update = pending[pcValidationKindIndex(lookup.kind)];
    if (!update.valid) {
        update.valid = true;
        update.kind = lookup.kind;
        update.pc = pc;
        update.triggerLine = trigger_line;
        update.index = lookup.index;
        update.set = lookup.set;
        update.way = lookup.way;
        update.tag = lookup.tag;
    } else if (update.index != lookup.index || update.tag != lookup.tag ||
               update.pc != pc || update.kind != lookup.kind) {
        panic("BOP PC validation shared table was not committed per demand\n");
    }

    update.validationHit = update.validationHit || validation_hit;
    update.participants++;
}

void
BOP::PCValidationConfidenceTable::noteOffsetChange(PCValidationKind kind)
{
    PendingUpdate &update = pending[pcValidationKindIndex(kind)];
    update.kind = kind;
    update.offsetChanged = true;
}

BOP::PCValidationConfidenceTable::CommitResult
BOP::PCValidationConfidenceTable::commitOne(PendingUpdate &update)
{
    CommitResult result;
    result.kind = update.kind;
    if (!update.valid && !update.offsetChanged) {
        return result;
    }

    const unsigned int kind_index = pcValidationKindIndex(update.kind);
    result.hadPending = true;
    result.hadValidation = update.participants != 0;
    result.offsetChanged = update.offsetChanged;
    result.validationHit = update.validationHit;
    result.pc = update.pc;
    result.triggerLine = update.triggerLine;
    result.index = update.index;
    result.set = update.set;
    result.way = update.way;
    result.tag = update.tag;
    result.participants = update.participants;
    if (result.hadValidation) {
        Entry &entry = table[update.index];
        assert(entry.valid && entry.tag == update.tag);
        result.confidenceBefore = entry.confidence;
        result.lowEntryMissStreakBefore = entry.lowEntryMissStreak;

        // A best-offset change starts a new validation regime. Preserve the
        // current-demand decision, then lazily reset entries before the next
        // demand instead of applying evidence tied to the old regime.
        if (!update.offsetChanged) {
            if (update.validationHit) {
                entry.confidence = std::min(
                    counterMax, static_cast<unsigned int>(entry.confidence) +
                                    hitIncrement);
                entry.lowEntryMissStreak = 0;
            } else if (sample(update.pc, update.kind, update.triggerLine,
                              missDecayPeriod, 0x7f4a)) {
                if (lowEntryMissStreakThreshold != 0 &&
                    entry.confidence == mediumThreshold) {
                    entry.lowEntryMissStreak = std::min(
                        lowEntryMissStreakThreshold,
                        static_cast<unsigned int>(entry.lowEntryMissStreak) +
                            1);
                    if (entry.lowEntryMissStreak ==
                        lowEntryMissStreakThreshold) {
                        entry.confidence = entry.confidence == 0
                            ? 0 : entry.confidence - 1;
                        entry.lowEntryMissStreak = 0;
                        result.decayed = true;
                        result.lowEntryHysteresisTransition = true;
                    } else {
                        result.lowEntryHysteresisHeld = true;
                    }
                } else {
                    entry.confidence = entry.confidence == 0
                        ? 0 : entry.confidence - 1;
                    entry.lowEntryMissStreak = 0;
                    result.decayed = true;
                }
            }
            result.confidenceAfter = entry.confidence;
            result.lowEntryMissStreakAfter = entry.lowEntryMissStreak;
        } else {
            result.confidenceAfter = entry.confidence;
            result.lowEntryMissStreakAfter = entry.lowEntryMissStreak;
        }
    }

    if (update.offsetChanged) {
        currentEpoch[kind_index] =
            (currentEpoch[kind_index] + 1) & epochMask;
    }
    result.epochAfter = currentEpoch[kind_index];
    update = PendingUpdate();
    return result;
}

std::vector<BOP::PCValidationConfidenceTable::CommitResult>
BOP::PCValidationConfidenceTable::commit()
{
    std::vector<CommitResult> results;
    results.reserve(PC_VALIDATION_KIND_COUNT);
    for (PendingUpdate &update : pending) {
        CommitResult result = commitOne(update);
        if (result.hadPending) {
            results.push_back(result);
        }
    }
    return results;
}

bool
BOP::PCValidationConfidenceTable::configMatches(
    const PCValidationConfidenceTable &other) const
{
    return entries == other.entries && tagBits == other.tagBits &&
           counterMax == other.counterMax &&
           initialConfidence == other.initialConfidence &&
           mediumThreshold == other.mediumThreshold &&
           highThreshold == other.highThreshold &&
           hitIncrement == other.hitIncrement &&
           mediumSamplePeriod == other.mediumSamplePeriod &&
           missDecayPeriod == other.missDecayPeriod &&
           lowEntryMissStreakThreshold ==
               other.lowEntryMissStreakThreshold &&
           epochMask == other.epochMask &&
           globalCoverageGuardEnabled == other.globalCoverageGuardEnabled &&
           globalUnusedThreshold == other.globalUnusedThreshold &&
           globalMinResolvedCoverageShift ==
               other.globalMinResolvedCoverageShift;
}

BOP::BOP(const BOPPrefetcherParams &p)
    : Queued(p),
      scoreMax(p.score_max), roundMax(p.round_max),
      badScore(p.bad_score), rrEntries(p.rr_size),
      rrEvictionValidationEntries(p.rr_eviction_validation_entries),
      tagMask((1 << p.tag_bits) - 1),
      delayQueueEnabled(p.delay_queue_enable),
      delayQueueSize(p.delay_queue_size),
      delayTicks(cyclesToTicks(p.delay_queue_cycles)),
      crossPage(p.crossPage),
      enableAdaptOffset(p.enable_adaptoffset),
      enableIssueValidation(p.enable_issue_validation),
      enablePCValidationConfidence(p.enable_pc_validation_confidence),
      enableGlobalBOPCoverageGuard(p.enable_global_bop_coverage_guard),
      enableValidationShadowTrace(p.enable_validation_shadow_trace),
      pcValidationEntries(p.pc_validation_entries),
      pcValidationTagBits(p.pc_validation_tag_bits),
      pcValidationCounterBits(p.pc_validation_counter_bits),
      pcValidationInitial(p.pc_validation_initial),
      pcValidationMediumThreshold(p.pc_validation_medium_threshold),
      pcValidationHighThreshold(p.pc_validation_high_threshold),
      pcValidationHitIncrement(p.pc_validation_hit_increment),
      pcValidationMediumSamplePeriod(p.pc_validation_medium_sample_period),
      pcValidationMissDecayPeriod(p.pc_validation_miss_decay_period),
      pcValidationLowEntryMissStreakThreshold(
          p.pc_validation_low_entry_miss_streak_threshold),
      pcValidationEpochBits(p.pc_validation_epoch_bits),
      globalBOPUnusedThreshold(p.global_bop_unused_threshold),
      globalBOPMinResolvedCoverageShift(
          p.global_bop_min_resolved_coverage_shift),
      victimListSize(p.victimOffsetsListSize),
      restoreCycle(p.restoreCycle),
      delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()),
      issuePrefetchRequests(false), bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0), stats(this)
{
    pcValidationGenericName = name();
    pcValidationLargeName = name();
    pcValidationSmallName = name();

    if (!isPowerOf2(rrEntries)) {
        fatal("%s: number of RR entries is not power of 2\n", name());
    }
    if (!isPowerOf2(blkSize)) {
        fatal("%s: cache line size is not power of 2\n", name());
    }
    if (enableIssueValidation && enablePCValidationConfidence) {
        fatal("%s: strict and PC-confidence BOP validation are mutually exclusive\n",
              name());
    }
    if (enableGlobalBOPCoverageGuard && !enablePCValidationConfidence) {
        fatal("%s: global BOP coverage guard requires PC validation confidence\n",
              name());
    }
    if (enablePCValidationConfidence) {
        pcValidationTable = std::make_shared<PCValidationConfidenceTable>(
            pcValidationEntries, pcValidationTagBits, pcValidationCounterBits,
            pcValidationInitial, pcValidationMediumThreshold,
            pcValidationHighThreshold, pcValidationHitIncrement,
            pcValidationMediumSamplePeriod, pcValidationMissDecayPeriod,
            pcValidationLowEntryMissStreakThreshold,
            pcValidationEpochBits, enableGlobalBOPCoverageGuard,
            globalBOPUnusedThreshold, globalBOPMinResolvedCoverageShift);
    }

    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);

    int offset_count = p.offsets.size();
    maxOffsetCount = p.negative_offsets_enable ? 2*p.offsets.size() : p.offsets.size();
    if (p.autoLearning) {
        maxOffsetCount = 32;
    }


    for (int i = 0; i < offset_count; i++) {
        offsetsList.emplace_back(p.offsets[i], (uint8_t) 0);
        originOffsets.push_back(p.offsets[i]);
        DPRINTF(BOPPrefetcher, "add %d to offset list\n", p.offsets[i]);
        if (p.negative_offsets_enable) {
            offsetsList.emplace_back(-p.offsets[i], (uint8_t) 0);
            originOffsets.push_back(-p.offsets[i]);
            DPRINTF(BOPPrefetcher, "add %d to offset list\n", -p.offsets[i]);
        }
    }

    bestOffset = offsetsList.back().calcOffset();

    offsetsListIterator = offsetsList.begin();
    bestoffsetsListIterator = offsetsListIterator;

    restore_event = new EventFunctionWrapper([this](){
        assert(victimOffsetsList.size() > 0);
        int offset = victimOffsetsList.front();
        victimOffsetsList.pop_front();
        DPRINTF(BOPPrefetcher, "restore offset %d to offsetsList\n", offset);
        tryAddOffset(offset);
        if (victimOffsetsList.size() > 0) {
            DPRINTF(BOPPrefetcher, "start victimOffset restore\n");
            schedule(restore_event, cyclesToTicks(curCycle() + Cycles(restoreCycle)));
        }
        else {
            victimRestoreScheduled = false;
        }
    },name(),false);
}

void
BOP::delayQueueEventWrapper()
{
    if (!delayQueue.empty() &&
            delayQueue.front().processTick <= curTick())
    {
        insertIntoRR(delayQueue.front().rrEntry, RRWay::Left);
        delayQueue.pop_front();
    }

    // Schedule an event for the next element if there is one
    if (!delayQueue.empty() && (delayQueue.front().processTick <= curTick())) {
        schedule(delayQueueEvent, nextCycle());
    } else if (!delayQueue.empty()) {
        schedule(delayQueueEvent, delayQueue.front().processTick);
    }
}

unsigned int
BOP::hash(Addr addr, unsigned int way) const
{
    // NOTE: This unit-test BOP is used to replay XiangShan-generated traces.
    // Align RR indexing with XiangShan Chisel (BestOffsetPrefetch.scala):
    //   lineAddr = addr >> offsetBits
    //   hash1 = lineAddr[rrIdxBits-1:0]
    //   hash2 = lineAddr[2*rrIdxBits-1:rrIdxBits]
    //   idx   = hash1 ^ hash2
    //
    // The original gem5 BOP implementation used two banks (Left/Right) with
    // different hashing. XiangShan uses a single direct-mapped RR, so 'way'
    // is ignored here.
    //
    // Original gem5 BOP (indexed using the *tag* value, not full addr):
    //   Addr hash1 = tag >> way;
    //   Addr hash2 = hash1 >> floorLog2(rrEntries);
    //   idx = (hash1 ^ hash2) & (rrEntries - 1);
    (void)way;

    const unsigned rrIdxBits = floorLog2(rrEntries);
    const unsigned offsetBits = floorLog2(blkSize);
    const Addr line_addr = addr >> offsetBits;
    const Addr mask = static_cast<Addr>(rrEntries - 1);
    const Addr hash1 = line_addr & mask;
    const Addr hash2 = (line_addr >> rrIdxBits) & mask;
    return static_cast<unsigned int>((hash1 ^ hash2) & mask);
}

void
BOP::insertIntoRR(Addr full_addr, Addr tag, unsigned int way)
{
    insertIntoRR(RREntryDebug(full_addr, tag), way);
}

void
BOP::insertIntoRR(RREntryDebug rr_entry, unsigned int way)
{
    auto insert_into_table = [this, &rr_entry](std::vector<RREntryDebug> &table,
                                               unsigned int index) {
        auto &slot = table[index];
        if (slot.valid && slot.fullAddr != rr_entry.fullAddr) {
            recordRREviction(slot);
        }
        slot = rr_entry;
    };

    switch (way) {
        case RRWay::Left:
            insert_into_table(rrLeft, hash(rr_entry.fullAddr, RRWay::Left));
            break;
        case RRWay::Right:
            insert_into_table(rrRight, hash(rr_entry.fullAddr, RRWay::Right));
            break;
    }
}

void
BOP::recordRREviction(const RREntryDebug &rr_entry)
{
    if (rrEvictionValidationEntries == 0 || !rr_entry.valid) {
        return;
    }

    auto it = rrEvictionValidationCounts.find(rr_entry.fullAddr);
    if (it != rrEvictionValidationCounts.end() && it->second > 0) {
        stats.rrEvictionValidationDuplicateInserts++;
    }

    rrEvictionValidationQueue.push_back(rr_entry);
    rrEvictionValidationCounts[rr_entry.fullAddr]++;
    stats.rrEvictionValidationInserted++;

    if (rrEvictionValidationQueue.size() > rrEvictionValidationEntries) {
        const auto evicted = rrEvictionValidationQueue.front();
        rrEvictionValidationQueue.pop_front();

        auto evicted_it = rrEvictionValidationCounts.find(evicted.fullAddr);
        if (evicted_it != rrEvictionValidationCounts.end()) {
            if (evicted_it->second <= 1) {
                rrEvictionValidationCounts.erase(evicted_it);
            } else {
                evicted_it->second--;
            }
        }
        stats.rrEvictionValidationDropped++;
    }
}

bool
BOP::testRREvictionValidation(Addr full_addr) const
{
    if (rrEvictionValidationEntries == 0) {
        return false;
    }

    const auto it = rrEvictionValidationCounts.find(full_addr);
    return it != rrEvictionValidationCounts.end() && it->second > 0;
}

bool
BOP::insertIntoDelayQueue(Addr full_addr, Addr tag)
{
    if (delayQueue.size() == delayQueueSize) {
        return false;
    }

    // Add the address to the delay queue and schedule an event to process
    // it after the specified delay cycles
    Tick process_tick = curTick() + delayTicks;

    delayQueue.push_back(DelayQueueEntry({full_addr, tag}, process_tick));

    if (!delayQueueEvent.scheduled()) {
        schedule(delayQueueEvent, process_tick);
    }
    return true;
}

void
BOP::resetScores()
{
    for (auto& it : offsetsList) {
        it.score = 0;
    }
}

inline Addr
BOP::tag(Addr addr) const
{
    // Align tag extraction with XiangShan Chisel (BestOffsetPrefetch.scala):
    //   tag = lineAddr[rrIdxBits+rrTagBits-1:rrIdxBits]
    // where lineAddr = addr >> offsetBits.
    //
    // Original gem5 BOP (commented) used:
    //   (addr >> offsetBits) & tagMask
    // which kept the lowest tagBits of the line address.
    const unsigned rrIdxBits = floorLog2(rrEntries);
    const unsigned offsetBits = floorLog2(blkSize);
    const Addr line_addr = addr >> offsetBits;
    return (line_addr >> rrIdxBits) & tagMask;
}

std::pair<bool, BOP::RREntryDebug>
BOP::testRR(Addr addr) const
{
    const Addr t = tag(addr);
    const unsigned idx_l = hash(addr, RRWay::Left);
    if (rrLeft[idx_l].hashAddr == t) {
        return std::make_pair(true, rrLeft[idx_l]);
    }
    const unsigned idx_r = hash(addr, RRWay::Right);
    if (rrRight[idx_r].hashAddr == t) {
        return std::make_pair(true, rrRight[idx_r]);
    }

    return std::make_pair(false, RREntryDebug());
}

BOP::ValidationProbeResult
BOP::probeValidation(Addr validation_addr,
                     bool include_rr_eviction_validation)
{
    ValidationProbeResult result;
    result.probed = true;
    result.rrIndex = hash(validation_addr, RRWay::Left);
    result.rrExpectedTag = tag(validation_addr);
    result.delayQueueOccupancy = delayQueue.size();
    result.delayQueueFull = delayQueue.size() == delayQueueSize;

    const auto delay_it = std::find_if(
        delayQueue.begin(), delayQueue.end(),
        [validation_addr](const DelayQueueEntry &entry) {
            return entry.rrEntry.fullAddr == validation_addr;
        });
    if (delay_it != delayQueue.end()) {
        result.delayQueuePresent = true;
        result.delayQueuePosition = std::distance(delayQueue.begin(), delay_it);
        result.delayQueueProcessTick = static_cast<int64_t>(delay_it->processTick);
        result.rrReason = 2;
    }

    const unsigned idx_l = hash(validation_addr, RRWay::Left);
    const unsigned idx_r = hash(validation_addr, RRWay::Right);
    const auto &left = rrLeft[idx_l];
    const auto &right = rrRight[idx_r];
    result.leftSlotValid = left.valid;
    result.leftSlotFullAddr = left.fullAddr;
    result.leftSlotTag = left.hashAddr;
    result.rightSlotValid = right.valid;
    result.rightSlotFullAddr = right.fullAddr;
    result.rightSlotTag = right.hashAddr;

    if (left.valid && left.hashAddr == result.rrExpectedTag) {
        result.validationHit = true;
        result.rrHitWay = RRWay::Left;
        result.rrReason = 1;
    } else if (right.valid && right.hashAddr == result.rrExpectedTag) {
        result.validationHit = true;
        result.rrHitWay = RRWay::Right;
        result.rrReason = 1;
    } else if (include_rr_eviction_validation &&
               rrEvictionValidationEntries > 0) {
        stats.rrEvictionValidationLookups++;
        if (testRREvictionValidation(validation_addr)) {
            stats.rrEvictionValidationHits++;
            result.validationHit = true;
            result.rrReason = 5;
        } else if (!result.delayQueuePresent) {
            result.rrReason = (!left.valid && !right.valid) ? 3 : 4;
        }
    } else if (!result.delayQueuePresent) {
        result.rrReason = (!left.valid && !right.valid) ? 3 : 4;
    }

    return result;
}

bool
BOP::tryAddOffset(int64_t offset, bool late)
{
    assert(offset != 0);
    bool find_it = std::find(offsetsList.begin(), offsetsList.end(), offset) != offsetsList.end();
    if (find_it) {
        return false;
    }
    if (victimOffsetsList.size() >= victimListSize) {
        DPRINTF(BOPPrefetcher, "victimOffsetsList is full, can't add offset\n");
        return false;
    }

    DPRINTF(BOPPrefetcher, "Reach %s entry, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    // dump offsets:
    DPRINTF(BOPPrefetcher, "offset list:\n");
    for (const auto& it : offsetsList) {
        DPRINTF(BOPPrefetcher, "%d*%d\n", it.offset, it.depth);
    }
    DPRINTF(BOPPrefetcher, "victim offset list:\n");
    for (const auto& it : victimOffsetsList) {
        DPRINTF(BOPPrefetcher, "%d\n", it);
    }

    if (offsetsList.size() >= maxOffsetCount) {
        int evict_offset = 0;
        auto it = offsetsList.begin();
        while (it != offsetsList.end()) {
            if (it->score <= badScore) {
                break;
            }
            it++;
        }
        if (it == offsetsList.end()) {
            // all offsets are good, erase the one before the iterator
            if (offsetsListIterator == offsetsList.begin()) {
                // the iterator is the first element, erase the last one
                DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "erase offset %d from offset list\n",
                        offsetsList.rbegin()->offset);
                auto end_offset = --offsetsList.end();
                evict_offset = end_offset->offset;
                offsetsList.erase(end_offset);
            } else {
                auto temp = --offsetsListIterator;
                DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "erase offset %d from offset list\n",
                        temp->offset);
                evict_offset = temp->offset;
                offsetsListIterator = offsetsList.erase(temp);
            }
        } else {
            // erase it from set and list
            DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "erase unused offset %d from offset list\n",
                     it->offset);
            evict_offset = it->offset;
            if (it == offsetsListIterator) {
                offsetsListIterator = offsetsList.erase(it);  // update iterator
                if (offsetsListIterator == offsetsList.end()) {
                    offsetsListIterator = offsetsList.begin();
                }
            } else {
                offsetsList.erase(it);
            }
            DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "%s after erase: iter offset: %d\n", __FUNCTION__,
                     offsetsListIterator->calcOffset());
        }
        assert(evict_offset != 0);
        if (std::find(originOffsets.begin(), originOffsets.end(), evict_offset) != originOffsets.end()) {
            DPRINTF(BOPPrefetcher, "add offset %d to victimOffsetsList\n", evict_offset);
            victimOffsetsList.push_back(evict_offset);
        }
    }

    auto best_it = getBestOffsetIter();

    auto offset_it = std::find(offsetsList.begin(), offsetsList.end(), offset);
    if (offset_it == offsetsList.end()) {
        bool found = false;
        for (auto it = offsetsList.begin(); it != offsetsList.end(); it++) {
            if (it == offsetsListIterator) {
                found = true;
            }
        }
        DPRINTF(BOPPrefetcher, "%s mid: iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
        assert(found);
        // insert it next to the offsetsListIterator
        auto next_it = std::next(offsetsListIterator);
        offsetsList.emplace(next_it, (int32_t) offset, (uint8_t) 0);
        stats.learnOffsetCount++;
        DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "add %d to offset list\n", offset);

    } else {
        bool found = false;
        for (auto it = offsetsList.begin(); it != offsetsList.end(); it++) {
            if (it->offset == offset) {
                found = true;
                break;
            } else {
                DPRINTF(BOPPrefetcher || debug::BOPOffsets, "offset %d != %ld\n", offset, it->offset);
            }
        }
        assert(found);
    }
    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    return true;
}

std::list<BOP::OffsetListEntry>::iterator
BOP::getBestOffsetIter()
{
    return std::find(offsetsList.begin(), offsetsList.end(), bestOffset);
}

bool
BOP::bestOffsetLearning(Addr x, bool late, const PrefetchInfo &pfi)
{
    DPRINTF(BOPPrefetcher, "Reach %s entry, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    Addr offset = offsetsListIterator->calcOffset();
    Addr lookup_addr = x - (offset << lBlkSize);
    DPRINTF(BOPPrefetcher, "%s: offset: %d lookup addr: %#lx\n", __FUNCTION__, offset, lookup_addr);
    // There was a hit in the RR table, increment the score for this offset
    auto [exist, rr_entry] = testRR(lookup_addr);
    if (exist) {
        if (archDBer) {
            archDBer->bopTrainTraceWrite(curTick(), rr_entry.fullAddr, pfi.getAddr(), offset,
                                        offsetsListIterator->score + 1, pfi.isCacheMiss());
        }

        DPRINTF(BOPPrefetcher, "Address %#lx found in the RR table\n", x);
        offsetsListIterator->score++;
        if (enableAdaptOffset) {
            if (offsetsListIterator->score >= round / 2) {
                if (late) {
                    offsetsListIterator->late += 2;
                } else {
                    offsetsListIterator->late--;
                }

                auto best_it = getBestOffsetIter();
                bool update_depth = false;
                if (offsetsListIterator->late > (uint8_t)42) {
                    offsetsListIterator->depth++;
                    update_depth = true;
                }
                if (offsetsListIterator->late < (uint8_t)4) {
                    offsetsListIterator->depth = std::max(1, offsetsListIterator->depth - 1);
                    update_depth = true;
                }

                if (update_depth) {
                    if (best_it == offsetsListIterator) {
                        bestOffset = best_it->calcOffset();
                    }
                    DPRINTF(BOPPrefetcher, "Late saturates %u, offset updated to %d * %d\n",
                            (uint8_t)offsetsListIterator->late, offsetsListIterator->offset,
                            offsetsListIterator->depth);
                    offsetsListIterator->late.reset();
                }
            }
        }

        DPRINTF(BOPPrefetcher, "Offset %d score: %i, late: %i, depth: %i, late sat: %u\n", offsetsListIterator->offset,
                offsetsListIterator->score, late, offsetsListIterator->depth, (uint8_t)offsetsListIterator->late);
        if (offsetsListIterator->score > bestScore) {
            bestoffsetsListIterator = offsetsListIterator;
            bestScore = (*offsetsListIterator).score;
            phaseBestOffset = offsetsListIterator->calcOffset();
            DPRINTF(BOPPrefetcher, "New best score is %lu, phase best offset is %lu\n", bestScore, phaseBestOffset);
        }
    }

    offsetsListIterator++;

    // All the offsets in the list were visited meaning that a learning
    // phase finished. Check if
    if (offsetsListIterator == offsetsList.end()) {
        offsetsListIterator = offsetsList.begin();
        round++;

        // Check if the best offset must be updated if:
        // (1) One of the scores equals SCORE_MAX
        // (2) The number of rounds equals ROUND_MAX
        if ((bestScore >= scoreMax) || (round == roundMax)) {
            DPRINTF(BOPPrefetcher, "update new score: %d round: %d phase best offset: %d\n",
                    bestScore, round, phaseBestOffset);

            if (bestScore > badScore) {
                issuePrefetchRequests = true;
                DPRINTF(BOPPrefetcher, "Enable prefetch\n");
            } else {
                issuePrefetchRequests = false;
                DPRINTF(BOPPrefetcher, "Disable prefetch\n");
            }

            bestOffset = phaseBestOffset;
            round = 0;
            bestScore = 0;
            phaseBestOffset = 0;
            resetScores();
            //issuePrefetchRequests = true;
            return true;
         } // here temporarily disable early stop, to align with RTL
        // else if ((round >= roundMax/2) && (bestOffset != phaseBestOffset) && (bestScore <= badScore)) {
        //     DPRINTF(BOPPrefetcher, "last round offset has not enough confidence, early stop\n");
        //     DPRINTF(BOPPrefetcher, "score %u <  badScore %u\n", bestScore, badScore);
        //     issuePrefetchRequests = false;
        // }
    }
    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    return false;
}

void
BOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late)
{
    Addr addr = blockAddress(pfi.getAddr());
    Addr tag_x = tag(addr);
    const Addr trigger_pc = pfi.hasPC() ? pfi.getPC() : 0;
    const bool trigger_is_demand =
        pfi.trigger_info.pkt && pfi.trigger_info.pkt->isDemand();
    const int trigger_pf_source =
        static_cast<int>(pfi.getXsMetadata().prefetchSource);

    DPRINTF(BOPPrefetcher,
            "Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

    if (delayQueueEnabled) {
        insertIntoDelayQueue(addr, tag_x);
    } else {
        insertIntoRR(addr, tag_x, RRWay::Left);
    }

    // Go through the nth offset and update the score, the best score and the
    // current best offset if a better one is found.
    const int64_t previous_best_offset = bestOffset;
    bestOffsetLearning(addr, late, pfi);
    const bool best_offset_changed = bestOffset != previous_best_offset;
    if (enablePCValidationConfidence && best_offset_changed) {
        pcValidationTable->noteOffsetChange(pcValidationKind);
    }

    const Addr validation_addr = bestOffset != 0
        ? addr - (static_cast<Addr>(bestOffset) << lBlkSize) : 0;
    const Addr prefetch_addr = bestOffset != 0
        ? addr + (bestOffset * (1ULL << lBlkSize)) : 0;
    bool issue_prefetch = issuePrefetchRequests;
    int validation_hit = -1;
    int pc_entry_hit = -1;
    int pc_confidence = -1;
    int pc_state = static_cast<int>(PCConfidenceState::None);
    int pc_sampled = 0;
    int pc_low_entry_miss_streak = -1;
    int pc_epoch = -1;
    int pc_index = -1;
    Addr pc_tag = 0;
    bool bypass_mode = false;
    const bool validation_enabled =
        enableIssueValidation || enablePCValidationConfidence;
    ValidationProbeResult validation_probe;
    const bool shadow_validation_enabled =
        enableValidationShadowTrace && !validation_enabled;

    if (issue_prefetch && enableIssueValidation) {
        assert(bestOffset != 0);
        validation_probe = probeValidation(validation_addr, false);
        validation_hit = validation_probe.validationHit;

        stats.issueValidationChecks++;
        if (validation_hit) {
            stats.issueValidationHits++;
        } else {
            stats.issueValidationSuppressed++;
            issue_prefetch = false;
        }
        DPRINTF(BOPPrefetcher, "Issue validation addr %#lx best offset %lld: %s\n", validation_addr,
                static_cast<long long>(bestOffset), validation_hit ? "hit" : "miss");
    } else if (issue_prefetch && enablePCValidationConfidence) {
        assert(bestOffset != 0);
        validation_probe = probeValidation(validation_addr, true);
        validation_hit = validation_probe.validationHit;
        stats.issueValidationChecks++;
        if (validation_hit) {
            stats.issueValidationHits++;
        }

        if (pfi.hasPC()) {
            const auto pc_lookup =
                pcValidationTable->lookup(trigger_pc, pcValidationKind);
            pc_entry_hit = pc_lookup.entryHit;
            pc_confidence = pc_lookup.confidence;
            pc_low_entry_miss_streak = pc_lookup.lowEntryMissStreak;
            pc_state = static_cast<int>(pc_lookup.state);
            pc_epoch = pc_lookup.epoch;
            pc_index = pc_lookup.index;
            pc_tag = pc_lookup.tag;

            stats.pcValidationTableLookups++;
            if (pc_lookup.entryHit) {
                stats.pcValidationTableHits++;
            } else {
                stats.pcValidationTableMisses++;
            }
            if (pc_lookup.replaced) {
                stats.pcValidationTableReplacements++;
            }
            if (pc_lookup.epochReset) {
                stats.pcValidationEpochResets++;
            }
            stats.pcValidationConfidenceDist.sample(pc_lookup.confidence);

            if (!validation_hit) {
                if (pcValidationTable->notePCValidationMiss()) {
                    stats.globalBOPBypassModeIdleResets++;
                }
                bypass_mode = pcValidationTable->bypassPCValidationActive();
                if (bypass_mode) {
                    stats.globalBOPBypassModeChecks++;
                    stats.globalBOPBypassModeIssued++;
                    switch (pc_lookup.state) {
                      case PCConfidenceState::High:
                        stats.globalBOPBypassModeHighIssued++;
                        break;
                      case PCConfidenceState::Medium:
                        stats.globalBOPBypassModeMediumIssued++;
                        break;
                      case PCConfidenceState::Low:
                        stats.globalBOPBypassModeLowIssued++;
                        break;
                      case PCConfidenceState::None:
                        panic("Missing PC validation confidence state\n");
                        break;
                    }
                } else {
                    switch (pc_lookup.state) {
                      case PCConfidenceState::High:
                        stats.pcValidationHighMissIssued++;
                        break;
                      case PCConfidenceState::Medium:
                        pc_sampled = pcValidationTable->sampleMediumIssue(
                            trigger_pc, pcValidationKind, addr >> lBlkSize);
                        if (pc_sampled) {
                            stats.pcValidationMediumMissIssued++;
                        } else {
                            issue_prefetch = false;
                            stats.issueValidationSuppressed++;
                            stats.pcValidationMediumMissSuppressed++;
                        }
                        break;
                      case PCConfidenceState::Low:
                        issue_prefetch = false;
                        stats.issueValidationSuppressed++;
                        stats.pcValidationLowMissSuppressed++;
                        break;
                      case PCConfidenceState::None:
                        panic("Missing PC validation confidence state\n");
                    }
                }
            }
            pcValidationTable->submitValidation(
                pc_lookup, trigger_pc, addr >> lBlkSize, validation_hit);
        } else if (!validation_hit) {
            if (pcValidationTable->notePCValidationMiss()) {
                stats.globalBOPBypassModeIdleResets++;
            }
            bypass_mode = pcValidationTable->bypassPCValidationActive();
            if (bypass_mode) {
                stats.globalBOPBypassModeChecks++;
                stats.globalBOPBypassModeIssued++;
                stats.globalBOPBypassModeNoPCIssued++;
            } else {
                issue_prefetch = false;
                stats.issueValidationSuppressed++;
                stats.pcValidationNoPCSuppressions++;
            }
        }

        DPRINTF(BOPPrefetcher,
                "PC validation addr %#lx offset %lld: RR %s, PC state %d, "
                "confidence %d, issue %d, bypass %d\n",
                validation_addr, static_cast<long long>(bestOffset),
                validation_hit ? "hit" : "miss", pc_state, pc_confidence,
                issue_prefetch, bypass_mode);
    }

    if (issue_prefetch && shadow_validation_enabled && bestOffset != 0) {
        validation_probe = probeValidation(validation_addr, true);
        validation_hit = validation_probe.validationHit;
    }

    // This prefetcher is a degree 1 prefetch, so it will only generate one
    // prefetch at most per access.
    bool generated = false;
    bool buffered = false;
    bool filtered = false;
    bool filter_passed = false;
    if (issue_prefetch) {
        generated = true;
        buffered = samePage(pfi.getAddr(), prefetch_addr) || crossPage;
        stats.issuedOffsetDist.sample(bestOffset);
        filter_passed = sendPFWithFilter(
            pfi, prefetch_addr, addresses, 32, PrefetchSourceType::HWP_BOP);
        if (filter_passed && enableGlobalBOPCoverageGuard) {
            stats.globalBOPIssued++;
            pcValidationTable->noteGlobalBOPIssued();
        }
        filtered = !filter_passed;
        DPRINTF(BOPPrefetcher,
                "Generated prefetch %#lx offset: %d\n",
                prefetch_addr, bestOffset);
    } else if (!issuePrefetchRequests) {
        stats.throttledCount++;
        DPRINTF(BOPPrefetcher, "Issue prefetch is false, can't issue\n");
    }

    if (archDBer) {
        const auto global_trace = enableGlobalBOPCoverageGuard &&
            enablePCValidationConfidence ?
            pcValidationTable->traceState() :
            PCValidationConfidenceTable::GlobalTraceState();
        archDBer->bopValidationTraceWrite(
            curTick(), "candidate", name().c_str(), trigger_pc, addr,
            validation_addr, prefetch_addr, bestOffset, bestScore, round, late,
            trigger_is_demand, pfi.isCacheMiss(), trigger_pf_source,
            pfi.isPfFirstHit(), pfi.isPfHit(), issuePrefetchRequests,
            validation_enabled || shadow_validation_enabled, validation_hit,
            issuePrefetchRequests && validation_enabled && !issue_prefetch,
            generated, buffered, filtered, filter_passed,
            enablePCValidationConfidence, pc_index, pc_tag, pc_entry_hit,
            pc_confidence, pc_state, pc_sampled, pc_epoch,
            pc_low_entry_miss_streak,
            validation_probe.probed, validation_probe.rrIndex,
            validation_probe.rrExpectedTag, validation_probe.rrReason,
            validation_probe.validationHit, validation_probe.rrHitWay,
            validation_probe.delayQueuePresent,
            validation_probe.delayQueuePosition,
            validation_probe.delayQueueProcessTick,
            validation_probe.delayQueueOccupancy,
            validation_probe.delayQueueFull,
            validation_probe.leftSlotValid,
            validation_probe.leftSlotFullAddr,
            validation_probe.leftSlotTag,
            validation_probe.rightSlotValid,
            validation_probe.rightSlotFullAddr,
            validation_probe.rightSlotTag,
            global_trace.enabled, global_trace.bypassActive,
            global_trace.unusedEwma, global_trace.outcomeWindowResolved,
            global_trace.outcomeWindowUnused, global_trace.issuedWindowIssued,
            global_trace.checksSinceOutcome);
    }

    // A BOP outside a large/small composite still has well-defined behavior:
    // commit its one participant immediately. Shared pairs commit explicitly
    // after both engines have submitted their validation result.
    if (enablePCValidationConfidence && !pcValidationTableShared) {
        commitPCValidationConfidence();
    }

    if (!victimRestoreScheduled && victimOffsetsList.size() > 0) {
        victimRestoreScheduled = true;
        DPRINTF(BOPPrefetcher, "start victimOffset restore\n");
        schedule(restore_event, cyclesToTicks(curCycle() + Cycles(restoreCycle)));
    }

    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
}

void
BOP::sharePCValidationConfidenceWith(BOP &other)
{
    if (enablePCValidationConfidence != other.enablePCValidationConfidence) {
        fatal("%s and %s must agree on PC validation confidence enablement\n",
              name(), other.name());
    }
    if (!enablePCValidationConfidence) {
        return;
    }
    if (!pcValidationTable->configMatches(*other.pcValidationTable)) {
        fatal("%s and %s must use matching PC validation confidence parameters\n",
              name(), other.name());
    }
    other.pcValidationTable = pcValidationTable;
    pcValidationTableShared = true;
    other.pcValidationTableShared = true;
    pcValidationKind = PCValidationKind::Large;
    other.pcValidationKind = PCValidationKind::Small;
    pcValidationLargeName = name();
    pcValidationSmallName = other.name();
    other.pcValidationLargeName = pcValidationLargeName;
    other.pcValidationSmallName = pcValidationSmallName;
}

const char *
BOP::pcValidationTraceName(PCValidationKind kind) const
{
    switch (kind) {
      case PCValidationKind::Generic:
        return pcValidationGenericName.c_str();
      case PCValidationKind::Large:
        return pcValidationLargeName.c_str();
      case PCValidationKind::Small:
        return pcValidationSmallName.c_str();
    }
    return pcValidationKindName(kind);
}

void
BOP::tracePCValidationUpdate(
    const PCValidationConfidenceTable::CommitResult &result)
{
    if (!archDBer || !result.hadPending) {
        return;
    }

    archDBer->bopValidationConfidenceUpdateTraceWrite(
        curTick(), pcValidationTraceName(result.kind), result.pc, result.index,
        result.tag,
        result.validationHit, result.participants, result.confidenceBefore,
        result.confidenceAfter, result.decayed, result.offsetChanged,
        result.epochAfter, result.lowEntryMissStreakBefore,
        result.lowEntryMissStreakAfter, result.lowEntryHysteresisHeld,
        result.lowEntryHysteresisTransition);
}

void
BOP::commitPCValidationConfidence()
{
    if (!enablePCValidationConfidence) {
        return;
    }

    const auto results = pcValidationTable->commit();
    if (results.empty()) {
        return;
    }
    for (const auto &result : results) {
        if (result.offsetChanged) {
            stats.pcValidationOffsetEpochChanges++;
        }
        if (result.hadValidation && !result.offsetChanged) {
            if (result.validationHit) {
                stats.pcValidationHitUpdates++;
            } else if (result.decayed) {
                stats.pcValidationMissDecays++;
            } else {
                stats.pcValidationMissNoDecays++;
            }
            if (result.lowEntryHysteresisHeld) {
                stats.pcValidationLowEntryHysteresisHolds++;
            }
            if (result.lowEntryHysteresisTransition) {
                stats.pcValidationLowEntryHysteresisTransitions++;
            }
        }
        tracePCValidationUpdate(result);
    }
}

void
BOP::notifyGlobalBOPOutcome(bool useful)
{
    if (!enableGlobalBOPCoverageGuard) {
        return;
    }

    if (useful) {
        stats.globalBOPOutcomeUseful++;
    } else {
        stats.globalBOPOutcomeUnused++;
    }

    const auto result = pcValidationTable->noteGlobalBOPOutcome(useful);
    if (!result.ewmaUpdated) {
        return;
    }

    stats.globalBOPUnusedEwmaUpdates++;
    stats.globalBOPUnusedEwma.sample(result.unusedEwma);
    stats.globalBOPResolvedCoverage.sample(result.resolvedCoverageQ08);
    if (result.resolvedCoverageGood) {
        stats.globalBOPResolvedCoverageGood++;
    } else {
        stats.globalBOPResolvedCoverageBad++;
    }
    if (result.bypassBlockedByLowCoverage) {
        stats.globalBOPBypassBlockedLowCoverage++;
    }
    if (result.bypassModeEntered) {
        stats.globalBOPBypassModeEntries++;
    }
    if (result.bypassModeExited) {
        stats.globalBOPBypassModeExits++;
    }
}

bool
BOP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                      PrefetchSourceType src)
{
    // Count generated prefetch
    prefetchStats.pfGenerated++;

    if (!samePage(pfi.getAddr(), addr) && !crossPage) {
        // Count filtered prefetch (cross-page)
        prefetchStats.pfFiltered++;
        return false;
    }
    if (archDBer && cache->level() == 1) {
        archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
    }
    InsertPFRequestToBuffer(AddrPriority(addr, prio, src, pfi.trigger_info));
    if (filter->contains(addr)) {
        DPRINTF(BOPPrefetcher, "Skip recently prefetched: %lx\n", addr);
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;
    } else {
        DPRINTF(BOPPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}

void
BOP::notifyFill(const PacketPtr& pkt)
{

}

BOP::BopStats::BopStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(issuedOffsetDist, statistics::units::Count::get(), "Distribution of issued offsets"),
      ADD_STAT(learnOffsetCount, statistics::units::Count::get(), "Number of learning offsets"),
      ADD_STAT(throttledCount, statistics::units::Count::get(), "Number of globally throttled prefetches"),
      ADD_STAT(issueValidationChecks, statistics::units::Count::get(),
               "Number of current-best-offset issue validation checks"),
      ADD_STAT(issueValidationHits, statistics::units::Count::get(),
               "Number of current-best-offset validation hits"),
      ADD_STAT(issueValidationSuppressed, statistics::units::Count::get(),
               "Number of BOP prefetches suppressed by issue validation"),
      ADD_STAT(rrEvictionValidationInserted, statistics::units::Count::get(),
               "Number of RR entries copied into the eviction victim table"),
      ADD_STAT(rrEvictionValidationDropped, statistics::units::Count::get(),
               "Number of RR eviction victim entries dropped from FIFO"),
      ADD_STAT(rrEvictionValidationDuplicateInserts,
               statistics::units::Count::get(),
               "Number of RR eviction victim addresses inserted more than once"),
      ADD_STAT(rrEvictionValidationLookups, statistics::units::Count::get(),
               "Number of RR eviction victim table lookups"),
      ADD_STAT(rrEvictionValidationHits, statistics::units::Count::get(),
               "Number of RR eviction victim table hits"),
      ADD_STAT(pcValidationTableLookups, statistics::units::Count::get(),
               "Number of PC validation-confidence table lookups"),
      ADD_STAT(pcValidationTableHits, statistics::units::Count::get(),
               "Number of PC validation-confidence partial-tag hits"),
      ADD_STAT(pcValidationTableMisses, statistics::units::Count::get(),
               "Number of PC validation-confidence misses"),
      ADD_STAT(pcValidationTableReplacements, statistics::units::Count::get(),
               "Number of valid PC validation-confidence entries replaced"),
      ADD_STAT(pcValidationEpochResets, statistics::units::Count::get(),
               "Number of lazy PC validation-confidence epoch resets"),
      ADD_STAT(pcValidationNoPCSuppressions, statistics::units::Count::get(),
               "Validation misses suppressed because the trigger has no PC"),
      ADD_STAT(pcValidationHighMissIssued, statistics::units::Count::get(),
               "Validation misses issued at high PC confidence"),
      ADD_STAT(pcValidationMediumMissIssued, statistics::units::Count::get(),
               "Sampled validation misses issued at medium PC confidence"),
      ADD_STAT(pcValidationMediumMissSuppressed, statistics::units::Count::get(),
               "Unsampled validation misses suppressed at medium PC confidence"),
      ADD_STAT(pcValidationLowMissSuppressed, statistics::units::Count::get(),
               "Validation misses suppressed at low PC confidence"),
      ADD_STAT(pcValidationHitUpdates, statistics::units::Count::get(),
               "Committed PC-kind validation-hit confidence updates"),
      ADD_STAT(pcValidationMissDecays, statistics::units::Count::get(),
               "Committed sampled validation-miss confidence decays"),
      ADD_STAT(pcValidationMissNoDecays, statistics::units::Count::get(),
               "Committed validation-miss updates without sampled decay"),
      ADD_STAT(pcValidationLowEntryHysteresisHolds,
               statistics::units::Count::get(),
               "Sampled all-miss updates held at the medium-to-low boundary"),
      ADD_STAT(pcValidationLowEntryHysteresisTransitions,
               statistics::units::Count::get(),
               "Medium-to-low transitions released after local miss streak"),
      ADD_STAT(pcValidationOffsetEpochChanges, statistics::units::Count::get(),
               "PC-kind validation-confidence epoch changes"),
      ADD_STAT(pcValidationConfidenceDist, statistics::units::Count::get(),
               "PC validation confidence observed at candidate issue"),
      ADD_STAT(globalBOPOutcomeUseful, statistics::units::Count::get(),
               "Resolved useful BOP outcomes received by the global guard"),
      ADD_STAT(globalBOPOutcomeUnused, statistics::units::Count::get(),
               "Resolved unused BOP outcomes received by the global guard"),
      ADD_STAT(globalBOPIssued, statistics::units::Count::get(),
               "BOP prefetches admitted into the global guard issued window"),
      ADD_STAT(globalBOPUnusedEwmaUpdates, statistics::units::Count::get(),
               "Completed global BOP outcome windows folded into the EWMA"),
      ADD_STAT(globalBOPResolvedCoverageGood,
               statistics::units::Count::get(),
               "Global BOP outcome windows with sufficient resolved coverage"),
      ADD_STAT(globalBOPResolvedCoverageBad,
               statistics::units::Count::get(),
               "Global BOP outcome windows with insufficient resolved coverage"),
      ADD_STAT(globalBOPBypassBlockedLowCoverage,
               statistics::units::Count::get(),
               "Healthy-unused windows blocked from bypass by low resolved coverage"),
      ADD_STAT(globalBOPBypassModeEntries, statistics::units::Count::get(),
               "Entries into global BOP bypass mode"),
      ADD_STAT(globalBOPBypassModeExits, statistics::units::Count::get(),
               "Exits from global BOP bypass mode"),
      ADD_STAT(globalBOPBypassModeIdleResets, statistics::units::Count::get(),
               "Global BOP bypass-mode resets after feedback inactivity"),
      ADD_STAT(globalBOPBypassModeChecks, statistics::units::Count::get(),
               "Validation misses observed while global bypass mode is active"),
      ADD_STAT(globalBOPBypassModeIssued, statistics::units::Count::get(),
               "Validation-miss prefetches issued while global bypass mode is active"),
      ADD_STAT(globalBOPBypassModeHighIssued, statistics::units::Count::get(),
               "High-confidence validation misses issued while global bypass mode is active"),
      ADD_STAT(globalBOPBypassModeMediumIssued,
               statistics::units::Count::get(),
               "Medium-confidence validation misses issued while global bypass mode is active"),
      ADD_STAT(globalBOPBypassModeLowIssued, statistics::units::Count::get(),
               "Low-confidence validation misses issued while global bypass mode is active"),
      ADD_STAT(globalBOPBypassModeNoPCIssued, statistics::units::Count::get(),
               "No-PC validation misses issued while global bypass mode is active"),
      ADD_STAT(globalBOPUnusedEwma, statistics::units::Count::get(),
               "Q0.8 global BOP unused-rate EWMA after completed windows"),
      ADD_STAT(globalBOPResolvedCoverage, statistics::units::Count::get(),
               "Q0.8 resolved coverage for completed global BOP outcome windows")
{
    issuedOffsetDist.init(-64, 256, 1).prereq(issuedOffsetDist);
    pcValidationConfidenceDist.init(0, 256, 1).prereq(
        pcValidationConfidenceDist);
    globalBOPUnusedEwma.init(0, 256, 1).prereq(globalBOPUnusedEwma);
    globalBOPResolvedCoverage.init(0, 256, 1).prereq(
        globalBOPResolvedCoverage);
}

} // namespace prefetch
} // namespace gem5
