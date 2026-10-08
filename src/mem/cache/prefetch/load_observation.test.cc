#include <gtest/gtest.h>

#include "mem/cache/prefetch/load_observation.hh"

namespace gem5::prefetch
{

TEST(LldpLoadObservation, ReplayKeepsFirstMissAndSplitRequiresEveryFragment)
{
    LoadObservation observation;
    observation.observe(0x1000, false, 0);
    observation.observe(0x1000, true, 15); // replay after refill
    EXPECT_EQ(observation.fragments, 1);
    EXPECT_EQ(observation.hitSources, 0);
    EXPECT_TRUE(observation.complete(1));
    EXPECT_FALSE(observation.allHit(1));
    EXPECT_FALSE(observation.complete(2));
    observation.observe(0x1040, true, 16);
    EXPECT_TRUE(observation.complete(2));
    EXPECT_FALSE(observation.allHit(2));
    EXPECT_EQ(observation.hitSources, uint64_t(1) << 16);
}

TEST(LldpLoadObservation, RepeatedLoadsHaveSeparateDynamicObservations)
{
    LoadObservation first, second;
    first.observe(0x1000, true, 15);
    second.observe(0x1000, true, 15);
    EXPECT_TRUE(first.allHit(1));
    EXPECT_TRUE(second.allHit(1));
    first.woken = true;
    first.woken = false; // accepted wakeup was canceled
    EXPECT_FALSE(first.woken);
    EXPECT_FALSE(second.woken);
    LoadObservation forwarded;
    EXPECT_FALSE(forwarded.complete(1));
    EXPECT_FALSE(forwarded.allHit(1));
}

TEST(LldpLoadObservation, BoundedOverflowIsExplicitlyUnobserved)
{
    LoadObservation observation;
    for (unsigned i = 0; i < 9; ++i)
        observation.observe(i * 64, true, 15);
    EXPECT_EQ(observation.fragments, 8);
    EXPECT_TRUE(observation.overflow);
    EXPECT_FALSE(observation.complete(9));
    EXPECT_FALSE(observation.allHit(8));
}

} // namespace gem5::prefetch
