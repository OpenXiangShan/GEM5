#include <gtest/gtest.h>

#include "mem/cache/prefetch/direct_quality_gate.hh"

namespace gem5
{
namespace prefetch
{

DirectQualityGate::Config
testConfig()
{
    DirectQualityGate::Config config;
    config.qualityEntries = 4;
    config.qualityWays = 1;
    config.feedbackEntries = 4;
    config.feedbackWays = 1;
    config.minSamples = 2;
    config.observeSamplePeriod = 1;
    config.openSamplePeriod = 1;
    config.blockProbePeriod = 4;
    config.borderlineBlockProbePeriod = 2;
    config.reopenProbePeriod = 1;
    config.unusedPerUseful = 1;
    config.blockGuard = 0;
    config.reopenUnusedPerUseful = 1;
    config.reopenGuard = 0;
    return config;
}

TEST(DirectQualityGate, BlocksAtTenToOneAndReopens)
{
    DirectQualityGate gate(testConfig());
    auto decision = gate.admit(0x1000, 1, 0x2000);
    gate.recordIssued(0x2000, 0x1000, 1, decision);
    gate.resolve(0x2000, false);
    decision = gate.admit(0x1000, 1, 0x2040);
    gate.recordIssued(0x2040, 0x1000, 1, decision);
    gate.resolve(0x2040, false);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);
    for (unsigned i = 0; i < 8; ++i) {
        decision = gate.admit(0x1000, 1, 0x2080 + i * 64);
        if (decision.allowed) {
            gate.recordIssued(0x2080 + i * 64, 0x1000, 1, decision);
            gate.resolve(0x2080 + i * 64, true);
        }
    }
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Open);
}

TEST(DirectQualityGate, SeparatesKindsAndDropsFeedbackConflicts)
{
    DirectQualityGate gate(testConfig());
    auto large = gate.admit(0x1000, 1, 0x3000);
    auto small = gate.admit(0x1000, 2, 0x3000);
    gate.recordIssued(0x3000, 0x1000, 1, large);
    gate.recordIssued(0x3000, 0x1000, 2, small);
    EXPECT_EQ(gate.feedbackConflicts(), 0U);
    EXPECT_TRUE(gate.resolve(0x3000, true).resolved);
    EXPECT_TRUE(gate.resolve(0x3000, true).conflict);
}

TEST(DirectQualityGate, ExpiresAndProtectsReplacedGeneration)
{
    auto config = testConfig();
    config.horizon = 2;
    DirectQualityGate gate(config);
    auto decision = gate.admit(0x1000, 1, 0x4000);
    gate.recordIssued(0x4000, 0x1000, 1, decision);
    gate.advanceDemand();
    gate.advanceDemand();
    EXPECT_EQ(gate.feedbackExpiries(), 1U);
    EXPECT_EQ(gate.unused(), 1U);
    EXPECT_TRUE(gate.resolve(0x4000, true).conflict);
}

} // namespace prefetch
} // namespace gem5
