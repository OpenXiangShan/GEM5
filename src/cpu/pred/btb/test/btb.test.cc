#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/mbtb.hh"

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

BranchOutcome
createResolveEvent(ThreadID tid, FetchTargetId ftqId, InstSeqNum seqNum,
                   const BranchInfo &branch, bool taken, bool mispredicted)
{
    return BranchOutcome{
        tid,
        ftqId,
        seqNum,
        branch.pc,
        branch.target,
        taken,
        mispredicted,
        branch.isCond,
        branch.isIndirect,
        branch.isDirect,
        branch.isCall,
        branch.isReturn,
        branch.size
    };
}

/**
 * @brief Setup a FetchTarget with common parameters for BTB update
 *
 * @param startPC Start address of the fetch block
 * @param branch Branch information
 * @param taken Whether the branch was taken
 * @param meta Prediction metadata from previous prediction
 * @return FetchTarget Initialized fetch stream
 */
FetchTarget setupStream(Addr startPC, const BranchInfo& branch, bool taken,
                       std::shared_ptr<void> meta, ThreadID tid = 0) {
    FetchTarget stream;
    stream.tid = tid;
    stream.startPC = startPC;
    stream.predBranchInfo = branch;
    stream.predTaken = taken;
    stream.predMetas[0] = meta;
    return stream;
}

PreparedUpdate
createPreparedUpdate(const FetchTarget &stream, const BranchInfo &branch,
                     bool taken,
                     bool mispredicted = false)
{
    std::vector<BranchOutcome> events = {
        createResolveEvent(stream.tid, 0, 1, branch, taken, mispredicted)
    };
    return PreparedUpdate(events);
}

/**
 * @brief Helper function to find conditional taken prediction for a given PC
 *
 * @param condTakens Vector of conditional predictions
 * @param pc Branch PC to search for
 * @return Pair of (found, prediction) where found indicates if PC was found
 */
std::pair<bool, bool> findCondTaken(const CondTakens& condTakens, Addr pc) {
    auto it = CondTakens_find(condTakens, pc);
    if (it != condTakens.end()) {
        return {true, it->second};
    }
    return {false, false};
}

/**
 * @brief Helper function to find indirect target for a given PC
 *
 * @param indirectTargets Vector of indirect targets
 * @param pc Branch PC to search for
 * @return Pair of (found, target) where found indicates if PC was found
 */
std::pair<bool, Addr> findIndirectTarget(const IndirectTargets& indirectTargets, Addr pc) {
    auto it = IndirectTakens_find(indirectTargets, pc);
    if (it != indirectTargets.end()) {
        return {true, it->second};
    }
    return {false, 0};
}

/**
 * @brief Execute a complete BTB prediction-update cycle
 *
 * @param btb The BTB to test
 * @param startPC Start address for prediction
 * @param branch Branch information for update
 * @param taken Whether the branch was taken
 * @param history Branch history register value
 * @return std::vector<FullBTBPrediction> Final stage predictions
 */
std::vector<FullBTBPrediction>
predictUpdateCycle(MBTB* btb,
     Addr startPC,
     const BranchInfo& branch,
     bool taken,
     const boost::dynamic_bitset<>& history = boost::dynamic_bitset<>(8, 0),
     ThreadID tid = 0) {
    // Prediction phase
    std::vector<FullBTBPrediction> stagePreds(4);
    for (auto &pred : stagePreds) {
        pred.tid = tid;
    }
    btb->putPCHistory(startPC, history, stagePreds);
    auto meta = btb->getPredictionMeta(tid);

    // Update phase
    FetchTarget stream = setupStream(startPC, branch, taken, meta, tid);
    // Populate predicted BTB entries in stream from stage predictions
    // Use entries from the first valid stage (delay)
    if (btb->getDelay() < stagePreds.size()) {
        stream.setPredictedBranches(
            stagePreds[btb->getDelay()].btbEntries);
    }
    auto update = createPreparedUpdate(stream, branch, taken);

    btb->update(PredictionUpdateContext(stream), update);

    // Return final predictions after update
    stagePreds.clear();
    stagePreds.resize(4);
    for (auto &pred : stagePreds) {
        pred.tid = tid;
    }
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
        mbtb_small = new MBTB(16, 8, 4, 1); // mbtb (L1 BTB)
        mbtb = new MBTB (2048, 20, 4, 1);  // 2 sram, 4 way each, total 8 ways
    }
    
    
    MBTB* mbtb_small;
    MBTB* mbtb;
};

// Test basic initialization
TEST_F(BTBTest, Initialization) {
    // Create a new BTB with different parameters
    MBTB testBtb(32, 12, 8, 0);
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

// Interleaved SMT predictions must retain each thread's metadata until the
// corresponding FetchTarget is created.
TEST_F(BTBTest, PredictionMetadataIsPerThread) {
    boost::dynamic_bitset<> history(8, 0);
    std::vector<FullBTBPrediction> thread0Preds(4);
    std::vector<FullBTBPrediction> thread1Preds(4);

    for (auto &pred : thread0Preds) {
        pred.tid = 0;
    }
    for (auto &pred : thread1Preds) {
        pred.tid = 1;
    }

    mbtb->putPCHistory(0x1000, history, thread0Preds);
    auto thread0Meta = mbtb->getPredictionMeta(0);
    ASSERT_NE(thread0Meta, nullptr);

    mbtb->putPCHistory(0x2000, history, thread1Preds);
    auto thread1Meta = mbtb->getPredictionMeta(1);
    ASSERT_NE(thread1Meta, nullptr);

    EXPECT_NE(thread0Meta, thread1Meta);
    EXPECT_EQ(mbtb->getPredictionMeta(0), thread0Meta);
    EXPECT_EQ(mbtb->getPredictionMeta(1), thread1Meta);
}

TEST_F(BTBTest, TidPartitionKeepsSamePcEntriesIndependent) {
    MBTB partitionedBtb(16, 8, 4, 1);
    partitionedBtb.setSmtTidPartitioned(true);

    constexpr Addr startPC = 0x1000;
    constexpr Addr branchPC = 0x1004;
    auto thread0Branch = createBranchInfo(branchPC, 0x2000, true);
    auto thread1Branch = createBranchInfo(branchPC, 0x3000, true);

    predictUpdateCycle(
        &partitionedBtb, startPC, thread0Branch, true,
        boost::dynamic_bitset<>(8, 0), 0);
    predictUpdateCycle(
        &partitionedBtb, startPC, thread1Branch, true,
        boost::dynamic_bitset<>(8, 0), 1);

    for (ThreadID tid = 0; tid < 2; ++tid) {
        std::vector<FullBTBPrediction> predictions(4);
        for (auto &pred : predictions) {
            pred.tid = tid;
        }
        partitionedBtb.putPCHistory(
            startPC, boost::dynamic_bitset<>(8, 0), predictions);

        ASSERT_EQ(predictions[partitionedBtb.getDelay()].btbEntries.size(), 1);
        EXPECT_EQ(predictions[partitionedBtb.getDelay()].btbEntries[0].target,
                  tid == 0 ? thread0Branch.target : thread1Branch.target);
    }
}

TEST_F(BTBTest, PrivateVictimCachesKeepSamePcTargetsIndependent)
{
    MBTB btb(16, 20, 4, 1, 32);
    btb.setSmtTidPartitioned(true);
    constexpr Addr startPC = 0x1000;

    for (ThreadID tid = 0; tid < 2; ++tid) {
        for (unsigned i = 0; i < 5; ++i) {
            auto branch = createBranchInfo(
                startPC + i * 4, 0x2000 + tid * 0x1000 + i * 16, true);
            predictUpdateCycle(
                &btb, startPC, branch, true,
                boost::dynamic_bitset<>(8, 0), tid);
        }
    }

    // One branch is resident only in the VC. Update all five to exercise it
    // without depending on which SRAM way was evicted. No update may consume
    // or overwrite the other thread's same-PC entry.
    const auto updatesBefore = btb.btbStats.updateInVC;
    for (unsigned i = 0; i < 5; ++i) {
        auto updated = createBranchInfo(startPC + i * 4, 0x4000 + i * 16, true);
        predictUpdateCycle(
            &btb, startPC, updated, true, boost::dynamic_bitset<>(8, 0), 0);
    }
    EXPECT_EQ(btb.btbStats.updateInVC, updatesBefore + 1);

    for (ThreadID tid = 0; tid < 2; ++tid) {
        const auto entries = btb.getPredictedEntriesNoSideEffect(
            startPC, tid, 0);
        std::vector<FullBTBPrediction> predictions(4);
        for (auto &prediction : predictions) {
            prediction.tid = tid;
        }
        btb.putPCHistory(startPC, boost::dynamic_bitset<>(8, 0), predictions);
        ASSERT_EQ(entries.size(), 5);
        const auto &predictedEntries = predictions[btb.getDelay()].btbEntries;
        ASSERT_EQ(predictedEntries.size(), entries.size());
        for (unsigned i = 0; i < entries.size(); ++i) {
            EXPECT_EQ(entries[i].pc, startPC + i * 4);
            const Addr expected = (tid == 0 ? 0x4000 : 0x3000) + i * 16;
            EXPECT_EQ(entries[i].target, expected);
            EXPECT_EQ(predictedEntries[i].target, expected);
        }
    }
}

TEST_F(BTBTest, PrivateVictimCacheReplacementDoesNotEvictOtherThread)
{
    MBTB btb(16, 20, 4, 1, 32);
    btb.setSmtTidPartitioned(true);
    constexpr unsigned residentEntries = 4 + 32;

    for (ThreadID tid = 0; tid < 2; ++tid) {
        // All entries use SRAM0 and the thread's single set. Thread 1 then
        // overflows its VC, exercising both replacement and same-PC cleanup.
        const unsigned count = tid == 0 ? residentEntries : 80;
        for (unsigned i = 0; i < count; ++i) {
            const Addr pc = 0x1000 + i * 64;
            auto branch = createBranchInfo(
                pc, 0x10000 + tid * 0x10000 + i * 64, true);
            predictUpdateCycle(
                &btb, pc, branch, true,
                boost::dynamic_bitset<>(8, 0), tid);
        }
    }

    // Each thread retains a full 32-entry VC in addition to its SRAM ways.
    for (ThreadID tid = 0; tid < 2; ++tid) {
        const unsigned first = tid == 0 ? 0 : 80 - residentEntries;
        for (unsigned i = first; i < first + residentEntries; ++i) {
            const Addr pc = 0x1000 + i * 64;
            const auto entries = btb.getPredictedEntriesNoSideEffect(
                pc, tid, 0);
            ASSERT_EQ(entries.size(), 1) << "tid=" << tid << " i=" << i;
            EXPECT_EQ(entries[0].pc, pc);
            EXPECT_EQ(entries[0].target,
                      0x10000 + tid * 0x10000 + i * 64);
        }
    }
}

TEST_F(BTBTest, DisabledVictimCacheSupportsTidPartitionedStorage)
{
    MBTB btb(16, 20, 4, 1, 0);
    btb.setSmtTidPartitioned(true);
    constexpr Addr startPC = 0x1000;

    for (ThreadID tid = 0; tid < 2; ++tid) {
        for (unsigned i = 0; i < 5; ++i) {
            auto branch = createBranchInfo(
                startPC + i * 4, 0x2000 + tid * 0x1000 + i * 16, true);
            predictUpdateCycle(
                &btb, startPC, branch, true,
                boost::dynamic_bitset<>(8, 0), tid);
        }
        EXPECT_EQ(btb.getPredictedEntriesNoSideEffect(startPC, tid, 0).size(),
                  4);
    }
    EXPECT_EQ(btb.btbStats.victimCacheHit, 0);
    EXPECT_EQ(btb.btbStats.updateInVC, 0);
}

// BTB actual update process:
// 1. putPCHistory, store result in stagePreds, update meta
// 2. getPredictionMeta, set to stream.predMetas[0]
// 3. prepareUpdate, only L1 BTB derives the old/new entry candidate
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

    // A newly allocated entry starts weakly taken, matching RTL.
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, 0);
    }
    
    // A second taken update makes the entry strongly taken.
    stagePreds = predictUpdateCycle(mbtb, 0x1000, branch, true);
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        EXPECT_EQ(stagePreds[i].btbEntries[0].ctr, 1);
    }

    // The first not-taken update preserves a taken prediction.
    stagePreds = predictUpdateCycle(mbtb, 0x1000, branch, false);
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        EXPECT_EQ(stagePreds[i].btbEntries[0].ctr, 0);
    }

    // A second not-taken update changes the prediction direction.
    stagePreds = predictUpdateCycle(mbtb, 0x1000, branch, false);
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        EXPECT_EQ(stagePreds[i].btbEntries[0].ctr, -1);
    }
}

TEST_F(BTBTest, ActualBranchAttributesReplaceExistingEntry)
{
    constexpr Addr start_pc = 0x1000;
    auto old_branch = createBranchInfo(
        0x1004, 0x2000, false, true, true, true, 4);
    predictUpdateCycle(mbtb, start_pc, old_branch, true);

    auto actual_branch = createBranchInfo(
        old_branch.pc, 0x3000, true, false, false, false, 2);
    actual_branch.isDirect = true;
    const auto predictions =
        predictUpdateCycle(mbtb, start_pc, actual_branch, false);
    const auto &entries = predictions[mbtb->getDelay()].btbEntries;
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].pc, actual_branch.pc);
    EXPECT_EQ(entries[0].target, actual_branch.target);
    EXPECT_EQ(entries[0].size, actual_branch.size);
    EXPECT_TRUE(entries[0].isCond);
    EXPECT_TRUE(entries[0].isDirect);
    EXPECT_FALSE(entries[0].isIndirect);
    EXPECT_FALSE(entries[0].isCall);
    EXPECT_FALSE(entries[0].isReturn);
    EXPECT_EQ(entries[0].ctr, -1);
}

TEST_F(BTBTest, ActualBranchUpdatePreservesLiveCounter)
{
    constexpr Addr start_pc = 0x1000;
    const auto branch = createBranchInfo(0x1004, 0x2000, true);
    predictUpdateCycle(mbtb, start_pc, branch, true);
    auto stream = setupStream(
        start_pc, branch, true, mbtb->getPredictionMeta());

    // Both updates retain the prediction made at ctr=0. The second update
    // must use the live ctr=1 produced by the first update.
    mbtb->update(PredictionUpdateContext(stream),
                 createPreparedUpdate(stream, branch, true));
    auto actual_branch = branch;
    actual_branch.target = 0x3000;
    actual_branch.size = 2;
    mbtb->update(PredictionUpdateContext(stream),
                 createPreparedUpdate(stream, actual_branch, false));

    std::vector<FullBTBPrediction> predictions(4);
    mbtb->putPCHistory(start_pc, boost::dynamic_bitset<>(8, 0), predictions);
    const auto &entries = predictions[mbtb->getDelay()].btbEntries;
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].ctr, 0);
    EXPECT_EQ(entries[0].target, actual_branch.target);
    EXPECT_EQ(entries[0].size, actual_branch.size);
}

// Test counter saturation behavior, for mBTB
TEST_F(BTBTest, CounterSaturation) {
    // Create conditional branch info
    BranchInfo branch = createBranchInfo(0x1000, 0x2000, true);

    // First entry is initialized with ctr=0
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch, true);

    // Check the weakly-taken allocation state.
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, 0);  // Counter should be at 0
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
    }
}

// Test MRU replacement policy, failed after add victim cache
// TEST_F(BTBTest, ReplacementPolicy) {
//     // Fill up a BTB set completely with branches in same set but different ways
//     for (int i = 0; i < 4; i++) {
//         BranchInfo branch = createBranchInfo(0x1000 + i * 0x1000, 0x2000 + i * 0x1000, true);
//         predictUpdateCycle(mbtb_small, branch.pc, branch, true);
//     }

//     // Add one more branch to force replacement
//     BranchInfo newBranch = createBranchInfo(0x5000, 0x6000, true);
//     predictUpdateCycle(mbtb_small, newBranch.pc, newBranch, true);

//     // The oldest entry (0x1000) should be replaced
//     // Check by trying to find it
//     std::vector<FullBTBPrediction> stagePreds(4);
//     boost::dynamic_bitset<> history(8, 0);
//     mbtb_small->putPCHistory(0x1000, history, stagePreds);

//     // 0x1000 should be evicted, so no entry should be found
//     EXPECT_TRUE(stagePreds[mbtb_small->getDelay()].btbEntries.empty());
// }

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
        auto [found1, target1] = findIndirectTarget(stagePreds[i].indirectTargets, 0x1000);
        ASSERT_TRUE(found1);
        EXPECT_EQ(target1, 0x2000);
    }
    
    // Update with new target
    BranchInfo updatedBranch = createBranchInfo(0x1000, 0x3000, false, true);
    stagePreds = predictUpdateCycle(mbtb, 0x1000, updatedBranch, true);

    // Verify new indirect target
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        auto [found2, target2] = findIndirectTarget(stagePreds[i].indirectTargets, 0x1000);
        ASSERT_TRUE(found2);
        EXPECT_EQ(target2, 0x3000);
    }
}

// Test multiple branch predictions in same fetch block
TEST_F(BTBTest, MultipleBranchPrediction) {
    // Create two branches in the same fetch block
    BranchInfo branch1 = createBranchInfo(0x1000, 0x2000, true);
    BranchInfo branch2 = createBranchInfo(0x1004, 0x3000, true);

    // Add first branch
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(
            mbtb, 0x1000, branch1, true,
            boost::dynamic_bitset<>(8, 0));

    // Add second branch
    boost::dynamic_bitset<> history(8, 0);
    std::vector<FullBTBPrediction> tempPreds(4);
    mbtb->putPCHistory(0x1000, history, tempPreds);
    auto meta = mbtb->getPredictionMeta();

    FetchTarget stream = setupStream(0x1000, branch2, true, meta);
    auto update = createPreparedUpdate(stream, branch2, true);
    mbtb->update(PredictionUpdateContext(stream), update);

    // Check final predictions
    stagePreds.clear();
    stagePreds.resize(4);
    mbtb->putPCHistory(0x1000, history, stagePreds);
    
    // Verify both branches are predicted
    std::vector<BranchInfo> expectedBranches = {branch1, branch2};
    verifyPrediction(stagePreds, mbtb->getDelay(), expectedBranches);
}

TEST(PreparedUpdateTest, SortsAndKeepsActualBranches)
{
    auto branch_a = BTBEntry(createBranchInfo(0x1000, 0x1010, true));
    auto branch_b = BTBEntry(createBranchInfo(0x1004, 0x2000, true));

    std::vector<BranchOutcome> events = {
        createResolveEvent(0, 0, 2, branch_b, true, true),
        createResolveEvent(0, 0, 1, branch_a, false, false)
    };
    PreparedUpdate update(events);
    ASSERT_EQ(update.branches.size(), 2);
    EXPECT_EQ(update.branches[0].pc, branch_a.pc);
    EXPECT_EQ(update.branches[1].pc, branch_b.pc);
    EXPECT_FALSE(update.branches[0].taken);
    EXPECT_TRUE(update.branches[1].taken);
    EXPECT_TRUE(update.branches[1].mispredicted);
    EXPECT_EQ(update.branches[1].target, branch_b.target);
}

TEST(PreparedUpdateTest, NotTakenBranchUsesActualBranchFacts)
{
    FetchTarget target;
    target.startPC = 0x1000;

    auto predictedIndirect =
        BTBEntry(createBranchInfo(0x1004, 0xdead, false, true));
    predictedIndirect.valid = true;
    predictedIndirect.ctr = 2;
    predictedIndirect.source = 3;
    target.setPredictedBranches({predictedIndirect});
    const auto actualBranch =
        createBranchInfo(0x1004, 0x1008, true, false);

    const auto update = createPreparedUpdate(target, actualBranch, false, true);
    ASSERT_EQ(update.branches.size(), 1);
    EXPECT_FALSE(update.branches[0].taken);
    EXPECT_EQ(update.branches[0].target, 0x1008);
    EXPECT_TRUE(update.branches[0].isCond);
    EXPECT_FALSE(update.branches[0].isIndirect);
}

TEST(PreparedUpdateTest, ResolveEventsOverrideStaleFtqOutcome)
{
    FetchTarget target;
    target.tid = 1;
    target.startPC = 0x1000;

    auto branch_a = BTBEntry(
        createBranchInfo(0x1000, 0x1010, false, true));
    auto unresolved = BTBEntry(createBranchInfo(0x1002, 0x1800, true));
    auto branch_b = BTBEntry(createBranchInfo(0x1004, 0x2000, true));
    auto branch_c = BTBEntry(createBranchInfo(0x1008, 0x3000, true));
    target.setPredictedBranches(
        {branch_a, unresolved, branch_b, branch_c});

    auto resolved_a = createBranchInfo(0x1000, 0x5000, true);
    auto resolved_b = createBranchInfo(0x1004, 0x4000, true);
    std::vector<BranchOutcome> events = {
        createResolveEvent(1, 7, 11, resolved_b, true, true),
        createResolveEvent(1, 7, 10, resolved_a, false, false)
    };

    PreparedUpdate update(events);

    EXPECT_TRUE(update.outcome.taken);
    EXPECT_TRUE(update.outcome.controlMispred);
    EXPECT_EQ(update.outcome.branch.pc, resolved_b.pc);
    ASSERT_EQ(update.branches.size(), 2);
    EXPECT_FALSE(update.branches[0].taken);
    EXPECT_TRUE(update.branches[0].isCond);
    EXPECT_FALSE(update.branches[0].isIndirect);
    EXPECT_EQ(update.branches[0].target, resolved_a.target);
    EXPECT_TRUE(update.branches[1].taken);
    EXPECT_EQ(update.branches[1].target, 0x4000);

}

TEST(PreparedUpdateTest, EmptyOutcomeBlockIgnoresPredictionSnapshot)
{
    FetchTarget target;
    target.startPC = 0x1000;
    target.predTaken = true;
    target.predBranchInfo = createBranchInfo(0x1004, 0x2000, true);
    target.setPredictedBranches({BTBEntry(target.predBranchInfo)});

    const std::vector<BranchOutcome> no_branches;
    PreparedUpdate update(no_branches);

    EXPECT_FALSE(update.outcome.valid);
    EXPECT_FALSE(update.outcome.taken);
    EXPECT_TRUE(update.branches.empty());
}

TEST(PreparedUpdateTest, NotTakenMispredictionPrecedesYoungerTaken)
{
    const auto older = createBranchInfo(0x1004, 0x2000, true);
    const auto younger = createBranchInfo(0x1008, 0x3000, true);
    const PreparedUpdate update({
        createResolveEvent(0, 7, 12, younger, true, false),
        createResolveEvent(0, 7, 11, older, false, true)
    });

    EXPECT_TRUE(update.outcome.valid);
    EXPECT_EQ(update.outcome.branch.pc, older.pc);
    EXPECT_FALSE(update.outcome.taken);
    EXPECT_TRUE(update.outcome.controlMispred);
}

TEST(PreparedUpdateTest, CorrectFallthroughUsesLastDynamicBranch)
{
    const auto older = createBranchInfo(0x1004, 0x2000, true);
    const auto younger = createBranchInfo(0x1008, 0x3000, true);
    const PreparedUpdate update({
        createResolveEvent(0, 7, 12, younger, false, false),
        createResolveEvent(0, 7, 11, older, false, false)
    });

    EXPECT_TRUE(update.outcome.valid);
    EXPECT_EQ(update.outcome.branch.pc, younger.pc);
    EXPECT_FALSE(update.outcome.taken);
    EXPECT_FALSE(update.outcome.controlMispred);
}

TEST(PreparedUpdateTest, KeepsUnpredictedActualBranches)
{
    FetchTarget target;
    target.startPC = 0x1000;

    auto branch = createBranchInfo(0x1004, 0x1008, true);
    auto update = createPreparedUpdate(target, branch, false, true);

    ASSERT_EQ(update.branches.size(), 1);
    EXPECT_EQ(update.branches.front().pc, branch.pc);
    EXPECT_FALSE(update.branches.front().taken);
}

TEST_F(BTBTest, UnpredictedTakenBranchAllocates)
{
    constexpr Addr start_pc = 0x1000;
    auto actual_branch = createBranchInfo(0x1004, 0x3000, true);
    boost::dynamic_bitset<> history(8, 0);
    std::vector<FullBTBPrediction> stage_preds(4);
    mbtb->putPCHistory(start_pc, history, stage_preds);

    FetchTarget target;
    target.tid = 0;
    target.startPC = start_pc;
    target.predMetas[0] = mbtb->getPredictionMeta();
    auto event = createResolveEvent(0, 9, 20, actual_branch, true, true);
    std::vector<BranchOutcome> events = {event};

    const PreparedUpdate update(events);
    mbtb->update(PredictionUpdateContext(target), update);

    stage_preds.assign(4, FullBTBPrediction());
    mbtb->putPCHistory(start_pc, history, stage_preds);
    ASSERT_EQ(stage_preds[mbtb->getDelay()].btbEntries.size(), 1);
    EXPECT_EQ(stage_preds[mbtb->getDelay()].btbEntries.front().pc,
              actual_branch.pc);
}

TEST_F(BTBTest, ResolvedUpdateOnlyAppliesMarkedBranch)
{
    const auto branch_a = createBranchInfo(0x1000, 0x2000, true);
    const auto branch_b = createBranchInfo(0x1004, 0x3000, true);
    predictUpdateCycle(mbtb, 0x1000, branch_a, true);

    boost::dynamic_bitset<> history(8, 0);
    std::vector<FullBTBPrediction> predictions(4);
    mbtb->putPCHistory(0x1000, history, predictions);
    auto meta = mbtb->getPredictionMeta();
    auto insert_b = setupStream(0x1000, branch_b, true, meta);
    auto insert_update = createPreparedUpdate(
        insert_b, branch_b, true, true);
    mbtb->update(PredictionUpdateContext(insert_b), insert_update);

    predictions.assign(4, FullBTBPrediction());
    mbtb->putPCHistory(0x1000, history, predictions);
    ASSERT_EQ(predictions[mbtb->getDelay()].btbEntries.size(), 2);
    meta = mbtb->getPredictionMeta();

    auto resolve_a = setupStream(0x1000, branch_b, true, meta);
    resolve_a.setPredictedBranches(
        predictions[mbtb->getDelay()].btbEntries);
    std::vector<BranchOutcome> resolve_events = {
        createResolveEvent(0, 0, 1, branch_a, false, false)
    };
    PreparedUpdate resolve_update(resolve_events);
    mbtb->setTrainingStage(PredictorTrainingStage::Resolve);
    mbtb->update(PredictionUpdateContext(resolve_a), resolve_update);

    predictions.assign(4, FullBTBPrediction());
    mbtb->putPCHistory(0x1000, history, predictions);
    const auto &entries = predictions[mbtb->getDelay()].btbEntries;
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].ctr, -1);
    EXPECT_EQ(entries[1].ctr, 0);
}

TEST_F(BTBTest, UnpredictedNotTakenBranchDoesNotAllocate)
{
    constexpr Addr start_pc = 0x1000;
    auto branch = createBranchInfo(0x1004, 0x2000, true);
    boost::dynamic_bitset<> history(8, 0);
    std::vector<FullBTBPrediction> stage_preds(4);
    mbtb->putPCHistory(start_pc, history, stage_preds);
    auto meta = mbtb->getPredictionMeta();

    auto target = setupStream(start_pc, branch, false, meta);
    const auto update = createPreparedUpdate(target, branch, false, true);
    mbtb->update(PredictionUpdateContext(target), update);

    stage_preds.assign(4, FullBTBPrediction());
    mbtb->putPCHistory(start_pc, history, stage_preds);
    EXPECT_TRUE(stage_preds[mbtb->getDelay()].btbEntries.empty());
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
        EXPECT_LT(entries[0].ctr, 0);
    }
}

// Test half-aligned mode basic functionality
TEST_F(BTBTest, HalfAlignedBasicTest) {

    // Create branches in two consecutive 32B blocks
    BranchInfo branch1 = createBranchInfo(0x100, 0x200, true);
    BranchInfo branch2 = createBranchInfo(0x120, 0x300, true);

    // Add first branch
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x100, branch1, true, 
            boost::dynamic_bitset<>(64, 0));

    // Add second branch
    stagePreds = predictUpdateCycle(mbtb, 0x100, branch2, true, 
        boost::dynamic_bitset<>(64, 0));

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
        predictUpdateCycle(
            mbtb, 0x104, branch1, true,
            boost::dynamic_bitset<>(64, 0));

    // Add second branch
    stagePreds = predictUpdateCycle(
        mbtb, 0x104, branch2, true,
        boost::dynamic_bitset<>(64, 0));

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
        predictUpdateCycle(
            mbtb, 0x100, branch, true,
            boost::dynamic_bitset<>(64, 0));

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
        predictUpdateCycle(
            mbtb, 0x100, branch1, true,
            boost::dynamic_bitset<>(64, 0));

    // Add second branch
    stagePreds = predictUpdateCycle(
        mbtb, 0x100, branch2, true,
        boost::dynamic_bitset<>(64, 0));

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
        predictUpdateCycle(
            mbtb, 0x10A, branch, true,
            boost::dynamic_bitset<>(64, 0));

    // Verify branch is predicted
    verifyPrediction(stagePreds, mbtb->getDelay(), {branch});
}

// Test half-aligned mode with multiple updates to same branch
TEST_F(BTBTest, HalfAlignedMultipleUpdates) {

    // Create indirect branch in second block with initial target
    BranchInfo branch = createBranchInfo(0x124, 0x200, false, true);

    // Execute first prediction-update cycle
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(
            mbtb, 0x100, branch, true,
            boost::dynamic_bitset<>(64, 0));

    // Update with new target
    branch.target = 0x300;
    stagePreds = predictUpdateCycle(
        mbtb, 0x100, branch, true,
        boost::dynamic_bitset<>(64, 0));

    // Verify branch is predicted with new target
    verifyPrediction(stagePreds, mbtb->getDelay(), {branch});
}

// Test victim cache effectiveness with SRAM overflow
TEST_F(BTBTest, VictimCacheEffectivenessTest) {
    // Create 5 branches in same 32B block that all map to SRAM0
    // Use addresses with bit[5]=0 to ensure they go to SRAM0
    std::vector<BranchInfo> branches;
    std::vector<Addr> branch_pcs = {0x100, 0x104, 0x108, 0x10C, 0x110};

    for (int i = 0; i < 5; i++) {
        Addr target = 0x200 + i * 0x10;
        branches.push_back(createBranchInfo(branch_pcs[i], target, true));
    }

    // Add all 5 branches one by one
    std::vector<FullBTBPrediction> stagePreds;
    for (int i = 0; i < 5; i++) {
        stagePreds = predictUpdateCycle(mbtb, 0x100, branches[i], true,
            boost::dynamic_bitset<>(64, 0));
    }

    // Check if victim cache had hits
    EXPECT_EQ(mbtb->btbStats.victimCacheHit, 1)
        << "Expected victim cache hits when accessing evicted branch";

    // With victim cache, all branch should still be predictable
    verifyPrediction(stagePreds, mbtb->getDelay(), {branches});
}

// Test victim cache promotion mechanism
TEST_F(BTBTest, VictimCachePromotionTest) {
    // Create 5 branches to overflow SRAM0 (4 ways)
    std::vector<BranchInfo> branches;
    for (int i = 0; i < 5; i++) {
        Addr pc = 0x100 + i * 4;  // All map to same SRAM0
        branches.push_back(createBranchInfo(pc, 0x200 + i * 0x10, true));
    }

    // Add all 5 branches - first 4 in main BTB, 5th evicts branch 0 to victim cache
    for (int i = 0; i < 5; i++) {
        predictUpdateCycle(mbtb, 0x100, branches[i], true);
    }

    // Access first branch again - should trigger promotion from victim cache
    auto stagePreds = predictUpdateCycle(mbtb, 0x100, branches[0], true);

    // Verify promotion occurred
    EXPECT_GT(mbtb->btbStats.updateReplaceValidOne, 0);

    // Verify branch is still predictable after promotion
    verifyPrediction(stagePreds, mbtb->getDelay(), {branches});
}

// Test victim cache FIFO replacement
TEST_F(BTBTest, VictimCacheFIFOTest) {
    // Create 10 branches to overflow both main BTB and victim cache
    std::vector<BranchInfo> branches;
    for (int i = 0; i < 10; i++) {
        Addr pc = 0x100 + i * 4;  // All map to same SRAM0
        branches.push_back(createBranchInfo(pc, 0x200 + i * 0x10, true));
    }

    // Add all 10 branches - first 4 in main BTB, next 8 should evict to victim cache
    // But victim cache only has 8 slots, so earliest evicted entries get overwritten
    for (int i = 0; i < 10; i++) {
        predictUpdateCycle(mbtb, 0x100, branches[i], true);
    }

    // Try to access very early branch (should be evicted from victim cache too)
    auto stagePreds1 = predictUpdateCycle(mbtb, 0x100, branches[0], true);

    // Try to access recent branch (should still be in victim cache)
    auto stagePreds2 = predictUpdateCycle(mbtb, 0x100, branches[6], true);

    // Recent branch should still be predictable via victim cache
    verifyPrediction(stagePreds2, mbtb->getDelay(), {branches});
}

// Test: update path when entry exists only in victim cache
TEST_F(BTBTest, UpdateFromVictimCachePath) {
    // Prepare 5 branches mapping to same SRAM0 set
    std::vector<BranchInfo> branches;
    for (int i = 0; i < 5; i++) {
        Addr pc = 0x200 + i * 4;
        branches.push_back(createBranchInfo(pc, 0x800 + i * 0x10, true));
    }

    // Insert first 4 branches into MBTB
    for (int i = 0; i < 4; i++) {
        predictUpdateCycle(mbtb, 0x200, branches[i], true,
            boost::dynamic_bitset<>(64, 0));
    }

    // Insert 5th branch to evict one into VC
    predictUpdateCycle(mbtb, 0x200, branches[4], true,
        boost::dynamic_bitset<>(64, 0));

    // At this point, one of the earlier branches should be in VC. Force update on that branch.
    // We choose branches[0] which is likely evicted first.
    auto before_replace = mbtb->btbStats.updateReplace;

    auto stagePreds = predictUpdateCycle(mbtb, 0x200, branches[0], false,
        boost::dynamic_bitset<>(64, 0));

    // Ensure replacement path executed (entry was inserted back from VC to MBTB during update)
    // EXPECT_GE(mbtb->btbStats.updateReplace, before_replace + 1);

    // And the branch should be predictable after update
    verifyPrediction(stagePreds, mbtb->getDelay(), {branches});
}


class MBTBBankConflictTest : public ::testing::Test
{
  protected:
    MBTB btb{64, 20, 4, 1, 8, true};

    void predict(Addr pc, ThreadID tid = 0)
    {
        std::vector<FullBTBPrediction> preds(4);
        for (auto &pred : preds) {
            pred.tid = tid;
        }
        btb.putPCHistory(pc, boost::dynamic_bitset<>(8), preds);
    }

    FetchTarget stream(Addr pc = 0x1000, ThreadID tid = 0)
    {
        FullBTBPrediction pred;
        pred.tid = tid;
        pred.asidHash = 0;
        btb.refreshPredictionMeta(pc, boost::dynamic_bitset<>(8), pred);
        return setupStream(pc, createBranchInfo(pc, pc + 0x200, true),
                           true, btb.getPredictionMeta(tid), tid);
    }

    PreparedUpdate packet(std::initializer_list<Addr> pcs, bool taken = true,
                          ThreadID tid = 0)
    {
        std::vector<BranchOutcome> events;
        for (const Addr pc : pcs) {
            events.push_back(createResolveEvent(
                tid, 0, events.size() + 1,
                createBranchInfo(pc, pc + 0x200, true), taken, false));
        }
        return PreparedUpdate(events);
    }

    bool canUpdate(const FetchTarget &target, const PreparedUpdate &update)
    {
        return btb.canResolveUpdate(PredictionUpdateContext(target), update);
    }
};

TEST_F(MBTBBankConflictTest, HalfBlocksCarryAndPeriodicMapping)
{
    // Explicit RTL bank order for successive 32B blocks, including PC[7]
    // carry into PC[8]. Offset within a block must not affect reservation.
    const unsigned bankIds[] = {0, 4, 1, 5, 2, 6, 3, 7};
    auto target = stream();
    unsigned expectedReads[8] = {};
    for (unsigned block = 0; block < 8; ++block) {
        SCOPED_TRACE(block);
        btb.tickStart();
        predict(0x1000 + block * 32 + 30);
        const unsigned first = bankIds[block];
        const unsigned second = bankIds[(block + 1) % 8];
        ++expectedReads[first];
        ++expectedReads[second];
        for (unsigned candidate = 0; candidate < 8; ++candidate) {
            // The +256 verifies periodic resource mapping despite a
            // different storage index/tag.
            const bool conflict = candidate == block ||
                                  candidate == (block + 1) % 8;
            EXPECT_EQ(canUpdate(target, packet({0x1100 + candidate * 32})),
                      !conflict);
            EXPECT_EQ(btb.btbStats.predAccessPerBank[candidate],
                      expectedReads[candidate]);
        }
    }
    EXPECT_EQ(btb.btbStats.updateBankConflict, 16);
}

TEST_F(MBTBBankConflictTest, ProbeDefersUntilIdleCycleWithoutTraining)
{
    auto target = stream();
    const auto update = packet({0x1000});
    predict(0x1000);
    for (unsigned retry = 0; retry < 8; ++retry) {
        EXPECT_FALSE(canUpdate(target, update));
        EXPECT_TRUE(btb.getPredictedEntriesNoSideEffect(0x1000, 0, 0).empty());
        EXPECT_EQ(btb.btbStats.updateTotal, 0);
        EXPECT_EQ(btb.btbStats.newEntry, 0);
        EXPECT_EQ(btb.btbStats.updateReplace, 0);
    }
    EXPECT_EQ(btb.btbStats.updateDeferredDueToConflict, 8);
    EXPECT_EQ(btb.btbStats.updateBankConflictPerBank[0], 8);
    EXPECT_EQ(btb.btbStats.predAccessPerBank[0], 1);
    // An idle prediction cycle still clears the previous reservation.
    btb.tickStart();
    EXPECT_TRUE(canUpdate(target, update));
    EXPECT_EQ(btb.btbStats.updateTotal, 0);
    btb.doResolveUpdate(PredictionUpdateContext(target), update);
    EXPECT_EQ(btb.btbStats.updateTotal, 1);
    EXPECT_EQ(btb.btbStats.newEntry, 1);
    ASSERT_EQ(btb.getPredictedEntriesNoSideEffect(0x1000, 0, 0).size(), 1);
    EXPECT_EQ(btb.btbStats.updateAccessPerBank[0], 9);
}

TEST_F(MBTBBankConflictTest, DifferentBankAndDisabledModelAllowUpdates)
{
    auto target = stream();
    predict(0x1000); // Banks 0 and 4.
    EXPECT_TRUE(canUpdate(target, packet({0x1040}))); // Bank 1.
    EXPECT_EQ(btb.btbStats.updateBankConflict, 0);

    MBTB disabled(64, 20, 4, 1); // Default switch is off.
    std::vector<FullBTBPrediction> preds(4);
    for (auto &pred : preds) {
        pred.tid = 0;
    }
    disabled.putPCHistory(0x1000, boost::dynamic_bitset<>(8), preds);
    auto disabledTarget = setupStream(0x1000, createBranchInfo(0x1000, 0x1200),
        true, disabled.getPredictionMeta());
    EXPECT_TRUE(disabled.canResolveUpdate(
        PredictionUpdateContext(disabledTarget), packet({0x1000})));
    EXPECT_EQ(disabled.btbStats.predAccessPerBank[0], 0);
    EXPECT_EQ(disabled.btbStats.updateAccessPerBank[0], 0);
}

TEST_F(MBTBBankConflictTest, EmptyAndUnpredictedNotTakenDoNotWriteSRAM)
{
    auto target = stream();
    predict(0x1000);
    EXPECT_TRUE(canUpdate(target, packet({})));
    const auto notTaken = packet({0x1000}, false);
    EXPECT_TRUE(canUpdate(target, notTaken));
    btb.doResolveUpdate(PredictionUpdateContext(target), notTaken);
    EXPECT_EQ(btb.btbStats.updateTotal, 0);
    EXPECT_EQ(btb.btbStats.updateAccessPerBank[0], 0);
    EXPECT_EQ(btb.btbStats.updateBankConflict, 0);
}

TEST_F(MBTBBankConflictTest, PredictedNotTakenStillWritesSRAM)
{
    auto initial = stream();
    btb.update(PredictionUpdateContext(initial), packet({0x1000}));
    auto trained = stream();
    predict(0x1000);
    EXPECT_FALSE(canUpdate(trained, packet({0x1000}, false)));
    EXPECT_EQ(btb.btbStats.updateTotal, 1);
}

TEST_F(MBTBBankConflictTest, PartialConflictDefersWholePacket)
{
    auto target = stream();
    const auto update = packet({0x1040, 0x1000, 0x1004, 0x1020});
    predict(0x1000);
    EXPECT_FALSE(canUpdate(target, update));
    EXPECT_EQ(btb.btbStats.updateTotal, 0);
    EXPECT_TRUE(btb.getPredictedEntriesNoSideEffect(0x1040, 0, 0).empty());
    EXPECT_EQ(btb.btbStats.updateBankConflict, 1);
    EXPECT_EQ(btb.btbStats.updateBankConflictPerBank[0], 1);
    EXPECT_EQ(btb.btbStats.updateBankConflictPerBank[4], 1);
    // Two branches in bank 0 count as one attempted bank access.
    EXPECT_EQ(btb.btbStats.updateAccessPerBank[0], 1);
    EXPECT_EQ(btb.btbStats.updateAccessPerBank[1], 1);
    EXPECT_EQ(btb.btbStats.updateAccessPerBank[4], 1);
    btb.tickStart();
    EXPECT_TRUE(canUpdate(target, update));
    btb.doResolveUpdate(PredictionUpdateContext(target), update);
    EXPECT_EQ(btb.btbStats.updateTotal, 4);
}

TEST_F(MBTBBankConflictTest, VictimCacheOnlyUpdatesDoNotConflict)
{
    auto initial = stream();
    btb.update(PredictionUpdateContext(initial),
               packet({0x1000, 0x1004, 0x1008, 0x100c, 0x1010}));
    auto target = stream();
    predict(0x1000);
    unsigned vcOnly = 0;
    for (unsigned offset = 0; offset <= 16; offset += 4) {
        const auto update = packet({0x1000 + offset}, false);
        if (canUpdate(target, update)) {
            ++vcOnly;
            btb.doResolveUpdate(PredictionUpdateContext(target), update);
        }
    }
    EXPECT_EQ(vcOnly, 1);
    EXPECT_EQ(btb.btbStats.updateInVC, 1);
    EXPECT_EQ(btb.btbStats.updateAccessPerBank[0], 4);
}

TEST_F(MBTBBankConflictTest, ThreadRequestsAccumulateSharedBanks)
{
    btb.setSmtTidPartitioned(true);
    auto target0 = stream(0x1000, 0);
    auto target1 = stream(0x1000, 1);
    btb.tickStart();
    predict(0x1000, 0); // Banks 0, 4.
    predict(0x1040, 1); // Banks 1, 5.
    EXPECT_FALSE(canUpdate(target0, packet({0x1040})));
    EXPECT_FALSE(canUpdate(target1, packet({0x1000}, true, 1)));
    EXPECT_TRUE(canUpdate(target0, packet({0x1080})));
    for (unsigned bank : {0, 4, 1, 5}) {
        EXPECT_EQ(btb.btbStats.predAccessPerBank[bank], 1);
    }
    btb.tickStart();
    EXPECT_TRUE(canUpdate(target0, packet({0x1040})));
    EXPECT_TRUE(canUpdate(target1, packet({0x1000}, true, 1)));
}

TEST_F(MBTBBankConflictTest, SideEffectFreeQueriesDoNotReserveBanks)
{
    auto target = stream();
    btb.tickStart();
    btb.getPredictedEntriesNoSideEffect(0x1000, 0, 0);
    FullBTBPrediction pred;
    pred.tid = 0;
    pred.asidHash = 0;
    btb.refreshPredictionMeta(0x1000, boost::dynamic_bitset<>(8), pred);
    EXPECT_TRUE(canUpdate(target, packet({0x1000})));
    EXPECT_EQ(btb.btbStats.predAccessPerBank[0], 0);
}

TEST_F(MBTBBankConflictTest, DirectCommitUpdateBypassesConflictGate)
{
    auto target = stream();
    const auto update = packet({0x1000});
    predict(0x1000);
    EXPECT_FALSE(canUpdate(target, update));
    btb.update(PredictionUpdateContext(target), update);
    EXPECT_EQ(btb.btbStats.updateTotal, 1);
}

class MBTBWriteBufferTest : public ::testing::Test
{
  protected:
    MBTB btb{1024, 20, 4, 1, 8, true, true, 4};

    void SetUp() override { btb.testTick = 1; }

    void predict(Addr pc, ThreadID tid = 0)
    {
        std::vector<FullBTBPrediction> preds(4);
        for (auto &pred : preds) {
            pred.tid = tid;
        }
        btb.putPCHistory(pc, boost::dynamic_bitset<>(8), preds);
    }

    void beginBusyCycle(Addr pc = 0x1000)
    {
        btb.tickStart();
        predict(pc);
    }

    void idleCycle()
    {
        btb.tickStart();
        btb.tick();
    }

    FetchTarget stream(Addr pc, ThreadID tid = 0)
    {
        FullBTBPrediction pred;
        pred.tid = tid;
        pred.asidHash = 0;
        btb.refreshPredictionMeta(pc, boost::dynamic_bitset<>(8), pred);
        return setupStream(pc, createBranchInfo(pc, 0x8000, true),
                           true, btb.getPredictionMeta(tid), tid);
    }

    void resolve(Addr pc, Addr target = 0x8000, bool taken = true,
                 ThreadID tid = 0, bool indirect = false)
    {
        auto context = stream(pc, tid);
        auto branch = createBranchInfo(pc, target, !indirect, indirect);
        const auto update = createPreparedUpdate(context, branch, taken);
        ASSERT_TRUE(btb.canResolveUpdate(PredictionUpdateContext(context), update));
        btb.doResolveUpdate(PredictionUpdateContext(context), update);
    }

    std::vector<BTBEntry> entries(Addr pc, ThreadID tid = 0)
    {
        return btb.getPredictedEntriesNoSideEffect(pc, tid, 0);
    }

    void seed(Addr pc = 0x1000)
    {
        auto context = stream(pc);
        btb.update(PredictionUpdateContext(context),
            createPreparedUpdate(context, createBranchInfo(pc, 0x8000, true),
                                 true));
    }
};

TEST_F(MBTBWriteBufferTest, ReadPriorityAcceptsTrainingWithoutForwarding)
{
    beginBusyCycle();
    resolve(0x1000);
    EXPECT_TRUE(entries(0x1000).empty());
    btb.tick(); // Entry request is in its input register.
    EXPECT_TRUE(entries(0x1000).empty());
    beginBusyCycle();
    btb.tick(); // Entry request reaches the write buffer.
    beginBusyCycle();
    btb.tick(); // Prediction still blocks SRAM writes.
    EXPECT_TRUE(entries(0x1000).empty());
    EXPECT_EQ(btb.btbStats.updateDeferredDueToConflict, 0);
    EXPECT_EQ(btb.btbStats.entryBufferDrains, 0);
    EXPECT_EQ(btb.btbStats.counterBufferDrains, 0);
    idleCycle();
    ASSERT_EQ(entries(0x1000).size(), 1);
    EXPECT_EQ(btb.btbStats.entryBufferDrains, 1);
    EXPECT_EQ(btb.btbStats.counterBufferDrains, 1);
}

TEST_F(MBTBWriteBufferTest, IndependentBankDrainsDuringOtherBankReads)
{
    beginBusyCycle(); // Banks 0 and 4, training writes bank 1.
    resolve(0x1040);
    btb.tick();
    EXPECT_EQ(btb.btbStats.counterBufferDrains, 1);
    EXPECT_EQ(btb.btbStats.counterBufferEnqueues, 0); // Empty FIFO flows.
    beginBusyCycle();
    btb.tick();
    beginBusyCycle();
    btb.tick();
    EXPECT_EQ(entries(0x1040).size(), 1);
    EXPECT_EQ(btb.btbStats.bufferReadBlockedPerBank[1], 0);
}

TEST_F(MBTBWriteBufferTest, DirtyHitMergesLatestEntry)
{
    beginBusyCycle();
    resolve(0x1000, 0x8000);
    btb.tick();
    beginBusyCycle();
    resolve(0x1000, 0x9000);
    btb.tick();
    beginBusyCycle();
    btb.tick();
    EXPECT_EQ(btb.btbStats.entryBufferEnqueues, 1);
    EXPECT_EQ(btb.btbStats.entryBufferMerges, 1);
    EXPECT_EQ(btb.btbStats.entryBufferOverwrites, 0);
    idleCycle();
    ASSERT_EQ(entries(0x1000).size(), 1);
    EXPECT_EQ(entries(0x1000)[0].target, 0x9000);
}

TEST_F(MBTBWriteBufferTest, SimultaneousDrainAndMergeRetainsNewWrite)
{
    beginBusyCycle();
    resolve(0x1000, 0x8000);
    btb.tick();
    beginBusyCycle();
    resolve(0x1000, 0x9000);
    btb.tick();
    idleCycle(); // Drain old payload while the newer request merges.
    ASSERT_EQ(entries(0x1000).size(), 1);
    EXPECT_EQ(entries(0x1000)[0].target, 0x8000);
    idleCycle();
    EXPECT_EQ(entries(0x1000)[0].target, 0x9000);
    EXPECT_EQ(btb.btbStats.entryBufferDrains, 2);
}

TEST_F(MBTBWriteBufferTest, FullRowOverwritesDirtyPLRUVictim)
{
    // All requests use bank 0 and the same way in five different sets.
    for (unsigned i = 0; i < 5; ++i) {
        beginBusyCycle();
        resolve(0x1000 + i * 256);
        btb.tick();
    }
    beginBusyCycle();
    btb.tick(); // Deliver the fifth entry through its input register.
    EXPECT_EQ(btb.btbStats.entryBufferEnqueues, 5);
    EXPECT_EQ(btb.btbStats.entryBufferOverwrites, 1);
    EXPECT_EQ(btb.btbStats.counterBufferDrops, 1);
    for (unsigned i = 0; i < 4; ++i) {
        idleCycle();
    }
    EXPECT_TRUE(entries(0x1000).empty());
    for (unsigned i = 1; i < 5; ++i) {
        EXPECT_EQ(entries(0x1000 + i * 256).size(), 1);
    }
    EXPECT_EQ(btb.btbStats.entryBufferDrains, 4);
}

TEST_F(MBTBWriteBufferTest, SameSetReplacementOverridesEvenWithCleanSlots)
{
    // Five allocations in one SRAM set rotate through four destination ways.
    // The fifth must overwrite the same-set shadow in the first way row.
    for (unsigned i = 0; i < 5; ++i) {
        beginBusyCycle();
        resolve(0x1000 + i * 4096);
        btb.tick();
    }
    beginBusyCycle();
    btb.tick();
    EXPECT_EQ(btb.btbStats.entryBufferOverwrites, 1);
    idleCycle(); // Every way row has its own drain port.
    EXPECT_EQ(btb.btbStats.entryBufferDrains, 4);
    EXPECT_TRUE(entries(0x1000).empty());
    EXPECT_EQ(entries(0x5000).size(), 1);
}

TEST_F(MBTBWriteBufferTest, CounterFIFOFullDropsNewPayload)
{
    seed();
    for (unsigned i = 0; i < 5; ++i) {
        beginBusyCycle();
        resolve(0x1000, 0x8000, i == 4);
        btb.tick();
        EXPECT_EQ(entries(0x1000)[0].ctr, 0);
    }
    EXPECT_EQ(btb.btbStats.entryBufferEnqueues, 0);
    EXPECT_EQ(btb.btbStats.counterBufferEnqueues, 4);
    EXPECT_EQ(btb.btbStats.counterBufferDrops, 1);
    for (unsigned i = 0; i < 4; ++i) {
        idleCycle();
    }
    // FIFO contains four -1 writes. The new +1 write was dropped. Counter
    // payloads use live SRAM state at training; the FIFO does not forward.
    EXPECT_EQ(entries(0x1000)[0].ctr, -1);
    EXPECT_EQ(btb.btbStats.counterBufferDrains, 4);
}

TEST_F(MBTBWriteBufferTest, FullCounterFIFOAcceptsOnSameCycleDequeue)
{
    seed();
    for (unsigned i = 0; i < 4; ++i) {
        beginBusyCycle();
        resolve(0x1000, 0x8000, false);
        btb.tick();
    }
    btb.tickStart();
    resolve(0x1000, 0x8000, true);
    btb.tick(); // Old head dequeues and the new +1 request enqueues.
    EXPECT_EQ(btb.btbStats.counterBufferDrops, 0);
    for (unsigned i = 0; i < 4; ++i) {
        idleCycle();
    }
    EXPECT_EQ(entries(0x1000)[0].ctr, 1);
    EXPECT_EQ(btb.btbStats.counterBufferDrains, 5);
}

TEST_F(MBTBWriteBufferTest, CleanShadowSuppressesIdenticalIndirectWrite)
{
    btb.tickStart();
    resolve(0x1000, 0x8000, true, 0, true);
    btb.tick();
    idleCycle();
    idleCycle();
    ASSERT_EQ(entries(0x1000).size(), 1);
    btb.tickStart();
    resolve(0x1000, 0x8000, true, 0, true);
    btb.tick();
    idleCycle();
    EXPECT_EQ(btb.btbStats.entryBufferRedundant, 1);
    EXPECT_EQ(btb.btbStats.entryBufferDrains, 1);
}

TEST_F(MBTBWriteBufferTest, CounterWritesInOneSetShareFIFOEntry)
{
    auto context = stream(0x1000);
    std::vector<BranchOutcome> events;
    for (unsigned i = 0; i < 5; ++i) {
        events.push_back(createResolveEvent(0, 0, i + 1,
            createBranchInfo(0x1000 + i * 4, 0x8000, true), true, false));
    }
    btb.update(PredictionUpdateContext(context), PreparedUpdate(events));
    context = stream(0x1000);
    for (auto &event : events) {
        event.taken = false;
    }
    beginBusyCycle();
    btb.doResolveUpdate(PredictionUpdateContext(context), PreparedUpdate(events));
    btb.tick();
    EXPECT_EQ(btb.btbStats.updateInVC, 1);
    EXPECT_EQ(btb.btbStats.entryBufferEnqueues, 0);
    EXPECT_EQ(btb.btbStats.counterBufferEnqueues, 1);
    idleCycle();
    EXPECT_EQ(btb.btbStats.counterBufferDrains, 1);
}

TEST_F(MBTBWriteBufferTest, BothThreadsBlockSharedBankDrains)
{
    btb.setSmtTidPartitioned(true);
    beginBusyCycle(0x1040); // Thread 0 reads banks 1 and 5.
    predict(0x1000, 1); // Thread 1 reads banks 0 and 4.
    resolve(0x1000, 0x8000, true, 1);
    btb.tick();
    beginBusyCycle();
    btb.tick();
    beginBusyCycle();
    btb.tick();
    EXPECT_TRUE(entries(0x1000, 1).empty());
    idleCycle();
    EXPECT_EQ(entries(0x1000, 1).size(), 1);
    EXPECT_TRUE(entries(0x1000, 0).empty());
}

TEST_F(MBTBWriteBufferTest, EmptyAndUnpredictedNotTakenDoNotEnterBuffers)
{
    beginBusyCycle();
    resolve(0x1000, 0x8000, false);
    auto context = stream(0x1000);
    btb.doResolveUpdate(PredictionUpdateContext(context), PreparedUpdate({}));
    btb.tick();
    idleCycle();
    idleCycle();
    EXPECT_EQ(btb.btbStats.entryBufferEnqueues, 0);
    EXPECT_EQ(btb.btbStats.counterBufferEnqueues, 0);
    EXPECT_EQ(btb.btbStats.counterBufferDrains, 0);
    EXPECT_EQ(btb.btbStats.updateBankConflict, 0);
}

TEST_F(MBTBWriteBufferTest, DisabledMasterSwitchKeepsDirectResolveUpdates)
{
    MBTB disabled(1024, 20, 4, 1, 8, false, true);
    std::vector<FullBTBPrediction> preds(4);
    for (auto &pred : preds) {
        pred.tid = 0;
    }
    disabled.putPCHistory(0x1000, boost::dynamic_bitset<>(8), preds);
    auto branch = createBranchInfo(0x1000, 0x8000, true);
    auto context = setupStream(0x1000, branch, true, disabled.getPredictionMeta());
    const auto update = createPreparedUpdate(context, branch, true);
    EXPECT_TRUE(disabled.canResolveUpdate(PredictionUpdateContext(context), update));
    disabled.doResolveUpdate(PredictionUpdateContext(context), update);
    ASSERT_EQ(disabled.getPredictedEntriesNoSideEffect(0x1000, 0, 0).size(), 1);
    disabled.tick();
    EXPECT_EQ(disabled.btbStats.entryBufferEnqueues, 0);
}

TEST_F(MBTBWriteBufferTest, CommitTrainingRemainsImmediate)
{
    beginBusyCycle();
    seed();
    EXPECT_EQ(entries(0x1000).size(), 1);
    btb.tick();
    EXPECT_EQ(btb.btbStats.entryBufferEnqueues, 0);
    EXPECT_EQ(btb.btbStats.counterBufferEnqueues, 0);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
