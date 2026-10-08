#include <gtest/gtest.h>

#include "mem/cache/prefetch/demand_pair_history.hh"

namespace gem5::prefetch
{

TEST(LldpDemandPair, DynamicSequenceContextAndReplacement)
{
    DemandPairHistory<4> history;
    EXPECT_FALSE(history.lookup(1, 0x100, 0));
    history.record(1, 0x100, 0x2008, 0);
    ASSERT_EQ(history.lookup(1, 0x100, 0), 0x2008);
    EXPECT_FALSE(history.lookup(1, 0x100, 1));
    EXPECT_FALSE(history.lookup(1, 0x104, 0));
    EXPECT_FALSE(history.lookup(5, 0x100, 0));
    history.record(5, 0x100, 0x3008, 0);
    EXPECT_FALSE(history.lookup(1, 0x100, 0));
    ASSERT_EQ(history.lookup(5, 0x100, 0), 0x3008);
    // Exact PC/sequence/context tags keep zero addresses legal.
    history.record(0, 0, 0, 0);
    ASSERT_TRUE(history.lookup(0, 0, 0));
    EXPECT_EQ(*history.lookup(0, 0, 0), 0);
}

TEST(LldpDemandPair, DemandEvidenceIsIndependentOfPrefetchIssue)
{
    DemandPairHistory<8> history;
    unsigned pairs = 0;
    for (unsigned sequence = 1; sequence < 4096; sequence += 2) {
        // Only retirement addresses enter this table; no candidate/useful
        // event is needed to retain an exact dynamic producer mapping.
        history.record(sequence, 0x100, sequence * 64 + 8, 0);
        const auto producer = history.lookup(sequence, 0x100, 0);
        ASSERT_TRUE(producer);
        EXPECT_EQ(*producer, sequence * 64 + 8);
        ++pairs;
        history.record(sequence + 1, 0x200, (sequence + 1) * 64, 0);
    }
    EXPECT_EQ(pairs, 2048);
}

} // namespace gem5::prefetch
