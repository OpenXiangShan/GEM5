/*
 * VTAGE is largely based on the open-source implementation of the
 * 1st-place Championship Value Prediction (CVP-1) submission.
 *
 * The version in this codebase is adapted and modified for our environment.
 * It is not intended to be a bit-exact copy of the original code.
 *
 * For detailed background and reference material:
 * Paper: https://microarch.org/cvp1/papers/Seznec.pdf
 * Open-source implementation: https://www.microarch.org/cvp1/code/Seznec.tar.gz
 * Official website: https://www.microarch.org/cvp1/
 */

#ifndef __CPU_VALUEPRED_VTAGE_HH__
#define __CPU_VALUEPRED_VTAGE_HH__

#include <array>
#include <cstdint>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "base/random.hh"
#include "base/types.hh"
#include "cpu/op_class.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/VTAGE.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

struct FetchTarget;

} // namespace btb_pred

} // namespace branch_prediction

namespace valuepred
{

class LoadTrainInfoExt;

class VTAGE : public VPUnit
{
  private:
    using Params = VTAGEParams;
    static constexpr unsigned ValueWays = 3;

    struct VTAGEEntry
    {
        uint32_t hashOrValue = 0;
        uint32_t conf = 0;
        uint32_t tag = 0;
        uint32_t useful = 0;
        bool valid = false;
    };

    struct ValueEntry
    {
        RegVal data = 0;
        uint32_t useful = 0;
        bool valid = false;
    };

    struct TrainInfo
    {
        bool cacheHit = false;
        uint32_t observedLatencyCycles = 0;
        bool latencyValid = false;
        uint8_t numSrcRegs = 0;
        OpClass opClass = No_OpClass;
        bool criticalLoad = false;
    };

    class VTAGEPredictionRecord : public VPPredictionRecord
    {
      public:
        std::vector<uint32_t> indices;
        std::vector<uint32_t> tags;
        int hitBank = -1;
        bool historyAvailable = false;
        bool pointerHit = false;
        bool hashOnlyHit = false;
        bool backoffBlocked = false;
    };

    const unsigned numHistories;
    const unsigned numBanks;
    const std::vector<unsigned> histLengths;
    const bool requireHistoryExt;
    const unsigned logBankSize;
    const unsigned bankSize;
    const unsigned tagBits;
    const unsigned confBits;
    const unsigned usefulBits;
    const unsigned logValueArrayEntries;
    const unsigned valueEntriesPerWay;
    const unsigned totalValueEntries;
    const unsigned predictConfThreshold;
    const unsigned hashOnlyUpgradeThreshold;
    const uint64_t mispredBackoffDistance;
    const unsigned agingTickMax;
    const unsigned agingPenaltyOnAlloc;
    const unsigned agingPenaltyOnNoAlloc;
    const uint32_t l1HitMaxCycles;
    const uint32_t l2HitMaxCycles;
    const uint32_t llcHitMaxCycles;
    const uint32_t fastInstCycles;
    const uint32_t mfastInstCycles;
    const bool enableStochasticTraining;
    const uint32_t rngSeed;
    const double allocProbLoadL1Hit;
    const double allocProbLoadMiss;
    const double confIncProbLowValue;
    const double confIncProbFastLoad;
    const double uIncProbFastLoad;
    const double valueArrayUpgradeProb;
    const double shortHistoryAllocBias;
    const double deepAllocExtraHopProb;

    Random rng;

    std::vector<std::vector<std::vector<VTAGEEntry>>> tables;
    std::vector<std::vector<ValueEntry>> valueArrays;
    std::vector<unsigned> agingTicks;
    std::vector<uint64_t> lastSelectedMispredictSeq;
    std::vector<bool> hasSelectedMispredictSeq;

    struct VTAGEStats : public statistics::Group
    {
        statistics::Vector predictHitBank;
        statistics::Scalar predictPointerHit;
        statistics::Scalar predictHashOnlyHit;
        statistics::Scalar predictBackoffBlocked;
        statistics::Scalar commitSatisfiedHit;
        statistics::Scalar commitMismatchedHit;
        statistics::Scalar allocHashOnly;
        statistics::Scalar allocLongHistory;
        statistics::Scalar upgradeToPointer;
        statistics::Scalar valueArrayHit;
        statistics::Scalar valueArraySteal;
        statistics::Scalar agingPasses;
        statistics::Scalar missingHistoryReq;

        explicit VTAGEStats(statistics::Group *parent)
            : statistics::Group(parent),
              ADD_STAT(predictHitBank, statistics::units::Count::get(),
                       "Per-bank tagged hits seen at predict time"),
              ADD_STAT(predictPointerHit, statistics::units::Count::get(),
                       "Predict-time hits on pointer-backed entries"),
              ADD_STAT(predictHashOnlyHit, statistics::units::Count::get(),
                       "Predict-time hits on hash-only entries"),
              ADD_STAT(predictBackoffBlocked, statistics::units::Count::get(),
                       "Predictions suppressed by the VTAGE backoff window"),
              ADD_STAT(commitSatisfiedHit, statistics::units::Count::get(),
                       "Commit-time hits whose value/hash matched the actual load result"),
              ADD_STAT(commitMismatchedHit, statistics::units::Count::get(),
                       "Commit-time hits whose value/hash mismatched the actual load result"),
              ADD_STAT(allocHashOnly, statistics::units::Count::get(),
                       "New hash-only VTAGE allocations"),
              ADD_STAT(allocLongHistory, statistics::units::Count::get(),
                       "Allocations placed in a bank longer than the previous hit bank"),
              ADD_STAT(upgradeToPointer, statistics::units::Count::get(),
                       "Hash-only entries upgraded to point at the value array"),
              ADD_STAT(valueArrayHit, statistics::units::Count::get(),
                       "Value-array lookups that found an existing matching value"),
              ADD_STAT(valueArraySteal, statistics::units::Count::get(),
                       "Value-array insertions that claimed a replacement slot"),
              ADD_STAT(agingPasses, statistics::units::Count::get(),
                       "Global usefulness-aging passes"),
              ADD_STAT(missingHistoryReq, statistics::units::Count::get(),
                       "Predict requests that did not provide FetchTarget history")
        {
        }
    } vtageStats;

  private:
    uint32_t bitMask(unsigned bits) const;
    uint32_t maxConf() const;
    uint32_t maxUseful() const;
    uint64_t mix64(uint64_t value) const;
    bool chance(double probability);
    bool isFastLoad(const TrainInfo &train_info) const;
    bool isMFastLoad(const TrainInfo &train_info) const;
    bool isLowValue(RegVal value) const;
    uint32_t foldHistory(const boost::dynamic_bitset<> &history,
            unsigned length, unsigned out_bits, uint64_t salt) const;
    void fillIndicesAndTags(const branch_prediction::btb_pred::FetchTarget &fetch_target,
            Addr pc, std::vector<uint32_t> &indices,
            std::vector<uint32_t> &tags) const;
    uint32_t hashActualValue(RegVal value) const;
    uint32_t makeHashToken(RegVal value) const;
    bool entryMatchesActualValue(ThreadID tid, const VTAGEEntry &entry,
            RegVal actual_value) const;
    std::array<uint32_t, ValueWays> valueArrayIndices(RegVal actual_value) const;
    TrainInfo decodeTrainInfo(const VPUpdateInfo &update_info) const;
    bool shouldIncreaseConfidence(const TrainInfo &train_info,
            RegVal actual_value);
    bool shouldIncreaseUseful(const TrainInfo &train_info);
    bool shouldAllocateEntry(const TrainInfo &train_info);
    bool shouldUpgradeValueArray(const TrainInfo &train_info);
    unsigned chooseAllocationStartBank(int hit_bank);
    bool tryUpgradeToPointer(ThreadID tid, VTAGEEntry &entry,
            RegVal actual_value, const TrainInfo &train_info);
    void ageEntries(ThreadID tid);
    void advanceAging(ThreadID tid, unsigned delta);
    bool backoffActive(ThreadID tid, uint64_t seq_no) const;

  public:
    explicit VTAGE(const Params &params);

    std::string name() const override { return "VTAGE"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;

    void update(const VPUpdateInfo &update_info,
            const VPPredictionRecord *record,
            const VPFeedback &feedback) override;

    void specUpdate(const VPSpecUpdateInfo &spec_update_info) override;

    void squash(ThreadID tid, const uint64_t seq_no) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::VTAGE;
    }
};

} // namespace valuepred

} // namespace gem5

#endif // __CPU_VALUEPRED_VTAGE_HH__
