#include <gtest/gtest.h>

#include "cpu/pred/btb/final_prediction_selector.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace
{

constexpr int RasSource = 10;
constexpr int IttageSource = 11;
constexpr int TageSource = 12;
constexpr int MbtbSource = 13;

FinalPredictionSelectorConfig
makeSelectorConfig(bool ittage_hit = false)
{
    FinalPredictionSelectorConfig config;
    config.rasSource = RasSource;
    config.ittageSource = IttageSource;
    config.tageSource = TageSource;
    config.mbtbSource = MbtbSource;
    config.ittageHit = ittage_hit;
    return config;
}

BranchSlot
makeSlot(Addr pc, Addr target, bool is_cond, bool is_indirect,
         bool is_call = false, bool is_return = false, uint8_t size = 4)
{
    BranchSlot slot;
    slot.pc = pc;
    slot.target = target;
    slot.setTypeFromFlags(is_cond, is_indirect, !is_cond && !is_indirect,
                          is_call, is_return);
    slot.size = size;
    return slot;
}

BTBEntry
makeEntry(Addr pc, Addr target, bool is_cond = false,
          bool is_indirect = false, bool is_call = false,
          bool is_return = false, int source = -1)
{
    BTBEntry entry(
        makeSlot(pc, target, is_cond, is_indirect, is_call, is_return));
    entry.source = source;
    return entry;
}

BTBEntry
makeNotTakenCondEntry(Addr pc, Addr target, int source = -1)
{
    auto entry = makeEntry(pc, target, true, false, false, false, source);
    entry.ctr = -1;
    entry.alwaysTaken = false;
    return entry;
}

std::vector<FullBTBPrediction>
makeStagePreds(unsigned num_stages = 4)
{
    std::vector<FullBTBPrediction> stage_preds(num_stages);
    for (unsigned stage = 0; stage < num_stages; ++stage) {
        stage_preds[stage].bbStart = 0x1000;
        stage_preds[stage].predSource = stage;
    }
    return stage_preds;
}

TEST(FinalPredictionSelectorTest, ChoosesLatestStageWithEntries)
{
    auto stage_preds = makeStagePreds();
    stage_preds[1].btbEntries = {makeEntry(0x1010, 0x1200)};
    stage_preds[3].btbEntries = {makeNotTakenCondEntry(0x1020, 0x1300)};
    stage_preds[3].condTakens = {{0x1020, false}};

    const auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());

    EXPECT_EQ(selection.chosenStage, 3);
    EXPECT_FALSE(stage_preds[selection.chosenStage].isTaken());
}

TEST(FinalPredictionSelectorTest, EmptyPredictionsFallBackToStageZero)
{
    const auto stage_preds = makeStagePreds();

    const auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());

    EXPECT_EQ(selection.chosenStage, 0);
    EXPECT_EQ(selection.firstMatchingStage, 0);
    EXPECT_EQ(selection.overrideReason, OverrideReason::NO_OVERRIDE);
    EXPECT_FALSE(selection.updateAheadFromLastStage);
}

TEST(FinalPredictionSelectorTest, FirstMatchingStageKeepsEarlierMismatchReason)
{
    auto stage_preds = makeStagePreds();
    stage_preds[0].btbEntries = {makeNotTakenCondEntry(0x1010, 0x1200)};
    stage_preds[0].condTakens = {{0x1010, false}};
    stage_preds[1].btbEntries = {makeEntry(0x1020, 0x1300)};
    stage_preds[3].btbEntries = {makeEntry(0x1020, 0x1300)};

    const auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());

    EXPECT_EQ(selection.chosenStage, 3);
    EXPECT_EQ(selection.firstMatchingStage, 1);
    EXPECT_EQ(selection.overrideReason, OverrideReason::FALL_THRU);
}

TEST(FinalPredictionSelectorTest, S1SourceUsesFirstTakenLikeEntrySource)
{
    auto stage_preds = makeStagePreds();
    stage_preds[0].btbEntries = {
        makeNotTakenCondEntry(0x1008, 0x1100, 3),
        makeEntry(0x1010, 0x1200, false, false, false, false, 7),
    };

    const auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());

    EXPECT_EQ(selection.s1Source, 7);
}

TEST(FinalPredictionSelectorTest, S3SourceClassifiesTakenEntry)
{
    auto stage_preds = makeStagePreds();
    stage_preds[2].btbEntries = {
        makeEntry(0x1010, 0x1200, false, true, false, true),
    };

    auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());
    EXPECT_EQ(selection.s3Source, RasSource);

    stage_preds[2].btbEntries = {makeEntry(0x1010, 0x1200, false, true)};
    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig(true));
    EXPECT_EQ(selection.s3Source, IttageSource);

    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig(false));
    EXPECT_EQ(selection.s3Source, MbtbSource);

    stage_preds[2].btbEntries = {makeEntry(0x1010, 0x1200, true)};
    stage_preds[2].condTakens = {{0x1010, true}};
    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());
    EXPECT_EQ(selection.s3Source, TageSource);

    stage_preds[2].btbEntries = {makeEntry(0x1010, 0x1200)};
    stage_preds[2].condTakens.clear();
    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());
    EXPECT_EQ(selection.s3Source, MbtbSource);
}

TEST(FinalPredictionSelectorTest, IttageHitProviderIsLazy)
{
    auto stage_preds = makeStagePreds();
    unsigned ittage_hit_calls = 0;
    const auto ittage_hit = [&ittage_hit_calls]() {
        ittage_hit_calls++;
        return true;
    };

    stage_preds[2].btbEntries = {makeEntry(0x1010, 0x1200)};
    auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig(),
                              ittage_hit);
    EXPECT_EQ(selection.s3Source, MbtbSource);
    EXPECT_EQ(ittage_hit_calls, 0);

    stage_preds[2].btbEntries = {
        makeEntry(0x1010, 0x1200, false, true, false, true),
    };
    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig(),
                              ittage_hit);
    EXPECT_EQ(selection.s3Source, RasSource);
    EXPECT_EQ(ittage_hit_calls, 0);

    stage_preds[2].btbEntries = {makeEntry(0x1010, 0x1200, true)};
    stage_preds[2].condTakens = {{0x1010, true}};
    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig(),
                              ittage_hit);
    EXPECT_EQ(selection.s3Source, TageSource);
    EXPECT_EQ(ittage_hit_calls, 0);

    stage_preds[2].btbEntries = {makeEntry(0x1010, 0x1200, false, true)};
    stage_preds[2].condTakens.clear();
    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig(),
                              ittage_hit);
    EXPECT_EQ(selection.s3Source, IttageSource);
    EXPECT_EQ(ittage_hit_calls, 1);
}

TEST(FinalPredictionSelectorTest, S3TakenLikeEntryPlusCondCanAttributeToTage)
{
    auto stage_preds = makeStagePreds();
    stage_preds[2].btbEntries = {
        makeEntry(0x1010, 0x1200),
        makeNotTakenCondEntry(0x1020, 0x1300),
    };
    stage_preds[3].btbEntries = {makeNotTakenCondEntry(0x1030, 0x1400)};
    stage_preds[3].condTakens = {{0x1030, false}};

    const auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());

    EXPECT_EQ(selection.chosenStage, 3);
    EXPECT_FALSE(stage_preds[selection.chosenStage].isTaken());
    EXPECT_EQ(selection.s3Source, TageSource);
}

TEST(FinalPredictionSelectorTest, AheadUpdateUsesLastStage)
{
    auto stage_preds = makeStagePreds();
    stage_preds[2].btbEntries = {makeEntry(0x1010, 0x1200)};

    auto selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());
    EXPECT_FALSE(selection.updateAheadFromLastStage);

    stage_preds[3].btbEntries = {makeEntry(0x1020, 0x1300)};
    selection =
        selectFinalPrediction(stage_preds, 64, makeSelectorConfig());
    EXPECT_TRUE(selection.updateAheadFromLastStage);
}

} // namespace

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
