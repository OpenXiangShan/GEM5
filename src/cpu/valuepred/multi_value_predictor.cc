#include "cpu/valuepred/multi_value_predictor.hh"

#include <algorithm>
#include <limits>
#include <memory>

#include "base/logging.hh"
#include "cpu/valuepred/valuepred_metadata.hh"

namespace gem5
{

namespace valuepred
{

MultiValuePredictor::MultiValuePredictor(const Params &params)
    : VPUnit(params),
      predictors(params.predictors.begin(), params.predictors.end()),
      dynamicArb(params.dynamicArb),
      arbCounterBits(params.arbCounterBits),
      maxArbScore(0),
      lastChosenPredictor(0)
{
    fatal_if(predictors.empty(), "MultiValuePredictor requires at least one sub predictor.");
    fatal_if(arbCounterBits == 0 || arbCounterBits > 30,
             "MultiValuePredictor arbCounterBits must be in [1, 30].");

    maxArbScore = (1 << arbCounterBits) - 1;
    arbScores.assign(predictors.size(), maxArbScore / 2);
    lastChosenPredictor = predictors.size() - 1;
}

size_t
MultiValuePredictor::chooseValidPredictor(const std::vector<bool> &validMask) const
{
    std::vector<size_t> validIndices;
    validIndices.reserve(validMask.size());
    for (size_t i = 0; i < validMask.size(); i++) {
        if (validMask[i]) {
            validIndices.push_back(i);
        }
    }

    if (validIndices.empty()) {
        return predictors.size();
    }

    if (validIndices.size() == 1 || !dynamicArb) {
        return validIndices.front();
    }

    int bestScore = std::numeric_limits<int>::min();
    for (auto idx : validIndices) {
        bestScore = std::max(bestScore, arbScores[idx]);
    }

    std::vector<size_t> bestIndices;
    bestIndices.reserve(validIndices.size());
    for (auto idx : validIndices) {
        if (arbScores[idx] == bestScore) {
            bestIndices.push_back(idx);
        }
    }

    if (bestIndices.size() == 1) {
        return bestIndices.front();
    }

    for (size_t step = 1; step <= predictors.size(); step++) {
        size_t nextIdx = (lastChosenPredictor + step) % predictors.size();
        if (std::find(bestIndices.begin(), bestIndices.end(), nextIdx) != bestIndices.end()) {
            return nextIdx;
        }
    }

    return bestIndices.front();
}

void
MultiValuePredictor::updateArbScore(size_t predictorIdx, bool isMisprediction)
{
    if (!dynamicArb || predictorIdx >= arbScores.size()) {
        return;
    }

    if (isMisprediction) {
        arbScores[predictorIdx] = std::max(0, arbScores[predictorIdx] - 1);
    } else {
        arbScores[predictorIdx] = std::min(maxArbScore, arbScores[predictorIdx] + 1);
    }
}

VPResult
MultiValuePredictor::valuePredict(VPPredMetaData *predMetaData)
{
    gem5_assert(predMetaData, "can't pass nullptr to vpunit\n");

    std::vector<VPResult> subPredResults(predictors.size());
    std::vector<bool> validMask(predictors.size(), false);

    for (size_t i = 0; i < predictors.size(); i++) {
        std::unique_ptr<VPPredMetaData> subMetaData(
            VPDataStructFactory::buildPredMetaData(predictors[i]->getValuePredictorType()));
        // Generic split: copy all common fields + extension fields once.
        subMetaData->copyFrom(*predMetaData);
        subPredResults[i] = predictors[i]->valuePredict(subMetaData.get());
        validMask[i] = subPredResults[i].speculative;
    }

    inflightPredResults[predMetaData->seq_no] = subPredResults;

    size_t chosenPredictor = chooseValidPredictor(validMask);
    if (chosenPredictor >= predictors.size()) {
        selectedPredictorBySeq.erase(predMetaData->seq_no);
        return {false, 0, false, ValuePredType::NullPredictor};
    }

    selectedPredictorBySeq[predMetaData->seq_no] = chosenPredictor;
    lastChosenPredictor = chosenPredictor;

    VPResult finalResult = subPredResults[chosenPredictor];
    finalResult.predictionSource = predictors[chosenPredictor]->getValuePredictorType();
    return finalResult;
}

void
MultiValuePredictor::updateValuePredictor(VPUpdateMetaData *updateMetaData)
{
    gem5_assert(updateMetaData, "can't pass nullptr to vpunit\n");

    auto resultIt = inflightPredResults.find(updateMetaData->seq_no);
    auto chosenIt = selectedPredictorBySeq.find(updateMetaData->seq_no);
    const bool hasInflightResult = resultIt != inflightPredResults.end();

    size_t chosenPredictor = predictors.size();
    if (chosenIt != selectedPredictorBySeq.end()) {
        chosenPredictor = chosenIt->second;
        updateArbScore(chosenPredictor, updateMetaData->isMisprediction);
    }

    for (size_t i = 0; i < predictors.size(); i++) {
        std::unique_ptr<VPUpdateMetaData> subMetaData(
            VPDataStructFactory::buildUpdateMetaData(predictors[i]->getValuePredictorType()));
        // Generic split: preserve all metadata carried by caller.
        subMetaData->copyFrom(*updateMetaData);
        // Only selected predictor should receive misprediction penalty signal.
        subMetaData->isMisprediction = updateMetaData->isMisprediction && (i == chosenPredictor);

        // Keep per-sub predictor stats in sync with top-level commit accounting.
        predictors[i]->stats.VPsupported++;
        if (hasInflightResult && i < resultIt->second.size()) {
            const auto &subPredResult = resultIt->second[i];
            subMetaData->hasCandidatePrediction = subPredResult.hasCandidate;
            subMetaData->candidateValue = subPredResult.value;
            subMetaData->predictionSource = subPredResult.predictionSource;

            if (subPredResult.speculative) {
                predictors[i]->stats.VPpredicted++;
                if (subPredResult.value == updateMetaData->actualValue) {
                    predictors[i]->stats.VPcorrected++;
                }
            }
        } else {
            subMetaData->hasCandidatePrediction = false;
            subMetaData->candidateValue = 0;
            subMetaData->predictionSource = ValuePredType::NullPredictor;
        }

        predictors[i]->updateValuePredictor(subMetaData.get());
    }

    if (hasInflightResult) {
        inflightPredResults.erase(resultIt);
    }
    if (chosenIt != selectedPredictorBySeq.end()) {
        selectedPredictorBySeq.erase(chosenIt);
    }
}

void
MultiValuePredictor::specUpdateValuePredictor(VPSpecUpdateMetaData *specUpdateMetaData)
{
    gem5_assert(specUpdateMetaData, "can't pass nullptr to vpunit\n");

    for (size_t i = 0; i < predictors.size(); i++) {
        std::unique_ptr<VPSpecUpdateMetaData> subMetaData(
            VPDataStructFactory::buildSpecUpdateMetaData(predictors[i]->getValuePredictorType()));
        subMetaData->copyFrom(*specUpdateMetaData);
        predictors[i]->specUpdateValuePredictor(subMetaData.get());
    }
}

void
MultiValuePredictor::squash(const uint64_t seq_no)
{
    for (auto *predictor : predictors) {
        predictor->squash(seq_no);
    }

    // Keep the violator entry for commit-time training/statistics, and only
    // drop younger instructions.
    auto resultIt = inflightPredResults.upper_bound(seq_no);
    inflightPredResults.erase(resultIt, inflightPredResults.end());

    auto chosenIt = selectedPredictorBySeq.upper_bound(seq_no);
    selectedPredictorBySeq.erase(chosenIt, selectedPredictorBySeq.end());
}

} // namespace valuepred

} // namespace gem5
