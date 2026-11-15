/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>

#include "mem/cache/replacement_policies/pa_maip_logic.hh"

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
