#include <gtest/gtest.h>

#include <deque>
#include <random>

#include "mem/cache/prefetch/banked_queue.hh"

namespace gem5
{
namespace prefetch
{

TEST(BankedQueue, FifoAndBankIsolation)
{
    BankedQueue<int> queue(4, 2);
    EXPECT_FALSE(queue.push(0, 10));
    EXPECT_FALSE(queue.push(0, 11));
    EXPECT_EQ(queue.push(0, 12), 10);
    EXPECT_FALSE(queue.push(1, 20));
    EXPECT_EQ(queue.pop(1), 20);
    EXPECT_EQ(queue.pop(0), 11);
    EXPECT_EQ(queue.pop(0), 12);
    EXPECT_TRUE(queue.empty(0));
    EXPECT_TRUE(queue.empty(2));
}

TEST(BankedQueue, FullWithConcurrentDequeue)
{
    BankedQueue<int> queue(1, 2);
    queue.push(0, 1);
    queue.push(0, 2);
    // The old head enters the output pipeline before this cycle's enqueue.
    EXPECT_EQ(queue.pop(0), 1);
    EXPECT_FALSE(queue.push(0, 3));
    EXPECT_EQ(queue.pop(0), 2);
    EXPECT_EQ(queue.pop(0), 3);
}

TEST(BankedQueue, SourcePriorityAndParallelBanks)
{
    // Stream, large BOP and small BOP each offer one head.
    EXPECT_EQ(BankedQueue<int>::select(4, {2, 2, 1}),
              (std::vector<int>{-1, 2, 0, -1}));
    // A competing BOP remains a candidate for the following cycle.
    EXPECT_EQ(BankedQueue<int>::select(4, {-1, 2, 1}),
              (std::vector<int>{-1, 2, 1, -1}));
    EXPECT_EQ(BankedQueue<int>::select(4, {0, 1, 2}),
              (std::vector<int>{0, 1, 2, -1}));
}

TEST(BankedQueue, RandomBackpressureAndOverwriteReference)
{
    BankedQueue<int> queue(4, 32);
    std::vector<std::deque<int>> expected(4);
    std::mt19937 rng(7129);
    for (int id = 0; id < 20000; ++id) {
        const unsigned bank = rng() % 4;
        if (!expected[bank].empty() && rng() % 4 == 0) {
            EXPECT_EQ(queue.pop(bank), expected[bank].front());
            expected[bank].pop_front();
        }
        std::optional<int> victim;
        expected[bank].push_back(id);
        if (expected[bank].size() > 32) {
            victim = expected[bank].front();
            expected[bank].pop_front();
        }
        EXPECT_EQ(queue.push(bank, id), victim);
        for (unsigned b = 0; b < 4; ++b)
            EXPECT_EQ(queue.size(b), expected[b].size());
    }
    for (unsigned bank = 0; bank < 4; ++bank) {
        while (!expected[bank].empty()) {
            EXPECT_EQ(queue.pop(bank), expected[bank].front());
            expected[bank].pop_front();
        }
    }
}

} // namespace prefetch
} // namespace gem5
