#include <gtest/gtest.h>

#include "cpu/pred/btb/ras.hh"

namespace gem5::branch_prediction::btb_pred::test
{

namespace
{

FullBTBPrediction
predict(BTBRAS &ras, Addr pc)
{
    std::vector<FullBTBPrediction> preds(1);
    ras.putPCHistory(pc, boost::dynamic_bitset<>(8, 0), preds);
    return preds[0];
}

FullBTBPrediction
callPrediction(Addr pc)
{
    FullBTBPrediction pred;
    pred.bbStart = pc;
    BTBEntry entry;
    entry.valid = true;
    entry.pc = pc;
    entry.target = pc + 0x100;
    entry.size = 4;
    entry.isDirect = true;
    entry.isCall = true;
    pred.btbEntries.push_back(entry);
    return pred;
}

BranchOutcome
callOutcome(Addr pc)
{
    BranchOutcome outcome;
    outcome.pc = pc;
    outcome.target = pc + 0x100;
    outcome.taken = true;
    outcome.isDirect = true;
    outcome.isCall = true;
    outcome.size = 4;
    return outcome;
}

} // anonymous namespace

TEST(BTBRASFlushTest, RestoresSpeculativeAndCommittedStacks)
{
    BTBRAS ras(16, 2, 8);
    EXPECT_EQ(predict(ras, 0x1000).returnTarget, 0x80000000L);

    auto call = callPrediction(0x1000);
    auto meta = ras.getPredictionMeta();
    ras.specUpdateState(call);
    EXPECT_EQ(predict(ras, 0x2000).returnTarget, 0x1004);

    FetchTarget committed;
    committed.startPC = 0x1000;
    committed.predMetas[0] = meta;
    const PredictionUpdateContext context(committed);
    const PreparedUpdate update({callOutcome(0x1000)});
    ras.update(context, update);

    auto speculative = callPrediction(0x2000);
    ras.specUpdateState(speculative);
    EXPECT_EQ(predict(ras, 0x3000).returnTarget, 0x2004);

    ras.flushState();

    EXPECT_EQ(predict(ras, 0x1000).returnTarget, 0x80000000L);
}

} // namespace gem5::branch_prediction::btb_pred::test
