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

DirectionUpdateContext
makeDirectionContext(Addr control_pc, bool actual_taken)
{
    DirectionUpdateContext ctx;
    ctx.startPC = control_pc & ~0xf;
    ctx.controlPC = control_pc;
    ctx.actualTaken = actual_taken;
    return ctx;
}

TargetUpdateContext
makeTargetContext(Addr control_pc, bool actual_taken)
{
    TargetUpdateContext ctx;
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
    const auto ctx = stream.makeTargetUpdateContext();
    const auto update_end_inst_pc = buildUpdateEndInstPC(
        ctx.startPC, predict_width, ctx.actualTaken, ctx.controlPC,
        ctx.squashType, ctx.squashPC);
    return makeUpdateBTBEntries(
        stream.predBTBEntries, ctx.startPC, update_end_inst_pc, branches);
}

} // namespace

TEST(UpdateEntryBuilderTest, DirectionResolvedPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x1000, true, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x1004, true, true);
    const DirectionUpdateContext ctx =
        makeDirectionContext(prefix_entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {prefix_entry, legacy_resolved_entry}, {}, BTBEntry(), true,
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
    const DirectionUpdateContext ctx =
        makeDirectionContext(new_entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {}, {}, new_entry, false, {},
        DirectionUpdateEntryFilter::Conditional, false, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, new_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionResolvedBranchOutcomeOverridesContext)
{
    const BTBEntry entry = makeEntry(0x1018, true, false);
    const DirectionUpdateContext ctx = makeDirectionContext(entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {entry}, {}, BTBEntry(), true,
        {makeResolvedBranch(entry.pc, true, true)},
        DirectionUpdateEntryFilter::Conditional, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, entry.pc);
    EXPECT_TRUE(entries[0].actualTaken);
}

TEST(UpdateEntryBuilderTest, MgscResolvedUpdateKeepsConditionalEntriesWithoutPrefix)
{
    const BTBEntry cond_entry = makeEntry(0x1020, true, false);
    const DirectionUpdateContext ctx =
        makeDirectionContext(cond_entry.pc, true);

    const auto entries = buildDirectionUpdateEntries(
        {cond_entry}, {}, BTBEntry(), true, {},
        DirectionUpdateEntryFilter::Mgsc, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, cond_entry.pc);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, TargetResolvedPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x2000, false, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x2004, false, true);
    const TargetUpdateContext ctx = makeTargetContext(prefix_entry.pc, false);

    const auto entries = buildTargetUpdateEntries(
        {prefix_entry, legacy_resolved_entry}, BTBEntry(), true,
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
    const TargetUpdateContext ctx = makeTargetContext(indirect.pc, true);

    const auto entries = buildTargetUpdateEntries(
        {indirect, ret}, BTBEntry(), true, {},
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

    TargetUpdateContext ctx = makeTargetContext(second.pc, true);
    ctx.actualBranch.target = 0xdead;

    const auto entries = buildTargetUpdateEntries(
        {first, second}, BTBEntry(), true, {},
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
    TargetUpdateContext ctx = makeTargetContext(indirect.pc, false);
    ctx.actualBranch.target = 0xdead;

    ResolvedBranch resolved = makeResolvedBranch(indirect.pc, true, true);
    resolved.isCond = false;
    resolved.isDirect = false;
    resolved.isIndirect = true;
    resolved.target = 0xbeef;

    const auto entries = buildTargetUpdateEntries(
        {indirect}, BTBEntry(), true, {resolved},
        TargetUpdateEntryFilter::IndirectNonReturn, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_EQ(entries[0].actualBranch.pc, indirect.pc);
    EXPECT_EQ(entries[0].actualBranch.target, resolved.target);
}

TEST(UpdateEntryBuilderTest, UpdateEndInstPCUsesActualTakenOrSquashBoundary)
{
    EXPECT_EQ(buildUpdateEndInstPC(0x1000, 32, true, 0x1010,
                                   SquashType::SQUASH_NONE, 0),
              0x1010);
    EXPECT_EQ(buildUpdateEndInstPC(0x1004, 32, false, 0x1010,
                                   SquashType::SQUASH_NONE, 0),
              0x1020);
    EXPECT_EQ(buildUpdateEndInstPC(0x1000, 32, true, 0x1010,
                                   SquashType::SQUASH_CTRL, 0x1008),
              0x1008);
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
    applyResolvedBranchResult(stream, update_branches);
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto update_selection =
        markResolvedSelection(selection, update_branches);

    EXPECT_TRUE(stream.resolved);
    EXPECT_TRUE(stream.exeTaken);
    EXPECT_EQ(stream.exeBranchInfo.pc, second.pc);
    EXPECT_EQ(stream.exeBranchInfo.target, second.pc + 0x200);

    ASSERT_EQ(update_btb_entries.size(), 2);
    EXPECT_EQ(update_btb_entries[0].pc, first.pc);
    EXPECT_EQ(update_btb_entries[1].pc, second.pc);
    EXPECT_TRUE(update_btb_entries[0].resolved);
    EXPECT_TRUE(update_btb_entries[1].resolved);
    EXPECT_EQ(update_selection.entry.pc, second.pc);
    EXPECT_TRUE(update_selection.entry.resolved);
    ASSERT_EQ(update_branches.size(), 2);
    EXPECT_EQ(update_branches[0].pc, first.pc);
    EXPECT_EQ(update_branches[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, FetchTargetAccumulatesResolvedBranchesByPC)
{
    FetchTarget stream;
    const ResolvedBranch first = makeResolvedBranch(0x1000, false, false);
    const ResolvedBranch second = makeResolvedBranch(0x1008, false, false);
    const ResolvedBranch duplicate = makeResolvedBranch(0x1008, true, true);
    const ResolvedBranch third = makeResolvedBranch(0x1010, false, false);

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
    applyResolvedBranchResult(stream, update_branches);

    EXPECT_TRUE(stream.resolved);
    EXPECT_TRUE(stream.exeTaken);
    EXPECT_EQ(stream.exeBranchInfo.pc, second.pc);

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
    applyResolvedBranchResult(stream, update_branches);
    const BTBUpdateEntrySelection selection{selected, false};
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto update_new_direction_entries =
        makeNewDirectionEntries(
            update_btb_entries, selection, update_branches);
    const auto update_selection =
        markResolvedSelection(selection, update_branches);

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
        update_selection.entry, update_selection.isOldEntry,
        update_branches, DirectionUpdateEntryFilter::Conditional,
        true, stream.makeDirectionUpdateContext());

    ASSERT_EQ(direction_entries.size(), 2);
    EXPECT_EQ(direction_entries[0].entry.pc, missing.pc);
    EXPECT_TRUE(direction_entries[0].isNewEntry);
    EXPECT_FALSE(direction_entries[0].actualTaken);
    EXPECT_EQ(direction_entries[1].entry.pc, selected.pc);
    EXPECT_TRUE(direction_entries[1].isNewEntry);
    EXPECT_TRUE(direction_entries[1].actualTaken);

    const auto target_entries = buildTargetUpdateEntries(
        update_btb_entries, update_selection.entry,
        update_selection.isOldEntry, update_branches,
        TargetUpdateEntryFilter::Any, true, stream.makeTargetUpdateContext());

    ASSERT_EQ(target_entries.size(), 1);
    EXPECT_EQ(target_entries[0].entry.pc, selected.pc);
}

TEST(UpdateEntryBuilderTest, ResolvedBranchSetEnablesBpuUpdate)
{
    FetchTarget stream;
    stream.isHit = false;
    stream.exeTaken = false;

    EXPECT_FALSE(shouldUpdateBpuPredictors(stream, {}));

    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);
    EXPECT_TRUE(shouldUpdateBpuPredictors(stream, {missing}));

    stream.isHit = true;
    EXPECT_TRUE(shouldUpdateBpuPredictors(stream, {}));

    stream.isHit = false;
    stream.exeTaken = true;
    EXPECT_TRUE(shouldUpdateBpuPredictors(stream, {}));
}

TEST(UpdateEntryBuilderTest, InvalidSelectedEntryDoesNotTrainTarget)
{
    const ResolvedBranch missing = makeResolvedBranch(0x1008, false, false);

    FetchTarget stream;
    stream.startPC = 0x1000;

    const auto update_branches = makeResolvedUpdateBranches({missing});
    applyResolvedBranchResult(stream, update_branches);
    const BTBUpdateEntrySelection selection;
    const auto update_btb_entries =
        makeUpdateEntries(stream, 32, update_branches);
    const auto update_new_direction_entries =
        makeNewDirectionEntries(
            update_btb_entries, selection, update_branches);
    const auto update_selection =
        markResolvedSelection(selection, update_branches);

    ASSERT_TRUE(update_btb_entries.empty());
    ASSERT_EQ(update_new_direction_entries.size(), 1);
    EXPECT_EQ(update_new_direction_entries[0].pc, missing.pc);
    EXPECT_TRUE(update_new_direction_entries[0].valid);

    const auto direction_entries = buildDirectionUpdateEntries(
        update_btb_entries, update_new_direction_entries,
        update_selection.entry, update_selection.isOldEntry,
        update_branches, DirectionUpdateEntryFilter::Conditional,
        true, stream.makeDirectionUpdateContext());

    ASSERT_EQ(direction_entries.size(), 1);
    EXPECT_EQ(direction_entries[0].entry.pc, missing.pc);
    EXPECT_FALSE(direction_entries[0].actualTaken);

    const auto target_entries = buildTargetUpdateEntries(
        update_btb_entries, update_selection.entry,
        update_selection.isOldEntry, update_branches,
        TargetUpdateEntryFilter::Any, true, stream.makeTargetUpdateContext());

    EXPECT_TRUE(target_entries.empty());
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
