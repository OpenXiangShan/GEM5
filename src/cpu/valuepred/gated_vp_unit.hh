#ifndef __GATED_VP_UNIT_HH__
#define __GATED_VP_UNIT_HH__

#include <cstdint>

#include "base/stats/units.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "params/GatedVPUnit.hh"

namespace gem5
{

namespace valuepred
{

// Optional wrapper layer on top of VPUnit.
//
// Inheritance with shadow gate:
//   YourPredictor -> GatedVPUnit -> VPUnit
//
// Main role:
//   Control whether a predictor can do *real* speculative value prediction
//   through a shadow-accuracy gate.
//
// Lifecycle:
//   1) Training:
//      updateValuePredictor() is called at commit.
//      GatedVPUnit updates shadow accuracy counters, then calls
//      updateValuePredictorInternal() to train algorithm state (table/confidence).
//   2) Prediction gate:
//      valuePredict() calls valuePredictInternal() first to get a candidate.
//      If no candidate exists, return directly.
//      If a candidate exists but shadow accuracy is below threshold, force
//      speculative=false (shadow-only behavior, no real VP effects).
//   3) Real prediction:
//      If shadow accuracy reaches threshold, keep speculative=true and allow
//      normal VP behavior in downstream pipeline stages.
//
// Typical example (shadowThresholdPercent = 0.99):
//   - shadowCorrect=1200, shadowPredicted=1210 => 99.17% >= 99%:
//     real value prediction is allowed.
//   - shadowCorrect=300, shadowPredicted=400 => 75% < 99%:
//     candidate values are generated but blocked from real speculation.
//
// For an algorithm inheriting from GatedVPUnit:
//   - Implement valuePredictInternal() and updateValuePredictorInternal()
//     for algorithm-specific behavior.
//   - Still implement inherited VPUnit pure virtual methods:
//       1) specUpdateValuePredictor()
//       2) squash()
//       3) getValuePredictorType()
class GatedVPUnit : public VPUnit
{
  private:
    using Params = GatedVPUnitParams;

    const double shadowThresholdPercent;
    uint64_t shadowPredictedCount = 0;
    uint64_t shadowCorrectCount = 0;

    bool shadowGateOpen() const;
    void updateShadowStats(const VPUpdateMetaData *updateMetadata);

  protected:
    GatedVPUnit(const Params &params);

    // Return raw predictor behavior before shadow gate is applied.
    virtual VPResult valuePredictInternal(VPPredMetaData *predMetaData) = 0;
    // Train algorithm state; shadow counters are already handled by wrapper.
    virtual void updateValuePredictorInternal(VPUpdateMetaData *updateMetaData) = 0;

  public:
    // Wrapper entry: candidate generation + shadow-accuracy gate decision.
    VPResult valuePredict(VPPredMetaData *predMetaData) final;
    // Wrapper entry: shadow accounting + algorithm training.
    void updateValuePredictor(VPUpdateMetaData *updateMetaData) final;

    struct GatedVPUnitStats : public statistics::Group
    {
        statistics::Scalar shadowPredicted;
        statistics::Scalar shadowCorrected;
        statistics::Formula shadowAccuracy;
        statistics::Scalar shadowGateAllowed;
        statistics::Scalar shadowGateBlocked;

        GatedVPUnitStats(statistics::Group *parent)
            : statistics::Group(parent),
              ADD_STAT(shadowPredicted, statistics::units::Count::get(),
                       "number of shadow predictions"),
              ADD_STAT(shadowCorrected, statistics::units::Count::get(),
                       "number of correct shadow predictions"),
              ADD_STAT(shadowAccuracy, statistics::units::Ratio::get(),
                       "shadow prediction accuracy", shadowCorrected / shadowPredicted),
              ADD_STAT(shadowGateAllowed, statistics::units::Count::get(),
                       "number of predictions allowed by shadow gate"),
              ADD_STAT(shadowGateBlocked, statistics::units::Count::get(),
                       "number of predictions blocked by shadow gate")
        {
        }
    } gatestats;
};

} // namespace valuepred

} // namespace gem5

#endif // __GATED_VP_UNIT_HH__
