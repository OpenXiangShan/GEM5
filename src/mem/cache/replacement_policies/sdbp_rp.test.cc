/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "mem/cache/replacement_policies/sdbp_rp.hh"
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
    p.sampler_protect_youngest_two = false;
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

class SDBPInterface : public ::testing::Test
{
  protected:
    void SetUp() override { curEventQueue(getEventQueue(0)); }
};

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
    params.sampler_protect_youngest_two = true;
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

TEST_F(SDBPInterface, FiltersPrefetchRequestsWritebackEvictionAndMissingPc)
{
    auto params = testParams(true, 0);
    SDBP policy(params);
    auto data = policy.instantiateEntry();
    for (auto cmd : {MemCmd::HardPFReq, MemCmd::SoftPFReq, MemCmd::HardPFResp, MemCmd::WritebackDirty,
                     MemCmd::WritebackClean, MemCmd::CleanEvict, MemCmd::WriteClean}) {
        auto excluded = packet(cmd);
        policy.observe(excluded.get(), 0, 1, true);
        policy.reset(data, excluded.get());
    }
    auto missing = packet(MemCmd::ReadReq, false);
    policy.observeFill(missing.get(), 0, 2);
    policy.reset(data, missing.get());
    EXPECT_EQ(stat(policy, "samplerAccesses"), 4);
    // An unflagged HardPFResp is treated as a demand response by isDemand.
    EXPECT_EQ(stat(policy, "predictionQueries"), 1);
    EXPECT_EQ(stat(policy, "excludedAccesses"), 4);
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
    EXPECT_EQ(stat(policy, "predictionQueries"), 0);
    EXPECT_EQ(stat(policy, "deadPredictions"), 0);
    EXPECT_EQ(stat(policy, "excludedAccesses"), 0);
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

TEST_F(SDBPInterface, L1CopyGuardDoesNotProbeInvalidLiveOrBypassedSets)
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
    EXPECT_EQ(policy.getVictim(candidates, demand.get()), nullptr);
    EXPECT_EQ(queries, 0);
    policy.touch(a.replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &b);
    EXPECT_EQ(queries, 0);
    EXPECT_EQ(stat(policy, "l1CopyLruFallbacks"), 0);
}

TEST_F(SDBPInterface, BypassOnlyFullSetsWithEligiblePc)
{
    auto params = testParams(true, 0);
    SDBP policy(params);
    ReplaceableEntry a, b;
    a.replacementData = policy.instantiateEntry();
    b.replacementData = policy.instantiateEntry();
    ReplacementCandidates candidates{&a, &b};
    auto demand = packet();
    policy.reset(a.replacementData);
    EXPECT_EQ(policy.getVictim(candidates, demand.get()), &b);
    policy.reset(b.replacementData);
    EXPECT_EQ(policy.getVictim(candidates, demand.get()), nullptr);
    auto response = packet(MemCmd::ReadResp);
    EXPECT_EQ(policy.getVictim(candidates, response.get()), nullptr);
    auto missing = packet(MemCmd::ReadReq, false);
    EXPECT_NE(policy.getVictim(candidates, missing.get()), nullptr);
    auto prefetch = packet(MemCmd::HardPFResp);
    EXPECT_EQ(policy.getVictim(candidates, prefetch.get()), nullptr);
    prefetch->req->setFlags(Request::PREFETCH);
    EXPECT_NE(policy.getVictim(candidates, prefetch.get()), nullptr);
    EXPECT_NE(policy.getVictim(candidates), nullptr);
    EXPECT_EQ(stat(policy, "bypasses"), 3);
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

}  // anonymous namespace
}  // namespace replacement_policy
}  // namespace gem5
