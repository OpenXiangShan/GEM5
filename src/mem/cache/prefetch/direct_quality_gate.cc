#include "mem/cache/prefetch/direct_quality_gate.hh"

#include <algorithm>
#include <cassert>

namespace gem5
{
namespace prefetch
{

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
    assert(cfg.qualityWays > 0 && cfg.qualityWays <= MaxQualityWays);
    assert(cfg.feedbackWays > 0 && cfg.feedbackWays <= MaxFeedbackWays);
    assert(cfg.qualityEntries > 0 && cfg.qualityEntries <= MaxQualityEntries);
    assert(cfg.feedbackEntries > 0 && cfg.feedbackEntries <= MaxFeedbackEntries);
    assert((cfg.qualityEntries % cfg.qualityWays) == 0);
    assert((cfg.feedbackEntries % cfg.feedbackWays) == 0);
    assert((cfg.qualityEntries & (cfg.qualityEntries - 1)) == 0);
    assert((cfg.feedbackEntries & (cfg.feedbackEntries - 1)) == 0);
    assert((qualitySets & (qualitySets - 1)) == 0);
    assert((feedbackSets & (feedbackSets - 1)) == 0);
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

unsigned
DirectQualityGate::qualitySetFor(Addr pc, uint8_t kind) const
{
    Addr hash = pc ^ (pc >> 17) ^ (Addr(kind) * 0x9e3779b9ULL);
    return hash & (qualitySets - 1);
}

Addr
DirectQualityGate::qualityTagFor(Addr pc, uint8_t kind) const
{
    Addr hash = pc ^ (pc >> 23) ^ (Addr(kind) * 0x517cc1b7ULL);
    return (hash >> qualitySetBits) & qualityTagMask;
}

unsigned
DirectQualityGate::feedbackSetFor(Addr line) const
{
    return (line ^ (line >> 13)) & (feedbackSets - 1);
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
    for (unsigned way = 0; way < cfg.qualityWays; ++way)
        if (!quality[base + way].valid)
            return way;
    return quality[base].plru % cfg.qualityWays;
}

void
DirectQualityGate::touchQuality(unsigned set, unsigned way)
{
    const unsigned base = set * cfg.qualityWays;
    for (unsigned candidate = 0; candidate < cfg.qualityWays; ++candidate) {
        if (candidate == way)
            quality[base + candidate].plru = 0;
        else if (quality[base + candidate].valid)
            quality[base + candidate].plru =
                std::min<unsigned>(cfg.qualityWays - 1,
                                   quality[base + candidate].plru + 1);
    }
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
    touchQuality(set, way);
    return way;
}

unsigned
DirectQualityGate::findFeedback(unsigned set, Addr line) const
{
    const unsigned base = set * cfg.feedbackWays;
    for (unsigned way = 0; way < cfg.feedbackWays; ++way)
        if (feedback[base + way].valid && feedback[base + way].line == line)
            return way;
    return cfg.feedbackWays;
}

unsigned
DirectQualityGate::feedbackVictim(unsigned set) const
{
    const unsigned base = set * cfg.feedbackWays;
    for (unsigned way = 0; way < cfg.feedbackWays; ++way)
        if (!feedback[base + way].valid)
            return way;
    unsigned victim = 0;
    uint64_t oldest = feedback[base].issueAge;
    for (unsigned way = 1; way < cfg.feedbackWays; ++way) {
        if (feedback[base + way].issueAge < oldest) {
            oldest = feedback[base + way].issueAge;
            victim = way;
        }
    }
    return victim;
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

DirectQualityGate::Decision
DirectQualityGate::admit(Addr pc, uint8_t kind, Addr line)
{
    const unsigned set = qualitySetFor(pc, kind);
    const Addr tag = qualityTagFor(pc, kind);
    unsigned way = findQuality(set, tag, kind);
    if (way == cfg.qualityWays)
        way = allocateQuality(set, tag, kind);
    auto &entry = quality[set * cfg.qualityWays + way];
    touchQuality(set, way);
    ++entry.issued;
    ++issuedCount;

    Decision decision;
    decision.set = set;
    decision.way = way;
    decision.generation = entry.generation;
    decision.state = entry.state;
    if (entry.state == State::Block || entry.state == State::Recover) {
        const unsigned period = entry.state == State::Block ?
            blockProbePeriod(entry) : entry.recoveryProbePeriod;
        decision.allowed = period != 0 && (entry.issued % period) == 0;
        decision.sampled = decision.allowed;
    } else {
        const unsigned period = entry.state == State::Observe ?
            cfg.observeSamplePeriod : cfg.openSamplePeriod;
        decision.sampled = period != 0 && (entry.issued % period) == 0;
    }
    if (!decision.allowed)
        ++suppressedCount;
    (void)line;
    return decision;
}

void
DirectQualityGate::recordIssued(Addr line, uint8_t kind, unsigned quality_set,
                                 unsigned quality_way,
                                 uint8_t quality_generation)
{
    if (quality_set >= qualitySets || quality_way >= cfg.qualityWays) {
        ++feedbackTokenDropCount;
        ++unknownDropCount;
        return;
    }

    auto &quality_entry = quality[quality_set * cfg.qualityWays + quality_way];
    if (!quality_entry.valid || quality_entry.generation != quality_generation ||
        quality_entry.kind != kind) {
        ++feedbackTokenDropCount;
        ++unknownDropCount;
        return;
    }

    const unsigned set = feedbackSetFor(line);
    unsigned way = findFeedback(set, line);
    if (way != cfg.feedbackWays) {
        // The cache can carry only one source tag for a coalesced line.
        // Preserve the first physical request's attribution instead of
        // replacing it with a later logical candidate.
        return;
    }
    if (way == cfg.feedbackWays)
        way = allocateFeedback(set);
    const unsigned index = feedbackIndex(set, way);
    auto &entry = feedback[index];
    entry.valid = true;
    entry.line = line;
    entry.qualitySet = quality_set;
    entry.qualityWay = quality_way;
    entry.qualityGeneration = quality_generation;
    entry.kind = kind;
    entry.recoveryGeneration = quality_entry.recoveryGeneration;
    entry.issueAge = demandAge;
    entry.traceId = ++nextFeedbackId;
    insertExpiry(index);
    ++outstandingCount;
    peakOutstandingCount = std::max(peakOutstandingCount, outstandingCount);
    ++quality_entry.sampled;
    ++sampledCount;
    if (traceSink) {
        traceSink->directQualityTraceIssue(++nextTraceEventSequence,
                                           entry.traceId, entry.issueAge,
                                           entry.line, entry.kind);
    }
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
    const uint64_t strict_limit =
        uint64_t(cfg.strictUnusedPerUseful) * entry.useful +
        cfg.strictBlockGuard;
    return entry.unused >= strict_limit ? cfg.blockProbePeriod :
        cfg.borderlineBlockProbePeriod;
}

bool
DirectQualityGate::shouldBlock(const QualityEntry &entry) const
{
    const uint64_t block_limit = uint64_t(cfg.unusedPerUseful) * entry.useful +
        cfg.blockGuard;
    return entry.unused >= block_limit;
}

bool
DirectQualityGate::meetsReopen(const QualityEntry &entry) const
{
    const uint64_t reopen_limit =
        uint64_t(cfg.reopenUnusedPerUseful) * entry.useful;
    return reopen_limit >= cfg.reopenGuard &&
        entry.unused <= reopen_limit - cfg.reopenGuard;
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
DirectQualityGate::applyOutcome(QualityEntry &entry,
                                uint32_t recovery_generation,
                                bool isUseful)
{
    const State previous_state = entry.state;
    const unsigned previous_block_probe_period = blockProbePeriod(entry);

    if (isUseful) {
        ++entry.useful;
        ++usefulCount;
    } else {
        ++entry.unused;
        ++unusedCount;
    }

    if (entry.useful + entry.unused < cfg.minSamples)
        return;

    if (previous_state == State::Observe || previous_state == State::Open) {
        transitionTo(entry, shouldBlock(entry) ? State::Block : State::Open);
        return;
    }

    if (previous_state == State::Block) {
        if (!meetsReopen(entry))
            return;

        if (cfg.reopenConfirmSamples == 0) {
            transitionTo(entry, State::Open);
            return;
        }

        entry.recoverySamples = 0;
        ++entry.recoveryGeneration;
        entry.recoveryProbePeriod = previous_block_probe_period;
        transitionTo(entry, State::Recover);
        return;
    }

    assert(previous_state == State::Recover);
    if (recovery_generation == entry.recoveryGeneration)
        ++entry.recoverySamples;

    if (shouldBlock(entry)) {
        entry.recoverySamples = 0;
        transitionTo(entry, State::Block);
    } else if (entry.recoverySamples >= cfg.reopenConfirmSamples &&
               meetsReopen(entry)) {
        transitionTo(entry, State::Open);
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
        lhs < rhs;
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
                            TraceOutcome::UnusedExpiry))
            ++feedbackExpiryUnusedCount;
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
