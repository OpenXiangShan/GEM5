/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "mem/cache/replacement_policies/lru_rp.hh"
#include "mem/cache/replacement_policies/sdbp_rp.hh"
#include "params/LRURP.hh"
#include "params/SDBPRP.hh"
#include "sim/eventq.hh"

namespace gem5
{
namespace replacement_policy
{
namespace
{

SDBPRPParams
testParams(bool bypass = false, unsigned threshold = 8)
{
    SDBPRPParams p;
    p.name = "sdbp_test";
    p.eventq_index = 0;
    p.num_sets = 64;
    p.sampler_num = 8;
    p.sampler_assoc = 2;
    p.sampler_protect_youngest = 0;
    p.sampler_dead_first = true;
    p.sampler_demand_pc_only = false;
    p.l1_copy_protection = true;
    p.predictor_tables = 3;
    p.predictor_entries = 4096;
    p.counter_bits = 2;
    p.dead_threshold = threshold;
    p.partial_tag_bits = 15;
    p.partial_pc_bits = 15;
    p.pc_shift = 1;
    p.pc_hash_seed = 0;
    p.pc_hash_type = "xor_fold";
    p.index_hash_type = "mixed";
    p.crc_live_update = true;
    p.enable_bypass = bypass;
    p.observation_only = false;
    return p;
}

std::unique_ptr<Packet>
packet(MemCmd cmd = MemCmd::ReadReq, bool pc = true)
{
    auto req = std::make_shared<Request>(0x1000, 64, 0, 0);
    if (pc)
        req->setPC(0x100);
    return std::make_unique<Packet>(req, cmd);
}

double
stat(const SDBP &policy, const std::string &name)
{
    const auto *info = dynamic_cast<const statistics::ScalarInfo *>(policy.resolveStat(name));
    EXPECT_NE(info, nullptr);
    return info ? info->value() : -1;
}

double
sourceStat(const SDBP &policy, const std::string &name, unsigned source)
{
    const auto *info = dynamic_cast<const statistics::VectorInfo *>(
        policy.resolveStat(name));
    EXPECT_NE(info, nullptr);
    return info ? info->result().at(source) : -1;
}

std::unique_ptr<Packet>
sourcedPacket(Request::L2RequestSource source, Addr pc)
{
    auto pkt = packet(MemCmd::ReadSharedReq);
    pkt->req->setL2RequestSource(source);
    pkt->req->setPC(pc);
    if (source != Request::L2RequestSource::L1Demand) {
        pkt->req->setFlags(Request::PREFETCH);
        pkt->req->setPrefetchTriggerPC(pc);
    }
    return pkt;
}

class SDBPInterface : public ::testing::Test
{
  protected:
    void SetUp() override { curEventQueue(getEventQueue(0)); }
};

SDBPRPParams
observerParams(unsigned threshold = 8)
{
    auto params = testParams(false, threshold);
    params.observation_only = true;
    params.l1_copy_protection = false;
    return params;
}

TEST_F(SDBPInterface, ResetDoesNotTrainAndResponsePcPredicts)
{
    auto params = testParams();
    SDBP policy(params);
    auto demand = packet();
    for (unsigned tag = 0; tag < 8; ++tag)
        policy.observeFill(demand.get(), 0, tag);
    EXPECT_EQ(stat(policy, "deadTraining"), 6);
    auto data = policy.instantiateEntry();
    auto response = packet(MemCmd::ReadResp);
    policy.reset(data, response.get());
    EXPECT_EQ(stat(policy, "deadTraining"), 6);
    EXPECT_EQ(stat(policy, "samplerAccesses"), 8);
    EXPECT_EQ(stat(policy, "deadPredictions"), 1);
    policy.touch(data, demand.get());
    EXPECT_EQ(stat(policy, "deadHits"), 1);
}

TEST_F(SDBPInterface, YoungestTwoParameterReachesSampler)
{
    auto params = testParams(false, 0);
    params.sampler_assoc = 6;
    params.sampler_protect_youngest = 2;
    SDBP policy(params);
    auto demand = packet();
    for (unsigned tag = 1; tag <= 6; ++tag)
        policy.observeFill(demand.get(), 0, tag);
    for (unsigned tag : {3U, 1U, 2U})
        policy.observe(demand.get(), 0, tag, true);
    policy.observeFill(demand.get(), 0, 7);
    policy.observe(demand.get(), 0, 1, true);
    policy.observe(demand.get(), 0, 2, true);
    EXPECT_EQ(stat(policy, "samplerHits"), 5);
}

TEST_F(SDBPInterface, FiltersSoftwarePrefetchWritebackAndMissingPc)
{
    SDBP policy(testParams(true, 0));
    auto data = policy.instantiateEntry();
    for (auto cmd : {MemCmd::SoftPFReq, MemCmd::WritebackDirty,
                     MemCmd::WritebackClean, MemCmd::CleanEvict,
                     MemCmd::WriteClean}) {
        auto excluded = packet(cmd);
        policy.observe(excluded.get(), 0, 1, true);
        policy.reset(data, excluded.get());
    }
    auto missing = packet(MemCmd::ReadReq, false);
    policy.observeFill(missing.get(), 0, 2);
    policy.reset(data, missing.get());
    EXPECT_EQ(stat(policy, "samplerAccesses"), 0);
    EXPECT_EQ(stat(policy, "predictionQueries"), 0);
    EXPECT_EQ(stat(policy, "excludedAccesses"), 5);
    EXPECT_EQ(stat(policy, "noPcAccesses"), 1);
}

TEST_F(SDBPInterface, TrainsAndPredictsForwardedPrefetchMisses)
{
    SDBP policy(testParams(true, 0));
    auto data = policy.instantiateEntry();
    unsigned tag = 0;
    for (auto flag : {Request::PREFETCH, Request::PF_EXCLUSIVE}) {
        for (auto cmd : {MemCmd::ReadReq, MemCmd::ReadSharedReq, MemCmd::ReadCleanReq,
                         MemCmd::ReadExReq, MemCmd::ReadResp}) {
            auto forwarded = packet(cmd);
            forwarded->req->setFlags(flag);
            policy.observeFill(forwarded.get(), 0, ++tag);
            policy.reset(data, forwarded.get());
        }
    }
    EXPECT_EQ(stat(policy, "samplerAccesses"), 10);
    EXPECT_EQ(stat(policy, "deadTraining"), 8);
    EXPECT_EQ(stat(policy, "predictionQueries"), 10);
    EXPECT_EQ(stat(policy, "deadPredictions"), 10);
    EXPECT_EQ(stat(policy, "excludedAccesses"), 0);
}

TEST_F(SDBPInterface, DemandPcOnlyFiltersHitAndFillEvents)
{
    auto params = testParams();
    params.sampler_demand_pc_only = true;
    params.sampler_assoc = 1;
    SDBP policy(params);
    auto demand = packet();
    policy.observeFill(demand.get(), 0, 1);
    for (auto cmd : {MemCmd::ReadReq, MemCmd::WriteReq,
                     MemCmd::ReadResp, MemCmd::WriteResp}) {
        auto missing = packet(cmd, false);
        policy.observe(missing.get(), 0, 1, true);
        policy.observeFill(missing.get(), 0, 2);
    }
    for (auto flag : {Request::PREFETCH, Request::PF_EXCLUSIVE}) {
        for (auto cmd : {MemCmd::HardPFReq, MemCmd::SoftPFReq,
                         MemCmd::HardPFResp, MemCmd::ReadReq,
                         MemCmd::ReadSharedReq, MemCmd::ReadCleanReq,
                         MemCmd::ReadExReq, MemCmd::ReadResp}) {
            for (bool hasPc : {false, true}) {
                auto prefetch = packet(cmd, hasPc);
                prefetch->req->setFlags(flag);
                policy.observe(prefetch.get(), 0, 1, true);
                policy.observeFill(prefetch.get(), 0, 2);
            }
        }
    }
    EXPECT_EQ(stat(policy, "samplerAccesses"), 1);
    EXPECT_EQ(stat(policy, "samplerHits"), 0);
    EXPECT_EQ(stat(policy, "samplerEvictions"), 0);
    EXPECT_EQ(stat(policy, "liveTraining"), 0);
    EXPECT_EQ(stat(policy, "deadTraining"), 0);
    EXPECT_EQ(stat(policy, "noPcAccesses"), 36);

    policy.observe(demand.get(), 0, 1, true);
    policy.observeFill(demand.get(), 0, 2);
    EXPECT_EQ(stat(policy, "samplerAccesses"), 3);
    EXPECT_EQ(stat(policy, "samplerHits"), 1);
    EXPECT_EQ(stat(policy, "samplerEvictions"), 1);
    EXPECT_EQ(stat(policy, "liveTraining"), 1);
    EXPECT_EQ(stat(policy, "deadTraining"), 1);
}

TEST_F(SDBPInterface, DemandPcOnlyAcceptsResponsesAndMergedDemandsWithPc)
{
    auto params = testParams();
    params.sampler_demand_pc_only = true;
    params.sampler_assoc = 1;
    SDBP policy(params);
    auto response = packet(MemCmd::ReadResp);
    response->req->setPC(0);
    policy.observeFill(response.get(), 0, 1);
    auto merged = packet(MemCmd::HardPFResp);
    merged->req->setFlags(Request::PREFETCH);
    merged->setSamplerDemand();
    policy.observeFill(merged.get(), 0, 1);
    policy.observeFill(merged.get(), 0, 2);
    auto missing = packet(MemCmd::HardPFResp, false);
    missing->req->setFlags(Request::PREFETCH);
    missing->setSamplerDemand();
    policy.observeFill(missing.get(), 0, 3);
    auto writeResponse = packet(MemCmd::WriteResp);
    policy.observeFill(writeResponse.get(), 0, 3);
    EXPECT_EQ(stat(policy, "samplerAccesses"), 4);
    EXPECT_EQ(stat(policy, "samplerHits"), 1);
    EXPECT_EQ(stat(policy, "samplerEvictions"), 2);
    EXPECT_EQ(stat(policy, "liveTraining"), 1);
    EXPECT_EQ(stat(policy, "deadTraining"), 2);
    EXPECT_EQ(stat(policy, "noPcAccesses"), 1);
}

TEST_F(SDBPInterface, InvalidThenDeadThenLruAndPacketlessFallback)
{
    auto params = testParams(false, 0);
    SDBP policy(params);
    ReplaceableEntry a, b;
    a.replacementData = policy.instantiateEntry();
    b.replacementData = policy.instantiateEntry();
    ReplacementCandidates candidates{&a, &b};
    auto demand = packet();
    policy.reset(a.replacementData, demand.get());
    EXPECT_EQ(policy.getVictim(candidates), &b);
    policy.reset(b.replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &a);
    // Clear a's dead bit and make it MRU. Same tick ordering is preserved.
    policy.touch(a.replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &b);
    policy.invalidate(a.replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &a);
    EXPECT_EQ(stat(policy, "deadTraining"), 0);
}

TEST_F(SDBPInterface, DeadVictimCanOverrideLru)
{
    auto params = testParams(false, 0);
    SDBP policy(params);
    ReplaceableEntry a, b;
    a.replacementData = policy.instantiateEntry();
    b.replacementData = policy.instantiateEntry();
    auto demand = packet();
    policy.reset(b.replacementData);
    policy.reset(a.replacementData, demand.get());
    EXPECT_EQ(policy.getVictim(ReplacementCandidates{&a, &b}), &a);
    EXPECT_EQ(stat(policy, "deadVictims"), 1);
    EXPECT_EQ(stat(policy, "nonLruDeadVictims"), 1);
    EXPECT_EQ(stat(policy, "lruVictims"), 0);
    EXPECT_EQ(SDBP::victimClass(a.replacementData), 2);
    EXPECT_EQ(SDBP::victimClass(b.replacementData), 1);
    // The diagnostic snapshot must precede invalidation.
    const unsigned snapshot = SDBP::victimClass(a.replacementData);
    policy.invalidate(a.replacementData);
    EXPECT_EQ(snapshot, 2);
    EXPECT_EQ(SDBP::victimClass(a.replacementData), 1);

    demand->replacementVictimClass = snapshot + 1;
    demand->replacementProbe = true;
    demand->replacementProbeMask = 7;
    Packet snoop(demand.get(), true, false);
    EXPECT_EQ(snoop.replacementVictimClass, 0);
    EXPECT_FALSE(snoop.replacementProbe);
    EXPECT_EQ(snoop.replacementProbeMask, 0);
}

TEST_F(SDBPInterface, DisabledL1GuardRetainsL2DeadFirst)
{
    auto params = testParams(false, 0);
    params.l1_copy_protection = false;
    params.sampler_dead_first = false;
    params.sampler_protect_youngest = 2;
    SDBP policy(params);
    ReplaceableEntry lru, dead;
    lru.replacementData = policy.instantiateEntry();
    dead.replacementData = policy.instantiateEntry();
    policy.reset(lru.replacementData);
    auto demand = packet();
    policy.reset(dead.replacementData, demand.get());
    unsigned queries = 0;
    policy.setL1CopyProbe([&](const ReplaceableEntry *) {
        ++queries;
        return true;
    });
    // Sampler policy and disabled L1 protection cannot disable L2 dead-first.
    EXPECT_EQ(policy.getVictim(ReplacementCandidates{&lru, &dead}), &dead);
    EXPECT_EQ(queries, 0);
    EXPECT_EQ(stat(policy, "deadVictims"), 1);
    EXPECT_EQ(stat(policy, "l1CopyChecks"), 0);
}

TEST_F(SDBPInterface, PureLruParameterReachesSampler)
{
    auto params = testParams(false, 0);
    params.sampler_assoc = 3;
    params.sampler_dead_first = false;
    SDBP policy(params);
    auto demand = packet();
    for (unsigned tag = 1; tag <= 3; ++tag)
        policy.observeFill(demand.get(), 0, tag);
    policy.observe(demand.get(), 0, 1, true);
    policy.observeFill(demand.get(), 0, 4);
    // Dead-first would evict way 0 (tag 1); pure LRU evicts tag 2.
    policy.observe(demand.get(), 0, 1, true);
    EXPECT_EQ(stat(policy, "samplerHits"), 2);
}

TEST_F(SDBPInterface, L1CopyGuardSkipsProtectedDeadAndAllowsUnprotectedMru)
{
    SDBP policy(testParams(false, 0));
    std::array<ReplaceableEntry, 4> entries;
    ReplacementCandidates candidates;
    auto demand = packet();
    for (auto &entry : entries) {
        entry.replacementData = policy.instantiateEntry();
        candidates.push_back(&entry);
        policy.reset(entry.replacementData, demand.get());
    }
    std::vector<const ReplaceableEntry *> checked;
    policy.setL1CopyProbe([&](const ReplaceableEntry *entry) {
        checked.push_back(entry);
        return entry != &entries[3];
    });
    // The MRU is eligible because only L1 presence determines protection.
    EXPECT_EQ(policy.getVictim(candidates), &entries[3]);
    EXPECT_EQ(checked.size(), 4);
    EXPECT_EQ(stat(policy, "l1CopyProtectedDead"), 3);
    EXPECT_EQ(stat(policy, "deadVictims"), 1);
    EXPECT_EQ(stat(policy, "l1CopyLruFallbacks"), 0);
    for (auto &entry : entries)
        EXPECT_EQ(SDBP::victimClass(entry.replacementData), 2);
}

TEST_F(SDBPInterface, AllDeadProtectedFallsBackToLruIncludingProtectedLru)
{
    SDBP policy(testParams(false, 0));
    std::array<ReplaceableEntry, 3> entries;
    auto demand = packet();
    for (auto &entry : entries) {
        entry.replacementData = policy.instantiateEntry();
        policy.reset(entry.replacementData, demand.get());
    }
    policy.setL1CopyProbe([](const ReplaceableEntry *) { return true; });
    // Candidate order differs from recency; the fallback is still true LRU.
    ReplacementCandidates candidates{&entries[2], &entries[0], &entries[1]};
    EXPECT_EQ(policy.getVictim(candidates), &entries[0]);
    EXPECT_EQ(stat(policy, "l1CopyChecks"), 3);
    EXPECT_EQ(stat(policy, "l1CopyProtectedDead"), 3);
    EXPECT_EQ(stat(policy, "l1CopyLruFallbacks"), 1);
    EXPECT_EQ(stat(policy, "lruVictims"), 1);
    EXPECT_EQ(stat(policy, "deadVictims"), 0);
}

TEST_F(SDBPInterface, L1CopyGuardKeepsFirstEligibleDeadOrder)
{
    SDBP policy(testParams(false, 0));
    std::array<ReplaceableEntry, 4> entries;
    auto demand = packet();
    for (unsigned i = 0; i < entries.size(); ++i) {
        entries[i].replacementData = policy.instantiateEntry();
        if (i == 0)
            policy.reset(entries[i].replacementData);
        else
            policy.reset(entries[i].replacementData, demand.get());
    }
    std::vector<const ReplaceableEntry *> checked;
    policy.setL1CopyProbe([&](const ReplaceableEntry *entry) {
        checked.push_back(entry);
        return entry == &entries[1];
    });
    ReplacementCandidates candidates{&entries[1], &entries[0],
                                     &entries[3], &entries[2]};
    EXPECT_EQ(policy.getVictim(candidates), &entries[3]);
    ASSERT_EQ(checked.size(), 2);
    EXPECT_EQ(checked[0], &entries[1]);
    EXPECT_EQ(checked[1], &entries[3]);
    EXPECT_EQ(stat(policy, "nonLruDeadVictims"), 1);
}

TEST_F(SDBPInterface, L1CopyGuardUsesCurrentPresenceAndPreservesDeadBit)
{
    SDBP policy(testParams(false, 0));
    ReplaceableEntry lru, dead;
    lru.replacementData = policy.instantiateEntry();
    dead.replacementData = policy.instantiateEntry();
    policy.reset(lru.replacementData);
    auto demand = packet();
    policy.reset(dead.replacementData, demand.get());
    bool present = true;
    policy.setL1CopyProbe([&](const ReplaceableEntry *) { return present; });
    ReplacementCandidates candidates{&dead, &lru};
    EXPECT_EQ(policy.getVictim(candidates), &lru);
    EXPECT_EQ(SDBP::victimClass(dead.replacementData), 2);
    present = false;
    EXPECT_EQ(policy.getVictim(candidates), &dead);
    EXPECT_EQ(SDBP::victimClass(dead.replacementData), 2);
}

TEST_F(SDBPInterface, L1CopyGuardDoesNotProbeInvalidOrBypassedSets)
{
    SDBP policy(testParams(true, 0));
    ReplaceableEntry a, b;
    a.replacementData = policy.instantiateEntry();
    b.replacementData = policy.instantiateEntry();
    unsigned queries = 0;
    policy.setL1CopyProbe([&](const ReplaceableEntry *) {
        ++queries;
        return true;
    });
    auto demand = packet();
    policy.reset(a.replacementData, demand.get());
    ReplacementCandidates candidates{&a, &b};
    EXPECT_EQ(policy.getVictim(candidates, demand.get()), &b);
    EXPECT_EQ(queries, 0);
    policy.reset(b.replacementData);
    auto response = packet(MemCmd::ReadExResp);
    EXPECT_EQ(policy.getVictim(candidates, response.get()), nullptr);
    EXPECT_TRUE(response->bypassL2);
    EXPECT_EQ(queries, 0);
    policy.touch(a.replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &b);
    EXPECT_EQ(queries, 0);
    EXPECT_EQ(stat(policy, "l1CopyLruFallbacks"), 0);
}

TEST_F(SDBPInterface, BypassPreservesInvalidWaysAndOrdinaryReadRequests)
{
    SDBP policy(testParams(true, 0));
    ReplaceableEntry a, b;
    a.replacementData = policy.instantiateEntry();
    b.replacementData = policy.instantiateEntry();
    const ReplacementCandidates candidates{&a, &b};
    auto first = packet(MemCmd::ReadResp);
    EXPECT_EQ(policy.getVictim(candidates, first.get()), &a);
    EXPECT_FALSE(first->bypassL2);
    policy.reset(a.replacementData);
    EXPECT_EQ(policy.getVictim(candidates, first.get()), &b);
    policy.reset(b.replacementData);
    auto full = packet(MemCmd::ReadResp);
    EXPECT_EQ(policy.getVictim(candidates, full.get()), nullptr);
    EXPECT_TRUE(full->bypassL2);
    policy.observeFill(full.get(), 0, 1);
    EXPECT_EQ(stat(policy, "samplerBypassAccesses"), 1);
    auto missing = packet(MemCmd::ReadResp, false);
    EXPECT_NE(policy.getVictim(candidates, missing.get()), nullptr);
    EXPECT_FALSE(missing->bypassL2);
    auto prefetch = packet(MemCmd::HardPFResp);
    EXPECT_NE(policy.getVictim(candidates, prefetch.get()), nullptr);
    EXPECT_FALSE(prefetch->bypassL2);
    for (auto cmd : {MemCmd::ReadReq, MemCmd::UpgradeResp,
                    MemCmd::InvalidateResp, MemCmd::WritebackDirty,
                    MemCmd::WritebackClean, MemCmd::WriteClean,
                    MemCmd::CleanEvict}) {
        auto other = packet(cmd);
        EXPECT_NE(policy.getVictim(candidates, other.get()), nullptr);
        EXPECT_FALSE(other->bypassL2);
    }
    EXPECT_NE(policy.getVictim(candidates), nullptr);
    EXPECT_EQ(stat(policy, "bypasses"), 1);
}

TEST_F(SDBPInterface, DisabledBypassStillAllocatesDeadRefills)
{
    SDBP policy(testParams(false, 0));
    ReplaceableEntry entry;
    entry.replacementData = policy.instantiateEntry();
    policy.reset(entry.replacementData);
    auto response = packet(MemCmd::ReadResp);
    EXPECT_EQ(policy.getVictim({&entry}, response.get()), &entry);
    EXPECT_FALSE(response->bypassL2);
    EXPECT_EQ(stat(policy, "bypasses"), 0);
}

TEST_F(SDBPInterface, BypassExcludesInstructionFetches)
{
    SDBP policy(testParams(true, 0));
    ReplaceableEntry entry;
    entry.replacementData = policy.instantiateEntry();
    policy.reset(entry.replacementData);
    auto instruction = packet(MemCmd::ReadResp);
    instruction->req->setFlags(Request::INST_FETCH);
    EXPECT_EQ(policy.getVictim({&entry}, instruction.get()), &entry);
    EXPECT_FALSE(instruction->bypassL2);
    auto exclusive = packet(MemCmd::ReadExResp);
    EXPECT_EQ(policy.getVictim({&entry}, exclusive.get()), nullptr);
    EXPECT_TRUE(exclusive->bypassL2);
    EXPECT_EQ(stat(policy, "bypasses"), 1);
}

TEST_F(SDBPInterface, InvalidationDoesNotForgetIndependentSampler)
{
    auto params = testParams();
    SDBP policy(params);
    auto demand = packet();
    auto data = policy.instantiateEntry();
    policy.observeFill(demand.get(), 0, 1);
    policy.reset(data, demand.get());
    policy.invalidate(data);
    policy.observe(demand.get(), 0, 1, true);
    EXPECT_EQ(stat(policy, "samplerHits"), 1);
    EXPECT_EQ(stat(policy, "liveTraining"), 1);
    EXPECT_EQ(stat(policy, "deadTraining"), 0);
}

TEST_F(SDBPInterface, CrossSourceHitsTrainOldBankAndReplaceItsSignature)
{
    auto params = testParams();
    params.sampler_assoc = 1;
    SDBP policy(params);
    using Source = Request::L2RequestSource;
    auto demand = sourcedPacket(Source::L1Demand, 0x1000);
    auto l1 = sourcedPacket(Source::L1Prefetch, 0x2000);
    auto l2 = sourcedPacket(Source::L2Prefetch, 0x3000);
    policy.observeFill(demand.get(), 0, 1);
    policy.observe(l1.get(), 0, 1, true);
    policy.observe(l2.get(), 0, 1, true);
    policy.observeFill(demand.get(), 0, 2);
    EXPECT_EQ(sourceStat(policy, "liveTrainingBySource", 0), 1);
    EXPECT_EQ(sourceStat(policy, "liveTrainingBySource", 1), 1);
    EXPECT_EQ(sourceStat(policy, "liveTrainingBySource", 2), 0);
    EXPECT_EQ(sourceStat(policy, "deadTrainingBySource", 0), 0);
    EXPECT_EQ(sourceStat(policy, "deadTrainingBySource", 1), 0);
    EXPECT_EQ(sourceStat(policy, "deadTrainingBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "samplerAccessesBySource", 0), 2);
    EXPECT_EQ(sourceStat(policy, "samplerAccessesBySource", 1), 1);
    EXPECT_EQ(sourceStat(policy, "samplerAccessesBySource", 2), 1);
}

TEST_F(SDBPInterface, PrefetchUsesTriggerPcAndOnlyItsOwnPredictor)
{
    auto params = testParams();
    params.sampler_assoc = 1;
    SDBP policy(params);
    using Source = Request::L2RequestSource;
    auto l1 = sourcedPacket(Source::L1Prefetch, 0x1000);
    auto l2 = sourcedPacket(Source::L2Prefetch, 0x1000);
    auto demand = sourcedPacket(Source::L1Demand, 0x1000);
    for (unsigned tag = 1; tag <= 8; ++tag)
        policy.observeFill(l1.get(), 0, tag);
    // The ordinary PC can change without changing PF attribution.
    l1->req->setPC(0x2000);
    auto data = policy.instantiateEntry();
    policy.reset(data, l1.get());
    EXPECT_EQ(SDBP::victimClass(data), 2);
    policy.reset(data, demand.get());
    EXPECT_EQ(SDBP::victimClass(data), 1);
    policy.reset(data, l2.get());
    EXPECT_EQ(SDBP::victimClass(data), 1);
    EXPECT_EQ(sourceStat(policy, "deadPredictionsBySource", 1), 1);
    EXPECT_EQ(sourceStat(policy, "deadPredictionsBySource", 0), 0);
    EXPECT_EQ(sourceStat(policy, "deadPredictionsBySource", 2), 0);
    for (unsigned tag = 9; tag <= 16; ++tag)
        policy.observeFill(l2.get(), 0, tag);
    policy.reset(data, l2.get());
    EXPECT_EQ(SDBP::victimClass(data), 2);
}

TEST_F(SDBPInterface, FullCacheObserverScoresEpisodesAndPrefetchDemandUse)
{
    auto params = testParams(false, 0);
    params.observation_only = true;
    params.l1_copy_protection = false;
    SDBP policy(params);
    auto data = policy.instantiateEntry();
    auto prefetch = sourcedPacket(Request::L2RequestSource::L2Prefetch,
                                  0x1000);
    auto demand = sourcedPacket(Request::L2RequestSource::L1Demand,
                                0x2000);

    policy.reset(data, prefetch.get());
    EXPECT_EQ(stat(policy, "fullCachePending"), 1);
    EXPECT_EQ(sourceStat(policy, "fullCachePrefetchPendingBySource", 2), 1);

    policy.touch(data, demand.get());
    EXPECT_EQ(sourceStat(policy, "fullCacheDeadFalsePositiveBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "fullCachePredDeadDemandReuseBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "fullCachePrefetchDemandUsesBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "fullCachePrefetchPendingBySource", 2), 0);

    policy.notifyReplacementEviction(data);
    EXPECT_EQ(sourceStat(policy, "fullCacheDeadTruePositiveBySource", 0), 1);
    EXPECT_EQ(stat(policy, "fullCacheResolved"), 2);
    EXPECT_EQ(stat(policy, "fullCachePending"), 0);
    EXPECT_EQ(stat(policy, "liveTraining"), 1);
    EXPECT_EQ(stat(policy, "deadTraining"), 1);

    policy.invalidate(data);
    EXPECT_EQ(stat(policy, "fullCacheCensored"), 0);
    EXPECT_EQ(stat(policy, "fullCachePending"), 0);
}

TEST_F(SDBPInterface, FullCacheObserverCensorsExternalInvalidation)
{
    auto params = testParams();
    params.observation_only = true;
    params.l1_copy_protection = false;
    SDBP policy(params);
    auto data = policy.instantiateEntry();
    auto demand = sourcedPacket(Request::L2RequestSource::L1Demand,
                                0x1000);

    policy.reset(data, demand.get());
    policy.invalidate(data);

    EXPECT_EQ(stat(policy, "fullCacheCensored"), 1);
    EXPECT_EQ(stat(policy, "fullCachePending"), 0);
    EXPECT_EQ(sourceStat(policy, "fullCacheResolvedBySource", 0), 0);
    EXPECT_EQ(sourceStat(policy, "fullCacheCensoredBySource", 0), 1);
}

TEST_F(SDBPInterface, FullCacheObserverVictimMatchesLruDespiteDeadPrediction)
{
    auto params = testParams(false, 0);
    params.observation_only = true;
    params.l1_copy_protection = false;
    SDBP policy(params);
    LRURPParams lru_params;
    lru_params.name = "lru_test";
    lru_params.eventq_index = 0;
    LRU lru(lru_params);

    std::array<ReplaceableEntry, 3> observer_entries;
    std::array<ReplaceableEntry, 3> lru_entries;
    std::array<std::shared_ptr<ReplacementData>, 3> observer_data;
    std::array<std::shared_ptr<ReplacementData>, 3> lru_data;
    auto demand = sourcedPacket(Request::L2RequestSource::L1Demand,
                                0x1000);
    for (unsigned way = 0; way < observer_entries.size(); ++way) {
        observer_entries[way].setPosition(0, way);
        lru_entries[way].setPosition(0, way);
        observer_data[way] = policy.instantiateEntry();
        lru_data[way] = lru.instantiateEntry();
        observer_entries[way].replacementData = observer_data[way];
        lru_entries[way].replacementData = lru_data[way];
        curEventQueue()->setCurTick(Tick(way + 1));
        policy.reset(observer_data[way], demand.get());
        lru.reset(lru_data[way]);
    }

    curEventQueue()->setCurTick(Tick(4));
    policy.touch(observer_data[0], demand.get());
    lru.touch(lru_data[0]);
    const ReplacementCandidates observer_candidates{
        &observer_entries[0], &observer_entries[1], &observer_entries[2]};
    const ReplacementCandidates lru_candidates{
        &lru_entries[0], &lru_entries[1], &lru_entries[2]};

    EXPECT_TRUE(SDBP::victimClass(observer_data[1]) == 2);
    EXPECT_EQ(policy.getVictim(observer_candidates), &observer_entries[1]);
    EXPECT_EQ(lru.getVictim(lru_candidates), &lru_entries[1]);
    // A tick tie must retain native candidate order, not access sequence.
    curEventQueue()->setCurTick(Tick(7));
    for (unsigned way : {2U, 1U, 0U}) {
        policy.touch(observer_data[way], demand.get());
        lru.touch(lru_data[way]);
    }
    EXPECT_EQ(policy.getVictim(observer_candidates), &observer_entries[0]);
    EXPECT_EQ(lru.getVictim(lru_candidates), &lru_entries[0]);
    policy.invalidate(observer_data[1]);
    lru.invalidate(lru_data[1]);
    EXPECT_EQ(policy.getVictim(observer_candidates), &observer_entries[1]);
    EXPECT_EQ(lru.getVictim(lru_candidates), &lru_entries[1]);
}

TEST_F(SDBPInterface, FullCacheObserverTrainsRealLinesAndScoresLivePredictions)
{
    SDBP policy(observerParams());
    auto data = policy.instantiateEntry();
    auto pf = sourcedPacket(Request::L2RequestSource::L1Prefetch, 0x1000);
    auto other = sourcedPacket(Request::L2RequestSource::L2Prefetch, 0x1000);
    // Three real LRU evictions saturate this source's dead counters.
    for (unsigned i = 0; i < 3; ++i) {
        policy.reset(data, pf.get());
        EXPECT_EQ(SDBP::victimClass(data), 1);
        policy.notifyReplacementEviction(data);
        policy.invalidate(data);
    }
    policy.reset(data, pf.get());
    EXPECT_EQ(SDBP::victimClass(data), 2);
    // Ordinary request PC does not change prefetch signature attribution.
    pf->req->setPC(0x4000);
    policy.touch(data, pf.get());
    EXPECT_EQ(SDBP::victimClass(data), 1);
    policy.touch(data, other.get());
    policy.notifyReplacementEviction(data);
    policy.invalidate(data);
    EXPECT_EQ(sourceStat(policy, "fullCacheLiveFalseNegativeBySource", 1), 3);
    EXPECT_EQ(sourceStat(policy, "fullCacheDeadFalsePositiveBySource", 1), 1);
    EXPECT_EQ(sourceStat(policy, "fullCacheLiveTrueNegativeBySource", 1), 1);
    EXPECT_EQ(sourceStat(policy, "fullCacheLiveFalseNegativeBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "liveTrainingBySource", 1), 2);
    EXPECT_EQ(stat(policy, "samplerAccesses"), 0);
    EXPECT_EQ(stat(policy, "fullCachePending"), 0);
}

TEST_F(SDBPInterface, FullCacheObserverResetStatsPreservesPendingAndPredictor)
{
    SDBP policy(observerParams(0));
    auto data = policy.instantiateEntry();
    auto pf = sourcedPacket(Request::L2RequestSource::L2Prefetch, 0x1000);
    auto demand = sourcedPacket(Request::L2RequestSource::L1Demand, 0x2000);
    policy.reset(data, pf.get());
    policy.resetStats();
    EXPECT_EQ(stat(policy, "fullCachePredictions"), 0);
    EXPECT_EQ(stat(policy, "fullCachePending"), 1);
    EXPECT_EQ(sourceStat(policy, "fullCacheCarriedInBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "fullCachePrefetchCarriedInBySource", 2), 1);
    policy.touch(data, demand.get());
    EXPECT_EQ(sourceStat(policy, "fullCacheDeadFalsePositiveBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "fullCachePrefetchDemandUsesBySource", 2), 1);
    EXPECT_EQ(sourceStat(policy, "fullCachePrefetchPendingBySource", 2), 0);
    policy.notifyReplacementEviction(data);
    policy.invalidate(data);
    EXPECT_EQ(stat(policy, "fullCachePending"), 0);
    EXPECT_EQ(stat(policy, "fullCacheResolved"), 2);
    EXPECT_EQ(stat(policy, "fullCachePredictions"), 1);
}

TEST_F(SDBPInterface, FullCacheObserverLocalPrefetchDoesNotRefreshLru)
{
    SDBP policy(observerParams(0));
    ReplaceableEntry a, b;
    a.replacementData = policy.instantiateEntry();
    b.replacementData = policy.instantiateEntry();
    auto demand = sourcedPacket(Request::L2RequestSource::L1Demand, 0x1000);
    auto pf = sourcedPacket(Request::L2RequestSource::L2Prefetch, 0x2000);
    curEventQueue()->setCurTick(10);
    policy.reset(a.replacementData, demand.get());
    curEventQueue()->setCurTick(20);
    policy.reset(b.replacementData, demand.get());
    curEventQueue()->setCurTick(30);
    policy.observeLocalPrefetch(pf.get(), a.replacementData);
    EXPECT_EQ(policy.getVictim({&a, &b}), &a);
    EXPECT_EQ(sourceStat(policy, "fullCachePrefetchReuseBySource", 0), 1);
    // Selection is speculative; only committed eviction trains dead.
    EXPECT_EQ(stat(policy, "deadTraining"), 0);
    policy.notifyReplacementEviction(a.replacementData);
    EXPECT_EQ(sourceStat(policy, "deadTrainingBySource", 2), 1);
}

TEST_F(SDBPInterface, FullCacheObserverMissingPcReuseResolvesButCannotPredict)
{
    SDBP policy(observerParams(0));
    auto data = policy.instantiateEntry();
    auto pf = sourcedPacket(Request::L2RequestSource::L1Prefetch, 0x1000);
    auto demand = packet(MemCmd::ReadReq, false);
    auto wb = packet(MemCmd::WritebackDirty);
    policy.reset(data, pf.get());
    policy.touch(data, wb.get());
    EXPECT_EQ(stat(policy, "fullCacheResolved"), 0);
    policy.touch(data, demand.get());
    EXPECT_EQ(sourceStat(policy, "fullCacheDeadFalsePositiveBySource", 1), 1);
    EXPECT_EQ(stat(policy, "fullCachePending"), 0);
    EXPECT_EQ(stat(policy, "fullCachePredictions"), 1);
    policy.notifyReplacementEviction(data);
    policy.invalidate(data);
    EXPECT_EQ(stat(policy, "deadTraining"), 0);
}

TEST_F(SDBPInterface, LocalPrefetchCacheHitTrainsSamplerVictimDead)
{
    auto params = testParams();
    params.sampler_assoc = 1;
    SDBP policy(params);
    using Source = Request::L2RequestSource;
    auto demand = sourcedPacket(Source::L1Demand, 0x1000);
    auto local = sourcedPacket(Source::L2Prefetch, 0x2000);
    policy.observeFill(demand.get(), 0, 1);
    policy.observe(local.get(), 0, 2, true);
    EXPECT_EQ(stat(policy, "samplerEvictions"), 1);
    EXPECT_EQ(stat(policy, "deadTraining"), 1);
    EXPECT_EQ(sourceStat(policy, "deadTrainingBySource", 0), 1);
    EXPECT_EQ(sourceStat(policy, "deadTrainingBySource", 2), 0);
    policy.observe(demand.get(), 0, 2, true);
    EXPECT_EQ(sourceStat(policy, "liveTrainingBySource", 2), 1);
}

TEST_F(SDBPInterface, TriggerOnlyPrefetchPcIsValidIncludingZero)
{
    SDBP policy(testParams(false, 0));
    auto local = packet(MemCmd::ReadResp, false);
    local->req->setFlags(Request::PREFETCH);
    local->req->setL2RequestSource(Request::L2RequestSource::L2Prefetch);
    local->req->setPrefetchTriggerPC(0);
    policy.observeFill(local.get(), 0, 1);
    EXPECT_EQ(sourceStat(policy, "samplerAccessesBySource", 2), 1);
    auto data = policy.instantiateEntry();
    policy.reset(data, local.get());
    EXPECT_EQ(SDBP::victimClass(data), 2);
}

TEST_F(SDBPInterface, MergedDemandUsesDemandPcAndRejectsMissingDemandPc)
{
    auto params = testParams();
    params.sampler_assoc = 1;
    SDBP policy(params);
    using Source = Request::L2RequestSource;
    auto demand = sourcedPacket(Source::L1Demand, 0x2000);
    for (unsigned tag = 1; tag <= 8; ++tag)
        policy.observeFill(demand.get(), 0, tag);
    auto merged = sourcedPacket(Source::L2Prefetch, 0x1000);
    merged->setSamplerDemand();
    merged->req->setPC(0x2000);
    auto data = policy.instantiateEntry();
    policy.reset(data, merged.get());
    EXPECT_EQ(SDBP::victimClass(data), 2);
    EXPECT_EQ(sourceStat(policy, "predictionQueriesBySource", 0), 1);
    EXPECT_EQ(sourceStat(policy, "predictionQueriesBySource", 2), 0);
    merged->req->clearPC();
    policy.observeFill(merged.get(), 0, 9);
    EXPECT_EQ(stat(policy, "noPcAccesses"), 1);
    EXPECT_EQ(stat(policy, "samplerAccesses"), 8);
}

TEST_F(SDBPInterface, MemoryInstructionUpgradesAndAtomicsUseDemandBank)
{
    SDBP policy(testParams(false, 0));
    unsigned tag = 0;
    for (auto command : {MemCmd::UpgradeReq, MemCmd::SCUpgradeReq,
                         MemCmd::UpgradeResp, MemCmd::LoadLockedReq,
                         MemCmd::StoreCondReq, MemCmd::LockedRMWReadReq,
                         MemCmd::LockedRMWWriteReq}) {
        auto demand = sourcedPacket(Request::L2RequestSource::L1Demand,
                                     0x1000);
        demand->cmd = command;
        policy.observeFill(demand.get(), 0, ++tag);
        auto data = policy.instantiateEntry();
        policy.reset(data, demand.get());
        EXPECT_EQ(SDBP::victimClass(data), 2);
    }
    EXPECT_EQ(stat(policy, "samplerAccesses"), tag);
    EXPECT_EQ(sourceStat(policy, "samplerAccessesBySource", 0), tag);
    EXPECT_EQ(sourceStat(policy, "predictionQueriesBySource", 0), tag);
    EXPECT_EQ(sourceStat(policy, "predictionQueriesBySource", 1), 0);
    EXPECT_EQ(sourceStat(policy, "predictionQueriesBySource", 2), 0);
}

TEST_F(SDBPInterface, ExplicitOtherInstructionAndPtwNeverTrain)
{
    SDBP policy(testParams(false, 0));
    auto other = sourcedPacket(Request::L2RequestSource::Other, 0x1000);
    policy.observeFill(other.get(), 0, 1);
    auto instruction = packet();
    instruction->req->setFlags(Request::INST_FETCH);
    policy.observeFill(instruction.get(), 0, 2);
    auto ptw = packet();
    ptw->req->setFlags(Request::PT_WALK);
    policy.observeFill(ptw.get(), 0, 3);
    EXPECT_EQ(stat(policy, "samplerAccesses"), 0);
    EXPECT_EQ(stat(policy, "excludedAccesses"), 3);
}

}  // anonymous namespace
}  // namespace replacement_policy
}  // namespace gem5
