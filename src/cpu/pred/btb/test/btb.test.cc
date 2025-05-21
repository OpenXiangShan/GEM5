#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "cpu/pred/btb/test/mockbtb.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{

// Helper functions for BTB tests

/**
 * @brief Create a Branch Info object with specified parameters
 *
 * @param pc Branch instruction address
 * @param target Branch target address
 * @param isCond Whether branch is conditional
 * @param isIndirect Whether branch is indirect
 * @param isCall Whether branch is a call instruction
 * @param isReturn Whether branch is a return instruction
 * @param size Instruction size
 * @return BranchInfo Initialized branch information
 */
BranchInfo createBranchInfo(Addr pc, Addr target, bool isCond = false,
                           bool isIndirect = false, bool isCall = false,
                           bool isReturn = false, uint8_t size = 4) {
    BranchInfo info;
    info.pc = pc;
    info.target = target;
    info.isCond = isCond;
    info.isIndirect = isIndirect;
    info.isCall = isCall;
    info.isReturn = isReturn;
    info.size = size;
    return info;
}

/**
 * @brief Setup a FetchStream with common parameters for BTB update
 *
 * @param startPC Start address of the fetch block
 * @param branch Branch information
 * @param taken Whether the branch was taken
 * @param meta Prediction metadata from previous prediction
 * @param endInstPC Last executed instruction PC, used for filtering entries
 * @return FetchStream Initialized fetch stream
 */
FetchStream setupStream(Addr startPC, const BranchInfo& branch, bool taken,
                       std::shared_ptr<void> meta, Addr endInstPC) {
    FetchStream stream;
    stream.startPC = startPC;
    stream.resolved = true;
    stream.exeBranchInfo = branch;
    stream.exeTaken = taken;
    stream.predMetas[0] = meta;
    stream.updateEndInstPC = endInstPC;
    return stream;
}

/**
 * @brief Execute a complete BTB prediction-update cycle
 *
 * @param btb The BTB to test
 * @param startPC Start address for prediction
 * @param branch Branch information for update
 * @param taken Whether the branch was taken
 * @param history Branch history register value
 * @param endInstPC End instruction PC for update
 * @return std::vector<FullBTBPrediction> Final stage predictions
 */
std::vector<FullBTBPrediction>
predictUpdateCycle(DefaultBTB* btb,
     Addr startPC,
     const BranchInfo& branch,
     bool taken,
     const boost::dynamic_bitset<>& history = boost::dynamic_bitset<>(8, 0),
     Addr endInstPC = 0) {
    // If endInstPC not specified, use branch.pc + branch.size
    if (endInstPC == 0) {
        endInstPC = branch.pc + branch.size;
    }

    // Prediction phase
    std::vector<FullBTBPrediction> stagePreds(4);
    btb->putPCHistory(startPC, history, stagePreds);
    auto meta = btb->getPredictionMeta();

    // Update phase
    FetchStream stream = setupStream(startPC, branch, taken, meta, endInstPC);

    // Only L1 BTB needs this step
    if (btb->getDelay() > 0) {
        btb->getAndSetNewBTBEntry(stream);
    }

    btb->update(stream);

    // Return final predictions after update
    stagePreds.clear();
    stagePreds.resize(4);
    btb->putPCHistory(startPC, history, stagePreds);

    return stagePreds;
}

/**
 * @brief Verify BTB prediction results
 *
 * @param stagePreds Stage predictions from BTB
 * @param delay BTB delay (0 for L0, >0 for L1)
 * @param expectedEntries Expected branch entries
 */
void verifyPrediction(const std::vector<FullBTBPrediction>& stagePreds,
                     unsigned delay,
                     const std::vector<BranchInfo>& expectedEntries) {
    // Check predictions for stages after delay
    for (int i = delay; i < stagePreds.size(); i++) {
        ASSERT_EQ(stagePreds[i].btbEntries.size(), expectedEntries.size());
        for (size_t j = 0; j < expectedEntries.size(); j++) {
            EXPECT_EQ(stagePreds[i].btbEntries[j].pc, expectedEntries[j].pc);
            EXPECT_EQ(stagePreds[i].btbEntries[j].target, expectedEntries[j].target);
        }
    }
}

// Test fixture for BTB tests
class BTBTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a BTB with 16 entries, 8-bit tags, and 4-way set associative
        mbtb_small = new DefaultBTB(16, 8, 4, 1, true); // mbtb (L1 BTB)
        mbtb = new DefaultBTB (16384, 20, 8, 1, true);
    }
    
    
    DefaultBTB* mbtb_small;
    DefaultBTB* mbtb;
};

// Test basic initialization
TEST_F(BTBTest, Initialization) {
    // Create a new BTB with different parameters
    DefaultBTB testBtb(32, 12, 8, 0);
    // Basic initialization test passes if no crashes/assertions
    SUCCEED();
}

// Test basic prediction with empty BTB
TEST_F(BTBTest, EmptyPrediction) {
    Addr startAddr = 0x1000;
    boost::dynamic_bitset<> history(8, 0);  // 8-bit history, all zeros
    std::vector<FullBTBPrediction> stagePreds(4);  // 4 stages
    
    mbtb->putPCHistory(startAddr, history, stagePreds);
    
    // Check predictions for all stages
    for (int i = 0; i < stagePreds.size(); i++) {
        EXPECT_TRUE(stagePreds[i].btbEntries.empty());
        EXPECT_FALSE(stagePreds[i].isTaken());
    }
}

// BTB actual update process:
// 1. putPCHistory, store result in stagePreds, update meta
// 2. getPredictionMeta, set to stream.predMetas[0]
// 3. getAndSetNewBTBEntry, only L1 BTB has this function
// 4. update, update btb entries

// Test basic prediction after update
TEST_F(BTBTest, PredictionAfterUpdate) {
    // Create branch info
    BranchInfo branch = createBranchInfo(0x1000, 0x2000, true);

    // Execute prediction-update cycle
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch, true);

    // Verify predictions
    verifyPrediction(stagePreds, mbtb->getDelay(), {branch});
}

// Test large virtual addr
TEST_F(BTBTest, PredictionAfterUpdateLargeAddr) {
    // Create branch info
    BranchInfo branch = createBranchInfo(0xffffffff8027ac64, 0xffffffff80261dee, true);

    // Execute prediction-update cycle
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0xffffffff8027ac60, branch, true);

    // Verify predictions
    verifyPrediction(stagePreds, mbtb->getDelay(), {branch});
}

// Test conditional branch prediction counter, for mBTB
TEST_F(BTBTest, ConditionalCounter) {
    // Create conditional branch info
    BranchInfo branch = createBranchInfo(0x1000, 0x2000, true);

    // First update with taken
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch, true);

    // Counter should be initialized to 1 and stay at 1 after taken
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, 1);
        EXPECT_TRUE(entries[0].alwaysTaken);
    }
    
    // Then update with not taken
    stagePreds = predictUpdateCycle(mbtb, 0x1000, branch, false);

    // Counter should be reduced after not taken (1 -> 0)
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, 0);
        EXPECT_FALSE(entries[0].alwaysTaken);
    }
}

// Test counter saturation behavior, for mBTB
TEST_F(BTBTest, CounterSaturation) {
    // Create conditional branch info
    BranchInfo branch = createBranchInfo(0x1000, 0x2000, true);

    // First entry is initialized with ctr=1
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch, true);

    // Check counter is at 1
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, 1);  // Counter should be at 1
        EXPECT_TRUE(entries[0].alwaysTaken);
    }
    
    // Update multiple times with not taken to test negative saturation
    for (int i = 0; i < 3; i++) {  // 3 times should reach saturation
        stagePreds = predictUpdateCycle(mbtb, 0x1000, branch, false);
    }
    
    // Check counter is saturated at -2
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, -2);  // Counter should saturate at -2
        EXPECT_FALSE(entries[0].alwaysTaken);
    }
}

// Test MRU replacement policy
TEST_F(BTBTest, ReplacementPolicy) {
    // Fill up a BTB set completely with branches in same set but different ways
    for (int i = 0; i < 4; i++) {
        BranchInfo branch = createBranchInfo(0x1000 + i * 0x1000, 0x2000 + i * 0x1000, true);
        predictUpdateCycle(mbtb_small, branch.pc, branch, true);
    }
    
    // Add one more branch to force replacement
    BranchInfo newBranch = createBranchInfo(0x5000, 0x6000, true);
    predictUpdateCycle(mbtb_small, newBranch.pc, newBranch, true);

    // The oldest entry (0x1000) should be replaced
    // Check by trying to find it
    std::vector<FullBTBPrediction> stagePreds(4);
    boost::dynamic_bitset<> history(8, 0);
    mbtb_small->putPCHistory(0x1000, history, stagePreds);

    // 0x1000 should be evicted, so no entry should be found
    EXPECT_TRUE(stagePreds[mbtb_small->getDelay()].btbEntries.empty());
}

// Test indirect branch prediction
TEST_F(BTBTest, IndirectBranchPrediction) {
    // Create indirect branch info
    BranchInfo branch = createBranchInfo(0x1000, 0x2000, false, true);

    // Initial prediction and update
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch, true);

    // Verify indirect target
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        EXPECT_EQ(stagePreds[i].indirectTargets[0x1000], 0x2000);
    }
    
    // Update with new target
    BranchInfo updatedBranch = createBranchInfo(0x1000, 0x3000, false, true);
    stagePreds = predictUpdateCycle(mbtb, 0x1000, updatedBranch, true);

    // Verify new indirect target
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        EXPECT_EQ(stagePreds[i].indirectTargets[0x1000], 0x3000);
    }
}

// Test multiple branch predictions in same fetch block
TEST_F(BTBTest, MultipleBranchPrediction) {
    // Create two branches in the same fetch block
    BranchInfo branch1 = createBranchInfo(0x1000, 0x2000, true);
    BranchInfo branch2 = createBranchInfo(0x1004, 0x3000, true);

    // Add first branch
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch1, true, boost::dynamic_bitset<>(8, 0), 0x1008);

    // Add second branch
    boost::dynamic_bitset<> history(8, 0);
    std::vector<FullBTBPrediction> tempPreds(4);
    mbtb->putPCHistory(0x1000, history, tempPreds);
    auto meta = mbtb->getPredictionMeta();

    FetchStream stream = setupStream(0x1000, branch2, true, meta, 0x1008);
    mbtb->getAndSetNewBTBEntry(stream);
    mbtb->update(stream);
    
    // Check final predictions
    stagePreds.clear();
    stagePreds.resize(4);
    mbtb->putPCHistory(0x1000, history, stagePreds);
    
    // Verify both branches are predicted
    std::vector<BranchInfo> expectedBranches = {branch1, branch2};
    verifyPrediction(stagePreds, mbtb->getDelay(), expectedBranches);
}

// Test recovery from misprediction
TEST_F(BTBTest, MispredictionRecovery) {
    // Create conditional branch initially taken
    BranchInfo branch = createBranchInfo(0x1000, 0x2000, true);

    // Initial prediction and update as taken
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch, true);

    // Update the same branch as not taken
    branch.target = 0x1004;  // Fall through target
    stagePreds = predictUpdateCycle(mbtb, 0x1000, branch, false);

    // Verify prediction is updated
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_FALSE(entries[0].alwaysTaken);
    }
}

// Test half-aligned mode basic functionality
TEST_F(BTBTest, HalfAlignedBasicTest) {

    // Create branches in two consecutive 32B blocks
    BranchInfo branch1 = createBranchInfo(0x100, 0x200, true);
    BranchInfo branch2 = createBranchInfo(0x120, 0x300, true);

    // Add first branch
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x100, branch1, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Add second branch
    stagePreds = predictUpdateCycle(mbtb, 0x100, branch2, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Verify both branches are predicted
    std::vector<BranchInfo> expectedBranches = {branch1, branch2};
    verifyPrediction(stagePreds, mbtb->getDelay(), expectedBranches);
}

// Test half-aligned mode with unaligned addresses
TEST_F(BTBTest, HalfAlignedUnalignedTest) {

    // Create unaligned branches in two consecutive 32B blocks
    BranchInfo branch1 = createBranchInfo(0x104, 0x200, true);
    BranchInfo branch2 = createBranchInfo(0x124, 0x300, true);

    // Add first branch
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x104, branch1, true, boost::dynamic_bitset<>(64, 0), 0x144);

    // Add second branch
    stagePreds = predictUpdateCycle(mbtb, 0x104, branch2, true, boost::dynamic_bitset<>(64, 0), 0x144);

    // Verify both branches are predicted
    std::vector<BranchInfo> expectedBranches = {branch1, branch2};
    verifyPrediction(stagePreds, mbtb->getDelay(), expectedBranches);
}

// Test half-aligned mode update with branch in second block
TEST_F(BTBTest, HalfAlignedUpdateSecondBlock) {

    // Create branch in second 32B block
    BranchInfo branch = createBranchInfo(0x124, 0x200, true);

    // Execute prediction-update cycle
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x100, branch, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Verify branch is predicted from first block
    verifyPrediction(stagePreds, mbtb->getDelay(), {branch});

    // Also verify prediction from second block
    stagePreds.clear();
    stagePreds.resize(2);
    mbtb->putPCHistory(0x120, boost::dynamic_bitset<>(64, 0), stagePreds);

    // Should still find the branch
    std::vector<BranchInfo> expectedBranches = {branch};
    verifyPrediction(stagePreds, mbtb->getDelay(), expectedBranches);
}

// Test half-aligned mode with branches in both blocks
TEST_F(BTBTest, HalfAlignedBothBlocks) {

    // Create branches in both 32B blocks
    BranchInfo branch1 = createBranchInfo(0x108, 0x200, true);
    BranchInfo branch2 = createBranchInfo(0x128, 0x300, true);

    // Add first branch
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x100, branch1, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Add second branch
    stagePreds = predictUpdateCycle(mbtb, 0x100, branch2, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Verify both branches are predicted
    std::vector<BranchInfo> expectedBranches = {branch1, branch2};
    verifyPrediction(stagePreds, mbtb->getDelay(), expectedBranches);
}

// Test half-aligned mode with unaligned start address
TEST_F(BTBTest, HalfAlignedUnalignedStart) {

    // Create branch in second block
    BranchInfo branch = createBranchInfo(0x12C, 0x200, true);

    // Execute prediction-update cycle from unaligned start address
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x10A, branch, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Verify branch is predicted
    verifyPrediction(stagePreds, mbtb->getDelay(), {branch});
}

// Test half-aligned mode with multiple updates to same branch
TEST_F(BTBTest, HalfAlignedMultipleUpdates) {

    // Create indirect branch in second block with initial target
    BranchInfo branch = createBranchInfo(0x124, 0x200, false, true);

    // Execute first prediction-update cycle
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x100, branch, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Update with new target
    branch.target = 0x300;
    stagePreds = predictUpdateCycle(mbtb, 0x100, branch, true, boost::dynamic_bitset<>(64, 0), 0x140);

    // Verify branch is predicted with new target
    verifyPrediction(stagePreds, mbtb->getDelay(), {branch});
}


} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
