/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 */

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/cache/replacement_policies/mockingjay_l2_rp.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/MockingjayL2RP.hh"

using namespace gem5;
using namespace gem5::replacement_policy;

namespace
{

MockingjayL2RPParams
makeParams(const std::string &name = "mockingjay_test")
{
    MockingjayL2RPParams params;
    params.name = name;
    params.eventq_index = 0;
    params.num_sets = 4;
    params.num_ways = 2;
    params.block_bits = 6;
    params.slice_bits = 0;
    params.history_multiplier = 8;
    params.aging_granularity = 2;
    params.sampled_sets = 1;
    params.sampled_cache_sets_per_set = 1;
    params.sampled_cache_ways = 1;
    params.sampled_tag_bits = 10;
    params.rdp_entries = 64;
    params.temporal_difference_threshold = 1;
    params.scan_threshold_margin = 2;
    params.prefetch_penalty_percent = 200;
    params.timestamp_bits = 8;
    return params;
}

class MockingjayL2Test : public testing::Test
{
  protected:
    class TestableMockingjayL2 : public MockingjayL2
    {
      public:
        explicit TestableMockingjayL2(const MockingjayL2RPParams &params)
          : MockingjayL2(params)
        {
        }

        uint16_t
        prediction(PacketPtr pkt, bool hit) const
        {
            return rdpEntry(getSignature(pkt, hit)).reuseDistance;
        }

        bool
        hasPrediction(PacketPtr pkt, bool hit) const
        {
            return rdpEntry(getSignature(pkt, hit)).valid;
        }

        uint16_t
        predictionForSignature(uint32_t signature) const
        {
            return rdpEntry(signature).reuseDistance;
        }

        void
        setPrediction(PacketPtr pkt, bool hit, uint16_t reuse_distance)
        {
            RdpEntry &entry = rdpEntry(getSignature(pkt, hit));
            entry.valid = true;
            entry.reuseDistance = reuse_distance;
        }

        uint32_t
        signature(PacketPtr pkt, bool hit) const
        {
            return getSignature(pkt, hit);
        }

        static uint64_t
        referenceHash(uint64_t value)
        {
            return hash(value);
        }

        bool
        sampledSet(unsigned set_id) const
        {
            return isSampledSet(set_id);
        }

        uint64_t
        sampleTag(Addr addr) const
        {
            return sampledTag(addr);
        }

        std::size_t
        sampleIndex(unsigned set_id, Addr addr) const
        {
            return sampledCacheIndex(set_id, addr);
        }

        void
        seedSampledEntry(std::size_t index, unsigned way, uint64_t tag,
                         uint32_t signature, uint16_t timestamp)
        {
            SampledEntry &entry = sampledCache[index][way];
            entry.valid = true;
            entry.tag = tag;
            entry.signature = signature;
            entry.timestamp = timestamp;
        }

        void
        setSampledTimestamp(unsigned set_id, uint16_t timestamp)
        {
            sampledTimestamps[set_id] = timestamp;
        }

        uint16_t
        sampledTimestamp(unsigned set_id) const
        {
            return sampledTimestamps[set_id];
        }

        void
        sampleAccess(const std::shared_ptr<ReplacementData> &replacement_data,
                     PacketPtr pkt, bool hit)
        {
            auto data = std::static_pointer_cast<MockingjayReplData>(
                replacement_data);
            processSampledAccess(*data, pkt, getSignature(pkt, hit));
        }

        uint16_t
        infiniteDistance() const
        {
            return infRd;
        }

        int16_t
        infiniteEtr() const
        {
            return infEtr;
        }

        void
        setEtr(const std::shared_ptr<ReplacementData> &replacement_data,
               int16_t etr)
        {
            auto data = std::static_pointer_cast<MockingjayReplData>(
                replacement_data);
            data->valid = true;
            data->etr = etr;
        }

        int16_t
        etr(const std::shared_ptr<ReplacementData> &replacement_data) const
        {
            return std::static_pointer_cast<MockingjayReplData>(
                replacement_data)->etr;
        }

        void
        setClock(unsigned set_id, uint8_t clock)
        {
            setClocks[set_id] = clock;
        }

        Counter
        sampledMisses() const
        {
            return stats.sampledMisses.value();
        }

        Counter
        scanTrainings() const
        {
            return stats.scanTrainings.value();
        }

        Counter
        maxEtrInsertions() const
        {
            return stats.maxEtrInsertions.value();
        }

        Counter
        writebackInsertions() const
        {
            return stats.writebackInsertions.value();
        }

        Counter
        noPcSignatures() const
        {
            return stats.noPcSignatures.value();
        }
    };

    GTestTickHandler tickHandler;
    MockingjayL2RPParams params = makeParams();
    TestableMockingjayL2 policy{params};
    std::vector<ReplaceableEntry> entries{params.num_sets * params.num_ways};

    void
    SetUp() override
    {
        for (unsigned index = 0; index < entries.size(); ++index) {
            entries[index].setPosition(index / params.num_ways,
                                      index % params.num_ways);
            entries[index].replacementData = policy.instantiateEntry();
        }
    }

    ReplacementCandidates
    set(unsigned set_id)
    {
        ReplacementCandidates candidates;
        const unsigned start = set_id * params.num_ways;
        for (unsigned way = 0; way < params.num_ways; ++way) {
            candidates.push_back(&entries[start + way]);
        }
        return candidates;
    }

    std::unique_ptr<Packet>
    packet(Addr addr, Addr pc, MemCmd::Command cmd = MemCmd::ReadReq,
           bool has_pc = true, Request::Flags flags = Request::Flags())
    {
        auto req = std::make_shared<Request>(addr, 64, flags, 0);
        if (has_pc) {
            req->setPC(pc);
        }
        return std::make_unique<Packet>(req, cmd);
    }

    void
    fill(unsigned set_id, unsigned way, PacketPtr pkt)
    {
        policy.reset(entries[set_id * params.num_ways + way].replacementData,
                     pkt);
    }

    void
    access(unsigned set_id, unsigned way, PacketPtr pkt)
    {
        policy.touch(entries[set_id * params.num_ways + way].replacementData,
                     pkt);
    }
};

TEST_F(MockingjayL2Test, InvalidEntryIsAdmitted)
{
    const auto candidates = set(0);

    EXPECT_EQ(policy.getVictim(candidates), candidates.front());
}

TEST_F(MockingjayL2Test, ReferenceSampledAddressAndSignatureMapping)
{
    MockingjayL2RPParams mapping_params = makeParams("mockingjay_mapping");
    mapping_params.num_sets = 1024;
    mapping_params.sampled_sets = 8;
    mapping_params.sampled_cache_sets_per_set = 16;
    mapping_params.sampled_tag_bits = 12;
    mapping_params.rdp_entries = 512;
    mapping_params.slice_bits = 2;
    TestableMockingjayL2 mapping_policy{mapping_params};

    const std::vector<unsigned> expected_sampled_sets{
        0, 146, 292, 438, 585, 731, 877, 1023};
    std::vector<unsigned> sampled_sets;
    for (unsigned set_id = 0; set_id < mapping_params.num_sets; ++set_id) {
        if (mapping_policy.sampledSet(set_id)) {
            sampled_sets.push_back(set_id);
        }
    }
    EXPECT_EQ(sampled_sets, expected_sampled_sets);

    const uint64_t local_block_addr =
        (uint64_t(0xabc) << 14) | (uint64_t(5) << 10);
    const Addr addr = static_cast<Addr>(local_block_addr <<
        (mapping_params.slice_bits + mapping_params.block_bits));
    EXPECT_EQ(mapping_policy.sampleIndex(0, addr), 5U);
    EXPECT_EQ(mapping_policy.sampleTag(addr), 0xabcU);

    auto demand = packet(addr, 0x1234);
    const uint64_t signature_input = demand->req->getPC() << 2;
    EXPECT_EQ(mapping_policy.signature(demand.get(), false),
              mapping_policy.referenceHash(signature_input) &
                  (mapping_params.rdp_entries - 1));

    auto distinct_demand = packet(addr, 0x1110);
    EXPECT_NE(mapping_policy.signature(demand.get(), false),
              mapping_policy.signature(distinct_demand.get(), false));
}

TEST_F(MockingjayL2Test, SampledReuseTrainsThenPredictsIncomingFill)
{
    auto first = packet(0, 0x1000);

    fill(0, 0, first.get());
    access(0, 0, first.get());

    // The initial miss signature is trained with a one-access per-set reuse
    // distance even though the later access uses a distinct hit signature.
    EXPECT_TRUE(policy.hasPrediction(first.get(), false));
    EXPECT_EQ(policy.prediction(first.get(), false), 1);
}

TEST_F(MockingjayL2Test, ScanPredictionUsesMaxEtrInsertion)
{
    auto resident0 = packet(0, 0x1110);
    auto resident1 = packet(0x100, 0x2220);

    fill(0, 0, resident0.get());
    fill(0, 1, resident1.get());

    // The one-entry sampled history evicts resident0's miss signature as a
    // scan. Its next fill is admitted normally with the maximum positive ETR.
    ASSERT_TRUE(policy.hasPrediction(resident0.get(), false));
    EXPECT_EQ(policy.prediction(resident0.get(), false),
              policy.infiniteDistance());
    const Counter misses_before = policy.sampledMisses();
    const auto candidates = set(0);
    ReplaceableEntry *victim = policy.getVictim(candidates);
    ASSERT_NE(victim, nullptr);
    policy.reset(victim->replacementData, resident0.get());
    EXPECT_EQ(policy.sampledMisses(), misses_before + 1);
    EXPECT_EQ(policy.maxEtrInsertions(), 1);
    EXPECT_EQ(policy.etr(victim->replacementData), policy.infiniteEtr());

    const unsigned victim_way = victim == &entries[0] ? 0 : 1;
    const unsigned other_way = victim_way == 0 ? 1 : 0;
    ReplaceableEntry *other = &entries[other_way];
    policy.setEtr(other->replacementData, 1);
    EXPECT_EQ(policy.getVictim(candidates), victim);

    policy.setClock(0, params.aging_granularity);
    access(0, other_way, resident1.get());
    EXPECT_EQ(policy.etr(victim->replacementData), policy.infiniteEtr());

    // A negative ETR keeps the documented signed tie-break: a writeback-like
    // -INF_ETR line is selected before an equally distant scan insertion.
    policy.setEtr(other->replacementData, -policy.infiniteEtr());
    EXPECT_EQ(policy.getVictim(candidates), other);
}

TEST_F(MockingjayL2Test, FartherPredictionUsesMaxEtrInsertion)
{
    auto incoming = packet(0x200, 0x3333);
    policy.setEtr(entries[0].replacementData, 2);
    policy.setEtr(entries[1].replacementData, 1);
    policy.setPrediction(incoming.get(), false, 6);

    const Counter insertions_before = policy.maxEtrInsertions();
    ReplaceableEntry *victim = policy.getVictim(set(0));
    ASSERT_EQ(victim, &entries[0]);
    policy.invalidate(victim->replacementData);
    policy.reset(victim->replacementData, incoming.get());

    EXPECT_EQ(policy.etr(victim->replacementData), policy.infiniteEtr());
    EXPECT_EQ(policy.maxEtrInsertions(), insertions_before + 1);
}

TEST_F(MockingjayL2Test, HardwarePrefetchUsesMaxEtrInsertion)
{
    auto incoming = packet(0x200, 0x3333, MemCmd::HardPFReq);
    policy.setEtr(entries[0].replacementData, 2);
    policy.setEtr(entries[1].replacementData, 1);
    policy.setPrediction(incoming.get(), false, 6);

    ReplaceableEntry *victim = policy.getVictim(set(0));
    ASSERT_EQ(victim, &entries[0]);
    policy.invalidate(victim->replacementData);
    policy.reset(victim->replacementData, incoming.get());

    EXPECT_EQ(policy.etr(victim->replacementData), policy.infiniteEtr());
    EXPECT_EQ(policy.maxEtrInsertions(), 1);
}

TEST_F(MockingjayL2Test, MaxEtrDecisionUsesPreTrainingPrediction)
{
    auto incoming = packet(0, 0x3333);
    policy.setPrediction(incoming.get(), false,
                         policy.infiniteDistance() - 1);
    const uint32_t signature = policy.signature(incoming.get(), false);
    policy.setSampledTimestamp(0, 1);
    policy.seedSampledEntry(policy.sampleIndex(0, incoming->getAddr()), 0,
                            policy.sampleTag(incoming->getAddr()),
                            signature, 0);

    policy.setEtr(entries[0].replacementData, policy.infiniteEtr());
    policy.setEtr(entries[1].replacementData, 0);
    ReplaceableEntry *victim = policy.getVictim(set(0));
    ASSERT_EQ(victim, &entries[0]);
    policy.invalidate(victim->replacementData);
    policy.reset(victim->replacementData, incoming.get());

    // The sampled hit trains 14 down to 13. The insertion decision must
    // retain the pre-training scan prediction, just as a real bypass would.
    EXPECT_EQ(policy.prediction(incoming.get(), false),
              policy.infiniteDistance() - 2);
    EXPECT_EQ(policy.etr(victim->replacementData), policy.infiniteEtr());
}

TEST_F(MockingjayL2Test, FiniteQuantizedMaxEtrIsNotAScanInsertion)
{
    MockingjayL2RPParams quantized_params =
        makeParams("mockingjay_quantized_max_etr");
    quantized_params.scan_threshold_margin = 1;
    TestableMockingjayL2 quantized_policy{quantized_params};
    std::vector<ReplaceableEntry> quantized_entries{
        quantized_params.num_sets * quantized_params.num_ways};
    for (unsigned index = 0; index < quantized_entries.size(); ++index) {
        quantized_entries[index].setPosition(
            index / quantized_params.num_ways,
            index % quantized_params.num_ways);
        quantized_entries[index].replacementData =
            quantized_policy.instantiateEntry();
    }

    auto incoming = packet(0, 0x4444);
    quantized_policy.setPrediction(
        incoming.get(), false, quantized_policy.infiniteDistance() - 1);
    quantized_policy.setEtr(quantized_entries[0].replacementData,
                            quantized_policy.infiniteEtr());
    quantized_policy.setEtr(quantized_entries[1].replacementData, 0);

    ReplacementCandidates candidates{
        &quantized_entries[0], &quantized_entries[1]};
    ReplaceableEntry *victim = quantized_policy.getVictim(candidates);
    ASSERT_EQ(victim, &quantized_entries[0]);
    quantized_policy.invalidate(victim->replacementData);
    quantized_policy.reset(victim->replacementData, incoming.get());

    EXPECT_EQ(quantized_policy.etr(victim->replacementData),
              quantized_policy.infiniteEtr());
    EXPECT_EQ(quantized_policy.maxEtrInsertions(), 0);
}

TEST_F(MockingjayL2Test, EveryStaleSampledEntryIsDetrained)
{
    MockingjayL2RPParams stale_params = makeParams("mockingjay_stale");
    stale_params.sampled_cache_ways = 2;
    TestableMockingjayL2 stale_policy{stale_params};
    const auto replacement_data = stale_policy.instantiateEntry();

    stale_policy.seedSampledEntry(0, 0, 1, 3, 0);
    stale_policy.seedSampledEntry(0, 1, 2, 4, 0);
    stale_policy.setSampledTimestamp(0, stale_policy.infiniteDistance() + 1);
    auto incoming = packet(0, 0x3333);

    const Counter scans_before = stale_policy.scanTrainings();
    stale_policy.sampleAccess(replacement_data, incoming.get(), false);

    EXPECT_EQ(stale_policy.scanTrainings(), scans_before + 2);
    EXPECT_EQ(stale_policy.predictionForSignature(3),
              stale_policy.infiniteDistance());
    EXPECT_EQ(stale_policy.predictionForSignature(4),
              stale_policy.infiniteDistance());
}

TEST_F(MockingjayL2Test, WritebacksRemainResidentDespiteScanPrediction)
{
    auto resident0 = packet(0, 0x1110);
    auto resident1 = packet(0x100, 0x2220);
    auto writeback = packet(0x200, 0x1110, MemCmd::WritebackDirty);

    fill(0, 0, resident0.get());
    fill(0, 1, resident1.get());

    ASSERT_TRUE(policy.hasPrediction(resident0.get(), false));
    ASSERT_EQ(policy.prediction(resident0.get(), false),
              policy.infiniteDistance());

    const Counter insertions_before = policy.writebackInsertions();
    fill(0, 0, writeback.get());
    EXPECT_EQ(policy.etr(entries[0].replacementData), -policy.infiniteEtr());
    EXPECT_EQ(policy.writebackInsertions(), insertions_before + 1);

    const uint16_t timestamp_before = policy.sampledTimestamp(0);
    access(0, 0, writeback.get());
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[0].replacementData), -policy.infiniteEtr());

    EXPECT_NE(policy.getVictim(set(0)), nullptr);
}

TEST_F(MockingjayL2Test, EvictionsAndMaintenanceDoNotTrain)
{
    auto demand = packet(0, 0x1000);
    fill(0, 0, demand.get());
    policy.setEtr(entries[0].replacementData, 3);

    const Counter no_pc_before = policy.noPcSignatures();
    const uint16_t timestamp_before = policy.sampledTimestamp(0);
    auto clean_evict = packet(0, 0, MemCmd::CleanEvict, false);
    access(0, 0, clean_evict.get());
    EXPECT_EQ(policy.noPcSignatures(), no_pc_before);
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[0].replacementData), 3);

    auto maintenance = packet(0, 0, MemCmd::ReadReq, false,
                              Request::Flags(Request::CLEAN));
    access(0, 0, maintenance.get());
    EXPECT_EQ(policy.noPcSignatures(), no_pc_before);
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[0].replacementData), 3);

    auto write_clean = packet(0, 0, MemCmd::WriteClean, false);
    access(0, 0, write_clean.get());
    EXPECT_EQ(policy.noPcSignatures(), no_pc_before);
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[0].replacementData), 3);
}

TEST_F(MockingjayL2Test, NonDemandFillsDoNotTrain)
{
    const Counter no_pc_before = policy.noPcSignatures();
    const uint16_t timestamp_before = policy.sampledTimestamp(0);
    auto write_clean = packet(0, 0, MemCmd::WriteClean, false);

    fill(0, 0, write_clean.get());

    EXPECT_EQ(policy.noPcSignatures(), no_pc_before);
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[0].replacementData), 0);

    auto maintenance = packet(0x40, 0, MemCmd::ReadReq, false,
                              Request::Flags(Request::CLEAN));
    fill(0, 1, maintenance.get());
    EXPECT_EQ(policy.noPcSignatures(), no_pc_before);
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[1].replacementData), 0);
}

TEST_F(MockingjayL2Test, SoftwarePrefetchesDoNotTrain)
{
    auto demand = packet(0, 0x1000);
    fill(0, 0, demand.get());
    policy.setEtr(entries[0].replacementData, 3);

    const Counter no_pc_before = policy.noPcSignatures();
    const uint16_t timestamp_before = policy.sampledTimestamp(0);
    auto software_prefetch = packet(0x40, 0, MemCmd::SoftPFReq, false);

    access(0, 0, software_prefetch.get());
    EXPECT_EQ(policy.noPcSignatures(), no_pc_before);
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[0].replacementData), 3);

    fill(0, 1, software_prefetch.get());
    EXPECT_EQ(policy.noPcSignatures(), no_pc_before);
    EXPECT_EQ(policy.sampledTimestamp(0), timestamp_before);
    EXPECT_EQ(policy.etr(entries[1].replacementData), 0);
}

TEST_F(MockingjayL2Test, AgingAndTrainingRemainPerSet)
{
    auto set0_a = packet(0, 0x1000);
    auto set0_b = packet(0x100, 0x2000);
    auto set1_a = packet(0x40, 0x3000);
    auto set1_b = packet(0x140, 0x4000);

    fill(0, 0, set0_a.get());
    fill(0, 1, set0_b.get());
    fill(1, 0, set1_a.get());
    fill(1, 1, set1_b.get());

    policy.setEtr(entries[1].replacementData, 3);
    policy.setEtr(entries[params.num_ways].replacementData, 3);
    policy.setClock(0, params.aging_granularity);
    access(0, 0, set0_a.get());

    EXPECT_EQ(policy.etr(entries[1].replacementData), 2);
    EXPECT_EQ(policy.etr(entries[params.num_ways].replacementData), 3);
}

TEST_F(MockingjayL2Test, NegativeEtrWinsAnAbsoluteTie)
{
    policy.setEtr(entries[0].replacementData, 2);
    policy.setEtr(entries[1].replacementData, -2);

    EXPECT_EQ(policy.getVictim(set(0)), &entries[1]);
}

TEST_F(MockingjayL2Test, MissingPcUsesTheReservedPredictionBucket)
{
    auto no_pc = packet(0, 0, MemCmd::ReadReq, false);

    fill(0, 0, no_pc.get());
    access(0, 0, no_pc.get());

    EXPECT_NE(policy.getVictim(set(0)), nullptr);
    EXPECT_EQ(policy.noPcSignatures(), 2);
}

TEST_F(MockingjayL2Test, RejectsUnsafeAddressAndTimestampGeometry)
{
    auto invalid_block_bits = makeParams("mockingjay_invalid_block_bits");
    invalid_block_bits.block_bits = std::numeric_limits<Addr>::digits;
    EXPECT_ANY_THROW({ TestableMockingjayL2 invalid{invalid_block_bits}; });

    auto invalid_slice_bits = makeParams("mockingjay_invalid_slice_bits");
    invalid_slice_bits.slice_bits = std::numeric_limits<Addr>::digits;
    EXPECT_ANY_THROW({ TestableMockingjayL2 invalid{invalid_slice_bits}; });

    auto invalid_timestamp = makeParams("mockingjay_invalid_timestamp");
    invalid_timestamp.num_ways = 8;
    invalid_timestamp.history_multiplier = 64;
    invalid_timestamp.timestamp_bits = 8;
    EXPECT_ANY_THROW({ TestableMockingjayL2 invalid{invalid_timestamp}; });

    auto invalid_sampled_cache = makeParams("mockingjay_invalid_sampled");
    const unsigned large_power_of_two =
        std::numeric_limits<unsigned>::max() / 2 + 1;
    invalid_sampled_cache.num_sets = large_power_of_two;
    invalid_sampled_cache.num_ways = 1;
    invalid_sampled_cache.block_bits = 0;
    invalid_sampled_cache.sampled_sets = large_power_of_two;
    invalid_sampled_cache.sampled_cache_sets_per_set = large_power_of_two;
    invalid_sampled_cache.history_multiplier = 32;
    EXPECT_ANY_THROW({
        TestableMockingjayL2 invalid{invalid_sampled_cache};
    });
}

} // anonymous namespace
