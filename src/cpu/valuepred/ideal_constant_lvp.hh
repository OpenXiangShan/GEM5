#ifndef __IDEAL_CONSTANT_LVP_HH__
#define __IDEAL_CONSTANT_LVP_HH__

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/IdealConstantLVP.hh"

namespace gem5
{

namespace valuepred
{

class IdealConstantLVP : public VPUnit
{
  private:
    using Params = IdealConstantLVPParams;

    struct ICEntry
    {
        SatCounter16 confidence;
        RegVal value;
        uint64_t saturationEpoch = 0;
        uint64_t saturationValueSegment = 0;

        ICEntry(unsigned sat_counter_bits, RegVal v)
            : confidence(sat_counter_bits, 0), value(v)
        {
        }
    };

    struct ProfileEntry
    {
        uint64_t updates = 0;
        uint64_t firstUpdate = 0;
        uint64_t lastUpdate = 0;
        uint64_t valueChanges = 0;
        uint64_t saturationTransitions = 0;
        uint64_t saturatedUpdates = 0;
        uint64_t firstSaturationUpdate = 0;
        uint64_t committedSaturatedOffers = 0;
        uint64_t predictionUses = 0;
        uint64_t correctPredictionUses = 0;
        // One-based indexes into the scope's interval vector, keyed by the
        // predictor epoch in which a prediction was offered. This stays on
        // the commit path, so lookups must not scan historical epochs.
        std::unordered_map<uint64_t, uint64_t> predictionIntervals;
        bool everSaturated = false;
    };

    struct PredictionInterval
    {
        ThreadID tid = 0;
        Addr pc = 0;
        uint64_t saturationEpoch = 0;
        uint64_t firstPredictionUseSeqNo = 0;
        uint64_t lastPredictionUseSeqNo = 0;
        uint64_t saturationEndSeqNo = 0;
        uint64_t predictionUses = 0;
        uint64_t correctPredictionUses = 0;
        bool openAtEnd = true;
    };

    using ProfileTable = std::unordered_map<Addr, ProfileEntry>;
    using PredictionIntervals = std::vector<PredictionInterval>;

    // A saturation epoch normally owns one value segment.  Keep a separate
    // segment when a zero-bit counter remains saturated across a value change
    // so each stored raw RegVal bit pattern remains observable.
    struct SaturatedValueSegment
    {
        ThreadID tid = 0;
        Addr pc = 0;
        uint64_t saturationEpoch = 0;
        uint64_t valueSegment = 0;
        RegVal saturatedValue = 0;
        uint64_t saturationStartSeqNo = 0;
        uint64_t saturationEndSeqNo = 0;
        uint64_t firstPredictionUseSeqNo = 0;
        uint64_t lastPredictionUseSeqNo = 0;
        uint64_t predictionUses = 0;
        uint64_t correctPredictionUses = 0;
        bool openAtScopeStart = false;
        bool openAtEnd = true;
    };

    struct SaturatedValueSegmentKey
    {
        Addr pc = 0;
        uint64_t saturationEpoch = 0;
        uint64_t valueSegment = 0;

        bool
        operator==(const SaturatedValueSegmentKey &other) const
        {
            return pc == other.pc &&
                saturationEpoch == other.saturationEpoch &&
                valueSegment == other.valueSegment;
        }
    };

    struct SaturatedValueSegmentKeyHash
    {
        size_t
        operator()(const SaturatedValueSegmentKey &key) const
        {
            const auto pc_hash = std::hash<Addr>{}(key.pc);
            const auto epoch_hash = std::hash<uint64_t>{}(
                key.saturationEpoch);
            const auto segment_hash = std::hash<uint64_t>{}(
                key.valueSegment);
            return pc_hash ^ (epoch_hash << 1) ^ (segment_hash << 2);
        }
    };

    struct SaturatedValueProfile
    {
        std::vector<SaturatedValueSegment> segments;
        // One-based vector indexes avoid searching historical segments when
        // a live epoch closes, changes value, or commits a delayed prediction.
        std::unordered_map<SaturatedValueSegmentKey, uint64_t,
                SaturatedValueSegmentKeyHash> segmentIndexes;
    };

    using SaturatedValueProfiles = std::vector<SaturatedValueProfile>;

    // The shadow tables model a bounded QF/PCT organization only when
    // profiling is explicitly enabled.  They are deliberately separate from
    // idealConstTables so profiling can never change architectural behavior.
    struct ShadowEntry
    {
        bool valid = false;
        Addr tag = 0;
        RegVal value = 0;
        uint32_t qualification = 0;
        uint32_t confidence = 0;
        uint64_t lru = 0;
        uint64_t generation = 0;
        uint64_t lastCommit = 0;
    };

    struct ShadowTable
    {
        unsigned sets = 0;
        unsigned ways = 0;
        std::vector<ShadowEntry> entries;
        std::vector<unsigned> setOccupancy;
        uint64_t lruClock = 0;
        unsigned occupancy = 0;
        unsigned peakOccupancy = 0;
        unsigned maxSetOccupancy = 0;
        unsigned roiMaxSetOccupancy = 0;

        ShadowTable() = default;
        ShadowTable(unsigned total_entries, unsigned table_ways)
            : sets(total_entries / table_ways), ways(table_ways),
              entries(total_entries), setOccupancy(sets, 0)
        {
        }
    };

    struct ShadowCounters
    {
        uint64_t committedUpdates = 0;
        uint64_t qfLookups = 0;
        uint64_t qfHits = 0;
        uint64_t qfMisses = 0;
        uint64_t qfPromotions = 0;
        uint64_t qfEvictions = 0;
        uint64_t qfQualifiedEvictions = 0;
        uint64_t pctFetchLookups = 0;
        uint64_t pctFetchHits = 0;
        uint64_t pctFetchMisses = 0;
        uint64_t pctCommitLookups = 0;
        uint64_t pctCommitHits = 0;
        uint64_t pctCommitMisses = 0;
        uint64_t pctMismatches = 0;
        uint64_t pctDemotions = 0;
        uint64_t pctEvictions = 0;
        uint64_t predictionOffers = 0;
        uint64_t predictionCorrect = 0;
        uint64_t predictionWrong = 0;
        uint64_t predictionEvictedBeforeCommit = 0;
        uint64_t predictionWrongAfterEviction = 0;
        uint64_t qfReuseReferences = 0;
        uint64_t qfReuseDistanceSum = 0;
        uint64_t qfReuseDistanceMax = 0;
        uint64_t pctReuseReferences = 0;
        uint64_t pctReuseDistanceSum = 0;
        uint64_t pctReuseDistanceMax = 0;
        uint64_t qfPeakOccupancy = 0;
        uint64_t pctPeakOccupancy = 0;
        uint64_t qfMaxSetOccupancy = 0;
        uint64_t pctMaxSetOccupancy = 0;
    };

    struct IdealConstantPredictionRecord : public VPPredictionRecord
    {
        uint64_t saturationEpoch = 0;
        uint64_t valueSegment = 0;
        RegVal saturatedValue = 0;
    };

    struct ShadowPredictionRecord : public IdealConstantPredictionRecord
    {
        bool pctHit = false;
        bool predictionOffered = false;
        RegVal shadowPredictedValue = 0;
        unsigned pctSet = 0;
        unsigned pctWay = 0;
        uint64_t pctGeneration = 0;
    };

    std::vector<std::unordered_map<Addr, ICEntry>> idealConstTables;
    std::vector<ProfileTable> lifetimeProfileTables;
    std::vector<ProfileTable> roiProfileTables;
    std::vector<uint64_t> lifetimeProfileUpdateSequences;
    std::vector<uint64_t> roiProfileUpdateSequences;
    std::vector<PredictionIntervals> lifetimePredictionIntervals;
    std::vector<PredictionIntervals> roiPredictionIntervals;
    std::vector<SaturatedValueProfile> lifetimeSaturatedValueProfiles;
    std::vector<SaturatedValueProfile> roiSaturatedValueProfiles;
    std::vector<ShadowTable> shadowQfTables;
    std::vector<ShadowTable> shadowPctTables;
    std::vector<ShadowCounters> lifetimeShadowCounters;
    std::vector<ShadowCounters> roiShadowCounters;
    std::vector<uint64_t> shadowUpdateSequences;

    // Each saturated (tid, PC) pair owns one table entry. Track its state
    // incrementally so capacity statistics never need to scan the table.
    uint64_t currentSaturatedPcs = 0;
    uint64_t lifetimePeakSaturatedPcs = 0;
    uint64_t roiPeakSaturatedPcs = 0;

    // Values are keyed by their complete RegVal bit pattern.  The refcount
    // lets a shared value register file's live capacity be tracked without
    // scanning all saturated PCs on every committed update.
    std::unordered_map<RegVal, uint64_t> saturatedValueRefCounts;
    uint64_t lifetimePeakDistinctSaturatedValues = 0;
    uint64_t roiPeakDistinctSaturatedValues = 0;

    const unsigned satCounterBits;
    const bool resetConfidence;
    const bool enableProfiling;
    const bool enableShadowProfiling;
    const unsigned shadowQfEntries;
    const unsigned shadowQfWays;
    const unsigned shadowPctEntries;
    const unsigned shadowPctWays;
    const unsigned shadowQualification;

    VPResult doPredict(Addr pc, ThreadID tid) const;
    void doUpdate(Addr pc, ThreadID tid, RegVal actualValue,
            const VPFeedback &feedback, bool is_misprediction,
            uint64_t committed_seq_no, uint64_t prediction_epoch);
    void updateProfile(ProfileTable &profile_table, Addr pc,
            uint64_t update_sequence, uint64_t committed_seq_no,
            ThreadID tid, PredictionIntervals &prediction_intervals,
            bool value_changed, bool was_saturated, bool is_saturated,
            uint64_t saturation_epoch_started,
            uint64_t saturation_epoch_ended,
            uint64_t prediction_epoch,
            bool offered_prediction, bool applied_prediction,
            bool correct_prediction,
            bool update_roi_stats);
    void observeSaturationTransition(bool was_saturated, bool is_saturated);
    void observeSaturatedValueChange(bool was_saturated, bool is_saturated,
            RegVal previous_value, RegVal current_value,
            bool value_changed);
    void addSaturatedValue(RegVal value);
    void removeSaturatedValue(RegVal value);
    void resetRoiSaturationStats();
    void refreshSaturationStats();
    void resetRoiProfile();
    void resetRoiSaturatedValueProfile();
    void openSaturatedValueSegment(SaturatedValueProfile &profile,
            ThreadID tid, Addr pc, uint64_t saturation_epoch,
            uint64_t value_segment, RegVal value,
            uint64_t saturation_start_seq_no, bool open_at_scope_start);
    void closeSaturatedValueSegment(SaturatedValueProfile &profile,
            Addr pc, uint64_t saturation_epoch, uint64_t value_segment,
            uint64_t saturation_end_seq_no);
    void observeSaturatedValuePrediction(ThreadID tid, Addr pc,
            uint64_t saturation_epoch, uint64_t value_segment,
            RegVal saturated_value, uint64_t committed_seq_no,
            bool correct_prediction);
    void refreshProfileStats();
    void dumpProfile() const;
    void dumpPredictionIntervals() const;
    void dumpSaturatedValues() const;

    static unsigned shadowIndex(Addr pc, unsigned sets);
    ShadowEntry *findShadowEntry(ShadowTable &table, Addr pc,
            unsigned &set, unsigned &way) const;
    const ShadowEntry *findShadowEntry(const ShadowTable &table, Addr pc,
            unsigned &set, unsigned &way) const;
    ShadowEntry *allocateShadowEntry(ShadowTable &table, Addr pc,
            RegVal value, bool qf, ShadowCounters &lifetime,
            ShadowCounters &roi,
            uint64_t commit_sequence, unsigned &set, unsigned &way);
    void invalidateShadowEntry(ShadowTable &table, unsigned set,
            unsigned way);
    void touchShadowEntry(ShadowTable &table, ShadowEntry &entry,
            uint64_t commit_sequence, bool qf,
            ShadowCounters &lifetime, ShadowCounters &roi);
    void observeShadowOccupancy(ShadowCounters &counters, ThreadID tid,
            bool roi) const;
    void shadowPredict(Addr pc, ThreadID tid,
            ShadowPredictionRecord &record);
    void shadowUpdate(Addr pc, ThreadID tid, RegVal actualValue,
            const VPPredictionRecord *record);
    void resetShadowRoiProfile();
    void refreshShadowStats();
    void dumpShadowProfile() const;

  public:
    IdealConstantLVP(const Params &params);

    std::string name() const override { return "IdealConstantLVP"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;

    void update(const VPUpdateInfo &updateInfo, const VPPredictionRecord *record,
            const VPFeedback &feedback) override;

    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;

    void squash(ThreadID tid, const uint64_t seq_no) override;

    virtual ValuePredType getValuePredictorType() override
    {
        return ValuePredType::IdealConstantLVP;
    }

    struct IdealConstantLVPStats : public statistics::Group
    {
        statistics::Scalar profileRoiUpdates;
        statistics::Scalar profileRoiDistinctPcs;
        statistics::Scalar profileRoiValueChanges;
        statistics::Scalar profileRoiSaturationTransitions;
        statistics::Scalar profileRoiEverSaturatedPcs;
        statistics::Scalar profileRoiSaturatedAtEndPcs;
        statistics::Scalar profileRoiPeakSaturatedPcs;
        statistics::Scalar profileRoiPeakDistinctSaturatedValues;
        statistics::Scalar profileRoiCommittedSaturatedOffers;
        statistics::Scalar profileRoiPredictionUses;
        statistics::Scalar profileRoiCorrectPredictionUses;
        statistics::Scalar profileLifetimePcsAtEnd;
        statistics::Scalar profileLifetimeEverSaturatedPcs;
        statistics::Scalar profileLifetimeSaturatedPcsAtEnd;
        statistics::Scalar profileLifetimePeakSaturatedPcs;
        statistics::Scalar profileLifetimePeakDistinctSaturatedValues;

        statistics::Scalar shadowRoiCommittedUpdates;
        statistics::Scalar shadowRoiQfLookups;
        statistics::Scalar shadowRoiQfHits;
        statistics::Scalar shadowRoiQfMisses;
        statistics::Scalar shadowRoiQfPromotions;
        statistics::Scalar shadowRoiQfEvictions;
        statistics::Scalar shadowRoiQfQualifiedEvictions;
        statistics::Scalar shadowRoiPctFetchLookups;
        statistics::Scalar shadowRoiPctFetchHits;
        statistics::Scalar shadowRoiPctFetchMisses;
        statistics::Scalar shadowRoiPctCommitLookups;
        statistics::Scalar shadowRoiPctCommitHits;
        statistics::Scalar shadowRoiPctCommitMisses;
        statistics::Scalar shadowRoiPctMismatches;
        statistics::Scalar shadowRoiPctDemotions;
        statistics::Scalar shadowRoiPctEvictions;
        statistics::Scalar shadowRoiPredictionOffers;
        statistics::Scalar shadowRoiPredictionCorrect;
        statistics::Scalar shadowRoiPredictionWrong;
        statistics::Scalar shadowRoiPredictionEvictedBeforeCommit;
        statistics::Scalar shadowRoiPredictionWrongAfterEviction;
        statistics::Scalar shadowRoiQfPeakOccupancy;
        statistics::Scalar shadowRoiPctPeakOccupancy;
        statistics::Scalar shadowRoiQfMaxSetOccupancy;
        statistics::Scalar shadowRoiPctMaxSetOccupancy;

        statistics::Scalar shadowLifetimeCommittedUpdates;
        statistics::Scalar shadowLifetimeQfPromotions;
        statistics::Scalar shadowLifetimeQfEvictions;
        statistics::Scalar shadowLifetimeQfQualifiedEvictions;
        statistics::Scalar shadowLifetimePctMismatches;
        statistics::Scalar shadowLifetimePctDemotions;
        statistics::Scalar shadowLifetimePctEvictions;
        statistics::Scalar shadowLifetimePredictionOffers;
        statistics::Scalar shadowLifetimePredictionCorrect;
        statistics::Scalar shadowLifetimePredictionWrong;
        statistics::Scalar shadowLifetimePredictionWrongAfterEviction;

        IdealConstantLVPStats(statistics::Group *parent);
    } profileStats;
};

} // namespace valuepred

} // namespace gem5

#endif // __IDEAL_CONSTANT_LVP_HH__
