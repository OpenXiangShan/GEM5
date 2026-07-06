#ifndef __CPU_VALUEPRED_COMPOSITE_VALUE_PREDICTOR_HH__
#define __CPU_VALUEPRED_COMPOSITE_VALUE_PREDICTOR_HH__

#include <memory>
#include <vector>

#include "cpu/valuepred/composite_value_predictor_arb.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/CompositeValuePredictor.hh"

namespace gem5
{

namespace valuepred
{

class CompositePredictionRecord : public VPPredictionRecord
{
  public:
    std::vector<VPChooserCandidate> chooserCandidates;

    struct ChildRecord
    {
        std::unique_ptr<VPPredictionRecord> record;
        VPResult result = {};
        bool selected = false;
    };

    std::vector<ChildRecord> children;
    int selectedChild = -1;
};

class CompositeValuePredictor : public VPUnit
{
  private:
    using Params = CompositeValuePredictorParams;

    std::vector<VPUnit *> predictors;
    CompositeValuePredictorArb *arb;

    struct CompositeStats : public statistics::Group
    {
        statistics::Vector childOffered;
        statistics::Vector childSelected;
        statistics::Vector childWouldHaveBeenCorrect;
        statistics::Scalar multiCandidatePredictions;

        CompositeStats(statistics::Group *parent)
            : statistics::Group(parent),
              ADD_STAT(childOffered, statistics::units::Count::get(),
                       "Per-child offered speculative predictions"),
              ADD_STAT(childSelected, statistics::units::Count::get(),
                       "Per-child selected predictions"),
              ADD_STAT(childWouldHaveBeenCorrect, statistics::units::Count::get(),
                       "Per-child predictions that would have been correct"),
              ADD_STAT(multiCandidatePredictions, statistics::units::Count::get(),
                       "Predictions where multiple children offered speculative values")
        {
        }
    } compositeStats;

  public:
    CompositeValuePredictor(const Params &params);

    std::string name() const override { return "CompositeValuePredictor"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;

    void update(const VPUpdateInfo &updateInfo, const VPPredictionRecord *record,
            const VPFeedback &feedback) override;

    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;

    void squash(ThreadID tid, const uint64_t seq_no) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::CompositeValuePredictor;
    }
};

} // namespace valuepred

} // namespace gem5

#endif
