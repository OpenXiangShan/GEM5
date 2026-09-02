#include <gtest/gtest.h>

#include <array>

#include "mem/cache/slice_hash.hh"

namespace gem5
{

namespace
{

constexpr std::array<SliceHashPolicy, 3> Policies = {
    SliceHashPolicy::None,
    SliceHashPolicy::XorFold,
    SliceHashPolicy::Murmur3,
};

} // anonymous namespace

TEST(SliceHashTest, ParsePolicy)
{
    EXPECT_EQ(parseSliceHashPolicy("none"), SliceHashPolicy::None);
    EXPECT_EQ(parseSliceHashPolicy("xor-fold"), SliceHashPolicy::XorFold);
    EXPECT_EQ(parseSliceHashPolicy("murmur3"), SliceHashPolicy::Murmur3);
    EXPECT_EQ(parseSliceHashPolicy("xor"), SliceHashPolicy::Invalid);
    EXPECT_EQ(parseSliceHashPolicy("unknown"), SliceHashPolicy::Invalid);
}

TEST(SliceHashTest, NonePreservesLowBits)
{
    for (Addr line = 0; line < 4096; ++line) {
        EXPECT_EQ(hashSlice(line, 2, SliceHashPolicy::None), line & 0x3);
    }
}

TEST(SliceHashTest, PoliciesUseExpectedMixing)
{
    constexpr Addr Line = 0x1c;

    EXPECT_EQ(hashSlice(Line, 2, SliceHashPolicy::None), 0);
    EXPECT_EQ(hashSlice(Line, 2, SliceHashPolicy::XorFold), 2);
    EXPECT_EQ(hashSlice(Line, 2, SliceHashPolicy::Murmur3), 2);
}

TEST(SliceHashTest, HashPoliciesRetainSliceBitsInSetAndTag)
{
    constexpr unsigned SliceBits = 2;
    EXPECT_EQ(sliceSetShift(SliceBits, SliceHashPolicy::None), SliceBits);
    EXPECT_EQ(sliceSetShift(SliceBits, SliceHashPolicy::XorFold), 0);
    EXPECT_EQ(sliceSetShift(SliceBits, SliceHashPolicy::Murmur3), 0);
}

TEST(SliceHashTest, HashPoliciesStoreTwoAdditionalTagBits)
{
    constexpr unsigned BlockBits = 6;
    constexpr unsigned SliceBits = 2;
    constexpr unsigned SetBits = 10;

    const auto tag_shift = [](SliceHashPolicy policy) {
        return BlockBits + sliceSetShift(SliceBits, policy) + SetBits;
    };

    EXPECT_EQ(tag_shift(SliceHashPolicy::None), 18);
    EXPECT_EQ(tag_shift(SliceHashPolicy::XorFold), 16);
    EXPECT_EQ(tag_shift(SliceHashPolicy::Murmur3), 16);
}

TEST(SliceHashTest, SetTagEncodingPreservesLineAddress)
{
    constexpr unsigned SliceBits = 2;
    constexpr unsigned SetBits = 10;
    constexpr Addr NumSlices = Addr(1) << SliceBits;
    constexpr Addr SetMask = (Addr(1) << SetBits) - 1;

    for (const auto policy : Policies) {
        const unsigned set_shift = sliceSetShift(SliceBits, policy);
        for (Addr line = 0; line < 16384; ++line) {
            const Addr slice = hashSlice(line, SliceBits, policy);
            const Addr set = (line >> set_shift) & SetMask;
            const Addr tag = line >> (set_shift + SetBits);
            Addr regenerated =
                (tag << (set_shift + SetBits)) | (set << set_shift);

            if (policy == SliceHashPolicy::None) {
                regenerated |= slice;
            }

            ASSERT_LT(slice, NumSlices);
            EXPECT_EQ(regenerated, line);
        }
    }
}

TEST(SliceHashTest, SameHashedSliceAndSetCannotAlias)
{
    constexpr unsigned SliceBits = 2;
    constexpr unsigned SetBits = 10;
    constexpr Addr NumSlices = Addr(1) << SliceBits;
    constexpr Addr Set = 0x155;

    for (const auto policy : {SliceHashPolicy::XorFold,
                              SliceHashPolicy::Murmur3}) {
        std::array<Addr, NumSlices> first_line = {};
        std::array<bool, NumSlices> seen = {};
        bool found_collision = false;

        // Five distinct tags must put at least two lines in the same one of
        // four slices. Their complete tags must still distinguish the lines.
        for (Addr tag = 0; tag <= NumSlices; ++tag) {
            const Addr line = (tag << SetBits) | Set;
            const Addr slice = hashSlice(line, SliceBits, policy);
            ASSERT_LT(slice, NumSlices);

            if (seen[slice]) {
                EXPECT_EQ(first_line[slice] & ((Addr(1) << SetBits) - 1),
                          line & ((Addr(1) << SetBits) - 1));
                EXPECT_NE(first_line[slice] >> SetBits, line >> SetBits);
                found_collision = true;
                break;
            }

            seen[slice] = true;
            first_line[slice] = line;
        }

        EXPECT_TRUE(found_collision);
    }
}

TEST(SliceHashTest, SingleSliceAlwaysMapsToZero)
{
    for (const auto policy : Policies) {
        EXPECT_EQ(hashSlice(0x123456789abcdefULL, 0, policy), 0);
        EXPECT_EQ(sliceSetShift(0, policy), 0);
    }
}

} // namespace gem5
