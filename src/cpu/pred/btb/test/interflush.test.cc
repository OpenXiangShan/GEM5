#include <gtest/gtest.h>

#include <cstddef>

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

unsigned interflushBubblePenalty(std::size_t entryCount,
                                 unsigned entryLimit,
                                 unsigned penaltyCycles);

TEST(InterflushTest, NoPenaltyAtOrBelowLimit)
{
    EXPECT_EQ(interflushBubblePenalty(0, 8, 2), 0u);
    EXPECT_EQ(interflushBubblePenalty(8, 8, 2), 0u);
}

TEST(InterflushTest, FixedPenaltyAboveLimit)
{
    EXPECT_EQ(interflushBubblePenalty(9, 8, 2), 2u);
    EXPECT_EQ(interflushBubblePenalty(12, 8, 2), 2u);
}

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
