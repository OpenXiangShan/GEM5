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

TEST(PartialStoreTest, EvictionProbeCommandClassification)
{
    for (const auto cmd : {MemCmd::WritebackDirty,
                           MemCmd::WritebackClean,
                           MemCmd::CleanEvict}) {
        Packet pkt(makeRequest(std::vector<bool>(BlkSize, true)), cmd);
        EXPECT_TRUE(pkt.isEviction());
        EXPECT_TRUE(pkt.mustCheckAbove());
        EXPECT_FALSE(pkt.needsResponse());
    }

    Packet read_shared(
        makeRequest(std::vector<bool>(BlkSize, true)), MemCmd::ReadSharedReq);
    EXPECT_FALSE(read_shared.isEviction());
    EXPECT_FALSE(read_shared.mustCheckAbove());
    EXPECT_TRUE(read_shared.needsResponse());
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

TEST(PartialStoreTest, MaskedWritebackMissInstallsPartialData)
{
    std::vector<bool> mask(BlkSize, false);
    std::fill(mask.begin() + 16, mask.begin() + 24, true);
    std::unique_ptr<Packet> pkt(makeWriteback(mask, 0xd7));

    std::vector<uint8_t> block_data(BlkSize, 0x00);
    CacheBlk blk;
    blk.data = block_data.data();
    blk.insert(TestAddr, false);
    blk.markPartial(BlkSize);

    pkt->writeDataToBlock(blk.data, BlkSize);
    blk.markValidData(pkt.get(), BlkSize);

    EXPECT_TRUE(blk.isPartial());
    EXPECT_TRUE(blk.hasValidData(16, 8));
    EXPECT_FALSE(blk.hasValidData(0, 16));
    for (unsigned i = 0; i < BlkSize; ++i) {
        EXPECT_EQ(block_data[i], mask[i] ? 0xd7 : 0x00);
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

TEST(PartialStoreTest, FunctionalReadCombinesPartialAndFullData)
{
    std::vector<bool> partial_mask(BlkSize, false);
    std::fill(partial_mask.begin() + 12, partial_mask.begin() + 20, true);
    std::unique_ptr<Packet> partial(
        makeWriteback(partial_mask, 0xcc));
    std::unique_ptr<Packet> full(
        makeWriteback(std::vector<bool>(BlkSize, true), 0x33));

    auto read_req = std::make_shared<Request>(
        TestAddr, BlkSize, 0, Request::funcRequestorId);
    Packet read(read_req, MemCmd::ReadReq);
    read.allocate();

    EXPECT_FALSE(read.trySatisfyFunctional(partial.get()));
    EXPECT_TRUE(read.trySatisfyFunctional(full.get()));
    for (unsigned i = 0; i < BlkSize; ++i) {
        EXPECT_EQ(read.getConstPtr<uint8_t>()[i],
                  partial_mask[i] ? 0xcc : 0x33);
    }
}

TEST(PartialStoreTest, FunctionalMaskedWriteUpdatesEnabledBytes)
{
    std::vector<bool> mask(BlkSize, false);
    mask[5] = true;
    mask[41] = true;

    auto req = makeRequest(std::vector<bool>(BlkSize, true));
    Packet write(req, MemCmd::WriteReq);
    write.allocate();
    std::fill(write.getPtr<uint8_t>(),
              write.getPtr<uint8_t>() + BlkSize, 0xa5);
    req->setByteEnable(mask);

    std::vector<uint8_t> destination(BlkSize, 0x5a);
    EXPECT_FALSE(write.trySatisfyFunctional(
        nullptr, TestAddr, false, BlkSize, destination.data()));

    for (unsigned i = 0; i < BlkSize; ++i) {
        EXPECT_EQ(destination[i], mask[i] ? 0xa5 : 0x5a);
    }
}

} // anonymous namespace
} // namespace gem5
