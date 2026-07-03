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

BranchUpdateContext
makeDirectionContext(Addr control_pc, bool actual_taken)
{
    BranchUpdateContext ctx;
    ctx.startPC = control_pc & ~0xf;
    ctx.controlPC = control_pc;
    ctx.actualTaken = actual_taken;
    return ctx;
}

BranchUpdateContext
makeTargetContext(Addr control_pc, bool actual_taken)
{
    BranchUpdateContext ctx;
    ctx.startPC = control_pc & ~0xf;
    ctx.controlPC = control_pc;
    ctx.actualTaken = actual_taken;
    ctx.actualBranch.pc = control_pc;
    ctx.actualBranch.target = control_pc + 0x100;
    return ctx;
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
    const auto ctx = makeBranchUpdateContext(stream, branches);
    const auto update_end_inst_pc =
        buildUpdateEndInstPC(ctx, predict_width);
    return makeUpdateBTBEntries(
        stream.predBTBEntries, ctx.startPC, update_end_inst_pc);
}

} // namespace

TEST(UpdateEntryBuilderTest, DirectionResolvedPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x1000, true, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x1004, true, true);
    const BranchUpdateContext ctx =
        makeDirectionContext(prefix_entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {prefix_entry, legacy_resolved_entry}, {},
        BTBUpdateEntrySelection{BTBEntry(), true},
        {makeResolvedBranch(prefix_entry.pc, false, false)},
        DirectionUpdateEntryFilter::Conditional, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, prefix_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionNewNotTakenEntryKeepsActualOutcome)
{
    const BTBEntry new_entry = makeEntry(0x1010, true, false);
    const BranchUpdateContext ctx =
        makeDirectionContext(new_entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {}, {}, BTBUpdateEntrySelection{new_entry, false}, {},
        DirectionUpdateEntryFilter::Conditional, false, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, new_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionResolvedBranchOutcomeOverridesContext)
{
    const BTBEntry entry = makeEntry(0x1018, true, false);
    const BranchUpdateContext ctx = makeDirectionContext(entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {entry}, {}, BTBUpdateEntrySelection{BTBEntry(), true},
        {makeResolvedBranch(entry.pc, true, true)},
        DirectionUpdateEntryFilter::Conditional, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, entry.pc);
    EXPECT_TRUE(entries[0].actualTaken);
}

TEST(UpdateEntryBuilderTest, MgscResolvedUpdateRequiresResolvedBranchSet)
{
    const BTBEntry cond_entry = makeEntry(0x1020, true, false);
    const BranchUpdateContext ctx =
        makeDirectionContext(cond_entry.pc, true);

    const auto entries = buildDirectionUpdateEntries(
        {cond_entry}, {}, BTBUpdateEntrySelection{BTBEntry(), true}, {},
        DirectionUpdateEntryFilter::Mgsc, true, ctx);

    EXPECT_TRUE(entries.empty());
}

TEST(UpdateEntryBuilderTest, TargetResolvedPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x2000, false, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x2004, false, true);
    const BranchUpdateContext ctx = makeTargetContext(prefix_entry.pc, false);

    const auto entries = buildTargetUpdateEntries(
        {prefix_entry, legacy_resolved_entry},
        BTBUpdateEntrySelection{BTBEntry(), true},
        {makeResolvedBranch(prefix_entry.pc, false, false)},
        TargetUpdateEntryFilter::Any, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, prefix_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, TargetFilterKeepsIndirectNonReturnOnly)
{
    const BTBEntry indirect = makeIndirectEntry(0x3000, false, true);
    const BTBEntry ret = makeIndirectEntry(0x3004, true, true);
    const BranchUpdateContext ctx = makeTargetContext(indirect.pc, true);

    const auto entries = buildTargetUpdateEntries(
        {indirect, ret}, BTBUpdateEntrySelection{BTBEntry(), true}, {},
        TargetUpdateEntryFilter::IndirectNonReturn, false, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, indirect.pc);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_EQ(entries[0].actualBranch.target, ctx.actualBranch.target);
}

TEST(UpdateEntryBuilderTest, TargetEntriesCarryPerEntryActualBranch)
{
    BTBEntry first = makeIndirectEntry(0x3000, false, true);
    first.target = 0x4440;
    BTBEntry second = makeIndirectEntry(0x3008, false, true);
    second.target = 0x5550;

    BranchUpdateContext ctx = makeTargetContext(second.pc, true);
    ctx.actualBranch.target = 0xdead;

    const auto entries = buildTargetUpdateEntries(
        {first, second}, BTBUpdateEntrySelection{BTBEntry(), true}, {},
        TargetUpdateEntryFilter::IndirectNonReturn, false, ctx);

    ASSERT_EQ(entries.size(), 2);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_EQ(entries[0].actualBranch.pc, first.pc);
    EXPECT_EQ(entries[0].actualBranch.target, first.target);
    EXPECT_TRUE(entries[1].actualTaken);
    EXPECT_EQ(entries[1].actualBranch.pc, second.pc);
    EXPECT_EQ(entries[1].actualBranch.target, ctx.actualBranch.target);
}

TEST(UpdateEntryBuilderTest, TargetResolvedBranchCarriesPerEntryActualTarget)
{
    const BTBEntry indirect = makeIndirectEntry(0x3010, false, true);
    BranchUpdateContext ctx = makeTargetContext(indirect.pc, false);
    ctx.actualBranch.target = 0xdead;

    ResolvedBranch resolved = makeResolvedBranch(indirect.pc, true, true);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;
    resolved.target = 0xbeef;

    const auto entries = buildTargetUpdateEntries(
        {indirect}, BTBUpdateEntrySelection{BTBEntry(), true}, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_EQ(entries[0].actualBranch.target, resolved.target);
}

TEST(UpdateEntryBuilderTest, UpdateEndInstPCUsesActualTakenOrSquashBoundary)
{
    BranchUpdateContext taken_ctx;
    taken_ctx.startPC = 0x1000;
    taken_ctx.actualTaken = true;
    taken_ctx.controlPC = 0x1010;
    EXPECT_EQ(buildUpdateEndInstPC(taken_ctx, 32), 0x1010);

    BranchUpdateContext fallthrough_ctx;
    fallthrough_ctx.startPC = 0x1004;
    fallthrough_ctx.actualTaken = false;
    fallthrough_ctx.controlPC = 0x1010;
    EXPECT_EQ(buildUpdateEndInstPC(fallthrough_ctx, 32), 0x1020);

    BranchUpdateContext squash_ctx;
    squash_ctx.startPC = 0x1000;
    squash_ctx.actualTaken = true;
    squash_ctx.controlPC = 0x1010;
    squash_ctx.squashType = SquashType::SQUASH_CTRL;
    squash_ctx.squashPC = 0x1008;
    EXPECT_EQ(buildUpdateEndInstPC(squash_ctx, 32), 0x1008);
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
    const auto update_branches = makeResolvedUpdateBranches({
        makeResolvedBranch(first.pc, false, false),
        resolved_second,
        makeResolvedBranch(after.pc, false, false),
    });
    const BTBUpdateEntrySelection selection{
        BTBEntry(makeBranchInfo(resolved_second)), false};
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto update_ctx = makeBranchUpdateContext(stream, update_branches);

    EXPECT_FALSE(stream.resolved);
    EXPECT_TRUE(update_ctx.actualTaken);
    EXPECT_EQ(update_ctx.actualBranch.pc, second.pc);
    EXPECT_EQ(update_ctx.actualBranch.target, second.pc + 0x200);

    ASSERT_EQ(update_btb_entries.size(), 2);
    EXPECT_EQ(update_btb_entries[0].pc, first.pc);
    EXPECT_EQ(update_btb_entries[1].pc, second.pc);
    EXPECT_FALSE(update_btb_entries[0].resolved);
    EXPECT_FALSE(update_btb_entries[1].resolved);
    EXPECT_EQ(selection.entry.pc, second.pc);
    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, first.pc);
    EXPECT_EQ(update_branches[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, BranchUpdateContextUsesResolvedPrefix)
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
        makeResolvedUpdateBranches({first, second});
    const auto ctx = makeBranchUpdateContext(stream, update_branches);

    EXPECT_FALSE(stream.resolved);
    EXPECT_FALSE(stream.exeTaken);
    EXPECT_EQ(ctx.tid, stream.tid);
    EXPECT_EQ(ctx.asidHash, stream.asidHash);
    EXPECT_EQ(ctx.startPC, stream.startPC);
    EXPECT_EQ(ctx.predTick, stream.predTick);
    EXPECT_TRUE(ctx.actualTaken);
    EXPECT_EQ(ctx.controlPC, second.pc);
    EXPECT_EQ(ctx.actualBranch.pc, second.pc);
    EXPECT_EQ(ctx.actualBranch.target, second.target);
    EXPECT_EQ(ctx.squashType, SquashType::SQUASH_CTRL);
    EXPECT_EQ(ctx.squashPC, second.pc);

    const auto fallback_ctx = makeFallbackBranchUpdateContext(stream);
    EXPECT_NE(ctx.controlPC, fallback_ctx.controlPC);
}

TEST(UpdateEntryBuilderTest, BranchUpdateContextFallsBackWithoutResolvedPrefix)
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

    const auto ctx = makeBranchUpdateContext(stream, {});

    EXPECT_EQ(ctx.tid, stream.tid);
    EXPECT_EQ(ctx.asidHash, stream.asidHash);
    EXPECT_EQ(ctx.startPC, stream.startPC);
    EXPECT_EQ(ctx.predTick, stream.predTick);
    EXPECT_EQ(ctx.controlPC, stream.exeBranchInfo.pc);
    EXPECT_EQ(ctx.actualBranch.pc, stream.exeBranchInfo.pc);
    EXPECT_EQ(ctx.actualBranch.target, stream.exeBranchInfo.target);
    EXPECT_TRUE(ctx.actualTaken);
    EXPECT_EQ(ctx.squashType, SquashType::SQUASH_TRAP);
    EXPECT_EQ(ctx.squashPC, stream.squashPC);
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

TEST(UpdateEntryBuilderTest, FetchTargetResolvedBranchesUseResolvedPrefix)
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
        makeResolvedUpdateBranches(stream.resolvedBranches);
    EXPECT_EQ(update_branches.size(), 2);
    const BTBUpdateEntrySelection selection{
        BTBEntry(makeBranchInfo(resolved_second)), false};
    const auto update_ctx = makeBranchUpdateContext(stream, update_branches);

    EXPECT_FALSE(stream.resolved);
    EXPECT_TRUE(update_ctx.actualTaken);
    EXPECT_EQ(update_ctx.actualBranch.pc, second.pc);

    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, first.pc);
    EXPECT_EQ(update_branches[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, ResolvedBranchMissingFromPredictionTrainsDirection)
{
    const BTBEntry predicted = makeEntry(0x1000, true, false);
    const BTBEntry selected = makeEntry(0x1010, true, false);
    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);
    const ResolvedBranch taken = makeResolvedBranch(selected.pc, true, true);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {predicted};

    const auto update_branches =
        makeResolvedUpdateBranches({missing, taken});
    const auto update_ctx = makeBranchUpdateContext(stream, update_branches);
    const BTBUpdateEntrySelection selection{selected, false};
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto update_new_direction_entries =
        makeNewDirectionEntries(
            update_btb_entries, selection, update_branches);

    ASSERT_EQ(update_btb_entries.size(), 1);
    EXPECT_EQ(update_btb_entries[0].pc, predicted.pc);
    EXPECT_FALSE(update_btb_entries[0].resolved);
    ASSERT_EQ(update_new_direction_entries.size(), 1);
    EXPECT_EQ(update_new_direction_entries[0].pc, missing.pc);
    EXPECT_TRUE(update_new_direction_entries[0].resolved);
    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, missing.pc);
    EXPECT_EQ(update_branches[1].pc, selected.pc);

    const auto direction_entries = buildDirectionUpdateEntries(
        update_btb_entries, update_new_direction_entries,
        selection,
        update_branches, DirectionUpdateEntryFilter::Conditional,
        true, update_ctx);

    ASSERT_EQ(direction_entries.size(), 2);
    EXPECT_EQ(direction_entries[0].entry.pc, missing.pc);
    EXPECT_TRUE(direction_entries[0].isNewEntry);
    EXPECT_FALSE(direction_entries[0].actualTaken);
    EXPECT_EQ(direction_entries[1].entry.pc, selected.pc);
    EXPECT_TRUE(direction_entries[1].isNewEntry);
    EXPECT_TRUE(direction_entries[1].actualTaken);

    const auto target_entries = buildTargetUpdateEntries(
        update_btb_entries, selection, update_branches,
        TargetUpdateEntryFilter::Any, true, update_ctx);

    ASSERT_EQ(target_entries.size(), 1);
    EXPECT_EQ(target_entries[0].entry.pc, selected.pc);
}

TEST(UpdateEntryBuilderTest, ResolvedBranchSetEnablesBpuUpdate)
{
    FetchTarget stream;
    stream.isHit = false;
    stream.exeTaken = false;
    auto update_ctx = makeFallbackBranchUpdateContext(stream);

    EXPECT_FALSE(shouldUpdateBpuPredictors(stream.isHit, update_ctx, {}));

    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);
    EXPECT_TRUE(
        shouldUpdateBpuPredictors(stream.isHit, update_ctx, {missing}));

    stream.isHit = true;
    EXPECT_TRUE(shouldUpdateBpuPredictors(stream.isHit, update_ctx, {}));

    stream.isHit = false;
    stream.exeTaken = true;
    update_ctx = makeFallbackBranchUpdateContext(stream);
    EXPECT_TRUE(shouldUpdateBpuPredictors(stream.isHit, update_ctx, {}));
}

TEST(UpdateEntryBuilderTest, InvalidSelectedEntryDoesNotTrainTarget)
{
    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);

    FetchTarget stream;
    stream.startPC = 0x1000;

    const auto update_branches = makeResolvedUpdateBranches({missing});
    const auto update_ctx = makeBranchUpdateContext(stream, update_branches);
    const BTBUpdateEntrySelection selection;
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto update_new_direction_entries =
        makeNewDirectionEntries(
            update_btb_entries, selection, update_branches);

    ASSERT_TRUE(update_btb_entries.empty());
    ASSERT_EQ(update_new_direction_entries.size(), 1);
    EXPECT_EQ(update_new_direction_entries[0].pc, missing.pc);
    EXPECT_TRUE(update_new_direction_entries[0].valid);

    const auto direction_entries = buildDirectionUpdateEntries(
        update_btb_entries, update_new_direction_entries,
        selection,
        update_branches, DirectionUpdateEntryFilter::Conditional,
        true, update_ctx);

    ASSERT_EQ(direction_entries.size(), 1);
    EXPECT_EQ(direction_entries[0].entry.pc, missing.pc);
    EXPECT_FALSE(direction_entries[0].actualTaken);

    const auto target_entries = buildTargetUpdateEntries(
        update_btb_entries, selection, update_branches,
        TargetUpdateEntryFilter::Any, true, update_ctx);

    EXPECT_TRUE(target_entries.empty());
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
