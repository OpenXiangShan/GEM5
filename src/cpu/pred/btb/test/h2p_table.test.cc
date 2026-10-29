#include <gtest/gtest.h>

#include <array>
#include <stdexcept>

#include "cpu/pred/btb/h2p_table.hh"

namespace gem5::branch_prediction::btb_pred::test
{

TEST(H2PTableTest, ShadowLookupDoesNotChangeReplacementOrder)
{
    H2PTable table(4);
    constexpr Addr setStride = 2 * H2PTable::LineBytes;
    const Addr first = 0x1000;
    const Addr second = first + setStride;
    const Addr replacement = second + setStride;
    for (Addr pc : {first, second}) {
        for (int i = 0; i < 3; ++i)
            table.trainMispred(pc);
    }
    EXPECT_TRUE(table.peek(first).h2p);
    table.trainMispred(replacement);
    EXPECT_FALSE(table.peek(first).hit);
    EXPECT_TRUE(table.peek(second).h2p);
}

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

TEST(H2PTableTest, ConfigurableLineAlignment)
{
    H2PTable narrow(4, 32, 2, 1);
    H2PTable wide(4, 64, 2, 1);
    for (auto *table : {&narrow, &wide})
        EXPECT_TRUE(table->trainMispred(0x1000).allocated);

    EXPECT_TRUE(narrow.trainMispred(0x1020).allocated);
    EXPECT_TRUE(wide.trainMispred(0x1020).dropped);
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

TEST(H2PTableTest, ConfigurableBranchesPerEntry)
{
    H2PTable table(4, 32, 2, 3);
    for (Addr pc : {0x1000, 0x1004, 0x1008})
        EXPECT_TRUE(table.trainMispred(pc).allocated);
    EXPECT_TRUE(table.trainMispred(0x100c).dropped);
    for (Addr pc : {0x1000, 0x1004, 0x1008})
        EXPECT_TRUE(table.peek(pc).hit);
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
    const Addr second = 0x105c;
    for (int i = 0; i < 3; ++i) {
        table.trainMispred(first);
        table.trainMispred(second);
    }
    EXPECT_TRUE(table.lookup(first).h2p);
    EXPECT_TRUE(table.lookup(second).h2p);
}

TEST(H2PTableTest, BranchesAcrossLineBoundaryRemainDistinct)
{
    H2PTable table(128);
    const Addr first = 0x1000 + H2PTable::LineBytes - 4;
    const Addr second = first + 4;
    EXPECT_TRUE(table.trainMispred(first).allocated);
    EXPECT_TRUE(table.trainMispred(second).allocated);
    EXPECT_TRUE(table.lookup(first).hit);
    EXPECT_TRUE(table.lookup(second).hit);
}

TEST(H2PTableTest, ThirdBranchInLineIsDropped)
{
    H2PTable table(128);
    const Addr first = 0x1040;
    const Addr second = 0x105c;
    const Addr third = 0x1050;
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

TEST(H2PTableTest, TwoWaySetAssociativityHandlesDistinctLines)
{
    H2PTable table(4);
    constexpr Addr lineStride = H2PTable::LineBytes;
    const std::array<Addr, 4> pcs = {
        0x1000, 0x1000 + lineStride, 0x1000 + 2 * lineStride,
        0x1000 + 3 * lineStride};
    for (const auto pc : pcs)
        table.trainMispred(pc);

    for (const auto pc : pcs)
        EXPECT_TRUE(table.lookup(pc).hit);
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
