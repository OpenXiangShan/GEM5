#include <gtest/gtest.h>

#include "mem/cache/prefetch/direct_quality_gate.hh"

namespace gem5
{
namespace prefetch
{

void
recordIssued(DirectQualityGate &gate, Addr line,
             const DirectQualityGate::Decision &decision, uint8_t kind)
{
    gate.recordIssued(line, kind, decision.set, decision.way,
                      decision.generation);
}

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
    recordIssued(gate, 0x2000, decision, 1);
    gate.resolve(0x2000, false);
    decision = gate.admit(0x1000, 1, 0x2040);
    recordIssued(gate, 0x2040, decision, 1);
    gate.resolve(0x2040, false);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);
    for (unsigned i = 0; i < 8; ++i) {
        decision = gate.admit(0x1000, 1, 0x2080 + i * 64);
        if (decision.allowed) {
            recordIssued(gate, 0x2080 + i * 64, decision, 1);
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
    recordIssued(gate, 0x3000, large, 1);
    recordIssued(gate, 0x3000, small, 2);
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
    recordIssued(gate, 0x4000, decision, 1);
    gate.advanceDemand();
    gate.advanceDemand();
    EXPECT_EQ(gate.feedbackExpiries(), 1U);
    EXPECT_EQ(gate.unknownDrops(), 1U);
    EXPECT_EQ(gate.unused(), 0U);
    EXPECT_TRUE(gate.resolve(0x4000, true).conflict);
}

TEST(DirectQualityGate, RejectsQueuedTokenAfterQualityReplacement)
{
    auto config = testConfig();
    config.qualityEntries = 1;
    config.qualityWays = 1;
    DirectQualityGate gate(config);

    const auto old_decision = gate.admit(0x1000, 1, 0x5000);
    gate.admit(0x1040, 1, 0x5040);
    recordIssued(gate, 0x5000, old_decision, 1);

    EXPECT_EQ(gate.sampled(), 0U);
    EXPECT_EQ(gate.feedbackTokenDrops(), 1U);
    EXPECT_EQ(gate.unknownDrops(), 1U);
    EXPECT_TRUE(gate.resolve(0x5000, true).conflict);
}

} // namespace prefetch
} // namespace gem5
