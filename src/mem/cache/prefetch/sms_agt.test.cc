/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>

#include "mem/cache/prefetch/sms_agt.hh"

namespace gem5::prefetch
{

TEST(SmsAgtRegion, ForwardAndBackwardUseRtlLookaheadAndPartialRegion)
{
    // Line 64 + 30 = line 94: region [80,95], offsets 14..15.
    auto forward = smsAgtRegion(64 * 64, 64, 16, 30, false);
    ASSERT_TRUE(forward);
    EXPECT_EQ(forward->base, 80 * 64);
    EXPECT_EQ(forward->bits, 0xc000);
    // Line 64 - 30 = line 34: region [32,47], offsets 0..2.
    auto backward = smsAgtRegion(64 * 64, 64, 16, 30, true);
    ASSERT_TRUE(backward);
    EXPECT_EQ(backward->base, 32 * 64);
    EXPECT_EQ(backward->bits, 0x7);
}

TEST(SmsAgtRegion, EveryOffsetAndDirectionMatchesRtlMask)
{
    for (unsigned blocks : {16, 64}) {
        for (unsigned offset = 0; offset < blocks; ++offset) {
            for (bool backward : {false, true}) {
                const Addr target = 128 + offset;
                const Addr current = backward ? target + 30 : target - 30;
                auto region = smsAgtRegion(current * 64 + 7, 64, blocks,
                                            30, backward);
                ASSERT_TRUE(region);
                EXPECT_EQ(region->base, 128 * 64);
                for (unsigned i = 0; i < blocks; ++i) {
                    EXPECT_EQ(bool(region->bits & (uint64_t(1) << i)),
                              backward ? i <= offset : i >= offset);
                }
            }
        }
    }
}

TEST(SmsAgtRegion, AddressWrapDoesNotGenerateOppositeEndRequests)
{
    EXPECT_FALSE(smsAgtRegion(0, 64, 16, 30, true));
    EXPECT_FALSE(smsAgtRegion(std::numeric_limits<Addr>::max(),
                              64, 16, 30, false));
    auto last = smsAgtRegion(std::numeric_limits<Addr>::max() - 30 * 64,
                              64, 16, 30, false);
    ASSERT_TRUE(last);
    EXPECT_EQ(last->bits, 0x8000);
}

} // namespace gem5::prefetch
