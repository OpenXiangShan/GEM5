/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/request.hh"

using namespace gem5;

GTestTickHandler tickHandler;

TEST(PAMAIPRequestTest, SnapshotBelongsToOnePredictor)
{
    Request request(0x1000, 64, 0, 0);
    int first_owner = 0;
    int other_owner = 0;
    uint8_t state = 0;
    EXPECT_FALSE(request.getInsertionHint(&first_owner, state));
    request.setInsertionHint(&first_owner, 2);
    ASSERT_TRUE(request.getInsertionHint(&first_owner, state));
    EXPECT_EQ(state, 2);
    EXPECT_FALSE(request.getInsertionHint(&other_owner, state));
}

TEST(PAMAIPRequestTest, ForwardedRequestCopiesPreserveSnapshot)
{
    Request original(0x1000, 64, 0, 0);
    int owner = 0;
    original.setInsertionHint(&owner, 1);
    Request forwarded(original);
    uint8_t state = 0;
    ASSERT_TRUE(forwarded.getInsertionHint(&owner, state));
    EXPECT_EQ(state, 1);
    forwarded.setInsertionHint(&owner, 3);
    ASSERT_TRUE(original.getInsertionHint(&owner, state));
    EXPECT_EQ(state, 1);
}

TEST(PAMAIPRequestTest, RecycledVirtualRequestCannotReuseOldHint)
{
    Request request(0x1000, 64, 0, 0);
    int owner = 0;
    request.setInsertionHint(&owner, 3);
    request.setVirt(0x2000, 64, 0, 0, 0x80000000);
    uint8_t state = 0;
    EXPECT_FALSE(request.getInsertionHint(&owner, state));
    request.setPaddr(0x3000);
    request.setInsertionHint(&owner, 0);
    ASSERT_TRUE(request.getInsertionHint(&owner, state));
    EXPECT_EQ(state, 0);
}
