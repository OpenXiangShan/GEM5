/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <array>

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
    ReplaceableEntry a, b, c;
    a.replacementData = policy.instantiateEntry();
    b.replacementData = policy.instantiateEntry();
    c.replacementData = policy.instantiateEntry();
    auto demand = packet();
    policy.reset(b.replacementData);
    policy.reset(a.replacementData, demand.get());
    policy.reset(c.replacementData);
    EXPECT_EQ(policy.getVictim(ReplacementCandidates{&a, &b, &c}), &a);
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

TEST_F(SDBPInterface, MruDeadIsProtectedUntilAnotherWayIsTouched)
{
    SDBP policy(testParams(false, 0));
    std::array<ReplaceableEntry, 8> entries;
    ReplacementCandidates candidates;
    auto demand = packet();
    for (unsigned i = 0; i < entries.size(); ++i) {
        entries[i].replacementData = policy.instantiateEntry();
        candidates.push_back(&entries[i]);
        if (i == 7)
            policy.reset(entries[i].replacementData, demand.get());
        else
            policy.reset(entries[i].replacementData);
    }
    EXPECT_EQ(policy.getVictim(candidates), &entries[0]);
    EXPECT_EQ(stat(policy, "lruVictims"), 1);
    EXPECT_EQ(stat(policy, "deadVictims"), 0);
    EXPECT_EQ(SDBP::victimClass(entries[7].replacementData), 2);

    // A hit to another way ends protection without clearing the old dead bit.
    policy.touch(entries[1].replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &entries[7]);
    EXPECT_EQ(stat(policy, "deadVictims"), 1);
    EXPECT_EQ(stat(policy, "nonLruDeadVictims"), 1);
    EXPECT_EQ(SDBP::victimClass(entries[7].replacementData), 2);
}

TEST_F(SDBPInterface, MruGuardPreservesDeadCandidateAndInvalidPriority)
{
    SDBP policy(testParams(false, 0));
    std::array<ReplaceableEntry, 8> entries;
    ReplacementCandidates candidates;
    auto demand = packet();
    for (unsigned i = 0; i < entries.size(); ++i) {
        entries[i].replacementData = policy.instantiateEntry();
        if (i == 2 || i == 5 || i == 7)
            policy.reset(entries[i].replacementData, demand.get());
        else
            policy.reset(entries[i].replacementData);
    }
    // MRU comes first; choose way 5 before the older dead way 2.
    for (unsigned i : {7, 5, 2, 0, 1, 3, 4, 6})
        candidates.push_back(&entries[i]);
    EXPECT_EQ(policy.getVictim(candidates), &entries[5]);
    EXPECT_EQ(SDBP::victimClass(entries[7].replacementData), 2);

    // A fill also moves MRU and makes the previous MRU eligible.
    policy.reset(entries[6].replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &entries[7]);
    policy.invalidate(entries[3].replacementData);
    EXPECT_EQ(policy.getVictim(candidates), &entries[3]);
}

TEST_F(SDBPInterface, MruGuardAllowsLruFallbackWithOneWay)
{
    SDBP policy(testParams(false, 0));
    ReplaceableEntry entry;
    entry.replacementData = policy.instantiateEntry();
    auto demand = packet();
    policy.reset(entry.replacementData, demand.get());
    EXPECT_EQ(policy.getVictim(ReplacementCandidates{&entry}), &entry);
    EXPECT_EQ(stat(policy, "lruVictims"), 1);
    EXPECT_EQ(stat(policy, "deadVictims"), 0);
    EXPECT_EQ(SDBP::victimClass(entry.replacementData), 2);
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
