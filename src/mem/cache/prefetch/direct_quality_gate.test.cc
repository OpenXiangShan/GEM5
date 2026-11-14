#include <gtest/gtest.h>

#include "mem/cache/prefetch/direct_quality_gate.hh"

namespace gem5
{
namespace prefetch
{
namespace
{

DirectQualityGate::Config
smallConfig()
{
    DirectQualityGate::Config config;
    config.qualityEntries = 4;
    config.qualityWays = 4;
    config.feedbackEntries = 4;
    config.feedbackWays = 4;
    config.horizon = 16;
    config.minSamples = 1;
    config.observeSamplePeriod = 1;
    config.openSamplePeriod = 1;
    config.blockProbePeriod = 1;
    config.borderlineBlockProbePeriod = 1;
    config.unusedPerUseful = 1;
    config.blockGuard = 0;
    config.strictUnusedPerUseful = 1;
    config.strictBlockGuard = 0;
    config.reopenUnusedPerUseful = 1;
    config.reopenGuard = 0;
    config.decayPeriod = 0;
    config.epochBits = 3;
    config.epochShift = 0;
    config.epochTimeout = 3;
    return config;
}

void
advancePastExpiry(DirectQualityGate &gate)
{
    for (unsigned index = 0; index < 8; ++index)
        gate.observeDemand(0x100000 + index * 64);
}

TEST(DirectQualityGate, DemandResolvesSampleAsUseful)
{
    DirectQualityGate gate(smallConfig());
    const auto decision = gate.admit(0x1000, 1, 0x2000, 0x3000);

    ASSERT_TRUE(decision.allowed);
    ASSERT_TRUE(decision.sampled);
    ASSERT_TRUE(decision.feedbackInserted);
    gate.observeDemand(0x3000);

    EXPECT_EQ(gate.useful(), 1);
    EXPECT_EQ(gate.unused(), 0);
}

TEST(DirectQualityGate, ExpiryResolvesSampleAsUnused)
{
    DirectQualityGate gate(smallConfig());
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);

    advancePastExpiry(gate);

    EXPECT_EQ(gate.useful(), 0);
    EXPECT_EQ(gate.unused(), 1);
    EXPECT_EQ(gate.feedbackExpiries(), 1);
}

TEST(DirectQualityGate, BadEntryBlocksAndUsefulProbeReopens)
{
    DirectQualityGate gate(smallConfig());
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    advancePastExpiry(gate);
    ASSERT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);

    const auto probe = gate.admit(0x1000, 1, 0x2040, 0x4000);
    ASSERT_TRUE(probe.allowed);
    ASSERT_TRUE(probe.feedbackInserted);
    gate.observeDemand(0x4000);

    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Open);
}

TEST(DirectQualityGate, KindKeepsLargeAndSmallQualitySeparate)
{
    DirectQualityGate gate(smallConfig());
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    advancePastExpiry(gate);

    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);
    EXPECT_EQ(gate.state(0x1000, 2), DirectQualityGate::State::Observe);
}

TEST(DirectQualityGate, DuplicateCandidateCoalescesFeedback)
{
    DirectQualityGate gate(smallConfig());
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    const auto duplicate = gate.admit(0x1000, 1, 0x2040, 0x3000);

    EXPECT_FALSE(duplicate.feedbackInserted);
    EXPECT_EQ(gate.sampled(), 1);
    EXPECT_EQ(gate.feedbackCoalesced(), 1);
}

TEST(DirectQualityGate, FeedbackReplacementIsUnknownNotUnused)
{
    auto config = smallConfig();
    config.feedbackEntries = 1;
    config.feedbackWays = 1;
    DirectQualityGate gate(config);
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    ASSERT_TRUE(gate.admit(0x2000, 1, 0x2040, 0x4000).feedbackInserted);

    EXPECT_EQ(gate.feedbackReplacements(), 1);
    EXPECT_EQ(gate.unknownDrops(), 1);
    EXPECT_EQ(gate.unused(), 0);
}

TEST(DirectQualityGate, ReplacedOwnerMakesOutcomeUnknown)
{
    auto config = smallConfig();
    config.qualityEntries = 1;
    config.qualityWays = 1;
    DirectQualityGate gate(config);
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    ASSERT_TRUE(gate.admit(0x2000, 1, 0x2040, 0x4000).feedbackInserted);

    gate.observeDemand(0x3000);

    EXPECT_EQ(gate.useful(), 0);
    EXPECT_EQ(gate.unused(), 0);
    EXPECT_EQ(gate.orphanOutcomes(), 1);
    EXPECT_EQ(gate.unknownDrops(), 1);
}

TEST(DirectQualityGate, NonCanonicalAddressUsesStableFoldedKey)
{
    DirectQualityGate gate(smallConfig());
    constexpr Addr candidate = UINT64_C(0x0001000000001000);
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, candidate).feedbackInserted);

    gate.observeDemand(candidate);

    EXPECT_EQ(gate.useful(), 1);
    EXPECT_EQ(gate.nonCanonicalFeedbackCandidates(), 1);
    EXPECT_EQ(gate.nonCanonicalFeedbackDemands(), 1);
}

}  // anonymous namespace
}  // namespace prefetch
}  // namespace gem5
