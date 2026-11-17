// Copyright (c) 2026 Beijing Institute of Open Source Chip (BOSC)
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "mem/cache/icache_banks.hh"

namespace gem5::icache_banks
{

TEST(IcacheBanks, EffectiveRangeBoundaries)
{
    EXPECT_EQ(bankMask(0, 64), 0xff);
    EXPECT_EQ(bankMask(62, 2), 0x80);
    EXPECT_EQ(bankMask(6, 4), 0x03);
    EXPECT_EQ(bankMask(64, 2), 0x01);
    EXPECT_EQ(bankMask(64, 0), 0);
}

TEST(IcacheBanks, ReadContentionAndFutureReservations)
{
    ReadScheduler scheduler;
    EXPECT_EQ(scheduler.reserve(0, 4, 0, 1, 0xff), 0);
    // Two 64B blocks in different ways can read together.
    EXPECT_EQ(scheduler.reserve(0, 4, 1, 2, 0xff), 0);
    EXPECT_EQ(scheduler.reserve(0, 4, 0, 1, 0xff), 0);
    // Even small blocks can contend on the same bank/way across sets.
    EXPECT_EQ(scheduler.reserve(0, 4, 0, 2, 0x01), 1);
    EXPECT_EQ(scheduler.reserve(1, 5, 0, 3, 0x01), 1);
    EXPECT_EQ(scheduler.reserve(1, 5, 0, 3, 0x02), 0);
    EXPECT_EQ(scheduler.reserve(1, 5, 0, 4, 0), 0);
    EXPECT_EQ(scheduler.reserve(10, 14, 0, 4, 0xff), 0);
}

} // namespace gem5::icache_banks
