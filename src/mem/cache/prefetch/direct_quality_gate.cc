#include "mem/cache/prefetch/direct_quality_gate.hh"

#include <algorithm>
#include <cassert>

#include "base/logging.hh"

namespace gem5
{
namespace prefetch
{

bool
DirectQualityGate::isPowerOf2(unsigned value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

unsigned
DirectQualityGate::log2Of(unsigned value)
{
    unsigned bits = 0;
    while ((1U << bits) < value)
        ++bits;
    return bits;
}

uint64_t
DirectQualityGate::mix(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

DirectQualityGate::DirectQualityGate() : DirectQualityGate(Config()) {}

DirectQualityGate::DirectQualityGate(const Config &config)
    : cfg(config),
      qualitySets(config.qualityEntries / config.qualityWays),
      feedbackSets(config.feedbackEntries / config.feedbackWays),
      qualitySetBits(0),
      feedbackSetBits(0),
      qualityTagMask(config.qualityTagBits >= 63 ? ~uint64_t(0) : ((uint64_t(1) << config.qualityTagBits) - 1)),
      feedbackTagMask(config.feedbackTagBits >= 63 ? ~uint64_t(0) : ((uint64_t(1) << config.feedbackTagBits) - 1))
{
    fatal_if(cfg.qualityWays == 0 || cfg.qualityWays > 4 || cfg.feedbackWays == 0 || cfg.feedbackWays > 16,
             "Invalid direct-quality associativity\n");
    fatal_if(cfg.qualityEntries == 0 || cfg.qualityEntries > MaxQualityEntries || cfg.feedbackEntries == 0 ||
                 cfg.feedbackEntries > MaxFeedbackEntries,
             "Invalid direct-quality table size\n");
    fatal_if(!isPowerOf2(cfg.qualityEntries) || !isPowerOf2(cfg.feedbackEntries) ||
                 cfg.qualityEntries % cfg.qualityWays != 0 || cfg.feedbackEntries % cfg.feedbackWays != 0,
             "Direct-quality entries must be powers of two and divisible by ways\n");
    fatal_if(!isPowerOf2(qualitySets) || !isPowerOf2(feedbackSets),
             "Direct-quality set counts must be powers of two\n");
    fatal_if(
        cfg.qualityTagBits == 0 || cfg.qualityTagBits > 16 || cfg.feedbackTagBits == 0 || cfg.feedbackTagBits > 36,
        "Invalid direct-quality tag width\n");
    fatal_if(cfg.epochBits == 0 || cfg.epochBits > 7 || cfg.epochShift > 7 || cfg.epochTimeout == 0 ||
                 cfg.epochTimeout >= (1U << (cfg.epochBits - 1)),
             "Invalid direct-quality epoch encoding\n");
    fatal_if(cfg.observeSamplePeriod == 0 || cfg.openSamplePeriod == 0 || cfg.blockProbePeriod == 0 ||
                 cfg.borderlineBlockProbePeriod == 0 || cfg.minSamples == 0,
             "Direct-quality sample periods and min_samples must be non-zero\n");
    fatal_if(cfg.strictUnusedPerUseful < cfg.unusedPerUseful || cfg.reopenUnusedPerUseful > cfg.unusedPerUseful,
             "Direct-quality ratios must satisfy strict >= block >= reopen\n");
    fatal_if(uint64_t(cfg.epochTimeout) * (uint64_t(1) << cfg.epochShift) + cfg.feedbackEntries / 2 > cfg.horizon,
             "Direct-quality epoch timeout exceeds the demand horizon\n");
    qualitySetBits = log2Of(qualitySets);
    feedbackSetBits = log2Of(feedbackSets);
}

unsigned
DirectQualityGate::qualitySetFor(Addr pc, uint8_t kind) const
{
    uint64_t signature = pc >> 1;
    signature ^= signature >> 7;
    signature ^= signature >> 13;
    signature ^= signature >> 27;
    signature ^= uint64_t(kind) * UINT64_C(0x9e3779b97f4a7c15);
    signature ^= signature >> 11;
    signature ^= signature >> 23;
    return signature & (qualitySets - 1);
}

uint64_t
DirectQualityGate::qualityTagFor(Addr pc, uint8_t kind) const
{
    uint64_t signature = pc >> 1;
    signature ^= signature >> 7;
    signature ^= signature >> 13;
    signature ^= signature >> 27;
    signature ^= uint64_t(kind) * UINT64_C(0x9e3779b97f4a7c15);
    signature ^= signature >> 11;
    signature ^= signature >> 23;
    return (signature >> qualitySetBits) & qualityTagMask;
}

uint64_t
DirectQualityGate::feedbackKeyFor(uint64_t line, bool *nonCanonical)
{
    uint64_t compactLine = line & FeedbackLineMask;
    const uint64_t canonicalLine =
        compactLine & FeedbackLineSignBit ? compactLine | (HostLineMask & ~FeedbackLineMask) : compactLine;
    const bool nonCanonicalLine = line != canonicalLine;
    if (nonCanonical)
        *nonCanonical = nonCanonicalLine;
    if (nonCanonicalLine) {
        const uint64_t highLine = line >> FeedbackLineBits;
        compactLine ^= highLine;
        compactLine ^= highLine << 13;
        compactLine ^= highLine << 27;
        compactLine &= FeedbackLineMask;
    }
    uint64_t key = compactLine;
    key ^= key >> 17;
    key ^= (key << 13) & FeedbackLineMask;
    key ^= key >> 6;
    key ^= (key << 7) & FeedbackLineMask;
    key ^= key >> 11;
    return key & FeedbackLineMask;
}

unsigned
DirectQualityGate::feedbackSetFor(uint64_t key) const
{
    return key & (feedbackSets - 1);
}

uint64_t
DirectQualityGate::feedbackTagFor(uint64_t key) const
{
    return (key >> feedbackSetBits) & feedbackTagMask;
}

unsigned
DirectQualityGate::findQuality(unsigned set, uint64_t tag, uint8_t kind) const
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
    if (cfg.qualityWays == 1)
        return 0;
    if (cfg.qualityWays == 2)
        return qualityPLRU[set] & 1;
    const uint8_t state = qualityPLRU[set] & 7;
    if ((state & 1) == 0)
        return (state & 2) == 0 ? 0 : 1;
    return (state & 4) == 0 ? 2 : 3;
}

void
DirectQualityGate::touchQuality(unsigned set, unsigned way)
{
    if (cfg.qualityWays == 1)
        return;
    if (cfg.qualityWays == 2) {
        qualityPLRU[set] = way == 0 ? 1 : 0;
        return;
    }
    uint8_t &state = qualityPLRU[set];
    if (way == 0) {
        state |= 1;
        state |= 2;
    } else if (way == 1) {
        state |= 1;
        state &= ~2;
    } else if (way == 2) {
        state &= ~1;
        state |= 4;
    } else {
        state &= ~1;
        state &= ~4;
    }
}

unsigned
DirectQualityGate::allocateQuality(unsigned set, uint64_t tag, uint8_t kind)
{
    const unsigned way = qualityVictim(set);
    auto &entry = quality[set * cfg.qualityWays + way];
    entry = QualityEntry();
    entry.valid = true;
    entry.tag = tag;
    entry.kind = kind;
    touchQuality(set, way);
    return way;
}

unsigned
DirectQualityGate::findFeedback(unsigned set, uint64_t tag) const
{
    const unsigned base = set * cfg.feedbackWays;
    for (unsigned way = 0; way < cfg.feedbackWays; ++way) {
        const auto &entry = feedback[base + way];
        if (entry.valid && entry.tag == tag)
            return way;
    }
    return cfg.feedbackWays;
}

unsigned
DirectQualityGate::feedbackVictim(unsigned set)
{
    const unsigned base = set * cfg.feedbackWays;
    for (unsigned way = 0; way < cfg.feedbackWays; ++way)
        if (!feedback[base + way].valid)
            return way;
    const unsigned way = feedbackNextVictim[set];
    feedbackNextVictim[set] = (way + 1) % cfg.feedbackWays;
    return way;
}

unsigned
DirectQualityGate::allocateFeedback(unsigned set)
{
    const unsigned way = feedbackVictim(set);
    auto &entry = feedback[set * cfg.feedbackWays + way];
    if (entry.valid) {
        ++feedbackConflictCount;
        ++feedbackReplacementCount;
        assert(outstandingCount > 0);
        --outstandingCount;
        ++unknownDropCount;
    }
    entry = FeedbackEntry();
    entry.valid = true;
    return way;
}

bool
DirectQualityGate::sample(Addr pc, uint8_t kind, Addr triggerLine, unsigned period, uint64_t salt) const
{
    if (period == 0)
        return false;
    const uint64_t pcSignature = mix((pc >> 1) ^ (uint64_t(kind) * UINT64_C(0x9e3779b97f4a7c15)));
    const uint64_t signature = mix(pcSignature ^ triggerLine ^ salt);
    return isPowerOf2(period) ? (signature & (period - 1)) == 0 : (signature % period) == 0;
}

unsigned
DirectQualityGate::blockPeriod(const QualityEntry &entry) const
{
    const uint64_t strict = uint64_t(cfg.strictUnusedPerUseful) * entry.useful + cfg.strictBlockGuard;
    return entry.unused >= strict ? cfg.blockProbePeriod : cfg.borderlineBlockProbePeriod;
}

bool
DirectQualityGate::shouldBlock(const QualityEntry &entry) const
{
    return entry.unused >= uint64_t(cfg.unusedPerUseful) * entry.useful + cfg.blockGuard;
}

bool
DirectQualityGate::shouldReopen(const QualityEntry &entry) const
{
    return uint64_t(entry.unused) + cfg.reopenGuard <= uint64_t(cfg.reopenUnusedPerUseful) * entry.useful;
}

void
DirectQualityGate::transition(QualityEntry &entry, State next)
{
    if (entry.state != next) {
        entry.state = next;
        ++stateTransitionCount;
    }
}

void
DirectQualityGate::applyOutcome(QualityEntry &entry, bool isUseful)
{
    if (isUseful) {
        ++entry.useful;
        ++usefulCount;
    } else {
        ++entry.unused;
        ++unusedCount;
    }
    ++entry.resolvedSinceDecay;

    const auto updateState = [this](QualityEntry &target) {
        const uint64_t samples = uint64_t(target.useful) + target.unused;
        if (samples < cfg.minSamples && target.state == State::Observe)
            return;
        if (target.state == State::Block) {
            if (shouldReopen(target))
                transition(target, State::Open);
        } else {
            transition(target, shouldBlock(target) ? State::Block : State::Open);
        }
    };
    updateState(entry);
    if (cfg.decayPeriod != 0 && entry.resolvedSinceDecay >= cfg.decayPeriod) {
        entry.useful >>= 1;
        entry.unused >>= 1;
        entry.resolvedSinceDecay = 0;
        updateState(entry);
    }
}

void
DirectQualityGate::resolveFeedback(unsigned feedbackIndex, bool isUseful)
{
    auto &fb = feedback[feedbackIndex];
    if (!fb.valid)
        return;
    const unsigned way = findQuality(fb.qualitySet, fb.qualityTag, fb.qualityKind);
    if (way == cfg.qualityWays) {
        ++unknownDropCount;
        ++orphanOutcomeCount;
    } else {
        applyOutcome(quality[fb.qualitySet * cfg.qualityWays + way], isUseful);
    }
    fb.valid = false;
    assert(outstandingCount > 0);
    --outstandingCount;
}

void
DirectQualityGate::expireFeedback(unsigned feedbackIndex)
{
    auto &entry = feedback[feedbackIndex];
    if (!entry.valid || epochDistance(entry.issueEpoch) < cfg.epochTimeout)
        return;
    ++feedbackExpiryCount;
    resolveFeedback(feedbackIndex, false);
}

DirectQualityGate::Decision
DirectQualityGate::admit(Addr pc, uint8_t kind, Addr triggerLine, Addr candidateLine)
{
    fatal_if(kind > 3, "Direct-quality kind must fit in 2 bits\n");
    Decision decision;
    ++candidateCount;

    const unsigned set = qualitySetFor(pc, kind);
    const uint64_t tag = qualityTagFor(pc, kind);
    unsigned way = findQuality(set, tag, kind);
    if (way == cfg.qualityWays)
        way = allocateQuality(set, tag, kind);
    auto &entry = quality[set * cfg.qualityWays + way];
    touchQuality(set, way);
    decision.state = entry.state;

    unsigned period = 0;
    switch (entry.state) {
        case State::Observe:
            period = cfg.observeSamplePeriod;
            break;
        case State::Open:
            period = cfg.openSamplePeriod;
            break;
        case State::Block:
            period = blockPeriod(entry);
            decision.allowed = sample(pc, kind, triggerLine, period, UINT64_C(0xb10c));
            break;
    }
    if (entry.state == State::Observe) {
        decision.sampled = sample(pc, kind, triggerLine, period, UINT64_C(0x0b5e));
    } else if (entry.state == State::Open) {
        decision.sampled = sample(pc, kind, triggerLine, period, UINT64_C(0x5a6d));
    } else {
        decision.sampled = decision.allowed;
    }
    if (!decision.allowed) {
        ++suppressedCount;
        return decision;
    }
    ++allowedCount;
    if (!decision.sampled)
        return decision;

    const uint64_t line = candidateLine >> CacheLineBits;
    bool nonCanonical = false;
    const uint64_t key = feedbackKeyFor(line, &nonCanonical);
    if (nonCanonical)
        ++nonCanonicalFeedbackCandidateCount;
    const unsigned feedbackSet = feedbackSetFor(key);
    const uint64_t feedbackTag = feedbackTagFor(key);
    const unsigned existing = findFeedback(feedbackSet, feedbackTag);
    if (existing != cfg.feedbackWays) {
        ++feedbackCoalescedCount;
        return decision;
    }
    const unsigned feedbackWay = allocateFeedback(feedbackSet);
    auto &feedbackEntry = feedback[feedbackSet * cfg.feedbackWays + feedbackWay];
    feedbackEntry.tag = feedbackTag;
    feedbackEntry.qualitySet = set;
    feedbackEntry.qualityTag = entry.tag;
    feedbackEntry.qualityKind = kind;
    feedbackEntry.issueEpoch = currentEpoch();
    ++sampledCount;
    ++outstandingCount;
    peakOutstandingCount = std::max(peakOutstandingCount, outstandingCount);
    decision.feedbackInserted = true;
    return decision;
}

void
DirectQualityGate::observeDemand(Addr demandLine)
{
    ++demandAge;
    const uint64_t line = demandLine >> CacheLineBits;
    bool nonCanonical = false;
    const uint64_t key = feedbackKeyFor(line, &nonCanonical);
    if (nonCanonical)
        ++nonCanonicalFeedbackDemandCount;
    const unsigned set = feedbackSetFor(key);
    const unsigned way = findFeedback(set, feedbackTagFor(key));
    if (way != cfg.feedbackWays)
        resolveFeedback(set * cfg.feedbackWays + way, true);
    const unsigned sweepIndex = feedbackSweepPointer;
    feedbackSweepPointer = (feedbackSweepPointer + 1) % cfg.feedbackEntries;
    expireFeedback(sweepIndex);
}

uint8_t
DirectQualityGate::currentEpoch() const
{
    const uint8_t mask = static_cast<uint8_t>((1U << cfg.epochBits) - 1);
    return static_cast<uint8_t>((demandAge >> cfg.epochShift) & mask);
}

uint8_t
DirectQualityGate::epochDistance(uint8_t issueEpoch) const
{
    const uint8_t mask = static_cast<uint8_t>((1U << cfg.epochBits) - 1);
    return static_cast<uint8_t>((currentEpoch() - issueEpoch) & mask);
}

DirectQualityGate::State
DirectQualityGate::state(Addr pc, uint8_t kind) const
{
    const unsigned set = qualitySetFor(pc, kind);
    const unsigned way = findQuality(set, qualityTagFor(pc, kind), kind);
    return way == cfg.qualityWays ? State::Observe : quality[set * cfg.qualityWays + way].state;
}

bool
DirectQualityGate::configMatches(const DirectQualityGate &other) const
{
    const auto &rhs = other.cfg;
    return cfg.qualityEntries == rhs.qualityEntries && cfg.qualityWays == rhs.qualityWays &&
           cfg.qualityTagBits == rhs.qualityTagBits && cfg.feedbackEntries == rhs.feedbackEntries &&
           cfg.feedbackWays == rhs.feedbackWays && cfg.feedbackTagBits == rhs.feedbackTagBits &&
           cfg.horizon == rhs.horizon && cfg.minSamples == rhs.minSamples &&
           cfg.observeSamplePeriod == rhs.observeSamplePeriod && cfg.openSamplePeriod == rhs.openSamplePeriod &&
           cfg.blockProbePeriod == rhs.blockProbePeriod &&
           cfg.borderlineBlockProbePeriod == rhs.borderlineBlockProbePeriod &&
           cfg.unusedPerUseful == rhs.unusedPerUseful && cfg.blockGuard == rhs.blockGuard &&
           cfg.strictUnusedPerUseful == rhs.strictUnusedPerUseful && cfg.strictBlockGuard == rhs.strictBlockGuard &&
           cfg.reopenUnusedPerUseful == rhs.reopenUnusedPerUseful && cfg.reopenGuard == rhs.reopenGuard &&
           cfg.decayPeriod == rhs.decayPeriod && cfg.epochBits == rhs.epochBits && cfg.epochShift == rhs.epochShift &&
           cfg.epochTimeout == rhs.epochTimeout;
}

}  // namespace prefetch
}  // namespace gem5
