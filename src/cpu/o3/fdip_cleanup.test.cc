#include <gtest/gtest.h>

#include <deque>
#include <vector>

#include "cpu/o3/fdip_cleanup.hh"

namespace gem5
{

namespace o3
{

namespace
{

struct FakeThreadState
{
    bool valid = true;
    unsigned resetCount = 0;

    void reset()
    {
        valid = false;
        ++resetCount;
    }
};

struct FakePendingRequest
{
    ThreadID tid = 0;
    bool outstanding = false;
};

struct FakeProbeHint
{
    int token = 0;
};

} // namespace

TEST(FdipCleanupTest, ClearsOnlyTargetThreadPendingRequestsAndHints)
{
    FakeThreadState state;
    std::vector<FakePendingRequest> pending{
        {0, true}, {0, false}, {1, true}};
    std::deque<FakeProbeHint> hints{{1}, {2}};

    const auto summary = cleanupFdipPartialState(
        0, state, pending, hints, 2);

    EXPECT_FALSE(state.valid);
    EXPECT_EQ(state.resetCount, 1u);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front().tid, 1);
    EXPECT_TRUE(pending.front().outstanding);
    EXPECT_TRUE(hints.empty());
    EXPECT_EQ(summary.outstandingLines, 1u);
    EXPECT_EQ(summary.removedPendingRequests, 2u);
    EXPECT_EQ(summary.clearedProbeHints, 2u);
}

TEST(FdipCleanupTest, EmptyHintQueueStillResetsStateWithoutUnderflow)
{
    FakeThreadState state;
    std::vector<FakePendingRequest> pending{{1, true}};
    std::deque<FakeProbeHint> hints;

    const auto summary = cleanupFdipPartialState(
        0, state, pending, hints, 1);

    EXPECT_FALSE(state.valid);
    EXPECT_EQ(state.resetCount, 1u);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_EQ(pending.front().tid, 1);
    EXPECT_EQ(summary.outstandingLines, 1u);
    EXPECT_EQ(summary.removedPendingRequests, 0u);
    EXPECT_EQ(summary.clearedProbeHints, 0u);
}

} // namespace o3
} // namespace gem5
