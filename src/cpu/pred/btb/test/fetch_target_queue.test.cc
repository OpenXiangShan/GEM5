#include <gtest/gtest.h>

#include "cpu/pred/btb/ftq.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace test
{

class FetchTargetQueueTest : public ::testing::Test
{
  protected:
    FetchTarget makeTarget(ThreadID tid, Addr startPC)
    {
        FetchTarget target;
        target.tid = tid;
        target.startPC = startPC;
        return target;
    }
};

TEST_F(FetchTargetQueueTest, SquashAfterFirstTargetDropsSecondTarget)
{
    FetchTargetQueue ftq(1, 8);
    auto target0 = makeTarget(0, 0x1000);
    auto target1 = makeTarget(0, 0x2000);

    ftq.insert(target0);
    ftq.insert(target1);

    ASSERT_EQ(ftq.size(0), 2U);
    EXPECT_EQ(ftq.frontId(0), 1U);
    EXPECT_EQ(ftq.backId(0), 2U);

    ftq.squashAfter(1, 0);

    EXPECT_EQ(ftq.size(0), 1U);
    EXPECT_TRUE(ftq.hasTarget(1, 0));
    EXPECT_FALSE(ftq.hasTarget(2, 0));
    EXPECT_EQ(ftq.fetchId(0), 2U);
    EXPECT_EQ(ftq.front(0).startPC, 0x1000U);
}

TEST_F(FetchTargetQueueTest, FreeSlotsTracksTwoTakenCapacity)
{
    FetchTargetQueue ftq(1, 2);
    auto target0 = makeTarget(0, 0x1000);

    EXPECT_EQ(ftq.freeSlots(0), 2U);

    ftq.insert(target0);

    EXPECT_EQ(ftq.freeSlots(0), 1U);
    EXPECT_FALSE(ftq.full(0));

    auto target1 = makeTarget(0, 0x2000);
    ftq.insert(target1);

    EXPECT_EQ(ftq.freeSlots(0), 0U);
    EXPECT_TRUE(ftq.full(0));
}

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5
