#include <gtest/gtest.h>

#include "cpu/pred/btb/h2p_table.hh"

namespace gem5::branch_prediction::btb_pred::test
{

TEST(H2PTableTest, RequiresThreeMispredictions)
{
    H2PTable table(128);
    const Addr pc = 0x1040;
    EXPECT_FALSE(table.lookup(pc).h2p);
    table.trainMispred(pc);
    table.trainMispred(pc);
    EXPECT_FALSE(table.lookup(pc).h2p);
    table.trainMispred(pc);
    EXPECT_TRUE(table.lookup(pc).h2p);
}

TEST(H2PTableTest, SaturatesAndAges)
{
    H2PTable table(128);
    const Addr pc = 0x1040;
    for (int i = 0; i < 10; ++i)
        table.trainMispred(pc);
    EXPECT_TRUE(table.lookup(pc).h2p);
    for (int i = 0; i < 7; ++i)
        table.age();
    EXPECT_FALSE(table.lookup(pc).hit);
}

TEST(H2PTableTest, TwoBranchesShareLine)
{
    H2PTable table(128);
    const Addr first = 0x1040;
    const Addr second = 0x107c;
    for (int i = 0; i < 3; ++i) {
        table.trainMispred(first);
        table.trainMispred(second);
    }
    EXPECT_TRUE(table.lookup(first).h2p);
    EXPECT_TRUE(table.lookup(second).h2p);
}

TEST(H2PTableTest, ThirdBranchInLineIsDropped)
{
    H2PTable table(128);
    const Addr first = 0x1040;
    const Addr second = 0x107c;
    const Addr third = 0x1060;
    for (int i = 0; i < 3; ++i) {
        table.trainMispred(first);
        table.trainMispred(second);
    }
    const auto result = table.trainMispred(third);
    EXPECT_TRUE(result.dropped);
    EXPECT_FALSE(table.lookup(third).hit);
}

TEST(H2PTableTest, DifferentLinesDoNotAlias)
{
    H2PTable table(128);
    const Addr first = 0x1040;
    const Addr second = first + H2PTable::LineBytes;
    for (int i = 0; i < 3; ++i)
        table.trainMispred(first);
    EXPECT_FALSE(table.lookup(second).hit);
}

TEST(H2PTableTest, AllocationFilterRejectsNewBranch)
{
    H2PTable table(4);
    const Addr pc = 0x1000;

    const auto result = table.trainMispred(pc, false);

    EXPECT_TRUE(result.allocationFiltered);
    EXPECT_FALSE(result.allocated);
    EXPECT_FALSE(table.lookup(pc).hit);
}

TEST(H2PTableTest, AllocationFilterStillUpdatesExistingBranch)
{
    H2PTable table(4);
    const Addr pc = 0x1000;

    EXPECT_TRUE(table.trainMispred(pc, true).allocated);
    EXPECT_TRUE(table.trainMispred(pc, false).incremented);
    EXPECT_TRUE(table.trainMispred(pc, false).incremented);
    EXPECT_TRUE(table.lookup(pc).h2p);
}

TEST(H2PTableTest, FullyAssociativeTableAvoidsSetConflicts)
{
    H2PTable table(4);
    constexpr Addr oldSetStride = H2PTable::LineBytes * 16;
    for (unsigned entry = 0; entry < 4; ++entry) {
        const Addr pc = 0x1000 + entry * oldSetStride;
        table.trainMispred(pc);
    }

    for (unsigned entry = 0; entry < 4; ++entry)
        EXPECT_TRUE(table.lookup(0x1000 + entry * oldSetStride).hit);
}

TEST(H2PTableTest, FullTableUsesLruWhenAllCountersAreActive)
{
    H2PTable table(4);
    constexpr Addr lineStride = H2PTable::LineBytes;
    for (unsigned entry = 0; entry < 4; ++entry) {
        const Addr pc = 0x1000 + entry * lineStride;
        table.trainMispred(pc);
        table.trainMispred(pc);
    }

    const Addr replacement = 0x1000 + 4 * lineStride;
    const auto result = table.trainMispred(replacement);

    EXPECT_TRUE(result.replaced);
    EXPECT_TRUE(table.lookup(replacement).hit);
    EXPECT_FALSE(table.lookup(0x1000).hit);
}

TEST(H2PTableTest, FullTablePrioritizesEntryWithZeroCounters)
{
    H2PTable table(4);
    constexpr Addr lineStride = H2PTable::LineBytes;
    const Addr reusable = 0x1000;
    const Addr oldestActive = reusable + lineStride;

    table.trainMispred(reusable);
    for (unsigned entry = 1; entry < 4; ++entry) {
        const Addr pc = reusable + entry * lineStride;
        for (int i = 0; i < 3; ++i)
            table.trainMispred(pc);
    }
    table.age();
    EXPECT_FALSE(table.lookup(reusable).hit);

    // Make the reusable entry newer than an active entry. Counter state must
    // still take priority over the global LRU order.
    table.trainMispred(reusable);
    table.age();
    EXPECT_FALSE(table.lookup(reusable).hit);

    const Addr replacement = reusable + 4 * lineStride;
    const auto result = table.trainMispred(replacement);

    EXPECT_TRUE(result.replaced);
    EXPECT_TRUE(table.lookup(replacement).hit);
    EXPECT_FALSE(table.lookup(reusable).hit);
    EXPECT_TRUE(table.lookup(oldestActive).hit);
}

} // namespace gem5::branch_prediction::btb_pred::test
