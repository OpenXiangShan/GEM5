#ifndef __IDEAL_CONSTANT_LVP_HH__
#define __IDEAL_CONSTANT_LVP_HH__

#include <cstdint>
#include <unordered_map>
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
        bool everSaturated = false;
    };

    using ProfileTable = std::unordered_map<Addr, ProfileEntry>;

    std::vector<std::unordered_map<Addr, ICEntry>> idealConstTables;
    std::vector<ProfileTable> lifetimeProfileTables;
    std::vector<ProfileTable> roiProfileTables;
    std::vector<uint64_t> lifetimeProfileUpdateSequences;
    std::vector<uint64_t> roiProfileUpdateSequences;

    const unsigned satCounterBits;
    const bool resetConfidence;
    const bool enableProfiling;

    VPResult doPredict(Addr pc, ThreadID tid) const;
    void doUpdate(Addr pc, ThreadID tid, RegVal actualValue);
    void updateProfile(ProfileTable &profile_table, Addr pc,
            uint64_t update_sequence,
            bool value_changed, bool was_saturated, bool is_saturated,
            bool update_roi_stats);
    void resetRoiProfile();
    void refreshProfileStats();
    void dumpProfile() const;

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
        statistics::Scalar profileLifetimePcsAtEnd;
        statistics::Scalar profileLifetimeEverSaturatedPcs;
        statistics::Scalar profileLifetimeSaturatedPcsAtEnd;

        IdealConstantLVPStats(statistics::Group *parent);
    } profileStats;
};

} // namespace valuepred

} // namespace gem5

#endif // __IDEAL_CONSTANT_LVP_HH__
