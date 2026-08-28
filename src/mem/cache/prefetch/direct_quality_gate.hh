#ifndef __MEM_CACHE_PREFETCH_DIRECT_QUALITY_GATE_HH__
#define __MEM_CACHE_PREFETCH_DIRECT_QUALITY_GATE_HH__

#include <array>
#include <cstdint>
#include <vector>

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
        unsigned decayPeriod = 64;
    };

    enum class TraceOutcome : uint8_t
    {
        UsefulDemand,
        UnusedExpiry,
        UnknownFeedbackReplacement,
        UnknownOwnerReplaced,
    };

    /**
     * Optional observer for exact raw-candidate direct-quality certification.
     * The normal gate has no sink; this interface deliberately carries only
     * the compact event identity needed by the offline oracle.
     */
    class TraceSink
    {
      public:
        virtual ~TraceSink() = default;
        virtual void directQualityTraceConfig(const Config &config) = 0;
        virtual void directQualityTraceCandidate(uint64_t event_sequence, Addr pc, uint8_t kind, Addr trigger_line,
                                                 Addr candidate_line, State state, bool allowed, bool sampled) = 0;
        // "Issue" is retained for the existing ArchDB table name.  The
        // event is a selected raw BOP candidate, not a physical request.
        virtual void directQualityTraceIssue(uint64_t event_sequence, uint64_t feedback_id,
                                             uint64_t candidate_demand_sequence, Addr line, uint8_t kind) = 0;
        virtual void directQualityTraceDemand(uint64_t event_sequence, uint64_t demand_sequence, Addr line) = 0;
        virtual void directQualityTraceOutcome(uint64_t event_sequence, uint64_t feedback_id,
                                               uint64_t resolve_demand_sequence, Addr line, TraceOutcome outcome) = 0;
    };

    struct Decision
    {
        bool allowed = true;
        bool sampled = false;
        bool feedbackInserted = false;
        State state = State::Observe;
        unsigned set = 0;
        unsigned way = 0;
        uint8_t generation = 0;
    };

    DirectQualityGate();
    explicit DirectQualityGate(const Config &config);

    void setTraceSink(TraceSink *sink);

    /**
     * Evaluate one raw BOP candidate. Selected feedback is registered here,
     * before local filtering or queueing, so controller training is a BOP
     * algorithm property rather than a physical prefetch-path property.
     */
    Decision admit(Addr pc, uint8_t kind, Addr trigger_line, Addr candidate_line);
    Decision admit(Addr pc, uint8_t kind, Addr line) { return admit(pc, kind, line, line); }
    /**
     * Observe one L2 read demand.  This is the online form of the replay
     * oracle: the first later demand to a sampled line is useful, and an
     * unmatched request becomes unused after the configured demand horizon.
     */
    void observeDemand(Addr line);

    State state(Addr pc, uint8_t kind) const;
    uint64_t candidates() const { return candidateCount; }
    uint64_t allowed() const { return allowedCount; }
    uint64_t sampleSelected() const { return sampleSelectedCount; }
    uint64_t suppressed() const { return suppressedCount; }
    uint64_t sampled() const { return sampledCount; }
    uint64_t useful() const { return usefulCount; }
    uint64_t unused() const { return unusedCount; }
    uint64_t feedbackCoalesced() const { return feedbackCoalescedCount; }
    uint64_t feedbackConflicts() const { return feedbackConflictCount; }
    uint64_t feedbackReplacements() const { return feedbackReplacementCount; }
    uint64_t feedbackExpiries() const { return feedbackExpiryCount; }
    uint64_t feedbackExpiryUnused() const { return feedbackExpiryUnusedCount; }
    uint64_t unknownDrops() const { return unknownDropCount; }
    uint64_t orphanOutcomes() const { return orphanOutcomeCount; }
    uint64_t stateTransitions() const { return stateTransitionCount; }
    uint64_t blockToRecoverTransitions() const { return blockToRecoverTransitionCount; }
    uint64_t recoverToOpenTransitions() const { return recoverToOpenTransitionCount; }
    uint64_t recoverToBlockTransitions() const { return recoverToBlockTransitionCount; }
    uint64_t peakOutstanding() const { return peakOutstandingCount; }

  private:
    static constexpr unsigned MaxQualityWays = 4;
    static constexpr unsigned MaxFeedbackWays = 16;
    static constexpr unsigned MaxQualityEntries = 256;
    static constexpr unsigned MaxFeedbackEntries = 4096;
    static constexpr unsigned NoExpiryRecord = MaxFeedbackEntries;
    static constexpr unsigned CacheLineBits = 6;
    // Horizon is bounded below half the age space; this permits wrap-safe
    // expiry comparisons while retaining the full demand sequence for trace
    // output.
    static constexpr unsigned AgeBits = 16;
    static constexpr uint16_t AgeHalfRange = uint16_t(1U << (AgeBits - 1));

    struct QualityEntry
    {
        // valid, kind, and state share one byte in the hardware layout.
        uint8_t metadata = 0;
        uint16_t tag = 0;
        uint8_t generation = 0;
        uint16_t useful = 0;
        uint16_t unused = 0;
        uint16_t resolvedSinceDecay = 0;
        uint16_t recoverySamples = 0;
        uint16_t recoveryGeneration = 0;
        uint8_t recoveryProbePeriod = 0;

        static constexpr uint8_t ValidMask = 1U << 0;
        static constexpr uint8_t KindMask = 0x3U << 1;
        static constexpr uint8_t StateMask = 0x3U << 3;

        bool isValid() const { return metadata & ValidMask; }
        void setValid(bool value) { metadata = value ? metadata | ValidMask : metadata & ~ValidMask; }
        uint8_t kindValue() const { return (metadata >> 1) & 0x3U; }
        void setKind(uint8_t value) { metadata = (metadata & ~KindMask) | ((value & 0x3U) << 1); }
        State stateValue() const { return static_cast<State>((metadata >> 3) & 0x3U); }
        void setState(State value)
        {
            metadata = (metadata & ~StateMask) | ((static_cast<uint8_t>(value) & 0x3U) << 3);
        }
    };

    struct FeedbackEntry
    {
        bool valid = false;
        // A 64-byte-aligned physical line is represented by its line number.
        uint64_t line = 0;
        uint8_t qualityIndex = 0;
        uint8_t qualityGeneration = 0;
        uint16_t recoveryGeneration = 0;
        uint16_t issueAge = 0;
        uint16_t expiryHeapIndex = NoExpiryRecord;
    };

    struct FeedbackTraceInfo
    {
        uint64_t id = 0;
        uint64_t issueAge = 0;
    };

    Config cfg;
    unsigned qualitySets;
    unsigned feedbackSets;
    unsigned qualitySetBits;
    unsigned feedbackSetBits;
    Addr qualityTagMask;
    std::array<QualityEntry, MaxQualityEntries> quality = {};
    std::array<uint8_t, MaxQualityEntries> qualityPLRU = {};
    std::array<FeedbackEntry, MaxFeedbackEntries> feedback = {};
    std::array<uint16_t, MaxFeedbackEntries> feedbackNextVictim = {};
    std::array<uint16_t, MaxFeedbackEntries> expiryHeap = {};
    // Allocated only with a TraceSink; never part of the hardware entry.
    std::vector<FeedbackTraceInfo> feedbackTrace;
    unsigned expiryHeapSize = 0;
    uint64_t demandAge = 0;
    uint64_t nextFeedbackId = 0;
    uint64_t nextTraceEventSequence = 0;
    TraceSink *traceSink = nullptr;
    uint64_t candidateCount = 0;
    uint64_t allowedCount = 0;
    uint64_t sampleSelectedCount = 0;
    uint64_t suppressedCount = 0;
    uint64_t sampledCount = 0;
    uint64_t usefulCount = 0;
    uint64_t unusedCount = 0;
    uint64_t feedbackConflictCount = 0;
    uint64_t feedbackReplacementCount = 0;
    uint64_t feedbackCoalescedCount = 0;
    uint64_t feedbackExpiryCount = 0;
    uint64_t feedbackExpiryUnusedCount = 0;
    uint64_t unknownDropCount = 0;
    uint64_t orphanOutcomeCount = 0;
    uint64_t stateTransitionCount = 0;
    uint64_t blockToRecoverTransitionCount = 0;
    uint64_t recoverToOpenTransitionCount = 0;
    uint64_t recoverToBlockTransitionCount = 0;
    uint64_t outstandingCount = 0;
    uint64_t peakOutstandingCount = 0;

    static uint64_t mix64(uint64_t value);
    uint64_t qualitySignature(Addr pc, uint8_t kind) const;
    unsigned qualitySetFor(Addr pc, uint8_t kind) const;
    Addr qualityTagFor(Addr pc, uint8_t kind) const;
    unsigned feedbackSetFor(uint64_t line) const;
    unsigned findQuality(unsigned set, Addr tag, uint8_t kind) const;
    unsigned allocateQuality(unsigned set, Addr tag, uint8_t kind);
    unsigned findFeedback(unsigned set, uint64_t line) const;
    unsigned allocateFeedback(unsigned set);
    unsigned feedbackIndex(unsigned set, unsigned way) const;
    void touchQuality(unsigned set, unsigned way);
    unsigned qualityVictim(unsigned set) const;
    unsigned feedbackVictim(unsigned set);
    unsigned blockProbePeriod(const QualityEntry &entry) const;
    bool sample(Addr pc, uint8_t kind, Addr trigger_line, unsigned period, uint64_t salt) const;
    bool shouldBlock(const QualityEntry &entry) const;
    bool meetsReopen(const QualityEntry &entry) const;
    void transitionTo(QualityEntry &entry, State next);
    void applyOutcome(QualityEntry &entry, uint16_t recovery_generation, bool useful);
    void updateState(QualityEntry &entry, unsigned previous_block_probe_period = 0);
    uint64_t recordCandidate(Addr line, uint8_t kind, unsigned quality_set, unsigned quality_way,
                             uint8_t quality_generation);
    void retireUnknown(unsigned feedback_index, TraceOutcome outcome);
    void invalidateFeedback(unsigned feedback_index);
    bool resolveFeedback(unsigned feedback_index, bool useful, TraceOutcome outcome);
    void traceOutcome(unsigned feedback_index, TraceOutcome outcome);
    static uint64_t compactLine(Addr line) { return line >> CacheLineBits; }
    static Addr expandLine(uint64_t line) { return line << CacheLineBits; }
    uint16_t compactAge() const { return static_cast<uint16_t>(demandAge); }
    uint16_t ageDistance(uint16_t age) const { return static_cast<uint16_t>(compactAge() - age); }
    bool expiryBefore(unsigned lhs, unsigned rhs) const;
    void insertExpiry(unsigned feedback_index);
    void removeExpiry(unsigned feedback_index);
    void restoreExpiryHeap(unsigned heap_index);
    void expireFeedback();
};

}  // namespace prefetch
}  // namespace gem5

#endif
