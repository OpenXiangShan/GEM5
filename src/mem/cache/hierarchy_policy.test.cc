/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include "mem/cache/cache_blk.hh"
#include "mem/cache/hierarchy_policy.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace cache_hierarchy
{
namespace
{

std::unique_ptr<Packet>
readPacket(MemCmd cmd = MemCmd::ReadSharedReq, Request::Flags flags = 0)
{
    auto req = std::make_shared<Request>(0x1000, 64, flags, 0);
    return std::make_unique<Packet>(req, cmd);
}

class CacheHierarchyPolicy : public ::testing::Test
{
  protected:
    void SetUp() override { curEventQueue(getEventQueue(0)); }
};

TEST_F(CacheHierarchyPolicy, OrdinaryReadPromiseDoesNotRequestInvalidation)
{
    const Policy policy(enums::xiangshan_single_core);
    auto read = readPacket();
    read->writableFillExpected = policy.expectWritableRead(read.get(), false);
    EXPECT_TRUE(read->writableFillExpected);
    EXPECT_EQ(read->cmd, MemCmd::ReadSharedReq);
    EXPECT_FALSE(read->needsWritable());
    EXPECT_FALSE(read->isInvalidate());
    EXPECT_TRUE(policy.allowDeferredWritable(read.get()));
}

TEST_F(CacheHierarchyPolicy, ReadOnlyPrefetchAndUncacheableKeepTheirProtocol)
{
    const Policy policy(enums::xiangshan_single_core);
    auto read = readPacket();
    EXPECT_FALSE(policy.expectWritableRead(read.get(), true));
    for (auto flag : {Request::PREFETCH, Request::PF_EXCLUSIVE,
                      Request::INST_FETCH, Request::UNCACHEABLE}) {
        auto excluded = readPacket(MemCmd::ReadSharedReq, flag);
        EXPECT_FALSE(policy.expectWritableRead(excluded.get(), false));
    }
    for (auto cmd : {MemCmd::ReadCleanReq, MemCmd::HardPFReq,
                     MemCmd::WritebackDirty, MemCmd::CleanEvict}) {
        auto excluded = readPacket(cmd);
        EXPECT_FALSE(policy.expectWritableRead(excluded.get(), false));
    }
}

TEST_F(CacheHierarchyPolicy, PromiseDoesNotLeakIntoSnoopsOrPacketCopies)
{
    auto read = readPacket();
    read->writableFillExpected = true;
    read->bypassL2 = true;
    Packet snoop(read.get(), true, false);
    EXPECT_FALSE(snoop.writableFillExpected);
    EXPECT_FALSE(snoop.bypassL2);
}

TEST_F(CacheHierarchyPolicy, LegacyDoesNotEnableHierarchyOverrides)
{
    const Policy legacy(enums::legacy);
    auto read = readPacket();
    read->writableFillExpected = true;
    read->bypassL2 = true;
    EXPECT_FALSE(legacy.expectWritableRead(read.get(), false));
    EXPECT_FALSE(legacy.allowDeferredWritable(read.get()));
    EXPECT_FALSE(legacy.dropCleanL1Data(1));
    EXPECT_FALSE(legacy.captureBypassFill(1, read.get()));
    auto wb = readPacket(MemCmd::WritebackDirty);
    wb->bypassL2 = true;
    EXPECT_FALSE(legacy.forwardBypassWriteback(2, wb.get()));
}

TEST_F(CacheHierarchyPolicy, CleanL1OverrideDoesNotChangeLowerLevelClusivity)
{
    const Policy policy(enums::xiangshan_single_core);
    EXPECT_TRUE(policy.dropCleanL1Data(1));
    EXPECT_FALSE(policy.dropCleanL1Data(2));
    EXPECT_FALSE(policy.dropCleanL1Data(3));
    auto wb = readPacket(MemCmd::WritebackDirty);
    wb->bypassL2 = true;
    EXPECT_TRUE(policy.forwardBypassWriteback(2, wb.get()));
    EXPECT_FALSE(policy.forwardBypassWriteback(3, wb.get()));
    EXPECT_TRUE(policy.consumeBypassWriteback(3, wb.get()));
}

TEST_F(CacheHierarchyPolicy, WritablePermissionDoesNotMakeVictimDirty)
{
    CacheBlk block;
    block.insert(0x1000, false);
    block.setCoherenceBits(CacheBlk::ReadableBit | CacheBlk::WritableBit);
    EXPECT_FALSE(block.isSet(CacheBlk::DirtyBit));
    block.bypassL2 = true;
    EXPECT_FALSE(block.isSet(CacheBlk::DirtyBit));
    block.invalidate();
    EXPECT_FALSE(block.bypassL2);
}

TEST_F(CacheHierarchyPolicy, BypassMetadataMovesAndClearsWithoutChangingDirtyState)
{
    CacheBlk original, moved;
    original.insert(0x1000, false);
    original.setCoherenceBits(CacheBlk::ReadableBit);
    original.bypassL2 = true;
    EXPECT_FALSE(original.isSet(CacheBlk::DirtyBit));

    moved = std::move(original);
    EXPECT_TRUE(moved.bypassL2);
    EXPECT_FALSE(moved.isSet(CacheBlk::DirtyBit));
    EXPECT_FALSE(original.bypassL2);
    EXPECT_FALSE(original.isValid());
    moved.invalidate();
    EXPECT_FALSE(moved.bypassL2);

    auto response = readPacket(MemCmd::ReadResp);
    response->bypassL2 = true;
    Packet snoop(response.get(), true, false);
    EXPECT_FALSE(snoop.bypassL2);
}

} // anonymous namespace
} // namespace cache_hierarchy
} // namespace gem5
