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
    stream.predBTBEntries = pred.btbEntries;
    stream.predTaken = pred.isTaken();
    stream.predEndPC = fallThroughAddr;
    stream.predMetas[0] = abtb->getPredictionMeta(stream.tid);
    return stream;
}

void resolveStream(FetchTarget &stream, bool taken, Addr brPc, Addr target, bool isCond, int size=4) {
    stream.resolved = true;
    stream.exeBranchInfo.pc = brPc;
    stream.exeBranchInfo.target = target;
    stream.exeBranchInfo.isCond = isCond;
    stream.exeBranchInfo.size = size;
    stream.exeTaken = taken;
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

void updateABTB(FetchTarget &stream, AheadBTB *abtb) {
    // ABTB and MBTB use different metadata types.  In UNIT_TEST builds both
    // components use index 0, so do not ask MBTB to read ABTB's metadata here.
    PreparedUpdate update(stream, abtb->predictWidth);
    update.setBTBEntryCandidate(
        BTBEntry(stream.exeBranchInfo), false);
    abtb->update(PredictionUpdateContext(stream), update);
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
    // resolve Fetch Stream (FS reached commit stage of backend)
    resolveStream(stream_A, true, brPC_A, target_A, true);
    resolveStream(stream_B, true, brPC_B, target_B, true);
    // update BTB with branch information
    updateABTB(stream_A, abtb);
    updateABTB(stream_B, abtb);

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
    // resolve Fetch Stream (FS reached commit stage of backend)
    resolveStream(stream_A, true, brPC1_A, target1_A, true);
    resolveStream(stream_B, true, brPC_B, target_B, true);
    // update BTB with branch information
    // now aBTB ought to have a entry, indexed by startPC_A, tagged with startPC_B
    updateABTB(stream_A, bigAbtb);
    updateABTB(stream_B, bigAbtb);

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
    resolveStream(stream_t0, true, t0BrPC, t0Target, true);
    updateABTB(stream_t0, &twoThreadAbtb);

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

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
