#ifndef __CPU_VALUEPRED_MULTI_VALUE_PREDICTOR_HH__
#define __CPU_VALUEPRED_MULTI_VALUE_PREDICTOR_HH__

#include <cstdint>
#include <map>
#include <vector>

#include "cpu/valuepred/valuepred_unit.hh"
#include "params/MultiValuePredictor.hh"

namespace gem5
{

namespace valuepred
{

class MultiValuePredictor : public VPUnit
{
  private:
    using Params = MultiValuePredictorParams;

    std::vector<VPUnit *> predictors;
    const bool dynamicArb;
    const unsigned arbCounterBits;
    int maxArbScore;
    std::vector<int> arbScores;

    // Arbitration tie-breaker cursor.
    size_t lastChosenPredictor;

    // Per-inflight metadata for source-aware update.
    std::map<uint64_t, std::vector<VPResult>> inflightPredResults;
    std::map<uint64_t, size_t> selectedPredictorBySeq;

    size_t chooseValidPredictor(const std::vector<bool> &validMask) const;
    void updateArbScore(size_t predictorIdx, bool isMisprediction);

  public:
    MultiValuePredictor(const Params &params);

    std::string name() const override { return "MultiValuePredictor"; }

    VPResult valuePredict(VPPredMetaData *predMetaData) override;

    void updateValuePredictor(VPUpdateMetaData *updateMetaData) override;

    void specUpdateValuePredictor(VPSpecUpdateMetaData *specUpdateMetaData) override;

    void squash(const uint64_t seq_no) override;

    ValuePredType getValuePredictorType() override
    {
        return ValuePredType::MultiValuePredictor;
    }
};

} // namespace valuepred

} // namespace gem5

#endif // __CPU_VALUEPRED_MULTI_VALUE_PREDICTOR_HH__
