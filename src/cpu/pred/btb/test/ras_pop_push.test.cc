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
        ras->update(create_stream(pc, target, true, false, meta));
    }

    void reset_ras(unsigned inflight_entries) {
        ras = std::make_unique<BTBRAS>(16, 3, inflight_entries);
        check_return_target(0, 0x80000000L);
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

    ras->update(create_stream(0x3000, 0x2004, true, true, popPushMeta));
    check_return_target(0x2004, 0x3004);
}

TEST_F(RASPopPushTest, BlocksSpecUpdatesNearInflightOverflow) {
    reset_ras(32);

    std::shared_ptr<void> recovery_meta;
    for (int i = 0; i < 31; ++i) {
        const Addr call_pc = 0x1000 + i * 4;
        check_return_target(call_pc,
            i == 0 ? 0x80000000L : call_pc);
        if (i == 20) {
            recovery_meta = ras->getPredictionMeta();
        }
        auto call = create_prediction(call_pc, 0x4000 + i * 4,
                                      true, false);
        ras->specUpdateState(call);
    }

    // The 32nd call and a following return are ignored while 31 entries are
    // occupied, matching the RTL near-overflow policy.
    check_return_target(0x2000, 0x107c);
    auto blocked_call = create_prediction(0x2000, 0x5000, true, false);
    ras->specUpdateState(blocked_call);
    check_return_target(0x5000, 0x107c);

    auto blocked_return = create_prediction(0x5000, 0x107c, false, true);
    ras->specUpdateState(blocked_return);
    check_return_target(0x107c, 0x107c);

    // Rolling back to an older prediction frees speculative entries and
    // allows updates to resume.
    auto recovery = create_stream(0x1050, 0, false, false, recovery_meta);
    recovery.exeTaken = false;
    ras->recoverState(recovery);
    check_return_target(0x1050, 0x1050);

    auto resumed_call = create_prediction(0x3000, 0x6000, true, false);
    ras->specUpdateState(resumed_call);
    check_return_target(0x6000, 0x3004);
}

TEST_F(RASPopPushTest, RetainsCommittedPushAtInflightBottom) {
    reset_ras(4);

    // RTL keeps the committed push itself at BOS because younger speculative
    // entries can still name it as their parent.
    spec_and_commit_call(0x1000, 0x2000);

    check_return_target(0x2000, 0x1004);
    auto second_call = create_prediction(0x2000, 0x3000, true, false);
    ras->specUpdateState(second_call);

    check_return_target(0x3000, 0x2004);
    auto third_call = create_prediction(0x3000, 0x4000, true, false);
    ras->specUpdateState(third_call);

    // Occupancy is now three in a four-entry ring, so the RTL near-overflow
    // policy blocks another speculative update.
    check_return_target(0x4000, 0x3004);
    auto blocked_call = create_prediction(0x4000, 0x5000, true, false);
    ras->specUpdateState(blocked_call);
    check_return_target(0x5000, 0x3004);
}

TEST_F(RASPopPushTest, ReclaimsInflightPredecessorsAtCommit) {
    reset_ras(4);

    for (int i = 0; i < 3; ++i) {
        const Addr call_pc = 0x1000 + i * 4;
        check_return_target(call_pc,
            i == 0 ? 0x80000000L : call_pc);
        auto call = create_prediction(call_pc, 0x2000 + i * 4,
                                      true, false);
        ras->specUpdateState(call);
    }

    check_return_target(0x3000, 0x100c);
    auto committed_meta = ras->getPredictionMeta();

    auto blocked_call = create_prediction(0x3000, 0x4000, true, false);
    ras->specUpdateState(blocked_call);
    check_return_target(0x4000, 0x100c);

    // A later non-call commit moves BOS to one entry before its TOSW, which
    // preserves the parent link while reclaiming older queue entries.
    auto neutral_commit = create_stream(0x3000, 0, false, false,
                                        committed_meta);
    neutral_commit.exeTaken = false;
    ras->update(neutral_commit);

    auto resumed_call = create_prediction(0x4000, 0x5000, true, false);
    ras->specUpdateState(resumed_call);
    check_return_target(0x5000, 0x4004);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
