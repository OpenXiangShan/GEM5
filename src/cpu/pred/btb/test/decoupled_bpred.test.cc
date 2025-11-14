#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>

#include <boost/dynamic_bitset.hpp>

// #include "arch/generic/pcstate.hh"
#include "cpu/pred/btb/fetch_target_queue.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/test/decoupled_bpred.hh"
#include "cpu/pred/btb/test/mock_PCState.hh"
#include "cpu/pred/btb/test/test_dprintf.hh"

// #include "cpu/static_inst.hh"


namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace test
{

// // Forward declarations for mock classes
// class MockStaticInst;
// class MockPCState;

/**
 * @brief Helper function to create a branch information object
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
 * @brief Helper function to create a BTB entry
 */
BTBEntry createBTBEntry(Addr pc, Addr target, Addr tag = 0, bool isCond = false,
                       bool isIndirect = false, bool isCall = false,
                       bool isReturn = false, bool valid = true,
                       bool alwaysTaken = false, int ctr = 0,
                       uint8_t size = 4) {
    BTBEntry entry;
    entry.pc = pc;
    entry.target = target;
    entry.tag = tag;
    entry.isCond = isCond;
    entry.isIndirect = isIndirect;
    entry.isCall = isCall;
    entry.isReturn = isReturn;
    entry.valid = valid;
    entry.alwaysTaken = alwaysTaken;
    entry.ctr = ctr;
    entry.size = size;
    return entry;
}

/**
 * @brief Execute individual functions of the prediction pipeline
 *
 * This function helps test the pipeline stages individually
 * rather than calling tick() which runs the entire pipeline
 */
class PipelineStage
{
public:
    // Stage 0: Generate predictions by predictor components
    static void generatePredictions(DecoupledBPUWithBTB& dbpu) {
        // Reset sentPCHist to false first to ensure we can generate a new prediction
        dbpu.sentPCHist = false;

        // Request new prediction if FSQ not full and no prediction is already received
        if (!dbpu.receivedPred && !dbpu.streamQueueFull()) {
            for (int i = 0; i < dbpu.numStages; i++) {
                dbpu.predsOfEachStage[i].bbStart = dbpu.s0PC;
            }

            for (int i = 0; i < dbpu.numComponents; i++) {
                dbpu.components[i]->putPCHistory(dbpu.s0PC, dbpu.s0History, dbpu.predsOfEachStage);
            }

            dbpu.sentPCHist = true;
        }
    }

    // Stage 1: Generate final prediction
    static void generateFinalPrediction(DecoupledBPUWithBTB& dbpu) {
        if (dbpu.sentPCHist && dbpu.numOverrideBubbles == 0) {
            dbpu.generateFinalPredAndCreateBubbles();
            // Note: This sets receivedPred = true
        }
    }

    // Stage 2: Enqueue fetch stream
    static void enqueueFetchStream(DecoupledBPUWithBTB& dbpu) {
        if (!dbpu.squashing) {
            dbpu.tryEnqFetchStream();
            // Note: This resets receivedPred = false if a stream was enqueued
        }
    }

    // Stage 3: Enqueue fetch target
    static void enqueueFetchTarget(DecoupledBPUWithBTB& dbpu) {
        if (!dbpu.squashing) {
            dbpu.tryEnqFetchTarget();
        }
    }

    // Test helper: Consume one override bubble
    static void consumeOverrideBubble(DecoupledBPUWithBTB& dbpu) {
        if (dbpu.numOverrideBubbles > 0) {
            dbpu.numOverrideBubbles--;
        }
    }
};

/**
 * @brief Execute one full cycle of the pipelined predictor
 */
void executePredictionCycle(DecoupledBPUWithBTB& dbpu) {
    // Execute a single prediction cycle
    dbpu.tick();
    bool in_loop = false;
    dbpu.trySupplyFetchWithTarget(0x1000, in_loop);
}


/**
 * @brief Add a branch to BTB and update decoupled BPU
 */
void updateWithBranch(DecoupledBPUWithBTB& dbpu, Addr startPC,
                     const BranchInfo& branch, bool taken) {
    // Create PC states for branch PC and target PC
    PCStateBase branchPC(branch.pc);
    PCStateBase targetPC(branch.target);

    // Create appropriate static instruction
    // StaticInstPtr inst = nullptr;
    StaticInstPtr inst = std::make_shared<MockStaticInst>();
    if (branch.isCond) inst->setFlag(MockStaticInst::IsCondControl);
    if (branch.isIndirect) inst->setFlag(MockStaticInst::IsIndirectControl);
    if (branch.isCall) inst->setFlag(MockStaticInst::IsCall);
    if (branch.isReturn) inst->setFlag(MockStaticInst::IsReturn);
    inst->setFlag(MockStaticInst::IsControl);

    // Get current FSQ and FTQ IDs
    FetchStreamId fsqId = dbpu.fsqId - 1;
    unsigned targetId = dbpu.fetchTargetQueue.getSupplyingTargetId();

    // Control squash to update predictor
    dbpu.controlSquash(
        targetId,       // FTQ ID
        fsqId,          // FSQ ID
        branchPC,       // Control PC
        targetPC,       // Target PC
        inst,           // Static instruction
        branch.size,    // Instruction size
        taken,          // Actually taken
        1,              // Sequence number
        0,              // Thread ID
        0,              // Current loop iteration
        false           // From commit
    );

    // Run a few ticks to process the update
    for (int i = 0; i < 3; i++) {
        dbpu.tick();
    }
}

/**
 * Function to check if a branch is in the FSQ
 * Since fetchStreamQueue is private, we'll use the existing public interface
 */
bool checkBranchInFSQ(DecoupledBPUWithBTB* dbpu, Addr pc) {
    // We need to simulate a call to the predictor with the branch's starting PC
    // and check if it predicts the branch correctly

    // Execute a prediction cycle first
    executePredictionCycle(*dbpu);

    // Create a dummy instruction to query the predictor
    StaticInstPtr inst = std::make_shared<MockStaticInst>();
    InstSeqNum seq = 1;
    PCStateBase pcState(pc);

    // Use decoupledPredict to check if the branch is predicted
    auto [taken, run_out] = dbpu->decoupledPredict(inst, seq, pcState, 0);

    // For branches that are in the predictor, the prediction would return meaningful results
    // This is a simplistic check but works for basic testing
    return (pcState.instAddr() != pc + 4);
}

class DecoupledBPUTest : public ::testing::Test
{
protected:
    Addr pc = 0x1000;
    void SetUp() override {
        dbpu = new DecoupledBPUWithBTB();
        // Set initial PC
        dbpu->resetPC(pc);
    }

    void pc_advance() {
        pc += 4;
    }

    void TearDown() override {
        delete dbpu;
    }

    DecoupledBPUWithBTB* dbpu;
};

// Test basic initialization
TEST_F(DecoupledBPUTest, Initialization) {
    // Basic initialization test passes if no crashes/assertions
    SUCCEED();
}

// Test basic prediction cycle
TEST_F(DecoupledBPUTest, BasicPredictionCycle) {
    // Clock 0: Initial prediction request
    executePredictionCycle(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist) << "sentPCHist should be true after initial prediction";
    EXPECT_FALSE(dbpu->receivedPred) << "receivedPred should be false initially";
    EXPECT_FALSE(dbpu->squashing) << "squashing should be false initially";
    EXPECT_EQ(dbpu->numOverrideBubbles, 0) << "No override bubbles should exist initially";

    // Clock 1: Generate final prediction and enqueue FSQ
    executePredictionCycle(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist) << "sentPCHist should be set each clock cycle";
    EXPECT_FALSE(dbpu->receivedPred) << "receivedPred should be reset after FSQ enqueue";
    EXPECT_FALSE(dbpu->squashing) << "squashing should still be false";

    // Clock 2: Enqueue FTQ
    executePredictionCycle(*dbpu);

    // Verify that a prediction happened - FTQ should have entries
    EXPECT_TRUE(dbpu->fetchTargetAvailable()) << "FTQ should have entries after 3 cycles";
    EXPECT_GT(dbpu->fsqId, 1) << "FSQ ID should have been incremented";
}


/**
 * @brief Test prediction with override bubbles
 */
TEST_F(DecoupledBPUTest, PredictionWithOverrideBubbles) {
    // First, create a situation where later stages override earlier ones
    // by manually setting override bubbles
    dbpu->numOverrideBubbles = 2;

    // Execute cycles and observe the bubbles being consumed
    executePredictionCycle(*dbpu);
    EXPECT_EQ(dbpu->numOverrideBubbles, 1) << "One bubble should be consumed";

    executePredictionCycle(*dbpu);
    EXPECT_EQ(dbpu->numOverrideBubbles, 0) << "All bubbles should be consumed";

    // Now the prediction should proceed normally
    // Request prediction
    executePredictionCycle(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist);

    // Generate prediction
    executePredictionCycle(*dbpu);

    // Enqueue FTQ
    executePredictionCycle(*dbpu);

    // Verify prediction completed
    EXPECT_TRUE(dbpu->fetchTargetAvailable());
}

/**
 * @brief Test squashing mechanism
 */
TEST_F(DecoupledBPUTest, SquashingMechanism) {
    // Perform initial prediction
    for (int i = 0; i < 3; i++) {
        executePredictionCycle(*dbpu);
    }

    // Verify prediction completed
    EXPECT_TRUE(dbpu->fetchTargetAvailable());

    // Now manually trigger squashing
    dbpu->squashing = true;

    // Execute a cycle with squashing enabled
    executePredictionCycle(*dbpu);

    // Verify no prediction was made
    EXPECT_FALSE(dbpu->receivedPred) << "No finalprediction should be made during squashing";
    EXPECT_TRUE(dbpu->sentPCHist) << "Prediction is done during squashing";

    // Clear squashing and verify prediction resumes
    dbpu->squashing = false;
    executePredictionCycle(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist) << "Prediction should resume after squashing is cleared";
}

/**
 * @brief Test prediction content with a branch in BTB
 */
// TEST_F(DecoupledBPUTest, PredictionContent) {
//     // First, perform initial prediction to warm up
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Create a branch and add it to BTB
//     BranchInfo branch = createBranchInfo(0x1010, 0x2000, true);  // Conditional branch
//     updateWithBranch(*dbpu, 0x1000, branch, true);  // Update as taken

//     // Reset PC to trigger prediction from start
//     dbpu->resetPC(0x1000);

//     // Execute prediction cycles
//     // Clock 0: Request prediction
//     executePredictionCycle(*dbpu);
//     EXPECT_TRUE(dbpu->sentPCHist);

//     // Clock 1: Generate prediction and enqueue FSQ
//     executePredictionCycle(*dbpu);

//     // Clock 2: Enqueue FTQ
//     executePredictionCycle(*dbpu);

//     // Verify prediction is available
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());

//     // Verify prediction content
//     // Get the most recent FSQ entry
//     ASSERT_GT(dbpu->fetchStreamQueue.size(), 0);
//     auto fsqId = dbpu->fsqId - 1;
//     auto fsqEntryIt = dbpu->fetchStreamQueue.find(fsqId);
//     ASSERT_NE(fsqEntryIt, dbpu->fetchStreamQueue.end());

//     const auto& fsqEntry = fsqEntryIt->second;
//     EXPECT_EQ(fsqEntry.startPC, 0x1000) << "FSQ start PC should be 0x1000";
//     EXPECT_TRUE(fsqEntry.isHit) << "BTB hit should be true";
//     EXPECT_TRUE(fsqEntry.predTaken) << "Branch should be predicted taken";
//     EXPECT_EQ(fsqEntry.predBranchInfo.pc, 0x1010) << "Branch PC should be 0x1010";
//     EXPECT_EQ(fsqEntry.predBranchInfo.target, 0x2000) << "Branch target should be 0x2000";

//     // Get FTQ entry
//     const auto& ftqEntry = dbpu->getSupplyingFetchTarget();
//     EXPECT_EQ(ftqEntry.startPC, 0x1000) << "FTQ start PC should be 0x1000";
//     EXPECT_TRUE(ftqEntry.taken) << "FTQ entry should be taken";
//     EXPECT_EQ(ftqEntry.takenPC, 0x1010) << "FTQ taken PC should be 0x1010";
//     EXPECT_EQ(ftqEntry.target, 0x2000) << "FTQ target should be 0x2000";

//     // Verify s0PC was updated to point to the branch target
//     EXPECT_EQ(dbpu->s0PC, 0x2000) << "s0PC should be updated to branch target";
// }

/**
 * @brief Test complete branch prediction cycle with misprediction
 */
// TEST_F(DecoupledBPUTest, CompletePredictionCycle) {
//     // 1. Initial prediction without branches
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // 2. Create a branch and update the BTB (predict taken)
//     BranchInfo branch = createBranchInfo(0x1010, 0x2000, true);
//     updateWithBranch(*dbpu, 0x1000, branch, true);

//     // Reset PC to trigger prediction from start
//     dbpu->resetPC(0x1000);

//     // 3. Make prediction with the branch
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Verify prediction is correct
//     ASSERT_TRUE(dbpu->fetchTargetAvailable());
//     const auto& ftqEntry = dbpu->getSupplyingFetchTarget();
//     EXPECT_TRUE(ftqEntry.taken);
//     EXPECT_EQ(ftqEntry.target, 0x2000);

//     // 4. Simulate branch misprediction (actually not taken)
//     unsigned targetId = dbpu->getSupplyingTargetId();
//     unsigned streamId = ftqEntry.fsqID;

//     // Create mock PC states
//     PCStateBase branchPC(0x1010);
//     PCStateBase correctTarget(0x1014);  // Next PC after branch (not taken)

//     // Create static instruction for conditional branch
//     StaticInstPtr inst = std::make_shared<MockStaticInst>();
//     inst->setFlag(MockStaticInst::IsCondControl);
//     inst->setFlag(MockStaticInst::IsControl);

//     // Squash with actual outcome (not taken)
//     dbpu->controlSquash(
//         targetId,        // FTQ ID
//         streamId,        // FSQ ID
//         branchPC,        // Control PC
//         correctTarget,   // Target PC (not taken)
//         inst,            // Static instruction
//         4,               // Instruction size
//         false,           // Actually not taken
//         1,               // Sequence number
//         0,               // Thread ID
//         0,               // Current loop iteration
//         false            // From commit
//     );

//     // 5. Verify squashing state
//     EXPECT_TRUE(dbpu->squashing) << "Squashing flag should be set after misprediction";

//     // 6. Run a cycle to process the squash
//     executePredictionCycle(*dbpu);
//     EXPECT_FALSE(dbpu->squashing) << "Squashing flag should be cleared";

//     // 7. Run additional cycles to generate new prediction
//     for (int i = 0; i < 2; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // 8. Verify new prediction
//     ASSERT_TRUE(dbpu->fetchTargetAvailable());
//     const auto& newFtqEntry = dbpu->getSupplyingFetchTarget();
//     EXPECT_FALSE(newFtqEntry.taken) << "Branch should now be predicted not taken";

//     // 9. Verify s0PC was updated to point to the correct path
//     EXPECT_EQ(dbpu->s0PC, 0x1014) << "s0PC should be updated to not-taken path";

//     // 10. Verify branch history was updated
//     // Note: Specific history updates would depend on branch predictor implementation
//     // Here we just verify the history changed in some way
//     EXPECT_NE(dbpu->s0History, boost::dynamic_bitset<>(dbpu->historyBits, 0))
//         << "Branch history should be updated";
// }

// Test prediction and update for conditional branch
// TEST_F(DecoupledBPUTest, ConditionalBranchPrediction) {
//     // warm up 3 cycles: BP -> FSQ -> FTQ
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Create a conditional branch and update the predictor
//     BranchInfo branch = createBranchInfo(0x1000, 0x2000, true);
//     updateWithBranch(*dbpu, 0x1000, branch, true);

//     // Run a few more cycles to make sure BTB is updated
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Verify the branch was added to BTB (indirectly)
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());
// }

// // Test prediction for multiple branches in sequence
// TEST_F(DecoupledBPUTest, MultipleBranchSequence) {
//     // Initialize the prediction cycle
//     executePredictionCycle(*dbpu);

//     // Create and update with first branch (taken)
//     BranchInfo branch1 = createBranchInfo(0x1008, 0x2000, true);
//     updateWithBranch(*dbpu, 0x1000, branch1, true);

//     // Run cycles to update
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Create and update with second branch at target (not taken)
//     BranchInfo branch2 = createBranchInfo(0x2008, 0x3000, true);
//     updateWithBranch(*dbpu, 0x2000, branch2, false);

//     // Run cycles to update
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Verify both branches are in the prediction system (indirectly)
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());
// }

// // Test prediction for indirect branches
// TEST_F(DecoupledBPUTest, IndirectBranchPrediction) {
//     // Initialize the prediction cycle
//     executePredictionCycle(*dbpu);

//     // Create an indirect branch
//     BranchInfo branch = createBranchInfo(0x1010, 0x2000, false, true);
//     updateWithBranch(*dbpu, 0x1000, branch, true);

//     // Run cycles to update
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Verify the branch was added (indirectly)
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());

//     // Update with a different target
//     branch.target = 0x3000;
//     updateWithBranch(*dbpu, 0x1000, branch, true);

//     // Run cycles to update
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }
// }

// // Test call/return prediction
// TEST_F(DecoupledBPUTest, CallReturnPrediction) {
//     // Initialize the prediction cycle
//     executePredictionCycle(*dbpu);

//     // Create a call instruction
//     BranchInfo call = createBranchInfo(0x1010, 0x2000, false, false, true, false);
//     updateWithBranch(*dbpu, 0x1000, call, true);

//     // Run cycles to update
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Create a return instruction
//     BranchInfo ret = createBranchInfo(0x2020, 0x1014, false, false, false, true);
//     updateWithBranch(*dbpu, 0x2000, ret, true);

//     // Run cycles to update
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Verify both instructions were added (indirectly)
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());
// }

// // Test non-control squash
// TEST_F(DecoupledBPUTest, NonControlSquash) {
//     // Initialize the prediction cycle
//     executePredictionCycle(*dbpu);

//     // Get current FSQ and FTQ IDs
//     FetchStreamId fsqId = dbpu->fsqId - 1;
//     unsigned targetId = dbpu->fetchTargetQueue.getSupplyingTargetId();

//     // Create PC state
//     MockPCState pcState(0x1020);

//     // Execute non-control squash
//     dbpu->nonControlSquash(targetId, fsqId, pcState, 1, 0, 0);

//     // Run a few cycles to recover
//     for (int i = 0; i < 3; i++) {
//         executePredictionCycle(*dbpu);
//     }

//     // Verify recovery happened (indirectly)
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());
// }

// More granular test for the pipelined prediction process
TEST_F(DecoupledBPUTest, PipelinedPredictionProcess) {
    // Initial state
    EXPECT_FALSE(dbpu->sentPCHist);
    EXPECT_FALSE(dbpu->receivedPred);
    dbpu->squashing = false;
    EXPECT_EQ(dbpu->numOverrideBubbles, 0);

    // ------------ CYCLE 0 ------------
    // Only prediction 0 is generated
    PipelineStage::generatePredictions(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist) << "sentPCHist should be true after generating predictions";
    EXPECT_FALSE(dbpu->receivedPred) << "receivedPred should still be false";

    // ------------ CYCLE 1 ------------
    // Prediction 0 gets finalized, and FSQ0 is created
    // Also prediction 1 is generated
    PipelineStage::generateFinalPrediction(*dbpu);
    EXPECT_TRUE(dbpu->receivedPred) << "receivedPred should be true after final prediction";

    PipelineStage::enqueueFetchStream(*dbpu);
    EXPECT_FALSE(dbpu->receivedPred) << "receivedPred should be reset after FSQ enqueue";

    auto fsqId0 = dbpu->fsqId - 1; // FSQ ID of the entry we just created
    EXPECT_EQ(fsqId0, 1) << "First FSQ entry should have ID 1";
    EXPECT_EQ(dbpu->fetchStreamQueue.count(fsqId0), 1) << "FSQ entry should exist";

    // Generate prediction 1
    PipelineStage::generatePredictions(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist) << "sentPCHist should be true after generating prediction 1";

    // ------------ CYCLE 2 ------------
    // Prediction 1 gets finalized, FSQ1 is created, FTQ0 is created
    // Also prediction 2 is generated
    PipelineStage::generateFinalPrediction(*dbpu);
    EXPECT_TRUE(dbpu->receivedPred) << "receivedPred should be true after final prediction";

    PipelineStage::enqueueFetchStream(*dbpu);
    EXPECT_FALSE(dbpu->receivedPred) << "receivedPred should be reset after FSQ enqueue";

    auto fsqId1 = dbpu->fsqId - 1; // FSQ ID of the entry we just created
    EXPECT_EQ(fsqId1, 2) << "Second FSQ entry should have ID 2";
    EXPECT_EQ(dbpu->fetchStreamQueue.count(fsqId1), 1) << "FSQ entry should exist";

    // Enqueue FTQ entry for prediction 0
    PipelineStage::enqueueFetchTarget(*dbpu);
    // Now fetch can use FTQ0
    bool in_loop = false;
    dbpu->trySupplyFetchWithTarget(0x1000, in_loop);
    EXPECT_TRUE(dbpu->fetchTargetAvailable()) << "FTQ should have an entry now";

    // Verify that the FTQ entry corresponds to FSQ entry 0
    const auto& ftqEntry = dbpu->getSupplyingFetchTarget();
    EXPECT_EQ(ftqEntry.fsqID, 1) << "FTQ entry should reference FSQ 1";

    // Generate prediction 2
    PipelineStage::generatePredictions(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist) << "sentPCHist should be true after generating prediction 2";

    // ------------ CYCLE 3 ------------
    // Prediction 2 gets finalized, FSQ2 is created, FTQ1 is created
    // Also prediction 3 is generated
    PipelineStage::generateFinalPrediction(*dbpu);
    EXPECT_TRUE(dbpu->receivedPred) << "receivedPred should be true after final prediction";

    PipelineStage::enqueueFetchStream(*dbpu);
    EXPECT_FALSE(dbpu->receivedPred) << "receivedPred should be reset after FSQ enqueue";

    auto fsqId2 = dbpu->fsqId - 1; // FSQ ID of the entry we just created
    EXPECT_EQ(fsqId2, 3) << "Third FSQ entry should have ID 3";
    EXPECT_EQ(dbpu->fetchStreamQueue.count(fsqId2), 1) << "FSQ entry should exist";

    // Enqueue FTQ entry for prediction 1
    PipelineStage::enqueueFetchTarget(*dbpu);

    // Verify that both FSQ entries and FTQ entries are in expected state
    EXPECT_EQ(dbpu->fetchStreamQueue.size(), 3) << "Should have 3 FSQ entries";
    EXPECT_TRUE(dbpu->fetchTargetAvailable()) << "FTQ should still have entries";


}

/**
 * @brief Test override bubbles in the pipelined prediction process
 */
TEST_F(DecoupledBPUTest, PipelinedOverrideBubbles) {
    dbpu->squashing = false;
    // ------------ CYCLE 0 ------------
    // Generate prediction 0
    PipelineStage::generatePredictions(*dbpu);
    EXPECT_TRUE(dbpu->sentPCHist);

    // ------------ CYCLE 1 ------------
    // Finalize prediction 0, but simulate override from later stage
    // by setting override bubbles
    PipelineStage::generateFinalPrediction(*dbpu);
    EXPECT_TRUE(dbpu->receivedPred);

    // Set override bubbles to 2 (as if stage 2 overrode stage 0)
    dbpu->numOverrideBubbles = 2;

    // Try to enqueue fetch stream but it should be blocked by bubbles
    PipelineStage::enqueueFetchStream(*dbpu);
    // receivedPred should still be true since enqueue was blocked
    EXPECT_TRUE(dbpu->receivedPred) << "receivedPred should still be true when blocked by bubbles";
    EXPECT_EQ(dbpu->fetchStreamQueue.size(), 0) << "No FSQ entries should be created yet";

    // Generate prediction 1
    PipelineStage::generatePredictions(*dbpu);
    EXPECT_FALSE(dbpu->sentPCHist) << "receivedPred is true, not generating prediction 1";

    // ------------ CYCLE 2 ------------
    // Consume one bubble
    PipelineStage::consumeOverrideBubble(*dbpu);
    EXPECT_EQ(dbpu->numOverrideBubbles, 1) << "Should have 1 bubble left";

    // Try to enqueue fetch stream but still blocked
    PipelineStage::enqueueFetchStream(*dbpu);
    EXPECT_TRUE(dbpu->receivedPred) << "receivedPred should still be true";
    EXPECT_EQ(dbpu->fetchStreamQueue.size(), 0) << "Still no FSQ entries";

    // ------------ CYCLE 3 ------------
    // Consume second bubble
    PipelineStage::consumeOverrideBubble(*dbpu);
    EXPECT_EQ(dbpu->numOverrideBubbles, 0) << "All bubbles should be consumed";

    // Now FSQ enqueue should succeed
    PipelineStage::enqueueFetchStream(*dbpu);
    EXPECT_FALSE(dbpu->receivedPred) << "receivedPred should be reset now";
    EXPECT_EQ(dbpu->fetchStreamQueue.size(), 1) << "FSQ entry should be created";

    // And the pipeline should continue normally
    PipelineStage::enqueueFetchTarget(*dbpu);
    EXPECT_TRUE(dbpu->fetchTargetQueue.size() == 1) << "FTQ should have an entry";
}

/**
 * @brief Test the pipelined prediction process with multiple branches
 */
// TEST_F(DecoupledBPUTest, PipelinedMultipleBranches) {
//     // Setup a sequence of branches to test
//     // PC 0x1000: Branch to 0x2000 (taken)
//     // PC 0x2000: Branch to 0x3000 (taken)
//     // PC 0x3000: Branch to 0x4000 (taken)

//     // Create branch infos
//     BranchInfo branch1 = createBranchInfo(0x1000, 0x2000, true);
//     BranchInfo branch2 = createBranchInfo(0x2000, 0x3000, true);
//     BranchInfo branch3 = createBranchInfo(0x3000, 0x4000, true);

//     // Manually add entries to BTB using updateWithBranch
//     updateWithBranch(*dbpu, 0x1000, branch1, true);
//     updateWithBranch(*dbpu, 0x2000, branch2, true);
//     updateWithBranch(*dbpu, 0x3000, branch3, true);

//     // Reset PC to starting point
//     dbpu->resetPC(0x1000);

//     // ==== SIMULATE PIPELINED PREDICTION ====

//     // Cycle 0: Generate prediction for 0x1000 (branch1)
//     PipelineStage::generatePredictions(*dbpu);
//     EXPECT_TRUE(dbpu->sentPCHist);

//     // Cycle 1: Finalize prediction for 0x1000, create FSQ0
//     // Also generate prediction for 0x2000 (branch2)
//     PipelineStage::generateFinalPrediction(*dbpu);
//     EXPECT_TRUE(dbpu->receivedPred);

//     PipelineStage::enqueueFetchStream(*dbpu);
//     EXPECT_FALSE(dbpu->receivedPred);

//     // Verify FSQ entry for branch1
//     auto fsqId0 = dbpu->fsqId - 1;
//     const auto& fsq0 = dbpu->fetchStreamQueue.find(fsqId0)->second;
//     EXPECT_EQ(fsq0.startPC, 0x1000);
//     EXPECT_TRUE(fsq0.isHit);
//     EXPECT_TRUE(fsq0.predTaken);
//     EXPECT_EQ(fsq0.predBranchInfo.target, 0x2000);

//     // s0PC should now be 0x2000 (target of branch1)
//     EXPECT_EQ(dbpu->s0PC, 0x2000);

//     // Generate prediction for branch2
//     PipelineStage::generatePredictions(*dbpu);
//     EXPECT_TRUE(dbpu->sentPCHist);

//     // Cycle 2: Finalize prediction for 0x2000, create FSQ1
//     // Also generate prediction for 0x3000 (branch3)
//     // Also enqueue FTQ0 for branch1
//     PipelineStage::generateFinalPrediction(*dbpu);
//     EXPECT_TRUE(dbpu->receivedPred);

//     PipelineStage::enqueueFetchStream(*dbpu);
//     EXPECT_FALSE(dbpu->receivedPred);

//     // Verify FSQ entry for branch2
//     auto fsqId1 = dbpu->fsqId - 1;
//     const auto& fsq1 = dbpu->fetchStreamQueue.find(fsqId1)->second;
//     EXPECT_EQ(fsq1.startPC, 0x2000);
//     EXPECT_TRUE(fsq1.isHit);
//     EXPECT_TRUE(fsq1.predTaken);
//     EXPECT_EQ(fsq1.predBranchInfo.target, 0x3000);

//     // s0PC should now be 0x3000 (target of branch2)
//     EXPECT_EQ(dbpu->s0PC, 0x3000);

//     // Enqueue FTQ entry for branch1
//     PipelineStage::enqueueFetchTarget(*dbpu);
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());

//     // Verify FTQ entry for branch1
//     const auto& ftq0 = dbpu->getSupplyingFetchTarget();
//     EXPECT_EQ(ftq0.startPC, 0x1000);
//     EXPECT_EQ(ftq0.fsqID, fsqId0);
//     EXPECT_TRUE(ftq0.taken);
//     EXPECT_EQ(ftq0.target, 0x2000);

//     // Generate prediction for branch3
//     PipelineStage::generatePredictions(*dbpu);
//     EXPECT_TRUE(dbpu->sentPCHist);

//     // Cycle 3: Finalize prediction for 0x3000, create FSQ2
//     // Also enqueue FTQ1 for branch2
//     PipelineStage::generateFinalPrediction(*dbpu);
//     EXPECT_TRUE(dbpu->receivedPred);

//     PipelineStage::enqueueFetchStream(*dbpu);
//     EXPECT_FALSE(dbpu->receivedPred);

//     // Verify FSQ entry for branch3
//     auto fsqId2 = dbpu->fsqId - 1;
//     const auto& fsq2 = dbpu->fetchStreamQueue.find(fsqId2)->second;
//     EXPECT_EQ(fsq2.startPC, 0x3000);
//     EXPECT_TRUE(fsq2.isHit);
//     EXPECT_TRUE(fsq2.predTaken);
//     EXPECT_EQ(fsq2.predBranchInfo.target, 0x4000);

//     // s0PC should now be 0x4000 (target of branch3)
//     EXPECT_EQ(dbpu->s0PC, 0x4000);

//     // Enqueue FTQ entry for branch2
//     PipelineStage::enqueueFetchTarget(*dbpu);

//     // Fetch can now use FTQ0 (branch1)
//     bool in_loop = false;
//     bool has_target1 = dbpu->trySupplyFetchWithTarget(0x1000, in_loop);
//     EXPECT_TRUE(has_target1);

//     // Cycle 4: Enqueue FTQ2 for branch3
//     PipelineStage::enqueueFetchTarget(*dbpu);

//     // Fetch can now use FTQ1 (branch2)
//     bool has_target2 = dbpu->trySupplyFetchWithTarget(0x2000, in_loop);
//     EXPECT_TRUE(has_target2);

//     // Verify final state
//     EXPECT_EQ(dbpu->fetchStreamQueue.size(), 3);
//     EXPECT_TRUE(dbpu->fetchTargetAvailable());

//     // Fetch can now use FTQ2 (branch3)
//     bool has_target3 = dbpu->trySupplyFetchWithTarget(0x3000, in_loop);
//     EXPECT_TRUE(has_target3);
// }

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}


