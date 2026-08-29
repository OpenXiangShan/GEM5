#include "mem/cache/prefetch/direct_quality_gate.hh"

#include <algorithm>
#include <cassert>

#include "base/logging.hh"

namespace gem5
{
namespace prefetch
{

namespace
{

[[maybe_unused]] bool
isPowerOf2(unsigned value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

}  // anonymous namespace

DirectQualityGate::Config
DirectQualityGate::Config::bopCqf14E6T30()
{
    Config config;
    config.profile = Profile::BopCqf14E6T30;
    config.qualityEntries = 256;
    config.qualityWays = 4;
    config.qualityTagBits = 8;
    config.feedbackEntries = 256;
    config.feedbackWays = 4;
    config.feedbackTagBits = CompactFeedbackTagBits;
    config.horizon = 2048;
    config.minSamples = 32;
    config.observeSamplePeriod = 16;
    config.openSamplePeriod = 16;
    config.blockProbePeriod = 64;
    config.borderlineBlockProbePeriod = 8;
    config.unusedPerUseful = 10;
    config.blockGuard = 4;
    config.strictUnusedPerUseful = 20;
    config.strictBlockGuard = 4;
    config.reopenUnusedPerUseful = 10;
    config.reopenGuard = 4;
    config.reopenProbePeriod = 64;
    config.reopenConfirmSamples = 0;
    config.decayPeriod = 64;
    config.compactEpochBits = 6;
    config.compactEpochShift = 6;
    config.compactEpochTimeout = 30;
    return config;
}

DirectQualityGate::Config
DirectQualityGate::Config::bopCqfDse()
{
    Config config = bopCqf14E6T30();
    config.profile = Profile::BopCqfDse;
    return config;
}

const char *
DirectQualityGate::Config::profileName() const
{
    switch (profile) {
      case Profile::BopCqf14E6T30:
        return "BOP-CQF14E6T30";
      case Profile::BopCqfDse:
        return "BOP-CQF-DSE";
      case Profile::Legacy:
        return "legacy";
    }
    return "unknown";
}

const char *
DirectQualityGate::Config::qualityHashLayoutName() const
{
    return profile != Profile::Legacy ? "xor_fold" : "mix64";
}

const char *
DirectQualityGate::Config::feedbackOwnerLayoutName() const
{
    return profile != Profile::Legacy ? "quality_key" : "slot_generation";
}

const char *
DirectQualityGate::Config::feedbackAddressLayoutName() const
{
    return profile != Profile::Legacy ? "sv48_truncated_tag" : "sv48_reversible_set_tag";
}

const char *
DirectQualityGate::Config::feedbackExpiryModeName() const
{
    return profile != Profile::Legacy ? "round_robin" : "heap";
}

const char *
DirectQualityGate::Config::feedbackAgeEncodingName() const
{
    if (profile == Profile::Legacy)
        return "full";
    switch (compactEpochBits) {
      case 5:
        return "epoch5";
      case 6:
        return "epoch6";
      case 7:
        return "epoch7";
      default:
        return "invalid_epoch";
    }
}

unsigned
DirectQualityGate::Config::feedbackEpochBits() const
{
    return profile != Profile::Legacy ? compactEpochBits : 0;
}

unsigned
DirectQualityGate::Config::feedbackEpochShift() const
{
    return profile != Profile::Legacy ? compactEpochShift : 0;
}

unsigned
DirectQualityGate::Config::feedbackEpochTimeout() const
{
    return profile != Profile::Legacy ? compactEpochTimeout : 0;
}

DirectQualityGate::DirectQualityGate() : DirectQualityGate(Config()) {}

DirectQualityGate::DirectQualityGate(const Config &config)
    : cfg(config),
      qualitySets(config.qualityEntries / config.qualityWays),
      feedbackSets(config.feedbackEntries / config.feedbackWays),
      qualitySetBits(0),
      feedbackSetBits(0),
      qualityTagMask(config.qualityTagBits >= 63 ? ~Addr(0) : ((Addr(1) << config.qualityTagBits) - 1)),
      feedbackTagMask(config.feedbackTagBits >= 64 ? ~uint64_t(0) :
                      ((uint64_t(1) << config.feedbackTagBits) - 1))
{
    assert(cfg.qualityWays == 1 || cfg.qualityWays == 2 || cfg.qualityWays == 4);
    assert(cfg.feedbackWays > 0 && cfg.feedbackWays <= MaxFeedbackWays);
    assert(cfg.qualityEntries > 0 && cfg.qualityEntries <= MaxQualityEntries);
    assert(cfg.feedbackEntries > 0 && cfg.feedbackEntries <= MaxFeedbackEntries);
    fatal_if(cfg.qualityTagBits > 16, "Direct-quality compact entries support at most 16 tag bits\n");
    fatal_if(cfg.feedbackTagBits == 0 || cfg.feedbackTagBits > 36,
             "Direct-quality feedback tags must contain 1 to 36 bits\n");
    fatal_if(cfg.horizon >= AgeHalfRange, "Direct-quality horizon must be less than %u for compact ages\n",
             AgeHalfRange);
    fatal_if(cfg.blockProbePeriod > UINT8_MAX || cfg.borderlineBlockProbePeriod > UINT8_MAX,
             "Direct-quality compact recovery probe periods must fit in 8 bits\n");
    fatal_if(cfg.reopenConfirmSamples > UINT16_MAX,
             "Direct-quality compact recovery confirmation must fit in 16 bits\n");
    assert((cfg.qualityEntries % cfg.qualityWays) == 0);
    assert((cfg.feedbackEntries % cfg.feedbackWays) == 0);
    assert(isPowerOf2(cfg.qualityEntries));
    assert(isPowerOf2(cfg.feedbackEntries));
    assert(isPowerOf2(qualitySets));
    assert(isPowerOf2(feedbackSets));
    assert(cfg.decayPeriod == 0 || isPowerOf2(cfg.decayPeriod));
    if (cfg.profile == Profile::BopCqf14E6T30) {
        fatal_if(cfg.qualityEntries != 256 || cfg.qualityWays != 4 ||
                 cfg.qualityTagBits != 8 || cfg.feedbackEntries != 256 ||
                 cfg.feedbackWays != 4 || cfg.feedbackTagBits != CompactFeedbackTagBits ||
                 cfg.horizon != 2048 || cfg.minSamples != 32 ||
                 cfg.observeSamplePeriod != 16 || cfg.openSamplePeriod != 16 ||
                 cfg.blockProbePeriod != 64 || cfg.borderlineBlockProbePeriod != 8 ||
                 cfg.unusedPerUseful != 10 || cfg.blockGuard != 4 ||
                 cfg.strictUnusedPerUseful != 20 || cfg.strictBlockGuard != 4 ||
                 cfg.reopenUnusedPerUseful != 10 || cfg.reopenGuard != 4 ||
                 cfg.reopenProbePeriod != 64 || cfg.reopenConfirmSamples != 0 ||
                 cfg.decayPeriod != 64 || cfg.compactEpochBits != 6 ||
                 cfg.compactEpochShift != 6 || cfg.compactEpochTimeout != 30,
                 "BOP-CQF14E6T30 requires its fixed certified configuration\n");
    }
    if (cfg.profile == Profile::BopCqfDse) {
        const bool legal_quality_entries = cfg.qualityEntries == 64 ||
            cfg.qualityEntries == 128 || cfg.qualityEntries == 256;
        const bool legal_feedback_entries = cfg.feedbackEntries == 64 ||
            cfg.feedbackEntries == 128 || cfg.feedbackEntries == 256;
        fatal_if(!legal_quality_entries || !legal_feedback_entries ||
                 cfg.qualityWays != 4 || cfg.feedbackWays != 4 ||
                 cfg.qualityTagBits != 8 ||
                 cfg.feedbackTagBits != CompactFeedbackTagBits,
                 "BOP-CQF-DSE requires 64/128/256-entry 4-way Quality and "
                 "Feedback tables with tag8/tag14\n");
        fatal_if(cfg.compactEpochBits < 5 || cfg.compactEpochBits > 7 ||
                 cfg.compactEpochShift < 5 || cfg.compactEpochShift > 7 ||
                 cfg.compactEpochBits + cfg.compactEpochShift != 12,
                 "BOP-CQF-DSE requires E5/S7, E6/S6, or E7/S5 feedback age encoding\n");
        const unsigned half_range = 1U << (cfg.compactEpochBits - 1);
        fatal_if(cfg.compactEpochTimeout == 0 ||
                 cfg.compactEpochTimeout >= half_range,
                 "BOP-CQF-DSE epoch timeout must be in [1, %u)\n", half_range);
        fatal_if(uint64_t(cfg.compactEpochTimeout) *
                     (uint64_t(1) << cfg.compactEpochShift) +
                     cfg.feedbackEntries / 2 > cfg.horizon,
                 "BOP-CQF-DSE feedback timeout exceeds the configured Horizon\n");
        fatal_if(cfg.observeSamplePeriod == 0 || cfg.openSamplePeriod == 0 ||
                 cfg.blockProbePeriod == 0 || cfg.borderlineBlockProbePeriod == 0 ||
                 cfg.reopenProbePeriod == 0 || cfg.minSamples == 0,
                 "BOP-CQF-DSE sample and probe periods must be non-zero\n");
        fatal_if(cfg.strictUnusedPerUseful < cfg.unusedPerUseful ||
                 cfg.reopenUnusedPerUseful > cfg.unusedPerUseful,
                 "BOP-CQF-DSE requires strict ratio >= base ratio >= reopen ratio\n");
    }
    while ((1U << qualitySetBits) < qualitySets)
        ++qualitySetBits;
    while ((1U << feedbackSetBits) < feedbackSets)
        ++feedbackSetBits;
}

void
DirectQualityGate::setTraceSink(TraceSink *sink)
{
    traceSink = sink;
    if (traceSink) {
        feedbackTrace.assign(cfg.feedbackEntries, FeedbackTraceInfo());
        traceSink->directQualityTraceConfig(cfg);
    } else {
        feedbackTrace.clear();
    }
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
    if (compactProfile()) {
        uint64_t kindMix = 0;
        switch (kind) {
          case 0:
            break;
          case 1:
            kindMix = 0x9E3779B97F4A7C15ULL;
            break;
          case 2:
            kindMix = 0x3C6EF372FE94F82AULL;
            break;
          default:
            fatal("BOP-CQF direct-quality profiles do not support kind %u\n", kind);
        }
        uint64_t signature = pc >> 1;
        signature ^= signature >> 7;
        signature ^= signature >> 13;
        signature ^= signature >> 27;
        signature ^= kindMix;
        signature ^= signature >> 11;
        signature ^= signature >> 23;
        return signature;
    }
    return mix64((pc >> 1) ^ (uint64_t(kind) * 0x9E3779B97F4A7C15ULL));
}

uint64_t
DirectQualityGate::samplingSignature(Addr pc, uint8_t kind)
{
    return mix64((pc >> 1) ^ (uint64_t(kind) * 0x9E3779B97F4A7C15ULL));
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

uint64_t
DirectQualityGate::feedbackKeyFor(uint64_t line)
{
    const uint64_t compactLine = line & FeedbackLineMask;
    const uint64_t canonicalLine = compactLine & FeedbackLineSignBit
        ? compactLine | (HostLineMask & ~FeedbackLineMask) : compactLine;
    fatal_if(line != canonicalLine,
             "Direct-quality compact feedback layout requires a canonical Sv48 cache-line address\n");

    // Each fixed-width xorshift is bijective over the 42-bit line domain.
    // The alternating directions mix both low stride bits and high address
    // bits into the set field. Consequently, set bits plus the stored upper
    // tag reproduce an exact cache-line identity rather than a short
    // signature.
    uint64_t key = compactLine;
    key ^= key >> 17;
    key ^= (key << 13) & FeedbackLineMask;
    key ^= key >> 6;
    key ^= (key << 7) & FeedbackLineMask;
    key ^= key >> 11;
    return key & FeedbackLineMask;
}

unsigned
DirectQualityGate::feedbackSetForKey(uint64_t key) const
{
    return key & (feedbackSets - 1);
}

uint64_t
DirectQualityGate::feedbackTagForKey(uint64_t key) const
{
    return (key >> feedbackSetBits) & feedbackTagMask;
}

unsigned
DirectQualityGate::findQuality(unsigned set, Addr tag, uint8_t kind) const
{
    const unsigned base = set * cfg.qualityWays;
    for (unsigned way = 0; way < cfg.qualityWays; ++way) {
        const auto &entry = quality[base + way];
        if (entry.isValid() && entry.tag == tag && entry.kindValue() == kind)
            return way;
    }
    return cfg.qualityWays;
}

unsigned
DirectQualityGate::qualityVictim(unsigned set) const
{
    const unsigned base = set * cfg.qualityWays;
    for (unsigned way = 0; way < cfg.qualityWays; ++way) {
        if (!quality[base + way].isValid())
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
    entry.setValid(true);
    entry.tag = static_cast<uint16_t>(tag);
    entry.setKind(kind);
    entry.generation = nextGeneration;
    entry.setState(State::Observe);
    return way;
}

unsigned
DirectQualityGate::findFeedback(unsigned set, uint64_t tag) const
{
    const unsigned base = set * cfg.feedbackWays;
    for (unsigned way = 0; way < cfg.feedbackWays; ++way) {
        if (feedback[base + way].valid && feedback[base + way].tag == tag)
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
DirectQualityGate::sample(Addr pc, uint8_t kind, Addr trigger_line, unsigned period, uint64_t salt) const
{
    assert(isPowerOf2(period));
    // Sampling intentionally retains the established mix64 stream across
    // Quality-key experiments. Reusing qualitySignature() here would change
    // the sample population when CQF selects its XOR-fold Quality mapping.
    const uint64_t signature = samplingSignature(pc, kind) ^ trigger_line ^ salt;
    return (mix64(signature) & (period - 1)) == 0;
}

DirectQualityGate::Decision
DirectQualityGate::admit(Addr pc, uint8_t kind, Addr trigger_line, Addr candidate_line)
{
    fatal_if(kind > 3, "Direct-quality compact kind must fit in 2 bits\n");
    const unsigned set = qualitySetFor(pc, kind);
    const Addr tag = qualityTagFor(pc, kind);
    unsigned way = findQuality(set, tag, kind);
    if (way == cfg.qualityWays)
        way = allocateQuality(set, tag, kind);
    auto &entry = quality[set * cfg.qualityWays + way];
    touchQuality(set, way);
    ++candidateCount;

    Decision decision;
    decision.set = set;
    decision.way = way;
    decision.generation = entry.generation;
    decision.state = entry.stateValue();
    if (decision.state == State::Block) {
        decision.allowed = sample(pc, kind, trigger_line, blockProbePeriod(entry), 0xB10C);
        decision.sampled = decision.allowed;
    } else if (decision.state == State::Recover) {
        decision.allowed = sample(pc, kind, trigger_line, entry.recoveryProbePeriod, 0x5EC0);
        decision.sampled = decision.allowed;
    } else if (decision.state == State::Open) {
        decision.sampled = sample(pc, kind, trigger_line, cfg.openSamplePeriod, 0x5A6D);
    } else {
        decision.sampled = sample(pc, kind, trigger_line, cfg.observeSamplePeriod, 0x0B5E);
    }

    if (decision.allowed) {
        ++allowedCount;
    } else {
        ++suppressedCount;
    }

    if (traceSink) {
        traceSink->directQualityTraceCandidate(++nextTraceEventSequence, pc, kind, trigger_line, candidate_line,
                                               decision.state, decision.allowed, decision.sampled);
    }
    if (decision.sampled) {
        ++sampleSelectedCount;
        decision.feedbackInserted = recordCandidate(candidate_line, kind, set, way, entry.generation) != 0;
    }
    return decision;
}

uint64_t
DirectQualityGate::recordCandidate(Addr line, uint8_t kind, unsigned quality_set, unsigned quality_way,
                                   uint8_t quality_generation)
{
    assert(quality_set < qualitySets);
    assert(quality_way < cfg.qualityWays);
    auto &qualityEntry = quality[quality_set * cfg.qualityWays + quality_way];
    assert(qualityEntry.isValid());
    assert(qualityEntry.generation == quality_generation);
    assert(qualityEntry.kindValue() == kind);

    const uint64_t lineNumber = compactLine(line);
    const uint64_t key = feedbackKeyFor(lineNumber);
    const unsigned set = feedbackSetForKey(key);
    const uint64_t tag = feedbackTagForKey(key);
    if (findFeedback(set, tag) != cfg.feedbackWays) {
        ++feedbackCoalescedCount;
        return 0;
    }

    const unsigned way = allocateFeedback(set);
    const unsigned index = feedbackIndex(set, way);
    auto &entry = feedback[index];
    entry.valid = true;
    entry.tag = tag;
    entry.qualityIndex = static_cast<uint8_t>(quality_set * cfg.qualityWays + quality_way);
    entry.qualityGeneration = quality_generation;
    entry.recoveryGeneration = qualityEntry.recoveryGeneration;
    entry.issueAge = compactAge();
    entry.qualitySet = static_cast<uint8_t>(quality_set);
    entry.qualityTag = qualityEntry.tag;
    entry.qualityKind = kind;
    entry.issueEpoch = compactEpoch();
    if (traceSink) {
        auto &trace = feedbackTrace[index];
        trace.id = ++nextFeedbackId;
        trace.issueAge = demandAge;
        trace.line = lineNumber;
    }
    if (!compactProfile())
        insertExpiry(index);
    ++outstandingCount;
    peakOutstandingCount = std::max(peakOutstandingCount, outstandingCount);
    ++sampledCount;
    if (traceSink) {
        auto &trace = feedbackTrace[index];
        traceSink->directQualityTraceIssue(++nextTraceEventSequence, trace.id, trace.issueAge, expandLine(lineNumber),
                                           kind);
        return trace.id;
    }
    return uint64_t(index) + 1;
}

void
DirectQualityGate::invalidateFeedback(unsigned feedback_index)
{
    auto &entry = feedback[feedback_index];
    assert(entry.valid);
    if (!compactProfile())
        removeExpiry(feedback_index);
    entry.valid = false;
    assert(outstandingCount > 0);
    --outstandingCount;
}

void
DirectQualityGate::retireUnknown(unsigned feedback_index, TraceOutcome outcome)
{
    traceOutcome(feedback_index, outcome);
    invalidateFeedback(feedback_index);
    ++unknownDropCount;
}

void
DirectQualityGate::traceOutcome(unsigned feedback_index, TraceOutcome outcome)
{
    const auto &entry = feedback[feedback_index];
    if (traceSink) {
        const auto &trace = feedbackTrace[feedback_index];
        traceSink->directQualityTraceOutcome(++nextTraceEventSequence, trace.id, demandAge, expandLine(trace.line),
                                             outcome);
    }
}

unsigned
DirectQualityGate::blockProbePeriod(const QualityEntry &entry) const
{
    const uint64_t strictLimit = uint64_t(cfg.strictUnusedPerUseful) * entry.useful + cfg.strictBlockGuard;
    return entry.unused >= strictLimit ? cfg.blockProbePeriod : cfg.borderlineBlockProbePeriod;
}

bool
DirectQualityGate::shouldBlock(const QualityEntry &entry) const
{
    const uint64_t blockLimit = uint64_t(cfg.unusedPerUseful) * entry.useful + cfg.blockGuard;
    return entry.unused >= blockLimit;
}

bool
DirectQualityGate::meetsReopen(const QualityEntry &entry) const
{
    const uint64_t reopenLimit = uint64_t(cfg.reopenUnusedPerUseful) * entry.useful;
    return reopenLimit >= cfg.reopenGuard && entry.unused <= reopenLimit - cfg.reopenGuard;
}

void
DirectQualityGate::transitionTo(QualityEntry &entry, State next)
{
    if (entry.stateValue() == next)
        return;

    if (entry.stateValue() == State::Block && next == State::Recover) {
        ++blockToRecoverTransitionCount;
    } else if (entry.stateValue() == State::Recover && next == State::Open) {
        ++recoverToOpenTransitionCount;
    } else if (entry.stateValue() == State::Recover && next == State::Block) {
        ++recoverToBlockTransitionCount;
    }
    entry.setState(next);
    ++stateTransitionCount;
}

void
DirectQualityGate::updateState(QualityEntry &entry, unsigned previous_block_probe_period)
{
    const uint64_t samples = uint64_t(entry.useful) + entry.unused;
    if (samples < cfg.minSamples && entry.stateValue() == State::Observe) {
        transitionTo(entry, State::Observe);
        return;
    }

    if (entry.stateValue() == State::Block) {
        if (!meetsReopen(entry))
            return;
        if (cfg.reopenConfirmSamples == 0) {
            transitionTo(entry, State::Open);
            return;
        }
        entry.recoverySamples = 0;
        ++entry.recoveryGeneration;
        entry.recoveryProbePeriod =
            previous_block_probe_period != 0 ? previous_block_probe_period : blockProbePeriod(entry);
        transitionTo(entry, State::Recover);
        return;
    }

    if (entry.stateValue() == State::Recover) {
        if (shouldBlock(entry)) {
            entry.recoverySamples = 0;
            transitionTo(entry, State::Block);
        } else if (entry.recoverySamples >= cfg.reopenConfirmSamples && meetsReopen(entry)) {
            transitionTo(entry, State::Open);
        }
        return;
    }

    transitionTo(entry, shouldBlock(entry) ? State::Block : State::Open);
}

void
DirectQualityGate::applyOutcome(QualityEntry &entry, uint16_t recovery_generation, bool isUseful)
{
    const State previousState = entry.stateValue();
    const unsigned previousBlockProbePeriod = blockProbePeriod(entry);

    if (isUseful) {
        ++entry.useful;
        ++usefulCount;
    } else {
        ++entry.unused;
        ++unusedCount;
    }
    if (previousState == State::Recover && recovery_generation == entry.recoveryGeneration) {
        ++entry.recoverySamples;
    }
    ++entry.resolvedSinceDecay;
    updateState(entry, previousBlockProbePeriod);

    if (cfg.decayPeriod != 0 && entry.resolvedSinceDecay >= cfg.decayPeriod) {
        entry.useful >>= 1;
        entry.unused >>= 1;
        entry.resolvedSinceDecay = 0;
        updateState(entry);
    }
}

bool
DirectQualityGate::resolveFeedback(unsigned feedback_index, bool isUseful, TraceOutcome outcome)
{
    auto &fb = feedback[feedback_index];
    assert(fb.valid);
    QualityEntry *entry = nullptr;
    uint16_t recoveryGeneration = fb.recoveryGeneration;
    if (compactProfile()) {
        const unsigned way = findQuality(fb.qualitySet, fb.qualityTag, fb.qualityKind);
        if (way != cfg.qualityWays)
            entry = &quality[fb.qualitySet * cfg.qualityWays + way];
    } else {
        auto &physical = quality[fb.qualityIndex];
        if (physical.isValid() && physical.generation == fb.qualityGeneration)
            entry = &physical;
    }
    if (!entry) {
        retireUnknown(feedback_index, TraceOutcome::UnknownOwnerReplaced);
        ++orphanOutcomeCount;
        return false;
    }
    applyOutcome(*entry, recoveryGeneration, isUseful);
    traceOutcome(feedback_index, outcome);
    invalidateFeedback(feedback_index);
    return true;
}

void
DirectQualityGate::observeDemand(Addr line)
{
    ++demandAge;
    if (traceSink) {
        traceSink->directQualityTraceDemand(++nextTraceEventSequence, demandAge, line);
    }
    if (!compactProfile())
        expireFeedback();
    const uint64_t lineNumber = compactLine(line);
    const uint64_t key = feedbackKeyFor(lineNumber);
    const unsigned set = feedbackSetForKey(key);
    const unsigned way = findFeedback(set, feedbackTagForKey(key));
    if (way != cfg.feedbackWays) {
        resolveFeedback(feedbackIndex(set, way), true, TraceOutcome::UsefulDemand);
    }
    if (compactProfile()) {
        const unsigned sweepIndex = feedbackSweepPointer;
        feedbackSweepPointer = (feedbackSweepPointer + 1) % cfg.feedbackEntries;
        expireCompactFeedback(sweepIndex);
    }
}

bool
DirectQualityGate::expiryBefore(unsigned lhs, unsigned rhs) const
{
    const auto &left = feedback[lhs];
    const auto &right = feedback[rhs];
    if (left.issueAge != right.issueAge)
        return ageDistance(left.issueAge) > ageDistance(right.issueAge);
    return traceSink ? feedbackTrace[lhs].id < feedbackTrace[rhs].id : lhs < rhs;
}

void
DirectQualityGate::restoreExpiryHeap(unsigned heap_index)
{
    if (heap_index != 0 && expiryBefore(expiryHeap[heap_index], expiryHeap[(heap_index - 1) / 2])) {
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
        if (right < expiryHeapSize && expiryBefore(expiryHeap[right], expiryHeap[left])) {
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
    assert(!compactProfile());
    while (expiryHeapSize != 0) {
        const unsigned feedback_index = expiryHeap[0];
        const auto &entry = feedback[feedback_index];
        assert(entry.valid);
        if (ageDistance(entry.issueAge) <= cfg.horizon)
            return;

        ++feedbackExpiryCount;
        if (resolveFeedback(feedback_index, false, TraceOutcome::UnusedExpiry)) {
            ++feedbackExpiryUnusedCount;
        }
    }
}

void
DirectQualityGate::expireCompactFeedback(unsigned feedback_index)
{
    assert(compactProfile());
    auto &entry = feedback[feedback_index];
    if (!entry.valid)
        return;
    if (compactEpochDistance(entry.issueEpoch) < cfg.compactEpochTimeout)
        return;

    ++feedbackExpiryCount;
    if (resolveFeedback(feedback_index, false, TraceOutcome::UnusedExpiry))
        ++feedbackExpiryUnusedCount;
}

DirectQualityGate::State
DirectQualityGate::state(Addr pc, uint8_t kind) const
{
    const unsigned set = qualitySetFor(pc, kind);
    const unsigned way = findQuality(set, qualityTagFor(pc, kind), kind);
    return way == cfg.qualityWays ? State::Observe : quality[set * cfg.qualityWays + way].stateValue();
}

}  // namespace prefetch
}  // namespace gem5
