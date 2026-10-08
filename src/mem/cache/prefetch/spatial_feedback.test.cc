#include <gtest/gtest.h>

#include "mem/cache/prefetch/spatial_feedback.hh"

namespace gem5::prefetch
{

TEST(LldpSpatialFeedback, SlotReplacementRevokesAndRejectsOldTokens)
{
    using Source = PrefetchSourceType;
    SpatialFeedbackTable table;
    SpatialFeedback old{Source::SStream, 0x100, 0, 3, 11, true};
    ASSERT_TRUE(table.update(old));
    ASSERT_TRUE(table.contains(old.pc, old.context, old.source));
    EXPECT_FALSE(table.contains(old.pc, 1, old.source));
    EXPECT_FALSE(table.contains(old.pc, 0, Source::StoreStream));
    EXPECT_FALSE(table.update(old));
    SpatialFeedback next{Source::SStride, 0x100, 1, 3, 12, true};
    ASSERT_TRUE(table.update(next));
    EXPECT_FALSE(table.contains(old.pc, old.context, old.source));
    EXPECT_TRUE(table.contains(next.pc, next.context, next.source));
    old.valid = false;
    EXPECT_FALSE(table.update(old));
    EXPECT_TRUE(table.contains(next.pc, next.context, next.source));
    old.valid = true;
    EXPECT_FALSE(table.update(old));
    next.valid = false;
    ASSERT_TRUE(table.update(next));
    EXPECT_FALSE(table.contains(next.pc, next.context, next.source));
    next.valid = true;
    EXPECT_FALSE(table.update(next));
    ++next.generation;
    ASSERT_TRUE(table.update(next));
    EXPECT_TRUE(table.contains(next.pc, next.context, next.source));
}

TEST(LldpSpatialFeedback, BoundedSlotsAndIndependentOwners)
{
    SpatialFeedbackTable table;
    for (unsigned i = 0; i < 32; ++i)
        ASSERT_TRUE(table.update({PrefetchSourceType::SStride, 0x100 + i * 4,
                                  0, i, 1, true}));
    EXPECT_FALSE(table.update({PrefetchSourceType::SStride, 0x100, 0, 32, 1, true}));
    EXPECT_FALSE(table.update({PrefetchSourceType::SStride, 0x100, 0, 0, 0, true}));
    ASSERT_TRUE(table.update({PrefetchSourceType::SStride, 0x100, 0, 1, 2, true}));
    ASSERT_TRUE(table.update({PrefetchSourceType::SStride, 0x100, 0, 0, 1, false}));
    // Slot one still owns this PC; revoking slot zero cannot undo it.
    EXPECT_TRUE(table.contains(0x100, 0, PrefetchSourceType::SStride));
    EXPECT_FALSE(table.contains(0x104, 0, PrefetchSourceType::SStride));
}

} // namespace gem5::prefetch
