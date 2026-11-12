/*
 * Copyright (c) 2026 XiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>

#include "mem/cache/replacement_policies/kpc_sampler.hh"

namespace gem5
{
namespace replacement_policy
{

using Training = KPCSampler::Training;

TEST(KPCSampler, InsertionChangesOnlyAtSaturation)
{
    KPCSampler s(1, 1, 1, 3, 0);
    for (unsigned i = 0; i < 8; ++i) {
        EXPECT_EQ(s.insertionRRPV(false), 2);
        s.access(0, i, false, false);
    }
    EXPECT_EQ(s.hysteresis(false), 7);
    EXPECT_EQ(s.insertionRRPV(false), 3);
    EXPECT_EQ(s.insertionRRPV(true), 2);
    s.access(0, 8, false, false);
    EXPECT_EQ(s.hysteresis(false), 7);
    EXPECT_EQ(s.access(0, 8, false, true).training, Training::DemandReuse);
    EXPECT_EQ(s.hysteresis(false), 6);
    EXPECT_EQ(s.insertionRRPV(false), 2);
}

TEST(KPCSampler, ReuseTrainsAllocationTypeOnlyOnce)
{
    KPCSampler s(1, 2, 1, 3, 4);
    s.access(0, 10, false, true);
    EXPECT_EQ(s.access(0, 10, false, false).training, Training::PrefetchReuse);
    EXPECT_EQ(s.hysteresis(true), 3);
    EXPECT_EQ(s.hysteresis(false), 4);
    EXPECT_EQ(s.access(0, 10, false, true).training, Training::None);
    EXPECT_EQ(s.access(0, 10, false, false).training, Training::None);
    EXPECT_EQ(s.hysteresis(true), 3);
}

TEST(KPCSampler, UnusedVictimTrainsAllocationType)
{
    KPCSampler s(1, 1, 1, 3, 0);
    EXPECT_EQ(s.access(0, 10, false, true).training, Training::None);
    EXPECT_EQ(s.access(0, 20, false, false).training, Training::PrefetchDead);
    EXPECT_EQ(s.access(0, 30, false, true).training, Training::DemandDead);
    EXPECT_EQ(s.hysteresis(false), 1);
    EXPECT_EQ(s.hysteresis(true), 1);
}

TEST(KPCSampler, UsedVictimDoesNotTrainAgain)
{
    KPCSampler s(1, 1, 1, 3, 2);
    s.access(0, 10, false, true);
    s.access(0, 10, false, false);
    EXPECT_EQ(s.access(0, 20, false, false).training, Training::None);
    EXPECT_EQ(s.hysteresis(true), 1);
    EXPECT_EQ(s.hysteresis(false), 2);
}

TEST(KPCSampler, LRUOrderDoesNotDependOnSimulationTicks)
{
    KPCSampler s(1, 2, 1, 3, 3);
    s.access(0, 10, false, false);
    s.access(0, 20, false, true);
    s.access(0, 10, false, true);
    // A PF hit also refreshes true-LRU recency and preserves allocation type.
    EXPECT_EQ(s.access(0, 30, false, false).training, Training::PrefetchDead);
    EXPECT_TRUE(s.access(0, 10, false, false).hit);
    EXPECT_FALSE(s.access(0, 20, false, false).hit);
}

TEST(KPCSampler, SecureAndFullAddressTagsRemainDistinct)
{
    KPCSampler s(1, 4, 1, 3, 3);
    s.access(0, 0x1000, false, true);
    EXPECT_FALSE(s.access(0, 0x1000, true, false).hit);
    EXPECT_FALSE(s.access(0, 0x100001000, false, false).hit);
    EXPECT_TRUE(s.access(0, 0x1000, false, false).hit);
    EXPECT_EQ(s.hysteresis(true), 2);
    EXPECT_EQ(s.hysteresis(false), 3);
}

TEST(KPCSampler, SamplingIsBoundedDeterministicAndIndependent)
{
    KPCSampler a(4096, 8, 64, 3, 0);
    KPCSampler b(4096, 8, 64, 3, 0);
    unsigned count = 0;
    for (unsigned set = 0; set < 4096; ++set) {
        EXPECT_EQ(a.sampled(set), b.sampled(set));
        count += a.sampled(set);
        if (!a.sampled(set)) {
            for (unsigned i = 0; i < 10; ++i) {
                EXPECT_FALSE(a.access(set, i, false, false).sampled);
            }
        }
    }
    EXPECT_EQ(count, 64);
    EXPECT_EQ(a.hysteresis(false), 0);
}

TEST(KPCSampler, SlicesShareCountersButNotLRUEntries)
{
    KPCSampler s(4, 1, 4, 3, 0);
    // Set zero in four slices maps to four independent global set numbers.
    for (unsigned slice = 0; slice < 4; ++slice) {
        EXPECT_FALSE(s.access(slice, slice * 64, false, true).hit);
        EXPECT_EQ(s.access(slice, 4096 + slice * 64, false, false).training,
                  Training::PrefetchDead);
    }
    EXPECT_EQ(s.hysteresis(true), 4);
    EXPECT_EQ(s.hysteresis(false), 0);
}

TEST(KPCSampler, ReuseSaturatesAtZero)
{
    KPCSampler s(1, 1, 1, 1, 0);
    s.access(0, 10, false, false);
    s.access(0, 10, false, false);
    EXPECT_EQ(s.hysteresis(false), 0);
    EXPECT_EQ(s.insertionRRPV(false), 2);
}

} // namespace replacement_policy
} // namespace gem5
