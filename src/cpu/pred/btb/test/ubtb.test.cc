#include <gtest/gtest.h>

#include <vector>

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

FullBTBPrediction
predict(UBTB &ubtb, Addr start_pc, ThreadID tid = 0, uint8_t asid_hash = 0)
{
    std::vector<FullBTBPrediction> stage_preds(1);
    stage_preds.front().tid = tid;
    stage_preds.front().asidHash = asid_hash;
    stage_preds.front().bbStart = start_pc;
    boost::dynamic_bitset<> history;
    ubtb.putPCHistory(start_pc, history, stage_preds);
    return stage_preds.front();
}

void
trainTaken(UBTB &ubtb, Addr start_pc, Addr target, ThreadID tid = 0, uint8_t asid_hash = 0)
{
    predict(ubtb, start_pc, tid, asid_hash);

    BranchInfo branch;
    branch.pc = start_pc + 4;
    branch.target = target;
    branch.size = 4;
    branch.isDirect = true;

    FullBTBPrediction s3_pred;
    s3_pred.tid = tid;
    s3_pred.asidHash = asid_hash;
    s3_pred.bbStart = start_pc;
    s3_pred.btbEntries.emplace_back(branch);
    ubtb.updateUsingS3Pred(s3_pred);
}

bool
hitsTarget(UBTB &ubtb, Addr start_pc, Addr target, ThreadID tid = 0, uint8_t asid_hash = 0)
{
    const auto pred = predict(ubtb, start_pc, tid, asid_hash);
    return pred.btbEntries.size() == 1 && pred.btbEntries.front().target == target;
}

std::vector<Addr>
findAddressesForSet(const UBTB &ubtb, unsigned set, unsigned count)
{
    std::vector<Addr> addresses;
    for (Addr pc = 0x1000; addresses.size() < count; pc += 0x20) {
        const unsigned candidate_set = ubtb.testSetIndex(pc);
        if (candidate_set == set) {
            addresses.push_back(pc);
        }
    }
    return addresses;
}

Addr
findAddressOutsideSet(const UBTB &ubtb, unsigned excluded_set)
{
    for (Addr pc = 0x1000;; pc += 0x20) {
        if (ubtb.testSetIndex(pc) != excluded_set) {
            return pc;
        }
    }
}

}  // namespace

TEST(UBTBSetAssociativeTest, DefaultCompatibleFullyAssociativeCapacity)
{
    UBTB ubtb(1, 8, 38);

    for (unsigned i = 0; i < 8; ++i) {
        const Addr start_pc = 0x1000 + i * 0x20;
        trainTaken(ubtb, start_pc, 0x8000 + i * 0x100);
    }

    EXPECT_EQ(ubtb.testValidEntriesInSet(0), 8);
    for (unsigned i = 0; i < 8; ++i) {
        const Addr start_pc = 0x1000 + i * 0x20;
        EXPECT_TRUE(hitsTarget(ubtb, start_pc, 0x8000 + i * 0x100));
    }
}

TEST(UBTBSetAssociativeTest, EvictsOnlyLruWayInSelectedSet)
{
    UBTB ubtb(4, 2, 38);
    const unsigned selected_set = ubtb.testSetIndex(0x1000);
    const auto same_set = findAddressesForSet(ubtb, selected_set, 3);
    const Addr other_set_pc = findAddressOutsideSet(ubtb, selected_set);

    trainTaken(ubtb, same_set[0], 0x8000);
    trainTaken(ubtb, same_set[1], 0x9000);
    trainTaken(ubtb, other_set_pc, 0xa000);
    ASSERT_TRUE(hitsTarget(ubtb, same_set[0], 0x8000));

    trainTaken(ubtb, same_set[2], 0xb000);

    EXPECT_TRUE(hitsTarget(ubtb, same_set[0], 0x8000));
    EXPECT_FALSE(hitsTarget(ubtb, same_set[1], 0x9000));
    EXPECT_TRUE(hitsTarget(ubtb, same_set[2], 0xb000));
    EXPECT_TRUE(hitsTarget(ubtb, other_set_pc, 0xa000));
    EXPECT_EQ(ubtb.testValidEntriesInSet(selected_set), 2);
}

TEST(UBTBSetAssociativeTest, ExistingEntryUpdateDoesNotAllocateDuplicate)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;
    const unsigned set = ubtb.testSetIndex(StartPc);

    trainTaken(ubtb, StartPc, 0x8000);
    trainTaken(ubtb, StartPc, 0x9000);

    EXPECT_TRUE(hitsTarget(ubtb, StartPc, 0x9000));
    EXPECT_EQ(ubtb.testValidEntriesInSet(set), 1);
}

TEST(UBTBSetAssociativeTest, AsidsKeepIndependentEntries)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;

    trainTaken(ubtb, StartPc, 0x8000, 0, 1);
    trainTaken(ubtb, StartPc, 0x9000, 0, 2);

    EXPECT_TRUE(hitsTarget(ubtb, StartPc, 0x8000, 0, 1));
    EXPECT_TRUE(hitsTarget(ubtb, StartPc, 0x9000, 0, 2));
}

TEST(UBTBSetAssociativeTest, SmtPartitionsWaysWithinEachSet)
{
    UBTB ubtb(2, 4, 38, true, true);
    constexpr Addr StartPc = 0x1000;
    const unsigned set = ubtb.testSetIndex(StartPc);

    trainTaken(ubtb, StartPc, 0x8000, 0);
    trainTaken(ubtb, StartPc, 0x9000, 1);

    EXPECT_TRUE(hitsTarget(ubtb, StartPc, 0x8000, 0));
    EXPECT_TRUE(hitsTarget(ubtb, StartPc, 0x9000, 1));
    EXPECT_EQ(ubtb.testValidEntriesInSet(set, 0), 1);
    EXPECT_EQ(ubtb.testValidEntriesInSet(set, 1), 1);
}

TEST(UBTBSetAssociativeTest, SupportsNonPowerOfTwoWays)
{
    UBTB ubtb(2, 3, 38);
    const unsigned selected_set = ubtb.testSetIndex(0x1000);
    const auto same_set = findAddressesForSet(ubtb, selected_set, 3);

    for (unsigned i = 0; i < same_set.size(); ++i) {
        trainTaken(ubtb, same_set[i], 0x8000 + i * 0x100);
    }

    EXPECT_EQ(ubtb.testValidEntriesInSet(selected_set), 3);
    for (unsigned i = 0; i < same_set.size(); ++i) {
        EXPECT_TRUE(hitsTarget(ubtb, same_set[i], 0x8000 + i * 0x100));
    }
}

TEST(UBTBCheckerTest, ReturnsPredictedExitAndMissFallThroughSignal)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;

    EXPECT_FALSE(ubtb.lookupForChecker(StartPc, 0, 0).valid);

    trainTaken(ubtb, StartPc, 0x8000);
    const auto hit = ubtb.lookupForChecker(StartPc, 0, 0);
    ASSERT_TRUE(hit.valid);
    EXPECT_EQ(hit.pc, StartPc + 4);
    EXPECT_EQ(hit.target, 0x8000);
}

TEST(UBTBCheckerTest, DoesNotOverwritePrimaryPredictionState)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr PrimaryPc = 0x1000;
    const Addr checkerPc = findAddressOutsideSet(
        ubtb, ubtb.testSetIndex(PrimaryPc));

    trainTaken(ubtb, PrimaryPc, 0x8000);
    trainTaken(ubtb, checkerPc, 0x9000);

    predict(ubtb, PrimaryPc);
    ASSERT_TRUE(ubtb.lookupForChecker(checkerPc, 0, 0).valid);

    BranchInfo branch;
    branch.pc = PrimaryPc + 4;
    branch.target = 0xa000;
    branch.size = 4;
    branch.isDirect = true;

    FullBTBPrediction s3Pred;
    s3Pred.bbStart = PrimaryPc;
    s3Pred.btbEntries.emplace_back(branch);
    ubtb.updateUsingS3Pred(s3Pred);

    EXPECT_TRUE(hitsTarget(ubtb, PrimaryPc, 0xa000));
    EXPECT_TRUE(hitsTarget(ubtb, checkerPc, 0x9000));
}

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5
