/*
 * Copyright (c) 2026 XiangShan
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

#include "mem/cache/prefetch/sms_order.hh"

namespace gem5
{
namespace prefetch
{
namespace sms
{

TEST(SmsOrder, SelectsHistoricalFirstTouchOrder)
{
    std::vector<OrderScore> orders(16, InvalidOrder);
    orders[2] = 3 << OrderFractionBits;
    orders[5] = 1 << OrderFractionBits;
    orders[9] = 2 << OrderFractionBits;

    uint64_t pending = (uint64_t(1) << 2) | (uint64_t(1) << 5) |
                       (uint64_t(1) << 9);
    EXPECT_EQ(selectOffset(pending, orders, 16), 5);
    pending &= ~(uint64_t(1) << 5);
    EXPECT_EQ(selectOffset(pending, orders, 16), 9);
    pending &= ~(uint64_t(1) << 9);
    EXPECT_EQ(selectOffset(pending, orders, 16), 2);
}

TEST(SmsOrder, EqualOrderUsesSmallerOffset)
{
    std::vector<OrderScore> orders(16, InvalidOrder);
    orders[3] = 2 << OrderFractionBits;
    orders[11] = 2 << OrderFractionBits;
    const uint64_t pending = (uint64_t(1) << 3) | (uint64_t(1) << 11);

    EXPECT_EQ(selectOffset(pending, orders, 16), 3);
    EXPECT_TRUE(hasBestOrderTie(pending, orders, 16, 3));
}

TEST(SmsOrder, LegacySelectionPreservesDirection)
{
    const uint64_t pending = (uint64_t(1) << 2) | (uint64_t(1) << 9);
    EXPECT_EQ(legacyOffset(pending, false), 2);
    EXPECT_EQ(legacyOffset(pending, true), 9);
}

TEST(SmsOrder, EwmaUsesQuarterOfDifference)
{
    const OrderScore first = updateOrderScore(0, false, 8);
    EXPECT_EQ(first, 8 << OrderFractionBits);
    EXPECT_EQ(updateOrderScore(first, true, 4),
              7 << OrderFractionBits);
}

TEST(SmsOrder, RepeatedInsertKeepsFirstOrder)
{
    std::vector<OrderScore> stored(16, InvalidOrder);
    std::vector<OrderScore> first(16, InvalidOrder);
    first[2] = 3 << OrderFractionBits;
    first[5] = 1 << OrderFractionBits;
    mergeNewOffsetOrders(0, (uint64_t(1) << 2) | (uint64_t(1) << 5),
                         first, stored, 16);

    std::vector<OrderScore> second(16, InvalidOrder);
    second[2] = 1 << OrderFractionBits;
    second[9] = 2 << OrderFractionBits;
    const uint64_t existing = (uint64_t(1) << 2) | (uint64_t(1) << 5);
    mergeNewOffsetOrders(existing,
                         (uint64_t(1) << 2) | (uint64_t(1) << 9),
                         second, stored, 16);

    EXPECT_EQ(stored[2], 3 << OrderFractionBits);
    EXPECT_EQ(stored[5], 1 << OrderFractionBits);
    EXPECT_EQ(stored[9], 2 << OrderFractionBits);
    EXPECT_EQ(existing | (uint64_t(1) << 2) | (uint64_t(1) << 9),
              (uint64_t(1) << 2) | (uint64_t(1) << 5) |
              (uint64_t(1) << 9));
}

TEST(SmsPhtDest, TriggerUsesHighMedLowLevels)
{
    EXPECT_EQ(phtDestLevel(7, true, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 1);
    EXPECT_EQ(phtDestLevel(6, true, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 1);
    EXPECT_EQ(phtDestLevel(5, true, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 2);
    EXPECT_EQ(phtDestLevel(4, true, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 2);
    EXPECT_EQ(phtDestLevel(3, true, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 3);
    EXPECT_EQ(phtDestLevel(2, true, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 0);
    EXPECT_EQ(phtDestLevel(0, true, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 0);
}

TEST(SmsPhtDest, NonTriggerSendsOnlyHighAndMedium)
{
    EXPECT_EQ(phtDestLevel(7, false, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 2);
    EXPECT_EQ(phtDestLevel(6, false, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 2);
    EXPECT_EQ(phtDestLevel(4, false, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 3);
    EXPECT_EQ(phtDestLevel(3, false, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 0);
    EXPECT_EQ(phtDestLevel(2, false, DefaultHighConfThreshold,
                           DefaultMedConfThreshold, DefaultLowConfThreshold), 0);
}

} // namespace sms
} // namespace prefetch
} // namespace gem5
