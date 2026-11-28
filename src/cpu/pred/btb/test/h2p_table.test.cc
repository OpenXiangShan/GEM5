#include <gtest/gtest.h>

#include "cpu/pred/btb/h2p_table.hh"

namespace gem5::branch_prediction::btb_pred::test
{

TEST(H2PTableTest, TrainsExistingBranchesDespiteAllocationFilterAndAges)
{
    H2PTable table;
    const Addr pc = 0x1000;
    EXPECT_TRUE(table.trainMispred(pc, false).allocationFiltered);
    EXPECT_FALSE(table.lookup(pc).hit);
    EXPECT_TRUE(table.trainMispred(pc).allocated);
    table.trainMispred(pc, false);
    EXPECT_FALSE(table.lookup(pc).h2p);
    table.trainMispred(pc, false);
    EXPECT_TRUE(table.lookup(pc).h2p);
    for (int i = 0; i < 10; ++i)
        table.trainMispred(pc);
    for (int i = 0; i < 6; ++i)
        table.age();
    EXPECT_TRUE(table.lookup(pc).hit);
    EXPECT_FALSE(table.lookup(pc).h2p);
    table.age();
    EXPECT_FALSE(table.lookup(pc).hit);
}

TEST(H2PTableTest, ConfiguredLineKeepsIndependentSlotsAfterReplacement)
{
    H2PTable table(2, 64, 1, 3);
    for (Addr pc : {0x1000, 0x1004, 0x103e})
        EXPECT_TRUE(table.trainMispred(pc).allocated);
    EXPECT_FALSE(table.lookup(0x1000).h2p);
    EXPECT_TRUE(table.trainMispred(0x1008).dropped);
    EXPECT_TRUE(table.trainMispred(0x1040).allocated);
    EXPECT_TRUE(table.lookup(0x103e).hit);
    EXPECT_TRUE(table.trainMispred(0x1080).replaced);
    EXPECT_FALSE(table.lookup(0x1000).hit);
    EXPECT_TRUE(table.trainMispred(0x1084).allocated);
    EXPECT_TRUE(table.trainMispred(0x1088).allocated);
    EXPECT_TRUE(table.trainMispred(0x108c).dropped);
}

} // namespace gem5::branch_prediction::btb_pred::test
