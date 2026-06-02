#include <gtest/gtest.h>

#include "cpu/pred/btb/common.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace
{

using PredictionResult =
    PredictionBlockResultView<std::vector<BTBEntry>, CondTakens,
                              IndirectTargets>;

BranchSlot
makeSlot(Addr pc, Addr target, bool is_cond, bool is_indirect,
         bool is_call = false, bool is_return = false, uint8_t size = 4)
{
    BranchSlot slot;
    slot.pc = pc;
    slot.target = target;
    slot.setTypeFromFlags(is_cond, is_indirect, !is_cond && !is_indirect,
                          is_call, is_return);
    slot.size = size;
    return slot;
}

BTBEntry
makeEntry(Addr pc, Addr target, bool is_cond = false,
          bool is_indirect = false, bool is_call = false,
          bool is_return = false)
{
    return BTBEntry(
        makeSlot(pc, target, is_cond, is_indirect, is_call, is_return));
}

PredictionResult
makeResult(Addr bb_start, const std::vector<BTBEntry> &entries,
           const CondTakens &cond_takens,
           const IndirectTargets &indirect_targets, Addr return_target = 0)
{
    return PredictionResult(
        bb_start, entries, cond_takens, indirect_targets, return_target);
}

TEST(PredictionBlockResultTest, NotTakenConditionalFallsThrough)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1020, 0x1100, true),
    };
    const CondTakens cond_takens = {{0x1020, false}};
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1004, entries, cond_takens, indirect_targets);

    EXPECT_FALSE(result.isTaken());
    EXPECT_EQ(result.getTarget(64), 0x1040);
    EXPECT_EQ(result.getEnd(64), 0x1040);
    EXPECT_EQ(result.controlAddr(), 0);
    const auto phist = result.getPHistUpdate();
    EXPECT_FALSE(phist.taken);
    EXPECT_EQ(phist.pc, 0);
    EXPECT_EQ(phist.target, 0);
}

TEST(PredictionBlockResultTest, TakenConditionalUsesBranchTarget)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1020, 0x1100, true),
    };
    const CondTakens cond_takens = {{0x1020, true}};
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);
    const auto taken = result.getTakenSlotResult(64);

    EXPECT_TRUE(taken.taken());
    EXPECT_EQ(taken.controlPC(), 0x1020);
    EXPECT_EQ(taken.target, 0x1100);
    EXPECT_EQ(taken.endPC(), 0x1024);
    EXPECT_EQ(result.getTarget(64), 0x1100);
    EXPECT_EQ(result.getEnd(64), 0x1024);
}

TEST(PredictionBlockResultTest, EarlierTakenEntryWins)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1010, 0x1200, true),
        makeEntry(0x1020, 0x1300),
    };
    const CondTakens cond_takens = {{0x1010, true}};
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);

    EXPECT_EQ(result.controlAddr(), 0x1010);
    EXPECT_EQ(result.getTarget(64), 0x1200);
}

TEST(PredictionBlockResultTest, LaterUnconditionalWinsAfterNotTakenCond)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1010, 0x1200, true),
        makeEntry(0x1020, 0x1300),
    };
    const CondTakens cond_takens = {{0x1010, false}};
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);

    EXPECT_EQ(result.controlAddr(), 0x1020);
    EXPECT_EQ(result.getTarget(64), 0x1300);
}

TEST(PredictionBlockResultTest, IndirectTargetOverrideUsesIpredWhenPresent)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1020, 0x1100, false, true),
    };
    const CondTakens cond_takens;
    const IndirectTargets indirect_targets = {{0x1020, 0x2200}};

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);
    const auto slot = result.getTakenSlotResult(64).resolvedSlot();

    EXPECT_EQ(result.getTarget(64), 0x2200);
    const auto phist = result.getPHistUpdate();
    EXPECT_TRUE(phist.taken);
    EXPECT_EQ(phist.pc, 0x1020);
    EXPECT_EQ(phist.target, 0x2200);
    EXPECT_EQ(slot.pc, 0x1020);
    EXPECT_EQ(slot.target, 0x2200);
}

TEST(PredictionBlockResultTest, IndirectTargetFallsBackToBtbTarget)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1020, 0x1100, false, true),
    };
    const CondTakens cond_takens;
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);

    EXPECT_EQ(result.getTarget(64), 0x1100);
    const auto phist = result.getPHistUpdate();
    EXPECT_TRUE(phist.taken);
    EXPECT_EQ(phist.pc, 0x1020);
    EXPECT_EQ(phist.target, 0x1100);
}

TEST(PredictionBlockResultTest, ReturnAlwaysUsesRasTarget)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1020, 0x1100, false, true, false, true),
    };
    const CondTakens cond_takens;
    const IndirectTargets indirect_targets = {{0x1020, 0x2200}};

    const auto result = makeResult(0x1000, entries, cond_takens,
                                   indirect_targets, 0x3300);

    EXPECT_EQ(result.getTarget(64), 0x3300);
    const auto phist = result.getPHistUpdate();
    EXPECT_TRUE(phist.taken);
    EXPECT_EQ(phist.pc, 0x1020);
    EXPECT_EQ(phist.target, 0x3300);
}

TEST(PredictionBlockResultTest, HistoryCountsNotTakenCondsBeforeWinner)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1008, 0x1100, true),
        makeEntry(0x1010, 0x1200, true),
        makeEntry(0x1020, 0x1300),
    };
    const CondTakens cond_takens = {{0x1008, false}, {0x1010, true}};
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);

    const auto ghist = result.getGHistUpdate();
    EXPECT_EQ(ghist.shamt, 2);
    EXPECT_TRUE(ghist.taken);
}

TEST(PredictionBlockResultTest, UnconditionalStopsHistoryWithoutIncrement)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1008, 0x1100, true),
        makeEntry(0x1010, 0x1200),
        makeEntry(0x1020, 0x1300, true),
    };
    const CondTakens cond_takens = {{0x1008, false}, {0x1020, true}};
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);

    const auto ghist = result.getGHistUpdate();
    EXPECT_EQ(ghist.shamt, 1);
    EXPECT_FALSE(ghist.taken);
}

TEST(PredictionBlockResultTest, BackwardHistoryUsesRawBtbTarget)
{
    const std::vector<BTBEntry> entries = {
        makeEntry(0x1020, 0x1010, true),
    };
    const CondTakens cond_takens = {{0x1020, true}};
    const IndirectTargets indirect_targets;

    const auto result =
        makeResult(0x1000, entries, cond_takens, indirect_targets);

    const auto bwhist = result.getBwHistUpdate();
    EXPECT_EQ(bwhist.shamt, 1);
    EXPECT_TRUE(bwhist.taken);
}

TEST(PredictionBlockResultTest, MatchPreservesOverridePriority)
{
    const std::vector<BTBEntry> not_taken_entries = {
        makeEntry(0x1010, 0x1200, true),
    };
    const std::vector<BTBEntry> taken_entries = {
        makeEntry(0x1010, 0x1200, true),
    };
    const std::vector<BTBEntry> different_control_entries = {
        makeEntry(0x1020, 0x1200),
    };
    const std::vector<BTBEntry> different_target_entries = {
        makeEntry(0x1010, 0x1300, true),
    };
    const CondTakens not_taken_cond = {{0x1010, false}};
    const CondTakens taken_cond = {{0x1010, true}};
    const CondTakens empty_cond;
    const IndirectTargets indirect_targets;

    const auto not_taken =
        makeResult(0x1000, not_taken_entries, not_taken_cond,
                   indirect_targets);
    const auto taken =
        makeResult(0x1000, taken_entries, taken_cond, indirect_targets);
    const auto different_control =
        makeResult(0x1000, different_control_entries, empty_cond,
                   indirect_targets);
    const auto different_target =
        makeResult(0x1000, different_target_entries, taken_cond,
                   indirect_targets);

    EXPECT_EQ(not_taken.match(taken, 64).reason,
              PredictionResultMismatchReason::FallThrough);
    EXPECT_EQ(taken.match(different_control, 64).reason,
              PredictionResultMismatchReason::ControlAddr);
    EXPECT_EQ(taken.match(different_target, 64).reason,
              PredictionResultMismatchReason::Target);
    EXPECT_TRUE(taken.match(taken, 64).matches);
}

} // namespace

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
