#include <gtest/gtest.h>

#include "cpu/pred/btb/h2p_buffer_model.hh"

namespace gem5::branch_prediction::btb_pred::test
{

namespace
{

BranchOutcome
outcome(ThreadID tid, FetchTargetId ftqId, Addr pc, bool mispredicted)
{
    BranchOutcome branch;
    branch.tid = tid;
    branch.ftqId = ftqId;
    branch.pc = pc;
    branch.isCond = true;
    branch.mispredicted = mispredicted;
    return branch;
}

} // namespace

TEST(H2PBufferModelTest, AdmitsAndCountsCapacity)
{
    H2PBufferModel model(1);
    EXPECT_EQ(model.add(0, 1, 0x1000).admitted, true);
    EXPECT_EQ(model.add(0, 2, 0x2000).rejectedFull, true);
    EXPECT_EQ(model.occupancy(), 1u);
    EXPECT_EQ(model.maxOccupancy(), 1u);

    EXPECT_TRUE(model.resolve(outcome(0, 1, 0x1000, true)).admitted);
    EXPECT_EQ(model.occupancy(), 0u);
    EXPECT_TRUE(model.add(0, 3, 0x3000).admitted);
    EXPECT_FALSE(model.commit(outcome(0, 2, 0x2000, true)).admitted);
    EXPECT_TRUE(model.commit(outcome(0, 1, 0x1000, true)).found);
    EXPECT_EQ(model.occupancy(), 1u);
}

TEST(H2PBufferModelTest, ResolutionFreesEntryAndKeepsCommitMetadata)
{
    H2PBufferModel model(1);
    EXPECT_TRUE(model.add(0, 1, 0x1000).admitted);
    EXPECT_TRUE(model.resolve(outcome(0, 1, 0x1000, false)).found);
    EXPECT_EQ(model.occupancy(), 0u);
    EXPECT_TRUE(model.add(0, 2, 0x2000).admitted);
    EXPECT_TRUE(model.commit(outcome(0, 1, 0x1000, false)).found);
    EXPECT_EQ(model.occupancy(), 1u);
}

TEST(H2PBufferModelTest, SquashRemovesYoungerTargets)
{
    H2PBufferModel model(2);
    EXPECT_TRUE(model.add(0, 1, 0x1000).admitted);
    EXPECT_TRUE(model.add(0, 2, 0x2000).admitted);
    EXPECT_EQ(model.squashAfter(1, 0), 1u);
    EXPECT_TRUE(model.commit(outcome(0, 1, 0x1000, true)).found);
    EXPECT_FALSE(model.commit(outcome(0, 2, 0x2000, true)).found);
}

TEST(H2PBufferModelTest, ControlSquashKeepsResolvingBranch)
{
    H2PBufferModel model(2);
    EXPECT_TRUE(model.add(0, 1, 0x1000).admitted);
    EXPECT_TRUE(model.add(0, 1, 0x1010).admitted);
    EXPECT_EQ(model.squashTargetExcept(1, 0, 0x1000), 1u);
    EXPECT_TRUE(model.commit(outcome(0, 1, 0x1000, true)).found);
    EXPECT_FALSE(model.commit(outcome(0, 1, 0x1010, true)).found);
}

TEST(H2PBufferModelTest, SquashDropsResolvedCommitMetadata)
{
    H2PBufferModel model(1);
    EXPECT_TRUE(model.add(0, 2, 0x2000).admitted);
    EXPECT_TRUE(model.resolve(outcome(0, 2, 0x2000, true)).found);
    EXPECT_EQ(model.occupancy(), 0u);
    EXPECT_EQ(model.squashAfter(1, 0), 0u);
    EXPECT_FALSE(model.commit(outcome(0, 2, 0x2000, true)).found);
}

TEST(H2PBufferModelTest, SquashCountsOnlyAdmittedActiveEntries)
{
    H2PBufferModel model(1);
    EXPECT_TRUE(model.add(0, 1, 0x1000).admitted);
    EXPECT_TRUE(model.add(0, 2, 0x2000).rejectedFull);
    EXPECT_EQ(model.squashAfter(0, 0), 1u);
    EXPECT_EQ(model.occupancy(), 0u);
    EXPECT_FALSE(model.commit(outcome(0, 1, 0x1000, true)).found);
    EXPECT_FALSE(model.commit(outcome(0, 2, 0x2000, true)).found);
}

TEST(H2PBufferModelTest, RetireDropsUnresolvedEntries)
{
    H2PBufferModel model(1);
    EXPECT_TRUE(model.add(0, 7, 0x7000).admitted);
    EXPECT_TRUE(model.add(0, 7, 0x7010).rejectedFull);

    EXPECT_EQ(model.retireTarget(0, 7), 1u);
    EXPECT_EQ(model.occupancy(), 0u);
    EXPECT_TRUE(model.add(0, 8, 0x8000).admitted);
    EXPECT_FALSE(model.commit(outcome(0, 7, 0x7000, true)).found);
}

} // namespace gem5::branch_prediction::btb_pred::test
