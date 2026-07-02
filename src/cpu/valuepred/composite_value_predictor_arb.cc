#include "cpu/valuepred/composite_value_predictor_arb.hh"

#include <vector>

#include "base/random.hh"

namespace gem5
{

namespace valuepred
{

int
CompositeValuePredictorFixedPriorityArb::choose(
        const std::vector<VPChooserCandidate> &candidates)
{
    for (const auto &candidate : candidates) {
        if (candidate.speculative) {
            return candidate.childIndex;
        }
    }
    return -1;
}

int
CompositeValuePredictorRandomArb::choose(
        const std::vector<VPChooserCandidate> &candidates)
{
    std::vector<int> speculativeChildren;
    speculativeChildren.reserve(candidates.size());
    for (const auto &candidate : candidates) {
        if (candidate.speculative) {
            speculativeChildren.push_back(candidate.childIndex);
        }
    }

    if (speculativeChildren.empty()) {
        return -1;
    }

    return speculativeChildren[random_mt.random<size_t>(
            0, speculativeChildren.size() - 1)];
}

int
CompositeValuePredictorRRArb::choose(
        const std::vector<VPChooserCandidate> &candidates)
{
    if (candidates.empty()) {
        return -1;
    }

    const size_t count = candidates.size();
    size_t start = 0;
    if (nextChild >= 0) {
        start = static_cast<size_t>(nextChild) % count;
    }

    for (size_t offset = 0; offset < count; ++offset) {
        const size_t idx = (start + offset) % count;
        if (candidates[idx].speculative) {
            nextChild = (candidates[idx].childIndex + 1) % count;
            return candidates[idx].childIndex;
        }
    }

    return -1;
}

void
CompositeValuePredictorConfidenceArb::ensureCapacity(size_t num_children)
{
    if (childConfidence.size() >= num_children) {
        return;
    }

    childConfidence.reserve(num_children);
    while (childConfidence.size() < num_children) {
        childConfidence.emplace_back(counterBits);
    }
}

int
CompositeValuePredictorConfidenceArb::choose(
        const std::vector<VPChooserCandidate> &candidates)
{
    ensureCapacity(candidates.size());

    int bestChild = -1;
    uint8_t bestConfidence = 0;
    bool foundSpeculative = false;
    for (const auto &candidate : candidates) {
        if (!candidate.speculative) {
            continue;
        }

        const auto confidence =
            static_cast<uint8_t>(childConfidence[candidate.childIndex]);
        if (!foundSpeculative || confidence > bestConfidence) {
            foundSpeculative = true;
            bestConfidence = confidence;
            bestChild = candidate.childIndex;
        }
    }

    return bestChild;
}

void
CompositeValuePredictorConfidenceArb::update(
        const std::vector<VPChooserCandidate> &candidates,
        const std::vector<VPChooserFeedback> &feedbacks)
{
    ensureCapacity(candidates.size());

    for (const auto &feedback : feedbacks) {
        if (feedback.childIndex < 0) {
            continue;
        }
        if (!feedback.offeredPrediction) {
            continue;
        }

        if (feedback.wouldHaveBeenCorrect) {
            ++childConfidence[feedback.childIndex];
        } else {
            --childConfidence[feedback.childIndex];
        }
    }
}

} // namespace valuepred

} // namespace gem5
