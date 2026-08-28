#include <gtest/gtest.h>

#include "cpu/pred/btb/btb_llbpx.hh"
#include "cpu/pred/btb/btb_tage.hh"
#include "cpu/pred/btb/common.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{

namespace
{

BranchInfo
makeBranch(Addr pc, bool is_cond, bool is_call, bool is_return)
{
    BranchInfo info;
    info.pc = pc;
    info.target = pc + 4;
    info.isCond = is_cond;
    info.isDirect = !is_cond;
    info.isIndirect = false;
    info.isCall = is_call;
    info.isReturn = is_return;
    info.size = 4;
    return info;
}

} // namespace

TEST(BTBLLBPXTest, RcrTypeFilteringMatchesOriginalClasses)
{
    BTBLLBPX llbpx;
    auto call = makeBranch(0x1000, false, true, false);
    auto ret = makeBranch(0x2000, false, false, true);
    auto uncond = makeBranch(0x3000, false, false, false);
    auto cond = makeBranch(0x4000, true, false, false);

    EXPECT_TRUE(BTBLLBPX::TestAccess::shouldRecordRCR(llbpx, call, true));
    EXPECT_FALSE(BTBLLBPX::TestAccess::shouldRecordRCR(llbpx, cond, true));
    EXPECT_TRUE(BTBLLBPX::TestAccess::shouldRecordRCR(llbpx, ret, true));
    EXPECT_TRUE(BTBLLBPX::TestAccess::shouldRecordRCR(llbpx, uncond, true));
}

TEST(BTBLLBPXTest, RcrHashesUseOneRecordDistanceByDefault)
{
    BTBLLBPX llbpx;
    for (unsigned i = 0; i < 12; ++i) {
        auto branch = makeBranch(0x1000 + i * 0x100, false, false, false);
        BTBLLBPX::TestAccess::pushRCR(llbpx, 0, branch, true);
    }

    const auto &ids = BTBLLBPX::TestAccess::rcrIds(llbpx, 0);
    const Addr expected_ccid =
        BTBLLBPX::TestAccess::calcRCRHash(llbpx, 0, 8, 1, 1, 32);
    const Addr expected_pcid =
        BTBLLBPX::TestAccess::calcRCRHash(llbpx, 0, 8, 0, 1, 32);
    const Addr expected_bcid =
        BTBLLBPX::TestAccess::calcRCRHash(llbpx, 0, 2, 1, 1, 12);
    const Addr expected_pbcid =
        BTBLLBPX::TestAccess::calcRCRHash(llbpx, 0, 2, 0, 1, 12);

    EXPECT_EQ(ids.ccid, expected_ccid);
    EXPECT_EQ(ids.pcid, expected_pcid);
    EXPECT_EQ(ids.bcid, expected_bcid);
    EXPECT_EQ(ids.pbcid, expected_pbcid);
    EXPECT_NE(ids.ccid, ids.pcid);
    EXPECT_NE(ids.bcid, ids.pbcid);
}

TEST(BTBLLBPXTest, OriginalContextKeyUsesRcrHashInsteadOfLegacyMix)
{
    BTBLLBPX llbpx;
    BTBLLBPX::TestAccess::useOriginalRcr(llbpx) = true;

    for (unsigned i = 0; i < 12; ++i) {
        auto branch = makeBranch(0x1000 + i * 0x80, false, false, false);
        BTBLLBPX::TestAccess::pushRCR(llbpx, 0, branch, true);
    }

    boost::dynamic_bitset<> history(64, 0);
    const Addr key = BTBLLBPX::TestAccess::contextKey(
        llbpx, 0, 0x8000, 0x8010, history, 0);
    const Addr expected = BTBLLBPX::TestAccess::originalContextKey(
        llbpx, 0, 0x8010, 8, 1);

    EXPECT_EQ(key, expected);
}

TEST(BTBLLBPXTest, PatternBufferTracksContextReadinessByCid)
{
    BTBLLBPX llbpx;
    ASSERT_TRUE(BTBLLBPX::TestAccess::rememberPatternBuffer(
        llbpx, 0, 0x1234, false, 10, false));

    auto *entry = BTBLLBPX::TestAccess::findPatternBuffer(llbpx, 0, 0x1234);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->cid, 0x1234U);
    EXPECT_EQ(entry->readyTick, 10U);

    ASSERT_TRUE(BTBLLBPX::TestAccess::rememberPatternBuffer(
        llbpx, 0, 0x1234, true, 3, true));
    EXPECT_TRUE(entry->dirty);
    EXPECT_EQ(entry->readyTick, 3U);
}

TEST(BTBLLBPXTest, TimingLookupUsesContextBufferInsteadOfPatternKey)
{
    BTBLLBPX llbpx;
    BTBLLBPX::TestAccess::useOriginalRcr(llbpx) = true;

    for (unsigned i = 0; i < 12; ++i) {
        auto branch = makeBranch(0x1000 + i * 0x80, false, false, false);
        BTBLLBPX::TestAccess::pushRCR(llbpx, 0, branch, true);
    }

    BTBEntry entry;
    entry.pc = 0x4000;
    entry.target = 0x4004;
    entry.valid = true;
    entry.isCond = true;
    entry.alwaysTaken = false;
    entry.ctr = 1;

    boost::dynamic_bitset<> history(64, 0);
    const Addr cid = BTBLLBPX::TestAccess::contextIdForDepth(
        llbpx, 0, 0, false);
    const Addr ctag = BTBLLBPX::TestAccess::contextTag(llbpx, cid, entry.pc);
    auto &ctx = BTBLLBPX::TestAccess::contexts(llbpx).allocate(cid, ctag);
    ctx.patternKey = BTBLLBPX::TestAccess::patternKeyForTable(
        llbpx, 0, 0x3ff0, entry.pc, cid, 0, 0, 0);
    const Addr ptag = BTBLLBPX::TestAccess::patternTag(
        llbpx, ctx.patternKey, entry.pc, 0);
    auto *pattern = BTBLLBPX::TestAccess::patternSet(ctx).insert(ctx.patternKey);
    ASSERT_NE(pattern, nullptr);
    pattern->tag = ptag;
    pattern->counter = 1;
    pattern->providerDepth = 0;

    ASSERT_TRUE(BTBLLBPX::TestAccess::rememberPatternBuffer(
        llbpx, 0, cid, false, 0, false));
    auto meta = BTBLLBPX::TestAccess::lookup(
        llbpx, 0, 0x3ff0, entry, true, history, 0, -1);
    EXPECT_TRUE(meta.patternHit);
    EXPECT_TRUE(meta.providerTimingReady);
    EXPECT_EQ(meta.cid, cid);
}

TEST(BTBLLBPXTest, AdaptiveDepthSelectsDeepContextAndFiltersTables)
{
    BTBTAGE tage(8, 2, 64, 4, false);
    BTBLLBPX llbpx(true);
    llbpx.setTage(&tage);
    BTBLLBPX::TestAccess::useOriginalRcr(llbpx) = true;

    for (unsigned i = 0; i < 80; ++i) {
        auto branch = makeBranch(0x1000 + i * 0x40, false, false, false);
        BTBLLBPX::TestAccess::pushRCR(llbpx, 0, branch, true);
    }

    BTBEntry entry;
    entry.pc = 0x5000;
    entry.target = 0x5004;
    entry.valid = true;
    entry.isCond = true;
    entry.alwaysTaken = false;
    entry.ctr = 1;

    boost::dynamic_bitset<> history(64, 0);
    const Addr shallowCid =
        BTBLLBPX::TestAccess::contextIdForDepth(llbpx, 0, 0, false);
    const Addr deepCid =
        BTBLLBPX::TestAccess::contextIdForDepth(llbpx, 0, 1, false);
    const Addr bcid = BTBLLBPX::TestAccess::rcrIds(llbpx, 0).bcid;

    auto &shallowCtx = BTBLLBPX::TestAccess::contexts(llbpx).allocate(
        shallowCid, BTBLLBPX::TestAccess::contextTag(llbpx, shallowCid, entry.pc));
    shallowCtx.patternKey = 0;

    auto &deepCtx = BTBLLBPX::TestAccess::contexts(llbpx).allocate(
        deepCid, BTBLLBPX::TestAccess::contextTag(llbpx, deepCid, entry.pc));
    const unsigned deepOnlyTable = 6; // hist len = 28 in the unit-test TAGE ctor
    ASSERT_LT(22U, tage.getHistoryLength(deepOnlyTable));
    const Addr deepKey = BTBLLBPX::TestAccess::patternKeyForTable(
        llbpx, 0, 0x4ff0, entry.pc, deepCid, 1, deepOnlyTable, 0);
    deepCtx.patternKey = deepKey;
    auto *pattern = BTBLLBPX::TestAccess::patternSet(deepCtx).insert(deepKey);
    ASSERT_NE(pattern, nullptr);
    pattern->tag = BTBLLBPX::TestAccess::patternTag(llbpx, deepKey, entry.pc, 0);
    pattern->counter = 1;
    pattern->providerDepth = deepOnlyTable;

    auto shallowMeta = BTBLLBPX::TestAccess::lookup(
        llbpx, 0, 0x4ff0, entry, true, history, 0, -1);
    EXPECT_TRUE(shallowMeta.contextHit);
    EXPECT_FALSE(shallowMeta.patternHit);
    EXPECT_EQ(shallowMeta.wi, 0U);

    auto &ci = BTBLLBPX::TestAccess::ctt(llbpx).allocate(bcid, bcid);
    ci.wi = 1;
    ci.fullPatternSets = 1;

    auto deepMeta = BTBLLBPX::TestAccess::lookup(
        llbpx, 0, 0x4ff0, entry, true, history, 0, -1);
    EXPECT_TRUE(deepMeta.contextHit);
    EXPECT_TRUE(deepMeta.patternHit);
    EXPECT_EQ(deepMeta.wi, 1U);
    EXPECT_EQ(deepMeta.hitHistIdx, static_cast<int>(deepOnlyTable));
}

TEST(BTBLLBPXTest, UpdateWithoutTimingStillWritesPatternState)
{
    BTBLLBPX llbpx;

    BTBEntry entry;
    entry.pc = 0x6000;
    entry.target = 0x6004;
    entry.valid = true;
    entry.isCond = true;
    entry.alwaysTaken = false;
    entry.ctr = 0;

    const Addr cid = 0x12340;
    const Addr ctag = BTBLLBPX::TestAccess::contextTag(llbpx, cid, entry.pc);
    const Addr key = 0x56789;
    auto &ctx = BTBLLBPX::TestAccess::contexts(llbpx).allocate(cid, ctag);
    auto *pattern = BTBLLBPX::TestAccess::patternSet(ctx).insert(key);
    ASSERT_NE(pattern, nullptr);
    pattern->tag = BTBLLBPX::TestAccess::patternTag(llbpx, key, entry.pc, 0);
    pattern->counter = -1;
    pattern->providerDepth = 3;

    ASSERT_EQ(BTBLLBPX::TestAccess::findPatternBuffer(llbpx, 0, cid), nullptr);

    FetchTarget stream;
    stream.tid = 0;
    stream.startPC = 0x5ff0;
    stream.exeBranchInfo = entry;
    stream.exeTaken = true;
    stream.resolved = true;
    stream.predBranchInfo = entry;
    stream.updateBTBEntries = {entry};
    stream.updateIsOldEntry = true;
    auto meta = BTBLLBPX::TestAccess::makeProviderMeta(
        stream.tid, stream.startPC, entry.pc, 0, cid, ctag, key, false);
    BTBLLBPX::TestAccess::attachMeta(llbpx, stream, meta);

    llbpx.update(stream);

    EXPECT_EQ(pattern->counter, 0);
    auto *bufferEntry = BTBLLBPX::TestAccess::findPatternBuffer(llbpx, 0, cid);
    ASSERT_NE(bufferEntry, nullptr);
    EXPECT_TRUE(bufferEntry->dirty);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
