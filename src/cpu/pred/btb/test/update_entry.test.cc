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
makeEntry(Addr pc, bool is_cond, bool always_taken, bool resolved)
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
    entry.alwaysTaken = always_taken;
    entry.ctr = 0;
    return entry;
}

BTBEntry
makeIndirectEntry(Addr pc, bool is_return, bool resolved)
{
    BTBEntry entry = makeEntry(pc, false, false, resolved);
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

} // namespace

TEST(UpdateEntryBuilderTest, DirectionResolvedPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x1000, true, false, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x1004, true, false, true);
    const DirectionUpdateContext ctx =
        makeDirectionContext(prefix_entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {prefix_entry, legacy_resolved_entry}, BTBEntry(), true,
        {prefix_entry.pc}, DirectionUpdateEntryFilter::ConditionalNonAlwaysTaken,
        true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, prefix_entry.pc);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, DirectionNewNotTakenEntryClearsAlwaysTaken)
{
    const BTBEntry new_entry = makeEntry(0x1010, true, true, false);
    const DirectionUpdateContext ctx =
        makeDirectionContext(new_entry.pc, false);

    const auto entries = buildDirectionUpdateEntries(
        {}, new_entry, false, {},
        DirectionUpdateEntryFilter::ConditionalNonAlwaysTaken, false, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, new_entry.pc);
    EXPECT_FALSE(entries[0].entry.alwaysTaken);
    EXPECT_FALSE(entries[0].actualTaken);
    EXPECT_TRUE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, MgscResolvedUpdateKeepsLegacyOldEntriesWithoutPrefix)
{
    const BTBEntry always_entry = makeEntry(0x1020, false, true, false);
    const DirectionUpdateContext ctx =
        makeDirectionContext(always_entry.pc, true);

    const auto entries = buildDirectionUpdateEntries(
        {always_entry}, BTBEntry(), true, {},
        DirectionUpdateEntryFilter::Mgsc, true, ctx);

    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries[0].entry.pc, always_entry.pc);
    EXPECT_TRUE(entries[0].entry.alwaysTaken);
    EXPECT_TRUE(entries[0].actualTaken);
    EXPECT_FALSE(entries[0].isNewEntry);
}

TEST(UpdateEntryBuilderTest, TargetResolvedPrefixOverridesEntryResolvedBits)
{
    const BTBEntry prefix_entry = makeEntry(0x2000, false, false, false);
    const BTBEntry legacy_resolved_entry = makeEntry(0x2004, false, false, true);
    const TargetUpdateContext ctx = makeTargetContext(prefix_entry.pc, false);

    const auto entries = buildTargetUpdateEntries(
        {prefix_entry, legacy_resolved_entry}, BTBEntry(), true,
        {prefix_entry.pc}, TargetUpdateEntryFilter::Any, true, ctx);

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

TEST(UpdateEntryBuilderTest, SelectedTargetEntryUsesContextAndOldEntryFlag)
{
    const BTBEntry selected = makeEntry(0x4000, false, false, true);
    const TargetUpdateContext ctx = makeTargetContext(selected.pc, true);

    const auto new_entry =
        buildSelectedTargetUpdateEntry(selected, false, ctx);
    EXPECT_EQ(new_entry.entry.pc, selected.pc);
    EXPECT_TRUE(new_entry.actualTaken);
    EXPECT_TRUE(new_entry.isNewEntry);
    EXPECT_EQ(new_entry.actualBranch.pc, selected.pc);
    EXPECT_EQ(new_entry.actualBranch.target, ctx.actualBranch.target);

    const auto old_entry =
        buildSelectedTargetUpdateEntry(selected, true, ctx);
    EXPECT_EQ(old_entry.entry.pc, selected.pc);
    EXPECT_TRUE(old_entry.actualTaken);
    EXPECT_FALSE(old_entry.isNewEntry);
    EXPECT_EQ(old_entry.actualBranch.pc, selected.pc);
    EXPECT_EQ(old_entry.actualBranch.target, ctx.actualBranch.target);
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
    const BTBEntry before = makeEntry(0x0ffc, true, false, true);
    const BTBEntry first = makeEntry(0x1000, true, false, true);
    const BTBEntry second = makeEntry(0x1008, true, false, true);
    const BTBEntry after = makeEntry(0x1010, true, false, true);
    BTBEntry invalid = makeEntry(0x1004, true, false, true);
    invalid.valid = false;

    const auto entries = buildUpdateBTBEntries(
        {before, first, invalid, second, after}, 0x1000, 0x1008);

    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].pc, first.pc);
    EXPECT_EQ(entries[1].pc, second.pc);
}

TEST(UpdateEntryBuilderTest, BPUUpdateEventPreparesLegacyTarget)
{
    const BTBEntry first = makeEntry(0x1000, true, false, false);
    const BTBEntry second = makeEntry(0x1008, true, false, false);
    const BTBEntry after = makeEntry(0x1010, true, false, false);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {first, second, after};

    const BPUUpdateEvent event = BPUUpdateEvent::fromResolvedBranches({
        makeResolvedBranch(first.pc, false, false),
        makeResolvedBranch(second.pc, true, true),
        makeResolvedBranch(after.pc, false, false),
    });
    event.prepareLegacyTarget(
        stream,
        32,
        [](const TargetUpdateContext &ctx) {
            return BTBUpdateEntrySelection{BTBEntry(ctx.actualBranch), false};
        });

    EXPECT_TRUE(stream.resolved);
    EXPECT_TRUE(stream.exeTaken);
    EXPECT_EQ(stream.exeBranchInfo.pc, second.pc);
    EXPECT_EQ(stream.exeBranchInfo.target, second.pc + 0x200);

    ASSERT_EQ(stream.updateBTBEntries.size(), 2);
    EXPECT_EQ(stream.updateBTBEntries[0].pc, first.pc);
    EXPECT_EQ(stream.updateBTBEntries[1].pc, second.pc);
    EXPECT_TRUE(stream.updateBTBEntries[0].resolved);
    EXPECT_TRUE(stream.updateBTBEntries[1].resolved);

    EXPECT_EQ(stream.updateNewBTBEntry.pc, second.pc);
    EXPECT_TRUE(stream.updateNewBTBEntry.resolved);
    EXPECT_FALSE(stream.updateIsOldEntry);
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

TEST(UpdateEntryBuilderTest, BPUUpdateEventFromFetchTargetUsesResolvedPrefix)
{
    const BTBEntry first = makeEntry(0x1000, true, false, false);
    const BTBEntry second = makeEntry(0x1008, true, false, false);
    const BTBEntry after = makeEntry(0x1010, true, false, false);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.predBTBEntries = {first, second, after};
    stream.addResolvedBranches({
        makeResolvedBranch(after.pc, false, false),
        makeResolvedBranch(second.pc, true, true),
        makeResolvedBranch(first.pc, false, false),
    });

    const BPUUpdateEvent event = BPUUpdateEvent::fromFetchTarget(stream);
    EXPECT_EQ(event.resolvedBranchCount(), 2);
    event.prepareLegacyTarget(
        stream,
        32,
        [](const TargetUpdateContext &ctx) {
            return BTBUpdateEntrySelection{BTBEntry(ctx.actualBranch), false};
        });

    EXPECT_TRUE(stream.resolved);
    EXPECT_TRUE(stream.exeTaken);
    EXPECT_EQ(stream.exeBranchInfo.pc, second.pc);

    ASSERT_EQ(stream.updateBTBEntries.size(), 2);
    EXPECT_EQ(stream.updateBTBEntries[0].pc, first.pc);
    EXPECT_EQ(stream.updateBTBEntries[1].pc, second.pc);
    EXPECT_EQ(stream.updateNewBTBEntry.pc, second.pc);
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
