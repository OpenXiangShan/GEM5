#include <gtest/gtest.h>

#include "mem/cache/prefetch/candidate_table.hh"

namespace gem5::prefetch
{

struct TestOwner : CandidateLifecycle
{
    unsigned payload{0};
};

TEST(LldpCandidateTable, CapacityReuseAndDelayedCallbacks)
{
    CandidateTable<TestOwner> table(2);
    TestOwner owner;
    owner.payload = 17;
    const auto first = table.allocate(7, owner);
    const auto second = table.allocate(7, owner);
    ASSERT_NE(first, 0);
    ASSERT_NE(second, 0);
    ASSERT_NE(first, second);
    EXPECT_TRUE(table.full());
    EXPECT_EQ(table.allocate(7, owner), 0);
    EXPECT_EQ(table.size(), 2);
    EXPECT_EQ(table.find(first)->payload, 17);
    EXPECT_EQ(table.find(first ^ (uint64_t(1) << 48)), nullptr);
    EXPECT_EQ(table.find(0), nullptr);
    EXPECT_EQ(table.find(~uint64_t(0)), nullptr);
    ASSERT_TRUE(table.erase(first));
    ASSERT_FALSE(table.erase(first));
    for (unsigned i = 0; i < 65536; ++i) {
        const auto replacement = table.allocate(7, owner);
        ASSERT_NE(replacement, first);
        ASSERT_EQ(table.find(first), nullptr);
        ASSERT_NE(table.find(second), nullptr);
        ASSERT_EQ(table.size(), 2);
        ASSERT_TRUE(table.erase(replacement));
    }
    EXPECT_EQ(table.size(), 1);
    EXPECT_FALSE(table.full());
}

TEST(LldpCandidateTable, PreIssueDropAndTerminalAreIndependent)
{
    CandidateTable<TestOwner> table(4);
    const auto a = table.allocate(1, {});
    const auto b = table.allocate(1, {});
    auto *issued = table.find(a);
    auto *queued = table.find(b);
    unsigned outstanding = 0;
    if (issued->issue())
        ++outstanding;
    EXPECT_FALSE(issued->issue());
    ASSERT_TRUE(queued->finish());
    if (queued->hasIssued)
        --outstanding;
    EXPECT_EQ(outstanding, 1);
    EXPECT_FALSE(queued->issue());
    EXPECT_FALSE(queued->finish());
    // A merge completes A; a later useful/eviction must not settle it twice.
    ASSERT_TRUE(issued->finish());
    if (issued->hasIssued)
        --outstanding;
    EXPECT_FALSE(issued->finish());
    EXPECT_FALSE(issued->issue());
    EXPECT_EQ(outstanding, 0);
}

TEST(LldpCandidateTable, RejectsInvalidCapacity)
{
    EXPECT_THROW(CandidateTable<TestOwner>(0), std::invalid_argument);
    EXPECT_THROW(CandidateTable<TestOwner>(65537), std::invalid_argument);
}

} // namespace gem5::prefetch
