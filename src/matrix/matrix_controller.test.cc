#include <gtest/gtest.h>

#include "base/gtest/cur_tick_fake.hh"
#include "base/stats/group.hh"
#include "matrix/matrix_controller.hh"

namespace gem5
{
namespace matrix
{
namespace
{

GTestTickHandler tickHandler;

MatrixController::TimingConfig
testTimingConfig()
{
    MatrixController::TimingConfig config;
    config.issueIntervalCycles = 2;
    config.zeroCycles = 3;
    config.computeBaseCycles = 5;
    config.computeReadCycles = 1;
    config.releaseCycles = 7;
    return config;
}

statistics::Counter
value(const statistics::Scalar &stat)
{
    return stat.value();
}

} // namespace

TEST(MatrixControllerTest, MmaUpdatesDataAndSchedulesFixedTiming)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    controller.setTimingConfig(testTimingConfig());
    controller.setTileM(2);
    controller.setTileN(2);
    controller.setTileK(3);

    controller.zeroAcc(nullptr);
    controller.writeABRegForTest(MatrixController::DefaultAReg, 0, 0, 1);
    controller.writeABRegForTest(MatrixController::DefaultAReg, 0, 1, 2);
    controller.writeABRegForTest(MatrixController::DefaultAReg, 0, 2, 3);
    controller.writeABRegForTest(MatrixController::DefaultAReg, 1, 0, 4);
    controller.writeABRegForTest(MatrixController::DefaultAReg, 1, 1, 5);
    controller.writeABRegForTest(MatrixController::DefaultAReg, 1, 2, 6);
    controller.writeABRegForTest(MatrixController::DefaultBReg, 0, 0, 7);
    controller.writeABRegForTest(MatrixController::DefaultBReg, 0, 1, 8);
    controller.writeABRegForTest(MatrixController::DefaultBReg, 0, 2, 9);
    controller.writeABRegForTest(MatrixController::DefaultBReg, 1, 0, 10);
    controller.writeABRegForTest(MatrixController::DefaultBReg, 1, 1, 11);
    controller.writeABRegForTest(MatrixController::DefaultBReg, 1, 2, 12);

    controller.mmaccWB(nullptr);

    EXPECT_EQ(controller.readAccRegForTest(MatrixController::DefaultAccReg, 0,
                  0),
        50);
    EXPECT_EQ(controller.readAccRegForTest(MatrixController::DefaultAccReg, 0,
                  1),
        68);
    EXPECT_EQ(controller.readAccRegForTest(MatrixController::DefaultAccReg, 1,
                  0),
        122);
    EXPECT_EQ(controller.readAccRegForTest(MatrixController::DefaultAccReg, 1,
                  1),
        167);

    const auto snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.decodedQueueSize, 0);
    EXPECT_EQ(snapshot.fuBusyMask, 0);
    EXPECT_EQ(snapshot.abPendingReaderMask, 0);
    EXPECT_EQ(snapshot.cPendingReaderMask, 0);
    EXPECT_EQ(snapshot.timingLastIssueTick, 3);
    EXPECT_EQ(snapshot.timingLastCompletionTick, 8);
    EXPECT_EQ(snapshot.timingNextIssueTick, 5);

    const auto &stats = controller.getStats();
    EXPECT_EQ(value(stats.tasksAccepted), 2);
    EXPECT_EQ(value(stats.tasksIssued), 2);
    EXPECT_EQ(value(stats.tasksCompleted), 2);
    EXPECT_EQ(value(stats.zeroTasks), 1);
    EXPECT_EQ(value(stats.mmaTasks), 1);
    EXPECT_EQ(value(stats.timingTasks), 2);
    EXPECT_EQ(value(stats.timingQueueTicks), 3);
    EXPECT_EQ(value(stats.timingMaxQueueTicks), 3);
    EXPECT_EQ(value(stats.timingBusyTicks), 8);
    EXPECT_EQ(value(stats.timingLastIssueTick), 3);
    EXPECT_EQ(value(stats.timingLastCompletionTick), 8);
    EXPECT_EQ(stats.taskEvents[static_cast<size_t>(
                  MatrixController::TaskEvent::ComputeIssue)],
        1);
}

TEST(MatrixControllerTest, MmaTimingDoesNotScaleWithShape)
{
    tickHandler.setCurTick(0);
    statistics::Group small_root(nullptr);
    MatrixController small(&small_root);
    small.setTimingConfig(testTimingConfig());
    small.setTileM(2);
    small.setTileN(2);
    small.setTileK(3);
    small.mmaccWB(nullptr);

    tickHandler.setCurTick(0);
    statistics::Group large_root(nullptr);
    MatrixController large(&large_root);
    large.setTimingConfig(testTimingConfig());
    large.setTileM(8);
    large.setTileN(8);
    large.setTileK(8);
    large.mmaccWB(nullptr);

    EXPECT_EQ(small.controlSnapshot().timingLastIssueTick, 0);
    EXPECT_EQ(large.controlSnapshot().timingLastIssueTick, 0);
    EXPECT_EQ(small.controlSnapshot().timingLastCompletionTick, 5);
    EXPECT_EQ(large.controlSnapshot().timingLastCompletionTick, 5);
}

TEST(MatrixControllerTest, ReleaseWaitsForScheduledMatrixWork)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    controller.setTimingConfig(testTimingConfig());
    controller.setTileM(2);
    controller.setTileN(2);
    controller.setTileK(3);

    controller.zeroAcc(nullptr);
    controller.mmaccWB(nullptr);
    controller.release(nullptr, 0);

    const auto snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.timingLastIssueTick, 8);
    EXPECT_EQ(snapshot.timingLastCompletionTick, 15);
    EXPECT_EQ(snapshot.timingNextIssueTick, 10);
    EXPECT_EQ(snapshot.pendingTokenEvents, 1);
    EXPECT_EQ(controller.readTokenForTest(0), 0);

    const auto &stats = controller.getStats();
    EXPECT_EQ(value(stats.releaseTasks), 1);
    EXPECT_EQ(value(stats.tokenReleaseEvents), 1);
    EXPECT_EQ(value(stats.timingTasks), 3);
    EXPECT_EQ(value(stats.timingQueueTicks), 11);
    EXPECT_EQ(value(stats.timingMaxQueueTicks), 8);
    EXPECT_EQ(value(stats.timingBusyTicks), 15);
    EXPECT_EQ(stats.taskEvents[static_cast<size_t>(
                  MatrixController::TaskEvent::ReleaseIssue)],
        1);
}

TEST(MatrixControllerTest, TimingConfigClampsZeroIssueInterval)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    auto config = testTimingConfig();
    config.issueIntervalCycles = 0;
    config.zeroCycles = 1;
    controller.setTimingConfig(config);

    controller.zero(nullptr, 0, true);
    controller.zero(nullptr, 1, false);

    const auto snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.timingLastIssueTick, 1);
    EXPECT_EQ(snapshot.timingLastCompletionTick, 2);
    EXPECT_EQ(snapshot.timingNextIssueTick, 2);
    EXPECT_EQ(value(controller.getStats().timingQueueTicks), 1);
}

TEST(MatrixControllerTest, MemoryPipelineLimitsOutstandingSources)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    auto config = testTimingConfig();
    config.issueIntervalCycles = 1;
    config.loadBaseCycles = 0;
    config.localMmuArbCycles = 0;
    config.l2RequestPipelineCycles = 0;
    config.localMmuIssuePerCycle = MatrixController::LocalMmuSourceCount;
    config.localMmuReadLatencyCycles = 10;
    config.l2ResponsePipelineCycles = 1;
    controller.setTimingConfig(config);

    controller.scheduleMemoryTimingForTest(
        MatrixController::MemPort::A, false,
        MatrixController::LocalMmuSourceCount + 1);

    const auto snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.timingLastIssueTick, 0);
    EXPECT_EQ(snapshot.timingLocalMmuLastRequestTick, 11);
    EXPECT_EQ(snapshot.timingLocalMmuLastResponseTick, 74);
    EXPECT_EQ(snapshot.timingLastCompletionTick, 74);
    EXPECT_EQ(snapshot.timingLocalMmuOutstanding,
        MatrixController::LocalMmuSourceCount);

    const auto &stats = controller.getStats();
    EXPECT_EQ(value(stats.memoryPipelineRequests), 65);
    EXPECT_EQ(value(stats.memoryPipelineReadResponses), 65);
    EXPECT_EQ(value(stats.memoryPipelineWriteAcks), 0);
    EXPECT_EQ(value(stats.memoryPipelineSourceStallTicks), 10);
    EXPECT_EQ(value(stats.memoryPipelineRequestQueueTicks), 1);
    EXPECT_EQ(value(stats.memoryPipelineResponseQueueTicks), 2069);
    EXPECT_EQ(value(stats.memoryPipelineLastRequestTick), 11);
    EXPECT_EQ(value(stats.memoryPipelineLastResponseTick), 74);
    EXPECT_EQ(value(stats.memoryPipelineMaxOutstanding),
        MatrixController::LocalMmuSourceCount);
}

TEST(MatrixControllerTest, MemoryPipelineChoosesSourceAtIssueTick)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    auto config = testTimingConfig();
    config.issueIntervalCycles = 1;
    config.loadBaseCycles = 0;
    config.localMmuArbCycles = 0;
    config.l2RequestPipelineCycles = 0;
    config.localMmuIssuePerCycle = 1;
    config.localMmuReadLatencyCycles = 0;
    config.l2ResponsePipelineCycles = 1;
    controller.setTimingConfig(config);

    controller.scheduleMemoryTimingForTest(
        MatrixController::MemPort::A, false, 2);

    const auto snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.timingLocalMmuLastRequestTick, 1);
    EXPECT_EQ(snapshot.timingLocalMmuLastResponseTick, 1);
    const auto &stats = controller.getStats();
    EXPECT_EQ(value(stats.memoryPipelineSourceStallTicks), 0);
    EXPECT_EQ(value(stats.memoryPipelineRequestQueueTicks), 1);
}

TEST(MatrixControllerTest, MemoryPipelineSeparatesWriteAckResponses)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    auto config = testTimingConfig();
    config.issueIntervalCycles = 1;
    config.storeBaseCycles = 0;
    config.localMmuArbCycles = 0;
    config.l2RequestPipelineCycles = 0;
    config.localMmuIssuePerCycle = 2;
    config.localMmuWriteAckLatencyCycles = 5;
    config.l2ResponsePipelineCycles = 1;
    controller.setTimingConfig(config);

    controller.scheduleMemoryTimingForTest(
        MatrixController::MemPort::CStore, true, 2);

    const auto snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.timingLocalMmuLastRequestTick, 0);
    EXPECT_EQ(snapshot.timingLocalMmuLastResponseTick, 6);
    EXPECT_EQ(snapshot.timingLastCompletionTick, 6);

    const auto &stats = controller.getStats();
    EXPECT_EQ(value(stats.memoryPipelineRequests), 2);
    EXPECT_EQ(value(stats.memoryPipelineReadResponses), 0);
    EXPECT_EQ(value(stats.memoryPipelineWriteAcks), 2);
    EXPECT_EQ(value(stats.memoryPipelineResponseQueueTicks), 1);
    EXPECT_EQ(stats.memPortRequests[static_cast<size_t>(
                  MatrixController::MemPort::CStore)],
        2);
}

TEST(MatrixControllerTest, CStoreTimingUsesRoundedStridedRequestCount)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    auto config = testTimingConfig();
    config.issueIntervalCycles = 1;
    config.storeBaseCycles = 0;
    config.localMmuArbCycles = 0;
    config.l2RequestPipelineCycles = 0;
    config.localMmuIssuePerCycle = MatrixController::LocalMmuSourceCount;
    config.localMmuWriteAckLatencyCycles = 5;
    config.l2ResponsePipelineCycles = 1;
    controller.setTimingConfig(config);

    controller.scheduleMemoryTimingForTest(
        MatrixController::MemPort::CStore, true, 0x103c, 0x80, 3, 2,
        MatrixController::ElemWidth::E32, false);

    const auto &stats = controller.getStats();
    EXPECT_EQ(value(stats.memoryPipelineRequests), 8);
    EXPECT_EQ(value(stats.memoryPipelineWriteAcks), 8);
    EXPECT_EQ(stats.memPortRequests[static_cast<size_t>(
                  MatrixController::MemPort::CStore)],
        8);
}

TEST(MatrixControllerTest, ControlLocalMmuUsesHighestFreeSource)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);

    auto snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.nextLocalMmuSource,
        MatrixController::LocalMmuSourceCount - 1);

    const uint8_t first = controller.allocateLocalMmuSourceForTest(
        MatrixController::MemPort::A);
    const uint8_t second = controller.allocateLocalMmuSourceForTest(
        MatrixController::MemPort::B);
    EXPECT_EQ(first, MatrixController::LocalMmuSourceCount - 1);
    EXPECT_EQ(second, MatrixController::LocalMmuSourceCount - 2);

    snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.nextLocalMmuSource,
        MatrixController::LocalMmuSourceCount - 3);
    EXPECT_EQ(snapshot.localMmuOutstanding, 2);
    EXPECT_TRUE(snapshot.localMmuBusySourceMask & (1ULL << first));
    EXPECT_TRUE(snapshot.localMmuBusySourceMask & (1ULL << second));

    controller.releaseLocalMmuSourceForTest(first);
    snapshot = controller.controlSnapshot();
    EXPECT_EQ(snapshot.nextLocalMmuSource, first);
    EXPECT_EQ(snapshot.localMmuOutstanding, 1);

    const uint8_t reused = controller.allocateLocalMmuSourceForTest(
        MatrixController::MemPort::CLoad);
    EXPECT_EQ(reused, first);
    EXPECT_EQ(value(controller.getStats().localMmuSourceAllocations), 3);
    EXPECT_EQ(value(controller.getStats().localMmuSourceReleases), 1);
}

TEST(MatrixControllerTest, SyncResetClearsPendingTokenEvents)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    auto config = testTimingConfig();
    config.releaseCycles = 5;
    controller.setTimingConfig(config);

    controller.release(nullptr, 3);
    ASSERT_EQ(controller.controlSnapshot().pendingTokenEvents, 1);

    controller.syncReset(3);

    EXPECT_EQ(controller.controlSnapshot().pendingTokenEvents, 0);
    EXPECT_EQ(controller.readTokenForTest(3), 0);
    EXPECT_EQ(value(controller.getStats().tokenReleaseEvents), 1);
}

TEST(MatrixControllerTest, LaterTaskDoesNotRetirePendingTokenRelease)
{
    tickHandler.setCurTick(0);
    statistics::Group root(nullptr);
    MatrixController controller(&root);
    auto config = testTimingConfig();
    config.releaseCycles = 1;
    controller.setTimingConfig(config);

    controller.release(nullptr, 0);
    ASSERT_EQ(controller.tokenReadyTickForTest(0, 1), 1);
    ASSERT_EQ(controller.readTokenForTest(0), 0);

    tickHandler.setCurTick(10);
    controller.zeroAcc(nullptr);

    EXPECT_EQ(controller.readTokenForTest(0), 0);
    EXPECT_EQ(controller.tokenReadyTickForTest(0, 1), 1);

    controller.retireReadyTokensUpTo(10);
    EXPECT_EQ(controller.readTokenForTest(0), 1);
}

} // namespace matrix
} // namespace gem5
