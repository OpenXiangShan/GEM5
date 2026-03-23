#include <gtest/gtest.h>

#include "cpu/pred/btb/ftq.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace
{

FetchTarget
makeTarget(ThreadID tid, Addr start_pc)
{
    FetchTarget target;
    target.tid = tid;
    target.startPC = start_pc;
    target.predEndPC = start_pc + 4;
    return target;
}

} // namespace

TEST(FetchTargetQueueTest, PrefetchHeadStartsAlignedWithFetchHead)
{
    FetchTargetQueue queue(1, 8);
    auto first = makeTarget(0, 0x1000);
    auto second = makeTarget(0, 0x1040);
    queue.insert(first);
    queue.insert(second);

    EXPECT_EQ(queue.fetchId(0), 1u);
    EXPECT_EQ(queue.prefetchId(0), 1u);
    EXPECT_TRUE(queue.hasPrefetch(0));
    EXPECT_EQ(queue.prefetching(0).startPC, 0x1000u);
}

TEST(FetchTargetQueueTest, FinishTargetClampsPrefetchHeadToFetchHead)
{
    FetchTargetQueue queue(1, 8);
    auto first = makeTarget(0, 0x1000);
    auto second = makeTarget(0, 0x1040);
    queue.insert(first);
    queue.insert(second);

    queue.finishTarget(0);

    EXPECT_EQ(queue.fetchId(0), 2u);
    EXPECT_EQ(queue.prefetchId(0), 2u);
    EXPECT_TRUE(queue.hasPrefetch(0));
    EXPECT_EQ(queue.fetching(0).startPC, 0x1040u);
    EXPECT_EQ(queue.prefetching(0).startPC, 0x1040u);
}

TEST(FetchTargetQueueTest, PrefetchHeadCanAdvanceIndependently)
{
    FetchTargetQueue queue(1, 8);
    auto first = makeTarget(0, 0x1000);
    auto second = makeTarget(0, 0x1040);
    auto third = makeTarget(0, 0x1080);
    queue.insert(first);
    queue.insert(second);
    queue.insert(third);

    queue.finishTarget(0);
    queue.finishPrefetchTarget(0);

    EXPECT_EQ(queue.fetchId(0), 2u);
    EXPECT_EQ(queue.prefetchId(0), 3u);
    EXPECT_EQ(queue.fetching(0).startPC, 0x1040u);
    EXPECT_EQ(queue.prefetching(0).startPC, 0x1080u);
}

TEST(FetchTargetQueueTest, SquashAlignsPrefetchHeadWithFetchHead)
{
    FetchTargetQueue queue(1, 8);
    auto first = makeTarget(0, 0x1000);
    auto second = makeTarget(0, 0x1040);
    auto third = makeTarget(0, 0x1080);
    queue.insert(first);
    queue.insert(second);
    queue.insert(third);

    queue.finishTarget(0);
    queue.finishPrefetchTarget(0);
    queue.squashAfter(1, 0);

    EXPECT_EQ(queue.fetchId(0), 2u);
    EXPECT_EQ(queue.prefetchId(0), 2u);
    EXPECT_FALSE(queue.hasPrefetch(0));
}

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
