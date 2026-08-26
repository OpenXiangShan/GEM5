/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences
 * All rights reserved.
 *
 * The license is the same as that in register_prefetcher.hh.
 */

#include <gtest/gtest.h>

#include <limits>

#include "cpu/o3/rfp_stride_table.hh"

namespace gem5
{
namespace o3
{

namespace
{

void
trainToConfidence(RfpStrideTable &table, Addr pc, Addr first,
                  int64_t stride, uint64_t generation = 1)
{
    for (unsigned sample = 0; sample < 5; ++sample) {
        const Addr address = stride >= 0 ?
            first + sample * static_cast<Addr>(stride) :
            first - sample * static_cast<Addr>(-stride);
        table.train(pc, address, generation, sample + 1, sample + 1);
    }
}

TEST(RfpStrideTableTest, PredictsConfidentPositiveAndNegativeStrides)
{
    RfpStrideTable table(8, 2, 2, 3, 4096, true);

    trainToConfidence(table, 0x100, 0x4000, 8);
    auto positive = table.lookup(0x100, 1, 10);
    ASSERT_TRUE(positive.prediction);
    EXPECT_EQ(positive.prediction->address, 0x4028);

    trainToConfidence(table, 0x200, 0x5000, -16);
    auto negative = table.lookup(0x200, 1, 20);
    ASSERT_TRUE(negative.prediction);
    EXPECT_EQ(negative.prediction->address, 0x4fb0);
}

TEST(RfpStrideTableTest, RequiresThresholdAndChangesVersionWithStride)
{
    RfpStrideTable table(8, 2, 2, 3, 4096, true);
    constexpr Addr Pc = 0x300;

    table.train(Pc, 0x6000, 1, 1, 1);
    table.train(Pc, 0x6008, 1, 2, 2);
    EXPECT_EQ(table.lookup(Pc, 1, 3).reject,
              RfpStrideTable::RejectReason::LowConfidence);
    table.train(Pc, 0x6010, 1, 3, 3);
    table.train(Pc, 0x6018, 1, 4, 4);
    table.train(Pc, 0x6020, 1, 5, 5);

    auto prediction = table.lookup(Pc, 1, 6);
    ASSERT_TRUE(prediction.prediction);
    const uint32_t stable_version = prediction.prediction->version;
    table.train(Pc, 0x6028, 1, 6, 7);
    EXPECT_TRUE(table.versionMatches(Pc, 1, stable_version));

    table.train(Pc, 0x6100, 1, 7, 8);
    table.train(Pc, 0x6200, 1, 8, 9);
    table.train(Pc, 0x6300, 1, 9, 10);
    EXPECT_FALSE(table.versionMatches(Pc, 1, stable_version));
}

TEST(RfpStrideTableTest, RejectsPageCrossingAndAddressOverflow)
{
    RfpStrideTable page_table(8, 2, 2, 3, 4096, true);
    trainToConfidence(page_table, 0x400, 0xfd8, 8);
    EXPECT_EQ(page_table.lookup(0x400, 1, 10).reject,
              RfpStrideTable::RejectReason::CrossPage);

    RfpStrideTable high_table(8, 2, 2, 3, 4096, false);
    const Addr max = std::numeric_limits<Addr>::max();
    trainToConfidence(high_table, 0x500, max - 39, 8);
    EXPECT_EQ(high_table.lookup(0x500, 1, 10).reject,
              RfpStrideTable::RejectReason::AddressOverflow);

    RfpStrideTable low_table(8, 2, 2, 3, 4096, false);
    trainToConfidence(low_table, 0x600, 39, -8);
    EXPECT_EQ(low_table.lookup(0x600, 1, 10).reject,
              RfpStrideTable::RejectReason::AddressOverflow);
}

TEST(RfpStrideTableTest, KeepsAddressSpaceGenerationsIndependent)
{
    RfpStrideTable table(8, 2, 2, 3, 4096, true);
    trainToConfidence(table, 0x700, 0x7000, 8, 3);

    EXPECT_TRUE(table.lookup(0x700, 3, 10).prediction);
    EXPECT_EQ(table.lookup(0x700, 4, 11).reject,
              RfpStrideTable::RejectReason::Miss);
}

TEST(RfpStrideTableTest, LaunchesOncePerCommittedSample)
{
    RfpStrideTable table(8, 2, 2, 3, 4096, true);
    constexpr Addr Pc = 0x800;
    trainToConfidence(table, Pc, 0x8000, 8);

    auto prediction = table.lookup(Pc, 1, 10);
    ASSERT_TRUE(prediction.prediction);
    EXPECT_TRUE(table.claimPrediction(Pc, 1, prediction.prediction->version));
    EXPECT_FALSE(table.claimPrediction(Pc, 1, prediction.prediction->version));

    table.train(Pc, 0x8028, 1, 6, 11);
    prediction = table.lookup(Pc, 1, 12);
    ASSERT_TRUE(prediction.prediction);
    EXPECT_TRUE(table.claimPrediction(Pc, 1, prediction.prediction->version));
}

} // anonymous namespace
} // namespace o3
} // namespace gem5
