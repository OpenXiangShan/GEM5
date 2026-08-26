#include "mem/cache/prefetch/direct_quality_gate.hh"

#include <algorithm>
#include <cassert>

namespace gem5
{
namespace prefetch
{

namespace
{

bool
isPowerOf2(unsigned value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

} // anonymous namespace

DirectQualityGate::DirectQualityGate() : DirectQualityGate(Config())
{}

DirectQualityGate::DirectQualityGate(const Config &config)
    : cfg(config),
      qualitySets(config.qualityEntries / config.qualityWays),
      feedbackSets(config.feedbackEntries / config.feedbackWays),
      qualitySetBits(0), feedbackSetBits(0),
      qualityTagMask(config.qualityTagBits >= 63 ? ~Addr(0) :
                     ((Addr(1) << config.qualityTagBits) - 1))
{
    assert(cfg.qualityWays == 1 || cfg.qualityWays == 2 ||
           cfg.qualityWays == 4);
    assert(cfg.feedbackWays > 0 && cfg.feedbackWays <= MaxFeedbackWays);
    assert(cfg.qualityEntries > 0 && cfg.qualityEntries <= MaxQualityEntries);
    assert(cfg.feedbackEntries > 0 && cfg.feedbackEntries <= MaxFeedbackEntries);
    assert((cfg.qualityEntries % cfg.qualityWays) == 0);
    assert((cfg.feedbackEntries % cfg.feedbackWays) == 0);
    assert(isPowerOf2(cfg.qualityEntries));
    assert(isPowerOf2(cfg.feedbackEntries));
    assert(isPowerOf2(qualitySets));
    assert(isPowerOf2(feedbackSets));
    assert(cfg.decayPeriod == 0 || isPowerOf2(cfg.decayPeriod));
    while ((1U << qualitySetBits) < qualitySets)
        ++qualitySetBits;
    while ((1U << feedbackSetBits) < feedbackSets)
        ++feedbackSetBits;
}

void
DirectQualityGate::setTraceSink(TraceSink *sink)
{
    traceSink = sink;
    if (traceSink)
        traceSink->directQualityTraceConfig(cfg);
}

uint64_t
DirectQualityGate::mix64(uint64_t value)
{
    value ^= value >> 30;
    value *= 0xBF58476D1CE4E5B9ULL;
    value ^= value >> 27;
    value *= 0x94D049BB133111EBULL;
    value ^= value >> 31;
    return value;
}

uint64_t
DirectQualityGate::qualitySignature(Addr pc, uint8_t kind) const
{
    return mix64((pc >> 1) ^
                 (uint64_t(kind) * 0x9E3779B97F4A7C15ULL));
}

unsigned
DirectQualityGate::qualitySetFor(Addr pc, uint8_t kind) const
{
    return qualitySignature(pc, kind) & (qualitySets - 1);
}

Addr
DirectQualityGate::qualityTagFor(Addr pc, uint8_t kind) const
{
    return (qualitySignature(pc, kind) >> qualitySetBits) & qualityTagMask;
}

unsigned
DirectQualityGate::feedbackSetFor(Addr line) const
{
    return mix64(line >> 6) & (feedbackSets - 1);
}

unsigned
DirectQualityGate::findQuality(unsigned set, Addr tag, uint8_t kind) const
{
    const unsigned base = set * cfg.qualityWays;
    for (unsigned way = 0; way < cfg.qualityWays; ++way) {
        const auto &entry = quality[base + way];
        if (entry.valid && entry.tag == tag && entry.kind == kind)
            return way;
    }
    return cfg.qualityWays;
}

unsigned
DirectQualityGate::qualityVictim(unsigned set) const
{
    const unsigned base = set * cfg.qualityWays;
    for (unsigned way = 0; way < cfg.qualityWays; ++way) {
        if (!quality[base + way].valid)
            return way;
    }

    if (cfg.qualityWays == 1)
        return 0;
    if (cfg.qualityWays == 2)
        return qualityPLRU[set] & 0x1;

    const uint8_t state = qualityPLRU[set] & 0x7;
    if ((state & 0x1) == 0)
        return (state & 0x2) == 0 ? 0 : 1;
    return (state & 0x4) == 0 ? 2 : 3;
}

void
DirectQualityGate::touchQuality(unsigned set, unsigned way)
{
    if (cfg.qualityWays == 1) {
        assert(way == 0);
        return;
    }

    if (cfg.qualityWays == 2) {
        assert(way < 2);
        qualityPLRU[set] = way == 0 ? 1 : 0;
        return;
    }

    assert(way < 4);
    uint8_t state = qualityPLRU[set];
    if (way == 0) {
        state |= 0x1;
        state |= 0x2;
    } else if (way == 1) {
        state |= 0x1;
        state &= ~0x2;
    } else if (way == 2) {
        state &= ~0x1;
        state |= 0x4;
    } else {
        state &= ~0x1;
        state &= ~0x4;
    }
    qualityPLRU[set] = state;
}

unsigned
DirectQualityGate::allocateQuality(unsigned set, Addr tag, uint8_t kind)
{
    const unsigned way = qualityVictim(set);
    auto &entry = quality[set * cfg.qualityWays + way];
    const uint8_t nextGeneration = entry.generation + 1;
    entry = QualityEntry();
    entry.valid = true;
    entry.tag = tag;
    entry.kind = kind;
    entry.generation = nextGeneration;
    return way;
}

unsigned
DirectQualityGate::findFeedback(unsigned set, Addr line) const
{
    const unsigned base = set * cfg.feedbackWays;
    for (unsigned way = 0; way < cfg.feedbackWays; ++way) {
        if (feedback[base + way].valid && feedback[base + way].line == line)
            return way;
    }
    return cfg.feedbackWays;
}

unsigned
DirectQualityGate::feedbackVictim(unsigned set)
{
    const unsigned base = set * cfg.feedbackWays;
    for (unsigned way = 0; way < cfg.feedbackWays; ++way) {
        if (!feedback[base + way].valid)
            return way;
    }

    const unsigned way = feedbackNextVictim[set];
    feedbackNextVictim[set] = (way + 1) % cfg.feedbackWays;
    return way;
}

unsigned
DirectQualityGate::allocateFeedback(unsigned set)
{
    const unsigned way = feedbackVictim(set);
    const unsigned index = feedbackIndex(set, way);
    auto &entry = feedback[index];
    if (entry.valid) {
        ++feedbackConflictCount;
        ++feedbackReplacementCount;
        retireUnknown(index, TraceOutcome::UnknownFeedbackReplacement);
    }
    entry = FeedbackEntry();
    return way;
}

unsigned
DirectQualityGate::feedbackIndex(unsigned set, unsigned way) const
{
    return set * cfg.feedbackWays + way;
}

bool
DirectQualityGate::sample(Addr pc, uint8_t kind, Addr trigger_line,
                           unsigned period, uint64_t salt) const
{
    assert(isPowerOf2(period));
    const uint64_t signature = qualitySignature(pc, kind) ^ trigger_line ^ salt;
    return (mix64(signature) & (period - 1)) == 0;
}

DirectQualityGate::Decision
DirectQualityGate::admit(Addr pc, uint8_t kind, Addr trigger_line,
                         Addr candidate_line)
{
    const unsigned set = qualitySetFor(pc, kind);
    const Addr tag = qualityTagFor(pc, kind);
    unsigned way = findQuality(set, tag, kind);
    if (way == cfg.qualityWays)
        way = allocateQuality(set, tag, kind);
    auto &entry = quality[set * cfg.qualityWays + way];
    touchQuality(set, way);
    ++entry.candidates;
    ++candidateCount;

    Decision decision;
    decision.set = set;
    decision.way = way;
    decision.generation = entry.generation;
    decision.state = entry.state;
    if (entry.state == State::Block) {
        decision.allowed = sample(pc, kind, trigger_line,
                                  blockProbePeriod(entry), 0xB10C);
        decision.sampled = decision.allowed;
    } else if (entry.state == State::Recover) {
        decision.allowed = sample(pc, kind, trigger_line,
                                  entry.recoveryProbePeriod, 0x5EC0);
        decision.sampled = decision.allowed;
    } else if (entry.state == State::Open) {
        decision.sampled = sample(pc, kind, trigger_line,
                                  cfg.openSamplePeriod, 0x5A6D);
    } else {
        decision.sampled = sample(pc, kind, trigger_line,
                                  cfg.observeSamplePeriod, 0x0B5E);
    }

    if (decision.allowed) {
        ++allowedCount;
    } else {
        ++suppressedCount;
    }

    if (traceSink) {
        traceSink->directQualityTraceCandidate(
            ++nextTraceEventSequence, pc, kind, trigger_line, candidate_line,
            decision.state, decision.allowed, decision.sampled);
    }
    if (decision.sampled) {
        ++sampleSelectedCount;
        decision.feedbackInserted = recordCandidate(
            candidate_line, kind, set, way, entry.generation) != 0;
    }
    return decision;
}

uint64_t
DirectQualityGate::recordCandidate(Addr line, uint8_t kind,
                                   unsigned quality_set,
                                   unsigned quality_way,
                                   uint8_t quality_generation)
{
    assert(quality_set < qualitySets);
    assert(quality_way < cfg.qualityWays);
    auto &qualityEntry = quality[quality_set * cfg.qualityWays + quality_way];
    assert(qualityEntry.valid);
    assert(qualityEntry.generation == quality_generation);
    assert(qualityEntry.kind == kind);

    const unsigned set = feedbackSetFor(line);
    if (findFeedback(set, line) != cfg.feedbackWays) {
        ++feedbackCoalescedCount;
        return 0;
    }

    const unsigned way = allocateFeedback(set);
    const unsigned index = feedbackIndex(set, way);
    auto &entry = feedback[index];
    entry.valid = true;
    entry.line = line;
    entry.qualitySet = quality_set;
    entry.qualityWay = quality_way;
    entry.qualityGeneration = quality_generation;
    entry.kind = kind;
    entry.recoveryGeneration = qualityEntry.recoveryGeneration;
    entry.issueAge = demandAge;
    entry.traceId = ++nextFeedbackId;
    insertExpiry(index);
    ++outstandingCount;
    peakOutstandingCount = std::max(peakOutstandingCount, outstandingCount);
    ++qualityEntry.sampled;
    ++sampledCount;
    if (traceSink) {
        traceSink->directQualityTraceIssue(++nextTraceEventSequence,
                                           entry.traceId, entry.issueAge,
                                           entry.line, entry.kind);
    }
    return entry.traceId;
}

void
DirectQualityGate::invalidateFeedback(unsigned feedback_index)
{
    auto &entry = feedback[feedback_index];
    assert(entry.valid);
    removeExpiry(feedback_index);
    entry.valid = false;
    assert(outstandingCount > 0);
    --outstandingCount;
}

void
DirectQualityGate::retireUnknown(unsigned feedback_index, TraceOutcome outcome)
{
    const auto entry = feedback[feedback_index];
    traceOutcome(entry, outcome);
    invalidateFeedback(feedback_index);
    ++unknownDropCount;
}

void
DirectQualityGate::traceOutcome(const FeedbackEntry &entry,
                                TraceOutcome outcome)
{
    if (traceSink) {
        traceSink->directQualityTraceOutcome(++nextTraceEventSequence,
                                             entry.traceId, demandAge,
                                             entry.line, outcome);
    }
}

unsigned
DirectQualityGate::blockProbePeriod(const QualityEntry &entry) const
{
    const uint64_t strictLimit =
        uint64_t(cfg.strictUnusedPerUseful) * entry.useful +
        cfg.strictBlockGuard;
    return entry.unused >= strictLimit ? cfg.blockProbePeriod :
        cfg.borderlineBlockProbePeriod;
}

bool
DirectQualityGate::shouldBlock(const QualityEntry &entry) const
{
    const uint64_t blockLimit = uint64_t(cfg.unusedPerUseful) * entry.useful +
        cfg.blockGuard;
    return entry.unused >= blockLimit;
}

bool
DirectQualityGate::meetsReopen(const QualityEntry &entry) const
{
    const uint64_t reopenLimit =
        uint64_t(cfg.reopenUnusedPerUseful) * entry.useful;
    return reopenLimit >= cfg.reopenGuard &&
        entry.unused <= reopenLimit - cfg.reopenGuard;
}

void
DirectQualityGate::transitionTo(QualityEntry &entry, State next)
{
    if (entry.state == next)
        return;

    if (entry.state == State::Block && next == State::Recover) {
        ++blockToRecoverTransitionCount;
    } else if (entry.state == State::Recover && next == State::Open) {
        ++recoverToOpenTransitionCount;
    } else if (entry.state == State::Recover && next == State::Block) {
        ++recoverToBlockTransitionCount;
    }
    entry.state = next;
    ++stateTransitionCount;
}

void
DirectQualityGate::updateState(QualityEntry &entry,
                               unsigned previous_block_probe_period)
{
    const uint64_t samples = uint64_t(entry.useful) + entry.unused;
    if (!entry.trained && samples < cfg.minSamples) {
        transitionTo(entry, State::Observe);
        return;
    }
    if (samples >= cfg.minSamples)
        entry.trained = true;
    if (!entry.trained)
        return;

    if (entry.state == State::Block) {
        if (!meetsReopen(entry))
            return;
        if (cfg.reopenConfirmSamples == 0) {
            transitionTo(entry, State::Open);
            return;
        }
        entry.recoverySamples = 0;
        ++entry.recoveryGeneration;
        entry.recoveryProbePeriod = previous_block_probe_period != 0 ?
            previous_block_probe_period : blockProbePeriod(entry);
        transitionTo(entry, State::Recover);
        return;
    }

    if (entry.state == State::Recover) {
        if (shouldBlock(entry)) {
            entry.recoverySamples = 0;
            transitionTo(entry, State::Block);
        } else if (entry.recoverySamples >= cfg.reopenConfirmSamples &&
                   meetsReopen(entry)) {
            transitionTo(entry, State::Open);
        }
        return;
    }

    transitionTo(entry, shouldBlock(entry) ? State::Block : State::Open);
}

void
DirectQualityGate::applyOutcome(QualityEntry &entry,
                                uint32_t recovery_generation,
                                bool isUseful)
{
    const State previousState = entry.state;
    const unsigned previousBlockProbePeriod = blockProbePeriod(entry);

    if (isUseful) {
        ++entry.useful;
        ++usefulCount;
    } else {
        ++entry.unused;
        ++unusedCount;
    }
    if (previousState == State::Recover &&
        recovery_generation == entry.recoveryGeneration) {
        ++entry.recoverySamples;
    }
    ++entry.resolvedSinceDecay;
    updateState(entry, previousBlockProbePeriod);

    if (cfg.decayPeriod != 0 &&
        entry.resolvedSinceDecay >= cfg.decayPeriod) {
        entry.useful >>= 1;
        entry.unused >>= 1;
        entry.resolvedSinceDecay = 0;
        updateState(entry);
    }
}

bool
DirectQualityGate::resolveFeedback(unsigned feedback_index, bool isUseful,
                                   TraceOutcome outcome)
{
    auto &fb = feedback[feedback_index];
    assert(fb.valid);
    const unsigned qbase = fb.qualitySet * cfg.qualityWays;
    auto &entry = quality[qbase + fb.qualityWay];
    if (!entry.valid || entry.generation != fb.qualityGeneration ||
        entry.kind != fb.kind) {
        retireUnknown(feedback_index, TraceOutcome::UnknownOwnerReplaced);
        ++orphanOutcomeCount;
        return false;
    }
    applyOutcome(entry, fb.recoveryGeneration, isUseful);
    traceOutcome(fb, outcome);
    invalidateFeedback(feedback_index);
    return true;
}

void
DirectQualityGate::observeDemand(Addr line)
{
    ++demandAge;
    if (traceSink) {
        traceSink->directQualityTraceDemand(++nextTraceEventSequence,
                                            demandAge, line);
    }
    expireFeedback();

    const unsigned set = feedbackSetFor(line);
    const unsigned way = findFeedback(set, line);
    if (way != cfg.feedbackWays) {
        resolveFeedback(feedbackIndex(set, way), true,
                        TraceOutcome::UsefulDemand);
    }
}

bool
DirectQualityGate::expiryBefore(unsigned lhs, unsigned rhs) const
{
    const auto &left = feedback[lhs];
    const auto &right = feedback[rhs];
    return left.issueAge != right.issueAge ? left.issueAge < right.issueAge :
        left.traceId < right.traceId;
}

void
DirectQualityGate::restoreExpiryHeap(unsigned heap_index)
{
    if (heap_index != 0 &&
        expiryBefore(expiryHeap[heap_index],
                     expiryHeap[(heap_index - 1) / 2])) {
        while (heap_index != 0) {
            const unsigned parent = (heap_index - 1) / 2;
            if (!expiryBefore(expiryHeap[heap_index], expiryHeap[parent]))
                break;
            std::swap(expiryHeap[heap_index], expiryHeap[parent]);
            feedback[expiryHeap[heap_index]].expiryHeapIndex = heap_index;
            feedback[expiryHeap[parent]].expiryHeapIndex = parent;
            heap_index = parent;
        }
        return;
    }

    while (true) {
        const unsigned left = heap_index * 2 + 1;
        if (left >= expiryHeapSize)
            break;
        unsigned smallest = left;
        const unsigned right = left + 1;
        if (right < expiryHeapSize &&
            expiryBefore(expiryHeap[right], expiryHeap[left])) {
            smallest = right;
        }
        if (!expiryBefore(expiryHeap[smallest], expiryHeap[heap_index]))
            break;
        std::swap(expiryHeap[heap_index], expiryHeap[smallest]);
        feedback[expiryHeap[heap_index]].expiryHeapIndex = heap_index;
        feedback[expiryHeap[smallest]].expiryHeapIndex = smallest;
        heap_index = smallest;
    }
}

void
DirectQualityGate::insertExpiry(unsigned feedback_index)
{
    assert(expiryHeapSize < cfg.feedbackEntries);
    assert(expiryHeapSize < MaxFeedbackEntries);
    auto &entry = feedback[feedback_index];
    assert(entry.valid);
    assert(entry.expiryHeapIndex == NoExpiryRecord);
    const unsigned heap_index = expiryHeapSize++;
    expiryHeap[heap_index] = feedback_index;
    entry.expiryHeapIndex = heap_index;
    restoreExpiryHeap(heap_index);
}

void
DirectQualityGate::removeExpiry(unsigned feedback_index)
{
    auto &entry = feedback[feedback_index];
    const unsigned heap_index = entry.expiryHeapIndex;
    assert(heap_index != NoExpiryRecord);
    assert(heap_index < expiryHeapSize);
    assert(expiryHeap[heap_index] == feedback_index);

    --expiryHeapSize;
    if (heap_index != expiryHeapSize) {
        expiryHeap[heap_index] = expiryHeap[expiryHeapSize];
        feedback[expiryHeap[heap_index]].expiryHeapIndex = heap_index;
    }
    entry.expiryHeapIndex = NoExpiryRecord;
    if (heap_index < expiryHeapSize)
        restoreExpiryHeap(heap_index);
}

void
DirectQualityGate::expireFeedback()
{
    while (expiryHeapSize != 0) {
        const unsigned feedback_index = expiryHeap[0];
        const auto &entry = feedback[feedback_index];
        assert(entry.valid);
        if (demandAge - entry.issueAge <= cfg.horizon)
            return;

        ++feedbackExpiryCount;
        if (resolveFeedback(feedback_index, false,
                            TraceOutcome::UnusedExpiry)) {
            ++feedbackExpiryUnusedCount;
        }
    }
}

DirectQualityGate::State
DirectQualityGate::state(Addr pc, uint8_t kind) const
{
    const unsigned set = qualitySetFor(pc, kind);
    const unsigned way = findQuality(set, qualityTagFor(pc, kind), kind);
    return way == cfg.qualityWays ? State::Observe :
        quality[set * cfg.qualityWays + way].state;
}

} // namespace prefetch
} // namespace gem5
