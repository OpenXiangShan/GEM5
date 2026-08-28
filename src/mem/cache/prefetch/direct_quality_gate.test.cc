#include <gtest/gtest.h>

#include <vector>

#include "mem/cache/prefetch/direct_quality_gate.hh"

namespace gem5
{
namespace prefetch
{

void
recordIssued(DirectQualityGate &gate, Addr line, const DirectQualityGate::Decision &decision, uint8_t kind)
{
    // Feedback is deliberately registered by admit() at the raw BOP
    // candidate point. Keeping this no-op lets the older state tests express
    // their logical candidate sequence without reintroducing queue timing.
    (void)gate;
    (void)line;
    (void)decision;
    (void)kind;
}

void
issueAndResolve(DirectQualityGate &gate, Addr pc, uint8_t kind, Addr line, bool useful)
{
    const auto decision = gate.admit(pc, kind, line);
    ASSERT_TRUE(decision.allowed);
    recordIssued(gate, line, decision, kind);
    if (useful) {
        gate.observeDemand(line);
    } else {
        gate.observeDemand(0xdead);
        gate.observeDemand(0xdead);
    }
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

class CapturingTraceSink : public DirectQualityGate::TraceSink
{
  public:
    struct Candidate
    {
        uint64_t eventSequence;
        Addr pc;
        uint8_t kind;
        Addr triggerLine;
        Addr candidateLine;
        DirectQualityGate::State state;
        bool allowed;
        bool sampled;
    };

    struct Issue
    {
        uint64_t eventSequence;
        uint64_t feedbackId;
        uint64_t issueDemandSequence;
        Addr line;
        uint8_t kind;
    };

    struct Demand
    {
        uint64_t eventSequence;
        uint64_t demandSequence;
        Addr line;
    };

    struct Outcome
    {
        uint64_t eventSequence;
        uint64_t feedbackId;
        uint64_t resolveDemandSequence;
        Addr line;
        DirectQualityGate::TraceOutcome value;
    };

    unsigned horizon = 0;
    unsigned feedbackEntries = 0;
    unsigned feedbackWays = 0;
    std::vector<Candidate> candidates;
    std::vector<Issue> issues;
    std::vector<Demand> demands;
    std::vector<Outcome> outcomes;

    void directQualityTraceConfig(const DirectQualityGate::Config &config) override
    {
        horizon = config.horizon;
        feedbackEntries = config.feedbackEntries;
        feedbackWays = config.feedbackWays;
    }

    void directQualityTraceCandidate(uint64_t event_sequence, Addr pc, uint8_t kind, Addr trigger_line,
                                     Addr candidate_line, DirectQualityGate::State state, bool allowed,
                                     bool sampled) override
    {
        candidates.push_back({event_sequence, pc, kind, trigger_line, candidate_line, state, allowed, sampled});
    }

    void directQualityTraceIssue(uint64_t event_sequence, uint64_t feedback_id, uint64_t issue_demand_sequence,
                                 Addr line, uint8_t kind) override
    {
        issues.push_back({event_sequence, feedback_id, issue_demand_sequence, line, kind});
    }

    void directQualityTraceDemand(uint64_t event_sequence, uint64_t demand_sequence, Addr line) override
    {
        demands.push_back({event_sequence, demand_sequence, line});
    }

    void directQualityTraceOutcome(uint64_t event_sequence, uint64_t feedback_id, uint64_t resolve_demand_sequence,
                                   Addr line, DirectQualityGate::TraceOutcome outcome) override
    {
        outcomes.push_back({event_sequence, feedback_id, resolve_demand_sequence, line, outcome});
    }
};

TEST(DirectQualityGate, BlocksAtTenToOneAndReopens)
{
    auto config = testConfig();
    config.horizon = 1;
    config.blockProbePeriod = 1;
    config.borderlineBlockProbePeriod = 1;
    DirectQualityGate gate(config);
    auto decision = gate.admit(0x1000, 1, 0x2000);
    recordIssued(gate, 0x2000, decision, 1);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);
    decision = gate.admit(0x1000, 1, 0x2040);
    recordIssued(gate, 0x2040, decision, 1);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);
    for (unsigned i = 0; i < 8; ++i) {
        decision = gate.admit(0x1000, 1, 0x2080 + i * 64);
        if (decision.allowed) {
            recordIssued(gate, 0x2080 + i * 64, decision, 1);
            gate.observeDemand(0x2080 + i * 64);
        }
    }
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Open);
}

TEST(DirectQualityGate, ConfirmRecoveryNeedsOnlyCurrentEpochSamples)
{
    auto config = testConfig();
    config.horizon = 3;
    config.blockProbePeriod = 1;
    config.borderlineBlockProbePeriod = 1;
    config.reopenConfirmSamples = 2;
    DirectQualityGate gate(config);

    const auto issue_unused = [&](Addr line) {
        const auto decision = gate.admit(0x1000, 1, line);
        ASSERT_TRUE(decision.allowed);
        recordIssued(gate, line, decision, 1);
        for (unsigned demand = 0; demand <= config.horizon; ++demand)
            gate.observeDemand(0xdead);
    };
    issue_unused(0x2000);
    issue_unused(0x2040);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);

    // This request is issued before recovery but deliberately resolves after
    // BLOCK has entered RECOVER. Its old epoch must not confirm recovery.
    const auto stale = gate.admit(0x1000, 1, 0x2080);
    ASSERT_TRUE(stale.allowed);
    recordIssued(gate, 0x2080, stale, 1);

    issueAndResolve(gate, 0x1000, 1, 0x20c0, true);
    issueAndResolve(gate, 0x1000, 1, 0x2100, true);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Recover);
    EXPECT_EQ(gate.blockToRecoverTransitions(), 1U);

    gate.observeDemand(0x2080);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Recover);

    issueAndResolve(gate, 0x1000, 1, 0x2140, true);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Recover);
    issueAndResolve(gate, 0x1000, 1, 0x2180, true);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Open);
    EXPECT_EQ(gate.recoverToOpenTransitions(), 1U);
}

TEST(DirectQualityGate, RecoveryRetainsBorderlineProbePeriod)
{
    auto config = testConfig();
    config.horizon = 1;
    config.minSamples = 4;
    config.unusedPerUseful = 2;
    config.strictUnusedPerUseful = 4;
    config.strictBlockGuard = 0;
    config.reopenUnusedPerUseful = 4;
    config.blockProbePeriod = 1;
    config.borderlineBlockProbePeriod = 1;
    config.reopenConfirmSamples = 2;
    DirectQualityGate gate(config);

    issueAndResolve(gate, 0x1000, 1, 0x3000, false);
    issueAndResolve(gate, 0x1000, 1, 0x3040, false);
    issueAndResolve(gate, 0x1000, 1, 0x3080, false);
    issueAndResolve(gate, 0x1000, 1, 0x30c0, true);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);

    const auto probe = gate.admit(0x1000, 1, 0x3140);
    ASSERT_TRUE(probe.allowed);
    recordIssued(gate, 0x3140, probe, 1);
    gate.observeDemand(0x3140);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Recover);

    const auto recovery_probe = gate.admit(0x1000, 1, 0x3180);
    EXPECT_TRUE(recovery_probe.allowed);
}

TEST(DirectQualityGate, RecoveryRetainsStrictProbePeriod)
{
    auto config = testConfig();
    config.horizon = 1;
    config.minSamples = 4;
    config.unusedPerUseful = 2;
    config.strictUnusedPerUseful = 4;
    config.strictBlockGuard = 0;
    config.reopenUnusedPerUseful = 4;
    config.blockProbePeriod = 1;
    config.borderlineBlockProbePeriod = 1;
    config.reopenConfirmSamples = 2;
    DirectQualityGate gate(config);

    issueAndResolve(gate, 0x1000, 1, 0x4000, false);
    issueAndResolve(gate, 0x1000, 1, 0x4040, false);
    issueAndResolve(gate, 0x1000, 1, 0x4080, false);
    issueAndResolve(gate, 0x1000, 1, 0x40c0, false);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);

    const auto strict_probe = gate.admit(0x1000, 1, 0x43c0);
    ASSERT_TRUE(strict_probe.allowed);
    recordIssued(gate, 0x43c0, strict_probe, 1);
    gate.observeDemand(0x43c0);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Recover);

    const auto recovery_probe = gate.admit(0x1000, 1, 0x4400);
    EXPECT_TRUE(recovery_probe.allowed);
}

TEST(DirectQualityGate, NegativeRecoveryEvidenceReturnsToBlock)
{
    auto config = testConfig();
    config.horizon = 1;
    config.minSamples = 4;
    config.unusedPerUseful = 2;
    config.strictUnusedPerUseful = 4;
    config.strictBlockGuard = 0;
    config.reopenUnusedPerUseful = 2;
    config.blockProbePeriod = 1;
    config.borderlineBlockProbePeriod = 1;
    config.reopenConfirmSamples = 8;
    DirectQualityGate gate(config);

    issueAndResolve(gate, 0x1000, 1, 0x5000, false);
    issueAndResolve(gate, 0x1000, 1, 0x5040, false);
    issueAndResolve(gate, 0x1000, 1, 0x5080, false);
    issueAndResolve(gate, 0x1000, 1, 0x50c0, true);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);

    issueAndResolve(gate, 0x1000, 1, 0x5100, true);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Recover);
    issueAndResolve(gate, 0x1000, 1, 0x5140, false);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);
    EXPECT_EQ(gate.recoverToBlockTransitions(), 1U);
}

TEST(DirectQualityGate, SeparatesKindsAndDropsFeedbackConflicts)
{
    DirectQualityGate gate(testConfig());
    auto large = gate.admit(0x1000, 1, 0x3000);
    auto small = gate.admit(0x1000, 2, 0x3000);
    recordIssued(gate, 0x3000, large, 1);
    recordIssued(gate, 0x3000, small, 2);
    EXPECT_EQ(gate.feedbackConflicts(), 0U);
    EXPECT_EQ(gate.sampled(), 1U);
    gate.observeDemand(0x3000);
    EXPECT_EQ(gate.useful(), 1U);
    EXPECT_EQ(gate.unknownDrops(), 0U);
}

TEST(DirectQualityGate, DemandWindowExpiryIsUnused)
{
    auto config = testConfig();
    config.horizon = 2;
    DirectQualityGate gate(config);
    auto decision = gate.admit(0x1000, 1, 0x4000);
    recordIssued(gate, 0x4000, decision, 1);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);
    EXPECT_EQ(gate.feedbackExpiries(), 1U);
    EXPECT_EQ(gate.feedbackExpiryUnused(), 1U);
    EXPECT_EQ(gate.unknownDrops(), 0U);
    EXPECT_EQ(gate.unused(), 1U);
}

TEST(DirectQualityGate, DemandAtHorizonIsUseful)
{
    auto config = testConfig();
    config.horizon = 2;
    DirectQualityGate gate(config);

    const auto decision = gate.admit(0x1000, 1, 0x4400);
    recordIssued(gate, 0x4400, decision, 1);
    gate.observeDemand(0xdead);
    gate.observeDemand(0x4400);

    EXPECT_EQ(gate.useful(), 1U);
    EXPECT_EQ(gate.unused(), 0U);
    EXPECT_EQ(gate.feedbackExpiries(), 0U);
}

TEST(DirectQualityGate, CompactAgePreservesExpiryAcrossWrap)
{
    auto config = testConfig();
    config.horizon = 2;
    DirectQualityGate gate(config);

    // The hardware entry holds a compact 16-bit demand age. Advance to the
    // wrap boundary before inserting feedback, then verify the normal
    // horizon rule still applies after the compact age wraps.
    for (unsigned demand = 0; demand < 0xffff; ++demand)
        gate.observeDemand(0xdead);

    const auto decision = gate.admit(0x1000, 1, 0x4480);
    ASSERT_TRUE(decision.feedbackInserted);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);
    EXPECT_EQ(gate.unused(), 0U);
    gate.observeDemand(0xdead);
    EXPECT_EQ(gate.unused(), 1U);
    EXPECT_EQ(gate.feedbackExpiryUnused(), 1U);
}

TEST(DirectQualityGate, EarlyUsefulRemovalPreservesExpiryOrder)
{
    auto config = testConfig();
    config.horizon = 2;
    config.feedbackEntries = 4096;
    DirectQualityGate gate(config);

    const auto first = gate.admit(0x1000, 1, 0x8000);
    recordIssued(gate, 0x8000, first, 1);
    gate.observeDemand(0xdead);

    const auto second = gate.admit(0x1000, 1, 0xa000);
    recordIssued(gate, 0xa000, second, 1);
    gate.observeDemand(0xa000);

    const auto third = gate.admit(0x1000, 1, 0xc000);
    recordIssued(gate, 0xc000, third, 1);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);

    EXPECT_EQ(gate.useful(), 1U);
    EXPECT_EQ(gate.unused(), 2U);
    EXPECT_EQ(gate.feedbackExpiryUnused(), 2U);
    EXPECT_EQ(gate.peakOutstanding(), 2U);
}

TEST(DirectQualityGate, RegistersFeedbackBeforeAnyPhysicalQueueIssue)
{
    auto config = testConfig();
    config.qualityEntries = 1;
    config.qualityWays = 1;
    DirectQualityGate gate(config);

    const auto decision = gate.admit(0x1000, 1, 0x5000);
    EXPECT_TRUE(decision.feedbackInserted);
    EXPECT_EQ(gate.sampled(), 1U);
    gate.observeDemand(0x5000);
    EXPECT_EQ(gate.useful(), 1U);
}

TEST(DirectQualityGate, SamplesByTriggerLineNotCandidateOrdinal)
{
    auto config = testConfig();
    config.observeSamplePeriod = 4;
    DirectQualityGate gate(config);

    const auto first = gate.admit(0x1000, 1, 0x2000, 0x4000);
    const auto second = gate.admit(0x1000, 1, 0x2000, 0x4040);

    EXPECT_EQ(first.sampled, second.sampled);
    EXPECT_EQ(gate.candidates(), 2U);
}

TEST(DirectQualityGate, DecaysResolvedEvidenceLikeRawReplay)
{
    auto config = testConfig();
    config.horizon = 1;
    config.minSamples = 2;
    config.unusedPerUseful = 2;
    config.blockGuard = 0;
    config.reopenUnusedPerUseful = 1;
    config.reopenGuard = 0;
    config.decayPeriod = 2;
    config.blockProbePeriod = 1;
    config.borderlineBlockProbePeriod = 1;
    DirectQualityGate gate(config);

    issueAndResolve(gate, 0x1000, 1, 0x6000, false);
    issueAndResolve(gate, 0x1000, 1, 0x6040, false);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Block);

    issueAndResolve(gate, 0x1000, 1, 0x6080, true);
    EXPECT_EQ(gate.state(0x1000, 1), DirectQualityGate::State::Open);
}

TEST(DirectQualityGate, FeedbackReplacementIsUnknownNotUnused)
{
    auto config = testConfig();
    config.feedbackEntries = 1;
    config.feedbackWays = 1;
    DirectQualityGate gate(config);

    const auto first = gate.admit(0x1000, 1, 0x6000);
    recordIssued(gate, 0x6000, first, 1);
    const auto second = gate.admit(0x1040, 1, 0x6040);
    recordIssued(gate, 0x6040, second, 1);
    gate.observeDemand(0x6040);

    EXPECT_EQ(gate.feedbackReplacements(), 1U);
    EXPECT_EQ(gate.unknownDrops(), 1U);
    EXPECT_EQ(gate.unused(), 0U);
    EXPECT_EQ(gate.useful(), 1U);
}

TEST(DirectQualityGate, QualityReplacementDropsOutcomeAsUnknown)
{
    auto config = testConfig();
    config.qualityEntries = 1;
    config.qualityWays = 1;
    DirectQualityGate gate(config);

    const auto first = gate.admit(0x1000, 1, 0x7000);
    recordIssued(gate, 0x7000, first, 1);
    gate.admit(0x1040, 1, 0x7040);
    gate.observeDemand(0x7000);

    EXPECT_EQ(gate.useful(), 0U);
    EXPECT_EQ(gate.unused(), 0U);
    EXPECT_EQ(gate.unknownDrops(), 1U);
    EXPECT_EQ(gate.orphanOutcomes(), 1U);
}

TEST(DirectQualityGate, TraceOrdersIssueDemandAndUsefulOutcome)
{
    auto config = testConfig();
    config.horizon = 2;
    DirectQualityGate gate(config);
    CapturingTraceSink sink;
    gate.setTraceSink(&sink);

    const auto decision = gate.admit(0x1000, 1, 0x8000);
    recordIssued(gate, 0x8000, decision, 1);
    gate.observeDemand(0x8000);

    EXPECT_EQ(sink.horizon, 2U);
    EXPECT_EQ(sink.feedbackEntries, 4U);
    EXPECT_EQ(sink.feedbackWays, 1U);
    ASSERT_EQ(sink.issues.size(), 1U);
    ASSERT_EQ(sink.candidates.size(), 1U);
    EXPECT_EQ(sink.candidates[0].eventSequence, 1U);
    EXPECT_EQ(sink.candidates[0].pc, 0x1000U);
    EXPECT_EQ(sink.candidates[0].kind, 1U);
    EXPECT_EQ(sink.candidates[0].triggerLine, 0x8000U);
    EXPECT_EQ(sink.candidates[0].candidateLine, 0x8000U);
    EXPECT_TRUE(sink.candidates[0].allowed);
    EXPECT_TRUE(sink.candidates[0].sampled);
    EXPECT_EQ(sink.issues[0].eventSequence, 2U);
    EXPECT_EQ(sink.issues[0].feedbackId, 1U);
    EXPECT_EQ(sink.issues[0].issueDemandSequence, 0U);
    EXPECT_EQ(sink.issues[0].line, 0x8000U);
    ASSERT_EQ(sink.demands.size(), 1U);
    EXPECT_EQ(sink.demands[0].eventSequence, 3U);
    EXPECT_EQ(sink.demands[0].demandSequence, 1U);
    ASSERT_EQ(sink.outcomes.size(), 1U);
    EXPECT_EQ(sink.outcomes[0].eventSequence, 4U);
    EXPECT_EQ(sink.outcomes[0].feedbackId, 1U);
    EXPECT_EQ(sink.outcomes[0].resolveDemandSequence, 1U);
    EXPECT_EQ(sink.outcomes[0].value, DirectQualityGate::TraceOutcome::UsefulDemand);
}

TEST(DirectQualityGate, TraceMarksFeedbackReplacementUnknown)
{
    auto config = testConfig();
    config.feedbackEntries = 1;
    config.feedbackWays = 1;
    DirectQualityGate gate(config);
    CapturingTraceSink sink;
    gate.setTraceSink(&sink);

    const auto first = gate.admit(0x1000, 1, 0x9000);
    recordIssued(gate, 0x9000, first, 1);
    const auto second = gate.admit(0x1040, 1, 0x9040);
    recordIssued(gate, 0x9040, second, 1);

    ASSERT_EQ(sink.issues.size(), 2U);
    ASSERT_EQ(sink.candidates.size(), 2U);
    ASSERT_EQ(sink.outcomes.size(), 1U);
    EXPECT_EQ(sink.candidates[0].eventSequence, 1U);
    EXPECT_EQ(sink.issues[0].eventSequence, 2U);
    EXPECT_EQ(sink.candidates[1].eventSequence, 3U);
    EXPECT_EQ(sink.outcomes[0].eventSequence, 4U);
    EXPECT_EQ(sink.issues[1].eventSequence, 5U);
    EXPECT_EQ(sink.outcomes[0].feedbackId, sink.issues[0].feedbackId);
    EXPECT_EQ(sink.outcomes[0].value, DirectQualityGate::TraceOutcome::UnknownFeedbackReplacement);
}

TEST(DirectQualityGate, TraceKeepsSameAgeExpiryOrder)
{
    auto config = testConfig();
    config.horizon = 1;
    DirectQualityGate gate(config);
    CapturingTraceSink sink;
    gate.setTraceSink(&sink);

    ASSERT_TRUE(gate.admit(0x1000, 1, 0x9800).feedbackInserted);
    ASSERT_TRUE(gate.admit(0x1000, 1, 0x98c0).feedbackInserted);
    gate.observeDemand(0xdead);
    gate.observeDemand(0xdead);

    ASSERT_EQ(sink.issues.size(), 2U);
    ASSERT_EQ(sink.outcomes.size(), 2U);
    EXPECT_EQ(sink.outcomes[0].feedbackId, sink.issues[0].feedbackId);
    EXPECT_EQ(sink.outcomes[1].feedbackId, sink.issues[1].feedbackId);
    EXPECT_EQ(sink.outcomes[0].value, DirectQualityGate::TraceOutcome::UnusedExpiry);
    EXPECT_EQ(sink.outcomes[1].value, DirectQualityGate::TraceOutcome::UnusedExpiry);
}

}  // namespace prefetch
}  // namespace gem5
