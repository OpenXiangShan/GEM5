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
    enum class State : uint8_t { Observe, Open, Block, Recover };

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
        unsigned reopenConfirmSamples = 0;
    };

    enum class TraceOutcome : uint8_t
    {
        UsefulDemand,
        UnusedExpiry,
        UnknownFeedbackReplacement,
        UnknownOwnerReplaced,
    };

    /**
     * Optional observer for exact physical direct-quality certification.
     * The normal gate has no sink; this interface deliberately carries only
     * the compact event identity needed by the offline oracle.
     */
    class TraceSink
    {
      public:
        virtual ~TraceSink() = default;
        virtual void directQualityTraceConfig(const Config &config) = 0;
        virtual void directQualityTraceIssue(
            uint64_t event_sequence, uint64_t feedback_id,
            uint64_t issue_demand_sequence, Addr line, uint8_t kind) = 0;
        virtual void directQualityTraceDemand(
            uint64_t event_sequence, uint64_t demand_sequence,
            Addr line) = 0;
        virtual void directQualityTraceOutcome(
            uint64_t event_sequence, uint64_t feedback_id,
            uint64_t resolve_demand_sequence, Addr line,
            TraceOutcome outcome) = 0;
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

    DirectQualityGate();
    explicit DirectQualityGate(const Config &config);

    void setTraceSink(TraceSink *sink);

    Decision admit(Addr pc, uint8_t kind, Addr line);
    /**
     * Register a sampled request only after it reaches the real BOP issue
     * point.  The token was captured at candidate admission and protects a
     * newly allocated PC-kind entry from stale queued requests.
     */
    void recordIssued(Addr line, uint8_t kind, unsigned quality_set,
                      unsigned quality_way, uint8_t quality_generation);
    /**
     * Observe one L2 read demand.  This is the online form of the replay
     * oracle: the first later demand to a sampled line is useful, and an
     * unmatched request becomes unused after the configured demand horizon.
     */
    void observeDemand(Addr line);

    State state(Addr pc, uint8_t kind) const;
    uint64_t issued() const { return issuedCount; }
    uint64_t suppressed() const { return suppressedCount; }
    uint64_t sampled() const { return sampledCount; }
    uint64_t useful() const { return usefulCount; }
    uint64_t unused() const { return unusedCount; }
    uint64_t feedbackConflicts() const { return feedbackConflictCount; }
    uint64_t feedbackReplacements() const { return feedbackReplacementCount; }
    uint64_t feedbackExpiries() const { return feedbackExpiryCount; }
    uint64_t feedbackExpiryUnused() const { return feedbackExpiryUnusedCount; }
    uint64_t unknownDrops() const { return unknownDropCount; }
    uint64_t feedbackTokenDrops() const { return feedbackTokenDropCount; }
    uint64_t orphanOutcomes() const { return orphanOutcomeCount; }
    uint64_t stateTransitions() const { return stateTransitionCount; }
    uint64_t blockToRecoverTransitions() const
    {
        return blockToRecoverTransitionCount;
    }
    uint64_t recoverToOpenTransitions() const
    {
        return recoverToOpenTransitionCount;
    }
    uint64_t recoverToBlockTransitions() const
    {
        return recoverToBlockTransitionCount;
    }
    uint64_t peakOutstanding() const { return peakOutstandingCount; }

  private:
    static constexpr unsigned MaxQualityWays = 4;
    static constexpr unsigned MaxFeedbackWays = 16;
    static constexpr unsigned MaxQualityEntries = 256;
    static constexpr unsigned MaxFeedbackEntries = 4096;
    static constexpr unsigned NoExpiryRecord = MaxFeedbackEntries;

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
        uint32_t recoverySamples = 0;
        uint32_t recoveryGeneration = 0;
        unsigned recoveryProbePeriod = 0;
        uint8_t plru = 0;
    };

    struct FeedbackEntry
    {
        bool valid = false;
        Addr line = 0;
        unsigned qualitySet = 0;
        unsigned qualityWay = 0;
        uint8_t qualityGeneration = 0;
        uint8_t kind = 0;
        uint32_t recoveryGeneration = 0;
        uint64_t issueAge = 0;
        uint64_t traceId = 0;
        unsigned expiryHeapIndex = NoExpiryRecord;
    };

    Config cfg;
    unsigned qualitySets;
    unsigned feedbackSets;
    unsigned qualitySetBits;
    unsigned feedbackSetBits;
    Addr qualityTagMask;
    std::array<QualityEntry, MaxQualityEntries> quality = {};
    std::array<FeedbackEntry, MaxFeedbackEntries> feedback = {};
    std::array<unsigned, MaxFeedbackEntries> expiryHeap = {};
    unsigned expiryHeapSize = 0;
    uint64_t demandAge = 0;
    uint64_t nextFeedbackId = 0;
    uint64_t nextTraceEventSequence = 0;
    TraceSink *traceSink = nullptr;
    uint64_t issuedCount = 0;
    uint64_t suppressedCount = 0;
    uint64_t sampledCount = 0;
    uint64_t usefulCount = 0;
    uint64_t unusedCount = 0;
    uint64_t feedbackConflictCount = 0;
    uint64_t feedbackReplacementCount = 0;
    uint64_t feedbackExpiryCount = 0;
    uint64_t feedbackExpiryUnusedCount = 0;
    uint64_t unknownDropCount = 0;
    uint64_t feedbackTokenDropCount = 0;
    uint64_t orphanOutcomeCount = 0;
    uint64_t stateTransitionCount = 0;
    uint64_t blockToRecoverTransitionCount = 0;
    uint64_t recoverToOpenTransitionCount = 0;
    uint64_t recoverToBlockTransitionCount = 0;
    uint64_t outstandingCount = 0;
    uint64_t peakOutstandingCount = 0;

    unsigned qualitySetFor(Addr pc, uint8_t kind) const;
    Addr qualityTagFor(Addr pc, uint8_t kind) const;
    unsigned feedbackSetFor(Addr line) const;
    unsigned findQuality(unsigned set, Addr tag, uint8_t kind) const;
    unsigned allocateQuality(unsigned set, Addr tag, uint8_t kind);
    unsigned findFeedback(unsigned set, Addr line) const;
    unsigned allocateFeedback(unsigned set);
    unsigned feedbackIndex(unsigned set, unsigned way) const;
    void touchQuality(unsigned set, unsigned way);
    unsigned qualityVictim(unsigned set) const;
    unsigned feedbackVictim(unsigned set) const;
    unsigned blockProbePeriod(const QualityEntry &entry) const;
    bool shouldBlock(const QualityEntry &entry) const;
    bool meetsReopen(const QualityEntry &entry) const;
    void transitionTo(QualityEntry &entry, State next);
    void applyOutcome(QualityEntry &entry, uint32_t recovery_generation,
                      bool useful);
    void retireUnknown(unsigned feedback_index, TraceOutcome outcome);
    void invalidateFeedback(unsigned feedback_index);
    bool resolveFeedback(unsigned feedback_index, bool useful,
                         TraceOutcome outcome);
    void traceOutcome(const FeedbackEntry &entry, TraceOutcome outcome);
    bool expiryBefore(unsigned lhs, unsigned rhs) const;
    void insertExpiry(unsigned feedback_index);
    void removeExpiry(unsigned feedback_index);
    void restoreExpiryHeap(unsigned heap_index);
    void expireFeedback();
};

} // namespace prefetch
} // namespace gem5

#endif
