/*
 * Copyright (c) 2026 Beijing Institute of Open Source Chip
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <gtest/gtest.h>

#include <memory>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/cache/prefetch/step.hh"
#include "mem/cache/replacement_policies/lru_rp.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "params/LRURP.hh"
#include "params/SetAssociative.hh"

namespace gem5
{
namespace prefetch
{

class StepSpatialPrefetcherTest : public testing::Test
{
  protected:
    // LRU replacement policy updates timestamps on every table insertion.
    GTestTickHandler tickHandler;
    static constexpr unsigned RegionSize = 4096;
    static constexpr unsigned BlockSize = 64;

    SetAssociativeParams ftIndexParams;
    SetAssociativeParams atIndexParams;
    SetAssociativeParams phtIndexParams;
    LRURPParams ftReplacementParams;
    LRURPParams atReplacementParams;
    LRURPParams phtReplacementParams;
    SetAssociative ftIndex{configureIndex(ftIndexParams, "step.ft_index",
                                          256, 8)};
    SetAssociative atIndex{configureIndex(atIndexParams, "step.at_index",
                                          128, 8)};
    SetAssociative phtIndex{configureIndex(phtIndexParams, "step.pht_index",
                                           512, 8)};
    SetAssociativeParams singleFtIndexParams;
    SetAssociativeParams singleAtIndexParams;
    SetAssociativeParams singlePhtIndexParams;
    SetAssociative singleFtIndex{configureIndex(singleFtIndexParams,
                                                 "step.single_ft_index", 1, 1)};
    SetAssociative singleAtIndex{configureIndex(singleAtIndexParams,
                                                 "step.single_at_index", 1, 1)};
    SetAssociative singlePhtIndex{configureIndex(singlePhtIndexParams,
                                                  "step.single_pht_index", 1, 1)};
    replacement_policy::LRU ftReplacement{configureReplacement(
        ftReplacementParams, "step.ft_replacement")};
    replacement_policy::LRU atReplacement{configureReplacement(
        atReplacementParams, "step.at_replacement")};
    replacement_policy::LRU phtReplacement{configureReplacement(
        phtReplacementParams, "step.pht_replacement")};
    LRURPParams singleFtReplacementParams;
    LRURPParams singleAtReplacementParams;
    LRURPParams singlePhtReplacementParams;
    replacement_policy::LRU singleFtReplacement{configureReplacement(
        singleFtReplacementParams, "step.single_ft_replacement")};
    replacement_policy::LRU singleAtReplacement{configureReplacement(
        singleAtReplacementParams, "step.single_at_replacement")};
    replacement_policy::LRU singlePhtReplacement{configureReplacement(
        singlePhtReplacementParams, "step.single_pht_replacement")};

    static SetAssociativeParams &
    configureIndex(SetAssociativeParams &params, const char *name,
                   unsigned entries, unsigned assoc)
    {
        params.name = name;
        params.eventq_index = 0;
        params.entry_size = 1;
        params.assoc = assoc;
        params.size = entries;
        params.num_slices = 0;
        params.slice_idx = 0;
        return params;
    }

    static LRURPParams &
    configureReplacement(LRURPParams &params, const char *name)
    {
        params.name = name;
        params.eventq_index = 0;
        return params;
    }

    StepSpatialPrefetcher::Config
    config(unsigned confidence_entries = 3, bool enable_soe = false,
           bool single_ft = false, bool single_at = false,
           bool single_pht = false)
    {
        const unsigned ft_entries = single_ft ? 1 : 256;
        const unsigned ft_assoc = single_ft ? 1 : 8;
        const unsigned at_entries = single_at ? 1 : 128;
        const unsigned at_assoc = single_at ? 1 : 8;
        const unsigned pht_entries = single_pht ? 1 : 512;
        const unsigned pht_assoc = single_pht ? 1 : 8;
        return {
            RegionSize,
            BlockSize,
            12,
            confidence_entries,
            75,
            true,
            enable_soe,
            true,
            ft_entries,
            ft_assoc,
            single_ft ? &singleFtIndex : &ftIndex,
            single_ft ? &singleFtReplacement : &ftReplacement,
            at_entries,
            at_assoc,
            single_at ? &singleAtIndex : &atIndex,
            single_at ? &singleAtReplacement : &atReplacement,
            pht_entries,
            pht_assoc,
            single_pht ? &singlePhtIndex : &phtIndex,
            single_pht ? &singlePhtReplacement : &phtReplacement,
        };
    }

    std::unique_ptr<StepSpatialPrefetcher>
    makeStep(unsigned confidence_entries = 3, bool enable_soe = false,
             bool single_ft = false, bool single_at = false,
             bool single_pht = false)
    {
        return std::make_unique<StepSpatialPrefetcher>(
            config(confidence_entries, enable_soe, single_ft, single_at,
                   single_pht), nullptr);
    }

    static Addr
    address(Addr region, unsigned offset)
    {
        return region * RegionSize + offset * BlockSize;
    }

    static void
    trainPattern(StepSpatialPrefetcher &step, Addr region, Addr pc,
                 ContextID context_id, bool secure, unsigned first,
                 unsigned second, unsigned third, unsigned extra)
    {
        EXPECT_FALSE(step.observe(address(region, first), pc, context_id,
                                  secure, false));
        EXPECT_FALSE(step.observe(address(region, second), pc, context_id,
                                  secure, false));
        EXPECT_FALSE(step.observe(address(region, third), pc, context_id,
                                  secure, false));
        EXPECT_FALSE(step.observe(address(region, extra), pc, context_id,
                                  secure, false));
        step.flushActiveEntriesForTest();
    }
};

TEST(StepSpatialPrefetcher, AcceptsConvergentFootprints)
{
    std::array<uint64_t, StepSpatialPrefetcher::MaxHistoryEntries> patterns{};
    patterns[0] = 0b11111;
    patterns[1] = 0b11110;
    patterns[2] = 0b11101;

    EXPECT_TRUE(StepSpatialPrefetcher::footprintsConverge(patterns, 3, 75));
    EXPECT_EQ(StepSpatialPrefetcher::intersectFootprints(patterns, 3), 0b11100);
}

TEST(StepSpatialPrefetcher, RejectsExactConfidenceThreshold)
{
    std::array<uint64_t, StepSpatialPrefetcher::MaxHistoryEntries> patterns{};
    patterns[0] = 0b1111;
    patterns[1] = 0b1110;

    EXPECT_FALSE(StepSpatialPrefetcher::footprintsConverge(patterns, 2, 75));
}

TEST(StepSpatialPrefetcher, RejectsDivergentFootprints)
{
    std::array<uint64_t, StepSpatialPrefetcher::MaxHistoryEntries> patterns{};
    patterns[0] = 0b11110000;
    patterns[1] = 0b00001111;

    EXPECT_FALSE(StepSpatialPrefetcher::footprintsConverge(patterns, 2, 75));
}

TEST(StepSpatialPrefetcher, SingleMatchDoesNotNeedSimilarity)
{
    std::array<uint64_t, StepSpatialPrefetcher::MaxHistoryEntries> patterns{};
    patterns[0] = 0b1010;

    EXPECT_TRUE(StepSpatialPrefetcher::footprintsConverge(patterns, 1, 75));
    EXPECT_EQ(StepSpatialPrefetcher::intersectFootprints(patterns, 1), 0b1010);
}

TEST_F(StepSpatialPrefetcherTest, RepeatedOffsetsAdvanceToToeAndTrain)
{
    auto step = makeStep();
    const Addr region = 0x10;
    const Addr pc = 0x8000;

    // FT gates on the first three accesses, not the first three distinct
    // offsets. A region that repeatedly touches one line must still enter AT
    // and train its FO/SO/TO pattern when it is evicted.
    trainPattern(*step, region, pc, 0, false, 1, 1, 1, 4);
    EXPECT_EQ(step->stats.atAllocations.value(), 1);
    EXPECT_EQ(step->stats.phtInsertions.value(), 1);

    EXPECT_FALSE(step->observe(address(region + 1, 1), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(region + 1, 1), pc, 0, false, true));
    const auto toe = step->observe(address(region + 1, 1), pc, 0, false,
                                   true);
    ASSERT_TRUE(toe);
    EXPECT_EQ(toe->stage, StepSpatialPrefetcher::TriggerStage::Toe);
    EXPECT_EQ(toe->candidates, uint64_t(1) << 4);
    EXPECT_EQ(step->stats.toeLookups.value(), 1);
}

TEST_F(StepSpatialPrefetcherTest, ImmatureFoeDefersThenRecurrenceEnablesIt)
{
    // One PHT way makes the second completed footprint replace the first one
    // in the same history position, exercising STEP's maturity definition.
    auto step = makeStep(1, false, false, false, true);
    const Addr pc = 0x8800;

    trainPattern(*step, 0x20, pc, 0, false, 1, 2, 3, 4);
    const auto first = step->observe(address(0x21, 1), pc, 0, false, true);
    EXPECT_FALSE(first);
    EXPECT_EQ(step->stats.foeDeferredMaturity.value(), 1);

    trainPattern(*step, 0x22, pc, 0, false, 1, 2, 3, 4);
    const auto second = step->observe(address(0x23, 1), pc, 0, false, true);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->stage, StepSpatialPrefetcher::TriggerStage::Foe);
    EXPECT_EQ(second->candidates, (uint64_t(1) << 2) |
                                      (uint64_t(1) << 3) |
                                      (uint64_t(1) << 4));
}

TEST_F(StepSpatialPrefetcherTest, SoeMatchesWhenFoePcDoesNotMatch)
{
    auto step = makeStep(1, true);
    const Addr training_pc = 0x8000;
    const Addr lookup_pc = 0x8800;

    trainPattern(*step, 0x28, training_pc, 0, false, 1, 2, 3, 6);

    // FOE includes the hashed PC and must miss this different context.
    EXPECT_FALSE(step->observe(address(0x29, 1), lookup_pc, 0, false, true));
    EXPECT_EQ(step->stats.foePhtHits.value(), 0);

    // SOE intentionally drops the PC qualifier and matches FO+SO.
    const auto soe = step->observe(address(0x29, 2), lookup_pc, 0, false, true);
    ASSERT_TRUE(soe);
    EXPECT_EQ(soe->stage, StepSpatialPrefetcher::TriggerStage::Soe);
    EXPECT_EQ(soe->candidates, (uint64_t(1) << 3) |
                                (uint64_t(1) << 6));
}

TEST_F(StepSpatialPrefetcherTest,
       PhtMaturityRequiresSamePcInTheReplacedHistoryPosition)
{
    auto step = makeStep(1, false, false, false, true);
    const Addr first_pc = 0x8000;
    const Addr second_pc = 0x8800;

    // PC is not part of the PHT tag. Each completed footprint replaces the
    // sole history position; a different-PC predecessor must not establish
    // maturity for the next entry.
    trainPattern(*step, 0x2a, first_pc, 0, false, 1, 2, 3, 5);
    trainPattern(*step, 0x2b, second_pc, 0, false, 1, 2, 3, 6);
    trainPattern(*step, 0x2c, first_pc, 0, false, 1, 2, 3, 7);

    const auto foe = step->observe(address(0x2d, 1), first_pc, 0, false,
                                   true);
    EXPECT_FALSE(foe);
    EXPECT_EQ(step->stats.foeDeferredMaturity.value(), 1);
}

TEST_F(StepSpatialPrefetcherTest, ToeRequiresAllThreeOffsets)
{
    auto step = makeStep();
    const Addr pc = 0x9000;

    trainPattern(*step, 0x30, pc, 0, false, 1, 2, 3, 5);
    EXPECT_FALSE(step->observe(address(0x31, 1), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(0x31, 2), pc, 0, false, true));
    const auto matching = step->observe(address(0x31, 3), pc, 0, false, true);
    ASSERT_TRUE(matching);
    EXPECT_EQ(matching->stage, StepSpatialPrefetcher::TriggerStage::Toe);
    EXPECT_EQ(matching->candidates, uint64_t(1) << 5);

    EXPECT_FALSE(step->observe(address(0x32, 1), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(0x32, 2), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(0x32, 4), pc, 0, false, true));
    EXPECT_EQ(step->stats.toeMisses.value(), 1);
}

TEST_F(StepSpatialPrefetcherTest,
       SameToeEventsRetainSeparateRecentFootprintsForConfidence)
{
    auto step = makeStep(2);
    const Addr pc = 0x9400;

    // The same FO/SO/TO may lead to different completed footprints. STEP must
    // retain both histories so FOE can reject their low-confidence prediction.
    trainPattern(*step, 0x34, pc, 0, false, 1, 2, 3, 4);
    trainPattern(*step, 0x35, pc, 0, false, 1, 2, 3, 20);
    EXPECT_FALSE(step->observe(address(0x36, 1), pc, 0, false, true));
    EXPECT_EQ(step->stats.foePhtHits.value(), 1);
    EXPECT_EQ(step->stats.foeDeferredConfidence.value(), 1);
}

TEST_F(StepSpatialPrefetcherTest, AtVictimTrainsAUsablePhtHistory)
{
    auto step = makeStep(1, false, false, true);
    const Addr pc = 0x9600;

    // Region 0x38 reaches AT and remains active. Allocating 0x39 forces its
    // eviction through the normal AT-victim path, without using the test-only
    // flush helper.
    EXPECT_FALSE(step->observe(address(0x38, 1), pc, 0, false, false));
    EXPECT_FALSE(step->observe(address(0x38, 2), pc, 0, false, false));
    EXPECT_FALSE(step->observe(address(0x38, 3), pc, 0, false, false));
    EXPECT_FALSE(step->observe(address(0x38, 4), pc, 0, false, false));
    EXPECT_EQ(step->stats.phtInsertions.value(), 0);

    EXPECT_FALSE(step->observe(address(0x39, 1), pc, 0, false, false));
    EXPECT_FALSE(step->observe(address(0x39, 2), pc, 0, false, false));
    EXPECT_FALSE(step->observe(address(0x39, 3), pc, 0, false, false));
    EXPECT_EQ(step->stats.atAllocations.value(), 2);
    EXPECT_EQ(step->stats.phtInsertions.value(), 1);

    EXPECT_FALSE(step->observe(address(0x3a, 1), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(0x3a, 2), pc, 0, false, true));
    const auto toe = step->observe(address(0x3a, 3), pc, 0, false, true);
    ASSERT_TRUE(toe);
    EXPECT_EQ(toe->stage, StepSpatialPrefetcher::TriggerStage::Toe);
    EXPECT_EQ(toe->candidates, uint64_t(1) << 4);
}

TEST_F(StepSpatialPrefetcherTest, ContextAndSecureDomainsDoNotSharePatterns)
{
    auto step = makeStep();
    const Addr pc = 0x9800;

    trainPattern(*step, 0x40, pc, 0, false, 1, 2, 3, 6);
    EXPECT_FALSE(step->observe(address(0x41, 1), pc, 1, false, true));
    EXPECT_FALSE(step->observe(address(0x42, 1), pc, 0, true, true));
    EXPECT_EQ(step->stats.foePhtHits.value(), 0);
}

TEST_F(StepSpatialPrefetcherTest, InvalidContextDoesNotAliasContextZero)
{
    auto step = makeStep();
    const Addr region = 0x48;
    const Addr pc = 0x9a00;

    EXPECT_FALSE(step->observe(address(region, 1), pc, InvalidContextID,
                               false, false));
    EXPECT_FALSE(step->observe(address(region, 1), pc, 0, false, false));
    EXPECT_EQ(step->stats.ftAllocations.value(), 2);

    // Each domain keeps its own first offset. These must be second-offset
    // observations, not duplicate accesses to one shared FT entry.
    EXPECT_FALSE(step->observe(address(region, 2), pc, InvalidContextID,
                               false, false));
    EXPECT_FALSE(step->observe(address(region, 2), pc, 0, false, false));
}

TEST_F(StepSpatialPrefetcherTest, IssuedFoeSuppressesToe)
{
    auto step = makeStep(1, false, false, false, true);
    const Addr pc = 0xa000;

    trainPattern(*step, 0x50, pc, 0, false, 1, 2, 3, 7);
    trainPattern(*step, 0x51, pc, 0, false, 1, 2, 3, 7);
    const auto foe = step->observe(address(0x52, 1), pc, 0, false, true);
    ASSERT_TRUE(foe);
    step->recordBuffered(*foe, foe->candidates);
    step->recordHandoff(foe->region, foe->contextId, foe->secure, foe->id);
    EXPECT_FALSE(step->observe(address(0x52, 2), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(0x52, 3), pc, 0, false, true));
    EXPECT_EQ(step->stats.toeLookups.value(), 0);
}

TEST_F(StepSpatialPrefetcherTest,
       BufferedFoeSuppressesToeUntilItsFinalTerminalFailure)
{
    auto step = makeStep(1, false, false, false, true);
    const Addr pc = 0xa400;

    trainPattern(*step, 0x53, pc, 0, false, 1, 2, 3, 7);
    trainPattern(*step, 0x54, pc, 0, false, 1, 2, 3, 7);
    const auto foe = step->observe(address(0x55, 1), pc, 0, false, true);
    ASSERT_TRUE(foe);

    // A buffered FOE owns a pending PB decision. Later events must not submit
    // an overlapping SOE/TOE while any line of that decision can still reach
    // its target PFQ.
    step->recordBuffered(*foe, foe->candidates);
    EXPECT_FALSE(step->observe(address(0x55, 2), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(0x55, 3), pc, 0, false, true));
    EXPECT_EQ(step->stats.toeLookups.value(), 0);
}

TEST_F(StepSpatialPrefetcherTest,
       MultiLineFoeRestoresToeOnlyAfterEveryTerminalFailure)
{
    auto step = makeStep(1, false, false, false, true);
    const Addr pc = 0xa800;

    trainPattern(*step, 0x54, pc, 0, false, 1, 2, 3, 7);
    trainPattern(*step, 0x55, pc, 0, false, 1, 2, 3, 7);
    const auto partial_foe = step->observe(address(0x56, 1), pc, 0, false,
                                           true);
    ASSERT_TRUE(partial_foe);

    // One rejected candidate is insufficient: a multi-line FOE remains
    // pending until every PB candidate has reached a terminal failure.
    step->recordBuffered(*partial_foe, partial_foe->candidates);
    step->recordBufferHandoffRejected();
    step->recordTerminalFailure(partial_foe->region, partial_foe->contextId,
                                partial_foe->secure, partial_foe->id);
    EXPECT_FALSE(step->observe(address(0x56, 2), pc, 0, false, true));
    EXPECT_FALSE(step->observe(address(0x56, 3), pc, 0, false, true));
    EXPECT_EQ(step->stats.toeLookups.value(), 0);

    // A separate region loses every buffered FOE candidate before its next
    // demand access. Its final failure restores the exact TOE opportunity.
    const auto failed_foe = step->observe(address(0x57, 1), pc, 0, false,
                                          true);
    ASSERT_TRUE(failed_foe);
    step->recordBuffered(*failed_foe, failed_foe->candidates);
    const unsigned candidates = __builtin_popcountll(failed_foe->candidates);
    for (unsigned i = 0; i < candidates; ++i) {
        step->recordTerminalFailure(failed_foe->region,
                                    failed_foe->contextId,
                                    failed_foe->secure, failed_foe->id);
    }
    EXPECT_FALSE(step->observe(address(0x57, 2), pc, 0, false, true));
    const auto toe = step->observe(address(0x57, 3), pc, 0, false, true);
    ASSERT_TRUE(toe);
    EXPECT_EQ(toe->stage, StepSpatialPrefetcher::TriggerStage::Toe);
    EXPECT_EQ(step->stats.toeLookups.value(), 1);
}

}  // namespace prefetch
}  // namespace gem5
