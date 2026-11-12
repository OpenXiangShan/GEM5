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
using gem5::replacement_policy::fdp::SampledInsertion;

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
    stack.touch(0, false);
    EXPECT_TRUE(stack.lines[0].unusedPrefetch);
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

TEST(FDPConfidence, SourceIsolationAndThreshold)
{
    gem5::replacement_policy::fdp::SourceConfidence confidence(3);
    EXPECT_FALSE(confidence.protect(0));
    for (unsigned i = 0; i < 15; ++i)
        confidence.train(0, true);
    EXPECT_FALSE(confidence.protect(0));
    confidence.train(0, true);
    EXPECT_TRUE(confidence.protect(0));
    EXPECT_FALSE(confidence.protect(1));
    confidence.train(0, false);
    EXPECT_FALSE(confidence.protect(0));
}

TEST(FDPConfidence, SaturatesWithoutWrapping)
{
    gem5::replacement_policy::fdp::SourceConfidence confidence(2);
    for (unsigned i = 0; i < 1000; ++i) {
        confidence.train(0, true);
        confidence.train(1, false);
    }
    EXPECT_TRUE(confidence.protect(0));
    EXPECT_FALSE(confidence.protect(1));
    for (unsigned i = 0; i < 16; ++i)
        confidence.train(0, false);
    EXPECT_FALSE(confidence.protect(0));
}

TEST(FDPSampling, BalancedLeadersAcrossIndexSpace)
{
    for (unsigned sets : {32U, 64U, 1024U, 4096U}) {
        std::array<unsigned, 3> counts = {};
        for (unsigned set = 0; set < sets; ++set)
            ++counts[SampledInsertion::classifySet(set)];
        EXPECT_EQ(counts[SampledInsertion::FdpSample], sets / 32);
        EXPECT_EQ(counts[SampledInsertion::LruSample], sets / 32);
        EXPECT_EQ(counts[SampledInsertion::Follower], sets * 30 / 32);
    }
}

TEST(FDPSampling, FollowsDemandFillAdvantageAndKeepsLeadersFixed)
{
    SampledInsertion monitor;
    EXPECT_TRUE(monitor.useLru(SampledInsertion::Follower));
    // FDP wins if the equally sized LRU samples need more demand fills.
    for (unsigned i = 0; i < 200; ++i) {
        monitor.demandFill(SampledInsertion::LruSample);
        monitor.demandFill(SampledInsertion::FdpSample);
        monitor.demandFill(SampledInsertion::LruSample);
    }
    EXPECT_FALSE(monitor.useLru(SampledInsertion::Follower));
    EXPECT_FALSE(monitor.useLru(SampledInsertion::FdpSample));
    EXPECT_TRUE(monitor.useLru(SampledInsertion::LruSample));
    // A later phase with more FDP fills must be able to reverse the choice.
    for (unsigned i = 0; i < 400; ++i) {
        monitor.demandFill(SampledInsertion::FdpSample);
        monitor.demandFill(SampledInsertion::LruSample);
        monitor.demandFill(SampledInsertion::FdpSample);
    }
    EXPECT_TRUE(monitor.useLru(SampledInsertion::Follower));
    EXPECT_FALSE(monitor.useLru(SampledInsertion::FdpSample));
}

TEST(FDPSampling, FollowersDoNotTrainAndSelectorSaturates)
{
    SampledInsertion monitor;
    const unsigned initial = monitor.value();
    for (unsigned i = 0; i < 2000; ++i)
        monitor.demandFill(SampledInsertion::Follower);
    EXPECT_EQ(monitor.value(), initial);
    for (unsigned i = 0; i < 2000; ++i)
        monitor.demandFill(SampledInsertion::FdpSample);
    EXPECT_EQ(monitor.value(), 1023);
    for (unsigned i = 0; i < 2000; ++i)
        monitor.demandFill(SampledInsertion::LruSample);
    EXPECT_EQ(monitor.value(), 0);
}

TEST(FDPFeedback, AttributesPollutionToItsSourceOnceAndBySecurityDomain)
{
    Feedback feedback(6, 12, 16, 0.005, 0.25, true, true);
    feedback.evict(64, false, true, true, true, 3);
    EXPECT_FALSE(feedback.demandMiss(uint64_t(1) << 18, false));
    EXPECT_EQ(feedback.lastPolluter(), 0);
    EXPECT_FALSE(feedback.demandMiss(64, true));
    EXPECT_EQ(feedback.lastPolluter(), 0);
    EXPECT_TRUE(feedback.demandMiss(64, false));
    EXPECT_EQ(feedback.lastPolluter(), 3);
    EXPECT_FALSE(feedback.demandMiss(64, false));
    EXPECT_EQ(feedback.lastPolluter(), 0);
    feedback.evict(64, true, true, true, true, 7);
    EXPECT_TRUE(feedback.demandMiss(64, true));
    EXPECT_EQ(feedback.lastPolluter(), 7);
}

TEST(FDPFeedback, ReplacedOrRefilledRecordsCannotChargeAnOldSource)
{
    Feedback feedback(6, 12, 16, 0.005, 0.25, true, true);
    feedback.evict(64, false, true, true, true, 3);
    const uint64_t alias = uint64_t(1) << 18;
    feedback.evict(alias, false, true, true, true, 7);
    EXPECT_FALSE(feedback.demandMiss(64, false));
    EXPECT_EQ(feedback.lastPolluter(), 0);
    EXPECT_TRUE(feedback.demandMiss(alias, false));
    EXPECT_EQ(feedback.lastPolluter(), 7);
    feedback.evict(64, false, true, true, true, 3);
    feedback.demandFill(64, false);
    EXPECT_FALSE(feedback.demandMiss(64, false));
    EXPECT_EQ(feedback.lastPolluter(), 0);
    // An unused prefetch victim and a demand-caused victim are not pollution.
    feedback.evict(128, false, false, false, true, 3);
    EXPECT_FALSE(feedback.demandMiss(128, false));
    feedback.evict(256, false, true, true, false, 3);
    EXPECT_FALSE(feedback.demandMiss(256, false));
}

TEST(FDPFeedback, UtilityIncludesConsumedPrefetchVictimsButPaperDoesNot)
{
    Feedback paper(6, 12, 16, 0.005, 0.25, true);
    Feedback utility(6, 12, 16, 0.005, 0.25, true, true);
    paper.evict(64, false, false, true, true, 3);
    utility.evict(64, false, false, true, true, 3);
    EXPECT_FALSE(paper.demandMiss(64, false));
    EXPECT_TRUE(utility.demandMiss(64, false));
    EXPECT_EQ(utility.lastPolluter(), 3);
    // A prefetch never consumed by demand still provides no pollution evidence.
    utility.evict(128, false, false, false, true, 7);
    EXPECT_FALSE(utility.demandMiss(128, false));
    EXPECT_EQ(utility.lastPolluter(), 0);
}
