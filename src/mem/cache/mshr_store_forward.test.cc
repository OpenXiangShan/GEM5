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

#include <array>
#include <cstdint>
#include <vector>

#include "mem/cache/mshr_store_forward.hh"

namespace gem5
{

namespace
{

constexpr Addr BlockAddr = 0x1000;

MSHRStoreForwardSource
source(Addr addr, ContextID context, InstSeqNum seq,
       const std::vector<uint8_t> &data, const std::vector<bool> &mask)
{
    return {addr, data.size(), context, seq, data.data(), &mask};
}

} // anonymous namespace

TEST(MSHRStoreForward, SelectsNewestOlderStorePerByte)
{
    const std::vector<uint8_t> old_data = {0xa8, 0xa9, 0xaa, 0xab};
    const std::vector<bool> old_mask = {true, true, true, true};
    const std::vector<uint8_t> new_data = {0, 0xb9, 0, 0xbb};
    const std::vector<bool> new_mask = {false, true, false, true};
    const std::vector<uint8_t> younger_data = {0xc8, 0, 0, 0};
    const std::vector<bool> younger_mask = {true, false, false, false};
    const std::vector<uint8_t> other_context_data = {0, 0, 0xda, 0};
    const std::vector<bool> other_context_mask = {
        false, false, true, false};

    const std::vector<MSHRStoreForwardSource> sources = {
        source(BlockAddr + 8, 0, 10, old_data, old_mask),
        source(BlockAddr + 8, 0, 18, new_data, new_mask),
        source(BlockAddr + 8, 0, 25, younger_data, younger_mask),
        source(BlockAddr + 8, 1, 19,
               other_context_data, other_context_mask),
    };
    const auto forwarding = selectMSHRStoreForwardData(
        BlockAddr + 8, 4, 0, 20, sources);

    EXPECT_TRUE(forwarding.isFull());
    EXPECT_EQ(forwarding.data,
              (std::vector<uint8_t>{0xa8, 0xb9, 0xaa, 0xbb}));
}

TEST(MSHRStoreForward, ReportsPartialCoverage)
{
    const std::vector<uint8_t> data = {0, 0x5a, 0, 0};
    const std::vector<bool> mask = {false, true, false, false};
    const std::vector<MSHRStoreForwardSource> sources = {
        source(BlockAddr + 8, 0, 10, data, mask),
    };
    const auto forwarding = selectMSHRStoreForwardData(
        BlockAddr + 8, 4, 0, 20, sources);

    EXPECT_TRUE(forwarding.hasData());
    EXPECT_FALSE(forwarding.isFull());
    EXPECT_EQ(forwarding.valid,
              (std::vector<bool>{false, true, false, false}));
    EXPECT_EQ(forwarding.data[1], 0x5a);
}

TEST(MSHRStoreForward, AppliesOnlyValidBytes)
{
    std::array<uint8_t, 4> load_data = {0x10, 0x20, 0x30, 0x40};
    MSHRStoreForwardData forwarding{
        {0xa0, 0xb0, 0xc0, 0xd0}, {true, false, true, false}};

    EXPECT_EQ(applyMSHRStoreForwardData(load_data.data(), forwarding), 2);
    EXPECT_EQ(load_data,
              (std::array<uint8_t, 4>{0xa0, 0x20, 0xc0, 0x40}));
}

} // namespace gem5
