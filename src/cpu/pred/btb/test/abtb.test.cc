#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cpu/pred/btb/abtb.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{


FetchTarget createStream(Addr startPC, FullBTBPrediction &pred, AheadBTB *abtb) {
    FetchTarget stream;
    stream.tid = pred.tid;
    stream.asidHash = pred.asidHash;
    stream.startPC = startPC;
    Addr fallThroughAddr = pred.getFallThrough(abtb->predictWidth);
    stream.isHit = pred.btbEntries.size() > 0; // TODO: fix isHit and falseHit
    stream.falseHit = false;
    stream.setPredictedBranches(pred.btbEntries);
    stream.predTaken = pred.isTaken();
    stream.predEndPC = fallThroughAddr;
    stream.predMetas[0] = abtb->getPredictionMeta(stream.tid);
    return stream;
}

BranchOutcome
makeBranchOutcome(bool taken, Addr brPc, Addr target, bool isCond,
                  int size = 4)
{
    BranchOutcome outcome;
    outcome.pc = brPc;
    outcome.target = target;
    outcome.taken = taken;
    outcome.mispredicted = false;
    outcome.isCond = isCond;
    outcome.size = size;
    return outcome;
}

FullBTBPrediction makePrediction(Addr startPC, AheadBTB *abtb,
                                 ThreadID tid = 0, uint8_t asidHash = 0) {
    std::vector<FullBTBPrediction> stagePreds(2);  // 2 stages
    for (int i = 0; i < stagePreds.size(); i++) {
        stagePreds[i].tid = tid;
        stagePreds[i].asidHash = asidHash;
        stagePreds[i].bbStart = startPC;
        stagePreds[i].predSource = i;
    }
    boost::dynamic_bitset<> history(8, 0); // history does not matter for BTB
    abtb->putPCHistory(startPC, history, stagePreds);
    return stagePreds[1];
}

void clearAheadPipeline(AheadBTB *abtb, ThreadID tid) {
    abtb->recoverState(tid);
}

void
updateABTB(FetchTarget &stream, AheadBTB *abtb,
           const BranchOutcome &outcome)
{
    // ABTB and MBTB use different metadata types.  In UNIT_TEST builds both
    // components use index 0, so do not ask MBTB to read ABTB's metadata here.
    const PredictionUpdateContext context(stream);
    PreparedUpdate update(
        context, std::vector<BranchOutcome>{outcome});

    if (update.branches.empty() && outcome.taken) {
        BranchInfo actual_branch;
        actual_branch.pc = outcome.pc;
        actual_branch.target = outcome.target;
        actual_branch.isCond = outcome.isCond;
        actual_branch.size = outcome.size;
        update.setBTBEntryCandidate(BTBEntry(actual_branch), false);
        update.applyOutcome(outcome);
    }
    abtb->update(context, update);
}


// Test fixture for Pipelined BTB tests
class ABTBTest : public ::testing::Test
{
protected:
    void SetUp() override {
        // Create a BTB with 16 entries, 8-bit tags, 4-way associative, 1-cycle delay
        // The last parameter (true) enables pipelined operation
        abtb = new AheadBTB(16, 20, 4, 0);
        // AheadBTB never uses half-aligned mode

        bigAbtb = new AheadBTB(1024, 20, 1, 0);
    }

    AheadBTB* abtb;
    AheadBTB* bigAbtb;
};

TEST_F(ABTBTest, BasicPredictionUpdateCycle){
    // Some constants
    // Stream A addresses
    Addr startPC_A = 0x1000;
    Addr brPC_A = 0x1004;
    Addr target_A = 0x2000;

    // Stream B addresses
    Addr startPC_B = 0x2000;
    Addr brPC_B = 0x2004;
    Addr target_B = 0x3000;

    // ---------------- training phase ----------------
    // make predictions and create Fetch Streams
    auto pred_A = makePrediction(startPC_A, abtb);
    auto stream_A = createStream(startPC_A, pred_A, abtb);
    auto pred_B = makePrediction(startPC_B, abtb);
    auto stream_B = createStream(startPC_B, pred_B, abtb);
    stream_B.previousPCs.push(stream_A.startPC); // crucial! set previous PC for ahead pipelining
    // update BTB with branch information
    updateABTB(
        stream_A, abtb,
        makeBranchOutcome(true, brPC_A, target_A, true));
    updateABTB(
        stream_B, abtb,
        makeBranchOutcome(true, brPC_B, target_B, true));

    // ---------------- testing phase ----------------
    // make predictions and check if BTB is updated correctly
    auto pred_A_test = makePrediction(startPC_A, abtb);
    auto pred_B_test = makePrediction(startPC_B, abtb);
    EXPECT_EQ(pred_B_test.btbEntries.size(), 1);
    if (!pred_B_test.btbEntries.empty()) {
        EXPECT_EQ(pred_B_test.btbEntries[0].pc, brPC_B);
        EXPECT_EQ(pred_B_test.btbEntries[0].target, target_B);
    }

}

TEST_F(ABTBTest, AliasAvoidance){
    // Some constants
    // Stream A addresses
    Addr startPC_A = 0x100;
    Addr brPC1_A = 0x104;
    Addr brPC2_A = 0x108;
    Addr target1_A = 0x200;
    Addr target2_A = 0x300;
    // Stream B addresses
    Addr startPC_B = 0x300;
    Addr brPC_B = 0x304;
    Addr target_B = 0x3000;

    // Stream C addresses
    Addr startPC_C = 0x200;
    Addr brPC_C = 0x204;
    Addr target_C = 0x2000;

    // ---------------- training phase ----------------
    // make predictions and create Fetch Streams
    auto pred_A = makePrediction(startPC_A, bigAbtb);
    auto stream_A = createStream(startPC_A, pred_A, bigAbtb);
    auto pred_B = makePrediction(startPC_B, bigAbtb);
    auto stream_B = createStream(startPC_B, pred_B, bigAbtb);
    stream_B.previousPCs.push(stream_A.startPC); // crucial! set previous PC for ahead pipelining
    // update BTB with branch information
    // now aBTB ought to have a entry, indexed by startPC_A, tagged with startPC_B
    updateABTB(
        stream_A, bigAbtb,
        makeBranchOutcome(true, brPC1_A, target1_A, true));
    updateABTB(
        stream_B, bigAbtb,
        makeBranchOutcome(true, brPC_B, target_B, true));

    // ---------------- testing phase ----------------
    // when we've arrived at Fetch Block C, aBTB shouldn't return the entry trained with Fetch Block B
    // though the mistake is likely to happen, because FB C and FB B share the same tag bits
    // the solution is to store the startPC in a aBTB entry
    auto pred_A_test = makePrediction(startPC_A, bigAbtb);
    auto pred_C_test = makePrediction(startPC_C, bigAbtb);
    EXPECT_EQ(pred_C_test.btbEntries.size(), 0);
}

TEST_F(ABTBTest, AheadPipelineIsThreadIsolated){
    AheadBTB twoThreadAbtb(1024, 20, 1, 0, 2);

    Addr t0PrevPC = 0x1000;
    Addr t0StartPC = 0x2000;
    Addr t0BrPC = 0x2004;
    Addr t0Target = 0x3000;
    Addr t1PrevPC = 0x1040;

    // Train a thread-0 ABTB entry indexed by t0PrevPC and tagged by t0StartPC.
    auto pred_t0 = makePrediction(t0StartPC, &twoThreadAbtb, 0);
    auto stream_t0 = createStream(t0StartPC, pred_t0, &twoThreadAbtb);
    stream_t0.previousPCs.push(t0PrevPC);
    updateABTB(
        stream_t0, &twoThreadAbtb,
        makeBranchOutcome(true, t0BrPC, t0Target, true));

    clearAheadPipeline(&twoThreadAbtb, 0);
    clearAheadPipeline(&twoThreadAbtb, 1);

    // Interleave another thread between thread 0's previous/current blocks.
    // With a shared ahead FIFO, thread 0's current lookup would consume the
    // set read by thread 1 and miss the trained entry.
    makePrediction(t0PrevPC, &twoThreadAbtb, 0);
    makePrediction(t1PrevPC, &twoThreadAbtb, 1);
    auto pred_t0_test = makePrediction(t0StartPC, &twoThreadAbtb, 0);

    EXPECT_EQ(pred_t0_test.btbEntries.size(), 1);
    if (!pred_t0_test.btbEntries.empty()) {
        EXPECT_EQ(pred_t0_test.btbEntries[0].pc, t0BrPC);
        EXPECT_EQ(pred_t0_test.btbEntries[0].target, t0Target);
    }
}

TEST_F(ABTBTest, ResolveUpdateOnlyTrainsExplicitBranch)
{
    constexpr Addr previous_pc = 0x1000;
    constexpr Addr start_pc = 0x2000;
    constexpr Addr branch_a_pc = 0x2004;
    constexpr Addr branch_b_pc = 0x2008;

    auto empty_pred = makePrediction(start_pc, abtb);
    auto insert_a = createStream(start_pc, empty_pred, abtb);
    insert_a.previousPCs.push(previous_pc);
    updateABTB(
        insert_a, abtb,
        makeBranchOutcome(true, branch_a_pc, 0x3000, true));

    auto pred_after_a = makePrediction(start_pc, abtb);
    auto insert_b = createStream(start_pc, pred_after_a, abtb);
    insert_b.previousPCs.push(previous_pc);
    updateABTB(
        insert_b, abtb,
        makeBranchOutcome(true, branch_b_pc, 0x4000, true));

    clearAheadPipeline(abtb, 0);
    makePrediction(previous_pc, abtb);
    auto both_pred = makePrediction(start_pc, abtb);
    ASSERT_EQ(both_pred.btbEntries.size(), 2);

    auto resolve_a = createStream(start_pc, both_pred, abtb);
    resolve_a.previousPCs.push(previous_pc);
    abtb->setTrainingStage(PredictorTrainingStage::Resolve);
    const auto outcome =
        makeBranchOutcome(false, branch_a_pc, branch_a_pc + 4, true);
    PreparedUpdate update(
        PredictionUpdateContext(resolve_a),
        std::vector<BranchOutcome>{outcome});
    BranchInfo duplicate_candidate;
    duplicate_candidate.pc = outcome.pc;
    duplicate_candidate.target = outcome.target;
    duplicate_candidate.isCond = outcome.isCond;
    duplicate_candidate.size = outcome.size;
    update.setBTBEntryCandidate(BTBEntry(duplicate_candidate), false);
    update.applyOutcome(outcome);
    ASSERT_EQ(update.branches.size(), 2);
    abtb->update(PredictionUpdateContext(resolve_a), update);

    clearAheadPipeline(abtb, 0);
    makePrediction(previous_pc, abtb);
    auto updated_pred = makePrediction(start_pc, abtb);
    ASSERT_EQ(updated_pred.btbEntries.size(), 2);
    EXPECT_FALSE(updated_pred.btbEntries[0].alwaysTaken);
    EXPECT_EQ(updated_pred.btbEntries[0].ctr, -1);
    EXPECT_TRUE(updated_pred.btbEntries[1].alwaysTaken);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
