#ifndef __MEM_CACHE_PREFETCH_DIRECT_QUALITY_GATE_HH__
#define __MEM_CACHE_PREFETCH_DIRECT_QUALITY_GATE_HH__

#include <array>
#include <cstdint>

#include "base/types.hh"

namespace gem5
{
namespace prefetch
{

/**
 * Bounded quality-feedback admission controller for raw BOP candidates.
 *
 * A sampled candidate is entered before any queue or local-filter decision.
 * A later demand for the same cache line resolves it as useful; entries that
 * remain unmatched for horizon demand events resolve as unused.  Replacement
 * is censored rather than treated as a negative result, so table pressure does
 * not bias the quality estimate.
 */
class DirectQualityGate
{
  public:
    enum class State : uint8_t { Observe, Open, Block };

    struct Config
    {
        unsigned qualityEntries = 256;
        unsigned qualityWays = 4;
        unsigned qualityTagBits = 8;
        unsigned feedbackEntries = 256;
        unsigned feedbackWays = 4;
        unsigned feedbackTagBits = 14;
        unsigned horizon = 2048;
        unsigned minSamples = 32;
        unsigned observeSamplePeriod = 16;
        unsigned openSamplePeriod = 16;
        unsigned blockProbePeriod = 64;
        unsigned borderlineBlockProbePeriod = 8;
        unsigned unusedPerUseful = 10;
        unsigned blockGuard = 4;
        unsigned strictUnusedPerUseful = 20;
        unsigned strictBlockGuard = 4;
        unsigned reopenUnusedPerUseful = 10;
        unsigned reopenGuard = 4;
        unsigned decayPeriod = 64;
        unsigned epochBits = 6;
        unsigned epochShift = 6;
        unsigned epochTimeout = 30;
    };

    struct Decision
    {
        bool allowed = true;
        bool sampled = false;
        bool feedbackInserted = false;
        State state = State::Observe;
    };

    DirectQualityGate();
    explicit DirectQualityGate(const Config &config);

    Decision admit(Addr pc, uint8_t kind, Addr triggerLine, Addr candidateLine);
    void observeDemand(Addr demandLine);
    State state(Addr pc, uint8_t kind) const;
    bool configMatches(const DirectQualityGate &other) const;

    uint64_t candidates() const { return candidateCount; }
    uint64_t allowed() const { return allowedCount; }
    uint64_t suppressed() const { return suppressedCount; }
    uint64_t sampled() const { return sampledCount; }
    uint64_t useful() const { return usefulCount; }
    uint64_t unused() const { return unusedCount; }
    uint64_t feedbackConflicts() const { return feedbackConflictCount; }
    uint64_t feedbackReplacements() const { return feedbackReplacementCount; }
    uint64_t feedbackCoalesced() const { return feedbackCoalescedCount; }
    uint64_t nonCanonicalFeedbackCandidates() const { return nonCanonicalFeedbackCandidateCount; }
    uint64_t nonCanonicalFeedbackDemands() const { return nonCanonicalFeedbackDemandCount; }
    uint64_t feedbackExpiries() const { return feedbackExpiryCount; }
    uint64_t unknownDrops() const { return unknownDropCount; }
    uint64_t orphanOutcomes() const { return orphanOutcomeCount; }
    uint64_t stateTransitions() const { return stateTransitionCount; }
    uint64_t peakOutstanding() const { return peakOutstandingCount; }

  private:
    static constexpr unsigned MaxQualityEntries = 256;
    static constexpr unsigned MaxFeedbackEntries = 4096;
    static constexpr unsigned CacheLineBits = 6;
    static constexpr unsigned FeedbackLineBits = 48 - CacheLineBits;
    static constexpr uint64_t FeedbackLineMask = (uint64_t(1) << FeedbackLineBits) - 1;
    static constexpr uint64_t FeedbackLineSignBit = uint64_t(1) << (FeedbackLineBits - 1);
    static constexpr unsigned HostLineBits = 64 - CacheLineBits;
    static constexpr uint64_t HostLineMask = (uint64_t(1) << HostLineBits) - 1;

    struct QualityEntry
    {
        bool valid = false;
        uint64_t tag = 0;
        uint8_t kind = 0;
        State state = State::Observe;
        uint16_t useful = 0;
        uint16_t unused = 0;
        uint16_t resolvedSinceDecay = 0;
    };

    struct FeedbackEntry
    {
        bool valid = false;
        uint16_t tag = 0;
        uint8_t qualitySet = 0;
        uint16_t qualityTag = 0;
        uint8_t qualityKind = 0;
        uint8_t issueEpoch = 0;
    };

    Config cfg;
    unsigned qualitySets = 0;
    unsigned feedbackSets = 0;
    unsigned qualitySetBits = 0;
    unsigned feedbackSetBits = 0;
    uint64_t qualityTagMask = 0;
    uint64_t feedbackTagMask = 0;

    std::array<QualityEntry, MaxQualityEntries> quality = {};
    std::array<uint8_t, MaxQualityEntries> qualityPLRU = {};
    std::array<FeedbackEntry, MaxFeedbackEntries> feedback = {};
    std::array<uint8_t, MaxFeedbackEntries> feedbackNextVictim = {};

    uint64_t demandAge = 0;
    unsigned feedbackSweepPointer = 0;
    uint64_t candidateCount = 0;
    uint64_t allowedCount = 0;
    uint64_t suppressedCount = 0;
    uint64_t sampledCount = 0;
    uint64_t usefulCount = 0;
    uint64_t unusedCount = 0;
    uint64_t feedbackConflictCount = 0;
    uint64_t feedbackReplacementCount = 0;
    uint64_t feedbackCoalescedCount = 0;
    uint64_t nonCanonicalFeedbackCandidateCount = 0;
    uint64_t nonCanonicalFeedbackDemandCount = 0;
    uint64_t feedbackExpiryCount = 0;
    uint64_t unknownDropCount = 0;
    uint64_t orphanOutcomeCount = 0;
    uint64_t stateTransitionCount = 0;
    uint64_t outstandingCount = 0;
    uint64_t peakOutstandingCount = 0;

    static bool isPowerOf2(unsigned value);
    static unsigned log2Of(unsigned value);
    static uint64_t mix(uint64_t value);

    unsigned qualitySetFor(Addr pc, uint8_t kind) const;
    uint64_t qualityTagFor(Addr pc, uint8_t kind) const;
    static uint64_t feedbackKeyFor(uint64_t line, bool *nonCanonical = nullptr);
    unsigned feedbackSetFor(uint64_t key) const;
    uint64_t feedbackTagFor(uint64_t key) const;

    unsigned findQuality(unsigned set, uint64_t tag, uint8_t kind) const;
    unsigned allocateQuality(unsigned set, uint64_t tag, uint8_t kind);
    unsigned findFeedback(unsigned set, uint64_t tag) const;
    unsigned allocateFeedback(unsigned set);
    unsigned qualityVictim(unsigned set) const;
    unsigned feedbackVictim(unsigned set);
    void touchQuality(unsigned set, unsigned way);

    bool sample(Addr pc, uint8_t kind, Addr triggerLine, unsigned period, uint64_t salt) const;
    unsigned blockPeriod(const QualityEntry &entry) const;
    bool shouldBlock(const QualityEntry &entry) const;
    bool shouldReopen(const QualityEntry &entry) const;
    void transition(QualityEntry &entry, State next);
    void applyOutcome(QualityEntry &entry, bool useful);
    void resolveFeedback(unsigned feedbackIndex, bool useful);
    void expireFeedback(unsigned feedbackIndex);
    uint8_t currentEpoch() const;
    uint8_t epochDistance(uint8_t issueEpoch) const;
};

}  // namespace prefetch
}  // namespace gem5

#endif
