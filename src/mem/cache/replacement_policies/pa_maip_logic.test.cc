/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>

#include "mem/cache/replacement_policies/pa_maip_logic.hh"
#include "mem/cache/replacement_policies/pa_maip_reuse.hh"

using namespace gem5::replacement_policy::pa_maip;

TEST(PAMAIPTest, FirstPrefetchConsumptionIsNotReuse)
{
    AccessMap map(4, 2, 4096, 64);
    EXPECT_EQ(map.access(0x1000, false, true), Init);
    EXPECT_EQ(map.access(0x1000, false, true), Prefetched);
    EXPECT_EQ(map.access(0x1000, false, false), Prefetched);
    EXPECT_EQ(map.access(0x1000, false, true), Accessed);
    EXPECT_EQ(map.access(0x1000, false, false), Accessed);
    EXPECT_EQ(map.access(0x1000, false, false), Reused);
    EXPECT_FALSE(highPriority(Init));
    EXPECT_FALSE(highPriority(Prefetched));
    EXPECT_TRUE(highPriority(Accessed));
    EXPECT_TRUE(highPriority(Reused));
}

TEST(PAMAIPTest, FirstDemandMissBecomesReusableOnlyOnNextReference)
{
    AccessMap map(4, 2, 4096, 64);
    EXPECT_EQ(map.access(0x2000, false, false), Init);
    EXPECT_EQ(map.access(0x2000, false, false), Accessed);
    EXPECT_EQ(map.access(0x2000, false, false), Reused);
    EXPECT_EQ(map.access(0x2040, false, false), Init);
    EXPECT_EQ(map.access(0x2fc0, false, false), Init);
    EXPECT_EQ(map.access(0x3000, false, false), Init);
}

TEST(PAMAIPTest, MapEvictionClearsHistoryAndUsesLRU)
{
    AccessMap map(2, 2, 4096, 64);
    EXPECT_EQ(map.access(0x1000, false, false), Init);
    EXPECT_EQ(map.access(0x2000, false, false), Init);
    EXPECT_EQ(map.access(0x1000, false, false), Accessed);
    EXPECT_EQ(map.access(0x3000, false, false), Init);
    EXPECT_EQ(map.access(0x1000, false, false), Reused);
    EXPECT_EQ(map.access(0x2000, false, false), Init);
}

TEST(PAMAIPTest, SecurityDomainsDoNotManufactureReuse)
{
    AccessMap map(4, 4, 4096, 64);
    EXPECT_EQ(map.access(0x1000, false, true), Init);
    EXPECT_EQ(map.access(0x1000, true, false), Init);
    EXPECT_EQ(map.access(0x1000, false, false), Prefetched);
    EXPECT_EQ(map.access(0x1000, true, false), Accessed);
}

TEST(PAMAIPTest, PrefetchAndFirstDemandHitDoNotProtectAStream)
{
    Line line{3, true, true};
    EXPECT_FALSE(line.touch(true, true));
    EXPECT_EQ(line.rrpv, 3);
    EXPECT_TRUE(line.unusedPrefetch);
    EXPECT_FALSE(line.touch(false, true));
    EXPECT_FALSE(line.unusedPrefetch);
    EXPECT_EQ(line.rrpv, 3);
    EXPECT_TRUE(line.touch(false, true));
    EXPECT_EQ(line.rrpv, 0);
}

TEST(PAMAIPTest, MergedDemandConsumesFirstUseWithoutPromotion)
{
    Line line{2, true, true};
    EXPECT_TRUE(line.consumeDemand());
    EXPECT_FALSE(line.consumeDemand());
    EXPECT_EQ(line.rrpv, 2);
    EXPECT_TRUE(line.touch(false, true));
    EXPECT_EQ(line.rrpv, 0);
}

TEST(PAMAIPTest, InsertionOnlyControlAllowsFirstHitPromotion)
{
    Line line{3, true, true};
    EXPECT_TRUE(line.touch(false, false));
    EXPECT_EQ(line.rrpv, 0);
    EXPECT_FALSE(line.unusedPrefetch);
}

TEST(PAMAIPTest, DuelingLeadersAreDisjointAndBalanced)
{
    Dueling duel(1024);
    unsigned srrip = 0, brrip = 0, followers = 0;
    for (unsigned set = 0; set < 1024; ++set) {
        switch (duel.team(set)) {
          case Dueling::Srrip: srrip++; break;
          case Dueling::Brrip: brrip++; break;
          case Dueling::Follower: followers++; break;
        }
    }
    EXPECT_EQ(srrip, 32);
    EXPECT_EQ(brrip, 32);
    EXPECT_EQ(followers, 960);
}

TEST(PAMAIPTest, DuelingSelectsTheTeamWithFewerLeaderFills)
{
    Dueling duel(1024);
    EXPECT_TRUE(duel.useBrrip(Dueling::Follower));
    duel.sample(Dueling::Brrip);
    EXPECT_FALSE(duel.useBrrip(Dueling::Follower));
    EXPECT_TRUE(duel.useBrrip(Dueling::Brrip));
    EXPECT_FALSE(duel.useBrrip(Dueling::Srrip));
    duel.sample(Dueling::Srrip);
    EXPECT_TRUE(duel.useBrrip(Dueling::Follower));
}

TEST(PAMAIPTest, DuelingSaturatesAndFollowersDoNotTrain)
{
    Dueling duel(1024);
    for (unsigned i = 0; i < 2048; ++i) {
        duel.sample(Dueling::Brrip);
    }
    EXPECT_EQ(duel.value(), 0);
    duel.sample(Dueling::Follower);
    EXPECT_EQ(duel.value(), 0);
    for (unsigned i = 0; i < 2048; ++i) {
        duel.sample(Dueling::Srrip);
    }
    EXPECT_EQ(duel.value(), 1023);
    duel.sample(Dueling::Follower);
    EXPECT_EQ(duel.value(), 1023);
}


TEST(PAMAIPTest, ShuffledLeadersRemainBalancedAndDisjoint)
{
    for (unsigned seed : {1, 17}) {
        Dueling duel(1024, seed);
        unsigned srrip = 0, brrip = 0, followers = 0;
        for (unsigned set = 0; set < 1024; ++set) {
            switch (duel.team(set)) {
              case Dueling::Srrip: srrip++; break;
              case Dueling::Brrip: brrip++; break;
              case Dueling::Follower: followers++; break;
            }
        }
        EXPECT_EQ(srrip, 32);
        EXPECT_EQ(brrip, 32);
        EXPECT_EQ(followers, 960);
    }
}

TEST(PAMAIPTest, LeaderPermutationIsReproducibleAndSeeded)
{
    Dueling regular(1024), first(1024, 1), repeated(1024, 1);
    Dueling other(1024, 17);
    unsigned regular_diff = 0, seed_diff = 0;
    for (unsigned set = 0; set < 1024; ++set) {
        EXPECT_EQ(first.team(set), repeated.team(set));
        regular_diff += first.team(set) != regular.team(set);
        seed_diff += first.team(set) != other.team(set);
    }
    EXPECT_GT(regular_diff, 0);
    EXPECT_GT(seed_diff, 0);
}


TEST(PAMAIPTest, ForgottenPrefetchMapPromotesAnExistingCacheLine)
{
    AccessMap maps(1, 1, 4096, 64);
    EXPECT_EQ(maps.access(0, false, true), Init);
    Line line{6, true, true};
    maps.access(4096, false, false);
    const State previous = maps.access(0, false, false);
    EXPECT_EQ(previous, Init);
    EXPECT_TRUE(line.touchMap(false, previous));
    EXPECT_EQ(line.rrpv, 0);
    EXPECT_FALSE(line.unusedPrefetch);
}

TEST(PAMAIPTest, MapPrefetchStateSuppressesHitsIndependentlyOfLineHistory)
{
    Line line{6, true, false};
    EXPECT_FALSE(line.touchMap(true, Accessed));
    EXPECT_EQ(line.rrpv, 6);
    EXPECT_FALSE(line.touchMap(false, Prefetched));
    EXPECT_EQ(line.rrpv, 6);
    EXPECT_TRUE(line.touchMap(false, Accessed));
    EXPECT_EQ(line.rrpv, 0);
}

// The sampler observes the request stream independently of real-cache victims.
TEST(PAMAIPReuseTest, ColdAndUnsampledRequestsDoNotPredict)
{
    FirstUsePredictor predictor;
    EXPECT_EQ(predictor.predict(0x1000, false), FirstUsePredictor::Unknown);
    unsigned skipped = 0;
    for (unsigned i = 0; i < 512; ++i) {
        skipped += !predictor.observe(i * 64, false, true).sampled;
    }
    EXPECT_GT(skipped, 400);
}

TEST(PAMAIPReuseTest, SecondDemandTrainsOnceAndRequiresConfidence)
{
    FirstUsePredictor predictor(1, 4, 1);
    for (unsigned i = 0; i < 3; ++i) {
        const uint64_t address = i * 64;
        predictor.observe(address, false, true);
        EXPECT_FALSE(predictor.observe(address, false, false).positive);
        EXPECT_TRUE(predictor.observe(address, false, false).positive);
        EXPECT_FALSE(predictor.observe(address, false, false).positive);
        EXPECT_EQ(predictor.predict(address, false), i < 2 ?
                  FirstUsePredictor::Unknown : FirstUsePredictor::Reuse);
    }
}

static void
sampleOneUse(FirstUsePredictor &predictor, uint64_t address)
{
    predictor.observe(address, false, true);
    predictor.observe(address, false, false);
    unsigned negative = 0;
    for (unsigned i = 0; i < 4; ++i) {
        negative += predictor.observe(0x100000 + address + i * 64,
                                      false, false).negative;
    }
    EXPECT_EQ(negative, 1);
}

TEST(PAMAIPReuseTest, OneUseRetirementTrainsLowConfidence)
{
    FirstUsePredictor predictor(1, 4, 1);
    for (unsigned i = 0; i < 3; ++i) {
        sampleOneUse(predictor, i * 4096);
        EXPECT_EQ(predictor.predict(0, false), i < 2 ?
                  FirstUsePredictor::Unknown : FirstUsePredictor::Dead);
    }
}

TEST(PAMAIPReuseTest, NeverUsedPrefetchesDoNotTrainSecondUse)
{
    FirstUsePredictor predictor(1, 4, 1);
    unsigned unused = 0;
    for (unsigned i = 0; i < 16; ++i) {
        const auto result = predictor.observe(i * 64, false, true);
        unused += result.unused;
        EXPECT_FALSE(result.negative);
        EXPECT_FALSE(result.positive);
    }
    EXPECT_EQ(unused, 12);
    EXPECT_EQ(predictor.predict(0, false), FirstUsePredictor::Unknown);
}

TEST(PAMAIPReuseTest, DemandOriginAndPrefetchHitsCannotInventTraining)
{
    FirstUsePredictor predictor(1, 4, 1);
    predictor.observe(0, false, false);
    predictor.observe(0, false, true);
    EXPECT_FALSE(predictor.observe(0, false, false).positive);
    predictor.observe(64, false, true);
    predictor.observe(64, false, false);
    for (unsigned i = 0; i < 5; ++i) {
        EXPECT_FALSE(predictor.observe(64, false, true).positive);
    }
    EXPECT_TRUE(predictor.observe(64, false, false).positive);
}

TEST(PAMAIPReuseTest, SamplerRetainsEvidenceAcrossRealCacheMisses)
{
    FirstUsePredictor predictor(1, 4, 1);
    predictor.observe(0, false, true);
    predictor.observe(0, false, false);
    predictor.observe(64, false, false);
    predictor.observe(128, false, false);
    // This can be a miss in the real cache: no real-cache hit is required.
    EXPECT_TRUE(predictor.observe(0, false, false).positive);
}

TEST(PAMAIPReuseTest, SecurityDomainsDoNotShareSampleLifetimes)
{
    FirstUsePredictor predictor(1, 4, 1);
    predictor.observe(0, false, true);
    EXPECT_FALSE(predictor.observe(0, true, false).positive);
    EXPECT_FALSE(predictor.observe(0, false, false).positive);
    EXPECT_TRUE(predictor.observe(0, false, false).positive);
}

TEST(PAMAIPReuseTest, SamplerLRURefreshProtectsTheObservedLifetime)
{
    FirstUsePredictor predictor(1, 4, 1);
    for (unsigned i = 0; i < 4; ++i) {
        predictor.observe(i * 64, false, true);
    }
    predictor.observe(0, false, false);
    const auto eviction = predictor.observe(256, false, false);
    EXPECT_TRUE(eviction.unused);
    EXPECT_FALSE(eviction.negative);
    EXPECT_TRUE(predictor.observe(0, false, false).positive);
}

TEST(PAMAIPReuseTest, ReusedSampleRetirementDoesNotTrainTwice)
{
    FirstUsePredictor predictor(1, 4, 1);
    predictor.observe(0, false, true);
    predictor.observe(0, false, false);
    EXPECT_TRUE(predictor.observe(0, false, false).positive);
    for (unsigned i = 1; i <= 4; ++i) {
        const auto result = predictor.observe(i * 64, false, false);
        EXPECT_FALSE(result.positive);
        EXPECT_FALSE(result.negative);
    }
    EXPECT_EQ(predictor.predict(0, false), FirstUsePredictor::Unknown);
}

TEST(PAMAIPReuseTest, LowPredictionCanBeCorrectedByLaterDemand)
{
    FirstUsePredictor predictor(1, 4, 1);
    for (unsigned i = 0; i < 3; ++i) {
        sampleOneUse(predictor, i * 4096);
    }
    EXPECT_EQ(predictor.predict(0, false), FirstUsePredictor::Dead);
    predictor.observe(0x4000, false, true);
    predictor.observe(0x4000, false, false);
    const auto correction = predictor.observe(0x4000, false, false);
    EXPECT_TRUE(correction.positive);
    EXPECT_EQ(correction.evaluated, FirstUsePredictor::Dead);
    EXPECT_FALSE(correction.correct);
    EXPECT_EQ(predictor.predict(0, false), FirstUsePredictor::Unknown);
}

TEST(PAMAIPReuseTest, ConfidentReusePredictionIsEvaluatedAtFirstUse)
{
    FirstUsePredictor predictor(1, 4, 1);
    for (unsigned i = 0; i < 3; ++i) {
        predictor.observe(i * 64, false, true);
        predictor.observe(i * 64, false, false);
        predictor.observe(i * 64, false, false);
    }
    predictor.observe(256, false, true);
    predictor.observe(256, false, false);
    const auto feedback = predictor.observe(256, false, false);
    EXPECT_EQ(feedback.evaluated, FirstUsePredictor::Reuse);
    EXPECT_TRUE(feedback.correct);
}

TEST(PAMAIPReuseTest, PredictionCountersSaturateAndCanReverse)
{
    FirstUsePredictor predictor(1, 4, 1);
    for (unsigned i = 0; i < 100; ++i) {
        predictor.observe(i * 64, false, true);
        predictor.observe(i * 64, false, false);
        predictor.observe(i * 64, false, false);
    }
    EXPECT_EQ(predictor.predict(0, false), FirstUsePredictor::Reuse);
    for (unsigned i = 0; i < 100; ++i) {
        sampleOneUse(predictor, 0x1000000 + i * 4096);
    }
    EXPECT_EQ(predictor.predict(0, false), FirstUsePredictor::Dead);
}
