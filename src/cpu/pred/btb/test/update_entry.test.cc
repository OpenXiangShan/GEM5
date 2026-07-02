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

    const auto old_entry =
        buildSelectedTargetUpdateEntry(selected, true, ctx);
    EXPECT_EQ(old_entry.entry.pc, selected.pc);
    EXPECT_TRUE(old_entry.actualTaken);
    EXPECT_FALSE(old_entry.isNewEntry);
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

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
