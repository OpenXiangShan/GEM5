#include <gtest/gtest.h>

#include <array>

#include "mem/cache/slice_hash.hh"

namespace gem5
{

namespace
{

constexpr std::array<SliceHashPolicy, 4> Policies = {
    SliceHashPolicy::None,
    SliceHashPolicy::Xor,
    SliceHashPolicy::XorFold,
    SliceHashPolicy::Murmur3,
};

} // anonymous namespace

TEST(SliceHashTest, ParsePolicy)
{
    EXPECT_EQ(parseSliceHashPolicy("none"), SliceHashPolicy::None);
    EXPECT_EQ(parseSliceHashPolicy("xor"), SliceHashPolicy::Xor);
    EXPECT_EQ(parseSliceHashPolicy("xor-fold"), SliceHashPolicy::XorFold);
    EXPECT_EQ(parseSliceHashPolicy("murmur3"), SliceHashPolicy::Murmur3);
    EXPECT_EQ(parseSliceHashPolicy("unknown"), SliceHashPolicy::Invalid);
}

TEST(SliceHashTest, NonePreservesLowBits)
{
    for (Addr line = 0; line < 4096; ++line) {
        EXPECT_EQ(hashSlice(line, 2, SliceHashPolicy::None), line & 0x3);
    }
}

TEST(SliceHashTest, XorUsesAdjacentChunk)
{
    for (Addr line = 0; line < 4096; ++line) {
        const Addr expected = (line & 0x3) ^ ((line >> 2) & 0x3);
        EXPECT_EQ(hashSlice(line, 2, SliceHashPolicy::Xor), expected);
    }
}

TEST(SliceHashTest, PoliciesUseExpectedMixing)
{
    constexpr Addr Line = 0x1c;

    EXPECT_EQ(hashSlice(Line, 2, SliceHashPolicy::None), 0);
    EXPECT_EQ(hashSlice(Line, 2, SliceHashPolicy::Xor), 3);
    EXPECT_EQ(hashSlice(Line, 2, SliceHashPolicy::XorFold), 2);
    EXPECT_EQ(hashSlice(Line, 2, SliceHashPolicy::Murmur3), 1);
}

TEST(SliceHashTest, MappingIsBijectiveAndRecoverable)
{
    constexpr unsigned SliceBits = 2;
    constexpr Addr NumSlices = Addr(1) << SliceBits;

    for (const auto policy : Policies) {
        for (Addr upper = 0; upper < 4096; ++upper) {
            std::array<bool, NumSlices> seen = {};
            for (Addr low = 0; low < NumSlices; ++low) {
                const Addr line = (upper << SliceBits) | low;
                const Addr slice = hashSlice(line, SliceBits, policy);
                ASSERT_LT(slice, NumSlices);
                EXPECT_FALSE(seen[slice]);
                seen[slice] = true;
                EXPECT_EQ(recoverSliceLowBits(
                              upper, slice, SliceBits, policy), low);
            }
            for (bool mapped : seen) {
                EXPECT_TRUE(mapped);
            }
        }
    }
}

TEST(SliceHashTest, SingleSliceAlwaysMapsToZero)
{
    for (const auto policy : Policies) {
        EXPECT_EQ(hashSlice(0x123456789abcdefULL, 0, policy), 0);
        EXPECT_EQ(recoverSliceLowBits(
                      0x123456789abcdefULL, 0, 0, policy), 0);
    }
}

} // namespace gem5
