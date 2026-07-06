#ifndef __CPU_VALUEPRED_EXAMPLE_VALUE_PREDICTOR_HH__
#define __CPU_VALUEPRED_EXAMPLE_VALUE_PREDICTOR_HH__

#include "cpu/valuepred/example_value_predictor_metadata.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/ExampleValuePredictor.hh"

namespace gem5
{

namespace valuepred
{

class ExampleValuePredictionRecord : public VPPredictionRecord
{
  public:
    explicit ExampleValuePredictionRecord(Tick predict_tick)
        : predictTick(predict_tick)
    {
    }

    // Example of predictor-private state that is produced in predict() and
    // later recovered in update().
    Tick predictTick = 0;
};

class ExampleValuePredictor : public VPUnit
{
  private:
    using Params = ExampleValuePredictorParams;

  public:
    explicit ExampleValuePredictor(const Params &params);

    std::string name() const override { return "ExampleValuePredictor"; }

    VPPredictionCandidate predict(const VPPredictRequest &request) override;

    void update(const VPUpdateInfo &updateInfo, const VPPredictionRecord *record,
            const VPFeedback &feedback) override;

    void specUpdate(const VPSpecUpdateInfo &specUpdateInfo) override;

    void squash(ThreadID tid, const uint64_t seq_no) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::ExampleValuePredictor;
    }
};

} // namespace valuepred

} // namespace gem5

#endif // __CPU_VALUEPRED_EXAMPLE_VALUE_PREDICTOR_HH__
