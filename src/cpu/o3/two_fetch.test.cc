// Copyright (c) 2026 Beijing Institute of Open Source Chip (BOSC)
// SPDX-License-Identifier: BSD-3-Clause

#include "cpu/o3/two_fetch.hh"
#include "gtest/gtest.h"
#include "mem/cache/icache_banks.hh"

namespace gem5::o3::two_fetch
{

TEST(TwoFetch, UsesTakenInstructionEndRatherThanWindow)
{
    EXPECT_EQ(effectiveEnd(0x1100, 0x1140, true, 0x1108, 2), 0x110a);
    EXPECT_EQ(effectiveEnd(0x1100, 0x1140, true, 0x1108, 4), 0x110c);
    EXPECT_EQ(effectiveEnd(0x1100, 0x1140, false, 0, 0), 0x1140);
    EXPECT_FALSE(effectiveEnd(0x1100, 0x1140, true, 0x113e, 4));
    EXPECT_FALSE(effectiveEnd(0x1100, 0x1140, true, 0x1108, 0));
}

TEST(TwoFetch, CountsExecutedBytesNotAddressEnvelope)
{
    EXPECT_EQ(canPair(0x1100, 0x1108, 0x1188, 0x1188, 0x1190, 64), Reject::None);
    EXPECT_EQ(canPair(0x1188, 0x1190, 0x1100, 0x1100, 0x1108, 64), Reject::None);
    EXPECT_EQ(canPair(0x1100, 0x1128, 0x1104, 0x1104, 0x1128, 64), Reject::Size);
    EXPECT_EQ(canPair(0x1100, 0x1120, 0x1180, 0x1180, 0x11a0, 64), Reject::None);
    EXPECT_EQ(canPair(0x1100, 0x1120, 0x1180, 0x1180, 0x11a2, 64), Reject::Size);
}

TEST(TwoFetch, PageAndControlFlowBoundaries)
{
    EXPECT_EQ(canPair(0xff0, 0xff8, 0x1000, 0x1000, 0x1008, 64), Reject::Page);
    EXPECT_EQ(canPair(0xfe0, 0xfe8, 0xff0, 0xff0, 0x1002, 64), Reject::CrossPage);
    EXPECT_EQ(canPair(0xfe0, 0xfe8, 0xff0, 0xff0, 0x1000, 64), Reject::None);
    EXPECT_EQ(canPair(0x1100, 0x1108, 0x1180, 0x1188, 0x1190, 64), Reject::Link);
}

TEST(TwoFetch, DetectsControlInTheMiddleOfAnInstruction)
{
    EXPECT_TRUE(controlAlreadyPassed(true, 0x1120, 0x1122));
    EXPECT_FALSE(controlAlreadyPassed(true, 0x1122, 0x1122));
    EXPECT_FALSE(controlAlreadyPassed(false, 0x1120, 0x1122));
    EXPECT_TRUE(skippedControl(true, 0x1102, 0x1100, 0x1104));
    EXPECT_FALSE(skippedControl(true, 0x1100, 0x1100, 0x1104));
    EXPECT_FALSE(skippedControl(true, 0x1104, 0x1100, 0x1104));
    EXPECT_FALSE(skippedControl(false, 0x1102, 0x1100, 0x1104));
}

TEST(ICacheBanks, SelectsEightByteBanks)
{
    using icache_banks::bankMask;
    EXPECT_EQ(bankMask(0x1100, 8, 64, 8), 0x01);
    EXPECT_EQ(bankMask(0x1106, 4, 64, 8), 0x03);
    EXPECT_EQ(bankMask(0x113e, 2, 64, 8), 0x80);
    EXPECT_EQ(bankMask(0x1140, 2, 64, 8), 0x01);
    EXPECT_EQ(bankMask(0x1100, 64, 64, 8), 0xff);
}

TEST(ICacheBanks, MatchesRtlWaymaskAndSetConstraints)
{
    using icache_banks::conflict;
    EXPECT_TRUE(conflict(0x3, 0x2, 1, 1, 1, 2));
    EXPECT_FALSE(conflict(0x3, 0x4, 1, 1, 1, 2));
    EXPECT_FALSE(conflict(0x3, 0x2, 1, 2, 1, 2));
    EXPECT_FALSE(conflict(0x3, 0x2, 1, 1, 1, 1));
    EXPECT_TRUE(conflict(0x3, 0x2, 0, 0, 1, 2));
    EXPECT_FALSE(conflict(0x3, 0x2, 0, 1, 1, 2));
}

}  // namespace gem5::o3::two_fetch
