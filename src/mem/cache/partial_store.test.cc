/*
 * Copyright (c) 2026 OpenXiangShan
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

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/cache/cache_blk.hh"
#include "mem/cache/partial_line_meta.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

namespace
{

constexpr unsigned BlkSize = 64;
constexpr Addr TestAddr = 0x1000;
GTestTickHandler tickHandler;

RequestPtr
makeRequest(const std::vector<bool> &mask)
{
    auto req = std::make_shared<Request>(
        TestAddr, BlkSize, 0, Request::wbRequestorId);
    req->setByteEnable(mask);
    return req;
}

PacketPtr
makeStore(Addr addr, unsigned size, const std::vector<bool> &mask)
{
    auto req = std::make_shared<Request>(
        addr, size, 0, Request::wbRequestorId);
    req->setByteEnable(mask);
    auto *pkt = new Packet(req, MemCmd::WriteReq);
    pkt->allocate();
    return pkt;
}

PacketPtr
makeWriteback(const std::vector<bool> &mask, uint8_t value)
{
    auto req = makeRequest(std::vector<bool>(BlkSize, true));
    auto *pkt = new Packet(req, MemCmd::WritebackDirty);
    pkt->allocate();
    std::fill(pkt->getPtr<uint8_t>(), pkt->getPtr<uint8_t>() + BlkSize,
              value);
    req->setByteEnable(mask);
    return pkt;
}

TEST(PartialStoreTest, PermissionCommandHasNoData)
{
    MemCmd cmd(MemCmd::StorePermReq);
    EXPECT_TRUE(cmd.isUpgrade());
    EXPECT_TRUE(cmd.isInvalidate());
    EXPECT_TRUE(cmd.needsWritable());
    EXPECT_TRUE(cmd.needsResponse());
    EXPECT_FALSE(cmd.isRead());
    EXPECT_FALSE(cmd.isWrite());
    EXPECT_EQ(cmd.responseCommand(), MemCmd::StorePermResp);
}

TEST(PartialStoreTest, SplitPermissionGrantIsIntermediateAndHasNoData)
{
    MemCmd grant(MemCmd::StorePermGrantResp);
    EXPECT_TRUE(grant.isResponse());
    EXPECT_TRUE(grant.isUpgrade());
    EXPECT_FALSE(grant.isRead());
    EXPECT_FALSE(grant.isWrite());
    EXPECT_FALSE(grant.hasData());

    Packet request(
        makeRequest(std::vector<bool>(BlkSize, true)), MemCmd::ReadExReq);
    request.setSplitStorePermReq();
    EXPECT_TRUE(request.isSplitStorePermReq());

    Packet target(
        makeRequest(std::vector<bool>(BlkSize, true)), MemCmd::StorePermReq);
    EXPECT_FALSE(target.isStorePermRespSent());
    target.setStorePermRespSent();
    EXPECT_TRUE(target.isStorePermRespSent());

    EXPECT_FALSE(target.storePermSkipDataFetch());
    target.setStorePermSkipDataFetch();
    EXPECT_TRUE(target.storePermSkipDataFetch());
}

TEST(PartialStoreTest, ValidMaskGrowsAndBecomesFull)
{
    CacheBlk blk;
    blk.insert(TestAddr, false);
    blk.markPartial(BlkSize);

    std::vector<bool> first_mask(BlkSize, false);
    std::fill(first_mask.begin() + 8, first_mask.begin() + 16, true);
    Packet first(makeRequest(first_mask), MemCmd::WriteReq);
    first.allocate();
    blk.markValidData(&first, BlkSize);

    EXPECT_TRUE(blk.isPartial());
    EXPECT_TRUE(blk.hasValidData(8, 8));
    EXPECT_FALSE(blk.hasValidData(0, 8));

    std::vector<bool> rest_mask(BlkSize, true);
    std::fill(rest_mask.begin() + 8, rest_mask.begin() + 16, false);
    Packet rest(makeRequest(rest_mask), MemCmd::WriteReq);
    rest.allocate();
    blk.markValidData(&rest, BlkSize);

    EXPECT_FALSE(blk.isPartial());
    EXPECT_TRUE(blk.hasValidData(0, BlkSize));
}

TEST(PartialStoreTest, FullyCoveredBlockRejectsOlderPartialFill)
{
    CacheBlk blk;
    std::vector<uint8_t> block_data(BlkSize, 0x05);
    blk.data = block_data.data();
    blk.insert(TestAddr, false);
    blk.markPartial(BlkSize);

    Packet stores(
        makeRequest(std::vector<bool>(BlkSize, true)), MemCmd::WriteReq);
    stores.allocate();
    blk.markValidData(&stores, BlkSize);
    ASSERT_FALSE(blk.isPartial());

    const std::vector<uint8_t> old_fill(BlkSize, 0x6d);
    EXPECT_FALSE(blk.mergePartialFill(old_fill.data(), BlkSize));
    EXPECT_TRUE(std::all_of(block_data.begin(), block_data.end(),
                            [](uint8_t byte) { return byte == 0x05; }));
}

TEST(PartialStoreTest, GranularityTracksCompleteGranules)
{
    CacheBlk blk;
    blk.insert(TestAddr, false);
    blk.markPartial(BlkSize, 8);

    std::unique_ptr<Packet> one_byte(
        makeStore(TestAddr + 3, 1, std::vector<bool>(1, true)));
    blk.markValidData(one_byte.get(), BlkSize);

    EXPECT_TRUE(blk.isPartial());
    EXPECT_FALSE(blk.hasValidData(0, 8));
    const auto byte_mask_after_byte = blk.getValidMask();
    EXPECT_FALSE(std::any_of(byte_mask_after_byte.begin(),
                             byte_mask_after_byte.end(),
                             [](bool valid) { return valid; }));

    std::unique_ptr<Packet> one_granule(
        makeStore(TestAddr, 8, std::vector<bool>(8, true)));
    blk.markValidData(one_granule.get(), BlkSize);

    EXPECT_TRUE(blk.isPartial());
    EXPECT_TRUE(blk.hasValidData(0, 8));
    const auto byte_mask = blk.getValidMask();
    EXPECT_TRUE(std::all_of(byte_mask.begin(), byte_mask.begin() + 8,
                            [](bool valid) { return valid; }));
    EXPECT_TRUE(std::all_of(byte_mask.begin() + 8, byte_mask.end(),
                            [](bool valid) { return !valid; }));
}

TEST(PartialStoreTest, GranularWriteRequiresCompleteInvalidGranules)
{
    CacheBlk blk;
    blk.insert(TestAddr, false);
    blk.markPartial(BlkSize, 8);

    std::vector<bool> half_granule(BlkSize, false);
    std::fill(half_granule.begin() + 8, half_granule.begin() + 12, true);
    Packet half_write(makeRequest(half_granule), MemCmd::WriteReq);
    half_write.allocate();
    EXPECT_FALSE(blk.canWrite(&half_write, BlkSize));
    blk.markValidData(&half_write, BlkSize);
    EXPECT_FALSE(blk.hasValidData(8, 4));

    std::vector<bool> full_granule(BlkSize, false);
    std::fill(full_granule.begin() + 8, full_granule.begin() + 16, true);
    Packet full_write(makeRequest(full_granule), MemCmd::WriteReq);
    full_write.allocate();
    EXPECT_TRUE(blk.canWrite(&full_write, BlkSize));
    blk.markValidData(&full_write, BlkSize);
    EXPECT_TRUE(blk.hasValidData(8, 8));

    EXPECT_TRUE(blk.canWrite(&half_write, BlkSize));
}

TEST(PartialStoreTest, MaskedWritebackPreservesDisabledBytes)
{
    std::vector<bool> mask(BlkSize, false);
    mask[3] = true;
    mask[37] = true;
    std::unique_ptr<Packet> pkt(makeWriteback(mask, 0xa5));

    std::vector<uint8_t> destination(BlkSize, 0x5a);
    pkt->writeData(destination.data());

    for (unsigned i = 0; i < BlkSize; ++i) {
        EXPECT_EQ(destination[i], mask[i] ? 0xa5 : 0x5a);
    }
}

TEST(PartialStoreTest, NewerMaskedWritebackWinsInOverlay)
{
    std::vector<bool> first_mask(BlkSize, false);
    first_mask[3] = true;
    std::unique_ptr<Packet> first(makeWriteback(first_mask, 0x11));

    std::vector<bool> second_mask(BlkSize, false);
    second_mask[3] = true;
    second_mask[37] = true;
    std::unique_ptr<Packet> second(makeWriteback(second_mask, 0x22));

    std::vector<uint8_t> fill(BlkSize, 0x5a);
    first->writeDataToBlock(fill.data(), BlkSize);
    second->writeDataToBlock(fill.data(), BlkSize);
    EXPECT_EQ(fill[3], 0x22);
    EXPECT_EQ(fill[37], 0x22);
    EXPECT_EQ(fill[4], 0x5a);
}

TEST(PartialStoreTest, PartialLineMetaTracksLruAndCapacity)
{
    CacheBlk first;
    CacheBlk second;
    CacheBlk third;
    PartialLineMetaTable meta(2);

    meta.insert(&first);
    meta.insert(&second);
    EXPECT_TRUE(meta.full());
    ASSERT_EQ(meta.oldestFirst().size(), 2);
    EXPECT_EQ(meta.oldestFirst()[0], &first);

    meta.touch(&first);
    EXPECT_EQ(meta.oldestFirst()[0], &second);

    meta.erase(&second);
    EXPECT_FALSE(meta.full());
    meta.insert(&third);
    EXPECT_EQ(meta.size(), 2);
    EXPECT_TRUE(meta.contains(&first));
    EXPECT_TRUE(meta.contains(&third));
}

} // anonymous namespace
} // namespace gem5
