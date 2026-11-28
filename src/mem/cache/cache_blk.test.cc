#include <gtest/gtest.h>

#include "mem/cache/cache_blk.hh"

namespace gem5
{
static Tick testTick = 0;
}

using namespace gem5;

namespace
{
class CacheBlkLocks : public ::testing::Test
{
    Tick *oldTick = nullptr;

    void SetUp() override
    {
        oldTick = Gem5Internal::_curTickPtr;
        Gem5Internal::_curTickPtr = &testTick;
    }

    void TearDown() override { Gem5Internal::_curTickPtr = oldTick; }
};

RequestPtr
request(ContextID context, Addr address, unsigned size, bool llsc = true)
{
    auto req = std::make_shared<Request>(address, size, llsc ? Request::Flags(Request::LLSC) : Request::Flags(), 0);
    req->setContext(context);
    return req;
}

void
loadLocked(CacheBlk &block, ContextID context, Addr address, unsigned size = 8)
{
    Packet pkt(request(context, address, size), MemCmd::LoadLockedReq);
    block.trackLoadLocked(&pkt);
}

bool
write(CacheBlk &block, ContextID context, Addr address, unsigned size = 8, bool conditional = true)
{
    Packet pkt(request(context, address, size, conditional), conditional ? MemCmd::StoreCondReq : MemCmd::WriteReq);
    bool result = block.checkWrite(&pkt);
    if (conditional) {
        EXPECT_EQ(pkt.req->getExtraData(), result ? 1 : 0);
    }
    return result;
}
}

TEST_F(CacheBlkLocks, EmptyAndRepeatedConditional)
{
    CacheBlk block;
    EXPECT_FALSE(write(block, 0, 0x1000));
    EXPECT_TRUE(write(block, 0, 0x1000, 8, false));
    loadLocked(block, 0, 0x1000);
    EXPECT_TRUE(write(block, 0, 0x1000));
    EXPECT_FALSE(write(block, 0, 0x1000));
}

TEST_F(CacheBlkLocks, ContextAndRangeInvalidation)
{
    CacheBlk block;
    loadLocked(block, 0, 0x1000);
    EXPECT_TRUE(write(block, 1, 0x1010, 8, false));
    EXPECT_TRUE(write(block, 0, 0x1000));
    loadLocked(block, 0, 0x1000);
    EXPECT_TRUE(write(block, 1, 0x1004, 8, false));
    EXPECT_FALSE(write(block, 0, 0x1000));
    loadLocked(block, 0, 0x1000);
    EXPECT_TRUE(write(block, 0, 0x1000, 8, false));
    EXPECT_TRUE(write(block, 0, 0x1000));
}

TEST_F(CacheBlkLocks, RepeatedSubrangeAndOtherContext)
{
    CacheBlk block;
    loadLocked(block, 0, 0x1000, 16);
    loadLocked(block, 0, 0x1004, 4);
    EXPECT_FALSE(write(block, 0, 0x1000, 16));
    EXPECT_TRUE(write(block, 0, 0x1004, 4));
    loadLocked(block, 0, 0x2000);
    loadLocked(block, 0, 0x3000);
    EXPECT_FALSE(write(block, 0, 0x4000));
    EXPECT_TRUE(write(block, 0, 0x2000));
    EXPECT_TRUE(write(block, 0, 0x3000));
    loadLocked(block, 0, 0x1000);
    loadLocked(block, 1, 0x1000);
    EXPECT_TRUE(write(block, 1, 0x1000));
    EXPECT_FALSE(write(block, 0, 0x1000));
}

TEST_F(CacheBlkLocks, MoveAndInvalidate)
{
    CacheBlk source, target;
    source.insert(0x10, false);
    loadLocked(source, 0, 0x1000);
    target = std::move(source);
    EXPECT_FALSE(source.isValid());
    EXPECT_TRUE(target.isValid());
    EXPECT_FALSE(write(source, 0, 0x1000));
    EXPECT_TRUE(write(target, 0, 0x1000));
    loadLocked(target, 1, 0x1000);
    target.invalidate();
    EXPECT_FALSE(write(target, 1, 0x1000));
}
