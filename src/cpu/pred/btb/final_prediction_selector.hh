#ifndef __CPU_PRED_BTB_FINAL_PREDICTION_SELECTOR_HH__
#define __CPU_PRED_BTB_FINAL_PREDICTION_SELECTOR_HH__

#include <cassert>
#include <utility>
#include <vector>

#include "base/types.hh"
#include "cpu/pred/btb/common.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

struct FinalPredictionSelectorConfig
{
    int rasSource = -1;
    int ittageSource = -1;
    int tageSource = -1;
    int mbtbSource = -1;
    bool ittageHit = false;
    unsigned s1SourceStage = 0;
    unsigned s3SourceStage = 2;
};

struct FinalPredictionSelection
{
    unsigned chosenStage = 0;
    unsigned firstMatchingStage = 0;
    OverrideReason overrideReason = OverrideReason::NO_OVERRIDE;
    int s1Source = -1;
    int s3Source = -1;
    bool updateAheadFromLastStage = false;
};

inline unsigned
chooseFinalPredictionStage(const std::vector<FullBTBPrediction> &stage_preds)
{
    assert(!stage_preds.empty());

    for (int stage = static_cast<int>(stage_preds.size()) - 1; stage >= 0;
         --stage) {
        if (!stage_preds[stage].btbEntries.empty()) {
            return stage;
        }
    }

    return 0;
}

inline int
findS1PredictionSource(const std::vector<FullBTBPrediction> &stage_preds,
                       unsigned stage)
{
    if (stage >= stage_preds.size()) {
        return -1;
    }

    for (const auto &entry : stage_preds[stage].btbEntries) {
        if (entry.slot.isIndirect() || entry.slot.isDirect() ||
            entry.ctr >= 0 || entry.alwaysTaken) {
            return entry.source;
        }
    }

    return -1;
}

template <typename IttageHitProvider>
inline int
findS3PredictionSource(const std::vector<FullBTBPrediction> &stage_preds,
                       unsigned chosen_stage,
                       const FinalPredictionSelectorConfig &config,
                       IttageHitProvider ittage_hit)
{
    if (config.s3SourceStage >= stage_preds.size()) {
        return -1;
    }

    const auto &s3_pred = stage_preds[config.s3SourceStage];
    bool found_s3_taken = false;
    bool na_s3_taken_but_have_cond = false;

    for (const auto &entry : s3_pred.btbEntries) {
        if (entry.slot.isDirect() || entry.slot.isIndirect() ||
            entry.ctr >= 0 || entry.alwaysTaken) {
            found_s3_taken = true;
        } else if (entry.slot.isCond()) {
            // Only used when there is no taken prediction in final selection.
            na_s3_taken_but_have_cond = true;
        }
    }

    if (!found_s3_taken) {
        return -1;
    }

    const auto pred_taken_entry = stage_preds[chosen_stage].getTakenEntry();
    if (!pred_taken_entry.valid) {
        return na_s3_taken_but_have_cond ? config.tageSource : -1;
    }

    if (pred_taken_entry.slot.isReturn()) {
        return config.rasSource;
    }

    if (pred_taken_entry.slot.isIndirect() && ittage_hit()) {
        return config.ittageSource;
    }

    if (pred_taken_entry.slot.isCond()) {
        return config.tageSource;
    }

    return config.mbtbSource;
}

inline std::pair<unsigned, OverrideReason>
findFirstMatchingStage(const std::vector<FullBTBPrediction> &stage_preds,
                       unsigned chosen_stage, Addr predict_width)
{
    assert(!stage_preds.empty());

    const auto &chosen_pred = stage_preds[chosen_stage];
    unsigned first_matching_stage = 0;
    OverrideReason override_reason = OverrideReason::NO_OVERRIDE;

    while (first_matching_stage < stage_preds.size() - 1) {
        const auto [matches, reason] =
            stage_preds[first_matching_stage].match(chosen_pred,
                                                    predict_width);
        if (matches) {
            break;
        }

        first_matching_stage++;
        override_reason = reason;
    }

    return std::make_pair(first_matching_stage, override_reason);
}

template <typename IttageHitProvider>
inline FinalPredictionSelection
selectFinalPrediction(const std::vector<FullBTBPrediction> &stage_preds,
                      Addr predict_width,
                      const FinalPredictionSelectorConfig &config,
                      IttageHitProvider ittage_hit)
{
    assert(!stage_preds.empty());

    FinalPredictionSelection selection;
    selection.chosenStage = chooseFinalPredictionStage(stage_preds);
    selection.s1Source =
        findS1PredictionSource(stage_preds, config.s1SourceStage);
    selection.s3Source =
        findS3PredictionSource(stage_preds, selection.chosenStage,
                               config, ittage_hit);

    const auto [first_matching_stage, override_reason] =
        findFirstMatchingStage(stage_preds, selection.chosenStage,
                               predict_width);
    selection.firstMatchingStage = first_matching_stage;
    selection.overrideReason = override_reason;
    selection.updateAheadFromLastStage = !stage_preds.back().btbEntries.empty();

    return selection;
}

inline FinalPredictionSelection
selectFinalPrediction(const std::vector<FullBTBPrediction> &stage_preds,
                      Addr predict_width,
                      const FinalPredictionSelectorConfig &config)
{
    return selectFinalPrediction(stage_preds, predict_width, config,
                                 [&config]() {
                                     return config.ittageHit;
                                 });
}

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5

#endif // __CPU_PRED_BTB_FINAL_PREDICTION_SELECTOR_HH__
