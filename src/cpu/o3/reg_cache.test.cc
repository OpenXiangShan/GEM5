#include <gtest/gtest.h>

#include <random>

#include <boost/compute/detail/lru_cache.hpp>

#include "cpu/o3/reg_cache.hh"

TEST(RegCache, MatchesBoostRecencyAndEviction)
{
    for (unsigned capacity : {14, 28}) {
        gem5::o3::RegCache cache(capacity);
        boost::compute::detail::lru_cache<int, int> reference(capacity);
        std::mt19937 random(0x6509);
        for (unsigned i = 0; i < 100000; ++i) {
            int key = random() % 80;
            if (random() % 3) {
                cache.insert(key);
                reference.insert(key, 0);
            } else {
                EXPECT_EQ(cache.get(key), bool(reference.get(key)));
            }
            for (int candidate = 0; candidate < 80; ++candidate) {
                ASSERT_EQ(cache.contains(candidate), reference.contains(candidate));
            }
        }
    }
}

TEST(RegCache, InsertionDoesNotRefresh)
{
    gem5::o3::RegCache cache(14);
    for (int i = 0; i < 14; ++i) {
        cache.insert(i);
    }
    cache.insert(0);
    cache.insert(14);
    EXPECT_FALSE(cache.contains(0));
    EXPECT_TRUE(cache.get(1));
    cache.insert(15);
    EXPECT_TRUE(cache.contains(1));
    EXPECT_FALSE(cache.contains(2));
}
