// Unit tests for BTBRAS
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/pred/btb/ras.hh"
#include "cpu/pred/btb/stream_struct.hh"

namespace gem5 {
namespace branch_prediction {
namespace btb_pred {
namespace test {

// Test fixture for BTBRAS
class RASTest : public ::testing::Test
{
protected:
    void SetUp() override {
        // Create BTBRAS instance with test constructor
        ras = std::make_unique<BTBRAS>(16, 2, 8);  // 16 entries, 2-bit counter, 8 inflight entries
    }

    void TearDown() override {
        ras.reset();
    }

    // Helper function to create a call BTBEntry
    BTBEntry createCallEntry(Addr pc, Addr target, unsigned size = 4) {
        BTBEntry entry;
        entry.valid = true;
        entry.pc = pc;
        entry.isCall = true;
        entry.isReturn = false;
        entry.size = size;
        entry.target = target;
        return entry;
    }

    // Helper function to create a return BTBEntry
    BTBEntry createReturnEntry(Addr pc, Addr target, unsigned size = 4) {
        BTBEntry entry;
        entry.valid = true;
        entry.pc = pc;
        entry.isCall = false;
        entry.isReturn = true;
        entry.size = size;
        entry.target = target;
        return entry;
    }

    // Helper function to create a call prediction
    FullBTBPrediction createCallPrediction(Addr bbStart, Addr callPC, Addr target, unsigned size = 4) {
        FullBTBPrediction pred;
        pred.bbStart = bbStart;
        pred.btbEntries.push_back(createCallEntry(callPC, target, size));
        return pred;
    }

    // Helper function to create a return prediction
    FullBTBPrediction createReturnPrediction(Addr bbStart, Addr retPC, Addr target, unsigned size = 4) {
        FullBTBPrediction pred;
        pred.bbStart = bbStart;
        pred.btbEntries.push_back(createReturnEntry(retPC, target, size));
        return pred;
    }

    // Helper function to perform basic RAS state check
    void checkReturnTarget(Addr startAddr, Addr expectedTarget) {
        boost::dynamic_bitset<> history(8, 0);
        std::vector<FullBTBPrediction> stagePreds(4);
        ras->putPCHistory(startAddr, history, stagePreds);
        EXPECT_EQ(stagePreds[0].returnTarget, expectedTarget);
    }

    // Helper function to create a commit stream for call instructions
    FetchStream createCallCommitStream(Addr startPC, Addr branchPC, unsigned size,
                                       std::shared_ptr<void> meta, bool taken = true) {
        FetchStream stream;
        stream.startPC = startPC;
        stream.exeTaken = taken;
        stream.exeBranchInfo.pc = branchPC;
        stream.exeBranchInfo.isCall = true;
        stream.exeBranchInfo.isReturn = false;
        stream.exeBranchInfo.size = size;
        stream.predMetas[0] = meta;
        return stream;
    }

    // Helper function to create a commit stream for return instructions
    FetchStream createReturnCommitStream(Addr startPC, Addr branchPC, unsigned size,
                                         std::shared_ptr<void> meta, bool taken = true) {
        FetchStream stream;
        stream.startPC = startPC;
        stream.exeTaken = taken;
        stream.exeBranchInfo.pc = branchPC;
        stream.exeBranchInfo.isCall = false;
        stream.exeBranchInfo.isReturn = true;
        stream.exeBranchInfo.size = size;
        stream.predMetas[0] = meta;
        return stream;
    }

    // Helper function to create a recovery stream
    FetchStream createRecoveryStream(Addr startPC, Addr branchPC, bool isCall, unsigned size,
                                     std::shared_ptr<void> meta, bool taken = false) {
        FetchStream stream;
        stream.startPC = startPC;
        stream.exeTaken = taken;
        stream.exeBranchInfo.pc = branchPC;
        stream.exeBranchInfo.isCall = isCall;
        stream.exeBranchInfo.isReturn = !isCall;
        stream.exeBranchInfo.size = size;
        stream.predMetas[0] = meta;
        return stream;
    }

    // Helper function to perform speculative call and return metadata
    std::shared_ptr<void> performSpecCall(Addr callPC, Addr target, unsigned size = 4) {
        boost::dynamic_bitset<> history(8, 0);
        auto meta = ras->getPredictionMeta();
        auto callPred = createCallPrediction(callPC, callPC, target, size);
        ras->specUpdateHist(history, callPred);
        return meta;
    }

    // Helper function to commit a call instruction
    void commitCall(Addr startPC, Addr branchPC, std::shared_ptr<void> meta, unsigned size = 4) {
        auto commitStream = createCallCommitStream(startPC, branchPC, size, meta);
        ras->update(commitStream);
    }

    std::unique_ptr<BTBRAS> ras;
};

// Test basic construction
TEST_F(RASTest, BasicConstruction) {
    ASSERT_NE(ras, nullptr);

    // Initial return target should be the default value
    checkReturnTarget(0x1000, 0x80000000L);
}

// Test basic push and pop operations through specUpdateHist
TEST_F(RASTest, BasicPushPop) {
    boost::dynamic_bitset<> history(8, 0);

    // Initial state
    checkReturnTarget(0x1000, 0x80000000L);

    // Create a call prediction to trigger push
    auto callPred = createCallPrediction(0x1000, 0x1000, 0x2000);

    // Execute speculative update (this should push return address)
    ras->specUpdateHist(history, callPred);

    // Check new state - should have return address (pc + size)
    checkReturnTarget(0x2000, 0x1004);

    // Create a return prediction to trigger pop
    auto retPred = createReturnPrediction(0x2000, 0x2000, 0x1004);

    // Execute return speculative update (this should pop)
    ras->specUpdateHist(history, retPred);

    // Check final state - should be back to initial state
    checkReturnTarget(0x1004, 0x80000000L);
}

// Test multiple pushes
TEST_F(RASTest, MultiplePushes) {
    boost::dynamic_bitset<> history(8, 0);

    // Push first call
    auto callPred1 = createCallPrediction(0x1000, 0x1000, 0x2000);
    ras->specUpdateHist(history, callPred1);

    // Push second call
    auto callPred2 = createCallPrediction(0x2000, 0x2000, 0x3000);
    ras->specUpdateHist(history, callPred2);

    // Check current state (should be second call's return address)
    checkReturnTarget(0x3000, 0x2004);
}

// Test same address counter behavior
TEST_F(RASTest, SameAddressCounter) {
    boost::dynamic_bitset<> history(8, 0);

    Addr retAddr = 0x1004;

    // Push same return address multiple times
    for (int i = 0; i < 3; i++) {
        auto callPred = createCallPrediction(0x1000, 0x1000, 0x2000);
        ras->specUpdateHist(history, callPred);
    }

    // Check that we still get the same return address
    checkReturnTarget(0x2000, retAddr);

    // Pop once - should still get the same address due to counter
    auto retPred = createReturnPrediction(0x2000, 0x2000, retAddr);
    ras->specUpdateHist(history, retPred);

    // Should still be the same address due to counter > 0
    checkReturnTarget(0x1004, retAddr);
}

// Test recovery functionality
TEST_F(RASTest, BasicRecovery) {
    boost::dynamic_bitset<> history(8, 0);

    // Get initial meta
    checkReturnTarget(0x1000, 0x80000000L);
    auto initialMeta = ras->getPredictionMeta();

    // Do some speculative operations
    auto callPred = createCallPrediction(0x1000, 0x1000, 0x2000);
    ras->specUpdateHist(history, callPred);

    // Create recovery stream
    FetchStream recoverStream;
    recoverStream.startPC = 0x1000;
    recoverStream.exeTaken = false;  // Not taken, so no actual call
    recoverStream.predMetas[0] = initialMeta;

    // Recover to initial state
    ras->recoverHist(history, recoverStream, 0, false);

    // Check that we're back to initial state
    checkReturnTarget(0x1000, 0x80000000L);
}

// Test speculative stack overflow - push more than numInflightEntries (8)
TEST_F(RASTest, SpeculativeStackOverflow) {
    boost::dynamic_bitset<> history(8, 0);

    // Initial state
    checkReturnTarget(0x1000, 0x80000000L);

    // Push 10 calls (more than the 8 inflight entries)
    // This should test the circular buffer behavior
    for (int i = 0; i < 10; i++) {
        Addr callPC = 0x1000 + i * 0x100;
        Addr target = 0x2000 + i * 0x100;
        auto callPred = createCallPrediction(callPC, callPC, target);
        ras->specUpdateHist(history, callPred);
    }

    // The top of stack should be the last pushed return address
    // Last call: PC=0x1000+9*0x100=0x1900, so return addr = 0x1904
    checkReturnTarget(0x2000 + 9 * 0x100, 0x1904);

    // Pop all entries and verify we can retrieve them
    // Due to circular buffer, we should get the most recent 8 entries
    for (int i = 9; i >= 2; i--) {  // Only the last 8 entries should be available
        Addr expectedRetAddr = 0x1000 + i * 0x100 + 4;  // PC + 4
        Addr currentPC = 0x2000 + i * 0x100;

        // Check current return target
        checkReturnTarget(currentPC, expectedRetAddr);

        // Create return prediction to pop
        auto retPred = createReturnPrediction(currentPC, currentPC, expectedRetAddr);
        ras->specUpdateHist(history, retPred);
    }

    // After popping 8 entries, we should be back to the committed stack
    // which should still have the default value
    checkReturnTarget(0x3000, 0x80000000L);
}

// Test speculative stack wraparound behavior
TEST_F(RASTest, SpeculativeStackWraparound) {
    boost::dynamic_bitset<> history(8, 0);

    // Fill the speculative stack exactly (8 entries)
    for (int i = 0; i < 8; i++) {
        Addr callPC = 0x1000 + i * 0x10;
        Addr target = 0x2000 + i * 0x10;
        auto callPred = createCallPrediction(callPC, callPC, target);
        ras->specUpdateHist(history, callPred);
    }

    // Verify the top entry
    checkReturnTarget(0x2070, 0x1074);  // Last entry: PC=0x1070, ret=0x1074

    // Now push one more - this should wrap around and overwrite the first entry
    auto overflowCall = createCallPrediction(0x1080, 0x1080, 0x2080);
    ras->specUpdateHist(history, overflowCall);

    // Top should now be the overflow entry
    checkReturnTarget(0x2080, 0x1084);

    // Pop once - should get the overflow entry
    auto retPred = createReturnPrediction(0x2080, 0x2080, 0x1084);
    ras->specUpdateHist(history, retPred);

    // Now we should have entries 7,6,5,4,3,2,1 available (entry 0 was overwritten)
    // So the top should be entry 7: 0x1070 -> 0x1074
    checkReturnTarget(0x2070, 0x1074);
}

// Test committed stack overflow through update() calls
TEST_F(RASTest, CommittedStackOverflow) {
    // Create a series of call operations that will be committed
    // Push 20 calls (more than the 16 committed stack entries)
    for (int i = 0; i < 20; i++) {
        Addr callPC = 0x1000 + i * 0x100;
        Addr target = 0x2000 + i * 0x100;

        // Get metadata before speculation, then perform spec call and commit
        checkReturnTarget(callPC, 0x80000000L);  // Should be default initially
        auto meta = performSpecCall(callPC, target);
        commitCall(callPC, callPC, meta);
    }

    // The committed stack should now contain the most recent 16 entries
    // due to circular buffer behavior
    // The last committed return address should be at top
    Addr expectedLastRetAddr = (0x1000 + 19 * 0x100) + 4;  // 0x1900 + 4 = 0x1904

    // Check the behavior after overflow (exact behavior depends on implementation)
    checkReturnTarget(0x3000, expectedLastRetAddr);
}

// Test empty stack pop behavior
TEST_F(RASTest, EmptyStackPop) {
    boost::dynamic_bitset<> history(8, 0);

    // Initial state - stack should be empty
    checkReturnTarget(0x1000, 0x80000000L);

    // Try to pop from empty stack
    auto retPred = createReturnPrediction(0x1000, 0x1000, 0x1004);
    ras->specUpdateHist(history, retPred);

    // Should still return default value when popping from empty stack
    checkReturnTarget(0x1004, 0x80000000L);

    // Pop again to test multiple pops on empty stack
    auto retPred2 = createReturnPrediction(0x1004, 0x1004, 0x1008);
    ras->specUpdateHist(history, retPred2);

    // Should gracefully handle multiple pops without crashing
    checkReturnTarget(0x1008, 0x80000000L);
}

// Test counter overflow behavior - push same address beyond maxCtr
TEST_F(RASTest, CounterOverflow) {
    boost::dynamic_bitset<> history(8, 0);

    // With ctrWidth=2, maxCtr should be 3 (2^2 - 1)
    Addr callPC = 0x1000;
    Addr target = 0x2000;
    Addr retAddr = 0x1004;

    // Push the same return address 5 times (more than maxCtr=3)
    for (int i = 0; i < 5; i++) {
        auto callPred = createCallPrediction(callPC, callPC, target);
        ras->specUpdateHist(history, callPred);

        // Should always get the same return address
        checkReturnTarget(target, retAddr);
    }

    // Now pop 4 times - should still get the same address for first 3 pops
    // (due to counter), then move to next entry on 4th pop
    for (int i = 0; i < 3; i++) {
        auto retPred = createReturnPrediction(target, target, retAddr);
        ras->specUpdateHist(history, retPred);

        // Should still be the same address due to counter
        checkReturnTarget(0x3000, retAddr);
    }

    // One more pop should move to next entry (default in this case)
    auto retPred = createReturnPrediction(target, target, retAddr);
    ras->specUpdateHist(history, retPred);
    checkReturnTarget(0x3000, 0x80000000L);
}

// Test complex recovery scenarios
TEST_F(RASTest, ComplexRecovery) {
    boost::dynamic_bitset<> history(8, 0);

    // Setup: Push 3 calls
    auto call1 = createCallPrediction(0x1000, 0x1000, 0x2000);
    auto call2 = createCallPrediction(0x2000, 0x2000, 0x3000);
    auto call3 = createCallPrediction(0x3000, 0x3000, 0x4000);

    ras->specUpdateHist(history, call1);

    // Save state after first call
    checkReturnTarget(0x2000, 0x1004);
    auto meta1 = ras->getPredictionMeta();

    ras->specUpdateHist(history, call2);
    ras->specUpdateHist(history, call3);

    // Now we should have 3 entries on the stack
    checkReturnTarget(0x4000, 0x3004);

    // Recover to the state after first call (simulate misprediction)
    FetchStream recoverStream;
    recoverStream.startPC = 0x2000;
    recoverStream.exeTaken = true;  // The first call was actually taken
    recoverStream.exeBranchInfo.pc = 0x1000;
    recoverStream.exeBranchInfo.isCall = true;
    recoverStream.exeBranchInfo.size = 4;
    recoverStream.predMetas[0] = meta1;

    ras->recoverHist(history, recoverStream, 0, true);

    // Should be back to state with just one call
    checkReturnTarget(0x2000, 0x1004);

    // Now do a different speculation path
    auto call2_alt = createCallPrediction(0x2000, 0x2000, 0x5000);
    ras->specUpdateHist(history, call2_alt);

    // Should have the new call's return address on top
    checkReturnTarget(0x5000, 0x2004);
}

// Test commit flow - speculative operations committed via update()
TEST_F(RASTest, CommitFlow) {
    // Phase 1: Speculative operations
    checkReturnTarget(0x1000, 0x80000000L);
    auto initialMeta = performSpecCall(0x1000, 0x2000);

    checkReturnTarget(0x2000, 0x1004);
    auto meta1 = performSpecCall(0x2000, 0x3000);

    checkReturnTarget(0x3000, 0x2004);

    // Phase 2: Commit the operations
    commitCall(0x1000, 0x1000, initialMeta);
    commitCall(0x2000, 0x2000, meta1);

    // Phase 3: Verify committed state
    checkReturnTarget(0x3000, 0x2004);

    // Test recovery - should now use committed stack
    boost::dynamic_bitset<> history(8, 0);
    auto recoverStream = createCallCommitStream(0x1000, 0x1000, 4, initialMeta, false);
    ras->recoverHist(history, recoverStream, 0, false);

    // After recovery, committed stack should be available
    checkReturnTarget(0x2000, 0x1004);
}

// Test mixed speculative and commit operations
TEST_F(RASTest, MixedOperations) {
    boost::dynamic_bitset<> history(8, 0);

    // Step 1: Spec call, then commit it
    checkReturnTarget(0x1000, 0x80000000L);
    auto meta0 = performSpecCall(0x1000, 0x2000);

    checkReturnTarget(0x2000, 0x1004);
    commitCall(0x1000, 0x1000, meta0);

    // Step 2: More speculative operations
    performSpecCall(0x2000, 0x3000);
    checkReturnTarget(0x3000, 0x2004);

    performSpecCall(0x3000, 0x4000);
    checkReturnTarget(0x4000, 0x3004);

    // Step 3: Simulate misprediction and recovery
    auto recoverStream = createCallCommitStream(0x1000, 0x1000, 4, meta0, true);
    ras->recoverHist(history, recoverStream, 0, true);

    // Should have committed call1, but lose speculative call2 and call3
    checkReturnTarget(0x2000, 0x1004);

    // Step 4: New speculative path after recovery
    performSpecCall(0x2000, 0x5000);
    checkReturnTarget(0x5000, 0x2004);

    // Step 5: Pop operations
    auto ret1 = createReturnPrediction(0x5000, 0x5000, 0x2004);
    ras->specUpdateHist(history, ret1);
    checkReturnTarget(0x2004, 0x1004);

    auto ret2 = createReturnPrediction(0x2004, 0x2004, 0x1004);
    ras->specUpdateHist(history, ret2);
    checkReturnTarget(0x1004, 0x80000000L);
}

// Test pointer wraparound in circular buffers
TEST_F(RASTest, PointerWraparound) {
    boost::dynamic_bitset<> history(8, 0);

    // Test committed stack wraparound (16 entries)
    // Commit 18 calls to force wraparound
    for (int i = 0; i < 18; i++) {
        Addr callPC = 0x1000 + i * 0x10;
        Addr target = 0x2000 + i * 0x10;

        auto meta = performSpecCall(callPC, target);
        commitCall(callPC, callPC, meta);
    }

    // The last committed call should be at top
    Addr lastCallPC = 0x1000 + 17 * 0x10;  // 0x1110
    Addr lastTarget = 0x2000 + 17 * 0x10;   // 0x2110
    checkReturnTarget(lastTarget, lastCallPC + 4);  // 0x1114

    // Test inflight stack wraparound (8 entries)
    // Do 10 more speculative calls to force inflight wraparound
    for (int i = 0; i < 10; i++) {
        Addr callPC = 0x3000 + i * 0x10;
        Addr target = 0x4000 + i * 0x10;
        performSpecCall(callPC, target);
    }

    // The last speculative call should be at top
    Addr lastSpecCallPC = 0x3000 + 9 * 0x10;  // 0x3090
    Addr lastSpecTarget = 0x4000 + 9 * 0x10;   // 0x4090
    checkReturnTarget(lastSpecTarget, lastSpecCallPC + 4);  // 0x3094

    // Pop all speculative entries - should eventually reach committed stack
    for (int i = 0; i < 10; i++) {
        auto retPred = createReturnPrediction(0x5000, 0x5000, 0x5004);
        ras->specUpdateHist(history, retPred);
    }

    // Should be back to committed stack top
    checkReturnTarget(0x5000, lastCallPC + 4);  // 0x1114
}

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
