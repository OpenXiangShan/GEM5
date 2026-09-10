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

BTBEntry
makeSlot(Addr pc, Addr target, bool conditional = true, int counter = -1)
{
    BTBEntry slot;
    slot.valid = true;
    slot.pc = pc;
    slot.target = target;
    slot.size = 4;
    slot.isCond = conditional;
    slot.isDirect = true;
    slot.ctr = counter;
    return slot;
}

void
fillLayout(UBTB &ubtb, Addr start_pc, const std::vector<BTBEntry> &slots)
{
    predict(ubtb, start_pc);
    FullBTBPrediction pred;
    pred.bbStart = start_pc;
    pred.btbEntries = slots;
    ubtb.updateUsingS3Pred(pred);
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

TEST(UBTBCheckerTest, ReturnsLayoutAndExplicitMiss)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;

    EXPECT_FALSE(ubtb.lookupForChecker(StartPc, 0, 0).valid);

    trainTaken(ubtb, StartPc, 0x8000);
    const auto hit = ubtb.lookupForChecker(StartPc, 0, 0);
    ASSERT_TRUE(hit.valid);
    ASSERT_TRUE(hit.usable());
    ASSERT_EQ(hit.slots.size(), 1);
    EXPECT_EQ(hit.slots.front().pc, StartPc + 4);
    EXPECT_EQ(hit.slots.front().target, 0x8000);
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

TEST(UBTBLayoutTest, PreservesAllSlotsAndSelectsFirstTaken)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;
    const auto first = makeSlot(0x1004, 0x2000, true, -2);
    const auto second = makeSlot(0x1008, 0x3000, true, 0);
    const auto third = makeSlot(0x100c, 0x4000, true, 1);
    const auto jump = makeSlot(0x1010, 0x5000, false);
    fillLayout(ubtb, StartPc, {jump, third, first, second});

    const auto layout = ubtb.lookupForChecker(StartPc, 0, 0);
    ASSERT_TRUE(layout.usable());
    ASSERT_EQ(layout.slots.size(), 4);
    EXPECT_EQ(layout.slots[0].pc, first.pc);
    EXPECT_EQ(layout.slots[0].ctr, -2);
    EXPECT_EQ(layout.slots[1].ctr, 0);
    EXPECT_EQ(layout.slots[2].ctr, 1);
    EXPECT_EQ(layout.slots[3].pc, jump.pc);
    EXPECT_EQ(layout.getTakenEntry().pc, second.pc);

    auto pred = predict(ubtb, StartPc);
    EXPECT_EQ(pred.btbEntries.size(), 4);
    EXPECT_EQ(pred.getTakenEntry().pc, second.pc);
    EXPECT_EQ(pred.getTarget(64), second.target);
    EXPECT_EQ(pred.getGHistUpdate().shamt, 2);
}

TEST(UBTBLayoutTest, KeepsNotTakenAndEmptyLayoutsAsHits)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;
    EXPECT_FALSE(ubtb.lookupForChecker(StartPc, 0, 0).usable());
    fillLayout(ubtb, StartPc,
               {makeSlot(0x1004, 0x2000), makeSlot(0x1008, 0x3000)});
    auto pred = predict(ubtb, StartPc);
    EXPECT_FALSE(pred.isTaken());
    EXPECT_EQ(pred.getTarget(64), 0x1040);
    EXPECT_EQ(pred.getGHistUpdate().shamt, 2);
    ASSERT_EQ(ubtb.lookupForChecker(StartPc, 0, 0).slots.size(), 2);

    fillLayout(ubtb, StartPc, {});
    const auto empty = ubtb.lookupForChecker(StartPc, 0, 0);
    EXPECT_TRUE(empty.usable());
    EXPECT_TRUE(empty.slots.empty());
    EXPECT_EQ(ubtb.testValidEntriesInSet(ubtb.testSetIndex(StartPc)), 1);
    pred = predict(ubtb, StartPc);
    EXPECT_FALSE(pred.isTaken());
    EXPECT_EQ(pred.getGHistUpdate().shamt, 0);
}

TEST(UBTBLayoutTest, OverflowCannotSupplyAPartialPrediction)
{
    UBTB ubtb(4, 2, 38, true, false, 2);
    constexpr Addr StartPc = 0x1000;
    fillLayout(ubtb, StartPc, {makeSlot(0x1004, 0x2000, true, 1),
                              makeSlot(0x1008, 0x3000),
                              makeSlot(0x100c, 0x4000)});
    const auto overflow = ubtb.lookupForChecker(StartPc, 0, 0);
    EXPECT_TRUE(overflow.valid);
    EXPECT_TRUE(overflow.overflow);
    EXPECT_FALSE(overflow.usable());
    EXPECT_EQ(overflow.slots.size(), 2);
    EXPECT_FALSE(overflow.getTakenEntry().valid);
    auto pred = predict(ubtb, StartPc);
    EXPECT_TRUE(pred.btbEntries.empty());
    EXPECT_FALSE(pred.isTaken());

    fillLayout(ubtb, StartPc, {makeSlot(0x1004, 0x2000, true, 1)});
    const auto complete = ubtb.lookupForChecker(StartPc, 0, 0);
    EXPECT_TRUE(complete.usable());
    EXPECT_FALSE(complete.overflow);
    EXPECT_EQ(complete.getTakenEntry().pc, 0x1004);
    EXPECT_EQ(ubtb.testValidEntriesInSet(ubtb.testSetIndex(StartPc)), 1);
}

TEST(UBTBLayoutTest, FiltersWindowAndDeduplicatesBeforeOverflow)
{
    UBTB ubtb(4, 2, 38, true, false, 2);
    constexpr Addr StartPc = 0x1010;
    const auto first = makeSlot(0x1014, 0x2000);
    const auto last = makeSlot(0x103e, 0x3000, true, 1);
    auto invalid = makeSlot(0x1018, 0x4000);
    invalid.valid = false;
    fillLayout(ubtb, StartPc,
               {makeSlot(0x100c, 0x5000), last, first, invalid, first,
                makeSlot(0x1040, 0x6000)});
    const auto layout = ubtb.lookupForChecker(StartPc, 0, 0);
    ASSERT_TRUE(layout.usable());
    ASSERT_EQ(layout.slots.size(), 2);
    EXPECT_EQ(layout.slots.front().pc, first.pc);
    EXPECT_EQ(layout.slots.back().pc, last.pc);
}

TEST(UBTBLayoutTest, UpdatesBaseCountersWithoutTakingFinalDirections)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;
    fillLayout(ubtb, StartPc, {makeSlot(0x1004, 0x2000, true, 1)});

    FullBTBPrediction s3;
    s3.bbStart = StartPc;
    s3.btbEntries = {makeSlot(0x1004, 0x2000, true, -2),
                     makeSlot(0x1008, 0x3000, true, 0)};
    // TAGE overrides the base directions in S3; the layout keeps the bases.
    s3.condTakens = {{0x1004, true}, {0x1008, false}};
    ubtb.updateUsingS3Pred(s3);
    auto pred = predict(ubtb, StartPc);
    ASSERT_EQ(pred.btbEntries.size(), 2);
    EXPECT_EQ(pred.btbEntries.front().ctr, -2);
    EXPECT_EQ(pred.getTakenEntry().pc, 0x1008);
}

TEST(UBTBLayoutTest, PreservesIndirectAndReturnTargets)
{
    UBTB ubtb(4, 2, 38);
    constexpr Addr StartPc = 0x1000;
    auto indirect = makeSlot(0x1008, 0x3000, false);
    indirect.isDirect = false;
    indirect.isIndirect = true;
    fillLayout(ubtb, StartPc, {makeSlot(0x1004, 0x2000), indirect});
    auto pred = predict(ubtb, StartPc);
    EXPECT_EQ(pred.getTarget(64), 0x3000);

    indirect.isReturn = true;
    indirect.size = 2;
    auto youngerReturn = indirect;
    youngerReturn.pc = 0x1010;
    youngerReturn.target = 0x4000;
    fillLayout(ubtb, StartPc,
               {makeSlot(0x1004, 0x2000), youngerReturn, indirect});
    pred = predict(ubtb, StartPc);
    EXPECT_EQ(pred.getTarget(64), 0x3000);
    EXPECT_EQ(pred.getTakenEntry().size, 2);
    EXPECT_TRUE(pred.indirectTargets.empty());
}

TEST(UBTBLayoutTest, DistinguishesOverlappingBlockStarts)
{
    UBTB ubtb(1, 4, 38);
    fillLayout(ubtb, 0x1000, {makeSlot(0x1004, 0x2000)});
    fillLayout(ubtb, 0x1002, {});
    EXPECT_EQ(ubtb.lookupForChecker(0x1000, 0, 0).slots.size(), 1);
    const auto empty = ubtb.lookupForChecker(0x1002, 0, 0);
    EXPECT_TRUE(empty.usable());
    EXPECT_TRUE(empty.slots.empty());
    EXPECT_FALSE(ubtb.lookupForChecker(0x1001, 0, 0).valid);
}

TEST(UBTBLayoutTest, BackendTrainingRetainsSlotsAfterTakenExit)
{
    UBTB ubtb(4, 2, 38, false);
    constexpr Addr StartPc = 0x1000;
    predict(ubtb, StartPc);
    FetchTarget stream;
    stream.startPC = StartPc;
    stream.predMetas[ubtb.getComponentIdx()] = ubtb.getPredictionMeta();
    stream.predBTBEntries = {makeSlot(0x1004, 0x2000, true, 1),
                             makeSlot(0x1008, 0x3000),
                             makeSlot(0x100c, 0x4000, true, -2)};
    stream.exeTaken = true;
    stream.exeBranchInfo = stream.predBTBEntries[1];
    stream.updateEndInstPC = 0x1008;
    ubtb.update(stream);
    const auto layout = ubtb.lookupForChecker(StartPc, 0, 0);
    ASSERT_TRUE(layout.usable());
    ASSERT_EQ(layout.slots.size(), 3);
    EXPECT_EQ(layout.slots[0].ctr, 0);
    EXPECT_EQ(layout.slots[1].ctr, 0);
    EXPECT_EQ(layout.slots[2].ctr, -2);
}

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5
