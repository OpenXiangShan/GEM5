#include <gtest/gtest.h>

#include "mem/cache/prefetch/candidate_table.hh"
#include "mem/cache/prefetch/quality_control.hh"
#include "mem/lldp.hh"

using namespace gem5::lldp;

TEST(LldpChain, ZeroPCAndBoundedHistory)
{
    auto chain = Chain::start(0);
    ASSERT_TRUE(chain.valid);
    EXPECT_EQ(chain.length, 1);
    chain = chain.extend(decodeOperation("addi", -8));
    chain = chain.extend(decodeOperation("slli", 3));
    chain = chain.extend(decodeOperation("andi", 255));
    EXPECT_EQ(chain.length, 4);
    EXPECT_EQ(chain.ops[0].imm, -8);
    EXPECT_EQ(chain.ops[1].op, Op::Shl);
    EXPECT_EQ(chain.categories[0], 1);
    EXPECT_EQ(chain.categories[1], 1);
    EXPECT_EQ(chain.categories[2], 1);
    auto fresh = Chain::start(0x200);
    EXPECT_EQ(fresh.length, 1);
    EXPECT_EQ(fresh.categories[0], 0);
    chain.length = 255;
    EXPECT_EQ(chain.extend(decodeOperation("addi", 0)).length, 256);
}

TEST(LldpChain, DualSourceOnlyPropagatesLength)
{
    auto chain = Chain::start(0x1000);
    chain = chain.extendDependencyOnly(SourceForm::DualRegister);
    EXPECT_EQ(chain.length, 2);
    EXPECT_FALSE(chain.replayable);
    EXPECT_EQ(chain.dualSrcRegisterOps, 1);
    EXPECT_EQ(chain.singleSrcImmediateOps, 0);
    EXPECT_EQ(chain.ops[0].op, Op::Other);
    EXPECT_EQ(chain.categories[0], 0);

    chain = chain.extend(decodeOperation("addi", 8));
    EXPECT_EQ(chain.length, 3);
    EXPECT_FALSE(chain.replayable);
    EXPECT_EQ(chain.dualSrcRegisterOps, 1);
    EXPECT_EQ(chain.singleSrcImmediateOps, 1);
}

TEST(LldpReplay, ImmediateOperationsAndWordSignExtension)
{
    uint64_t value = 0x1000;
    ASSERT_TRUE(apply(decodeOperation("addi", -8), value));
    EXPECT_EQ(value, 0xff8);
    ASSERT_TRUE(apply(decodeOperation("slli", 3), value));
    EXPECT_EQ(value, 0x7fc0);
    ASSERT_TRUE(apply(decodeOperation("xori", 8), value));
    EXPECT_EQ(value, 0x7fc8);
    value = 0x80000000;
    ASSERT_TRUE(apply(decodeOperation("addiw", 0), value));
    EXPECT_EQ(value, 0xffffffff80000000ULL);
    ASSERT_TRUE(apply(decodeOperation("sraiw", 4), value));
    EXPECT_EQ(value, 0xfffffffff8000000ULL);
    value = 0x80000000;
    ASSERT_TRUE(apply(decodeOperation("srliw", 4), value));
    EXPECT_EQ(value, 0x8000000);
    value = ~uint64_t(0);
    ASSERT_TRUE(apply(decodeOperation("c_andi", 7), value));
    EXPECT_EQ(value, 7);
    ASSERT_FALSE(apply(decodeOperation("add", 0), value));
    ASSERT_FALSE(apply(decodeOperation("mul", 0), value));
}

TEST(LldpReplacement, TreePLRUIsNotTimestampLRU)
{
    PLRU<4> tree;
    EXPECT_EQ(tree.victim(), 0);
    tree.touch(0);
    EXPECT_EQ(tree.victim(), 2);
    tree.touch(2);
    EXPECT_EQ(tree.victim(), 1);
    tree.touch(1);
    EXPECT_EQ(tree.victim(), 3);
    tree.touch(3);
    EXPECT_EQ(tree.victim(), 0);
    PLRU<64> larger;
    for (unsigned i = 0; i < 64; ++i)
        larger.touch(i);
    EXPECT_EQ(larger.victim(), 0);
    larger.touch(0);
    EXPECT_EQ(larger.victim(), 32);
}

TEST(LldpChain, SourceFormDoesNotMeanReplaySupport)
{
    EXPECT_EQ(sourceForm(1, true), SourceForm::SingleImmediate);
    EXPECT_EQ(sourceForm(2, false), SourceForm::DualRegister);
    EXPECT_EQ(sourceForm(1, false), SourceForm::Other);
    EXPECT_EQ(sourceForm(3, false), SourceForm::Other);
    auto unsupported = Chain::start(0x100).extend(decodeOperation("slti", 4));
    EXPECT_EQ(unsupported.singleSrcImmediateOps, 1);
    EXPECT_EQ(unsupported.dualSrcRegisterOps, 0);
    EXPECT_FALSE(unsupported.trainable());
    auto other = Chain::start(0x100).extendDependencyOnly(SourceForm::Other);
    EXPECT_EQ(other.dualSrcRegisterOps, 0);
}

TEST(LldpChain, MixedChainsStayExcludedAndLoadRestarts)
{
    auto single = Chain::start(0x100).extend(decodeOperation("addi", 8));
    ASSERT_TRUE(single.trainable());
    auto mixed = single.extendDependencyOnly(SourceForm::DualRegister);
    EXPECT_EQ(mixed.producerPC, 0x100);
    EXPECT_EQ(mixed.length, 3);
    EXPECT_EQ(mixed.ops[0], single.ops[0]);
    EXPECT_EQ(mixed.ops[1], Operation{});
    EXPECT_FALSE(mixed.trainable());
    auto longer = mixed.extend(decodeOperation("slli", 2));
    EXPECT_EQ(longer.length, 4);
    EXPECT_EQ(longer.singleSrcImmediateOps, 2);
    EXPECT_EQ(longer.dualSrcRegisterOps, 1);
    EXPECT_FALSE(longer.trainable());
    EXPECT_EQ(longer.ops, mixed.ops);
    auto next = Chain::start(0x200);
    EXPECT_TRUE(next.trainable());
    EXPECT_EQ(next.length, 1);
    EXPECT_EQ(next.singleSrcImmediateOps, 0);
    EXPECT_EQ(next.dualSrcRegisterOps, 0);
    auto invalid = Chain{}.extendDependencyOnly(SourceForm::DualRegister);
    EXPECT_FALSE(invalid.valid);
    EXPECT_EQ(invalid.length, 0);
    EXPECT_EQ(invalid.dualSrcRegisterOps, 0);
}

TEST(LldpHint, ConsumerGenerationRejectsReplacedChild)
{
    Hint hint;
    hint.consumerGenerations = {11, 22, 0, 44};
    EXPECT_TRUE(hint.consumerMatches(0, 11));
    EXPECT_TRUE(hint.consumerMatches(1, 22));
    EXPECT_FALSE(hint.consumerMatches(1, 23));
    EXPECT_TRUE(hint.consumerMatches(2, 0));
    EXPECT_FALSE(hint.consumerMatches(4, 0));
}

TEST(LldpQuality, SaturationAndGenerationSafety)
{
    gem5::prefetch::PrefetchQualityControl control;
    gem5::prefetch::PrefetchQualityControl::Policy policy;
    policy.minSamples = 2;
    policy.minAccuracyPct = 50;
    policy.maxLatePct = 50;
    const gem5::prefetch::PrefetchQualityControl::Key key{
        gem5::enums::PrefetchSourceType::LLDPC, 0x100, 0x200, 0};
    auto handle = control.touch(key);
    EXPECT_TRUE(control.admit(handle, policy, 0));
    control.issued(handle);
    control.observe(handle,
                    gem5::prefetch::PrefetchQualityControl::Outcome::Useful);
    control.issued(handle);
    control.observe(handle,
                    gem5::prefetch::PrefetchQualityControl::Outcome::Useful);
    EXPECT_EQ(control.updateBoost(handle, policy), 1);
    EXPECT_TRUE(control.valid(handle));
    auto replacement = control.touch({
        gem5::enums::PrefetchSourceType::LLDPC, 0x100, 0x200, 0});
    EXPECT_EQ(replacement.generation, handle.generation);
}

TEST(LldpQuality, SpatialBoostRequiresIndependentEvidence)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    Control control;
    Control::Policy strict;
    strict.minSamples = 32;
    strict.minAccuracyPct = 75;
    strict.maxLatePct = 10;
    auto handle = control.touch({
        gem5::enums::PrefetchSourceType::LLDPS, 0x300, 0, 0});
    for (unsigned i = 0; i < 31; ++i) {
        control.issued(handle);
        control.observe(handle, Control::Outcome::Useful);
    }
    EXPECT_EQ(control.updateBoost(handle, strict), 0);
    control.issued(handle);
    control.observe(handle, Control::Outcome::Useful);
    EXPECT_EQ(control.updateBoost(handle, strict), 1);
    for (unsigned i = 0; i < 12; ++i) {
        control.issued(handle);
        control.observe(handle, Control::Outcome::Unused);
    }
    EXPECT_EQ(control.updateBoost(handle, strict), -1);
}

TEST(LldpChainDepth, OnlyFirstFamilyHopAllowed)
{
    using gem5::enums::PrefetchSourceType;
    using gem5::prefetch::allowLldpChainTrigger;
    EXPECT_TRUE(allowLldpChainTrigger(PrefetchSourceType::LLDP, 0, 1));
    EXPECT_TRUE(allowLldpChainTrigger(PrefetchSourceType::LLDPS, 0, 1));
    EXPECT_TRUE(allowLldpChainTrigger(PrefetchSourceType::LLDPT, 0, 1));
    EXPECT_FALSE(allowLldpChainTrigger(PrefetchSourceType::LLDPC, 1, 1));
    EXPECT_FALSE(allowLldpChainTrigger(PrefetchSourceType::LLDPC, 0, 1));
    EXPECT_FALSE(allowLldpChainTrigger(PrefetchSourceType::SStream, 0, 1));
    EXPECT_TRUE(allowLldpChainTrigger(PrefetchSourceType::LLDPC, 1, 2));
}

TEST(LldpQuality, UnhealthyKeysKeepProbingAcrossLongWindows)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    using gem5::enums::PrefetchSourceType;
    for (unsigned interval : {0, 1, 7, 16, 32, 255, 256}) {
        Control control;
        Control::Policy policy;
        policy.probeInterval = interval;
        auto handle = control.touch({PrefetchSourceType::LLDPC, 0x100, 0x200, 0});
        unsigned admitted = 0;
        for (unsigned opportunity = 1; opportunity <= 65536; ++opportunity) {
            const bool accepted = control.admit(handle, policy, 0);
            const bool expected = opportunity == 1 ||
                (interval && opportunity % interval == 0);
            ASSERT_EQ(accepted, expected)
                << "interval=" << interval << " opportunity=" << opportunity;
            if (accepted) {
                ++admitted;
                control.issued(handle);
                control.observe(handle, Control::Outcome::Unused);
            }
        }
        EXPECT_EQ(admitted, interval == 1 ? 65536 :
            1 + (interval ? 65536 / interval : 0));
    }
}

TEST(LldpQuality, ResourceRejectionDoesNotSpendProbeOpportunity)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    Control control;
    Control::Policy policy;
    auto handle = control.touch({gem5::enums::PrefetchSourceType::LLDPC, 1, 2, 0});
    for (unsigned i = 0; i < 4096; ++i)
        ASSERT_FALSE(control.admit(handle, policy, policy.maxPressurePct));
    ASSERT_TRUE(control.admit(handle, policy, 0));
    for (unsigned i = 0; i < policy.maxOutstanding; ++i)
        control.issued(handle);
    for (unsigned i = 0; i < 4096; ++i)
        ASSERT_FALSE(control.admit(handle, policy, 0));
    for (unsigned i = 0; i < policy.maxOutstanding; ++i)
        control.observe(handle, Control::Outcome::Unused);
    for (unsigned opportunity = 2; opportunity <= 16; ++opportunity)
        EXPECT_EQ(control.admit(handle, policy, 0), opportunity == 16);
    // Changing the configured period must recover without replacing the key.
    policy.probeInterval = 7;
    for (unsigned opportunity = 1; opportunity <= 4096; ++opportunity) {
        const bool accepted = control.admit(handle, policy, 0);
        ASSERT_EQ(accepted, opportunity % 7 == 0);
        if (accepted) {
            control.issued(handle);
            control.observe(handle, Control::Outcome::Unused);
        }
    }
}

TEST(LldpQuality, RripProtectsHotWayAndRejectsStaleFeedback)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    using gem5::enums::PrefetchSourceType;
    Control control;
    const Control::Key hot{PrefetchSourceType::LLDPC, 0x1000, 0, 0};
    std::array<Control::Handle, 4> handles;
    for (unsigned i = 0; i < handles.size(); ++i)
        handles[i] = control.touch({PrefetchSourceType::LLDPC, 0x1000 + i * 8, 0, 0});
    control.touch(hot);
    const auto replacement = control.touch({PrefetchSourceType::LLDPC, 0x1020, 0, 0});
    EXPECT_TRUE(control.valid(handles[0]));
    EXPECT_FALSE(control.valid(handles[1]));
    control.issued(replacement);
    control.observe(handles[1], Control::Outcome::Useful);
    EXPECT_EQ(control.outstanding(replacement), 1);
    // Continuous cold-key churn must not evict a key refreshed every round.
    for (unsigned i = 5; i < 4096; ++i) {
        control.touch(hot);
        control.touch({PrefetchSourceType::LLDPC, 0x1000 + i * 8, 0, 0});
        ASSERT_TRUE(control.valid(handles[0]));
    }
}

TEST(LldpQuality, BoostPressureRevocationAndVictimNotification)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    using gem5::enums::PrefetchSourceType;
    Control control;
    Control::Policy policy;
    policy.minSamples = 2;
    const Control::Key key{PrefetchSourceType::LLDPS, 0x1000, 0, 0,
                          PrefetchSourceType::SStream};
    auto handle = control.touch(key);
    for (unsigned i = 0; i < 2; ++i) {
        control.issued(handle);
        control.observe(handle, Control::Outcome::Useful);
    }
    EXPECT_EQ(control.updateBoost(handle, policy, 75), 0);
    EXPECT_EQ(control.updateBoost(handle, policy, 74), 1);
    EXPECT_EQ(control.updateBoost(handle, policy, 75), -1);
    EXPECT_EQ(control.updateBoost(handle, policy, 74), 1);
    bool revoked = false;
    for (unsigned i = 1; i < 64 && !revoked; ++i) {
        control.touch({PrefetchSourceType::LLDPS, 0x1000 + i * 8, 0, 0},
            [&](const Control::Key &victim, Control::Handle old, bool boosted) {
                if (old.generation == handle.generation) {
                    EXPECT_TRUE(control.valid(old)); // callback before overwrite
                    EXPECT_EQ(victim.provider, PrefetchSourceType::SStream);
                    EXPECT_EQ(victim.producerPC, 0x1000);
                    EXPECT_TRUE(boosted);
                    revoked = true;
                }
            });
    }
    EXPECT_TRUE(revoked);
    EXPECT_FALSE(control.valid(handle));
    EXPECT_EQ(control.updateBoost(handle, policy, 0), 0);
}

TEST(LldpQuality, ProviderAndContextKeepFeedbackIndependent)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    using gem5::enums::PrefetchSourceType;
    Control control;
    const auto stream = control.touch({PrefetchSourceType::LLDPS, 1, 0, 0,
                                      PrefetchSourceType::SStream});
    const auto stride = control.touch({PrefetchSourceType::LLDPS, 1, 0, 0,
                                      PrefetchSourceType::SStride});
    const auto other_context = control.touch({PrefetchSourceType::LLDPS, 1, 0, 1,
                                             PrefetchSourceType::SStream});
    EXPECT_NE(stream.generation, stride.generation);
    EXPECT_NE(stream.generation, other_context.generation);
    EXPECT_EQ(control.setBoost(stream, true), 1);
    EXPECT_EQ(control.setBoost(stride, false), 0);
    EXPECT_EQ(control.setBoost(other_context, false), 0);
    EXPECT_EQ(control.setBoost(stream, false), -1);
}

TEST(LldpQuality, DecisionReasonsAndPreIssueDropIsolation)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    using Life = gem5::prefetch::CandidateLifecycle;
    Control control;
    Control::Policy policy;
    policy.minSamples = 2;
    const auto handle = control.touch({gem5::enums::PrefetchSourceType::LLDPC, 1, 2, 0});
    Control::Decision reason;
    EXPECT_FALSE(control.admit(handle, policy, 75, &reason));
    EXPECT_EQ(reason, Control::Decision::Pressure);
    ASSERT_TRUE(control.admit(handle, policy, 0, &reason));
    EXPECT_EQ(reason, Control::Decision::Probe);
    Life issued, queued;
    ASSERT_TRUE(issued.issue());
    control.issued(handle);
    ASSERT_TRUE(queued.finish());
    if (queued.hasIssued)
        control.observe(handle, Control::Outcome::Dropped);
    EXPECT_EQ(control.outstanding(handle), 1);
    ASSERT_TRUE(issued.finish());
    control.observe(handle, Control::Outcome::Useful);
    EXPECT_EQ(control.outstanding(handle), 0);
    EXPECT_FALSE(issued.finish());
    EXPECT_FALSE(control.admit(handle, policy, 0, &reason));
    EXPECT_EQ(reason, Control::Decision::Cold);
    control.issued(handle);
    control.observe(handle, Control::Outcome::Useful);
    EXPECT_TRUE(control.admit(handle, policy, 0, &reason));
    EXPECT_EQ(reason, Control::Decision::Healthy);
    for (unsigned i = 0; i < policy.maxOutstanding; ++i)
        control.issued(handle);
    EXPECT_FALSE(control.admit(handle, policy, 0, &reason));
    EXPECT_EQ(reason, Control::Decision::Outstanding);
    EXPECT_FALSE(control.admit({999, 0}, policy, 0, &reason));
    EXPECT_EQ(reason, Control::Decision::Stale);
}

TEST(LldpQuality, DuplicateCollisionsDoNotDestroyAddressEvidence)
{
    using Control = gem5::prefetch::PrefetchQualityControl;
    Control control;
    Control::Policy policy;
    policy.minSamples = 2;
    policy.minAccuracyPct = 75;
    const auto handle = control.touch({gem5::enums::PrefetchSourceType::LLDPS, 1, 0, 0});
    for (unsigned i = 0; i < 2; ++i) {
        control.issued(handle);
        control.observe(handle, Control::Outcome::Useful);
    }
    ASSERT_EQ(control.updateBoost(handle, policy), 1);
    for (unsigned i = 0; i < 4096; ++i) {
        control.issued(handle);
        control.observe(handle, Control::Outcome::Collision);
        ASSERT_EQ(control.updateBoost(handle, policy), 0);
    }
    EXPECT_TRUE(control.admit(handle, policy, 0));
    control.issued(handle);
    control.observe(handle, Control::Outcome::DemandMerged);
    EXPECT_EQ(control.updateBoost(handle, policy), -1);
}
