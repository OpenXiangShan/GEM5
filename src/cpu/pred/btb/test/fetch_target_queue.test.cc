#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "cpu/pred/btb/fetch_target_queue.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/test/test_dprintf.hh"


namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

// Forward declarations and aliases needed for testing
using FetchTargetId = uint64_t;
using FetchStreamId = uint64_t;
using Addr = uint64_t;


// Test fixture for FetchTargetQueue
class FetchTargetQueueTest : public ::testing::Test
{
protected:
    std::unique_ptr<FetchTargetQueue> ftq;
    const unsigned defaultFtqSize = 16;
    FetchStreamId streamId = 1;

    void SetUp() override {
        ftq = std::make_unique<FetchTargetQueue>(defaultFtqSize);
    }

    void TearDown() override {
        ftq.reset();
    }

    // helper method to create a ftq entry
    FtqEntry createFtqEntry(Addr startPC, Addr endPC) {
        FtqEntry entry;
        entry.startPC = startPC;
        entry.endPC = endPC;
        entry.fsqID = streamId;
        streamId++;
        return entry;
    }

    // Helper method to populate FTQ with sequential entries
    void populateFtq(int numEntries, Addr startPC = 0x1000) {
        for (int i = 0; i < numEntries; i++) {
            Addr pc = startPC + i * 8;
            FtqEntry entry = createFtqEntry(pc, pc + 8);
            ftq->enqueue(entry);
        }
    }

    // Helper method to populate FTQ with entries including some loops
    void populateFtqWithLoops(int numEntries, Addr startPC = 0x1000) {
        // Loop metadata is not modeled in the BTB FTQ any more; keep this helper
        // for test readability and future extension.
        populateFtq(numEntries, startPC);
    }
};

// Test basic initialization
TEST_F(FetchTargetQueueTest, BasicInitialization) {
    EXPECT_TRUE(ftq->empty());
    EXPECT_EQ(ftq->size(), 0);
    EXPECT_FALSE(ftq->full());
    EXPECT_FALSE(ftq->fetchTargetAvailable());
}

// Test enqueue operation
TEST_F(FetchTargetQueueTest, Enqueue) {
    FtqEntry entry1 = createFtqEntry(0x1000, 0x1008);
    FtqEntry entry2 = createFtqEntry(0x1008, 0x1010);

    EXPECT_TRUE(ftq->empty());

    ftq->enqueue(entry1);
    EXPECT_EQ(ftq->size(), 1);
    EXPECT_FALSE(ftq->empty());

    ftq->enqueue(entry2);
    EXPECT_EQ(ftq->size(), 2);
}

// Test full queue detection
TEST_F(FetchTargetQueueTest, QueueFull) {
    populateFtq(defaultFtqSize);
    EXPECT_TRUE(ftq->full());

    // Add one more to exceed capacity
    FtqEntry entry = createFtqEntry(0x2000, 0x2008);
    ftq->enqueue(entry);
    EXPECT_EQ(ftq->size(), defaultFtqSize + 1);
}

// FTQ is head-driven FIFO: if non-empty, head is always available.
TEST_F(FetchTargetQueueTest, HeadFetchTarget) {
    populateFtq(5);

    EXPECT_TRUE(ftq->fetchTargetAvailable());

    FtqEntry& target = ftq->getTarget();
    EXPECT_EQ(target.startPC, 0x1000);
    EXPECT_EQ(target.endPC, 0x1008);
}

// Test fetch target advancement
TEST_F(FetchTargetQueueTest, AdvanceFetchTarget) {
    populateFtq(5);

    EXPECT_TRUE(ftq->fetchTargetAvailable());

    // Finish current target and advance
    ftq->finishCurrentFetchTarget();
    EXPECT_TRUE(ftq->fetchTargetAvailable());

    FtqEntry& target = ftq->getTarget();
    EXPECT_EQ(target.startPC, 0x1008);
    EXPECT_EQ(target.endPC, 0x1010);
}

// Test squash operation
TEST_F(FetchTargetQueueTest, Squash) {
    populateFtq(5);

    // Verify we have entries
    EXPECT_EQ(ftq->size(), 5);

    // Squash the queue
    Addr newPC = 0x2000;
    FetchStreamId newStreamId = 3;
    FetchTargetId newTargetId = 2; // streamid = targetid + 1!

    ftq->squash(newTargetId, newStreamId, newPC);

    // Verify queue is cleared
    EXPECT_TRUE(ftq->empty());
    EXPECT_EQ(ftq->size(), 0);

    // Verify enqueue state is updated
    auto& enqState = ftq->getEnqState();
    EXPECT_EQ(enqState.pc, newPC);
    EXPECT_EQ(enqState.streamId, newStreamId);
    EXPECT_EQ(enqState.nextEnqTargetId, newTargetId);

    // Verify fetch demand target is updated
    EXPECT_EQ(ftq->getSupplyingTargetId(), newTargetId);
}

// Test PC reset
TEST_F(FetchTargetQueueTest, ResetPC) {
    populateFtq(5);

    EXPECT_TRUE(ftq->fetchTargetAvailable());

    // Reset PC
    Addr newPC = 0x3000;
    ftq->resetPC(newPC);

    // Verify PC is updated
    EXPECT_EQ(ftq->getEnqState().pc, newPC);
}

// Test edge case with empty queue
TEST_F(FetchTargetQueueTest, EmptyQueue) {
    EXPECT_FALSE(ftq->fetchTargetAvailable());
}

// Test multiple consecutive fetches
TEST_F(FetchTargetQueueTest, MultipleFetches) {
    populateFtq(10);

    // Fetch and process 5 entries in sequence
    for (int i = 0; i < 5; i++) {
        Addr pc = 0x1000 + i * 8;
        EXPECT_TRUE(ftq->fetchTargetAvailable());

        FtqEntry& target = ftq->getTarget();
        EXPECT_EQ(target.startPC, pc);

        ftq->finishCurrentFetchTarget();
    }

    // Verify we're at the expected target ID
    EXPECT_EQ(ftq->getSupplyingTargetId(), 5);
    EXPECT_EQ(ftq->size(), 5);  // 5 entries left
}

}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
