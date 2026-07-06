#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "cpu/pred/btb/btb_ubtb.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{

namespace
{

ResolvedBranch
makeTakenBranch(Addr pc, Addr target, bool isCond = true)
{
    ResolvedBranch branch;
    branch.pc = pc;
    branch.target = target;
    branch.taken = true;
    branch.isCond = isCond;
    branch.isDirect = true;
    branch.isIndirect = false;
    branch.isCall = false;
    branch.isReturn = false;
    branch.size = 4;
    return branch;
}

FullBTBPrediction
predict(UBTB &ubtb, Addr startPC)
{
    std::vector<FullBTBPrediction> stagePreds(2);
    for (auto &pred : stagePreds) {
        pred.bbStart = startPC;
    }

    ubtb.putPCHistory(startPC, boost::dynamic_bitset<>(8, 0), stagePreds);
    return stagePreds[ubtb.getDelay()];
}

} // namespace

TEST(UBTBTest, ActualUpdateCanAllocateWithoutPredictionMeta)
{
    UBTB ubtb(16, 20, 0, false);

    const Addr startPC = 0x2000;
    const Addr branchPC = 0x2004;
    const Addr target = 0x3000;

    BranchUpdateContext ctx;
    ctx.startPC = startPC;
    const auto branches =
        std::vector<ResolvedBranch>{makeTakenBranch(branchPC, target)};

    ubtb.updateWithBranchUpdateContext(ctx, branches, nullptr);

    const auto pred = predict(ubtb, startPC);

    ASSERT_EQ(pred.btbEntries.size(), 1);
    EXPECT_EQ(pred.btbEntries[0].pc, branchPC);
    EXPECT_EQ(pred.btbEntries[0].target, target);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
