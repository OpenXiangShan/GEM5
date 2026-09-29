#include <gtest/gtest.h>

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
