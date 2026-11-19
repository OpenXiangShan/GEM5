#include <gtest/gtest.h>

#include <stdexcept>

#include "cpu/pred/btb/h2p_table.hh"

namespace gem5::branch_prediction::btb_pred::test
{

TEST(H2PTableTest, ThresholdSaturationAndAging)
{
    H2PTable table;
    const Addr pc = 0x1040;
    EXPECT_FALSE(table.lookup(pc).hit);
    table.trainMispred(pc);
    table.trainMispred(pc);
    EXPECT_FALSE(table.lookup(pc).h2p);
    table.trainMispred(pc);
    EXPECT_TRUE(table.lookup(pc).h2p);

    for (int i = 0; i < 10; ++i)
        table.trainMispred(pc);
    for (int i = 0; i < 7; ++i)
        table.age();
    EXPECT_FALSE(table.lookup(pc).hit);
}

TEST(H2PTableTest, BranchSlotsAreIndependentAndLimited)
{
    H2PTable table;
    const Addr first = 0x1040;
    const Addr second = 0x105c;
    for (int i = 0; i < 3; ++i)
        table.trainMispred(first);
    EXPECT_TRUE(table.trainMispred(second).allocated);
    EXPECT_TRUE(table.peek(first).h2p);
    EXPECT_TRUE(table.peek(second).hit);
    EXPECT_FALSE(table.peek(second).h2p);

    EXPECT_TRUE(table.trainMispred(0x1050).dropped);
    EXPECT_FALSE(table.peek(0x1050).hit);
    EXPECT_FALSE(table.peek(first + H2PTable::LineBytes).hit);
}

TEST(H2PTableTest, ConfigurableLineAlignment)
{
    H2PTable narrow(4, 32, 2, 1);
    H2PTable wide(4, 64, 2, 1);
    for (auto *table : {&narrow, &wide})
        EXPECT_TRUE(table->trainMispred(0x101c).allocated);

    EXPECT_TRUE(narrow.trainMispred(0x1020).allocated);
    EXPECT_TRUE(wide.trainMispred(0x1020).dropped);
    EXPECT_TRUE(narrow.peek(0x101c).hit);
    EXPECT_TRUE(narrow.peek(0x1020).hit);
}

TEST(H2PTableTest, ConfigurableAssociativity)
{
    H2PTable direct(4, 32, 1);
    H2PTable associative(4, 32, 4);
    for (Addr pc : {0x1000, 0x1080, 0x1100, 0x1180}) {
        direct.trainMispred(pc);
        associative.trainMispred(pc);
    }
    EXPECT_FALSE(direct.peek(0x1000).hit);
    for (Addr pc : {0x1000, 0x1080, 0x1100, 0x1180})
        EXPECT_TRUE(associative.peek(pc).hit);
}

TEST(H2PTableTest, ConfiguredEntryRetainsSlotsAfterReplacement)
{
    H2PTable table(2, 64, 1, 3);
    table.trainMispred(0x1000);
    EXPECT_TRUE(table.trainMispred(0x1080).replaced);
    EXPECT_FALSE(table.peek(0x1000).hit);
    EXPECT_TRUE(table.trainMispred(0x1084).allocated);
    EXPECT_TRUE(table.trainMispred(0x1088).allocated);
    EXPECT_TRUE(table.trainMispred(0x108c).dropped);
}

TEST(H2PTableTest, ConfigurableCounterWidthAndThreshold)
{
    H2PTable table(4, 32, 2, 2, 4, 9);
    for (int i = 0; i < 8; ++i)
        table.trainMispred(0x1000);
    EXPECT_FALSE(table.peek(0x1000).h2p);
    table.trainMispred(0x1000);
    EXPECT_TRUE(table.peek(0x1000).h2p);
    table.age();
    EXPECT_FALSE(table.peek(0x1000).h2p);

    for (int i = 0; i < 300; ++i)
        table.trainMispred(0x1000);
    for (int i = 0; i < 6; ++i)
        table.age();
    EXPECT_TRUE(table.peek(0x1000).h2p);
    table.age();
    EXPECT_FALSE(table.peek(0x1000).h2p);
}

TEST(H2PTableTest, EightBitCounterDoesNotWrap)
{
    H2PTable table(4, 32, 2, 2, 8, 255);
    for (int i = 0; i < 300; ++i)
        table.trainMispred(0x1000);
    EXPECT_TRUE(table.peek(0x1000).h2p);
    table.age();
    EXPECT_FALSE(table.peek(0x1000).h2p);
    EXPECT_TRUE(table.peek(0x1000).hit);
}

TEST(H2PTableTest, InvalidConfigurationIsRejected)
{
    EXPECT_THROW(H2PTable(0), std::invalid_argument);
    EXPECT_THROW(H2PTable(3), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 0), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 8), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 0), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 48), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 2, 0), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 2, 17), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 2, 2, 0), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 2, 2, 9), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 2, 2, 3, 0), std::invalid_argument);
    EXPECT_THROW(H2PTable(4, 32, 2, 2, 3, 8), std::invalid_argument);
}

TEST(H2PTableTest, AllocationFilterRejectsNewAndTrainsExistingBranches)
{
    H2PTable table(4);
    const Addr pc = 0x1000;
    const auto rejected = table.trainMispred(pc, false);
    EXPECT_TRUE(rejected.allocationFiltered);
    EXPECT_FALSE(rejected.allocated);
    EXPECT_FALSE(table.peek(pc).hit);

    EXPECT_TRUE(table.trainMispred(pc).allocated);
    EXPECT_TRUE(table.trainMispred(pc, false).incremented);
    EXPECT_TRUE(table.trainMispred(pc, false).incremented);
    EXPECT_TRUE(table.peek(pc).h2p);
}

TEST(H2PTableTest, LookupAndPeekHaveDifferentReplacementEffects)
{
    constexpr Addr stride = 2 * H2PTable::LineBytes;
    const Addr first = 0x1000;
    const Addr second = first + stride;
    const Addr replacement = second + stride;
    for (bool updateRecency : {false, true}) {
        SCOPED_TRACE(updateRecency);
        H2PTable table(4);
        table.trainMispred(first);
        table.trainMispred(second);
        const auto result = updateRecency ? table.lookup(first) : table.peek(first);
        EXPECT_TRUE(result.hit);
        EXPECT_TRUE(table.trainMispred(replacement).replaced);
        EXPECT_TRUE(table.peek(replacement).hit);
        EXPECT_EQ(table.peek(first).hit, updateRecency);
        EXPECT_EQ(table.peek(second).hit, !updateRecency);
    }
}

TEST(H2PTableTest, ReclaimsInactiveLineBeforeActiveLru)
{
    H2PTable table(2);
    const Addr reusable = 0x1000;
    const Addr active = reusable + H2PTable::LineBytes;
    const Addr replacement = active + H2PTable::LineBytes;
    table.trainMispred(reusable);
    for (int i = 0; i < 3; ++i)
        table.trainMispred(active);
    table.age();

    // Make the inactive line newer than the active line in the same set.
    table.trainMispred(reusable);
    table.age();
    EXPECT_FALSE(table.peek(reusable).hit);
    EXPECT_TRUE(table.peek(active).hit);

    EXPECT_TRUE(table.trainMispred(replacement).replaced);
    EXPECT_TRUE(table.peek(replacement).hit);
    EXPECT_TRUE(table.peek(active).hit);
}

}  // namespace gem5::branch_prediction::btb_pred::test
