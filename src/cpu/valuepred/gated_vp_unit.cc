#include "cpu/valuepred/gated_vp_unit.hh"

#include "base/logging.hh"
#include "base/types.hh"

namespace gem5
{

namespace valuepred
{

GatedVPUnit::GatedVPUnit(const Params &params)
    : VPUnit(params),
      shadowThresholdPercent(params.shadowThresholdPercent),
      gatestats(this)
{
    gem5_assert((params.shadowThresholdPercent >= 0.0) && (params.shadowThresholdPercent <= 1.0),
                "GatedVPUnit shadowThresholdPercent must be in [0.0, 1.0] \n");
}

bool
GatedVPUnit::shadowGateOpen() const
{
    if (shadowPredictedCount == 0) {
        return false;
    }

    const double shadowAccuracy =
        static_cast<double>(shadowCorrectCount) / static_cast<double>(shadowPredictedCount);
    return shadowAccuracy >= shadowThresholdPercent;
}

void
GatedVPUnit::updateShadowStats(const VPUpdateMetaData *updateMetadata)
{
    if (!updateMetadata->hasCandidatePrediction) {
        return;
    }

    const bool shadowCorrect = updateMetadata->candidateValue == updateMetadata->actualValue;
    shadowPredictedCount++;
    gatestats.shadowPredicted++;
    if (shadowCorrect) {
        shadowCorrectCount++;
        gatestats.shadowCorrected++;
    }
}

VPResult
GatedVPUnit::valuePredict(VPPredMetaData *predMetaData)
{
    gem5_assert(predMetaData, "can't pass nullptr to vpunit\n");
    VPResult predResult = valuePredictInternal(predMetaData);
    predResult.hasCandidate = predResult.hasCandidate || predResult.speculative;

    if (!predResult.hasCandidate) {
        return predResult;
    }

    if (!shadowGateOpen()) {
        predResult.speculative = false;
        gatestats.shadowGateBlocked++;
        return predResult;
    }

    gatestats.shadowGateAllowed++;
    return predResult;
}

void
GatedVPUnit::updateValuePredictor(VPUpdateMetaData *updateMetaData)
{
    gem5_assert(updateMetaData, "can't pass nullptr to vpunit\n");
    updateShadowStats(updateMetaData);
    updateValuePredictorInternal(updateMetaData);
}

} // namespace valuepred

} // namespace gem5
