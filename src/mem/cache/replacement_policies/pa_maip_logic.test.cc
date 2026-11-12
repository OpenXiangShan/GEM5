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
