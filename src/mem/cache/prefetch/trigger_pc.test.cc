// Copyright (c) 2026 Institute of Computing Technology, CAS
// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/cache/prefetch/trigger_pc.hh"

namespace gem5
{
namespace prefetch
{

GTestTickHandler triggerPCTickHandler;

TEST(PrefetchTriggerPC, CapturesDemandPC)
{
    Request demand(0x1000, 8, 0, 0, 0x1234, 0);
    TriggerPC trigger(demand);
    ASSERT_TRUE(trigger.hasPC());
    EXPECT_EQ(trigger.getPC(), 0x1234);
    EXPECT_FALSE(demand.hasPrefetchTriggerPC());

    Request prefetch(0x2000, 64, Request::PREFETCH, 1);
    trigger.tag(prefetch);
    ASSERT_TRUE(prefetch.hasPrefetchTriggerPC());
    EXPECT_EQ(prefetch.getPrefetchTriggerPC(), demand.getPC());
    // Trigger attribution also works with tag_prefetch=false / PA requests.
    EXPECT_FALSE(prefetch.hasPC());
}

TEST(PrefetchTriggerPC, PreservesZeroPC)
{
    Request demand(0x1000, 8, 0, 0, 0, 0);
    Request prefetch(0x2000, 64, Request::PREFETCH, 1);
    TriggerPC(demand).tag(prefetch);
    ASSERT_TRUE(prefetch.hasPrefetchTriggerPC());
    EXPECT_EQ(prefetch.getPrefetchTriggerPC(), 0);
}

TEST(PrefetchTriggerPC, DoesNotInventMissingPC)
{
    Request physical(0x1000, 64, 0, 0);
    TriggerPC trigger(physical);
    EXPECT_FALSE(trigger.hasPC());
    Request prefetch(0x2000, 64, Request::PREFETCH, 1);
    trigger.tag(prefetch);
    EXPECT_FALSE(prefetch.hasPrefetchTriggerPC());
}

TEST(PrefetchTriggerPC, SnapshotsBufferedTrigger)
{
    Request demand(0x1000, 8, 0, 0, 0x1234, 0);
    TriggerPC buffered(demand);
    demand.setPC(0x5678);
    const TriggerPC copied(buffered);
    Request prefetch(0x2000, 64, Request::PREFETCH, 1);
    copied.tag(prefetch);
    EXPECT_EQ(prefetch.getPrefetchTriggerPC(), 0x1234);
    EXPECT_EQ(demand.getPC(), 0x5678);
}

TEST(PrefetchTriggerPC, RetainsOriginAcrossDemandMergeAndPrefetchChain)
{
    Request parent(0x2000, 64, Request::PREFETCH, 1);
    parent.setPC(0x1234);
    parent.setPrefetchTriggerPC(0x1234);
    // SDBP refill uses a merged demand PC and clears the prefetch flag.
    parent.setPC(0x5678);
    parent.clearFlags(Request::PREFETCH);
    Request child(0x3000, 64, Request::PREFETCH, 2);
    TriggerPC(parent).tag(child);
    ASSERT_TRUE(child.hasPrefetchTriggerPC());
    EXPECT_EQ(child.getPrefetchTriggerPC(), 0x1234);
    EXPECT_EQ(parent.getPC(), 0x5678);
    EXPECT_FALSE(parent.isPrefetch());
}

TEST(PrefetchTriggerPC, SurvivesRequestCopyAndTranslation)
{
    Request translated(0x2000, 64, Request::PF_EXCLUSIVE, 1, 0x1234, 0);
    translated.setPrefetchTriggerPC(0x5678);
    translated.setPaddr(0x80002000);
    Request forwarded(translated);
    EXPECT_EQ(forwarded.getPrefetchTriggerPC(), 0x5678);
    EXPECT_EQ(forwarded.getPC(), 0x1234);
    EXPECT_EQ(forwarded.getPaddr(), 0x80002000);
}

TEST(PrefetchTriggerPC, ClearsOriginOnRequestReuse)
{
    Request request(0x2000, 64, Request::PREFETCH, 1, 0x1234, 0);
    request.setPrefetchTriggerPC(0x5678);
    request.setVirt(0x3000, 8, 0, 0, 0x9abc);
    EXPECT_FALSE(request.hasPrefetchTriggerPC());
    Request prefetch(0x4000, 64, Request::PREFETCH, 1);
    TriggerPC(request).tag(prefetch);
    EXPECT_EQ(prefetch.getPrefetchTriggerPC(), 0x9abc);
}

TEST(PrefetchTriggerPC, IndependentOfPrefetchSourceAndXsMetadata)
{
    for (unsigned source = 0; source < enums::Num_PrefetchSourceType; ++source) {
        Request prefetch(0x2000, 64, Request::PREFETCH, 1);
        prefetch.setPrefetchTriggerPC(0x1234);
        prefetch.setXsMetadata(Request::XsMetadata(
            static_cast<PrefetchSourceType>(source), 3));
        Request forwarded(prefetch);
        EXPECT_EQ(forwarded.getPrefetchTriggerPC(), 0x1234);
        EXPECT_EQ(forwarded.getXsMetadata().prefetchDepth, 3);
        EXPECT_EQ(unsigned(forwarded.getXsMetadata().prefetchSource), source);
    }
}

TEST(PrefetchTriggerPC, CopiesAndResetsL2RequestSource)
{
    Request request(0x2000, 64, Request::PREFETCH, 1, 0x1234, 0);
    request.setPrefetchTriggerPC(0x5678);
    request.setL2RequestSource(Request::L2RequestSource::L2Prefetch);
    Request copy(request);
    EXPECT_EQ(copy.l2RequestSource(), Request::L2RequestSource::L2Prefetch);
    copy.setVirt(0x3000, 8, 0, 0, 0x9abc);
    EXPECT_EQ(copy.l2RequestSource(), Request::L2RequestSource::Unclassified);
}

TEST(PrefetchTriggerPC, MissingMergedDemandPcDoesNotErasePrefetchOrigin)
{
    Request request(0x2000, 64, Request::PREFETCH, 1, 0x1234, 0);
    request.setPrefetchTriggerPC(0x5678);
    request.clearPC();
    EXPECT_FALSE(request.hasPC());
    EXPECT_TRUE(request.hasPrefetchTriggerPC());
    EXPECT_EQ(request.getPrefetchTriggerPC(), 0x5678);
}

} // namespace prefetch
} // namespace gem5
