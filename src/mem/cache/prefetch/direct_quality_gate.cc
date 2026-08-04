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
    assert(cfg.qualityWays > 0 && cfg.qualityWays <= MaxWays);
    assert(cfg.feedbackWays > 0 && cfg.feedbackWays <= MaxWays);
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
    auto &entry = feedback[set * cfg.feedbackWays + way];
    if (entry.valid)
        ++feedbackConflictCount;
    entry = FeedbackEntry();
    entry.valid = true;
    return way;
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
    decision.state = entry.state;
    if (entry.state == State::Block) {
        const unsigned period = (entry.unused >= cfg.strictUnusedPerUseful *
                                 entry.useful + cfg.strictBlockGuard) ?
            cfg.blockProbePeriod : cfg.borderlineBlockProbePeriod;
        decision.allowed = period != 0 && (entry.issued % period) == 0;
        decision.sampled = decision.allowed &&
            (period == 0 || (entry.issued % period) == 0);
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
DirectQualityGate::recordIssued(Addr line, Addr pc, uint8_t kind,
                                 const Decision &decision)
{
    if (!decision.sampled)
        return;
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
    auto &entry = feedback[set * cfg.feedbackWays + way];
    entry.valid = true;
    entry.line = line;
    entry.qualitySet = decision.set;
    entry.qualityWay = decision.way;
    entry.generation = quality[decision.set * cfg.qualityWays + decision.way].generation;
    entry.kind = kind;
    entry.issueAge = demandAge;
    ++quality[decision.set * cfg.qualityWays + decision.way].sampled;
    ++sampledCount;
    (void)pc;
}

void
DirectQualityGate::applyOutcome(QualityEntry &entry, bool isUseful)
{
    if (isUseful)
        ++entry.useful;
    else
        ++entry.unused;
    if (isUseful)
        ++usefulCount;
    else
        ++unusedCount;

    if (entry.useful + entry.unused < cfg.minSamples)
        return;
    const bool shouldBlock = entry.unused >=
        cfg.unusedPerUseful * entry.useful + cfg.blockGuard;
    if (entry.state == State::Observe || entry.state == State::Open) {
        const State next = shouldBlock ? State::Block : State::Open;
        if (next != entry.state) {
            entry.state = next;
            ++stateTransitionCount;
        }
    } else if (entry.state == State::Block &&
               entry.unused < cfg.reopenUnusedPerUseful * entry.useful +
                   cfg.reopenGuard && cfg.reopenProbePeriod != 0 &&
               entry.issued % cfg.reopenProbePeriod == 0) {
        entry.state = State::Open;
        ++stateTransitionCount;
    }
}

DirectQualityGate::Outcome
DirectQualityGate::resolve(Addr line, bool isUseful)
{
    Outcome result;
    const unsigned set = feedbackSetFor(line);
    const unsigned way = findFeedback(set, line);
    if (way == cfg.feedbackWays) {
        ++orphanOutcomeCount;
        result.conflict = true;
        return result;
    }
    auto &fb = feedback[set * cfg.feedbackWays + way];
    if (demandAge - fb.issueAge >= cfg.horizon) {
        const unsigned qbase = fb.qualitySet * cfg.qualityWays;
        auto &entry = quality[qbase + fb.qualityWay];
        if (entry.valid && entry.generation == fb.generation)
            applyOutcome(entry, false);
        fb.valid = false;
        ++feedbackExpiryCount;
        result.expired = true;
        return result;
    }
    const unsigned qbase = fb.qualitySet * cfg.qualityWays;
    auto &entry = quality[qbase + fb.qualityWay];
    if (!entry.valid || entry.generation != fb.generation ||
        entry.kind != fb.kind) {
        fb.valid = false;
        ++orphanOutcomeCount;
        result.conflict = true;
        return result;
    }
    applyOutcome(entry, isUseful);
    result.resolved = true;
    result.useful = isUseful;
    result.state = entry.state;
    fb.valid = false;
    return result;
}

void
DirectQualityGate::advanceDemand()
{
    ++demandAge;
    for (unsigned set = 0; set < feedbackSets; ++set) {
        const unsigned base = set * cfg.feedbackWays;
        for (unsigned way = 0; way < cfg.feedbackWays; ++way) {
            auto &fb = feedback[base + way];
            if (!fb.valid || demandAge - fb.issueAge < cfg.horizon)
                continue;

            auto &entry = quality[fb.qualitySet * cfg.qualityWays +
                                  fb.qualityWay];
            if (entry.valid && entry.generation == fb.generation &&
                entry.kind == fb.kind) {
                applyOutcome(entry, false);
            }
            fb.valid = false;
            ++feedbackExpiryCount;
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
