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

namespace
{

FetchTarget
target(ThreadID tid)
{
    FetchTarget entry;
    entry.tid = tid;
    return entry;
}

} // anonymous namespace

TEST(FetchTargetQueueTest, ReadySizeTracksUnconsumedTargets)
{
    FetchTargetQueue ftq(2, 8);
    auto t0 = target(0);
    auto t1 = target(1);

    EXPECT_EQ(ftq.readySize(0), 0);
    EXPECT_EQ(ftq.readySize(1), 0);

    ftq.insert(t0);
    ftq.insert(t0);
    ftq.insert(t0);
    ftq.insert(t1);
    EXPECT_EQ(ftq.readySize(0), 3);
    EXPECT_EQ(ftq.readySize(1), 1);

    ftq.finishTarget(0);
    EXPECT_EQ(ftq.readySize(0), 2);

    ftq.commitTarget(0);
    EXPECT_EQ(ftq.size(0), 2);
    EXPECT_EQ(ftq.readySize(0), 2);
}

TEST(FetchTargetQueueTest, ReadySizeResetsAfterSquashAndClear)
{
    FetchTargetQueue ftq(1, 8);
    auto entry = target(0);

    ftq.insert(entry);
    ftq.insert(entry);
    ftq.insert(entry);
    EXPECT_EQ(ftq.readySize(0), 3);

    ftq.squashAfter(ftq.frontId(0), 0);
    EXPECT_EQ(ftq.size(0), 1);
    EXPECT_EQ(ftq.readySize(0), 0);

    ftq.clear(0);
    EXPECT_EQ(ftq.size(0), 0);
    EXPECT_EQ(ftq.readySize(0), 0);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
