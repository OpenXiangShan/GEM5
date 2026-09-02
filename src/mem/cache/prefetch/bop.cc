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
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <tuple>

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

namespace
{

uint64_t
splitmix64(uint64_t value)
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

} // anonymous namespace

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
    unsigned int low_entry_miss_streak_threshold,
    unsigned int offset_context_slots,
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
      offsetContextSlots(offset_context_slots),
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
    if (offsetContextSlots == 0 || offsetContextSlots >
            PC_VALIDATION_MAX_OFFSET_CONTEXT_SLOTS ||
        !isPowerOf2(offsetContextSlots)) {
        fatal("BOP PC validation offset context slots must be a power of two "
              "in [1, %u]\n", PC_VALIDATION_MAX_OFFSET_CONTEXT_SLOTS);
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

unsigned int
BOP::PCValidationConfidenceTable::contextVictim(const Entry &entry) const
{
    switch (offsetContextSlots) {
      case 1:
        return 0;
      case 2:
        return entry.contextPLRU & 0x1;
      case 4:
        if ((entry.contextPLRU & 0x1) == 0) {
            return (entry.contextPLRU & 0x2) == 0 ? 0 : 1;
        }
        return (entry.contextPLRU & 0x4) == 0 ? 2 : 3;
      default:
        panic("Invalid BOP PC validation offset context slot count\n");
    }
}

void
BOP::PCValidationConfidenceTable::touchContext(
    Entry &entry, unsigned int context_way)
{
    assert(context_way < offsetContextSlots);
    if (offsetContextSlots == 1) {
        return;
    }
    if (offsetContextSlots == 2) {
        entry.contextPLRU = context_way == 0 ? 1 : 0;
        return;
    }

    switch (context_way) {
      case 0:
        entry.contextPLRU |= 0x1;
        entry.contextPLRU |= 0x2;
        break;
      case 1:
        entry.contextPLRU |= 0x1;
        entry.contextPLRU &= ~0x2;
        break;
      case 2:
        entry.contextPLRU &= ~0x1;
        entry.contextPLRU |= 0x4;
        break;
      case 3:
        entry.contextPLRU &= ~0x1;
        entry.contextPLRU &= ~0x4;
        break;
      default:
        panic("Invalid BOP PC validation offset context way\n");
    }
}

bool
BOP::PCValidationConfidenceTable::sample(
    Addr pc, PCValidationKind kind, int64_t offset, Addr line,
    unsigned int period,
    Addr salt) const
{
    assert(isPowerOf2(period));

    Addr sig = signature(pc, kind) ^ line ^ salt;
    sig ^= static_cast<Addr>(offset) * 0x9e3779b97f4a7c15ULL;
    sig ^= sig >> 9;
    sig ^= sig >> 17;
    sig ^= sig >> 29;
    return (sig & (period - 1)) == 0;
}

BOP::PCValidationConfidenceTable::LookupResult
BOP::PCValidationConfidenceTable::lookup(
    Addr pc, PCValidationKind kind, int64_t offset)
{
    return lookup(keyForPC(pc, kind), kind, offset);
}

BOP::PCValidationKey
BOP::PCValidationConfidenceTable::keyForPC(
    Addr pc, PCValidationKind kind) const
{
    const Addr sig = signature(pc, kind);
    PCValidationKey key;
    key.valid = true;
    key.set = sig & (sets - 1);
    key.tag = (sig >> setBits) & tagMask;
    return key;
}

BOP::PCValidationConfidenceTable::LookupResult
BOP::PCValidationConfidenceTable::lookup(
    const PCValidationKey &key, PCValidationKind kind, int64_t offset)
{
    assert(key.valid);
    assert(key.set < sets);
    const unsigned int set = key.set;
    const Addr tag = key.tag;
    LookupResult result;
    result.set = set;
    result.tag = tag;
    result.kind = kind;
    result.offset = offset;

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
        entry = Entry();
        entry.valid = true;
        entry.tag = tag;
    }
    touchPLRU(set, way);

    unsigned int context_way = offsetContextSlots;
    for (unsigned int candidate = 0; candidate < offsetContextSlots;
         candidate++) {
        const OffsetContext &context = entry.contexts[candidate];
        if (context.valid && context.offset == offset) {
            context_way = candidate;
            result.contextHit = true;
            break;
        }
    }
    if (context_way == offsetContextSlots) {
        for (unsigned int candidate = 0; candidate < offsetContextSlots;
             candidate++) {
            if (!entry.contexts[candidate].valid) {
                context_way = candidate;
                break;
            }
        }
        if (context_way == offsetContextSlots) {
            context_way = contextVictim(entry);
        }
    }

    OffsetContext &context = entry.contexts[context_way];
    result.contextWay = context_way;
    result.contextReplaced = !result.contextHit && context.valid;
    if (!result.contextHit) {
        context.valid = true;
        context.offset = offset;
        context.confidence = initialConfidence;
        context.lowEntryMissStreak = 0;
    }
    touchContext(entry, context_way);

    result.confidence = context.confidence;
    result.lowEntryMissStreak = context.lowEntryMissStreak;
    if (context.confidence >= highThreshold) {
        result.state = PCConfidenceState::High;
    } else if (context.confidence >= mediumThreshold) {
        result.state = PCConfidenceState::Medium;
    } else {
        result.state = PCConfidenceState::Low;
    }
    return result;
}

bool
BOP::PCValidationConfidenceTable::sampleMediumIssue(
    Addr pc, PCValidationKind kind, int64_t offset, Addr line) const
{
    return sample(pc, kind, offset, line, mediumSamplePeriod, 0x9e37);
}

void
BOP::PCValidationConfidenceTable::resetGlobalBypassPolicy()
{
    globalOutcomeWindowResolved = 0;
    globalOutcomeWindowUnused = 0;
    globalIssuedWindowIssued = 0;
    globalUnusedEwma = GLOBAL_UNUSED_EWMA_INITIAL;
    globalChecksSinceOutcome = 0;
    globalBypassPCValidation = false;
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
    auto &updates = pending[pcValidationKindIndex(lookup.kind)];
    PendingUpdate *free_update = nullptr;
    for (PendingUpdate &candidate : updates) {
        if (!candidate.valid) {
            if (!free_update) {
                free_update = &candidate;
            }
            continue;
        }
        if (candidate.index == lookup.index && candidate.tag == lookup.tag &&
            candidate.contextWay == lookup.contextWay &&
            candidate.offset == lookup.offset && candidate.kind == lookup.kind) {
            candidate.validationHit = candidate.validationHit || validation_hit;
            candidate.participants++;
            return;
        }
    }

    if (!free_update) {
        panic("BOP PC validation exceeded per-demand update capacity\n");
    }

    PendingUpdate &update = *free_update;
    if (!update.valid) {
        update.valid = true;
        update.kind = lookup.kind;
        update.pc = pc;
        update.triggerLine = trigger_line;
        update.index = lookup.index;
        update.set = lookup.set;
        update.way = lookup.way;
        update.contextWay = lookup.contextWay;
        update.tag = lookup.tag;
        update.offset = lookup.offset;
    }
    update.validationHit = validation_hit;
    update.participants++;
}

BOP::PCValidationConfidenceTable::CommitResult
BOP::PCValidationConfidenceTable::commitOne(PendingUpdate &update)
{
    CommitResult result;
    result.kind = update.kind;
    if (!update.valid) {
        return result;
    }

    result.hadPending = true;
    result.hadValidation = update.participants != 0;
    result.validationHit = update.validationHit;
    result.pc = update.pc;
    result.triggerLine = update.triggerLine;
    result.index = update.index;
    result.set = update.set;
    result.way = update.way;
    result.contextWay = update.contextWay;
    result.tag = update.tag;
    result.offset = update.offset;
    result.participants = update.participants;
    if (result.hadValidation) {
        Entry &entry = table[update.index];
        assert(entry.valid && entry.tag == update.tag);
        OffsetContext &context = entry.contexts[update.contextWay];
        assert(context.valid && context.offset == update.offset);
        result.confidenceBefore = context.confidence;
        result.lowEntryMissStreakBefore = context.lowEntryMissStreak;

        if (update.validationHit) {
            context.confidence = std::min(
                counterMax, static_cast<unsigned int>(context.confidence) +
                                hitIncrement);
            context.lowEntryMissStreak = 0;
        } else if (sample(update.pc, update.kind, update.offset,
                          update.triggerLine, missDecayPeriod, 0x7f4a)) {
            if (lowEntryMissStreakThreshold != 0 &&
                context.confidence == mediumThreshold) {
                context.lowEntryMissStreak = std::min(
                    lowEntryMissStreakThreshold,
                    static_cast<unsigned int>(context.lowEntryMissStreak) + 1);
                if (context.lowEntryMissStreak ==
                    lowEntryMissStreakThreshold) {
                    context.confidence = context.confidence == 0
                        ? 0 : context.confidence - 1;
                    context.lowEntryMissStreak = 0;
                    result.decayed = true;
                    result.lowEntryHysteresisTransition = true;
                } else {
                    result.lowEntryHysteresisHeld = true;
                }
            } else {
                context.confidence = context.confidence == 0
                    ? 0 : context.confidence - 1;
                context.lowEntryMissStreak = 0;
                result.decayed = true;
            }
        }
        result.confidenceAfter = context.confidence;
        result.lowEntryMissStreakAfter = context.lowEntryMissStreak;
    }

    update = PendingUpdate();
    return result;
}

std::vector<BOP::PCValidationConfidenceTable::CommitResult>
BOP::PCValidationConfidenceTable::commit()
{
    std::vector<CommitResult> results;
    results.reserve(PC_VALIDATION_KIND_COUNT *
                    PC_VALIDATION_MAX_PENDING_UPDATES_PER_KIND);
    for (auto &updates : pending) {
        for (PendingUpdate &update : updates) {
            CommitResult result = commitOne(update);
            if (result.hadPending) {
                results.push_back(result);
            }
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
           offsetContextSlots == other.offsetContextSlots &&
           globalCoverageGuardEnabled == other.globalCoverageGuardEnabled &&
           globalUnusedThreshold == other.globalUnusedThreshold &&
           globalMinResolvedCoverageShift ==
               other.globalMinResolvedCoverageShift;
}

BOP::BOP(const BOPPrefetcherParams &p)
    : Queued(p),
      scoreMax(p.score_max), roundMax(p.round_max),
      badScore(p.bad_score), rrEntries(p.rr_size),
      tagMask((1 << p.tag_bits) - 1),
      delayQueueEnabled(p.delay_queue_enable),
      delayQueueSize(p.delay_queue_size),
      delayTicks(cyclesToTicks(p.delay_queue_cycles)),
      crossPage(p.crossPage),
      enableAdaptOffset(p.enable_adaptoffset),
      negativeOffsetsEnable(p.negative_offsets_enable),
      autoLearning(p.autoLearning),
      enableIssueValidation(p.enable_issue_validation),
      enablePCValidationConfidence(p.enable_pc_validation_confidence),
      enablePCValidationProducerConsumer(
          p.enable_pc_validation_producer_consumer),
      enableGlobalBOPCoverageGuard(p.enable_global_bop_coverage_guard),
      enableDirectQualityGate(p.enable_direct_quality_gate),
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
      pcValidationOffsetContextSlots(
          p.pc_validation_offset_context_slots),
      globalBOPUnusedThreshold(p.global_bop_unused_threshold),
      globalBOPMinResolvedCoverageShift(
          p.global_bop_min_resolved_coverage_shift),
      victimListSize(p.victimOffsetsListSize),
      restoreCycle(p.restoreCycle),
      enableStudentCover(p.enable_student_cover),
      studentPoolSize(p.student_pool_size),
      studentConfAlpha(p.student_conf_alpha),
      studentCovThreshold(p.student_cov_threshold),
      studentTeacherTopN(p.student_teacher_top_n),
      studentFilterEntries(p.student_filter_entries),
      studentHashCount(p.student_hash_count),
      studentHashMode(p.student_hash_mode),
      studentLargeOffsetPriorityEnable(
          p.student_large_offset_priority_enable),
      studentLargeOffsetPriorityCoeff(
          p.student_large_offset_priority_coeff),
      studentDelayQueueEnabled(p.student_delay_queue_enable),
      studentDelayQueueSize(p.student_delay_queue_size),
      studentDelayTicks(cyclesToTicks(p.student_delay_queue_cycles)),
      delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()),
      issuePrefetchRequests(false), bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0),
      stats(this, p.student_pool_size, p.student_delay_queue_size)
{
    pcValidationGenericName = name();
    pcValidationLargeName = name();
    pcValidationSmallName = name();

    if (enableDirectQualityGate) {
        DirectQualityGate::Config config;
        if (p.direct_quality_profile == "bop-cqf14e6t30") {
            // This profile owns all semantic parameters. In particular, do
            // not inherit PC-validation tag bits or feedback capacity knobs.
            config = DirectQualityGate::Config::bopCqf14E6T30();
        } else if (p.direct_quality_profile == "bop-cqf-dse" ||
                   p.direct_quality_profile == "legacy") {
            if (p.direct_quality_profile == "bop-cqf-dse") {
                config = DirectQualityGate::Config::bopCqfDse();
                // BOP-CQF-DSE keeps tag8/tag14 and all CQF data-path
                // semantics fixed. The solver may change only these capacity,
                // policy, and compact-age fields before instantiation.
                config.qualityTagBits = 8;
            } else {
                config.qualityTagBits = p.pc_validation_tag_bits;
            }
            config.qualityEntries = p.direct_quality_entries;
            config.qualityWays = p.direct_quality_ways;
            config.feedbackEntries = p.direct_quality_feedback_entries;
            config.feedbackWays = p.direct_quality_feedback_ways;
            config.horizon = p.direct_quality_horizon;
            config.minSamples = p.direct_quality_min_samples;
            config.observeSamplePeriod = p.direct_quality_observe_sample_period;
            config.openSamplePeriod = p.direct_quality_open_sample_period;
            config.blockProbePeriod = p.direct_quality_block_probe_period;
            config.borderlineBlockProbePeriod =
                p.direct_quality_borderline_block_probe_period;
            config.unusedPerUseful = p.direct_quality_unused_per_useful;
            config.blockGuard = p.direct_quality_block_guard;
            config.strictUnusedPerUseful =
                p.direct_quality_strict_unused_per_useful;
            config.strictBlockGuard = p.direct_quality_strict_block_guard;
            config.reopenUnusedPerUseful = p.direct_quality_reopen_unused_per_useful;
            config.reopenGuard = p.direct_quality_reopen_guard;
            config.reopenProbePeriod = p.direct_quality_reopen_probe_period;
            config.reopenConfirmSamples =
                p.direct_quality_reopen_confirm_samples;
            config.decayPeriod = p.direct_quality_decay_period;
            if (p.direct_quality_profile == "bop-cqf-dse") {
                config.compactEpochBits = p.direct_quality_epoch_bits;
                config.compactEpochShift = p.direct_quality_epoch_shift;
                config.compactEpochTimeout = p.direct_quality_epoch_timeout;
            }
        } else {
            fatal("%s: unsupported direct-quality profile %s\n", name(),
                  p.direct_quality_profile);
        }
        directQualityGate = std::make_shared<DirectQualityGate>(config);
    }

    if (!isPowerOf2(rrEntries)) {
        fatal("%s: number of RR entries is not power of 2\n", name());
    }
    if (!isPowerOf2(blkSize)) {
        fatal("%s: cache line size is not power of 2\n", name());
    }
    fatal_if(enableStudentCover && studentPoolSize == 0,
             "%s: student_pool_size must be non-zero when student coverage is enabled",
             name());
    fatal_if(enableStudentCover && studentPoolSize > 64,
             "%s: student_pool_size=%u exceeds the 64-bit coverage mask",
             name(), studentPoolSize);
    fatal_if(enableStudentCover && !isPowerOf2(studentFilterEntries),
             "%s: student_filter_entries must be a power of two", name());
    fatal_if(enableStudentCover && studentHashCount == 0,
             "%s: student_hash_count must be non-zero when student coverage is enabled",
             name());
    fatal_if(enableStudentCover &&
                 (studentHashMode != "lowbits") &&
                 (studentHashMode != "bop_rr") &&
                 (studentHashMode != "splitmix"),
             "%s: unsupported bounded student_hash_mode '%s'", name(),
             studentHashMode);
    fatal_if(enableStudentCover &&
                 (studentConfAlpha < 0.0 || studentConfAlpha > 1.0),
             "%s: student_conf_alpha must be in [0, 1]", name());
    fatal_if(enableStudentCover &&
                 (studentCovThreshold < 0.0 || studentCovThreshold > 1.0),
             "%s: student_cov_threshold must be in [0, 1]", name());
    fatal_if(enableStudentCover && studentDelayQueueEnabled &&
                 studentDelayQueueSize == 0,
             "%s: student_delay_queue_size must be non-zero when delay is enabled",
             name());
    if (enableIssueValidation && enablePCValidationConfidence) {
        fatal("%s: strict and PC-confidence BOP validation are mutually exclusive\n",
              name());
    }
    if (enableGlobalBOPCoverageGuard && !enablePCValidationConfidence) {
        fatal("%s: global BOP coverage guard requires PC validation confidence\n",
              name());
    }
    if (enablePCValidationProducerConsumer &&
        !enablePCValidationConfidence) {
        fatal("%s: BOP producer/consumer validation requires PC confidence\n",
              name());
    }
    if (enablePCValidationConfidence) {
        pcValidationTable = std::make_shared<PCValidationConfidenceTable>(
            pcValidationEntries, pcValidationTagBits, pcValidationCounterBits,
            pcValidationInitial, pcValidationMediumThreshold,
            pcValidationHighThreshold, pcValidationHitIncrement,
            pcValidationMediumSamplePeriod, pcValidationMissDecayPeriod,
            pcValidationLowEntryMissStreakThreshold,
            pcValidationOffsetContextSlots, enableGlobalBOPCoverageGuard,
            globalBOPUnusedThreshold, globalBOPMinResolvedCoverageShift);
    }

    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);
    if (enableStudentCover) {
        studentPool.reserve(studentPoolSize);
        studentFilterBits.assign(studentFilterEntries, 0);
    }

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
        const auto &entry = delayQueue.front();
        const uint64_t replay_order =
            (archDBer && archDBer->dumpBopReplayTrace && archDBer->dumpGlobal)
            ? ++replayOrder : 0;
        writeBOPReplayDelayAction("dequeue_to_rr", replay_order,
                                  entry.rrEntry.fullAddr, entry.processTick,
                                  delayQueue.size() - 1);
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

void
BOP::writeBOPReplayMeta()
{
    if (replayMetaWritten || !archDBer || !archDBer->dumpBopReplayTrace ||
        !archDBer->dumpGlobal) {
        return;
    }

    std::ostringstream offsets;
    for (size_t index = 0; index < originOffsets.size(); ++index) {
        if (index != 0) {
            offsets << ',';
        }
        offsets << originOffsets[index];
    }

    archDBer->bopReplayMetaTraceWrite(
        name().c_str(), blkSize, scoreMax, roundMax, badScore, rrEntries,
        floorLog2(tagMask + 1), delayQueueEnabled, delayQueueSize, delayTicks,
        crossPage, enableAdaptOffset, enableIssueValidation,
        enablePCValidationConfidence, enablePCValidationProducerConsumer,
        enableGlobalBOPCoverageGuard,
        pcValidationEntries, pcValidationTagBits, pcValidationCounterBits,
        pcValidationInitial, pcValidationMediumThreshold,
        pcValidationHighThreshold, pcValidationHitIncrement,
        pcValidationMediumSamplePeriod, pcValidationMissDecayPeriod,
        pcValidationLowEntryMissStreakThreshold, pcValidationEpochBits,
        pcValidationOffsetContextSlots,
        globalBOPUnusedThreshold, globalBOPMinResolvedCoverageShift,
        negativeOffsetsEnable, autoLearning, victimListSize, restoreCycle,
        clockPeriod(), enableStudentCover, studentPoolSize,
        studentConfAlpha, studentCovThreshold, studentTeacherTopN,
        studentFilterEntries, studentHashMode, studentHashCount,
        studentLargeOffsetPriorityEnable, studentLargeOffsetPriorityCoeff,
        studentDelayQueueEnabled, studentDelayQueueSize, studentDelayTicks,
        offsets.str());
    replayMetaWritten = true;
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
    insertIntoRR(full_addr, tag, PCValidationKey(), way);
}

void
BOP::insertIntoRR(Addr full_addr, Addr tag, PCValidationKey owner_key,
                  unsigned int way)
{
    insertIntoRR(RREntryDebug(full_addr, tag, owner_key), way);
}

void
BOP::insertIntoRR(RREntryDebug rr_entry, unsigned int way)
{
    switch (way) {
        case RRWay::Left:
            rrLeft[hash(rr_entry.fullAddr, RRWay::Left)] = rr_entry;
            break;
        case RRWay::Right:
            rrRight[hash(rr_entry.fullAddr, RRWay::Right)] = rr_entry;
            break;
    }
}

void
BOP::writeBOPReplayDelayAction(const char *action, uint64_t replay_order,
                                Addr addr, Tick process_tick,
                                unsigned int queue_size_after)
{
    if (replay_order == 0 || !archDBer || !archDBer->dumpBopReplayTrace ||
        !archDBer->dumpGlobal) {
        return;
    }
    archDBer->bopReplayDelayActionTraceWrite(
        name().c_str(), replay_order, action, curTick(), addr, process_tick,
        queue_size_after);
}

void
BOP::insertIntoDelayQueue(Addr full_addr, Addr tag,
                           PCValidationKey owner_key,
                           uint64_t replay_order)
{
    if (delayQueue.size() == delayQueueSize) {
        writeBOPReplayDelayAction("drop_full", replay_order, full_addr,
                                  curTick() + delayTicks, delayQueue.size());
        return;
    }

    // Add the address to the delay queue and schedule an event to process
    // it after the specified delay cycles
    Tick process_tick = curTick() + delayTicks;

    delayQueue.push_back(
        DelayQueueEntry({full_addr, tag, owner_key}, process_tick));
    writeBOPReplayDelayAction("enqueue", replay_order, full_addr, process_tick,
                              delayQueue.size());

    if (!delayQueueEvent.scheduled()) {
        schedule(delayQueueEvent, process_tick);
    }
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

std::vector<unsigned int>
BOP::studentHashIndexes(Addr line_addr) const
{
    std::vector<unsigned int> indexes;
    if (!enableStudentCover || studentFilterEntries == 0) {
        return indexes;
    }

    indexes.reserve(studentHashCount);
    const uint64_t mask = static_cast<uint64_t>(studentFilterEntries - 1);
    uint64_t base1 = 0;
    uint64_t base2 = 1;
    if (studentHashMode == "lowbits") {
        base1 = line_addr;
        base2 = ((line_addr >> 6) ^ (line_addr >> 12) ^ 0x9E37ULL) | 1ULL;
    } else if (studentHashMode == "bop_rr") {
        const unsigned int lgm = floorLog2(studentFilterEntries);
        base1 = ((line_addr & mask) ^ ((line_addr >> lgm) & mask)) & mask;
        base2 = ((((line_addr >> (2 * lgm)) & mask) ^ line_addr ^
                  0xC2B2ULL) | 1ULL);
    } else {
        base1 = splitmix64(line_addr);
        base2 = splitmix64(line_addr ^ 0x9E3779B97F4A7C15ULL) | 1ULL;
    }

    for (unsigned int i = 0; i < studentHashCount; ++i) {
        indexes.push_back(static_cast<unsigned int>((base1 + i * base2) &
                                                    mask));
    }
    return indexes;
}

bool
BOP::studentPoolAllSameSign() const
{
    if (studentPool.empty()) {
        return false;
    }
    const bool positive = studentPool.front().offset > 0;
    return std::all_of(studentPool.begin(), studentPool.end(),
        [positive](const StudentOffsetEntry &entry) {
            return positive ? entry.offset > 0 : entry.offset < 0;
        });
}

bool
BOP::studentIntermediateOffsetsMatchSlope(size_t best_idx, size_t worst_idx,
                                          uint32_t best_cov,
                                          uint32_t worst_cov) const
{
    if (studentPool.size() <= 2) {
        return true;
    }

    const auto best_abs = std::llabs(studentPool[best_idx].offset);
    const auto worst_abs = std::llabs(studentPool[worst_idx].offset);
    const double ref_dist = static_cast<double>(worst_abs - best_abs);
    if (ref_dist <= 0.0) {
        return false;
    }
    const double ref_cov_gap =
        static_cast<double>(best_cov) - static_cast<double>(worst_cov);
    for (size_t index = 0; index < studentPool.size(); ++index) {
        if (index == best_idx || index == worst_idx) {
            continue;
        }
        const auto current_abs = std::llabs(studentPool[index].offset);
        if (current_abs <= best_abs || current_abs >= worst_abs) {
            return false;
        }
        const uint32_t current_cov = studentPool[index].curPhaseCov;
        if (current_cov > best_cov || current_cov < worst_cov) {
            return false;
        }
        const double current_dist =
            static_cast<double>(current_abs - best_abs);
        const double current_cov_gap =
            static_cast<double>(best_cov) - static_cast<double>(current_cov);
        if (ref_cov_gap == 0.0) {
            if (current_cov_gap != 0.0) {
                return false;
            }
            continue;
        }
        const double lhs = current_cov_gap * ref_dist;
        const double rhs = ref_cov_gap * current_dist;
        if (lhs < studentLargeOffsetPriorityCoeff * rhs ||
            studentLargeOffsetPriorityCoeff * lhs > rhs) {
            return false;
        }
    }
    return true;
}

bool
BOP::studentShouldPreferLargeOffset(size_t best_idx, size_t worst_idx,
                                    uint32_t best_cov,
                                    uint32_t worst_cov) const
{
    if (studentPool.size() < 2 || studentPhaseTrainCount == 0 ||
        !studentPoolAllSameSign()) {
        return false;
    }
    const auto abs_less = [](const StudentOffsetEntry &lhs,
                             const StudentOffsetEntry &rhs) {
        const auto lhs_abs = std::llabs(lhs.offset);
        const auto rhs_abs = std::llabs(rhs.offset);
        return lhs_abs == rhs_abs ? lhs.offset < rhs.offset :
                                    lhs_abs < rhs_abs;
    };
    const auto minmax = std::minmax_element(studentPool.begin(),
                                            studentPool.end(), abs_less);
    const auto best_abs = std::llabs(studentPool[best_idx].offset);
    const auto worst_abs = std::llabs(studentPool[worst_idx].offset);
    if (best_abs != std::llabs(minmax.first->offset) ||
        worst_abs != std::llabs(minmax.second->offset)) {
        return false;
    }
    const double lhs = studentLargeOffsetPriorityCoeff *
        (static_cast<double>(worst_cov) - static_cast<double>(best_cov));
    const double rhs = static_cast<double>(worst_abs - best_abs) /
        studentPhaseTrainCount;
    return lhs <= rhs && studentIntermediateOffsetsMatchSlope(
        best_idx, worst_idx, best_cov, worst_cov);
}

void
BOP::studentInsertFilterMask(const std::vector<unsigned int> &indexes,
                             uint64_t mask)
{
    for (const auto index : indexes) {
        studentFilterBits[index] |= mask;
    }
}

void
BOP::studentDrainDelayQueue(Tick now)
{
    if (!studentDelayQueueEnabled) {
        return;
    }
    while (!studentDelayQueue.empty() &&
           studentDelayQueue.front().readyTick <= now) {
        const auto &entry = studentDelayQueue.front();
        studentInsertFilterMask(entry.filterIndexes, entry.mask);
        stats.studentDelayQueueDrainCount++;
        studentDelayQueue.pop_front();
    }
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
}

void
BOP::studentEnqueuePrediction(Addr train_addr, size_t bit_idx,
                              int64_t offset)
{
    const int64_t predicted = static_cast<int64_t>(train_addr) +
        offset * static_cast<int64_t>(blkSize);
    if (predicted < 0) {
        return;
    }
    const Addr predicted_addr = static_cast<Addr>(predicted);
    if (!crossPage && !samePage(train_addr, predicted_addr)) {
        return;
    }
    const auto indexes = studentHashIndexes(predicted_addr >> lBlkSize);
    if (indexes.empty()) {
        return;
    }
    const uint64_t mask = 1ULL << bit_idx;
    if (!studentDelayQueueEnabled) {
        studentInsertFilterMask(indexes, mask);
        return;
    }
    if (studentDelayQueue.size() >= studentDelayQueueSize) {
        stats.studentDelayQueueFullDropCount++;
        return;
    }
    studentDelayQueue.emplace_back(curTick() + studentDelayTicks, indexes,
                                   mask);
    stats.studentDelayQueueInsertCount++;
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
}

size_t
BOP::studentPickBestIndex() const
{
    assert(!studentPool.empty());
    size_t best = 0;
    auto best_key = std::make_tuple(studentPool[0].curPhaseCov,
                                    studentPool[0].conf,
                                    -std::llabs(studentPool[0].offset),
                                    -studentPool[0].offset);
    for (size_t index = 1; index < studentPool.size(); ++index) {
        const auto key = std::make_tuple(studentPool[index].curPhaseCov,
                                         studentPool[index].conf,
                                         -std::llabs(studentPool[index].offset),
                                         -studentPool[index].offset);
        if (key > best_key) {
            best = index;
            best_key = key;
        }
    }
    return best;
}

size_t
BOP::studentPickWorstIndex() const
{
    assert(!studentPool.empty());
    size_t worst = 0;
    auto worst_key = std::make_tuple(studentPool[0].curPhaseCov,
                                     studentPool[0].conf,
                                     std::llabs(studentPool[0].offset),
                                     studentPool[0].offset);
    for (size_t index = 1; index < studentPool.size(); ++index) {
        const auto key = std::make_tuple(studentPool[index].curPhaseCov,
                                         studentPool[index].conf,
                                         std::llabs(studentPool[index].offset),
                                         studentPool[index].offset);
        if (key < worst_key) {
            worst = index;
            worst_key = key;
        }
    }
    return worst;
}

size_t
BOP::studentPickEvictIndex() const
{
    assert(!studentPool.empty());
    size_t victim = 0;
    auto victim_key = std::make_tuple(studentPool[0].conf,
                                      studentPool[0].lastPhaseCov,
                                      -std::llabs(studentPool[0].offset),
                                      studentPool[0].offset);
    for (size_t index = 1; index < studentPool.size(); ++index) {
        const auto key = std::make_tuple(studentPool[index].conf,
                                         studentPool[index].lastPhaseCov,
                                         -std::llabs(studentPool[index].offset),
                                         studentPool[index].offset);
        if (key < victim_key) {
            victim = index;
            victim_key = key;
        }
    }
    return victim;
}

void
BOP::studentObserveTrainAddr(Addr addr)
{
    if (!enableStudentCover) {
        return;
    }
    studentPhaseTrainCount++;
    if (studentPool.empty()) {
        return;
    }
    studentDrainDelayQueue(curTick());
    uint64_t hit_mask = ~0ULL;
    for (const auto index : studentHashIndexes(addr >> lBlkSize)) {
        hit_mask &= studentFilterBits[index];
    }
    for (size_t index = 0; index < studentPool.size(); ++index) {
        if (hit_mask & (1ULL << index)) {
            studentPool[index].curPhaseCov++;
        }
        studentEnqueuePrediction(addr, index, studentPool[index].offset);
    }
}

bool
BOP::studentInsertTeacherBest(int64_t offset)
{
    if (!enableStudentCover || offset == 0 || studentTeacherTopN == 0 ||
        std::any_of(studentPool.begin(), studentPool.end(),
            [offset](const StudentOffsetEntry &entry) {
                return entry.offset == offset;
            })) {
        return false;
    }
    if (studentPool.size() >= studentPoolSize) {
        studentPool.erase(studentPool.begin() + studentPickEvictIndex());
    }
    studentPool.emplace_back(offset);
    stats.teacherInjectedCount++;
    stats.teacherInjectedOffsetDist.sample(offset);
    return true;
}

void
BOP::studentClearPhaseState()
{
    if (!enableStudentCover) {
        return;
    }
    std::fill(studentFilterBits.begin(), studentFilterBits.end(), 0);
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
    studentDelayQueue.clear();
    studentPhaseTrainCount = 0;
    for (auto &entry : studentPool) {
        entry.curPhaseCov = 0;
    }
}

bool
BOP::studentShouldIssue() const
{
    return enableStudentCover && studentSelectedValid &&
           studentSelectedEnable;
}

int64_t
BOP::studentSelectIssueOffset(int64_t teacher_best_offset) const
{
    return studentShouldIssue() ? studentSelectedOffset : teacher_best_offset;
}

void
BOP::studentOnTeacherPhaseEnd(int64_t teacher_best_offset)
{
    if (!enableStudentCover) {
        return;
    }
    stats.studentPhaseCount++;
    stats.studentPoolOccupancyDist.sample(studentPool.size());
    if (!studentPool.empty()) {
        const size_t best = studentPickBestIndex();
        const size_t worst = studentPickWorstIndex();
        const uint32_t best_cov = studentPool[best].curPhaseCov;
        const uint32_t worst_cov = studentPool[worst].curPhaseCov;
        const bool prefer_large = studentLargeOffsetPriorityEnable &&
            studentShouldPreferLargeOffset(best, worst, best_cov, worst_cov);
        const size_t selected = prefer_large ? worst : best;
        const uint32_t selected_cov = prefer_large ? worst_cov : best_cov;
        const size_t reward = prefer_large ? worst : best;
        const size_t punish = prefer_large ? best : worst;
        const double coverage = studentPhaseTrainCount == 0 ? 0.0 :
            static_cast<double>(selected_cov) / studentPhaseTrainCount;
        const double other_coverage = studentPhaseTrainCount == 0 ? 0.0 :
            static_cast<double>(prefer_large ? best_cov : worst_cov) /
            studentPhaseTrainCount;
        studentSelectedOffset = studentPool[selected].offset;
        studentSelectedValid = true;
        studentSelectedEnable = coverage >= studentCovThreshold;
        stats.studentCovRatioPctDist.sample(
            static_cast<uint64_t>(std::round(coverage * 100.0)));
        stats.studentWorstCovRatioPctDist.sample(
            static_cast<uint64_t>(std::round(other_coverage * 100.0)));
        if (studentSelectedEnable) {
            stats.studentIssueCount++;
            stats.studentSelectedOffsetDist.sample(studentSelectedOffset);
        } else {
            stats.studentFallbackCount++;
        }
        if (prefer_large) {
            stats.studentLargeOffsetPriorityCount++;
        }
        for (size_t index = 0; index < studentPool.size(); ++index) {
            double update = 0.0;
            if (index == reward) {
                update = 1.0;
            } else if (index == punish) {
                update = -1.0;
            }
            studentPool[index].conf = studentPool[index].conf *
                studentConfAlpha + update * (1.0 - studentConfAlpha);
            studentPool[index].lastPhaseCov = studentPool[index].curPhaseCov;
        }
    } else {
        studentSelectedValid = false;
        studentSelectedEnable = false;
        stats.studentFallbackCount++;
    }
    if (teacher_best_offset != 0 && studentTeacherTopN != 0) {
        studentInsertTeacherBest(teacher_best_offset);
    }
    studentClearPhaseState();
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
        std::vector<AddrPriority> &addresses, bool late,
        uint64_t replay_event_id)
{
    Addr addr = blockAddress(pfi.getAddr());
    Addr tag_x = tag(addr);
    const Addr trigger_pc = pfi.hasPC() ? pfi.getPC() : 0;
    const bool trigger_is_demand =
        pfi.trigger_info.pkt && pfi.trigger_info.pkt->isDemand();
    const bool trigger_is_read =
        pfi.trigger_info.pkt && pfi.trigger_info.pkt->isRead() &&
        !pfi.trigger_info.pkt->isWrite();
    const int trigger_pf_source =
        static_cast<int>(pfi.getXsMetadata().prefetchSource);
    const bool replay_tracing = replay_event_id != 0 && archDBer &&
        archDBer->dumpBopReplayTrace && archDBer->dumpGlobal;
    const uint64_t replay_order = replay_tracing ? ++replayOrder : 0;

    if (replay_tracing) {
        writeBOPReplayMeta();
    }

    DPRINTF(BOPPrefetcher,
            "Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

    PCValidationKey rr_owner;
    if (enablePCValidationProducerConsumer && pfi.hasPC() &&
        trigger_is_demand && trigger_is_read) {
        rr_owner = pcValidationTable->keyForPC(
            trigger_pc, pcValidationKind);
    }
    if (delayQueueEnabled) {
        insertIntoDelayQueue(addr, tag_x, rr_owner, replay_order);
    } else {
        insertIntoRR(addr, tag_x, rr_owner, RRWay::Left);
    }

    // Go through the nth offset and update the score, the best score and the
    // current best offset if a better one is found.  The student observes the
    // same demand stream and advances only at a completed teacher phase.
    const int64_t previous_best_offset = bestOffset;
    const bool teacher_phase_end = bestOffsetLearning(addr, late, pfi);
    studentObserveTrainAddr(addr);
    if (teacher_phase_end) {
        studentOnTeacherPhaseEnd(bestOffset);
    }
    const bool best_offset_changed = bestOffset != previous_best_offset;

    const bool teacher_issue = issuePrefetchRequests;
    const bool student_issue = studentShouldIssue();
    const int64_t selected_offset =
        studentSelectIssueOffset(bestOffset);
    const bool algorithm_issue =
        (teacher_issue || student_issue) && selected_offset != 0;
    const Addr validation_addr = selected_offset != 0
        ? addr - (static_cast<Addr>(selected_offset) << lBlkSize) : 0;
    const Addr prefetch_addr = selected_offset != 0
        ? addr + (selected_offset * (1ULL << lBlkSize)) : 0;
    bool issue_prefetch = algorithm_issue;
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

    if (issue_prefetch && enableIssueValidation) {
        assert(selected_offset != 0);
        validation_hit = testRR(validation_addr).first;

        stats.issueValidationChecks++;
        if (validation_hit) {
            stats.issueValidationHits++;
        } else {
            stats.issueValidationSuppressed++;
            issue_prefetch = false;
        }
        DPRINTF(BOPPrefetcher, "Issue validation addr %#lx best offset %lld: %s\n", validation_addr,
                static_cast<long long>(selected_offset), validation_hit ? "hit" : "miss");
    } else if (issue_prefetch && enablePCValidationConfidence) {
        assert(selected_offset != 0);
        const auto [rr_hit, rr_entry] = testRR(validation_addr);
        validation_hit = rr_hit;
        stats.issueValidationChecks++;
        if (validation_hit) {
            stats.issueValidationHits++;
        }

        const auto account_lookup = [&](const auto &lookup) {
            stats.pcValidationTableLookups++;
            if (lookup.entryHit) {
                stats.pcValidationTableHits++;
            } else {
                stats.pcValidationTableMisses++;
            }
            if (lookup.replaced) {
                stats.pcValidationTableReplacements++;
            }
            if (lookup.contextHit) {
                stats.pcValidationOffsetContextHits++;
            } else {
                stats.pcValidationOffsetContextMisses++;
            }
            if (lookup.contextReplaced) {
                stats.pcValidationOffsetContextReplacements++;
            }
        };
        const auto set_current_lookup = [&](const auto &lookup) {
            pc_entry_hit = lookup.entryHit;
            pc_confidence = lookup.confidence;
            pc_low_entry_miss_streak = lookup.lowEntryMissStreak;
            pc_state = static_cast<int>(lookup.state);
            pc_index = lookup.index;
            pc_tag = lookup.tag;
            account_lookup(lookup);
            stats.pcValidationConfidenceDist.sample(lookup.confidence);
        };
        const auto apply_consumer_admission = [&](const auto &lookup) {
            if (pcValidationTable->notePCValidationMiss()) {
                stats.globalBOPBypassModeIdleResets++;
            }
            bypass_mode = pcValidationTable->bypassPCValidationActive();
            if (bypass_mode) {
                stats.globalBOPBypassModeChecks++;
                stats.globalBOPBypassModeIssued++;
                switch (lookup.state) {
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
                }
            } else {
                switch (lookup.state) {
                  case PCConfidenceState::High:
                    stats.pcValidationHighMissIssued++;
                    break;
                  case PCConfidenceState::Medium:
                    pc_sampled = pcValidationTable->sampleMediumIssue(
                        trigger_pc, pcValidationKind, selected_offset,
                        addr >> lBlkSize);
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
        };

        if (enablePCValidationProducerConsumer && validation_hit) {
            if (!rr_entry.owner.valid) {
                // Preserve the raw RR-hit admission, but never attribute a
                // value-initialized/tag-alias entry to the current PC.
                stats.rrOwnerInvalidHits++;
            } else {
                stats.rrOwnerValidHits++;
                if (pfi.hasPC()) {
                    const PCValidationKey current_key =
                        pcValidationTable->keyForPC(
                            trigger_pc, pcValidationKind);
                    if (rr_entry.owner == current_key) {
                        stats.rrOwnerSamePCHits++;
                        const auto current_lookup = pcValidationTable->lookup(
                            current_key, pcValidationKind, selected_offset);
                        set_current_lookup(current_lookup);
                        pcValidationTable->submitValidation(
                            current_lookup, trigger_pc, addr >> lBlkSize,
                            true);
                        stats.pcValidationProducerHitUpdates++;
                    } else {
                        stats.rrOwnerCrossPCHits++;
                        const auto current_lookup = pcValidationTable->lookup(
                            current_key, pcValidationKind, selected_offset);
                        set_current_lookup(current_lookup);
                        apply_consumer_admission(current_lookup);
                        pcValidationTable->submitValidation(
                            current_lookup, trigger_pc, addr >> lBlkSize,
                            false);
                        stats.pcValidationConsumerMissUpdates++;

                        const auto owner_lookup = pcValidationTable->lookup(
                            rr_entry.owner, pcValidationKind, selected_offset);
                        account_lookup(owner_lookup);
                        pcValidationTable->submitValidation(
                            owner_lookup, 0, addr >> lBlkSize, true);
                        stats.pcValidationProducerHitUpdates++;
                    }
                } else {
                    const auto owner_lookup = pcValidationTable->lookup(
                        rr_entry.owner, pcValidationKind, selected_offset);
                    account_lookup(owner_lookup);
                    pcValidationTable->submitValidation(
                        owner_lookup, 0, addr >> lBlkSize, true);
                    stats.pcValidationProducerHitUpdates++;
                }
            }
        } else if (pfi.hasPC()) {
            const auto pc_lookup =
                pcValidationTable->lookup(
                    trigger_pc, pcValidationKind, selected_offset);
            set_current_lookup(pc_lookup);

            if (!validation_hit) {
                apply_consumer_admission(pc_lookup);
            }
            pcValidationTable->submitValidation(
                pc_lookup, trigger_pc, addr >> lBlkSize, validation_hit);
            if (!validation_hit && enablePCValidationProducerConsumer) {
                stats.pcValidationConsumerMissUpdates++;
            }
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
                validation_addr, static_cast<long long>(selected_offset),
                validation_hit ? "hit" : "miss", pc_state, pc_confidence,
                issue_prefetch, bypass_mode);
    }

    if (issue_prefetch && selected_offset != 0 && pfi.hasPC() &&
        enableDirectQualityGate && directQualityGate) {
        const auto direct_quality_decision = directQualityGate->admit(
            trigger_pc, static_cast<uint8_t>(pcValidationKind),
            blockAddress(addr), blockAddress(prefetch_addr));
        if (!direct_quality_decision.allowed)
            issue_prefetch = false;
        updateDirectQualityStats();
    }

    // This prefetcher is a degree 1 prefetch, so it will only generate one
    // prefetch at most per access.
    bool generated = false;
    bool buffered = false;
    bool filtered = false;
    bool filter_passed = false;
    const bool raw_candidate_valid = algorithm_issue;
    const bool policy_candidate_valid = issue_prefetch && selected_offset != 0;
    const bool policy_suppressed = raw_candidate_valid && !policy_candidate_valid;

    if (issue_prefetch) {
        generated = true;
        buffered = samePage(pfi.getAddr(), prefetch_addr) || crossPage;
        stats.issuedOffsetDist.sample(selected_offset);
        filter_passed = sendPFWithFilter(
            pfi, prefetch_addr, addresses, 32, PrefetchSourceType::HWP_BOP);
        if (filter_passed && enableGlobalBOPCoverageGuard) {
            stats.globalBOPIssued++;
            pcValidationTable->noteGlobalBOPIssued();
        }
        filtered = !filter_passed;
        DPRINTF(BOPPrefetcher,
                "Generated prefetch %#lx offset: %lld teacher %d student %d\n",
                prefetch_addr, static_cast<long long>(selected_offset),
                teacher_issue, student_issue);
    } else if (!algorithm_issue) {
        stats.throttledCount++;
        DPRINTF(BOPPrefetcher, "Issue prefetch is false, can't issue\n");
    }

    if (replay_event_id != 0 && archDBer) {
        archDBer->bopReplayEventTraceWrite(
            replay_event_id, replay_order, curTick(), name().c_str(),
            pcValidationKindName(pcValidationKind), addr, trigger_pc,
            pfi.hasPC(), trigger_is_demand, trigger_is_read, pfi.isCacheMiss(),
            trigger_pf_source, pfi.isPfFirstHit(), pfi.isPfHit(), late,
            previous_best_offset, bestOffset, teacher_issue, student_issue,
            studentSelectedValid, studentSelectedEnable,
            studentSelectedOffset, selected_offset, bestScore, round,
            best_offset_changed, algorithm_issue, validation_enabled,
            validation_hit, enablePCValidationConfidence, pc_index, pc_tag,
            pc_entry_hit, pc_confidence, pc_state, pc_sampled,
            pc_low_entry_miss_streak, pc_epoch, bypass_mode, policy_suppressed,
            raw_candidate_valid,
            raw_candidate_valid ? prefetch_addr : 0, policy_candidate_valid,
            policy_candidate_valid ? prefetch_addr : 0, validation_addr,
            prefetch_addr, generated, buffered, filtered, filter_passed);
    }

    if (archDBer) {
        archDBer->bopValidationTraceWrite(
            curTick(), "candidate", name().c_str(), trigger_pc, addr,
            validation_addr, prefetch_addr, selected_offset, bestScore, round, late,
            trigger_is_demand, pfi.isCacheMiss(), trigger_pf_source,
            pfi.isPfFirstHit(), pfi.isPfHit(), algorithm_issue,
            validation_enabled, validation_hit,
            algorithm_issue && validation_enabled && !issue_prefetch,
            generated, buffered, filtered, filter_passed,
            enablePCValidationConfidence, pc_index, pc_tag, pc_entry_hit,
            pc_confidence, pc_state, pc_sampled, pc_epoch,
            pc_low_entry_miss_streak);
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
    if (enablePCValidationProducerConsumer !=
        other.enablePCValidationProducerConsumer) {
        fatal("%s and %s must agree on BOP producer/consumer validation\n",
              name(), other.name());
    }
    // The paired large/small identity is also the replay and direct-quality
    // key. It must not depend on whether native P/C happens to be enabled.
    pcValidationKind = PCValidationKind::Large;
    other.pcValidationKind = PCValidationKind::Small;
    pcValidationLargeName = name();
    pcValidationSmallName = other.name();
    other.pcValidationLargeName = pcValidationLargeName;
    other.pcValidationSmallName = pcValidationSmallName;
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
}

void
BOP::shareDirectQualityGateWith(BOP &other)
{
    if (enableDirectQualityGate != other.enableDirectQualityGate) {
        fatal("%s and %s must agree on direct-quality gate enablement\n",
              name(), other.name());
    }
    // Keep the direct-quality key stable even for a raw C run where the
    // shared gate is deliberately disabled.
    pcValidationKind = PCValidationKind::Large;
    other.pcValidationKind = PCValidationKind::Small;
    if (!enableDirectQualityGate)
        return;
    other.directQualityGate = directQualityGate;
}

void
BOP::setDirectQualityTraceSink(DirectQualityGate::TraceSink *sink)
{
    if (directQualityGate)
        directQualityGate->setTraceSink(sink);
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
        result.confidenceAfter, result.decayed, false, 0,
        result.lowEntryMissStreakBefore,
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
        if (result.hadValidation) {
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

void
BOP::updateDirectQualityStats()
{
    stats.directQualityCandidates = directQualityGate->candidates();
    // Kept as a compatibility alias for earlier result-processing scripts.
    stats.directQualityIssued = directQualityGate->candidates();
    stats.directQualityAllowed = directQualityGate->allowed();
    stats.directQualitySuppressed = directQualityGate->suppressed();
    stats.directQualitySampleSelected = directQualityGate->sampleSelected();
    stats.directQualitySampled = directQualityGate->sampled();
    stats.directQualityUseful = directQualityGate->useful();
    stats.directQualityUnused = directQualityGate->unused();
    stats.directQualityFeedbackConflicts =
        directQualityGate->feedbackConflicts();
    stats.directQualityFeedbackReplacements =
        directQualityGate->feedbackReplacements();
    stats.directQualityNonCanonicalFeedbackCandidates =
        directQualityGate->nonCanonicalFeedbackCandidates();
    stats.directQualityNonCanonicalFeedbackDemands =
        directQualityGate->nonCanonicalFeedbackDemands();
    stats.directQualityFeedbackExpiries = directQualityGate->feedbackExpiries();
    stats.directQualityFeedbackExpiryUnused =
        directQualityGate->feedbackExpiryUnused();
    stats.directQualityUnknownDrops = directQualityGate->unknownDrops();
    stats.directQualityFeedbackCoalesced =
        directQualityGate->feedbackCoalesced();
    stats.directQualityOrphanOutcomes = directQualityGate->orphanOutcomes();
    stats.directQualityStateTransitions = directQualityGate->stateTransitions();
    stats.directQualityBlockToRecoverTransitions =
        directQualityGate->blockToRecoverTransitions();
    stats.directQualityRecoverToOpenTransitions =
        directQualityGate->recoverToOpenTransitions();
    stats.directQualityRecoverToBlockTransitions =
        directQualityGate->recoverToBlockTransitions();
    stats.directQualityPeakOutstanding = directQualityGate->peakOutstanding();
}

void
BOP::notifyDirectQualityDemand(Addr paddr)
{
    if (!enableDirectQualityGate || !directQualityGate)
        return;
    directQualityGate->observeDemand(blockAddress(paddr));
    updateDirectQualityStats();
}

bool
BOP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                      std::vector<AddrPriority> &addresses, int prio,
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
    AddrPriority buffered_command(addr, prio, src, pfi.trigger_info);
    InsertPFRequestToBuffer(buffered_command);
    Addr filter_key = sharedFilterKey(pfi, addr);
    if (filter->contains(filter_key)) {
        DPRINTF(BOPPrefetcher, "Skip recently prefetched: %lx\n", addr);
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;
    } else {
        DPRINTF(BOPPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(filter_key, 0);
        AddrPriority queued_command(addr, prio, src, pfi.trigger_info);
        addresses.push_back(queued_command);
        return true;
    }
}

void
BOP::notifyFill(const PacketPtr& pkt)
{

}

BOP::BopStats::BopStats(statistics::Group *parent,
                         unsigned int student_pool_size,
                         unsigned int student_delay_queue_size)
    : statistics::Group(parent),
      ADD_STAT(issuedOffsetDist, statistics::units::Count::get(), "Distribution of issued offsets"),
      ADD_STAT(teacherInjectedOffsetDist, statistics::units::Count::get(),
               "Teacher offsets admitted to the student pool"),
      ADD_STAT(studentSelectedOffsetDist, statistics::units::Count::get(),
               "Student-selected offsets that pass the coverage threshold"),
      ADD_STAT(studentPoolOccupancyDist, statistics::units::Count::get(),
               "Student pool occupancy at teacher phase end"),
      ADD_STAT(studentCovRatioPctDist, statistics::units::Ratio::get(),
               "Student selected coverage percentage at phase end"),
      ADD_STAT(studentWorstCovRatioPctDist, statistics::units::Ratio::get(),
               "Student non-selected extreme coverage percentage at phase end"),
      ADD_STAT(studentDelayQueueOccupancyDist, statistics::units::Count::get(),
               "Student coverage delay queue occupancy"),
      ADD_STAT(learnOffsetCount, statistics::units::Count::get(), "Number of learning offsets"),
      ADD_STAT(teacherInjectedCount, statistics::units::Count::get(),
               "Teacher offsets inserted into the student pool"),
      ADD_STAT(studentPhaseCount, statistics::units::Count::get(),
               "Teacher-aligned student coverage phases"),
      ADD_STAT(studentIssueCount, statistics::units::Count::get(),
               "Student phases passing the independent issue threshold"),
      ADD_STAT(studentFallbackCount, statistics::units::Count::get(),
               "Student phases falling back to teacher output"),
      ADD_STAT(studentLargeOffsetPriorityCount, statistics::units::Count::get(),
               "Student phases selecting the large-offset priority path"),
      ADD_STAT(studentDelayQueueInsertCount, statistics::units::Count::get(),
               "Student coverage predictions inserted into the delay queue"),
      ADD_STAT(studentDelayQueueDrainCount, statistics::units::Count::get(),
               "Student coverage predictions made visible after delay"),
      ADD_STAT(studentDelayQueueFullDropCount, statistics::units::Count::get(),
               "Student coverage predictions dropped because the delay queue was full"),
      ADD_STAT(throttledCount, statistics::units::Count::get(), "Number of globally throttled prefetches"),
      ADD_STAT(issueValidationChecks, statistics::units::Count::get(),
               "Number of current-best-offset issue validation checks"),
      ADD_STAT(issueValidationHits, statistics::units::Count::get(),
               "Number of current-best-offset issue validation RR hits"),
      ADD_STAT(issueValidationSuppressed, statistics::units::Count::get(),
               "Number of BOP prefetches suppressed by issue validation"),
      ADD_STAT(pcValidationTableLookups, statistics::units::Count::get(),
               "Number of PC validation-confidence table lookups"),
      ADD_STAT(pcValidationTableHits, statistics::units::Count::get(),
               "Number of PC validation-confidence partial-tag hits"),
      ADD_STAT(pcValidationTableMisses, statistics::units::Count::get(),
               "Number of PC validation-confidence misses"),
      ADD_STAT(pcValidationTableReplacements, statistics::units::Count::get(),
               "Number of valid PC validation-confidence entries replaced"),
      ADD_STAT(pcValidationOffsetContextHits, statistics::units::Count::get(),
               "Number of per-PC offset confidence-context hits"),
      ADD_STAT(pcValidationOffsetContextMisses, statistics::units::Count::get(),
               "Number of per-PC offset confidence-context misses"),
      ADD_STAT(pcValidationOffsetContextReplacements,
               statistics::units::Count::get(),
               "Number of valid per-PC offset confidence contexts replaced"),
      ADD_STAT(pcValidationEpochResets, statistics::units::Count::get(),
               "Deprecated: epoch resets are disabled for offset contexts"),
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
               "Deprecated: offset contexts replace epoch changes"),
      ADD_STAT(rrOwnerValidHits, statistics::units::Count::get(),
               "RR hits with a valid producer-owner key"),
      ADD_STAT(rrOwnerInvalidHits, statistics::units::Count::get(),
               "RR hits whose producer-owner key is unavailable"),
      ADD_STAT(rrOwnerSamePCHits, statistics::units::Count::get(),
               "RR hits whose producer key matches the current PC key"),
      ADD_STAT(rrOwnerCrossPCHits, statistics::units::Count::get(),
               "RR hits whose producer key differs from the current PC key"),
      ADD_STAT(pcValidationProducerHitUpdates, statistics::units::Count::get(),
               "Producer-owned RR-hit updates submitted to the PC table"),
      ADD_STAT(pcValidationConsumerMissUpdates, statistics::units::Count::get(),
               "Current-consumer weak-miss updates submitted to the PC table"),
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
               "Q0.8 resolved coverage for completed global BOP outcome windows"),
      ADD_STAT(directQualityCandidates, statistics::units::Count::get(),
               "Raw BOP candidates evaluated by direct quality"),
      ADD_STAT(directQualityIssued, statistics::units::Count::get(),
               "Compatibility alias for raw direct-quality candidates"),
      ADD_STAT(directQualityAllowed, statistics::units::Count::get(),
               "Raw BOP candidates allowed by direct quality"),
      ADD_STAT(directQualitySuppressed, statistics::units::Count::get(),
               "Raw BOP candidates suppressed by direct quality"),
      ADD_STAT(directQualitySampleSelected, statistics::units::Count::get(),
               "Raw BOP candidates selected by direct-quality sampling"),
      ADD_STAT(directQualitySampled, statistics::units::Count::get(),
               "Selected raw BOP candidates inserted into feedback"),
      ADD_STAT(directQualityUseful, statistics::units::Count::get(),
               "Online direct-quality useful outcomes"),
      ADD_STAT(directQualityUnused, statistics::units::Count::get(),
               "Online direct-quality resolved unused outcomes"),
      ADD_STAT(directQualityFeedbackConflicts, statistics::units::Count::get(),
               "Online direct-quality feedback-table replacements"),
      ADD_STAT(directQualityFeedbackReplacements,
               statistics::units::Count::get(),
               "Online direct-quality feedback-table replacements"),
      ADD_STAT(directQualityNonCanonicalFeedbackCandidates,
               statistics::units::Count::get(),
               "Sampled direct-quality candidates using folded non-canonical addresses"),
      ADD_STAT(directQualityNonCanonicalFeedbackDemands,
               statistics::units::Count::get(),
               "L2 demands using folded non-canonical addresses"),
      ADD_STAT(directQualityFeedbackExpiries, statistics::units::Count::get(),
               "Online direct-quality demand-window expiry events"),
      ADD_STAT(directQualityFeedbackExpiryUnused,
               statistics::units::Count::get(),
               "Online direct-quality expiry events resolved as unused"),
      ADD_STAT(directQualityUnknownDrops, statistics::units::Count::get(),
               "Online direct-quality censored feedback drops"),
      ADD_STAT(directQualityFeedbackCoalesced,
               statistics::units::Count::get(),
               "Raw direct-quality samples coalesced by candidate line"),
      ADD_STAT(directQualityOrphanOutcomes, statistics::units::Count::get(),
               "Online direct-quality orphan outcomes"),
      ADD_STAT(directQualityStateTransitions, statistics::units::Count::get(),
               "Online direct-quality state transitions"),
      ADD_STAT(directQualityBlockToRecoverTransitions,
               statistics::units::Count::get(),
               "Online direct-quality BLOCK-to-RECOVER transitions"),
      ADD_STAT(directQualityRecoverToOpenTransitions,
               statistics::units::Count::get(),
               "Online direct-quality RECOVER-to-OPEN transitions"),
      ADD_STAT(directQualityRecoverToBlockTransitions,
               statistics::units::Count::get(),
               "Online direct-quality RECOVER-to-BLOCK transitions"),
      ADD_STAT(directQualityPeakOutstanding, statistics::units::Count::get(),
               "Peak retained online direct-quality feedback entries")
{
    issuedOffsetDist.init(-64, 256, 1).prereq(issuedOffsetDist);
    teacherInjectedOffsetDist.init(-256, 257, 1).prereq(
        teacherInjectedOffsetDist);
    studentSelectedOffsetDist.init(-256, 257, 1).prereq(
        studentSelectedOffsetDist);
    studentPoolOccupancyDist.init(0, student_pool_size + 1, 1).prereq(
        studentPoolOccupancyDist);
    studentCovRatioPctDist.init(0, 101, 1).prereq(studentCovRatioPctDist);
    studentWorstCovRatioPctDist.init(0, 101, 1).prereq(
        studentWorstCovRatioPctDist);
    studentDelayQueueOccupancyDist.init(
        0, std::max(1u, student_delay_queue_size) + 1, 1).prereq(
            studentDelayQueueOccupancyDist);
    pcValidationConfidenceDist.init(0, 256, 1).prereq(
        pcValidationConfidenceDist);
    globalBOPUnusedEwma.init(0, 256, 1).prereq(globalBOPUnusedEwma);
    globalBOPResolvedCoverage.init(0, 256, 1).prereq(
        globalBOPResolvedCoverage);
}

} // namespace prefetch
} // namespace gem5
