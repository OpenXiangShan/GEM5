#ifndef __CPU_VALUEPRED_EGDIFF_HH__
#define __CPU_VALUEPRED_EGDIFF_HH__

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "base/statistics.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/EgDiff.hh"

namespace gem5
{

namespace valuepred
{

class EgDiffPredictionRecord : public VPPredictionRecord
{
  public:
    bool slotAllocated = false;
    bool selected = false;
    bool appliedValueRecorded = false;
    bool skipFpcAdvanceAtCommit = false;
    uint64_t loadOrdinal = 0;
};

class EgDiff : public VPUnit
{
  private:
    using Params = EgDiffParams;

    static constexpr uint8_t MaxFpc = 7;

    struct Entry
    {
        bool valid = false;
        uint64_t tag = 0;
        uint8_t usefulness = 0;
        unsigned distance = 0;
        RegVal diff = 0;
        Addr basePc = 0;
        uint8_t fpc = 0;
        uint64_t randomState = 1;
    };

    enum class ValueSource : uint8_t
    {
        None,
        Predicted,
        Actual
    };

    struct HistoryEntry
    {
        uint64_t ordinal = 0;
        uint64_t seqNo = 0;
        Addr pc = 0;

        bool specValueValid = false;
        RegVal specValue = 0;
        ValueSource valueSource = ValueSource::None;

        bool actualValid = false;
        RegVal actualValue = 0;
        bool completed = false;
        bool committed = false;

        bool requestPending = false;
        bool requestReady = false;
        bool requestDelivered = false;
        uint64_t baseOrdinal = 0;
        Addr expectedBasePc = 0;
        unsigned predictionDistance = 0;
        RegVal predictionDiff = 0;
        uint64_t readyCycle = 0;
    };

    struct ThreadState
    {
        uint64_t nextOrdinal = 0;
        /*
         * This unified exact history has no explicit capacity; it does not
         * model the paper's separate 32-entry speculative and non-speculative
         * GVQs.
         */
        std::map<uint64_t, HistoryEntry> history;
    };

    struct LastMispActivation
    {
        ThreadID tid = 0;
        uint64_t seqNo = 0;
        uint64_t remaining = 0;
    };

    const unsigned order;
    const uint64_t fpcSeed;
    const unsigned tableEntryCount;
    const unsigned tagBits;
    const unsigned usefulBits;
    const unsigned allocationProbabilityDenominator;
    const unsigned tickBits;
    const uint64_t normalPredictionLatency;
    const uint64_t deferredPredictionLatency;
    const uint64_t lastMispWindow;
    std::vector<ThreadState> states;
    std::vector<Entry> table;
    std::vector<LastMispActivation> lastMispActivations;
    uint64_t maxHistoryEntriesSeen = 0;
    uint64_t agingTick = 0;
    uint64_t allocationRandomState = 1;
    bool virtSQHoldAdvance = false;

    EgDiffPredictionRecord *getRecord(VPPredictionRecord *record) const;
    const EgDiffPredictionRecord *getRecord(
            const VPPredictionRecord *record) const;
    HistoryEntry *findHistory(ThreadState &state, uint64_t ordinal);
    const HistoryEntry *findHistory(
            const ThreadState &state, uint64_t ordinal) const;
    static uint64_t mix64(uint64_t value);
    uint64_t initialRandomState(ThreadID tid, Addr pc) const;
    uint64_t initialAllocationRandomState() const;
    static uint64_t nextRandom(uint64_t &state);
    std::size_t tableIndex(ThreadID tid, Addr pc) const;
    uint64_t tableTag(ThreadID tid, Addr pc) const;
    uint8_t maxUsefulness() const;
    bool shouldAllocate();
    void advanceAgingTick();
    bool advanceFpc(Entry &entry);
    void makeSpecValueAvailable(ThreadState &state, ThreadID tid,
            HistoryEntry &base, RegVal value, ValueSource source,
            uint64_t cycle);
    void wakeDeferred(ThreadState &state, ThreadID tid,
            HistoryEntry &base, uint64_t cycle);
    void pruneHistory(ThreadState &state, ThreadID tid);
    void updateHistoryOccupancyStats();
    void updateTableEntryStats();
    bool lastMispActive() const { return !lastMispActivations.empty(); }
    static const char *valueSourceName(ValueSource source);

    struct EgDiffStats : public statistics::Group
    {
        statistics::Scalar dispatchSlots;
        statistics::Scalar predictionRequests;
        statistics::Scalar normalRequests;
        statistics::Scalar deferredBindings;
        statistics::Scalar deferredWakeups;
        statistics::Scalar predictionsOffered;
        statistics::Scalar predictionsApplied;
        statistics::Scalar appliedCorrect;
        statistics::Scalar appliedIncorrect;
        statistics::Scalar actualBaseUses;
        statistics::Scalar predictedBaseUses;
        statistics::Scalar confidenceSuppressions;
        statistics::Scalar lastMispSuppressions;
        statistics::Scalar noEntry;
        statistics::Scalar historyTooShort;
        statistics::Scalar targetCompletedDrops;
        statistics::Scalar valueAvailableUpdates;
        statistics::Scalar diffMatches;
        statistics::Scalar diffMismatches;
        statistics::Scalar fpcAdvances;
        statistics::Scalar fpcHolds;
        statistics::Scalar fpcTableClears;
        statistics::Scalar fpcEntriesCleared;
        statistics::Scalar fpcHoldByVirtSQ;
        statistics::Scalar pollingDistanceChanges;
        statistics::Scalar lastMispActivations;
        statistics::Scalar squashedSlots;
        statistics::Scalar cancelledRequests;
        statistics::Scalar staleValueCallbacks;
        statistics::Scalar basePcMismatches;
        statistics::Scalar basePcMismatchSuppressions;
        statistics::Scalar tableEntries;
        statistics::Scalar tableConflicts;
        statistics::Scalar tableReplacements;
        statistics::Scalar tableEvictions;
        statistics::Scalar allocations;
        statistics::Scalar allocationAttempts;
        statistics::Scalar allocationSkips;
        statistics::Scalar allocationProbabilitySkips;
        statistics::Scalar allocationUsefulnessSkips;
        statistics::Scalar usefulnessIncrements;
        statistics::Scalar usefulnessResets;
        statistics::Scalar usefulnessDecrements;
        statistics::Scalar usefulnessAgingPasses;
        statistics::Scalar agingTicks;
        statistics::Scalar historyCapacityDrops;
        statistics::Scalar historyEntries;
        statistics::Scalar maxHistoryEntries;
        statistics::Scalar prunedHistoryEntries;

        EgDiffStats(statistics::Group *parent);
    } egdiffStats;

  public:
    EgDiff(const Params &params);

    std::string name() const override { return "EgDiff"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;
    void dispatch(const VPDispatchInfo &dispatchInfo,
            VPPredictionRecord *record) override;
    VPPredictionCandidate latePredict(
            const VPLatePredictRequest &request,
            VPPredictionRecord *record) override;
    void valueAvailable(const VPValueAvailableInfo &valueInfo,
            VPPredictionRecord *record) override;
    void predictionApplied(const VPPredictionAppliedInfo &appliedInfo,
            VPPredictionRecord *record) override;
    void valueMispredicted(const VPMispredictionInfo &mispInfo,
            VPPredictionRecord *record) override;
    void commitInstruction(const VPCommitInfo &commitInfo) override;
    void update(const VPUpdateInfo &updateInfo,
            const VPPredictionRecord *record,
            const VPFeedback &feedback) override;
    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;
    void squash(ThreadID tid, const uint64_t seq_no) override;
    void onVirtSQThrottleRise() override;
    void setVirtSQThrottleHold(bool hold) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::EgDiff;
    }
};

} // namespace valuepred
} // namespace gem5

#endif // __CPU_VALUEPRED_EGDIFF_HH__
