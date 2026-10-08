#include <gtest/gtest.h>

#include "mem/cache/prefetch/temporal_control.hh"

namespace gem5::prefetch
{
struct TestMeta
{
    uint8_t trainConf{4}, qualityConf{4}, timelyConf{4}, tokens{2}, outstanding{0};
    uint8_t unusedStreak{0};
    uint32_t probePhase{0};
    bool probation{false}, valid{true};
};

TEST(LldpTemporal, ExhaustedTokensKeepPeriodicProbesWithoutLosingTraining)
{
    for (unsigned interval : {1, 7, 16, 32, 256}) {
        TestMeta entry;
        entry.tokens = 0;
        entry.qualityConf = 0;
        for (unsigned i = 1; i <= 4096; ++i) {
            bool probe;
            const bool accepted = TemporalControl::admit(entry, true, interval, probe);
            ASSERT_EQ(accepted, i % interval == 0);
            EXPECT_EQ(probe, accepted);
        }
        for (unsigned i = 0; i < 20; ++i)
            TemporalControl::unused(entry, true);
        EXPECT_EQ(entry.trainConf, 4);
        EXPECT_TRUE(entry.valid);
    }
}

TEST(LldpTemporal, InFlightAndTrainingGatesDoNotSpendProbe)
{
    TestMeta entry;
    entry.tokens = 0;
    bool probe;
    entry.outstanding = 1;
    for (unsigned i = 0; i < 4096; ++i)
        ASSERT_FALSE(TemporalControl::admit(entry, true, 16, probe));
    EXPECT_EQ(entry.probePhase, 0);
    entry.outstanding = 0;
    entry.trainConf = 2;
    EXPECT_FALSE(TemporalControl::admit(entry, true, 1, probe));
    entry.probation = true;
    EXPECT_TRUE(TemporalControl::admit(entry, true, 1, probe));
    entry.tokens = 1;
    EXPECT_TRUE(TemporalControl::admit(entry, true, 16, probe));
    EXPECT_FALSE(probe);
}

TEST(LldpTemporal, UnusedHysteresisAndLegacyControl)
{
    TestMeta entry;
    TemporalControl::unused(entry, true);
    EXPECT_EQ(entry.qualityConf, 4);
    EXPECT_EQ(entry.tokens, 2);
    TemporalControl::unused(entry, true);
    EXPECT_EQ(entry.qualityConf, 2);
    EXPECT_EQ(entry.tokens, 1);
    EXPECT_EQ(entry.trainConf, 4);
    entry.unusedStreak = 0; // useful/real demand merge
    TemporalControl::unused(entry, true);
    EXPECT_EQ(entry.qualityConf, 2);
    TestMeta old;
    TemporalControl::unused(old, false);
    EXPECT_EQ(old.tokens, 0);
    EXPECT_TRUE(old.valid);
    TemporalControl::unused(old, false);
    EXPECT_FALSE(old.valid);
}

TEST(LldpBenefit, BoundedReplacementBenefitDoesNotEraseDependencies)
{
    DependencyBenefit good, bad;
    for (unsigned i = 0; i < 65536; ++i) {
        good.observe(0);
        bad.observe(1);
        EXPECT_LE(unsigned(good.useful) + good.unused + good.merged, 16);
        EXPECT_GT(good.score(), bad.score());
    }
    const int before = good.score();
    good.observe(3); // collision contributes no negative evidence
    EXPECT_EQ(good.score(), before);
}
} // namespace gem5::prefetch
