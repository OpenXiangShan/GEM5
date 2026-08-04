#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "base/types.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/ras.hh"

namespace gem5 {
namespace branch_prediction {
namespace btb_pred {
namespace test {

class RASPopPushTest : public ::testing::Test
{
  protected:
    void SetUp() override {
        ras = std::make_unique<BTBRAS>(16, 2, 8);
        checkReturnTarget(0, 0x80000000L);
    }

    void checkReturnTarget(Addr startAddr, Addr expectedTarget) {
        boost::dynamic_bitset<> history(8, 0);
        std::vector<FullBTBPrediction> stagePreds(4);
        ras->putPCHistory(startAddr, history, stagePreds);
        EXPECT_EQ(stagePreds[0].returnTarget, expectedTarget);
    }

    FullBTBPrediction createPrediction(Addr pc, Addr target, bool isCall, bool isReturn) {
        BTBEntry entry;
        entry.valid = true;
        entry.pc = pc;
        entry.target = target;
        entry.size = 4;
        entry.isCall = isCall;
        entry.isReturn = isReturn;

        FullBTBPrediction pred;
        pred.bbStart = pc;
        pred.btbEntries.push_back(entry);
        return pred;
    }

    FetchTarget createStream(Addr pc, Addr target, bool isCall, bool isReturn,
                             std::shared_ptr<void> meta) {
        FetchTarget stream;
        stream.startPC = pc;
        stream.exeTaken = true;
        stream.exeBranchInfo.pc = pc;
        stream.exeBranchInfo.target = target;
        stream.exeBranchInfo.size = 4;
        stream.exeBranchInfo.isCall = isCall;
        stream.exeBranchInfo.isReturn = isReturn;
        stream.predMetas[0] = meta;
        return stream;
    }

    void specAndCommitCall(Addr pc, Addr target) {
        auto meta = ras->getPredictionMeta();
        auto pred = createPrediction(pc, target, true, false);
        ras->specUpdateState(pred);
        ras->update(createStream(pc, target, true, false, meta));
    }

    std::unique_ptr<BTBRAS> ras;
};

TEST_F(RASPopPushTest, CallReturnPopsBeforePushingInAllPaths) {
    specAndCommitCall(0x1000, 0x2000);
    checkReturnTarget(0x2000, 0x1004);
    specAndCommitCall(0x2000, 0x3000);
    checkReturnTarget(0x3000, 0x2004);

    // JALR with distinct link registers must pop before pushing its return PC.
    auto popPushMeta = ras->getPredictionMeta();
    auto popPush = createPrediction(0x3000, 0x2004, true, true);
    ras->specUpdateState(popPush);
    checkReturnTarget(0x2004, 0x3004);

    auto youngerCall = createPrediction(0x3004, 0x4000, true, false);
    ras->specUpdateState(youngerCall);
    ras->recoverState(createStream(0x3000, 0x2004, true, true, popPushMeta));
    checkReturnTarget(0x2004, 0x3004);

    ras->update(createStream(0x3000, 0x2004, true, true, popPushMeta));
    checkReturnTarget(0x2004, 0x3004);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

int
main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
