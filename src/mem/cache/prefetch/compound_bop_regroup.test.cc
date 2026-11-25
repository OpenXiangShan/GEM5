#include <gtest/gtest.h>

#include "mem/cache/prefetch/compound_bop_policy.hh"

namespace gem5::prefetch::compound_bop
{

constexpr Family V = Family::VBOP;
constexpr Family P = Family::PBOP;

TEST(CompoundBOPRegroup, OverlapMustExplainTheRejectedExtra)
{
    EXPECT_FALSE(redundantExtra(0, 0, 0, 2, 80));
    EXPECT_FALSE(redundantExtra(19, 19, 1000, 2, 80)); // Too weak alone.
    EXPECT_FALSE(redundantExtra(50, 39, 1000, 2, 80)); // Below 80%.
    EXPECT_FALSE(redundantExtra(100, 80, 1000, 2, 80)); // 2% is accepted.
    EXPECT_TRUE(redundantExtra(50, 40, 1000, 2, 80)); // Exactly 80%.
    EXPECT_TRUE(redundantExtra(100, 81, 1000, 2, 80));
}

TEST(CompoundBOPRegroup, FourRedundantWinnersMakeRoomForFifth)
{
    const std::vector<TeacherEntry> entries = {
        {32, 16, V}, {2, 12, P}, {4, 11, P}, {6, 10, P}, {8, 9, P},
        {1, 5, P}};
    FamilyAssignments groups(entries.size(), 8);
    for (size_t redundant = 1; redundant <= 4; ++redundant) {
        auto before = groups.bestInFamilies(entries);
        ASSERT_EQ(before[0], 0U);
        ASSERT_EQ(before[1], redundant);
        EXPECT_FALSE(groups.advance());
        ASSERT_TRUE(groups.moveTo(redundant, P, V));
        EXPECT_EQ(groups.bestInFamilies(entries)[1], redundant + 1);
    }
    // S5 can be nominated despite its lower standalone Teacher score.
    EXPECT_EQ(entries[*groups.bestInFamilies(entries)[1]].offset, 1);
    for (unsigned i = 0; i < 4; ++i) {
        EXPECT_FALSE(groups.advance());
    }
    EXPECT_EQ(groups.advance(), 1U);
    EXPECT_EQ(groups.bestInFamilies(entries)[1], 1U);
    EXPECT_EQ(groups.family(2, P), V);
}

TEST(CompoundBOPRegroup, FullTwelvePhaseLeaseWithoutCountermove)
{
    FamilyAssignments groups(3, 12);
    groups.advance();
    ASSERT_TRUE(groups.moveTo(0, V, P));
    for (unsigned i = 1; i < 12; ++i) {
        EXPECT_FALSE(groups.advance());
        EXPECT_FALSE(groups.moveTo(0, V, V));
        EXPECT_EQ(groups.family(0, V), P);
        EXPECT_EQ(groups.family(1, P), P);
        EXPECT_EQ(groups.family(2, V), V);
    }
    EXPECT_EQ(groups.advance(), 0U);
    EXPECT_EQ(groups.family(0, V), V);
}

TEST(CompoundBOPRegroup, EmptyFamilyAndStaticControl)
{
    const std::vector<TeacherEntry> entries = {{32, 2, V}, {1, 2, P}};
    FamilyAssignments fixed(entries.size(), 0);
    EXPECT_FALSE(fixed.advance());
    EXPECT_FALSE(fixed.moveTo(0, V, P));
    EXPECT_EQ(fixed.bestInFamilies(entries)[0], 0U);
    FamilyAssignments changing(entries.size(), 1);
    changing.advance();
    ASSERT_TRUE(changing.moveTo(0, V, P));
    EXPECT_FALSE(changing.bestInFamilies(entries)[0]);
    EXPECT_EQ(changing.bestInFamilies(entries)[1], 0U); // Stable tie order.
    EXPECT_EQ(changing.advance(), 0U);
    EXPECT_EQ(changing.bestInFamilies(entries)[1], 1U);
}

TEST(CompoundBOPRegroup, ReturnInvalidatesOldSlotAndKeepsPrimary)
{
    // -4 was born in V and was selected from P before its lease expired.
    std::vector<StudentEntry> slots = {{true, V, 32}, {true, P, -4}};
    std::vector<SelectedOffset> pair = {{-4, P}, {32, V}};
    auto change = reconcileFamilies(slots, pair, [](int64_t) { return V; });
    EXPECT_EQ(change.invalidated, 1U);
    EXPECT_FALSE(slots[1].valid);
    EXPECT_FALSE(change.canAdmit(-4));
    EXPECT_TRUE(change.canAdmit(32));
    EXPECT_TRUE(slots[0].valid);
    EXPECT_TRUE(change.extraRemoved);
    ASSERT_EQ(pair.size(), 1U);
    EXPECT_EQ(pair[0].offset, -4);
    EXPECT_EQ(pair[0].family, V);
}

} // namespace gem5::prefetch::compound_bop
