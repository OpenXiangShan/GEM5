/*
 * Copyright (c) 2026 XiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "mem/cache/replacement_policies/fdp_logic.hh"

using gem5::replacement_policy::fdp::Feedback;
using gem5::replacement_policy::fdp::Stack;

TEST(FDPStack, ExactInsertionAndPromotion)
{
    Stack stack(8);
    for (unsigned i = 0; i < 8; ++i)
        stack.insert(i, false, 0);
    stack.insert(0, true, 4);
    EXPECT_EQ(stack.lines[0].rank, 4);
    EXPECT_EQ(stack.lines[1].rank, 7);
    EXPECT_EQ(stack.lines[4].rank, 3);
    EXPECT_TRUE(stack.lines[0].unusedPrefetch);
    EXPECT_FALSE(stack.lines[0].demandFetched);
    stack.touch(0, true);
    EXPECT_EQ(stack.lines[0].rank, 0);
    EXPECT_FALSE(stack.lines[0].unusedPrefetch);
    EXPECT_EQ(stack.lines[1].rank, 7);
    stack.insert(1, true, 6);
    EXPECT_EQ(stack.lines[1].rank, 6);
    EXPECT_EQ(stack.lines[2].rank, 7);
}

TEST(FDPStack, InvalidWaysAndPartialSets)
{
    Stack stack(2);
    stack.insert(1, true, 1);
    EXPECT_EQ(stack.lines[1].rank, 0);
    stack.insert(0, false, 0);
    EXPECT_EQ(stack.lines[1].rank, 1);
    stack.invalidate(0);
    EXPECT_EQ(stack.lines[1].rank, 0);
    stack.invalidate(0);
    stack.insert(0, true, 1);
    EXPECT_EQ(stack.lines[0].rank, 1);
}

TEST(FDPStack, PreservePermutationThroughMixedOperations)
{
    Stack stack(8);
    std::mt19937 rng(19);
    for (unsigned i = 0; i < 10000; ++i) {
        const unsigned way = rng() % 8;
        switch (rng() % 3) {
          case 0:
            stack.invalidate(way);
            break;
          case 1:
            stack.insert(way, true, rng() % 8);
            break;
          case 2:
            if (stack.lines[way].valid)
                stack.touch(way, true);
            break;
        }
        std::vector<unsigned> ranks;
        for (const auto &line : stack.lines) {
            if (line.valid)
                ranks.push_back(line.rank);
        }
        std::sort(ranks.begin(), ranks.end());
        for (unsigned j = 0; j < ranks.size(); ++j)
            ASSERT_EQ(ranks[j], j);
    }
}

TEST(FDPFeedback, PollutionAndSecurityDomains)
{
    Feedback feedback(6, 12, 16, 0.005, 0.25);
    feedback.evict(0x8000, false, true, true, true);
    EXPECT_TRUE(feedback.demandMiss(0x8000, false));
    EXPECT_FALSE(feedback.demandMiss(0x8000, true));
    feedback.prefetchFill(0x8000, false);
    EXPECT_FALSE(feedback.demandMiss(0x8000, false));
    feedback.evict(0x9000, false, false, true, true);
    EXPECT_FALSE(feedback.demandMiss(0x9000, false));
    feedback.evict(0xa000, false, true, true, false);
    EXPECT_FALSE(feedback.demandMiss(0xa000, false));
}

TEST(FDPFeedback, FoldedAddressHash)
{
    Feedback feedback(6, 12, 16, 0.005, 0.25);
    feedback.evict(64, false, true, true, true);
    EXPECT_TRUE(feedback.demandMiss(uint64_t(1) << 18, false));
    EXPECT_FALSE(feedback.demandMiss(128, false));
}

TEST(FDPFeedback, UsefulEvictionsControlIntervals)
{
    Feedback feedback(6, 12, 2, 0.005, 0.25);
    EXPECT_FALSE(feedback.evict(0x8000, false, true, true, true));
    feedback.demandMiss(0x8000, false);
    for (unsigned i = 0; i < 100; ++i)
        EXPECT_FALSE(feedback.evict(0x9000, false, false, false, true));
    EXPECT_EQ(feedback.getPosition(), Feedback::Mid);
    EXPECT_TRUE(feedback.evict(0x9000, false, true, true, false));
    EXPECT_EQ(feedback.getPosition(), Feedback::Lru);
    EXPECT_EQ(feedback.insertionRank(8), 7);
    EXPECT_DOUBLE_EQ(feedback.getPollution(), 1);
}

TEST(FDPFeedback, SmoothedCountersChooseAllThreePositions)
{
    Feedback feedback(6, 12, 1, 0.005, 0.25);
    EXPECT_EQ(feedback.insertionRank(8), 4);
    // No demand samples must neither divide by zero nor invent pollution.
    EXPECT_TRUE(feedback.evict(0x8000, false, true, true, true));
    EXPECT_EQ(feedback.getPosition(), Feedback::Mid);
    feedback.demandMiss(0x8000, false);
    for (unsigned i = 0; i < 9; ++i)
        feedback.demandMiss(0x9000, false);
    EXPECT_TRUE(feedback.evict(0, false, true, true, false));
    EXPECT_EQ(feedback.getPosition(), Feedback::Quarter);
    EXPECT_EQ(feedback.insertionRank(8), 6);
    EXPECT_DOUBLE_EQ(feedback.getPollution(), 0.1);
    for (unsigned i = 0; i < 1000; ++i)
        feedback.demandMiss(0x9000, false);
    EXPECT_TRUE(feedback.evict(0, false, true, true, false));
    EXPECT_EQ(feedback.getPosition(), Feedback::Mid);
    EXPECT_EQ(feedback.insertionRank(1), 0);
    EXPECT_EQ(feedback.insertionRank(2), 1);
}

TEST(FDPFeedback, TaggedFilterRejectsAliasesAndConsumesEvidenceOnce)
{
    Feedback feedback(6, 12, 16, 0.005, 0.25, true);
    feedback.evict(64, false, true, true, true);
    EXPECT_FALSE(feedback.demandMiss(uint64_t(1) << 18, false));
    EXPECT_EQ(feedback.rejectedAliases(), 1);
    // An unrelated refill must not erase the original tagged record.
    feedback.prefetchFill(uint64_t(1) << 18, false);
    EXPECT_TRUE(feedback.demandMiss(64, false));
    EXPECT_FALSE(feedback.demandMiss(64, false));
    feedback.evict(64, false, true, true, true);
    feedback.demandFill(64, false);
    EXPECT_FALSE(feedback.demandMiss(64, false));
}

TEST(FDPFeedback, PaperFilterRetainsItsOriginalSemantics)
{
    Feedback feedback(6, 12, 16, 0.005, 0.25);
    feedback.evict(64, false, true, true, true);
    EXPECT_TRUE(feedback.demandMiss(uint64_t(1) << 18, false));
    EXPECT_TRUE(feedback.demandMiss(64, false));
    feedback.demandFill(64, false);
    EXPECT_TRUE(feedback.demandMiss(64, false));
    feedback.prefetchFill(uint64_t(1) << 18, false);
    EXPECT_FALSE(feedback.demandMiss(64, false));
}
