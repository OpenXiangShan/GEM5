/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of Sciences
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

#include "mem/cache/clean_wb_preserve.hh"

namespace gem5
{
namespace
{

TEST(L2CleanWbPreserveTest, UncachedAboveSendsUncachedRegardlessOfFlag)
{
    CleanWbProbeState probe;
    const auto d_off = decidePreservedWritebackClean(false, false, &probe);
    const auto d_on = decidePreservedWritebackClean(true, false, &probe);
    EXPECT_EQ(d_off.action, CleanWbAction::SendUncached);
    EXPECT_EQ(d_on.action, CleanWbAction::SendUncached);
}

TEST(L2CleanWbPreserveTest, DefaultOffDropsAllCachedAboveWritebackClean)
{
    CleanWbProbeState probe;
    probe.recordUpperHit(/*is_l1i=*/true, /*writes_back_clean=*/false,
                         /*is_dirty=*/false);
    ASSERT_TRUE(probe.anyCachedAbove());
    const auto d = decidePreservedWritebackClean(false, true, &probe);
    EXPECT_EQ(d.action, CleanWbAction::Drop);

    const auto d_null = decidePreservedWritebackClean(false, true, nullptr);
    EXPECT_EQ(d_null.action, CleanWbAction::Drop);
}

TEST(L2CleanWbPreserveTest, L1IExclusiveAndSharedWithCleanL1DArePreserved)
{
    {
        CleanWbProbeState l1i_only;
        l1i_only.recordUpperHit(true, false, false);
        const auto d = decidePreservedWritebackClean(true, true, &l1i_only);
        EXPECT_EQ(d.action, CleanWbAction::SendPreservedBlockCached);
        EXPECT_EQ(d.kind, CleanWbProbeKind::L1IOnly);
    }

    {
        CleanWbProbeState l1i_and_l1d_clean;
        l1i_and_l1d_clean.recordUpperHit(true, false, false);
        l1i_and_l1d_clean.recordUpperHit(false, false, false);
        const auto d = decidePreservedWritebackClean(
            true, true, &l1i_and_l1d_clean);
        EXPECT_EQ(d.action, CleanWbAction::SendPreservedBlockCached);
        EXPECT_EQ(d.kind, CleanWbProbeKind::L1IAndL1DClean);
    }

    {
        CleanWbProbeState l1d_clean_only;
        l1d_clean_only.recordUpperHit(false, false, false);
        const auto d = decidePreservedWritebackClean(
            true, true, &l1d_clean_only);
        EXPECT_EQ(d.action, CleanWbAction::SendPreservedBlockCached);
        EXPECT_EQ(d.kind, CleanWbProbeKind::L1DCleanOnly);
    }
}

TEST(L2CleanWbPreserveTest, DirtyUpperHitTrumpsCleanUpperHoldersAndDrops)
{
    CleanWbProbeState dirty_only;
    dirty_only.recordUpperHit(false, false, true);
    const auto d1 = decidePreservedWritebackClean(true, true, &dirty_only);
    EXPECT_EQ(d1.action, CleanWbAction::Drop);
    EXPECT_EQ(d1.kind, CleanWbProbeKind::DirtyAbove);

    // Even if L1I also holds clean alongside a dirty L1D or MSHR pending write,
    // dirty must dominate so L2's stale clean copy is never sent to L3.
    CleanWbProbeState dirty_and_l1i;
    dirty_and_l1i.recordUpperHit(true, false, false);
    dirty_and_l1i.recordUpperHit(false, false, true);
    const auto d2 = decidePreservedWritebackClean(true, true, &dirty_and_l1i);
    EXPECT_EQ(d2.action, CleanWbAction::Drop);
    EXPECT_EQ(d2.kind, CleanWbProbeKind::DirtyAbove);
}

TEST(L2CleanWbPreserveTest, WritebackCleanOnlyUpperCacheDropsRedundantWb)
{
    CleanWbProbeState walker_only;
    walker_only.recordUpperHit(false, true, false);
    const auto d = decidePreservedWritebackClean(true, true, &walker_only);
    EXPECT_EQ(d.action, CleanWbAction::Drop);
    EXPECT_EQ(d.kind, CleanWbProbeKind::WbCleanOnly);

    // Even a read-only upper cache configured with writeback_clean=True must
    // classify as WbCleanOnly and not spuriously preserve.
    CleanWbProbeState ro_wb_clean;
    ro_wb_clean.recordUpperHit(true, true, false);
    const auto d_ro = decidePreservedWritebackClean(true, true, &ro_wb_clean);
    EXPECT_EQ(d_ro.action, CleanWbAction::Drop);
    EXPECT_EQ(d_ro.kind, CleanWbProbeKind::WbCleanOnly);

    // If non-writeback_clean L1I also holds the line alongside WalkerCache,
    // L1I would block WalkerCache's clean writeback on the shared L1-L2 bus
    // and then evict with data-less CleanEvict, so preservation must win.
    walker_only.recordUpperHit(true, false, false);
    const auto d2 = decidePreservedWritebackClean(true, true, &walker_only);
    EXPECT_EQ(d2.action, CleanWbAction::SendPreservedBlockCached);
    EXPECT_EQ(d2.kind, CleanWbProbeKind::L1IOnly);
}

TEST(L2CleanWbPreserveTest, OpaqueCachedAboveDropsConservatively)
{
    CleanWbProbeState empty_probe;
    ASSERT_FALSE(empty_probe.anyCachedAbove());
    const auto d = decidePreservedWritebackClean(true, true, &empty_probe);
    EXPECT_EQ(d.action, CleanWbAction::Drop);
    EXPECT_EQ(d.kind, CleanWbProbeKind::OpaqueCachedAbove);

    CleanWbProbeState opaque_with_l1i;
    opaque_with_l1i.recordUpperHit(true, false, false);
    opaque_with_l1i.recordOpaqueHit();
    const auto d2 =
        decidePreservedWritebackClean(true, true, &opaque_with_l1i);
    EXPECT_EQ(d2.action, CleanWbAction::Drop);
    EXPECT_EQ(d2.kind, CleanWbProbeKind::OpaqueCachedAbove);
}

TEST(L2CleanWbPreserveTest, CoherentXBarSinksCleanEvictAndUnmarkedWb)
{
    // Default crossbar behavior (sink_cached_writeback_clean=true): sink all
    // cached CleanEvict and WritebackClean packets regardless of tag, and never
    // sink uncached clean evictions or WritebackDirty (!clean_evict && !wb_clean).
    EXPECT_TRUE(shouldSinkCleanEviction(true, false, true, false, true));
    EXPECT_TRUE(shouldSinkCleanEviction(true, false, true, true, true));
    EXPECT_TRUE(shouldSinkCleanEviction(true, true, false, false, true));
    EXPECT_FALSE(shouldSinkCleanEviction(false, false, true, true, true));
    EXPECT_FALSE(shouldSinkCleanEviction(false, true, false, false, true));
    EXPECT_FALSE(shouldSinkCleanEviction(true, false, false, false, true));

    // L2->L3 buses with sink_cached_writeback_clean=false:
    // forward only WritebackClean explicitly tagged PRESERVE_CLEAN_WB;
    // still sink cached CleanEvict and peer-cached unmarked WritebackClean.
    EXPECT_FALSE(shouldSinkCleanEviction(true, false, true, true, false));
    EXPECT_TRUE(shouldSinkCleanEviction(true, false, true, false, false));
    EXPECT_TRUE(shouldSinkCleanEviction(true, true, false, false, false));
    EXPECT_FALSE(shouldSinkCleanEviction(false, false, true, true, false));
    EXPECT_FALSE(shouldSinkCleanEviction(false, true, false, false, false));
    EXPECT_FALSE(shouldSinkCleanEviction(true, false, false, false, false));
}

} // namespace
} // namespace gem5
