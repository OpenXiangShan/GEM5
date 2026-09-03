#include <gtest/gtest.h>

#include "cpu/pred/btb/common.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace test
{
namespace
{

constexpr Addr StartPC = 0x1000;
constexpr Addr BranchPC = 0x1008;
constexpr Addr PredictWidth = 64;

FullBTBPrediction
makeFallThroughPrediction()
{
    FullBTBPrediction prediction;
    prediction.bbStart = StartPC;
    return prediction;
}

FullBTBPrediction
makeTakenPrediction(Addr target)
{
    auto prediction = makeFallThroughPrediction();
    BTBEntry entry;
    entry.valid = true;
    entry.pc = BranchPC;
    entry.target = target;
    entry.isDirect = true;
    entry.size = 4;
    prediction.btbEntries.push_back(entry);
    return prediction;
}

} // anonymous namespace

TEST(PredictionStageTest, UsefulS2CorrectionFinishesAtS2)
{
    auto s1 = makeFallThroughPrediction();
    auto s2 = makeTakenPrediction(0x2000);
    auto s3 = s2;

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, false);

    EXPECT_TRUE(result.s2Override);
    EXPECT_FALSE(result.s3Override);
    EXPECT_TRUE(result.s2Useful);
    EXPECT_EQ(result.readyStage, 1);
}

TEST(PredictionStageTest, HarmfulS2PingPongFinishesAtS3)
{
    auto s1 = makeTakenPrediction(0x2000);
    auto s2 = makeFallThroughPrediction();
    auto s3 = s1;

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, false);

    EXPECT_TRUE(result.s2Override);
    EXPECT_TRUE(result.s3Override);
    EXPECT_TRUE(result.s2Harmful);
    EXPECT_EQ(result.readyStage, 2);
}

TEST(PredictionStageTest, WrongToWrongS2StillFinishesAtS3)
{
    auto s1 = makeTakenPrediction(0x2000);
    auto s2 = makeTakenPrediction(0x3000);
    auto s3 = makeTakenPrediction(0x4000);

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, true);

    EXPECT_TRUE(result.s2Override);
    EXPECT_TRUE(result.s3Override);
    EXPECT_TRUE(result.s2WrongToWrong);
    EXPECT_EQ(result.readyStage, 2);
}

TEST(PredictionStageTest, SuppressedTargetDifferenceAvoidsHarmfulOverride)
{
    auto s1 = makeTakenPrediction(0x2000);
    auto s2 = makeTakenPrediction(0x3000);
    auto s3 = s1;

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, false);

    EXPECT_FALSE(result.s2Override);
    EXPECT_TRUE(result.s2Valid);
    EXPECT_FALSE(result.s3Override);
    EXPECT_TRUE(result.s2SuppressedTargetOnly);
    EXPECT_TRUE(result.s2SuppressedTargetWouldHarm);
    EXPECT_EQ(result.readyStage, 0);
}

TEST(PredictionStageTest, SuppressedHelpfulTargetWaitsForS3)
{
    auto s1 = makeTakenPrediction(0x2000);
    auto s2 = makeTakenPrediction(0x3000);
    auto s3 = s2;

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, false);

    EXPECT_FALSE(result.s2Override);
    EXPECT_TRUE(result.s3Override);
    EXPECT_TRUE(result.s2SuppressedTargetOnly);
    EXPECT_TRUE(result.s2SuppressedTargetWouldHelp);
    EXPECT_EQ(result.readyStage, 2);
}

TEST(PredictionStageTest, InvalidS2HasNoOpinion)
{
    auto s1 = makeTakenPrediction(0x2000);
    auto s2 = makeFallThroughPrediction();
    auto s3 = s1;

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, false, false);

    EXPECT_FALSE(result.s2Override);
    EXPECT_FALSE(result.s2Valid);
    EXPECT_FALSE(result.s3Override);
    EXPECT_FALSE(result.s2Harmful);
    EXPECT_EQ(result.readyStage, 0);
}

TEST(PredictionStageTest, InvalidS2CanStillBeCorrectedByS3)
{
    auto s1 = makeFallThroughPrediction();
    auto s2 = makeFallThroughPrediction();
    auto s3 = makeTakenPrediction(0x2000);

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, false, false);

    EXPECT_FALSE(result.s2Override);
    EXPECT_TRUE(result.s3Override);
    EXPECT_EQ(result.readyStage, 2);
}

TEST(PredictionStageTest, SuppressedTargetsCanBothDifferFromS3)
{
    auto s1 = makeTakenPrediction(0x2000);
    auto s2 = makeTakenPrediction(0x3000);
    auto s3 = makeTakenPrediction(0x4000);

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, false);

    EXPECT_FALSE(result.s2Override);
    EXPECT_TRUE(result.s3Override);
    EXPECT_TRUE(result.s2SuppressedTargetOnly);
    EXPECT_TRUE(result.s2SuppressedTargetWrongToWrong);
    EXPECT_EQ(result.readyStage, 2);
}

TEST(PredictionStageTest, OverrideReasonDescribesLastVisibleCorrection)
{
    auto s1 = makeFallThroughPrediction();
    auto s2 = makeTakenPrediction(0x2000);
    auto s3 = makeTakenPrediction(0x3000);

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, true);

    EXPECT_TRUE(result.s2Override);
    EXPECT_TRUE(result.s3Override);
    EXPECT_EQ(result.overrideReason, OverrideReason::TARGET);
    EXPECT_EQ(result.readyStage, 2);
}

TEST(PredictionStageTest, S2StillChecksBranchAttributes)
{
    auto s1 = makeTakenPrediction(0x2000);
    auto s2 = s1;
    s2.btbEntries[0].isDirect = false;
    auto s3 = s2;

    auto result = evaluateThreeStageOverrides(
        s1, s2, s3, PredictWidth, true, false);

    EXPECT_TRUE(result.s2Override);
    EXPECT_FALSE(result.s3Override);
    EXPECT_TRUE(result.s2Useful);
    EXPECT_EQ(result.overrideReason, OverrideReason::ATTRIBUTE);
    EXPECT_EQ(result.readyStage, 1);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
