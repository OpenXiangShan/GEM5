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
        check_return_target(0, 0x80000000L);
    }

    void check_return_target(Addr startAddr, Addr expectedTarget) {
        boost::dynamic_bitset<> history(8, 0);
        std::vector<FullBTBPrediction> stagePreds(4);
        ras->putPCHistory(startAddr, history, stagePreds);
        EXPECT_EQ(stagePreds[0].returnTarget, expectedTarget);
    }

    FullBTBPrediction create_prediction(Addr pc, Addr target, bool isCall,
                                        bool isReturn) {
        BTBEntry entry;
        entry.valid = true;
        entry.pc = pc;
        entry.target = target;
        entry.size = 4;
        entry.isCond = false;
        entry.isIndirect = isReturn;
        entry.isDirect = !entry.isIndirect;
        entry.isCall = isCall;
        entry.isReturn = isReturn;

        FullBTBPrediction pred;
        pred.bbStart = pc;
        pred.btbEntries.push_back(entry);
        return pred;
    }

    FetchTarget create_stream(Addr pc, Addr target, bool isCall, bool isReturn,
                              std::shared_ptr<void> meta) {
        FetchTarget stream;
        stream.startPC = pc;
        stream.exeTaken = true;
        stream.exeBranchInfo.pc = pc;
        stream.exeBranchInfo.target = target;
        stream.exeBranchInfo.size = 4;
        stream.exeBranchInfo.isCond = false;
        stream.exeBranchInfo.isIndirect = isReturn;
        stream.exeBranchInfo.isDirect = !stream.exeBranchInfo.isIndirect;
        stream.exeBranchInfo.isCall = isCall;
        stream.exeBranchInfo.isReturn = isReturn;
        stream.predMetas[0] = meta;
        return stream;
    }

    void spec_and_commit_call(Addr pc, Addr target) {
        auto meta = ras->getPredictionMeta();
        auto pred = create_prediction(pc, target, true, false);
        ras->specUpdateState(pred);
        const auto stream = create_stream(pc, target, true, false, meta);
        ras->update(PredictionUpdateContext(stream), PreparedUpdate());
    }

    std::unique_ptr<BTBRAS> ras;
};

TEST_F(RASPopPushTest, CallReturnPopsBeforePushingInAllPaths) {
    spec_and_commit_call(0x1000, 0x2000);
    check_return_target(0x2000, 0x1004);
    spec_and_commit_call(0x2000, 0x3000);
    check_return_target(0x3000, 0x2004);

    // JALR with distinct link registers must pop before pushing its return PC.
    auto popPushMeta = ras->getPredictionMeta();
    auto popPush = create_prediction(0x3000, 0x2004, true, true);
    ras->specUpdateState(popPush);
    check_return_target(0x2004, 0x3004);

    auto youngerCall = create_prediction(0x3004, 0x4000, true, false);
    ras->specUpdateState(youngerCall);
    ras->recoverState(create_stream(0x3000, 0x2004, true, true, popPushMeta));
    check_return_target(0x2004, 0x3004);

    const auto stream =
        create_stream(0x3000, 0x2004, true, true, popPushMeta);
    ras->update(PredictionUpdateContext(stream), PreparedUpdate());
    check_return_target(0x2004, 0x3004);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
