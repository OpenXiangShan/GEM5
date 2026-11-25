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

TEST(CompoundBOPRegroup, DrawsEveryNonPrimaryRegardlessOfScoreOrResidency)
{
    const std::vector<TeacherEntry> entries = {
        {32, 1, V}, {1, 20, P}, {64, 18, V}, {96, 0, V}, {2, 4, P}};
    for (size_t rank = 0; rank < 2; ++rank) {
        FamilyAssignments groups(entries.size(), true);
        auto partner = groups.swapExtra(entries, 1, 0, [&](size_t n) {
            EXPECT_EQ(n, 2U);
            return rank;
        });
        ASSERT_TRUE(partner);
        EXPECT_EQ(*partner, 2 + rank);
        EXPECT_EQ(groups.family(0, V), V);
        EXPECT_EQ(groups.family(1, P), V);
        EXPECT_EQ(groups.family(*partner, V), P);
        std::vector<StudentEntry> residents = {
            {true, V, 32}, {true, P, 1}, {true, V, 64}};
        std::vector<SelectedOffset> selected = {{32, V}};
        auto change = reconcileFamilies(residents, selected, [&](int64_t off) {
            for (size_t i = 0; i < entries.size(); ++i) {
                if (off == entries[i].offset) {
                    return groups.family(i, entries[i].home);
                }
            }
            ADD_FAILURE() << "unknown resident";
            return P;
        });
        EXPECT_TRUE(residents[0].valid);
        EXPECT_FALSE(residents[1].valid);
        EXPECT_EQ(residents[2].valid, rank != 0);
        EXPECT_EQ(change.invalidated, rank == 0 ? 2U : 1U);
        EXPECT_FALSE(change.canAdmit(1));
        EXPECT_EQ(change.canAdmit(64), rank != 0);
        EXPECT_TRUE(change.canAdmit(32));
        ASSERT_EQ(selected.size(), 1U);
        EXPECT_EQ(selected[0].offset, 32);
    }
}

TEST(CompoundBOPRegroup, MaintainsSizesAndUsesCurrentFamiliesOnEverySwap)
{
    std::vector<TeacherEntry> entries;
    for (size_t i = 0; i < 109; ++i) {
        entries.push_back({static_cast<int64_t>(i + 1), 3, i < 55 ? V : P});
    }
    FamilyAssignments groups(entries.size(), true);
    size_t extra = 55;
    for (size_t phase = 0; phase < 200; ++phase) {
        const auto target = phase % 2 ? P : V;
        const size_t primary = phase % 2 ? 108 : 0;
        const auto partner = groups.swapExtra(entries, extra, primary,
            [&](size_t count) {
                EXPECT_EQ(count, target == V ? 54U : 53U);
                return (phase * 17) % count;
            });
        ASSERT_TRUE(partner);
        EXPECT_NE(*partner, primary);
        EXPECT_EQ(groups.family(primary, entries[primary].home), target);
        EXPECT_EQ(groups.family(extra, entries[extra].home), target);
        unsigned v_count = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            v_count += groups.family(i, entries[i].home) == V;
        }
        EXPECT_EQ(v_count, 55U);
        // The same offset can move again, but never just because time passes.
        for (unsigned i = 0; i < 12; ++i) {
            groups.bestInFamilies(entries);
        }
        EXPECT_EQ(groups.family(extra, entries[extra].home), target);
    }
}

TEST(CompoundBOPRegroup, CannotDrawTheOnlyPrimaryOrDrawWhenDisabled)
{
    const std::vector<TeacherEntry> entries = {{32, 1, V}, {1, 2, P}};
    auto never = [](size_t) { ADD_FAILURE() << "unexpected RNG draw"; return 0; };
    FamilyAssignments active(entries.size(), true), fixed(entries.size(), false);
    EXPECT_FALSE(active.swapExtra(entries, 1, 0, never));
    EXPECT_FALSE(fixed.swapExtra(entries, 1, 0, never));
    EXPECT_EQ(active.family(1, P), P);
    EXPECT_EQ(active.family(0, V), V);
}

} // namespace gem5::prefetch::compound_bop
