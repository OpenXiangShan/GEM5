#include <gtest/gtest.h>

#include "mem/cache/prefetch/compound_bop_policy.hh"

namespace gem5::prefetch::compound_bop
{

TEST(CompoundBOPDelay, ScoresBothFamiliesStartingAtTheSameDeadline)
{
    StudentDelayQueues queues(1, 300);
    constexpr unsigned index = 42;
    EXPECT_TRUE(queues.enqueue(Family::VBOP, 10, index, 1));
    EXPECT_FALSE(queues.enqueue(Family::VBOP, 10, index, 2));
    // A full V queue must not block P, even though V slots are visited first.
    EXPECT_TRUE(queues.enqueue(Family::PBOP, 10, index, 16));
    EXPECT_FALSE(queues.enqueue(Family::PBOP, 10, index, 32));
    uint64_t hits = 0;
    auto record = [&](unsigned i, uint64_t mask) {
        EXPECT_EQ(i, index);
        hits |= mask;
    };
    queues.drain(309, record);
    EXPECT_EQ(hits, 0U);
    queues.drain(310, record);
    EXPECT_EQ(hits, 1U | 16U);
    EXPECT_EQ(queues.size(), 0U);
}

TEST(CompoundBOPDelay, PhaseResetDiscardsPendingPredictionsFromBothFamilies)
{
    StudentDelayQueues queues(2, 300);
    ASSERT_TRUE(queues.enqueue(Family::VBOP, 10, 1, 1));
    ASSERT_TRUE(queues.enqueue(Family::PBOP, 11, 2, 16));
    queues.clear();
    ASSERT_TRUE(queues.enqueue(Family::PBOP, 20, 3, 32));
    queues.drain(319, [](unsigned, uint64_t) {
        FAIL() << "stale prediction";
    });
    unsigned drained = 0;
    queues.drain(320, [&](unsigned i, uint64_t mask) {
        EXPECT_EQ(i, 3U);
        EXPECT_EQ(mask, 32U);
        ++drained;
    });
    EXPECT_EQ(drained, 1U);
}

} // namespace gem5::prefetch::compound_bop
