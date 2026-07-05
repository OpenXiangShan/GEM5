#include <gtest/gtest.h>

#include "cpu/pred/btb/common.hh"

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
makeEntry(Addr pc, bool is_cond, bool resolved)
{
    BTBEntry entry;
    entry.valid = true;
    entry.pc = pc;
    entry.target = pc + 0x100;
    entry.resolved = resolved;
    entry.isCond = is_cond;
    entry.isIndirect = false;
    entry.isDirect = !is_cond;
    entry.isCall = false;
    entry.isReturn = false;
    entry.size = 4;
    entry.ctr = 0;
    return entry;
}

BTBEntry
makeIndirectEntry(Addr pc, bool is_return, bool resolved)
{
    BTBEntry entry = makeEntry(pc, false, resolved);
    entry.isIndirect = true;
    entry.isDirect = false;
    entry.isReturn = is_return;
    return entry;
}

ResolvedBranch
makeResolvedBranch(Addr pc, bool taken, bool mispred)
{
    ResolvedBranch branch;
    branch.pc = pc;
    branch.target = pc + 0x200;
    branch.taken = taken;
    branch.mispred = mispred;
    branch.isCond = true;
    branch.isDirect = true;
    branch.size = 4;
    return branch;
}

std::vector<BTBEntry>
makeUpdateEntries(const FetchTarget &stream,
                  unsigned predict_width,
                  const std::vector<ResolvedBranch> &branches)
{
    const auto ctx = makeActualBranchUpdateContext(
        makeBaseBranchUpdateContext(stream), branches);
    const auto update_end_inst_pc = buildUpdateEndInstPC(
        ctx.startPC, ctx.squashType, ctx.squashPC, branches,
        predict_width);
    return makeUpdateBTBEntries(
        stream.predBTBEntries, ctx.startPC, update_end_inst_pc);
}

} // namespace

TEST(UpdateEntryBuilderTest, DirectionUpdateBranchPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x1000, true, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x1004, true, true);

    const auto entries = buildDirectionUpdateEntries(
        {prefix_entry, legacy_resolved_entry},
        {makeResolvedBranch(prefix_entry.pc, false, false)},
        DirectionUpdateEntryFilter::Conditional, true);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].branch.pc, prefix_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionNewNotTakenEntryKeepsActualOutcome)
{
    const BTBEntry new_entry = makeEntry(0x1010, true, false);

    const auto entries = buildDirectionUpdateEntries(
        {}, {makeResolvedBranch(new_entry.pc, false, false)},
        DirectionUpdateEntryFilter::Conditional, false);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].branch.pc, new_entry.pc);
    EXPECT_TRUE(entries[0].baseTaken);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionEntryKeepsBaseDirection)
{
    BTBEntry entry = makeEntry(0x1014, true, false);
    entry.ctr = -1;

    const auto entries = buildDirectionUpdateEntries(
        {entry}, {makeResolvedBranch(entry.pc, true, false)},
        DirectionUpdateEntryFilter::Conditional, false);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].branch.pc, entry.pc);
    EXPECT_FALSE(entries[0].baseTaken);
    EXPECT_TRUE(entries[0].actualTaken);
}

TEST(UpdateEntryBuilderTest, DirectionResolvedBranchOutcomeOverridesContext)
{
    const BTBEntry entry = makeEntry(0x1018, true, false);

    const auto entries = buildDirectionUpdateEntries(
        {entry}, {makeResolvedBranch(entry.pc, true, true)},
        DirectionUpdateEntryFilter::Conditional, true);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].branch.pc, entry.pc);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].actualMispred);
}

TEST(UpdateEntryBuilderTest, MgscResolvedUpdateRequiresResolvedBranchSet)
{
    const BTBEntry cond_entry = makeEntry(0x1020, true, false);

    const auto entries = buildDirectionUpdateEntries(
        {cond_entry}, {},
        DirectionUpdateEntryFilter::Mgsc, true);

    EXPECT_TRUE(entries.empty());
}

TEST(UpdateEntryBuilderTest, TargetUpdateBranchPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x2000, false, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x2004, false, true);

    const auto entries = buildTargetUpdateEntries(
        {prefix_entry, legacy_resolved_entry},
        {makeResolvedBranch(prefix_entry.pc, false, false)},
        TargetUpdateEntryFilter::Any, true);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, prefix_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, TargetFilterKeepsIndirectNonReturnOnly)
{
    const BTBEntry indirect = makeIndirectEntry(0x3000, false, true);
    const BTBEntry ret = makeIndirectEntry(0x3004, true, true);
    ResolvedBranch resolved = makeResolvedBranch(indirect.pc, true, false);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;

    const auto entries = buildTargetUpdateEntries(
        {indirect, ret}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn, false);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, indirect.pc);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].actualMispred);
    EXPECT_FALSE(entries[0].isNewEntry);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_EQ(entries[0].actualBranch.target, resolved.target);
}

TEST(UpdateEntryBuilderTest, TargetEntriesCarryPerEntryActualBranch)
{
    BTBEntry first = makeIndirectEntry(0x3000, false, true);
    first.target = 0x4440;
    BTBEntry second = makeIndirectEntry(0x3008, false, true);
    second.target = 0x5550;

    const Addr actual_target = 0xdead;
    ResolvedBranch resolved = makeResolvedBranch(second.pc, true, false);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;
    resolved.target = actual_target;

    const auto entries = buildTargetUpdateEntries(
        {first, second}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn, false);

    ASSERT_EQ(entries.size(), 2);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_EQ(entries[0].actualBranch.pc, first.pc);
    EXPECT_EQ(entries[0].actualBranch.target, first.target);
    EXPECT_TRUE(entries[1].actualTaken);
    EXPECT_EQ(entries[1].actualBranch.pc, second.pc);
    EXPECT_EQ(entries[1].actualBranch.target, actual_target);
}

TEST(UpdateEntryBuilderTest, TargetResolvedBranchCarriesPerEntryActualTarget)
{
    const BTBEntry indirect = makeIndirectEntry(0x3010, false, true);

    ResolvedBranch resolved = makeResolvedBranch(indirect.pc, true, true);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;
    resolved.target = 0xbeef;

    const auto entries = buildTargetUpdateEntries(
        {indirect}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn, true);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].actualMispred);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_EQ(entries[0].actualBranch.target, resolved.target);
}

TEST(UpdateEntryBuilderTest, TargetTakenControlKeepsOnlyActualControl)
{
    const BTBEntry first = makeEntry(0x3018, false, true);
    const BTBEntry control = makeEntry(0x3020, false, true);
    const Addr actual_target = 0xdead;
    ResolvedBranch resolved = makeResolvedBranch(control.pc, true, false);
    resolved.isCond = false;
    resolved.target = actual_target;

    const auto entries = buildTargetUpdateEntries(
        {first, control}, {resolved},
        TargetUpdateEntryFilter::TakenControl, false);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, control.pc);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].actualMispred);
    EXPECT_EQ(entries[0].actualBranch.pc, control.pc);
    EXPECT_EQ(entries[0].actualBranch.target, actual_target);
}

TEST(UpdateEntryBuilderTest, TargetTakenControlCanBuildActualEntry)
{
    const Addr branch_pc = 0x3028;
    const Addr actual_target = 0xbeef;
    ResolvedBranch resolved = makeResolvedBranch(branch_pc, true, false);
    resolved.isCond = false;
    resolved.target = actual_target;

    const auto entries = buildTargetUpdateEntries(
        {}, {resolved},
        TargetUpdateEntryFilter::TakenControl, false);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, branch_pc);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].isNewEntry);
    EXPECT_EQ(entries[0].actualBranch.target, actual_target);
}

TEST(UpdateEntryBuilderTest, TargetTakenControlFallsThroughWithoutEntry)
{
    const BTBEntry predicted = makeEntry(0x3030, false, true);

    const auto entries = buildTargetUpdateEntries(
        {predicted}, {},
        TargetUpdateEntryFilter::TakenControl, false);

    EXPECT_TRUE(entries.empty());
}

TEST(UpdateEntryBuilderTest, UpdatedTargetEntryUsesExistingCondCounter)
{
    BTBEntry requested = makeEntry(0x3020, true, true);
    requested.ctr = -2;
    requested.tag = 0x10;
    BTBEntry existing = requested;
    existing.ctr = 0;
    existing.tag = 0x20;

    const TargetUpdateEntry update{
        requested, true, false, BranchInfo(requested)};
    const auto written = buildUpdatedTargetEntry(update, &existing, 0x30);

    EXPECT_EQ(written.pc, requested.pc);
    EXPECT_EQ(written.ctr, 1);
    EXPECT_EQ(written.tag, 0x30);
    EXPECT_FALSE(written.resolved);
}

TEST(UpdateEntryBuilderTest, UpdatedTargetEntryUsesActualIndirectTarget)
{
    BTBEntry indirect = makeIndirectEntry(0x3030, false, true);
    indirect.target = 0x4000;
    BranchInfo actual_branch(indirect);
    actual_branch.target = 0x5000;

    const TargetUpdateEntry update{
        indirect, true, false, actual_branch};
    const auto written = buildUpdatedTargetEntry(update, nullptr, 0x40);

    EXPECT_EQ(written.pc, indirect.pc);
    EXPECT_EQ(written.target, actual_branch.target);
    EXPECT_EQ(written.tag, 0x40);
    EXPECT_FALSE(written.resolved);
}

TEST(UpdateEntryBuilderTest, UpdateEndInstPCUsesActualTakenOrSquashBoundary)
{
    const auto taken = makeResolvedBranch(0x1010, true, false);
    EXPECT_EQ(buildUpdateEndInstPC(
        0x1000, SquashType::SQUASH_NONE, 0, {taken}, 32), 0x1010);

    const auto not_taken = makeResolvedBranch(0x1010, false, false);
    EXPECT_EQ(buildUpdateEndInstPC(
        0x1004, SquashType::SQUASH_NONE, 0, {not_taken}, 32), 0x1020);

    const auto mispred = makeResolvedBranch(0x1008, false, true);
    EXPECT_EQ(buildUpdateEndInstPC(
        0x1000, SquashType::SQUASH_CTRL, 0x1008, {mispred}, 32),
        0x1008);
}

TEST(UpdateEntryBuilderTest, UpdateEndInstPCUsesFirstTakenActualBranch)
{
    const auto later_taken = makeResolvedBranch(0x1100, true, false);
    const auto not_taken = makeResolvedBranch(0x1000, false, false);
    const auto first_taken = makeResolvedBranch(0x1080, true, false);

    EXPECT_EQ(buildUpdateEndInstPC(
        0x1000, SquashType::SQUASH_NONE, 0,
        {later_taken, not_taken, first_taken}, 32), first_taken.pc);
}

TEST(UpdateEntryBuilderTest, UpdateBTBEntriesKeepsValidPrefix)
{
    const BTBEntry before = makeEntry(0x0ffc, true, true);
    const BTBEntry first = makeEntry(0x1000, true, true);
    const BTBEntry second = makeEntry(0x1008, true, true);
    const BTBEntry after = makeEntry(0x1010, true, true);
    BTBEntry invalid = makeEntry(0x1004, true, true);
    invalid.valid = false;

    const auto entries = buildUpdateBTBEntries(
        {before, first, invalid, second, after}, 0x1000, 0x1008);

    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].pc, first.pc);
    EXPECT_EQ(entries[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, ResolvedBranchesBuildUpdateInputs)
{
    const BTBEntry first = makeEntry(0x1000, true, false);
    const BTBEntry second = makeEntry(0x1008, true, false);
    const BTBEntry after = makeEntry(0x1010, true, false);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {first, second, after};

    const ResolvedBranch resolved_second =
        makeResolvedBranch(second.pc, true, true);
    const auto update_branches = makeUpdateBranchPrefix({
        makeResolvedBranch(first.pc, false, false),
        resolved_second,
        makeResolvedBranch(after.pc, false, false),
    });
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto update_ctx =
        makeActualBranchUpdateContext(
            makeBaseBranchUpdateContext(stream), update_branches);
    const auto *summary_branch =
        findActualUpdateSummaryBranch(update_branches);

    EXPECT_FALSE(stream.resolved);
    ASSERT_NE(summary_branch, nullptr);
    EXPECT_TRUE(summary_branch->taken);
    EXPECT_EQ(summary_branch->pc, second.pc);
    EXPECT_EQ(summary_branch->target, second.pc + 0x200);
    EXPECT_EQ(update_ctx.squashType, SquashType::SQUASH_CTRL);
    EXPECT_EQ(update_ctx.squashPC, second.pc);

    ASSERT_EQ(update_btb_entries.size(), 2);
    EXPECT_EQ(update_btb_entries[0].pc, first.pc);
    EXPECT_EQ(update_btb_entries[1].pc, second.pc);
    EXPECT_FALSE(update_btb_entries[0].resolved);
    EXPECT_FALSE(update_btb_entries[1].resolved);
    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, first.pc);
    EXPECT_EQ(update_branches[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, BranchUpdateContextUsesUpdateBranchPrefixForSquash)
{
    const ResolvedBranch first =
        makeResolvedBranch(0x1000, false, false);
    const ResolvedBranch second =
        makeResolvedBranch(0x1008, true, true);

    FetchTarget stream;
    stream.tid = 1;
    stream.asidHash = 3;
    stream.startPC = 0x1000;
    stream.predTick = 42;
    stream.exeTaken = false;
    stream.exeBranchInfo = makeBranchInfo(
        makeResolvedBranch(0x1080, false, false));

    const auto update_branches =
        makeUpdateBranchPrefix({first, second});
    const auto ctx = makeActualBranchUpdateContext(
        makeBaseBranchUpdateContext(stream), update_branches);
    const auto *summary_branch =
        findActualUpdateSummaryBranch(update_branches);

    EXPECT_FALSE(stream.resolved);
    EXPECT_FALSE(stream.exeTaken);
    EXPECT_EQ(ctx.tid, stream.tid);
    EXPECT_EQ(ctx.asidHash, stream.asidHash);
    EXPECT_EQ(ctx.startPC, stream.startPC);
    EXPECT_EQ(ctx.predTick, stream.predTick);
    ASSERT_NE(summary_branch, nullptr);
    EXPECT_TRUE(summary_branch->taken);
    EXPECT_EQ(summary_branch->pc, second.pc);
    EXPECT_EQ(summary_branch->target, second.target);
    EXPECT_EQ(ctx.squashType, SquashType::SQUASH_CTRL);
    EXPECT_EQ(ctx.squashPC, second.pc);

    EXPECT_NE(summary_branch->pc, stream.exeBranchInfo.pc);
}

TEST(UpdateEntryBuilderTest, BaseBranchUpdateContextKeepsPredictionContextOnly)
{
    FetchTarget stream;
    stream.tid = 2;
    stream.asidHash = 4;
    stream.startPC = 0x2000;
    stream.predTick = 24;
    stream.resolved = true;
    stream.exeTaken = true;
    stream.exeBranchInfo = makeBranchInfo(
        makeResolvedBranch(0x2008, true, false));
    stream.squashType = SquashType::SQUASH_TRAP;
    stream.squashPC = 0x2010;

    const auto ctx = makeBaseBranchUpdateContext(stream);

    EXPECT_EQ(ctx.tid, stream.tid);
    EXPECT_EQ(ctx.asidHash, stream.asidHash);
    EXPECT_EQ(ctx.startPC, stream.startPC);
    EXPECT_EQ(ctx.predTick, stream.predTick);
    EXPECT_EQ(ctx.squashType, SquashType::SQUASH_TRAP);
    EXPECT_EQ(ctx.squashPC, stream.squashPC);
}

TEST(UpdateEntryBuilderTest, FetchTargetPredictionDoesNotCreateActualBranch)
{
    FetchTarget stream;
    stream.predTaken = true;
    stream.predBranchInfo =
        makeBranchInfo(makeResolvedBranch(0x2008, true, false));

    EXPECT_FALSE(stream.resolved);
    EXPECT_FALSE(stream.exeTaken);
    EXPECT_EQ(stream.exeBranchInfo.pc, 0);
    EXPECT_TRUE(stream.resolvedBranches.empty());
}

TEST(UpdateEntryBuilderTest, FetchTargetAccumulatesResolvedBranchesByPC)
{
    FetchTarget stream;
    const ResolvedBranch first = makeResolvedBranch(0x1000, true, false);
    const ResolvedBranch second = makeResolvedBranch(0x1008, false, false);
    const ResolvedBranch duplicate = makeResolvedBranch(0x1008, true, true);
    const ResolvedBranch third = makeResolvedBranch(0x1010, true, false);

    EXPECT_TRUE(stream.addResolvedBranch(second));
    EXPECT_TRUE(stream.addResolvedBranch(first));
    EXPECT_FALSE(stream.addResolvedBranch(duplicate));
    EXPECT_EQ(stream.addResolvedBranches({third, first}), 1);

    ASSERT_EQ(stream.resolvedBranches.size(), 3);
    EXPECT_EQ(stream.resolvedBranches[0].pc, first.pc);
    EXPECT_EQ(stream.resolvedBranches[1].pc, second.pc);
    EXPECT_FALSE(stream.resolvedBranches[1].taken);
    EXPECT_EQ(stream.resolvedBranches[2].pc, third.pc);
}

TEST(UpdateEntryBuilderTest, FetchTargetResolvedBranchesUseUpdateBranchPrefix)
{
    const BTBEntry first = makeEntry(0x1000, true, false);
    const BTBEntry second = makeEntry(0x1008, true, false);
    const BTBEntry after = makeEntry(0x1010, true, false);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {first, second, after};
    const ResolvedBranch resolved_second =
        makeResolvedBranch(second.pc, true, true);
    stream.addResolvedBranches({
        makeResolvedBranch(after.pc, false, false),
        resolved_second,
        makeResolvedBranch(first.pc, false, false),
    });

    const auto update_branches =
        makeUpdateBranchPrefix(stream.resolvedBranches);
    EXPECT_EQ(update_branches.size(), 2);
    const auto update_ctx =
        makeActualBranchUpdateContext(
            makeBaseBranchUpdateContext(stream), update_branches);
    const auto *summary_branch =
        findActualUpdateSummaryBranch(update_branches);

    EXPECT_FALSE(stream.resolved);
    ASSERT_NE(summary_branch, nullptr);
    EXPECT_TRUE(summary_branch->taken);
    EXPECT_EQ(summary_branch->pc, second.pc);
    EXPECT_EQ(update_ctx.squashType, SquashType::SQUASH_CTRL);
    EXPECT_EQ(update_ctx.squashPC, second.pc);

    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, first.pc);
    EXPECT_EQ(update_branches[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, ResolvedBranchMissingFromPredictionTrainsDirectionAndTarget)
{
    const BTBEntry predicted = makeEntry(0x1000, true, false);
    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);
    const ResolvedBranch taken = makeResolvedBranch(0x1010, true, true);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {predicted};

    const auto update_branches =
        makeUpdateBranchPrefix({missing, taken});
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);

    ASSERT_EQ(update_btb_entries.size(), 1);
    EXPECT_EQ(update_btb_entries[0].pc, predicted.pc);
    EXPECT_FALSE(update_btb_entries[0].resolved);
    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, missing.pc);
    EXPECT_EQ(update_branches[1].pc, taken.pc);

    const auto direction_entries = buildDirectionUpdateEntries(
        update_btb_entries, update_branches,
        DirectionUpdateEntryFilter::Conditional, true);

    ASSERT_EQ(direction_entries.size(), 2);
    EXPECT_EQ(direction_entries[0].branch.pc, missing.pc);
    EXPECT_TRUE(direction_entries[0].isNewEntry);
    EXPECT_TRUE(direction_entries[0].baseTaken);
    EXPECT_FALSE(direction_entries[0].actualTaken);
    EXPECT_EQ(direction_entries[1].branch.pc, taken.pc);
    EXPECT_TRUE(direction_entries[1].isNewEntry);
    EXPECT_TRUE(direction_entries[1].actualTaken);
    EXPECT_TRUE(direction_entries[1].actualMispred);

    const auto target_entries = buildTargetUpdateEntries(
        update_btb_entries, update_branches,
        TargetUpdateEntryFilter::Any, true);

    ASSERT_EQ(target_entries.size(), 1);
    EXPECT_EQ(target_entries[0].entry.pc, taken.pc);
    EXPECT_TRUE(target_entries[0].isNewEntry);
    EXPECT_TRUE(target_entries[0].actualMispred);
    EXPECT_EQ(target_entries[0].actualBranch.target, taken.target);
}

TEST(UpdateEntryBuilderTest, FirstTakenDirectionEntryUsesLowestPC)
{
    DirectionUpdateEntry later;
    later.branch.pc = 0x1100;
    later.actualTaken = true;

    DirectionUpdateEntry not_taken;
    not_taken.branch.pc = 0x1000;

    DirectionUpdateEntry first;
    first.branch.pc = 0x1080;
    first.actualTaken = true;

    const auto entries = std::vector<DirectionUpdateEntry>{
        later, not_taken, first};
    const auto *first_taken = findFirstTakenDirectionUpdateEntry(entries);

    ASSERT_NE(first_taken, nullptr);
    EXPECT_EQ(first_taken->branch.pc, first.branch.pc);
}

TEST(UpdateEntryBuilderTest, ActualUpdateSummaryUsesTakenElseLastBranch)
{
    const auto first = makeResolvedBranch(0x1000, false, false);
    const auto taken = makeResolvedBranch(0x1080, true, false);
    const auto later = makeResolvedBranch(0x1100, false, false);

    const auto *taken_summary =
        findActualUpdateSummaryBranch({later, first, taken});
    ASSERT_NE(taken_summary, nullptr);
    EXPECT_EQ(taken_summary->pc, taken.pc);

    const auto *not_taken_summary =
        findActualUpdateSummaryBranch({first, later});
    ASSERT_NE(not_taken_summary, nullptr);
    EXPECT_EQ(not_taken_summary->pc, later.pc);

    EXPECT_EQ(findActualUpdateSummaryBranch({}), nullptr);
}

TEST(UpdateEntryBuilderTest, MispredictedActualUpdateBranchIsExplicit)
{
    const auto first = makeResolvedBranch(0x1000, false, false);
    const auto mispred = makeResolvedBranch(0x1080, false, true);
    const auto taken = makeResolvedBranch(0x1100, true, false);

    const auto *branch =
        findMispredictedActualUpdateBranch({first, mispred, taken});
    ASSERT_NE(branch, nullptr);
    EXPECT_EQ(branch->pc, mispred.pc);

    EXPECT_EQ(findMispredictedActualUpdateBranch({first, taken}), nullptr);
}

TEST(UpdateEntryBuilderTest, OnlyResolvedBranchSetEnablesBpuUpdate)
{
    FetchTarget stream;

    EXPECT_FALSE(shouldUpdateBpuPredictors({}));

    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);
    EXPECT_TRUE(shouldUpdateBpuPredictors({missing}));

    stream.isHit = true;
    EXPECT_FALSE(shouldUpdateBpuPredictors({}));

    stream.isHit = false;
    stream.predTaken = true;
    EXPECT_FALSE(shouldUpdateBpuPredictors({}));
}

TEST(UpdateEntryBuilderTest, NotTakenMissingBranchDoesNotTrainTarget)
{
    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);

    FetchTarget stream;
    stream.startPC = 0x1000;

    const auto update_branches = makeUpdateBranchPrefix({missing});
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);

    ASSERT_TRUE(update_btb_entries.empty());

    const auto direction_entries = buildDirectionUpdateEntries(
        update_btb_entries, update_branches,
        DirectionUpdateEntryFilter::Conditional, true);

    ASSERT_EQ(direction_entries.size(), 1);
    EXPECT_EQ(direction_entries[0].branch.pc, missing.pc);
    EXPECT_TRUE(direction_entries[0].baseTaken);
    EXPECT_FALSE(direction_entries[0].actualTaken);

    const auto target_entries = buildTargetUpdateEntries(
        update_btb_entries, update_branches,
        TargetUpdateEntryFilter::Any, true);

    EXPECT_TRUE(target_entries.empty());
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
