#include <gtest/gtest.h>

#include <memory>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/cache/prefetch/direct_quality_gate.hh"
#include "mem/cache/replacement_policies/lru_rp.hh"
#include "mem/cache/replacement_policies/tree_plru_rp.hh"
#include "params/LRURP.hh"
#include "params/TreePLRURP.hh"

namespace gem5
{
namespace prefetch
{
namespace
{

GTestTickHandler tickHandler;

struct TestGate
{
    DirectQualityGate::Config config;
    TreePLRURPParams treeParams;
    LRURPParams lruParams;
    std::unique_ptr<replacement_policy::Base> replacementPolicy;
    std::unique_ptr<DirectQualityGate> gate;

    explicit TestGate(const DirectQualityGate::Config &config_) : config(config_)
    {
        treeParams.name = "DirectQualityGateTest.TreePLRU";
        treeParams.eventq_index = 0;
        treeParams.num_leaves = config.qualityWays;
        lruParams.name = "DirectQualityGateTest.LRU";
        lruParams.eventq_index = 0;
        if (config.qualityWays == 1)
            replacementPolicy = std::make_unique<replacement_policy::LRU>(lruParams);
        else
            replacementPolicy = std::make_unique<replacement_policy::TreePLRU>(treeParams);
        gate = std::make_unique<DirectQualityGate>(config, replacementPolicy.get());
    }
};

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
    TestGate test(smallConfig());
    auto &gate = *test.gate;
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
    TestGate test(smallConfig());
    auto &gate = *test.gate;
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);

    advancePastExpiry(gate);

    EXPECT_EQ(gate.useful(), 0);
    EXPECT_EQ(gate.unused(), 1);
    EXPECT_EQ(gate.feedbackExpiries(), 1);
}

TEST(DirectQualityGate, BadEntryBlocksAndUsefulProbeReopens)
{
    TestGate test(smallConfig());
    auto &gate = *test.gate;
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
    TestGate test(smallConfig());
    auto &gate = *test.gate;
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    advancePastExpiry(gate);

    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);
    EXPECT_EQ(gate.state(0x1000, 2), DirectQualityGate::State::Observe);
}

TEST(DirectQualityGate, DuplicateCandidateCoalescesFeedback)
{
    TestGate test(smallConfig());
    auto &gate = *test.gate;
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
    TestGate test(config);
    auto &gate = *test.gate;
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
    TestGate test(config);
    auto &gate = *test.gate;
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    ASSERT_TRUE(gate.admit(0x2000, 1, 0x2040, 0x4000).feedbackInserted);

    gate.observeDemand(0x3000);

    EXPECT_EQ(gate.useful(), 0);
    EXPECT_EQ(gate.unused(), 0);
    EXPECT_EQ(gate.orphanOutcomes(), 1);
    EXPECT_EQ(gate.unknownDrops(), 1);
}

TEST(DirectQualityGate, InvalidQualityWaysAreUsedBeforeReplacement)
{
    TestGate test(smallConfig());
    auto &gate = *test.gate;
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, 0x3000).feedbackInserted);
    advancePastExpiry(gate);
    ASSERT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);

    for (unsigned index = 1; index < 4; ++index) {
        const Addr pc = 0x1000 + index * 0x1000;
        const auto decision = gate.admit(pc, 1, 0x2000 + index * 64, 0x3000 + index * 64);
        ASSERT_EQ(decision.state, DirectQualityGate::State::Observe);
    }

    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);
}

TEST(DirectQualityGate, FeedbackAddressFoldingUsesStableKey)
{
    TestGate test(smallConfig());
    auto &gate = *test.gate;
    constexpr Addr candidate = UINT64_C(0x0001000000001000);
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x2000, candidate).feedbackInserted);

    gate.observeDemand(candidate);

    EXPECT_EQ(gate.useful(), 1);
    EXPECT_EQ(gate.feedbackFoldedCandidates(), 1);
    EXPECT_EQ(gate.feedbackFoldedDemands(), 1);
}

}  // anonymous namespace
}  // namespace prefetch
}  // namespace gem5
