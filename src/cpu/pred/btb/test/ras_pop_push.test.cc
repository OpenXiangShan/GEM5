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

    BranchInfo create_branch(Addr pc, Addr target, bool isCall,
                             bool isReturn) {
        BranchInfo branch;
        branch.pc = pc;
        branch.target = target;
        branch.size = 4;
        branch.isIndirect = isReturn;
        branch.isDirect = !branch.isIndirect;
        branch.isCall = isCall;
        branch.isReturn = isReturn;
        return branch;
    }

    BranchOutcome create_outcome(const BranchInfo &branch, bool taken = true) {
        BranchOutcome outcome;
        outcome.pc = branch.pc;
        outcome.target = branch.target;
        outcome.taken = taken;
        outcome.isCond = branch.isCond;
        outcome.isIndirect = branch.isIndirect;
        outcome.isDirect = branch.isDirect;
        outcome.isCall = branch.isCall;
        outcome.isReturn = branch.isReturn;
        outcome.size = branch.size;
        return outcome;
    }

    FetchTarget create_context(Addr pc, std::shared_ptr<void> meta) {
        FetchTarget stream;
        stream.startPC = pc;
        stream.predMetas[0] = meta;
        return stream;
    }

    void spec_and_commit_call(Addr pc, Addr target) {
        auto meta = ras->getPredictionMeta();
        auto pred = create_prediction(pc, target, true, false);
        ras->specUpdateState(pred);
        const auto stream = create_context(pc, meta);
        const auto branch = create_branch(pc, target, true, false);
        const auto outcome = create_outcome(branch);
        const PredictionUpdateContext context(stream);
        const PreparedUpdate update(context, {outcome});
        ras->update(context, update);
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
    const auto recoveryStream = create_context(0x3000, popPushMeta);
    const auto actualBranch =
        create_branch(0x3000, 0x2004, true, true);
    ras->recoverState(
        HistoryRecoveryContext(recoveryStream),
        actualBranch, true);
    check_return_target(0x2004, 0x3004);

    const auto stream = create_context(0x3000, popPushMeta);
    const auto outcome = create_outcome(actualBranch);
    const PredictionUpdateContext context(stream);
    const PreparedUpdate update(context, {outcome});
    ras->update(context, update);
    check_return_target(0x2004, 0x3004);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
