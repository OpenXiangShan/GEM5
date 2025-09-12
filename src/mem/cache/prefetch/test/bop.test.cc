#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>

#include "base/types.hh"
#include "common/events.hh"
#include "common/mock_types.hh"
#include "mem/cache/prefetch/test/bop.hh"

namespace gem5
{
namespace prefetch
{
namespace test
{
  // Helper to create a PrefetchInfo object
  PrefetchInfo createPrefetchInfo(Addr addr) {
      return PrefetchInfo(addr, true);
  }

  // Test fixture for BOP tests
  class BOPTest : public ::testing::Test
  {
    protected:
      BOP *bop;
      BOPPrefetcherParams params;
      std::vector<AddrPriority> prefetch_candidates;
      SimpleEventQueue eventQueue;

      void SetUp() override {
          params.name = "BOPTest";
          params.block_size = 64;

          // BOP parameters
          params.score_max = 20;
          params.round_max = 50;
          params.bad_score = 12;
          params.rr_size = 256;
          params.tag_bits = 24;
          params.negative_offsets_enable = false;
          params.delay_queue_enable = true;
          params.delay_queue_size = 64;
          params.delay_queue_cycles = Cycles(150);
          params.autoLearning = false;
          params.offsets = {1}; // Use a simple offset for testing
          params.crossPage = true;
          params.victimOffsetsListSize = 10;
          params.restoreCycle = 250000;

          bop = new BOP(params, &eventQueue);
      }

      void TearDown() override {
          delete bop;
      }

    public:
      /* Helper function to issue a demand miss to the BOP */
      void issueDmdMissToBop(Addr addr) {
        PrefetchInfo pfi = createPrefetchInfo(addr);
        prefetch_candidates.clear();
        bop->calculatePrefetch(pfi, prefetch_candidates, false);
      }

      void advanceTime(SimpleCycle cycles) {
        eventQueue.advanceTo(eventQueue.curCycle() + cycles);
      }

      /* Helper function to train the BOP with a constant offset of given round */
      void trainBopWithConstantOffset(Addr startAddr, int offset, int round) {
        for (int i = 0; i < round; ++i) {
          Addr addr = startAddr + (i * params.block_size * offset);
          issueDmdMissToBop(addr);
          advanceTime(params.delay_queue_cycles);
        }
      }


  };

  // Test case to verify that the BOP prefetcher can be initialized without crashing.
  TEST_F(BOPTest, Initialization) {
    SUCCEED();
  }

  // Test case to check if a prefetch is generated for the first time.
  // The BOP prefetcher should not issue a prefetch on the very first access,
  // as it has not had a chance to learn any patterns yet.
  TEST_F(BOPTest, FirstPrefetch) {
    // BOP should not generate a prefetch for the first time
    issueDmdMissToBop(0x1000);
    ASSERT_TRUE(prefetch_candidates.empty());
  }

  // Test case to check if a prefetch is generated after a simple learning phase.
  // This test provides a simple, consistent stride pattern and verifies that
  // the BOP can learn the correct offset and start issuing prefetches.
  TEST_F(BOPTest, LearnAndPrefetch) {
      // We configured the BOP with a single offset of 1.
      // Let's simulate a stride of +1 cache blocks to make it learn.
      // The prefetcher needs to iterate through its offset list and rounds.
      // round_max is 50, score_max is 20. A single offset means it will be
      // tested every time.

      // Trigger learning by feeding a stream of addresses with a +1 block stride.
      // 30 is greater than score_max (20), ensuring the learning phase completes.
      trainBopWithConstantOffset(0x1000, 1, 30);

      // By now, the score for offset 1 should be high enough (>= 20),
      // which should update the bestOffset and enable prefetching.
      Addr testAddr = 0x1000;
      Addr expectedPfAddr = testAddr + 64; // bestOffset is 1 block

      issueDmdMissToBop(testAddr);

      // Check if a prefetch was generated with the correct address
      ASSERT_FALSE(prefetch_candidates.empty());
      EXPECT_EQ(prefetch_candidates[0].getAddr(), expectedPfAddr);
  }

  // Test the score_max parameter.
  // Prefetching should only start after an offset's score has reached score_max.
  TEST_F(BOPTest, ScoreMax) {
    // Phase 1: Train just enough times to reach the score_max threshold.
    // During this phase, no prefetches should be issued.
    for (int i = 0; i < params.score_max; ++i) {
        Addr addr = 0x1000 + i * params.block_size;
        issueDmdMissToBop(addr);
        advanceTime(params.delay_queue_cycles);
        ASSERT_TRUE(prefetch_candidates.size() == 0);
    }
    // Phase 2: The learning phase should now be complete.
    // Every subsequent access should trigger a correct prefetch.
    for (int i = 0; i < 20; ++i) {
      Addr addr = 0x1000 + (i + params.score_max) * params.block_size;
      issueDmdMissToBop(addr);
      ASSERT_TRUE(prefetch_candidates.size() == 1);
      EXPECT_EQ(prefetch_candidates[0].getAddr(), addr + params.block_size);
    }
  }

  // Test the round_max parameter.
  // If no single offset scores highly, the learning phase should still complete
  // after round_max rounds, and a prefetch should be issued for the best
  // offset found so far (if its score is above bad_score).
  TEST_F(BOPTest, RoundMax) {
    // Trigger learning for nearly round_max times.
    // By manipulating the event queue timing (not advancing time for some accesses),
    // we prevent any single offset from achieving a high score.
    for (int i = 1; i < params.round_max; ++i) {
      Addr addr = 0x1000 + i * params.block_size;
      issueDmdMissToBop(addr);
      if (i < params.score_max - 1) {
        // If we don't advance time, the address stays in the delayQueue
        // and doesn't contribute to scoring, so score_max is not reached.
        advanceTime(params.delay_queue_cycles);
      }
      ASSERT_TRUE(prefetch_candidates.size() == 0);
    }
    // After round_max accesses, the learning phase is forced to conclude.
    // The best offset found (even with a low score) should now be used.
    for (int i = 0; i < 20; ++i) {
      Addr addr = 0x1000 + (i + params.round_max) * params.block_size;
      issueDmdMissToBop(addr);
      ASSERT_TRUE(prefetch_candidates.size() == 1);
      EXPECT_EQ(prefetch_candidates[0].getAddr(), addr + params.block_size);
    }
  }

  // Test the learning of negative offsets.
  // The BOP should be able to learn patterns with negative strides (i.e.,
  // accessing memory backwards).
  TEST_F(BOPTest, NegativeOffsets) {
    params.negative_offsets_enable = true;
    if (bop) delete bop;
    bop = new BOP(params, &eventQueue);
    // Trigger learning with a descending address stream (-1 block stride).
    trainBopWithConstantOffset(0x900000, -1, params.round_max);

    // Verify that the prefetcher learned the negative offset correctly.
    Addr addr = 0x900000;
    issueDmdMissToBop(addr);
    ASSERT_TRUE(prefetch_candidates.size() == 1);
    EXPECT_EQ(prefetch_candidates[0].getAddr(), addr - params.block_size);
  }

  // Test the crossPage parameter.
  // When disabled, the BOP should not issue prefetches that cross page boundaries (typically 4KB).
  TEST_F(BOPTest, CrossPage) {
    params.crossPage = false;
    if (bop) delete bop;
    bop = new BOP(params, &eventQueue);

    // First, train the prefetcher to learn a simple +1 offset.
    trainBopWithConstantOffset(0x900000, 1, params.round_max);

    // Issue a prefetch that does NOT cross a page boundary. This should succeed.
    Addr addr = 0;
    issueDmdMissToBop(addr);
    ASSERT_TRUE(prefetch_candidates.size() == 1);

    // Issue a prefetch that WOULD cross a page boundary (4096 bytes).
    // The trigger address is the last block in the page. The prefetch
    // target would be in the next page. This should be blocked.
    addr = 4096 - params.block_size;
    issueDmdMissToBop(addr);
    ASSERT_TRUE(prefetch_candidates.size() == 0);
  }

  // Test bad_score parameter
  // If the best score at the end of a learning phase does not exceed bad_score,
  // prefetching should not be enabled.
  TEST_F(BOPTest, BadScoreTest) {
    params.bad_score = 10;
    params.score_max = 15;
    params.round_max = 20; // Shorten for test speed
    if (bop) delete bop;
    bop = new BOP(params, &eventQueue);

    // Train with a consistent offset, but for a number of rounds
    // just enough to finish the learning phase via round_max,
    // but not enough to get a high score.
    // Here, score will be less than bad_score=10.
    trainBopWithConstantOffset(0x1000, 1, 5);

    // Finish the learning phase with dummy accesses to trigger round_max
    for (int i = 0; i < params.round_max; ++i) {
        issueDmdMissToBop(0x8000);
        advanceTime(params.delay_queue_cycles);
    }

    // Now, the learning phase should be over due to round_max.
    // Because the best score (5) is less than bad_score (10),
    // prefetching should be disabled.
    Addr testAddr = 0x1000 + 5 * params.block_size;
    issueDmdMissToBop(testAddr);

    ASSERT_TRUE(prefetch_candidates.empty());
  }

  // Test learning from multiple offset options
  TEST_F(BOPTest, MultipleOffsetsTest) {
    params.offsets = {8, 4, 1, 16};
    params.bad_score = 2;
    params.round_max = 20;
    params.score_max = 15; // Lower for faster testing
    if (bop) delete bop;
    bop = new BOP(params, &eventQueue);

    // Train with a constant offset of +4 blocks.
    // BOP will cycle through {8, 4, 1, 16} to test.
    // Only offset 4 should get a high score.
    // +1 to make sure the learning phase is over.
    trainBopWithConstantOffset(0x1000, 4, (params.round_max + 1) * params.offsets.size());

    // After training, the bestOffset should be 4.
    Addr testAddr = 0x8000;
    Addr expectedPfAddr = testAddr + 4 * params.block_size;

    issueDmdMissToBop(testAddr);

    ASSERT_FALSE(prefetch_candidates.empty());
    EXPECT_EQ(prefetch_candidates[0].getAddr(), expectedPfAddr);
  }

  // Test prefetch timeliness.
  // BOP should select a larger offset if the smaller, correct offset
  // always results in late prefetches.
  TEST_F(BOPTest, TimelinessTest) {
      params.offsets = {1, 2}; // Correct offset is 1, but it will be "late"
      params.score_max = 20;
      params.round_max = 20;
      params.delay_queue_cycles = Cycles(200);
      if (bop) delete bop;
      bop = new BOP(params, &eventQueue);

      // Train with a constant stride of +1.
      for (int i = 0; i < ((params.round_max + 1) * params.offsets.size()); ++i) {
          Addr addr = 0x1000 + i * params.block_size;
          issueDmdMissToBop(addr);

          // Advance time, but less than the full delay.
          // This makes the +1 offset always appear "late", as the previous
          // address is likely still in the delay queue when checked.
          // The +2 offset, looking for an older address, is more likely to score.
          // Note: The 'late' parameter to calculatePrefetch is also part of
          // timeliness, but for this test, we focus on the delay queue impact.
          advanceTime(Cycles(120));
      }

      // After training under high-latency conditions, BOP should have
      // favored the larger, more timely offset of 2, or increased the
      // depth of offset 1. The gem5 implementation favors increasing depth.
      // Let's check if the prefetch is further than 1 block.

      Addr testAddr = 0x8000;
      issueDmdMissToBop(testAddr);

      ASSERT_FALSE(prefetch_candidates.empty());
      // The actual offset learned might be 1*depth or 2.
      // We expect it to be greater than a simple +1 offset.
      EXPECT_EQ(prefetch_candidates[0].getAddr(), testAddr + 2 * params.block_size);
  }

  // Test that the BOP stops prefetching when a learned pattern disappears.
  TEST_F(BOPTest, PatternChangeTest) {
      params.score_max = 15;
      if (bop) delete bop;
      bop = new BOP(params, &eventQueue);

      // Phase 1: Train with a clear +1 offset pattern until it learns.
      trainBopWithConstantOffset(0x1000, 1, 20);

      // Verify that prefetching is now active with the correct offset.
      Addr testAddr1 = 0x8000;
      issueDmdMissToBop(testAddr1);
      ASSERT_FALSE(prefetch_candidates.empty());
      EXPECT_EQ(prefetch_candidates[0].getAddr(), testAddr1 + params.block_size);

      // Phase 2: Switch to a random access pattern.
      // This should cause the score for the +1 offset to stagnate and eventually
      // lead to prefetching being disabled in a new learning round.
      for (int i = 0; i < 50; ++i) {
          // Addresses that do not match the +1 offset pattern
          Addr random_addr = 0xA0000 + i * 13 * params.block_size;
          issueDmdMissToBop(random_addr);
          advanceTime(params.delay_queue_cycles);
      }

      // Phase 3: Check if the prefetcher has turned itself off.
      // Issue an access that would have been prefetched by the old pattern.
      Addr testAddr2 = 0x9000;
      issueDmdMissToBop(testAddr2);

      // The prefetcher should have detected the pattern change and stopped.
      ASSERT_TRUE(prefetch_candidates.empty());
  }

} // namespace test
} // namespace prefetch
} // namespace gem5
