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
 * Bounded online approximation of the offline direct-quality ledger.
 *
 * The table is deliberately independent of BOP so that its state transitions
 * can be unit tested without constructing a cache or a SimObject graph.
 */
class DirectQualityGate
{
  public:
    enum class State : uint8_t { Observe, Open, Block };

    struct Config
    {
        unsigned qualityEntries = 256;
        unsigned qualityWays = 4;
        unsigned qualityTagBits = 10;
        unsigned feedbackEntries = 256;
        unsigned feedbackWays = 4;
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
        unsigned reopenProbePeriod = 64;
    };

    struct Decision
    {
        bool allowed = true;
        bool sampled = false;
        State state = State::Observe;
        unsigned set = 0;
        unsigned way = 0;
        uint8_t generation = 0;
    };

    struct Outcome
    {
        bool resolved = false;
        bool useful = false;
        bool expired = false;
        bool conflict = false;
        State state = State::Observe;
    };

    DirectQualityGate();
    explicit DirectQualityGate(const Config &config);

    Decision admit(Addr pc, uint8_t kind, Addr line);
    /**
     * Register a sampled request only after it reaches the real BOP issue
     * point.  The token was captured at candidate admission and protects a
     * newly allocated PC-kind entry from stale queued requests.
     */
    void recordIssued(Addr line, uint8_t kind, unsigned quality_set,
                      unsigned quality_way, uint8_t quality_generation);
    Outcome resolve(Addr line, bool useful);
    void advanceDemand();

    State state(Addr pc, uint8_t kind) const;
    uint64_t issued() const { return issuedCount; }
    uint64_t suppressed() const { return suppressedCount; }
    uint64_t sampled() const { return sampledCount; }
    uint64_t useful() const { return usefulCount; }
    uint64_t unused() const { return unusedCount; }
    uint64_t feedbackConflicts() const { return feedbackConflictCount; }
    uint64_t feedbackReplacements() const { return feedbackReplacementCount; }
    uint64_t feedbackExpiries() const { return feedbackExpiryCount; }
    uint64_t unknownDrops() const { return unknownDropCount; }
    uint64_t feedbackTokenDrops() const { return feedbackTokenDropCount; }
    uint64_t orphanOutcomes() const { return orphanOutcomeCount; }
    uint64_t stateTransitions() const { return stateTransitionCount; }

  private:
    static constexpr unsigned MaxWays = 4;
    static constexpr unsigned MaxQualityEntries = 256;
    static constexpr unsigned MaxFeedbackEntries = 256;

    struct QualityEntry
    {
        bool valid = false;
        Addr tag = 0;
        uint8_t kind = 0;
        uint8_t generation = 0;
        State state = State::Observe;
        uint32_t issued = 0;
        uint32_t sampled = 0;
        uint32_t useful = 0;
        uint32_t unused = 0;
        uint8_t plru = 0;
    };

    struct FeedbackEntry
    {
        bool valid = false;
        Addr line = 0;
        unsigned qualitySet = 0;
        unsigned qualityWay = 0;
        uint8_t generation = 0;
        uint8_t kind = 0;
        uint64_t issueAge = 0;
    };

    Config cfg;
    unsigned qualitySets;
    unsigned feedbackSets;
    unsigned qualitySetBits;
    unsigned feedbackSetBits;
    Addr qualityTagMask;
    std::array<QualityEntry, MaxQualityEntries> quality = {};
    std::array<FeedbackEntry, MaxFeedbackEntries> feedback = {};
    uint64_t demandAge = 0;
    uint64_t issuedCount = 0;
    uint64_t suppressedCount = 0;
    uint64_t sampledCount = 0;
    uint64_t usefulCount = 0;
    uint64_t unusedCount = 0;
    uint64_t feedbackConflictCount = 0;
    uint64_t feedbackReplacementCount = 0;
    uint64_t feedbackExpiryCount = 0;
    uint64_t unknownDropCount = 0;
    uint64_t feedbackTokenDropCount = 0;
    uint64_t orphanOutcomeCount = 0;
    uint64_t stateTransitionCount = 0;

    unsigned qualitySetFor(Addr pc, uint8_t kind) const;
    Addr qualityTagFor(Addr pc, uint8_t kind) const;
    unsigned feedbackSetFor(Addr line) const;
    unsigned findQuality(unsigned set, Addr tag, uint8_t kind) const;
    unsigned allocateQuality(unsigned set, Addr tag, uint8_t kind);
    unsigned findFeedback(unsigned set, Addr line) const;
    unsigned allocateFeedback(unsigned set);
    void touchQuality(unsigned set, unsigned way);
    unsigned qualityVictim(unsigned set) const;
    unsigned feedbackVictim(unsigned set) const;
    void applyOutcome(QualityEntry &entry, bool useful);
    void retireUnknown(FeedbackEntry &entry, bool expiry);
};

} // namespace prefetch
} // namespace gem5

#endif
