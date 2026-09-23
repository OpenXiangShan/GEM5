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

TEST(H2PTableTest, SetUsesDeterministicLruReplacement)
{
    H2PTable table(128);
    constexpr Addr setStride = H2PTable::LineBytes * 16;
    for (unsigned way = 0; way < H2PTable::Ways; ++way)
        table.trainMispred(0x1000 + way * setStride);

    const auto result = table.trainMispred(0x1000 + H2PTable::Ways * setStride);
    EXPECT_TRUE(result.replaced);
    EXPECT_TRUE(table.lookup(0x1000 + H2PTable::Ways * setStride).hit);
}

} // namespace gem5::branch_prediction::btb_pred::test
