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
createPreparedUpdate(const FetchTarget &stream, unsigned predictWidth,
                     const BranchInfo &branch, bool taken,
                     bool mispredicted = false)
{
    std::vector<BranchOutcome> events = {
        createResolveEvent(stream.tid, 0, 1, branch, taken, mispredicted)
    };
    return PreparedUpdate(
        PredictionUpdateContext(stream), predictWidth, events);
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
        stream.predBTBEntries = stagePreds[btb->getDelay()].btbEntries;
    }
    auto update = createPreparedUpdate(
        stream, btb->predictWidth, branch, taken);
    btb->prepareUpdate(PredictionUpdateContext(stream), update);
    update.applyOutcome(createResolveEvent(
        tid, 0, 1, branch, taken, false));

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

    // Counter should be initialized to 0 and stay at 0 after taken (since alwaysTaken=true)
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, 0);
        EXPECT_TRUE(entries[0].alwaysTaken);
    }
    
    // Then update with not taken
    stagePreds = predictUpdateCycle(mbtb, 0x1000, branch, false);

    // Counter should be reduced after not taken (0 -> -1)
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, -1);
        EXPECT_FALSE(entries[0].alwaysTaken);
    }
}

// Test counter saturation behavior, for mBTB
TEST_F(BTBTest, CounterSaturation) {
    // Create conditional branch info
    BranchInfo branch = createBranchInfo(0x1000, 0x2000, true);

    // First entry is initialized with ctr=0
    std::vector<FullBTBPrediction> stagePreds =
        predictUpdateCycle(mbtb, 0x1000, branch, true);

    // Check counter is at 0 (alwaysTaken=true, so updateCtr not called)
    for (int i = mbtb->getDelay(); i < stagePreds.size(); i++) {
        ASSERT_FALSE(stagePreds[i].btbEntries.empty());
        auto &entries = stagePreds[i].btbEntries;
        EXPECT_EQ(entries[0].ctr, 0);  // Counter should be at 0
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
    auto update = createPreparedUpdate(
        stream, mbtb->predictWidth, branch2, true);
    mbtb->prepareUpdate(PredictionUpdateContext(stream), update);
    update.applyOutcome(createResolveEvent(
        0, 0, 1, branch2, true, false));
    mbtb->update(PredictionUpdateContext(stream), update);

    // Check final predictions
    stagePreds.clear();
    stagePreds.resize(4);
    mbtb->putPCHistory(0x1000, history, stagePreds);
    
    // Verify both branches are predicted
    std::vector<BranchInfo> expectedBranches = {branch1, branch2};
    verifyPrediction(stagePreds, mbtb->getDelay(), expectedBranches);
}

TEST(PreparedUpdateTest, FiltersAndMarksResolvedBranches)
{
    FetchTarget target;
    target.startPC = 0x1000;

    auto branch_a = BTBEntry(createBranchInfo(0x1000, 0x1010, true));
    auto branch_b = BTBEntry(createBranchInfo(0x1004, 0x2000, true));
    auto branch_c = BTBEntry(createBranchInfo(0x1008, 0x3000, true));
    branch_a.valid = true;
    branch_b.valid = true;
    branch_c.valid = true;
    target.predBTBEntries = {branch_a, branch_b, branch_c};

    std::vector<BranchOutcome> events = {
        createResolveEvent(0, 0, 1, branch_a, false, false),
        createResolveEvent(0, 0, 2, branch_b, true, true)
    };
    PreparedUpdate update(PredictionUpdateContext(target), 64, events);
    ASSERT_EQ(update.branches.size(), 2);
    EXPECT_EQ(update.branches[0].entry.pc, branch_a.pc);
    EXPECT_EQ(update.branches[1].entry.pc, branch_b.pc);
    EXPECT_FALSE(update.branches[0].actualTaken);
    EXPECT_TRUE(update.branches[1].actualTaken);
    EXPECT_TRUE(update.branches[1].controlMispred);
    EXPECT_EQ(update.branches[1].actualTarget, branch_b.target);

    EXPECT_TRUE(update.branches[0].resolvedThisAttempt);
    EXPECT_TRUE(update.branches[1].resolvedThisAttempt);

    update.setBTBEntryCandidate(branch_b, false);
    update.applyOutcome(events[1]);
    EXPECT_TRUE(update.branches[1].resolvedThisAttempt);
    ASSERT_EQ(update.branches.size(), 3);
    EXPECT_TRUE(update.branches[2].resolvedThisAttempt);
    EXPECT_TRUE(update.branches[1].fromPrediction);
    EXPECT_TRUE(update.branches[1].matchesMbtbMissCandidate);
    EXPECT_FALSE(update.branches[2].fromPrediction);
    EXPECT_TRUE(update.branches[2].matchesMbtbMissCandidate);

    // Packet-local mutations must not leak back into the immutable prediction
    // snapshot. A retry reconstructs eligibility from the same outcome list.
    PreparedUpdate retry(PredictionUpdateContext(target), 64, events);
    ASSERT_EQ(retry.branches.size(), 2);
    EXPECT_TRUE(retry.branches[0].resolvedThisAttempt);
    EXPECT_TRUE(retry.branches[1].resolvedThisAttempt);
}

TEST(PreparedUpdateTest, NotTakenBranchUsesActualBranchFacts)
{
    FetchTarget target;
    target.startPC = 0x1000;

    auto predictedIndirect =
        BTBEntry(createBranchInfo(0x1004, 0xdead, false, true));
    predictedIndirect.valid = true;
    target.predBTBEntries = {predictedIndirect};
    const auto actualBranch =
        createBranchInfo(0x1004, 0x1008, true, false);

    auto update = createPreparedUpdate(
        target, 64, actualBranch, false, true);
    ASSERT_EQ(update.branches.size(), 1);
    EXPECT_FALSE(update.branches[0].actualTaken);
    EXPECT_EQ(update.branches[0].entry.target, 0x1008);
    EXPECT_TRUE(update.branches[0].entry.isCond);
    EXPECT_FALSE(update.branches[0].entry.isIndirect);
    EXPECT_EQ(update.branches[0].actualTarget, 0x1008);
    EXPECT_EQ(target.predBTBEntries[0].target, 0xdead);
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
    target.predBTBEntries = {branch_a, unresolved, branch_b, branch_c};

    auto resolved_a = createBranchInfo(0x1000, 0x5000, true);
    auto resolved_b = createBranchInfo(0x1004, 0x4000, true);
    std::vector<BranchOutcome> events = {
        createResolveEvent(1, 7, 11, resolved_b, true, true),
        createResolveEvent(1, 7, 10, resolved_a, false, false)
    };

    PreparedUpdate update(PredictionUpdateContext(target), 64, events);

    EXPECT_TRUE(update.outcome.fromOutcomeEvent);
    EXPECT_TRUE(update.outcome.taken);
    EXPECT_TRUE(update.outcome.controlMispred);
    EXPECT_EQ(update.outcome.branch.pc, resolved_b.pc);
    EXPECT_EQ(update.endInstPC, resolved_b.pc);
    ASSERT_EQ(update.branches.size(), 2);
    EXPECT_TRUE(update.branches[0].resolvedThisAttempt);
    EXPECT_FALSE(update.branches[0].actualTaken);
    EXPECT_TRUE(update.branches[0].entry.isCond);
    EXPECT_FALSE(update.branches[0].entry.isIndirect);
    EXPECT_EQ(update.branches[0].entry.target, resolved_a.target);
    EXPECT_TRUE(update.branches[1].resolvedThisAttempt);
    EXPECT_TRUE(update.branches[1].actualTaken);
    EXPECT_EQ(update.branches[1].actualTarget, 0x4000);

}

TEST(PreparedUpdateTest, EmptyOutcomeBlockIgnoresPredictionSnapshot)
{
    FetchTarget target;
    target.startPC = 0x1000;
    target.predTaken = true;
    target.predBranchInfo = createBranchInfo(0x1004, 0x2000, true);
    target.predBTBEntries = {BTBEntry(target.predBranchInfo)};

    const std::vector<BranchOutcome> no_branches;
    PreparedUpdate update(
        PredictionUpdateContext(target), 64, no_branches, 0x101c);

    EXPECT_FALSE(update.outcome.valid);
    EXPECT_FALSE(update.outcome.taken);
    EXPECT_EQ(update.endInstPC, 0x101c);
    EXPECT_TRUE(update.branches.empty());
}

TEST_F(BTBTest, ResolveEventCreatesUnpredictedTakenCandidate)
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

    PreparedUpdate update(
        PredictionUpdateContext(target), mbtb->predictWidth, events);
    mbtb->prepareUpdate(PredictionUpdateContext(target), update);
    update.applyOutcome(event);

    ASSERT_TRUE(update.btbEntryCandidate);
    EXPECT_EQ(update.btbEntryCandidate->pc, actual_branch.pc);
    ASSERT_EQ(update.branches.size(), 1);
    EXPECT_TRUE(update.branches[0].actualTaken);
    EXPECT_TRUE(update.branches[0].controlMispred);
    EXPECT_TRUE(update.branches[0].resolvedThisAttempt);
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
        insert_b, mbtb->predictWidth, branch_b, true, true);
    mbtb->prepareUpdate(PredictionUpdateContext(insert_b), insert_update);
    insert_update.applyOutcome(createResolveEvent(
        0, 0, 1, branch_b, true, true));
    mbtb->update(PredictionUpdateContext(insert_b), insert_update);

    predictions.assign(4, FullBTBPrediction());
    mbtb->putPCHistory(0x1000, history, predictions);
    ASSERT_EQ(predictions[mbtb->getDelay()].btbEntries.size(), 2);
    meta = mbtb->getPredictionMeta();

    auto resolve_a = setupStream(0x1000, branch_b, true, meta);
    resolve_a.predBTBEntries = predictions[mbtb->getDelay()].btbEntries;
    std::vector<BranchOutcome> resolve_events = {
        createResolveEvent(0, 0, 1, branch_a, false, false)
    };
    PreparedUpdate resolve_update(
        PredictionUpdateContext(resolve_a), mbtb->predictWidth,
        resolve_events);
    mbtb->prepareUpdate(PredictionUpdateContext(resolve_a), resolve_update);
    resolve_update.applyOutcome(resolve_events[0]);
    mbtb->setTrainingStage(PredictorTrainingStage::Resolve);
    mbtb->update(PredictionUpdateContext(resolve_a), resolve_update);

    predictions.assign(4, FullBTBPrediction());
    mbtb->putPCHistory(0x1000, history, predictions);
    const auto &entries = predictions[mbtb->getDelay()].btbEntries;
    ASSERT_EQ(entries.size(), 2);
    EXPECT_FALSE(entries[0].alwaysTaken);
    EXPECT_TRUE(entries[1].alwaysTaken);
}

TEST_F(BTBTest, PreparedUpdateDistinguishesMissingAndNewCandidate)
{
    constexpr Addr start_pc = 0x1000;
    auto branch = createBranchInfo(0x1004, 0x2000, true);
    boost::dynamic_bitset<> history(8, 0);
    std::vector<FullBTBPrediction> stage_preds(4);
    mbtb->putPCHistory(start_pc, history, stage_preds);
    auto meta = mbtb->getPredictionMeta();

    auto not_taken = setupStream(start_pc, branch, false, meta);
    auto no_candidate = createPreparedUpdate(
        not_taken, mbtb->predictWidth, branch, false, true);
    mbtb->prepareUpdate(PredictionUpdateContext(not_taken), no_candidate);
    EXPECT_FALSE(no_candidate.btbEntryCandidate);
    EXPECT_TRUE(no_candidate.branches.empty());

    auto taken = setupStream(start_pc, branch, true, meta);
    auto new_candidate = createPreparedUpdate(
        taken, mbtb->predictWidth, branch, true, true);
    mbtb->prepareUpdate(PredictionUpdateContext(taken), new_candidate);
    new_candidate.applyOutcome(createResolveEvent(
        0, 0, 1, branch, true, true));
    ASSERT_TRUE(new_candidate.btbEntryCandidate);
    EXPECT_TRUE(new_candidate.btbEntryCandidate->valid);
    EXPECT_EQ(new_candidate.btbEntryCandidate->pc, branch.pc);
    ASSERT_EQ(new_candidate.branches.size(), 1);
    EXPECT_FALSE(new_candidate.branches.front().fromPrediction);
    EXPECT_TRUE(
        new_candidate.branches.front().matchesMbtbMissCandidate);
    EXPECT_TRUE(new_candidate.branches.front().actualTaken);

    auto zero_pc_branch = createBranchInfo(0, 0x2000, true);
    auto zero_pc_taken = setupStream(0, zero_pc_branch, true, meta);
    auto zero_pc_candidate = createPreparedUpdate(
        zero_pc_taken, mbtb->predictWidth, zero_pc_branch, true, true);
    mbtb->prepareUpdate(
        PredictionUpdateContext(zero_pc_taken), zero_pc_candidate);
    EXPECT_TRUE(zero_pc_candidate.btbEntryCandidate);
    zero_pc_candidate.applyOutcome(createResolveEvent(
        0, 0, 1, zero_pc_branch, true, true));
    ASSERT_EQ(zero_pc_candidate.branches.size(), 1);
    EXPECT_TRUE(zero_pc_candidate.branches.front().resolvedThisAttempt);
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


} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
