#ifndef __CPU_VALUEPRED_IDEAL_EGDIFF_HH__
#define __CPU_VALUEPRED_IDEAL_EGDIFF_HH__

#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/IdealEgDiff.hh"

namespace gem5
{

namespace valuepred
{

class IdealEgDiffPredictionRecord : public VPPredictionRecord
{
  public:
    bool slotAllocated = false;
    uint64_t loadOrdinal = 0;
};

class IdealEgDiff : public VPUnit
{
  private:
    using Params = IdealEgDiffParams;

    struct Entry
    {
        unsigned distance;
        RegVal diff;
        SatCounter16 confidence;

        Entry(unsigned distance, RegVal diff, unsigned confidence_bits)
            : distance(distance), diff(diff),
              confidence(confidence_bits, 0)
        {
        }
    };

    struct HistoryEntry
    {
        uint64_t ordinal = 0;
        uint64_t seqNo = 0;
        Addr pc = 0;
        bool actualValid = false;
        RegVal actualValue = 0;
        bool committed = false;
    };

    struct ThreadState
    {
        uint64_t nextOrdinal = 0;
        std::unordered_map<Addr, Entry> table;
        std::map<uint64_t, HistoryEntry> history;
    };

    const unsigned order;
    const unsigned confidenceBits;
    std::vector<ThreadState> states;
    uint64_t maxHistoryEntriesSeen = 0;

    IdealEgDiffPredictionRecord *getRecord(VPPredictionRecord *record) const;
    const IdealEgDiffPredictionRecord *getRecord(
            const VPPredictionRecord *record) const;
    HistoryEntry *findHistory(ThreadState &state, uint64_t ordinal);
    const HistoryEntry *findHistory(
            const ThreadState &state, uint64_t ordinal) const;
    void pruneHistory(ThreadState &state, ThreadID tid);
    void updateHistoryOccupancyStats();

    struct IdealEgDiffStats : public statistics::Group
    {
        statistics::Scalar dispatchSlots;
        statistics::Scalar valueAvailableUpdates;
        statistics::Scalar latePredictAttempts;
        statistics::Scalar latePredictions;
        statistics::Scalar lateCorrect;
        statistics::Scalar lateIncorrect;
        statistics::Scalar lateBaseUnavailable;
        statistics::Scalar lateNoEntry;
        statistics::Scalar lateConfidenceSuppressed;
        statistics::Scalar diffMatches;
        statistics::Scalar diffMismatches;
        statistics::Scalar pollingDistanceChanges;
        statistics::Scalar squashedSlots;
        statistics::Scalar staleValueCallbacks;
        statistics::Scalar historyEntries;
        statistics::Scalar maxHistoryEntries;
        statistics::Scalar prunedHistoryEntries;

        IdealEgDiffStats(statistics::Group *parent);
    } egdiffStats;

  public:
    IdealEgDiff(const Params &params);

    std::string name() const override { return "IdealEgDiff"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;
    void dispatch(const VPDispatchInfo &dispatchInfo,
            VPPredictionRecord *record) override;
    VPPredictionCandidate latePredict(
            const VPLatePredictRequest &request,
            VPPredictionRecord *record) override;
    void valueAvailable(const VPValueAvailableInfo &valueInfo,
            VPPredictionRecord *record) override;
    void update(const VPUpdateInfo &updateInfo,
            const VPPredictionRecord *record,
            const VPFeedback &feedback) override;
    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;
    void squash(ThreadID tid, const uint64_t seq_no) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::IdealEgDiff;
    }
};

} // namespace valuepred
} // namespace gem5

#endif // __CPU_VALUEPRED_IDEAL_EGDIFF_HH__
