#include "cpu/valuepred/composite_value_predictor.hh"

#include "base/trace.hh"
#include "debug/VPCOMMON.hh"

namespace gem5
{

namespace valuepred
{

CompositeValuePredictor::CompositeValuePredictor(const Params &params)
    : VPUnit(params),
      predictors(params.predictors.begin(), params.predictors.end()),
      arb(params.arb),
      compositeStats(this)
{
    fatal_if(predictors.empty(),
            "CompositeValuePredictor requires at least one child predictor");
    fatal_if(!arb,
            "CompositeValuePredictor requires a non-null arb policy");
    for (auto *predictor : predictors) {
        fatal_if(predictor->getValuePredictorType() == ValuePredType::MemoryRenaming,
                "CompositeValuePredictor now only supports direct value predictors");
    }

    compositeStats.childOffered
        .init(predictors.size())
        .flags(statistics::nozero);
    compositeStats.childSelected
        .init(predictors.size())
        .flags(statistics::nozero);
    compositeStats.childWouldHaveBeenCorrect
        .init(predictors.size())
        .flags(statistics::nozero);

    for (size_t i = 0; i < predictors.size(); ++i) {
        const auto childName = std::to_string(i) + ":" + predictors[i]->name();
        compositeStats.childOffered.subname(i, childName);
        compositeStats.childSelected.subname(i, childName);
        compositeStats.childWouldHaveBeenCorrect.subname(i, childName);
    }
}

VPPredictionCandidate
CompositeValuePredictor::predict(const VPPredictRequest &request)
{
    auto compositeRecord = std::make_unique<CompositePredictionRecord>();
    compositeRecord->chooserCandidates.reserve(predictors.size());
    compositeRecord->children.reserve(predictors.size());
    std::vector<VPChooserCandidate> chooserCandidates;
    chooserCandidates.reserve(predictors.size());

    VPPredictionCandidate compositeCandidate;
    for (size_t i = 0; i < predictors.size(); ++i) {
        auto childCandidate = predictors[i]->predict(request);
        DPRINTF(VPCOMMON,
                "[VPComposite][predict] tid=%u seq=%llu child=%d type=%d speculative=%d value=%#llx\n",
                request.tid, request.seqNo, static_cast<int>(i),
                static_cast<int>(predictors[i]->getValuePredictorType()),
                childCandidate.result.speculative,
                static_cast<unsigned long long>(childCandidate.result.value));

        CompositePredictionRecord::ChildRecord childRecord;
        childRecord.result = childCandidate.result;
        childRecord.record = std::move(childCandidate.record);
        compositeRecord->children.emplace_back(std::move(childRecord));
        chooserCandidates.push_back({
            .childIndex = static_cast<int>(i),
            .predictorType = predictors[i]->getValuePredictorType(),
            .speculative = childCandidate.result.speculative,
            .value = childCandidate.result.value,
            .score = childCandidate.score,
        });
    }
    compositeRecord->chooserCandidates = chooserCandidates;

    compositeRecord->selectedChild = arb->choose(chooserCandidates);
    if (compositeRecord->selectedChild != -1) {
        auto &selectedChild =
            compositeRecord->children[compositeRecord->selectedChild];
        selectedChild.selected = true;
        compositeCandidate.result = selectedChild.result;
    }

    if (compositeRecord->selectedChild != -1) {
        compositeRecord->offeredPrediction = true;
        compositeRecord->predictedValue = compositeCandidate.result.value;
    }

    DPRINTF(VPCOMMON,
            "[VPComposite][predict] tid=%u seq=%llu selected=%d value=%#llx\n",
            request.tid, request.seqNo, compositeRecord->selectedChild,
            static_cast<unsigned long long>(compositeCandidate.result.value));

    compositeCandidate.record = std::move(compositeRecord);
    return compositeCandidate;
}

void
CompositeValuePredictor::dispatch(const VPDispatchInfo &dispatchInfo,
        VPPredictionRecord *record)
{
    auto *compositeRecord = dynamic_cast<CompositePredictionRecord *>(record);
    gem5_assert(compositeRecord,
            "CompositeValuePredictor expects CompositePredictionRecord");
    gem5_assert(compositeRecord->children.size() == predictors.size(),
            "Composite record size mismatch");

    for (size_t i = 0; i < predictors.size(); ++i) {
        predictors[i]->dispatch(dispatchInfo,
                compositeRecord->children[i].record.get());
    }
}

VPPredictionCandidate
CompositeValuePredictor::latePredict(const VPLatePredictRequest &request,
        VPPredictionRecord *record)
{
    VPPredictionCandidate compositeCandidate;
    auto *compositeRecord = dynamic_cast<CompositePredictionRecord *>(record);
    gem5_assert(compositeRecord,
            "CompositeValuePredictor expects CompositePredictionRecord");
    gem5_assert(compositeRecord->children.size() == predictors.size(),
            "Composite record size mismatch");

    // A value already propagated at rename must not be replaced at issue.
    if (compositeRecord->selectedChild != -1) {
        return compositeCandidate;
    }

    std::vector<VPPredictionCandidate> childCandidates(predictors.size());
    std::vector<VPChooserCandidate> chooserCandidates;
    chooserCandidates.reserve(predictors.size());
    for (size_t i = 0; i < predictors.size(); ++i) {
        childCandidates[i] = predictors[i]->latePredict(request,
                compositeRecord->children[i].record.get());
        compositeRecord->children[i].result = childCandidates[i].result;
        chooserCandidates.push_back({
            .childIndex = static_cast<int>(i),
            .predictorType = predictors[i]->getValuePredictorType(),
            .speculative = childCandidates[i].result.speculative,
            .value = childCandidates[i].result.value,
            .score = childCandidates[i].score,
        });
    }

    const int selected = arb->choose(chooserCandidates);
    if (selected == -1) {
        return compositeCandidate;
    }

    auto &childRecord = compositeRecord->children[selected];
    childRecord.selected = true;
    compositeRecord->chooserCandidates[selected] = chooserCandidates[selected];
    compositeRecord->selectedChild = selected;
    compositeRecord->offeredPrediction = true;
    compositeRecord->predictedValue = childRecord.result.value;
    compositeCandidate.result = childRecord.result;
    compositeCandidate.score = childCandidates[selected].score;

    DPRINTF(VPCOMMON,
            "[VPComposite][late] tid=%u seq=%llu selected=%d value=%#llx\n",
            request.tid, request.seqNo, selected,
            static_cast<unsigned long long>(compositeCandidate.result.value));
    return compositeCandidate;
}

void
CompositeValuePredictor::valueAvailable(
        const VPValueAvailableInfo &valueInfo,
        VPPredictionRecord *record)
{
    auto *compositeRecord = dynamic_cast<CompositePredictionRecord *>(record);
    gem5_assert(compositeRecord,
            "CompositeValuePredictor expects CompositePredictionRecord");
    gem5_assert(compositeRecord->children.size() == predictors.size(),
            "Composite record size mismatch");

    for (size_t i = 0; i < predictors.size(); ++i) {
        predictors[i]->valueAvailable(valueInfo,
                compositeRecord->children[i].record.get());
    }
}

void
CompositeValuePredictor::predictionApplied(
        const VPPredictionAppliedInfo &appliedInfo,
        VPPredictionRecord *record)
{
    auto *compositeRecord = dynamic_cast<CompositePredictionRecord *>(record);
    gem5_assert(compositeRecord,
            "CompositeValuePredictor expects CompositePredictionRecord");
    gem5_assert(compositeRecord->children.size() == predictors.size(),
            "Composite record size mismatch");

    for (size_t i = 0; i < predictors.size(); ++i) {
        auto child_info = appliedInfo;
        child_info.producedByReceiver =
            compositeRecord->selectedChild == static_cast<int>(i);
        predictors[i]->predictionApplied(child_info,
                compositeRecord->children[i].record.get());
    }
}

void
CompositeValuePredictor::valueMispredicted(
        const VPMispredictionInfo &mispInfo,
        VPPredictionRecord *record)
{
    auto *compositeRecord = dynamic_cast<CompositePredictionRecord *>(record);
    gem5_assert(compositeRecord,
            "CompositeValuePredictor expects CompositePredictionRecord");
    gem5_assert(compositeRecord->children.size() == predictors.size(),
            "Composite record size mismatch");

    for (size_t i = 0; i < predictors.size(); ++i) {
        predictors[i]->valueMispredicted(mispInfo,
                compositeRecord->children[i].record.get());
    }
}

void
CompositeValuePredictor::commitInstruction(const VPCommitInfo &commitInfo)
{
    for (auto *predictor : predictors) {
        predictor->commitInstruction(commitInfo);
    }
}

void
CompositeValuePredictor::update(const VPUpdateInfo &updateInfo,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    auto *compositeRecord = dynamic_cast<const CompositePredictionRecord *>(record);
    gem5_assert(compositeRecord,
            "CompositeValuePredictor expects CompositePredictionRecord");
    gem5_assert(compositeRecord->children.size() == predictors.size(),
            "Composite record size mismatch");
    gem5_assert(compositeRecord->chooserCandidates.size() == predictors.size(),
            "Composite chooser candidate size mismatch");

    std::vector<VPChooserFeedback> chooserFeedbacks;
    chooserFeedbacks.reserve(predictors.size());
    int offeredChildren = 0;
    for (size_t i = 0; i < predictors.size(); ++i) {
        VPFeedback childFeedback;
        childFeedback.selected = compositeRecord->children[i].selected;
        childFeedback.applied = feedback.applied && childFeedback.selected;
        childFeedback.offeredPrediction =
            compositeRecord->children[i].result.speculative;
        childFeedback.predictedValue =
            compositeRecord->children[i].result.value;
        childFeedback.wouldHaveBeenCorrect =
            childFeedback.offeredPrediction &&
            childFeedback.predictedValue == updateInfo.actualValue;
        childFeedback.overallPredictionMade = feedback.overallPredictionMade;
        childFeedback.overallPredictionCorrect =
            feedback.overallPredictionCorrect;
        if (childFeedback.offeredPrediction) {
            offeredChildren++;
            compositeStats.childOffered[i]++;
        }
        if (childFeedback.selected) {
            compositeStats.childSelected[i]++;
        }
        if (childFeedback.wouldHaveBeenCorrect) {
            compositeStats.childWouldHaveBeenCorrect[i]++;
        }
        DPRINTF(VPCOMMON,
                "[VPComposite][update] tid=%u seq=%llu child=%d "
                "selected=%d applied=%d offered=%d predicted=%#llx "
                "correct=%d\n",
                updateInfo.tid, updateInfo.seqNo, static_cast<int>(i),
                childFeedback.selected,
                childFeedback.applied, childFeedback.offeredPrediction,
                static_cast<unsigned long long>(childFeedback.predictedValue),
                childFeedback.wouldHaveBeenCorrect);
        chooserFeedbacks.push_back({
            .childIndex = static_cast<int>(i),
            .selected = childFeedback.selected,
            .offeredPrediction = childFeedback.offeredPrediction,
            .wouldHaveBeenCorrect = childFeedback.wouldHaveBeenCorrect,
        });
        predictors[i]->update(updateInfo,
                compositeRecord->children[i].record.get(), childFeedback);
        predictors[i]->stats.VPsupported++;
        if (childFeedback.offeredPrediction) {
            predictors[i]->stats.VPpredicted++;
            if (childFeedback.wouldHaveBeenCorrect) {
                predictors[i]->stats.VPcorrected++;
            }
        }
    }

    if (offeredChildren > 1) {
        compositeStats.multiCandidatePredictions++;
    }

    arb->update(compositeRecord->chooserCandidates, chooserFeedbacks);
}

void
CompositeValuePredictor::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    for (auto *predictor : predictors) {
        predictor->specUpdate(specUpdateInfo);
    }
}

void
CompositeValuePredictor::squash(ThreadID tid, const uint64_t seq_no)
{
    for (auto *predictor : predictors) {
        predictor->squash(tid, seq_no);
    }
}

void
CompositeValuePredictor::onVirtSQThrottleRise()
{
    for (auto *predictor : predictors) {
        predictor->onVirtSQThrottleRise();
    }
}

void
CompositeValuePredictor::setVirtSQThrottleHold(bool hold)
{
    for (auto *predictor : predictors) {
        predictor->setVirtSQThrottleHold(hold);
    }
}

} // namespace valuepred

} // namespace gem5
