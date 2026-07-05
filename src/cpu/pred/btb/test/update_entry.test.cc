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
makeEntry(Addr pc, bool is_cond)
{
    BTBEntry entry;
    entry.valid = true;
    entry.pc = pc;
    entry.target = pc + 0x100;
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
makeIndirectEntry(Addr pc, bool is_return)
{
    BTBEntry entry = makeEntry(pc, false);
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

ResolvedBranch
makeResolvedBranch(const BTBEntry &entry, bool taken, bool mispred)
{
    ResolvedBranch branch;
    branch.pc = entry.pc;
    branch.target = entry.target;
    branch.taken = taken;
    branch.mispred = mispred;
    branch.isCond = entry.isCond;
    branch.isIndirect = entry.isIndirect;
    branch.isDirect = entry.isDirect;
    branch.isCall = entry.isCall;
    branch.isReturn = entry.isReturn;
    branch.size = entry.size;
    return branch;
}

BranchInfo
makeLegacyBranchInfoForTest(Addr pc, bool taken, bool mispred)
{
    const auto branch = makeResolvedBranch(pc, taken, mispred);
    BranchInfo info;
    info.pc = branch.pc;
    info.target = branch.target;
    info.isCond = branch.isCond;
    info.isIndirect = branch.isIndirect;
    info.isDirect = branch.isDirect;
    info.isCall = branch.isCall;
    info.isReturn = branch.isReturn;
    info.size = branch.size;
    return info;
}

std::vector<BTBEntry>
makeUpdateEntries(const FetchTarget &stream,
                  unsigned predict_width,
                  const std::vector<ResolvedBranch> &branches)
{
    const auto ctx = makeBaseBranchUpdateContext(stream);
    return selectPredictedBTBEntriesForUpdate(
        stream.predBTBEntries, ctx, branches, predict_width);
}

} // namespace

TEST(UpdateEntryBuilderTest, DirectionUpdateUsesActualBranchPrefix)
{
    const BTBEntry prefix_entry = makeEntry(0x1000, true);
    const BTBEntry later_entry = makeEntry(0x1004, true);

    const auto entries = buildDirectionUpdateEntries(
        {prefix_entry, later_entry},
        {makeResolvedBranch(prefix_entry.pc, false, false)});

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].pc, prefix_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionNewNotTakenEntryKeepsActualOutcome)
{
    const BTBEntry new_entry = makeEntry(0x1010, true);

    const auto entries = buildDirectionUpdateEntries(
        {}, {makeResolvedBranch(new_entry.pc, false, false)});

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].pc, new_entry.pc);
    EXPECT_TRUE(entries[0].baseTaken);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionEntryKeepsBaseDirection)
{
    BTBEntry entry = makeEntry(0x1014, true);
    entry.ctr = -1;

    const auto entries = buildDirectionUpdateEntries(
        {entry}, {makeResolvedBranch(entry.pc, true, false)});

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].pc, entry.pc);
    EXPECT_FALSE(entries[0].baseTaken);
    EXPECT_TRUE(entries[0].actualTaken);
}

TEST(UpdateEntryBuilderTest, DirectionResolvedBranchOutcomeOverridesContext)
{
    const BTBEntry entry = makeEntry(0x1018, true);

    const auto entries = buildDirectionUpdateEntries(
        {entry}, {makeResolvedBranch(entry.pc, true, true)});

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].pc, entry.pc);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].mispred);
}

TEST(UpdateEntryBuilderTest, DirectionUpdateRequiresActualBranchSet)
{
    const BTBEntry cond_entry = makeEntry(0x1020, true);

    const auto entries = buildDirectionUpdateEntries({cond_entry}, {});

    EXPECT_TRUE(entries.empty());
}

TEST(UpdateEntryBuilderTest, DirectionUpdateRequiresMatchingActualBranch)
{
    const BTBEntry predicted_cond = makeEntry(0x1024, true);
    const ResolvedBranch other_branch =
        makeResolvedBranch(0x1028, false, false);

    const auto entries =
        buildDirectionUpdateEntries({predicted_cond}, {other_branch});

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].pc, other_branch.pc);
    EXPECT_TRUE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, TargetUpdateUsesActualBranchPrefix)
{
    const BTBEntry prefix_entry = makeEntry(0x2000, false);
    const BTBEntry later_entry = makeEntry(0x2004, false);

    const auto entries = buildTargetUpdateEntries(
        {prefix_entry, later_entry},
        {makeResolvedBranch(prefix_entry.pc, false, false)},
        TargetUpdateEntryFilter::Any);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].actualBranch.pc, prefix_entry.pc);
    EXPECT_FALSE(entries[0].actualBranch.taken);
}

TEST(UpdateEntryBuilderTest, TargetFilterKeepsIndirectNonReturnOnly)
{
    const BTBEntry indirect = makeIndirectEntry(0x3000, false);
    const BTBEntry ret = makeIndirectEntry(0x3004, true);
    ResolvedBranch resolved = makeResolvedBranch(indirect.pc, true, false);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;

    const auto entries = buildTargetUpdateEntries(
        {indirect, ret}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_TRUE(entries[0].actualBranch.taken);
    EXPECT_FALSE(entries[0].actualBranch.mispred);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_EQ(entries[0].actualBranch.target, resolved.target);
}

TEST(UpdateEntryBuilderTest, TargetUpdateRequiresMatchingActualBranch)
{
    BTBEntry first = makeIndirectEntry(0x3000, false);
    first.target = 0x4440;
    BTBEntry second = makeIndirectEntry(0x3008, false);
    second.target = 0x5550;

    const Addr actual_target = 0xdead;
    ResolvedBranch resolved = makeResolvedBranch(second.pc, true, false);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;
    resolved.target = actual_target;

    const auto entries = buildTargetUpdateEntries(
        {first, second}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].actualBranch.pc, second.pc);
    EXPECT_TRUE(entries[0].actualBranch.taken);
    EXPECT_EQ(entries[0].actualBranch.pc, second.pc);
    EXPECT_EQ(entries[0].actualBranch.target, actual_target);
}

TEST(UpdateEntryBuilderTest, TargetResolvedBranchCarriesPerEntryActualTarget)
{
    const BTBEntry indirect = makeIndirectEntry(0x3010, false);

    ResolvedBranch resolved = makeResolvedBranch(indirect.pc, true, true);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;
    resolved.target = 0xbeef;

    const auto entries = buildTargetUpdateEntries(
        {indirect}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_TRUE(entries[0].actualBranch.taken);
    EXPECT_TRUE(entries[0].actualBranch.mispred);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_EQ(entries[0].actualBranch.target, resolved.target);
}

TEST(UpdateEntryBuilderTest, TargetFilterUsesActualBranchType)
{
    BTBEntry stale_direct = makeEntry(0x3014, false);
    stale_direct.isIndirect = false;
    stale_direct.isDirect = true;

    ResolvedBranch resolved =
        makeResolvedBranch(stale_direct.pc, true, false);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;
    resolved.isReturn = false;

    const auto entries = buildTargetUpdateEntries(
        {stale_direct}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].actualBranch.pc, stale_direct.pc);
    EXPECT_TRUE(entries[0].actualBranch.isIndirect);
    EXPECT_FALSE(entries[0].actualBranch.isReturn);
}

TEST(UpdateEntryBuilderTest, TargetTakenControlKeepsOnlyActualControl)
{
    const BTBEntry first = makeEntry(0x3018, false);
    const BTBEntry control = makeEntry(0x3020, false);
    const Addr actual_target = 0xdead;
    ResolvedBranch resolved = makeResolvedBranch(control.pc, true, false);
    resolved.isCond = false;
    resolved.target = actual_target;

    const auto entries = buildTargetUpdateEntries(
        {first, control}, {resolved},
        TargetUpdateEntryFilter::TakenControl);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].actualBranch.pc, control.pc);
    EXPECT_TRUE(entries[0].actualBranch.taken);
    EXPECT_FALSE(entries[0].actualBranch.mispred);
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
        TargetUpdateEntryFilter::TakenControl);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].actualBranch.pc, branch_pc);
    EXPECT_TRUE(entries[0].actualBranch.taken);
    EXPECT_EQ(entries[0].actualBranch.target, actual_target);
}

TEST(UpdateEntryBuilderTest, TargetTakenControlFallsThroughWithoutEntry)
{
    const BTBEntry predicted = makeEntry(0x3030, false);

    const auto entries = buildTargetUpdateEntries(
        {predicted}, {},
        TargetUpdateEntryFilter::TakenControl);

    EXPECT_TRUE(entries.empty());
}

TEST(UpdateEntryBuilderTest, UpdatedTargetEntryUsesExistingCondCounter)
{
    BTBEntry requested = makeEntry(0x3020, true);
    requested.ctr = -2;
    requested.tag = 0x10;
    requested.target = 0x4000;
    BTBEntry existing = requested;
    existing.ctr = 0;
    existing.tag = 0x20;
    existing.target = 0x5000;
    ResolvedBranch actual_branch =
        makeResolvedBranch(requested, true, false);
    actual_branch.target = 0x6000;

    const auto update =
        makeTargetUpdateEntryFromBase(requested, actual_branch);
    const auto written = buildUpdatedTargetEntry(update, &existing, 0x30);

    EXPECT_EQ(written.pc, requested.pc);
    EXPECT_EQ(written.ctr, 1);
    EXPECT_EQ(written.target, actual_branch.target);
    EXPECT_EQ(written.tag, 0x30);
}

TEST(UpdateEntryBuilderTest, UpdatedTargetEntryUsesActualDirectTarget)
{
    BTBEntry direct = makeEntry(0x3028, false);
    direct.target = 0x4000;
    ResolvedBranch actual_branch =
        makeResolvedBranch(direct, true, false);
    actual_branch.target = 0x5000;

    const auto update =
        makeTargetUpdateEntryFromBase(direct, actual_branch);
    const auto written = buildUpdatedTargetEntry(update, nullptr, 0x38);

    EXPECT_EQ(written.pc, direct.pc);
    EXPECT_EQ(written.target, actual_branch.target);
    EXPECT_EQ(written.tag, 0x38);
}

TEST(UpdateEntryBuilderTest, UpdatedTargetEntryUsesActualIndirectTarget)
{
    BTBEntry indirect = makeIndirectEntry(0x3030, false);
    indirect.target = 0x4000;
    ResolvedBranch actual_branch =
        makeResolvedBranch(indirect, true, false);
    actual_branch.target = 0x5000;

    const auto update =
        makeTargetUpdateEntryFromBase(indirect, actual_branch);
    const auto written = buildUpdatedTargetEntry(update, nullptr, 0x40);

    EXPECT_EQ(written.pc, indirect.pc);
    EXPECT_EQ(written.target, actual_branch.target);
    EXPECT_EQ(written.tag, 0x40);
}

TEST(UpdateEntryBuilderTest, UpdatedTargetEntryUsesActualBranchIdentity)
{
    BTBEntry stale_direct = makeEntry(0x3038, false);
    stale_direct.isCond = false;
    stale_direct.isDirect = true;
    stale_direct.isIndirect = false;
    stale_direct.size = 2;

    ResolvedBranch actual_branch =
        makeResolvedBranch(stale_direct.pc, true, false);
    actual_branch.isCond = true;
    actual_branch.isDirect = false;
    actual_branch.isIndirect = false;
    actual_branch.isCall = true;
    actual_branch.size = 4;
    actual_branch.target = 0x7000;

    const auto update =
        makeTargetUpdateEntryFromBase(stale_direct, actual_branch);
    const auto written = buildUpdatedTargetEntry(update, nullptr, 0x48);

    EXPECT_EQ(written.pc, actual_branch.pc);
    EXPECT_TRUE(written.isCond);
    EXPECT_FALSE(written.isDirect);
    EXPECT_TRUE(written.isCall);
    EXPECT_EQ(written.size, actual_branch.size);
    EXPECT_EQ(written.target, actual_branch.target);
    EXPECT_EQ(written.tag, 0x48);
}

TEST(UpdateEntryBuilderTest, TargetEntryKeepsOnlySmallBaseState)
{
    BTBEntry stale_entry = makeEntry(0x3040, true);
    stale_entry.target = 0x4100;
    stale_entry.ctr = -2;
    stale_entry.source = 3;

    ResolvedBranch actual_branch =
        makeResolvedBranch(stale_entry.pc, false, false);
    actual_branch.isCond = true;
    actual_branch.target = 0x9000;

    const auto entries = buildTargetUpdateEntries(
        {stale_entry}, {actual_branch},
        TargetUpdateEntryFilter::Any);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].actualBranch.pc, actual_branch.pc);
    EXPECT_EQ(entries[0].baseTarget, stale_entry.target);
    EXPECT_EQ(entries[0].baseCtr, stale_entry.ctr);
    EXPECT_EQ(entries[0].baseSource, stale_entry.source);

    const auto written = buildUpdatedTargetEntry(entries[0], nullptr, 0x50);
    EXPECT_EQ(written.pc, actual_branch.pc);
    EXPECT_EQ(written.target, stale_entry.target);
    EXPECT_EQ(written.ctr, -2);
    EXPECT_EQ(written.source, stale_entry.source);
    EXPECT_EQ(written.tag, 0x50);
}

TEST(UpdateEntryBuilderTest, UpdateEndInstPCUsesActualTakenOrSquashBoundary)
{
    const auto taken = makeResolvedBranch(0x1010, true, false);
    EXPECT_EQ(buildUpdateEndInstPC(
        0x1000, {taken}, 32), 0x1010);

    const auto not_taken = makeResolvedBranch(0x1010, false, false);
    EXPECT_EQ(buildUpdateEndInstPC(
        0x1004, {not_taken}, 32), 0x1020);

    const auto mispred = makeResolvedBranch(0x1008, false, true);
    EXPECT_EQ(buildUpdateEndInstPC(
        0x1000, {mispred}, 32), 0x1008);
}

TEST(UpdateEntryBuilderTest, UpdateEndInstPCUsesFirstTakenActualBranch)
{
    const auto later_taken = makeResolvedBranch(0x1100, true, false);
    const auto not_taken = makeResolvedBranch(0x1000, false, false);
    const auto first_taken = makeResolvedBranch(0x1080, true, false);

    EXPECT_EQ(buildUpdateEndInstPC(
        0x1000, {later_taken, not_taken, first_taken}, 32),
        first_taken.pc);
}

TEST(UpdateEntryBuilderTest, SelectPredictedBTBEntriesKeepsValidPrefix)
{
    const BTBEntry before = makeEntry(0x0ffc, true);
    const BTBEntry first = makeEntry(0x1000, true);
    const BTBEntry second = makeEntry(0x1008, true);
    const BTBEntry after = makeEntry(0x1010, true);
    BTBEntry invalid = makeEntry(0x1004, true);
    invalid.valid = false;

    const auto entries = selectPredictedBTBEntriesForUpdate(
        {before, first, invalid, second, after}, 0x1000, 0x1008);

    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].pc, first.pc);
    EXPECT_EQ(entries[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, SelectPredictedBTBEntriesUsesUpdateContext)
{
    const BTBEntry first = makeEntry(0x1000, true);
    const BTBEntry second = makeEntry(0x1008, true);
    const BTBEntry after = makeEntry(0x1010, true);

    BranchUpdateContext ctx;
    ctx.startPC = 0x1000;
    const auto update_branches = makeUpdateBranchPrefix({
        makeResolvedBranch(first.pc, false, false),
        makeResolvedBranch(second.pc, true, true),
        makeResolvedBranch(after.pc, false, false),
    });

    const auto entries = selectPredictedBTBEntriesForUpdate(
        {first, second, after}, ctx, update_branches, 32);

    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].pc, first.pc);
    EXPECT_EQ(entries[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, SquashHistoryUpdateAcceptsNoActualBranch)
{
    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {
        makeEntry(0x1000, true),
        makeEntry(0x1008, true),
    };

    const auto ghist_without_actual =
        stream.getGHistUpdateDuringSquash(0x1010, nullptr);
    const auto bwhist_without_actual =
        stream.getBwHistUpdateDuringSquash(0x1010, nullptr);
    const auto phist_without_actual =
        stream.getPHistUpdateDuringSquash(0x1010, nullptr);

    EXPECT_EQ(ghist_without_actual.shamt, 2);
    EXPECT_FALSE(ghist_without_actual.taken);
    EXPECT_EQ(bwhist_without_actual.shamt, 2);
    EXPECT_FALSE(bwhist_without_actual.taken);
    EXPECT_FALSE(phist_without_actual.taken);

    auto actual_branch = makeResolvedBranch(0x1010, true, true);
    actual_branch.target = 0x0ff0;
    const auto ghist_with_actual =
        stream.getGHistUpdateDuringSquash(0x1010, &actual_branch);
    const auto bwhist_with_actual =
        stream.getBwHistUpdateDuringSquash(0x1010, &actual_branch);
    const auto phist_with_actual =
        stream.getPHistUpdateDuringSquash(0x1010, &actual_branch);

    EXPECT_EQ(ghist_with_actual.shamt, 3);
    EXPECT_TRUE(ghist_with_actual.taken);
    EXPECT_EQ(bwhist_with_actual.shamt, 3);
    EXPECT_TRUE(bwhist_with_actual.taken);
    EXPECT_TRUE(phist_with_actual.taken);
    EXPECT_EQ(phist_with_actual.pc, actual_branch.pc);
    EXPECT_EQ(phist_with_actual.target, actual_branch.target);
}

TEST(UpdateEntryBuilderTest, ResolvedBranchesBuildUpdateInputs)
{
    const BTBEntry first = makeEntry(0x1000, true);
    const BTBEntry second = makeEntry(0x1008, true);
    const BTBEntry after = makeEntry(0x1010, true);

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
    const auto pred_update_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto *summary_branch =
        findActualUpdateSummaryBranch(update_branches);
    const auto *boundary_branch =
        findUpdateBoundaryActualBranch(update_branches);

    EXPECT_FALSE(stream.resolved);
    ASSERT_NE(summary_branch, nullptr);
    EXPECT_TRUE(summary_branch->taken);
    EXPECT_EQ(summary_branch->pc, second.pc);
    EXPECT_EQ(summary_branch->target, second.pc + 0x200);
    ASSERT_NE(boundary_branch, nullptr);
    EXPECT_EQ(boundary_branch->pc, second.pc);
    EXPECT_TRUE(boundary_branch->mispred);

    ASSERT_EQ(pred_update_entries.size(), 2);
    EXPECT_EQ(pred_update_entries[0].pc, first.pc);
    EXPECT_EQ(pred_update_entries[1].pc, second.pc);
    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, first.pc);
    EXPECT_EQ(update_branches[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, ActualBranchPrefixProvidesSummaryAndBoundary)
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
    const BranchInfo ignored_legacy_summary =
        makeLegacyBranchInfoForTest(0x1080, false, false);

    const auto update_branches =
        makeUpdateBranchPrefix({first, second});
    const auto ctx = makeBaseBranchUpdateContext(stream);
    const auto *summary_branch =
        findActualUpdateSummaryBranch(update_branches);
    const auto *boundary_branch =
        findUpdateBoundaryActualBranch(update_branches);

    EXPECT_FALSE(stream.resolved);
    EXPECT_EQ(ctx.tid, stream.tid);
    EXPECT_EQ(ctx.asidHash, stream.asidHash);
    EXPECT_EQ(ctx.startPC, stream.startPC);
    EXPECT_EQ(ctx.predTick, stream.predTick);
    ASSERT_NE(summary_branch, nullptr);
    EXPECT_TRUE(summary_branch->taken);
    EXPECT_EQ(summary_branch->pc, second.pc);
    EXPECT_EQ(summary_branch->target, second.target);
    ASSERT_NE(boundary_branch, nullptr);
    EXPECT_EQ(boundary_branch->pc, second.pc);
    EXPECT_TRUE(boundary_branch->mispred);

    EXPECT_NE(summary_branch->pc, ignored_legacy_summary.pc);
}

TEST(UpdateEntryBuilderTest, BaseBranchUpdateContextKeepsPredictionContextOnly)
{
    FetchTarget stream;
    stream.tid = 2;
    stream.asidHash = 4;
    stream.startPC = 0x2000;
    stream.predTick = 24;
    stream.resolved = true;
    stream.squashType = SquashType::SQUASH_TRAP;
    stream.squashPC = 0x2010;

    const auto ctx = makeBaseBranchUpdateContext(stream);

    EXPECT_EQ(ctx.tid, stream.tid);
    EXPECT_EQ(ctx.asidHash, stream.asidHash);
    EXPECT_EQ(ctx.startPC, stream.startPC);
    EXPECT_EQ(ctx.predTick, stream.predTick);
}

TEST(UpdateEntryBuilderTest, FetchTargetPredictionDoesNotCreateActualBranch)
{
    FetchTarget stream;
    stream.predTaken = true;
    stream.predBranchInfo =
        makeLegacyBranchInfoForTest(0x2008, true, false);

    EXPECT_FALSE(stream.resolved);
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
    const BTBEntry first = makeEntry(0x1000, true);
    const BTBEntry second = makeEntry(0x1008, true);
    const BTBEntry after = makeEntry(0x1010, true);

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
    const auto *summary_branch =
        findActualUpdateSummaryBranch(update_branches);
    const auto *boundary_branch =
        findUpdateBoundaryActualBranch(update_branches);

    EXPECT_FALSE(stream.resolved);
    ASSERT_NE(summary_branch, nullptr);
    EXPECT_TRUE(summary_branch->taken);
    EXPECT_EQ(summary_branch->pc, second.pc);
    ASSERT_NE(boundary_branch, nullptr);
    EXPECT_EQ(boundary_branch->pc, second.pc);
    EXPECT_TRUE(boundary_branch->mispred);

    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, first.pc);
    EXPECT_EQ(update_branches[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, ResolvedBranchMissingFromPredictionTrainsDirectionAndTarget)
{
    const BTBEntry predicted = makeEntry(0x1000, true);
    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);
    const ResolvedBranch taken = makeResolvedBranch(0x1010, true, true);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {predicted};

    const auto update_branches =
        makeUpdateBranchPrefix({missing, taken});
    const auto pred_update_entries =
        makeUpdateEntries(stream, 32, update_branches);

    ASSERT_EQ(pred_update_entries.size(), 1);
    EXPECT_EQ(pred_update_entries[0].pc, predicted.pc);
    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, missing.pc);
    EXPECT_EQ(update_branches[1].pc, taken.pc);

    const auto direction_entries = buildDirectionUpdateEntries(
        pred_update_entries, update_branches);

    ASSERT_EQ(direction_entries.size(), 2);
    EXPECT_EQ(direction_entries[0].pc, missing.pc);
    EXPECT_TRUE(direction_entries[0].isNewEntry);
    EXPECT_TRUE(direction_entries[0].baseTaken);
    EXPECT_FALSE(direction_entries[0].actualTaken);
    EXPECT_EQ(direction_entries[1].pc, taken.pc);
    EXPECT_TRUE(direction_entries[1].isNewEntry);
    EXPECT_TRUE(direction_entries[1].actualTaken);
    EXPECT_TRUE(direction_entries[1].mispred);

    const auto target_entries = buildTargetUpdateEntries(
        pred_update_entries, update_branches,
        TargetUpdateEntryFilter::Any);

    ASSERT_EQ(target_entries.size(), 1);
    EXPECT_EQ(target_entries[0].actualBranch.pc, taken.pc);
    EXPECT_TRUE(target_entries[0].actualBranch.mispred);
    EXPECT_EQ(target_entries[0].actualBranch.target, taken.target);
}

TEST(UpdateEntryBuilderTest, FirstTakenDirectionEntryUsesLowestPC)
{
    DirectionUpdateEntry later =
        makeDirectionUpdateEntry(
            makeResolvedBranch(0x1100, true, false), true, false);
    DirectionUpdateEntry not_taken =
        makeDirectionUpdateEntry(
            makeResolvedBranch(0x1000, false, false), true, false);
    DirectionUpdateEntry first =
        makeDirectionUpdateEntry(
            makeResolvedBranch(0x1080, true, false), true, false);

    const auto entries = std::vector<DirectionUpdateEntry>{
        later, not_taken, first};
    const auto *first_taken = findFirstTakenDirectionUpdateEntry(entries);

    ASSERT_NE(first_taken, nullptr);
    EXPECT_EQ(first_taken->pc, first.pc);
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

TEST(UpdateEntryBuilderTest, ReturnStackUpdateUsesTakenSummaryBranch)
{
    auto call = makeResolvedBranch(0x1000, true, false);
    call.isCond = false;
    call.isCall = true;
    call.isReturn = false;

    auto ret = makeResolvedBranch(0x1008, true, false);
    ret.isCond = false;
    ret.isCall = false;
    ret.isReturn = true;

    const auto not_taken_call = [&] {
        auto branch = call;
        branch.taken = false;
        return branch;
    }();
    const auto taken_cond = makeResolvedBranch(0x1010, true, false);

    const std::vector<ResolvedBranch> call_branches = {not_taken_call, call};
    const auto *call_update =
        findTakenReturnStackUpdateBranch(call_branches);
    ASSERT_NE(call_update, nullptr);
    EXPECT_EQ(call_update->pc, call.pc);
    EXPECT_TRUE(isReturnStackActionBranch(*call_update));

    const std::vector<ResolvedBranch> ret_branches = {ret};
    const auto *ret_update =
        findTakenReturnStackUpdateBranch(ret_branches);
    ASSERT_NE(ret_update, nullptr);
    EXPECT_EQ(ret_update->pc, ret.pc);
    EXPECT_TRUE(isReturnStackActionBranch(*ret_update));

    const std::vector<ResolvedBranch> non_ras_branches = {taken_cond};
    const auto *non_ras_update =
        findTakenReturnStackUpdateBranch(non_ras_branches);
    ASSERT_NE(non_ras_update, nullptr);
    EXPECT_FALSE(isReturnStackActionBranch(*non_ras_update));

    const std::vector<ResolvedBranch> not_taken_branches = {not_taken_call};
    const std::vector<ResolvedBranch> empty_branches;
    EXPECT_EQ(findTakenReturnStackUpdateBranch(not_taken_branches), nullptr);
    EXPECT_EQ(findTakenReturnStackUpdateBranch(empty_branches), nullptr);
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

TEST(UpdateEntryBuilderTest, NotTakenMissingBranchDoesNotTrainTarget)
{
    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);

    FetchTarget stream;
    stream.startPC = 0x1000;

    const auto update_branches = makeUpdateBranchPrefix({missing});
    const auto pred_update_entries =
        makeUpdateEntries(stream, 32, update_branches);

    ASSERT_TRUE(pred_update_entries.empty());

    const auto direction_entries = buildDirectionUpdateEntries(
        pred_update_entries, update_branches);

    ASSERT_EQ(direction_entries.size(), 1);
    EXPECT_EQ(direction_entries[0].pc, missing.pc);
    EXPECT_TRUE(direction_entries[0].baseTaken);
    EXPECT_FALSE(direction_entries[0].actualTaken);

    const auto target_entries = buildTargetUpdateEntries(
        pred_update_entries, update_branches,
        TargetUpdateEntryFilter::Any);

    EXPECT_TRUE(target_entries.empty());
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
