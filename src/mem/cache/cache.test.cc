/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/cache/cache.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/tags/base_set_assoc.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "mem/tport.hh"
#include "params/BaseReplacementPolicy.hh"
#include "params/BaseSetAssoc.hh"
#include "params/Cache.hh"
#include "params/ClockDomain.hh"
#include "params/SetAssociative.hh"
#include "params/StubWorkload.hh"
#include "params/System.hh"
#include "sim/clock_domain.hh"
#include "sim/eventq.hh"
#include "sim/system.hh"
#include "sim/workload.hh"

namespace gem5
{

namespace
{

constexpr Addr TestAddr = 0x1000;
constexpr unsigned CacheLineSize = 64;

class FixedClockDomain : public ClockDomain
{
  public:
    explicit FixedClockDomain(const ClockDomainParams &params)
        : ClockDomain(params, nullptr)
    {
        _clockPeriod = 1;
    }
};

class CountingBypassPolicy : public replacement_policy::Base
{
  public:
    explicit CountingBypassPolicy(const BaseReplacementPolicyParams &params)
        : Base(params)
    {
    }

    void
    invalidate(const std::shared_ptr<replacement_policy::ReplacementData> &)
        override
    {
    }

    void
    touch(const std::shared_ptr<replacement_policy::ReplacementData> &) const
        override
    {
    }

    void
    reset(const std::shared_ptr<replacement_policy::ReplacementData> &) const
        override
    {
    }

    ReplaceableEntry *
    getVictim(const ReplacementCandidates &candidates) const override
    {
        ++legacyCalls;
        return candidates.front();
    }

    ReplaceableEntry *
    getVictim(const ReplacementCandidates &candidates,
              const PacketPtr) const override
    {
        ++packetCalls;
        return forceBypass ? nullptr : candidates.front();
    }

    std::shared_ptr<replacement_policy::ReplacementData>
    instantiateEntry() override
    {
        return std::make_shared<replacement_policy::ReplacementData>();
    }

    mutable unsigned legacyCalls = 0;
    mutable unsigned packetCalls = 0;
    bool forceBypass = true;
};

class UpperRequestPort : public RequestPort
{
  public:
    explicit UpperRequestPort(SimObject *owner)
        : RequestPort("upper", owner)
    {
    }

    bool
    recvTimingResp(PacketPtr pkt) override
    {
        response = pkt;
        return true;
    }

    void
    recvReqRetry() override
    {
        ++retryCount;
    }

    PacketPtr response = nullptr;
    unsigned retryCount = 0;
};

class DirtyResponderPort : public SimpleTimingPort
{
  public:
    explicit DirtyResponderPort(SimObject *owner)
        : SimpleTimingPort("dirty_responder", owner)
    {
    }

    AddrRangeList
    getAddrRanges() const override
    {
        return AddrRangeList{AddrRange(0, MaxAddr)};
    }

  protected:
    Tick
    recvAtomic(PacketPtr pkt) override
    {
        requestCmd = pkt->cmd;
        if (responseCacheResponding) {
            pkt->setCacheResponding();
        }
        if (responseHasSharers) {
            pkt->setHasSharers();
        }
        std::fill_n(pkt->getPtr<uint8_t>(), pkt->getSize(), 0x5a);
        pkt->makeAtomicResponse();
        return 1;
    }

  public:
    MemCmd requestCmd = MemCmd::InvalidCmd;
    bool responseCacheResponding = true;
    bool responseHasSharers = true;
};

class TestCache : public Cache
{
  public:
    explicit TestCache(const CacheParams &params)
        : Cache(params)
    {
    }

    void
    registerTestProbePoints()
    {
        regProbePoints();
    }
};

ClockDomainParams
makeClockDomainParams()
{
    ClockDomainParams params;
    params.name = "cache_test.clock";
    params.eventq_index = 0;
    return params;
}

StubWorkloadParams
makeWorkloadParams()
{
    StubWorkloadParams params;
    params.name = "cache_test.workload";
    params.eventq_index = 0;
    params.wait_for_remote_gdb = false;
    params.byte_order = ByteOrder::little;
    params.entry = 0;
    return params;
}

SystemParams
makeSystemParams(Workload *workload)
{
    SystemParams params;
    params.name = "cache_test.system";
    params.eventq_index = 0;
    params.arch_db = nullptr;
    params.auto_unlink_shared_backstore = false;
    params.cache_line_size = CacheLineSize;
    params.enable_difftest = false;
    params.enable_h_gcpt = false;
    params.enable_mem_dedup = false;
    params.enable_riscv_vector = false;
    params.exit_on_work_items = false;
    params.gcpt_file = "";
    params.gcpt_restorer_file = "";
    params.gcpt_restorer_size_limit = 0;
    params.init_param = 0;
    params.m5ops_base = 0;
    params.map_to_raw_cpt = false;
    params.mem_mode = enums::timing;
    params.mem_ranges.clear();
    params.memories.clear();
    params.mmap_using_noreserve = false;
    params.multi_thread = false;
    params.num_cpus = 1;
    params.num_work_ids = 0;
    params.readfile = "";
    params.redirect_paths.clear();
    params.restore_from_gcpt = false;
    params.shadow_rom_ranges.clear();
    params.shared_backstore = "";
    params.symbolfile = "";
    params.thermal_components.clear();
    params.thermal_model = nullptr;
    params.work_begin_ckpt_count = 0;
    params.work_begin_cpu_id_exit = -1;
    params.work_begin_exit_count = 0;
    params.work_cpus_ckpt_count = 0;
    params.work_end_ckpt_count = 0;
    params.work_end_exit_count = 0;
    params.work_item_id = -1;
    params.workload = workload;
    params.xiangshan_system = false;
    params.port_system_port_connection_count = 0;
    return params;
}

BaseReplacementPolicyParams
makeReplacementPolicyParams()
{
    BaseReplacementPolicyParams params;
    params.name = "cache_test.policy";
    params.eventq_index = 0;
    return params;
}

SetAssociativeParams
makeIndexingParams()
{
    SetAssociativeParams params;
    params.name = "cache_test.indexing";
    params.eventq_index = 0;
    params.assoc = 1;
    params.entry_size = CacheLineSize;
    params.num_slices = 0;
    params.size = CacheLineSize;
    params.slice_idx = 0;
    return params;
}

BaseSetAssocParams
makeTagsParams(ClockDomain *clock, System *system,
               BaseIndexingPolicy *indexing,
               replacement_policy::Base *replacement_policy)
{
    BaseSetAssocParams params;
    params.name = "cache_test.tags";
    params.eventq_index = 0;
    params.clk_domain = clock;
    params.power_model.clear();
    params.power_state = nullptr;
    params.block_size = CacheLineSize;
    params.entry_size = CacheLineSize;
    params.indexing_policy = indexing;
    params.sequential_access = false;
    params.size = CacheLineSize;
    params.system = system;
    params.tag_latency = Cycles(1);
    params.warmup_percentage = 0;
    params.assoc = 1;
    params.replacement_policy = replacement_policy;
    return params;
}

CacheParams
makeCacheParams(ClockDomain *clock, System *system, BaseTags *tags,
                replacement_policy::Base *replacement_policy)
{
    CacheParams params;
    params.name = "cache_test.l2";
    params.eventq_index = 0;
    params.clk_domain = clock;
    params.power_model.clear();
    params.power_state = nullptr;
    params.addr_ranges = {AddrRange(0, 0x10000)};
    params.arch_db = nullptr;
    params.assoc = 1;
    params.cache_level = 2;
    params.clusivity = enums::mostly_incl;
    params.compressor = nullptr;
    params.data_latency = Cycles(1);
    params.demand_mshr_reserve = 1;
    params.do_fast_writeline = true;
    params.force_hit = false;
    params.is_read_only = false;
    params.max_miss_count = 0;
    params.move_contractions = true;
    params.mshr_alloc_per_cycle = -1;
    params.mshrs = 1;
    params.num_slices = -1;
    params.pipe_latency = Cycles(0);
    params.prefetch_can_offload = true;
    params.prefetcher = nullptr;
    params.replace_expansions = true;
    params.replacement_policy = replacement_policy;
    params.response_latency = Cycles(1);
    params.sequential_access = false;
    params.simulate_dcache_refill = false;
    params.size = CacheLineSize;
    params.system = system;
    params.tag_latency = Cycles(1);
    params.tag_load_read_ports = 1;
    params.tags = tags;
    params.tgts_per_mshr = 4;
    params.warmup_percentage = 0;
    params.way_entries = 0;
    params.way_indexing_policy = nullptr;
    params.way_replacement_policy = nullptr;
    params.wpu = nullptr;
    params.write_allocator = nullptr;
    params.write_buffers = 1;
    params.writeback_clean = false;
    params.port_cpu_side_connection_count = 1;
    params.port_mem_side_connection_count = 1;
    return params;
}

class CacheTimingTest : public testing::Test
{
  protected:
    GTestTickHandler tickHandler;

    ClockDomainParams clockParams;
    FixedClockDomain clock;

    StubWorkloadParams workloadParams;
    StubWorkload workload;

    SystemParams systemParams;
    System system;

    BaseReplacementPolicyParams replacementPolicyParams;
    CountingBypassPolicy replacementPolicy;

    SetAssociativeParams indexingParams;
    SetAssociative indexing;

    BaseSetAssocParams tagsParams;
    BaseSetAssoc tags;

    CacheParams cacheParams;
    TestCache cache;

    UpperRequestPort upper;
    DirtyResponderPort lower;

    CacheTimingTest()
        : clockParams(makeClockDomainParams()),
          clock(clockParams),
          workloadParams(makeWorkloadParams()),
          workload(workloadParams),
          systemParams(makeSystemParams(&workload)),
          system(systemParams),
          replacementPolicyParams(makeReplacementPolicyParams()),
          replacementPolicy(replacementPolicyParams),
          indexingParams(makeIndexingParams()),
          indexing(indexingParams),
          tagsParams(makeTagsParams(&clock, &system, &indexing,
                                    &replacementPolicy)),
          tags(tagsParams),
          cacheParams(makeCacheParams(&clock, &system, &tags,
                                      &replacementPolicy)),
          cache(cacheParams),
          upper(&cache),
          lower(&cache)
    {
        curEventQueue(getEventQueue(0));
        upper.bind(cache.getPort("cpu_side"));
        cache.getPort("mem_side").bind(lower);
        system.regStats();
        tags.regStats();
        cache.regStats();
        cache.registerTestProbePoints();
        cache.init();
    }
};

TEST_F(CacheTimingTest, ReadCleanReqDirtyResponderFills)
{
    auto request = std::make_shared<Request>(
        TestAddr, CacheLineSize, Request::Flags(), Request::funcRequestorId);
    auto packet = std::make_unique<Packet>(request, MemCmd::ReadCleanReq,
                                           CacheLineSize);
    packet->allocate();

    ASSERT_TRUE(upper.sendTimingReq(packet.get()));

    EventQueue *eventq = getEventQueue(0);
    while (!upper.response && !eventq->empty()) {
        eventq->serviceOne();
    }

    EXPECT_EQ(lower.requestCmd, MemCmd::ReadSharedReq);
    EXPECT_EQ(replacementPolicy.packetCalls, 0U);
    EXPECT_EQ(replacementPolicy.legacyCalls, 1U);
    EXPECT_TRUE(cache.inCache(TestAddr, false));
    ASSERT_NE(upper.response, nullptr);
    EXPECT_EQ(upper.response->cmd, MemCmd::ReadResp);
    EXPECT_TRUE(upper.response->hasSharers());
    EXPECT_FALSE(upper.response->cacheResponding());
}

TEST_F(CacheTimingTest, ReadSharedReqBypassesWithoutAllocating)
{
    lower.responseCacheResponding = false;
    lower.responseHasSharers = false;

    auto request = std::make_shared<Request>(
        TestAddr, CacheLineSize, Request::Flags(), Request::funcRequestorId);
    auto packet = std::make_unique<Packet>(request, MemCmd::ReadSharedReq,
                                           CacheLineSize);
    packet->allocate();

    ASSERT_TRUE(upper.sendTimingReq(packet.get()));

    EventQueue *eventq = getEventQueue(0);
    while (!upper.response && !eventq->empty()) {
        eventq->serviceOne();
    }

    EXPECT_EQ(lower.requestCmd, MemCmd::ReadSharedReq);
    EXPECT_EQ(replacementPolicy.packetCalls, 1U);
    EXPECT_EQ(replacementPolicy.legacyCalls, 0U);
    EXPECT_FALSE(cache.inCache(TestAddr, false));
    ASSERT_NE(upper.response, nullptr);
    EXPECT_EQ(upper.response->cmd, MemCmd::ReadResp);
    EXPECT_FALSE(upper.response->hasSharers());
    EXPECT_FALSE(upper.response->cacheResponding());
    EXPECT_EQ(upper.response->getConstPtr<uint8_t>()[0], 0x5a);
}

TEST_F(CacheTimingTest, ReadSharedReqAdmittedCleanFillAllocates)
{
    lower.responseCacheResponding = false;
    lower.responseHasSharers = false;
    replacementPolicy.forceBypass = false;

    auto request = std::make_shared<Request>(
        TestAddr, CacheLineSize, Request::Flags(), Request::funcRequestorId);
    auto packet = std::make_unique<Packet>(request, MemCmd::ReadSharedReq,
                                           CacheLineSize);
    packet->allocate();

    ASSERT_TRUE(upper.sendTimingReq(packet.get()));

    EventQueue *eventq = getEventQueue(0);
    while (!upper.response && !eventq->empty()) {
        eventq->serviceOne();
    }

    EXPECT_EQ(replacementPolicy.packetCalls, 1U);
    EXPECT_EQ(replacementPolicy.legacyCalls, 0U);
    EXPECT_TRUE(cache.inCache(TestAddr, false));
    ASSERT_NE(upper.response, nullptr);
    EXPECT_EQ(upper.response->cmd, MemCmd::ReadResp);
    EXPECT_FALSE(upper.response->hasSharers());
    EXPECT_FALSE(upper.response->cacheResponding());
}

TEST_F(CacheTimingTest, ReadSharedReqDirtyResponderFills)
{
    auto request = std::make_shared<Request>(
        TestAddr, CacheLineSize, Request::Flags(), Request::funcRequestorId);
    auto packet = std::make_unique<Packet>(request, MemCmd::ReadSharedReq,
                                           CacheLineSize);
    packet->allocate();

    ASSERT_TRUE(upper.sendTimingReq(packet.get()));

    EventQueue *eventq = getEventQueue(0);
    while (!upper.response && !eventq->empty()) {
        eventq->serviceOne();
    }

    EXPECT_EQ(lower.requestCmd, MemCmd::ReadSharedReq);
    EXPECT_EQ(replacementPolicy.packetCalls, 0U);
    EXPECT_EQ(replacementPolicy.legacyCalls, 1U);
    EXPECT_TRUE(cache.inCache(TestAddr, false));
    ASSERT_NE(upper.response, nullptr);
    EXPECT_EQ(upper.response->cmd, MemCmd::ReadResp);
    EXPECT_TRUE(upper.response->hasSharers());
    EXPECT_FALSE(upper.response->cacheResponding());
}

TEST_F(CacheTimingTest, ReadSharedReqPendingDowngradeFills)
{
    lower.responseCacheResponding = false;
    lower.responseHasSharers = false;

    auto request = std::make_shared<Request>(
        TestAddr, CacheLineSize, Request::Flags(), Request::funcRequestorId);
    auto packet = std::make_unique<Packet>(request, MemCmd::ReadSharedReq,
                                           CacheLineSize);
    packet->allocate();

    ASSERT_TRUE(upper.sendTimingReq(packet.get()));

    EventQueue *eventq = getEventQueue(0);
    while (lower.requestCmd == MemCmd::InvalidCmd && !eventq->empty()) {
        eventq->serviceOne();
    }
    ASSERT_EQ(lower.requestCmd, MemCmd::ReadSharedReq);
    ASSERT_EQ(upper.response, nullptr);

    auto snoop_request = std::make_shared<Request>(
        TestAddr, CacheLineSize, Request::Flags(), Request::funcRequestorId);
    auto snoop = std::make_unique<Packet>(snoop_request, MemCmd::ReadSharedReq,
                                          CacheLineSize);
    lower.sendTimingSnoopReq(snoop.get());
    ASSERT_TRUE(snoop->hasSharers());

    while (!upper.response && !eventq->empty()) {
        eventq->serviceOne();
    }

    EXPECT_EQ(replacementPolicy.packetCalls, 0U);
    EXPECT_EQ(replacementPolicy.legacyCalls, 1U);
    EXPECT_TRUE(cache.inCache(TestAddr, false));
    ASSERT_NE(upper.response, nullptr);
    EXPECT_EQ(upper.response->cmd, MemCmd::ReadResp);
    EXPECT_TRUE(upper.response->hasSharers());
    EXPECT_FALSE(upper.response->cacheResponding());
}

} // anonymous namespace

} // namespace gem5
