#include <gtest/gtest.h>

#include <cstdint>

#include "cpu/valuepred/constant_lvp_policy.hh"

namespace gem5
{

namespace valuepred
{

namespace
{

TEST(ConstantLVPPolicyTest, CriticalCounterDecaysBelowFactor)
{
    constexpr uint16_t maximum = 3;

    EXPECT_EQ(constant_lvp::updatedCriticalCounter(3, 0, 4, maximum), 2);
    EXPECT_EQ(constant_lvp::updatedCriticalCounter(2, 3, 4, maximum), 1);
    EXPECT_EQ(constant_lvp::updatedCriticalCounter(0, 3, 4, maximum), 0);
}

TEST(ConstantLVPPolicyTest, CriticalCounterPreservesSeverity)
{
    constexpr uint16_t maximum = 3;

    EXPECT_EQ(constant_lvp::updatedCriticalCounter(0, 4, 4, maximum), 1);
    EXPECT_EQ(constant_lvp::updatedCriticalCounter(0, 7, 4, maximum), 1);
    EXPECT_EQ(constant_lvp::updatedCriticalCounter(0, 8, 4, maximum), 2);
    EXPECT_EQ(constant_lvp::updatedCriticalCounter(0, 12, 4, maximum), 3);
    EXPECT_EQ(constant_lvp::updatedCriticalCounter(2, 8, 4, maximum), 3);
    EXPECT_EQ(constant_lvp::updatedCriticalCounter(3, 4, 4, maximum), 3);
}

TEST(ConstantLVPPolicyTest, CriticalityLowersConfidenceThreshold)
{
    constexpr uint16_t base_threshold = 511;

    EXPECT_EQ(constant_lvp::effectiveConfidenceThreshold(
                  base_threshold, 0, 2),
              511);
    EXPECT_EQ(constant_lvp::effectiveConfidenceThreshold(
                  base_threshold, 1, 2),
              384);
    EXPECT_EQ(constant_lvp::effectiveConfidenceThreshold(
                  base_threshold, 2, 2),
              257);
    EXPECT_EQ(constant_lvp::effectiveConfidenceThreshold(
                  base_threshold, 3, 2),
              130);
}

TEST(ConstantLVPPolicyTest, ConfidenceThresholdNeverUnderflows)
{
    EXPECT_EQ(constant_lvp::effectiveConfidenceThreshold(3, 3, 1), 1);
}

TEST(ConstantLVPPolicyTest, ReplacementScoreCombinesCriticalityAndConfidence)
{
    EXPECT_EQ(constant_lvp::replacementScore(500, 0), 0);
    EXPECT_EQ(constant_lvp::replacementScore(100, 3), 300);
    EXPECT_LT(constant_lvp::replacementScore(100, 2),
              constant_lvp::replacementScore(300, 1));
}

} // anonymous namespace
} // namespace valuepred
} // namespace gem5
