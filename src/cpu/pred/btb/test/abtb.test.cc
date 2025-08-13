#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cpu/pred/btb/btb.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{


FetchStream createStream(Addr startPC, FullBTBPrediction &pred, DefaultBTB *abtb) {
    FetchStream stream;
    stream.startPC = startPC;
    Addr fallThroughAddr = pred.getFallThrough(abtb->predictWidth);
    stream.isHit = pred.btbEntries.size() > 0; // TODO: fix isHit and falseHit
    stream.falseHit = false;
    stream.predBTBEntries = pred.btbEntries;
    stream.predTaken = pred.isTaken();
    stream.predEndPC = fallThroughAddr;
    stream.predMetas[0] = abtb->getPredictionMeta();
    return stream;
}

void resolveStream(FetchStream &stream, bool taken, Addr brPc, Addr target, bool isCond, int size=4) {
    stream.resolved = true;
    stream.exeBranchInfo.pc = brPc;
    stream.exeBranchInfo.target = target;
    stream.exeBranchInfo.isCond = isCond;
    stream.exeBranchInfo.size = size;
    stream.exeTaken = taken;
}

FullBTBPrediction makePrediction(Addr startPC, DefaultBTB *abtb) {
    std::vector<FullBTBPrediction> stagePreds(2);  // 2 stages
    boost::dynamic_bitset<> history(8, 0); // history does not matter for BTB
    abtb->putPCHistory(startPC, history, stagePreds);
    return stagePreds[1];
}

void updateBTB(FetchStream &stream, DefaultBTB *abtb) {
    abtb->getAndSetNewBTBEntry(stream); // usually called by mbtb, here for testing purpose
    abtb->update(stream);
}


// Test fixture for Pipelined BTB tests
class ABTBTest : public ::testing::Test
{
protected:
    void SetUp() override {
        // Create a BTB with 16 entries, 8-bit tags, 4-way associative, 1-cycle delay
        // The last parameter (true) enables pipelined operation
        abtb = new DefaultBTB(16, 20, 4, 1, false, 1);
        // assert(!abtb->entryHalfAligned);

        bigAbtb = new DefaultBTB(1024, 20, 1, 1, false, 1);
    }

    DefaultBTB* abtb;
    DefaultBTB* bigAbtb;
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
    updateBTB(stream_A, abtb);
    updateBTB(stream_B, abtb);

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
    updateBTB(stream_A, bigAbtb);
    updateBTB(stream_B, bigAbtb);

    // ---------------- testing phase ----------------
    // when we've arrived at Fetch Block C, aBTB shouldn't return the entry trained with Fetch Block B
    // though the mistake is likely to happen, because FB C and FB B share the same tag bits
    // the solution is to store the startPC in a aBTB entry
    auto pred_A_test = makePrediction(startPC_A, bigAbtb);
    auto pred_C_test = makePrediction(startPC_C, bigAbtb);
    EXPECT_EQ(pred_C_test.btbEntries.size(), 0);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
