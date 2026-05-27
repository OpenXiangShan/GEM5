#include <gtest/gtest.h>

#include <vector>

#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/pairtage.hh"

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

BTBEntry
makeEntry(Addr pc, Addr target, bool is_cond, bool is_direct, bool is_indirect,
          bool is_call, bool is_return, uint8_t size = 4)
{
    BTBEntry entry;
    entry.valid = true;
    entry.pc = pc;
    entry.target = target;
    entry.isCond = is_cond;
    entry.isDirect = is_direct;
    entry.isIndirect = is_indirect;
    entry.isCall = is_call;
    entry.isReturn = is_return;
    entry.size = size;
    entry.alwaysTaken = !is_cond;
    entry.ctr = is_cond ? 0 : -1;
    return entry;
}

FetchTarget
makeTakenFetchTarget(const BTBEntry &entry, Addr final_target)
{
    FetchTarget target;
    auto branch_info = BranchInfo(entry);
    target.startPC = entry.pc & ~Addr(0x1f);
    target.predTaken = true;
    target.predBranchInfo = branch_info;
    target.predBranchInfo.target = final_target;
    target.predBTBEntries = {entry};
    return target;
}

FullBTBPrediction
makeTakenPrediction(const BTBEntry &entry, Addr final_target)
{
    FullBTBPrediction pred;
    pred.bbStart = entry.pc & ~Addr(0x1f);
    pred.btbEntries = {entry};
    if (entry.isCond) {
        pred.condTakens.push_back({entry.pc, true});
    }
    if (entry.isIndirect) {
        if (entry.isReturn) {
            pred.returnTarget = final_target;
        } else {
            pred.indirectTargets.push_back({entry.pc, final_target});
        }
    }
    return pred;
}

void
attachMeta(PairTAGE &pairtage, FetchTarget &target)
{
    std::vector<FullBTBPrediction> stage_preds(1);
    pairtage.putPCHistory(target.startPC, boost::dynamic_bitset<>(64),
                          stage_preds);
    target.predMetas[pairtage.getComponentIdx()] = pairtage.getPredictionMeta();
}

}  // namespace

TEST(PairTAGETest, FirstTrainingBlockAcceptsTakenDirectCall)
{
    PairTAGE pairtage;
    const auto entry = makeEntry(0x1040, 0x2040, false, true, false, true, false);
    const auto target = makeTakenFetchTarget(entry, 0x2040);

    auto block = pairtage.buildFirstTrainingBlockForTest(target);

    ASSERT_TRUE(block.valid);
    EXPECT_TRUE(block.taken);
    EXPECT_EQ(block.branchPC, entry.pc);
    EXPECT_EQ(block.targetPC, Addr(0x2040));
    EXPECT_FALSE(block.isCond);
    EXPECT_TRUE(block.isDirect);
    EXPECT_FALSE(block.isIndirect);
    EXPECT_TRUE(block.isCall);
    EXPECT_FALSE(block.isReturn);
    EXPECT_EQ(block.size, entry.size);
}

TEST(PairTAGETest, FirstTrainingBlockAcceptsTakenIndirectReturn)
{
    PairTAGE pairtage;
    const auto entry = makeEntry(0x1080, 0xdead, false, false, true, false, true);
    const auto target = makeTakenFetchTarget(entry, 0x3080);

    auto block = pairtage.buildFirstTrainingBlockForTest(target);

    ASSERT_TRUE(block.valid);
    EXPECT_TRUE(block.taken);
    EXPECT_EQ(block.branchPC, entry.pc);
    EXPECT_EQ(block.targetPC, Addr(0x3080));
    EXPECT_FALSE(block.isCond);
    EXPECT_FALSE(block.isDirect);
    EXPECT_TRUE(block.isIndirect);
    EXPECT_FALSE(block.isCall);
    EXPECT_TRUE(block.isReturn);
    EXPECT_EQ(block.size, entry.size);
}

TEST(PairTAGETest, StagePredictionPreservesIndirectAndReturnTargets)
{
    PairTAGE pairtage;
    PairTAGE::PairBlockInfo indirect_block(
        true, 0x1100, 0x4100, false, false, true, false, false, 4);
    PairTAGE::PairBlockInfo return_block(
        true, 0x1200, 0x4200, false, false, true, false, true, 4);

    auto indirect_pred = pairtage.buildStagePredictionForTest(indirect_block);
    auto return_pred = pairtage.buildStagePredictionForTest(return_block);

    ASSERT_EQ(indirect_pred.btbEntries.size(), 1u);
    EXPECT_TRUE(indirect_pred.btbEntries.front().isIndirect);
    EXPECT_FALSE(indirect_pred.btbEntries.front().isDirect);
    EXPECT_EQ(indirect_pred.getTarget(32), Addr(0x4100));

    ASSERT_EQ(return_pred.btbEntries.size(), 1u);
    EXPECT_TRUE(return_pred.btbEntries.front().isReturn);
    EXPECT_TRUE(return_pred.btbEntries.front().isIndirect);
    EXPECT_EQ(return_pred.getTarget(32), Addr(0x4200));
}

TEST(PairTAGETest, BlocksMatchIncludesBranchType)
{
    PairTAGE pairtage;
    PairTAGE::PairBlockInfo direct_call(
        true, 0x1300, 0x2300, false, true, false, true, false, 4);
    PairTAGE::PairBlockInfo indirect_call(
        true, 0x1300, 0x2300, false, false, true, true, false, 4);

    EXPECT_FALSE(pairtage.blocksMatchForTest(direct_call, indirect_call));
}

TEST(PairTAGETest, SecondTrainingBlockStillRejectsIndirectReturn)
{
    PairTAGE pairtage;
    const auto entry = makeEntry(0x1400, 0x5000, false, false, true, false, true);
    auto pred = makeTakenPrediction(entry, 0x5000);

    auto block = pairtage.buildSecondTrainingBlockForTest(pred);

    EXPECT_FALSE(block.valid);
}

TEST(PairTAGETest, MatchingProviderStrengthensIdentityConfidence)
{
    PairTAGE pairtage(2, 1, 512);
    const Addr start_pc = 0x2000;
    PairTAGE::PairBlockInfo block(true, 0x2008, 0x3000);
    pairtage.installEntryForTest(1, 0, start_pc, block,
                                 PairTAGE::PairBlockInfo{}, 1);

    FetchTarget target = makeTakenFetchTarget(
        pairtage.buildBTBEntryForTest(block), block.targetPC);
    attachMeta(pairtage, target);

    pairtage.trainFromActualPred(target);

    const auto &entry = pairtage.tableEntryForTest(1, 0, start_pc);
    EXPECT_EQ(entry.identityConfidence, 2);
    EXPECT_EQ(entry.firstBlock().branchPC, block.branchPC);
}

TEST(PairTAGETest, FirstIdentityMismatchAgesProviderBeforeRewrite)
{
    PairTAGE pairtage(2, 1, 512);
    const Addr start_pc = 0x2000;
    PairTAGE::PairBlockInfo old_block(true, 0x2008, 0x3000);
    PairTAGE::PairBlockInfo trained_block(true, 0x2010, 0x4000);
    pairtage.installEntryForTest(
        0, 0, start_pc, old_block, PairTAGE::PairBlockInfo{},
        PairTAGE::TageEntry::InitialIdentityConfidence);

    FetchTarget target = makeTakenFetchTarget(
        pairtage.buildBTBEntryForTest(trained_block), trained_block.targetPC);
    attachMeta(pairtage, target);

    pairtage.trainFromActualPred(target);

    const auto &entry = pairtage.tableEntryForTest(0, 0, start_pc);
    EXPECT_EQ(entry.identityConfidence, 0);
    EXPECT_EQ(entry.firstBlock().branchPC, old_block.branchPC);
}

TEST(PairTAGETest, RepeatedFirstIdentityMismatchRewritesProvider)
{
    PairTAGE pairtage(2, 1, 512);
    const Addr start_pc = 0x2000;
    PairTAGE::PairBlockInfo old_block(true, 0x2008, 0x3000);
    PairTAGE::PairBlockInfo trained_block(true, 0x2010, 0x4000);
    pairtage.installEntryForTest(0, 0, start_pc, old_block,
                                 PairTAGE::PairBlockInfo{}, 0);

    FetchTarget target = makeTakenFetchTarget(
        pairtage.buildBTBEntryForTest(trained_block), trained_block.targetPC);
    attachMeta(pairtage, target);

    pairtage.trainFromActualPred(target);

    const auto &entry = pairtage.tableEntryForTest(0, 0, start_pc);
    EXPECT_EQ(entry.identityConfidence,
              PairTAGE::TageEntry::InitialIdentityConfidence);
    EXPECT_EQ(entry.firstBlock().branchPC, trained_block.branchPC);
    EXPECT_EQ(entry.firstBlock().targetPC, trained_block.targetPC);
    EXPECT_FALSE(entry.useful);
}

TEST(PairTAGETest, TwoWayLookupChoosesBestSameTableProvider)
{
    PairTAGE pairtage(2, 2, 512);
    const Addr start_pc = 0x2000;
    PairTAGE::PairBlockInfo stale_block(true, 0x2008, 0x3000);
    PairTAGE::PairBlockInfo better_block(true, 0x2010, 0x4000);
    PairTAGE::PairBlockInfo second_block(true, 0x4004, 0x5000);
    PairTAGE::PairBlockInfo lower_block(true, 0x2008, 0x3000);

    pairtage.installEntryForTest(1, 0, start_pc, stale_block,
                                 PairTAGE::PairBlockInfo{}, 0);
    pairtage.installEntryForTest(1, 1, start_pc, better_block,
                                 second_block, 3);
    pairtage.installEntryForTest(0, 0, start_pc, lower_block,
                                 PairTAGE::PairBlockInfo{}, 1);

    auto providers = pairtage.lookupProvidersForTest(start_pc);

    ASSERT_TRUE(providers.main.found);
    EXPECT_EQ(providers.main.table, 1u);
    EXPECT_EQ(providers.main.way, 1u);
    EXPECT_EQ(providers.main.entry.firstBlock().branchPC,
              better_block.branchPC);
    EXPECT_TRUE(providers.main.entry.hasSecondBlock());

    ASSERT_TRUE(providers.alt.found);
    EXPECT_EQ(providers.alt.table, 0u);
    EXPECT_EQ(providers.alt.way, 0u);
}

TEST(PairTAGETest, TagsIgnoreBranchPositionForSameStartPc)
{
    PairTAGE pairtage(2, 2, 512);
    const Addr start_pc = 0x2000;
    PairTAGE::PairBlockInfo early_block(true, 0x2008, 0x3000);
    PairTAGE::PairBlockInfo late_block(true, 0x2010, 0x4000);

    pairtage.installEntryForTest(1, 0, start_pc, early_block);
    pairtage.installEntryForTest(1, 1, start_pc, late_block);

    const auto &way0 = pairtage.tableEntryForTest(1, 0, start_pc);
    const auto &way1 = pairtage.tableEntryForTest(1, 1, start_pc);
    EXPECT_EQ(way0.tag, way1.tag);
}

TEST(PairTAGETest, TwoWayAllocationReplacesStaleSameTagEntry)
{
    PairTAGE pairtage(2, 2, 512);
    const Addr start_pc = 0x2000;
    PairTAGE::PairBlockInfo stale_block(true, 0x2008, 0x3000);
    PairTAGE::PairBlockInfo trained_block(true, 0x2008, 0x4000);
    PairTAGE::PairBlockInfo second_block(true, 0x4004, 0x5000);

    pairtage.installEntryForTest(1, 0, start_pc, stale_block,
                                 PairTAGE::PairBlockInfo{}, 0);

    ASSERT_TRUE(pairtage.allocateEntriesForTest(start_pc, trained_block,
                                                second_block, 1));

    const auto &way0 = pairtage.tableEntryForTest(1, 0, start_pc);
    const auto &way1 = pairtage.tableEntryForTest(1, 1, start_pc);
    EXPECT_TRUE(pairtage.blocksMatchForTest(way0.firstBlock(),
                                            trained_block));
    EXPECT_TRUE(pairtage.blocksMatchForTest(way0.secondBlock(),
                                            second_block));
    EXPECT_FALSE(way1.valid);

    auto providers = pairtage.lookupProvidersForTest(start_pc);
    ASSERT_TRUE(providers.main.found);
    EXPECT_EQ(providers.main.table, 1u);
    EXPECT_EQ(providers.main.way, 0u);
    EXPECT_EQ(providers.main.entry.firstBlock().branchPC,
              trained_block.branchPC);
}

TEST(PairTAGETest, TwoWayAllocationDoesNotReplaceDifferentIdentityBeforeInvalidWay)
{
    PairTAGE pairtage(2, 2, 512);
    const Addr start_pc = 0x2000;
    PairTAGE::PairBlockInfo other_identity(true, 0x2008, 0x3000);
    PairTAGE::PairBlockInfo trained_block(true, 0x2010, 0x4000);
    PairTAGE::PairBlockInfo second_block(true, 0x4004, 0x5000);

    pairtage.installEntryForTest(1, 0, start_pc, other_identity,
                                 PairTAGE::PairBlockInfo{}, 3);

    ASSERT_TRUE(pairtage.allocateEntriesForTest(start_pc, trained_block,
                                                second_block, 1));

    const auto &way0 = pairtage.tableEntryForTest(1, 0, start_pc);
    const auto &way1 = pairtage.tableEntryForTest(1, 1, start_pc);
    EXPECT_TRUE(pairtage.blocksMatchForTest(way0.firstBlock(),
                                            other_identity));
    EXPECT_TRUE(pairtage.blocksMatchForTest(way1.firstBlock(),
                                            trained_block));
    EXPECT_TRUE(pairtage.blocksMatchForTest(way1.secondBlock(),
                                            second_block));
}

}  // namespace test

}  // namespace btb_pred

}  // namespace branch_prediction

}  // namespace gem5
