#include "cpu/valuepred/example_value_predictor.hh"

namespace gem5
{

namespace valuepred
{

ExampleValuePredictor::ExampleValuePredictor(const Params &params)
    : VPUnit(params)
{
}

VPPredictionCandidate
ExampleValuePredictor::predict(const VPPredictRequest &request)
{
    assertValidTid(request.tid);

    const auto *exampleExt = request.getExt<ExamplePredictRequestExt>();

    VPPredictionCandidate candidate;
    // Even though this template predictor does not make a prediction yet, it
    // still allocates a record so users can see how predict-time private state
    // survives until update().
    const Tick predictTick = exampleExt ? exampleExt->predictTick : 0;
    candidate.record = std::make_unique<ExampleValuePredictionRecord>(
            predictTick);

    // This predictor intentionally does not make a prediction. Replace the
    // empty body below with your own lookup logic and fill candidate.result.
    //
    // exampleExt shows how to read predictor-specific request inputs, and
    // candidate.record already shows one way to persist private predict-time
    // state for the later update() call.
    //
    // Example:
    // candidate.result.speculative = true;
    // candidate.result.value = predicted_value;
    // candidate.score = chooser_score;
    return candidate;
}

void
ExampleValuePredictor::update(const VPUpdateInfo &updateInfo,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    assertValidTid(updateInfo.tid);

    const auto *exampleExt = updateInfo.getExt<ExampleUpdateInfoExt>();
    const auto *exampleRecord =
        dynamic_cast<const ExampleValuePredictionRecord *>(record);
    const Tick predictTick = exampleRecord ? exampleRecord->predictTick : 0;
    (void)exampleExt;
    (void)predictTick;
    (void)feedback;

    // This predictor intentionally has no training logic. Use exampleExt to
    // read predictor-specific commit-time inputs, and exampleRecord to recover
    // private state that was produced in predict(), such as predictTick above.
}

void
ExampleValuePredictor::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    (void)specUpdateInfo;
    // Leave empty until the predictor needs speculative state updates.
}

void
ExampleValuePredictor::squash(ThreadID tid, const uint64_t seq_no)
{
    (void)tid;
    (void)seq_no;
    // Leave empty until the predictor needs to discard in-flight state.
}

} // namespace valuepred

} // namespace gem5
