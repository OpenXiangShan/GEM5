/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

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
        policy.observe(demand.get(), 0, tag, false);
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

TEST_F(SDBPInterface, FiltersPrefetchWritebackEvictionAndMissingPc)
{
    auto params = testParams(true, 0);
    SDBP policy(params);
    auto data = policy.instantiateEntry();
    for (auto cmd : {MemCmd::HardPFReq, MemCmd::SoftPFReq, MemCmd::HardPFResp, MemCmd::WritebackDirty,
                     MemCmd::WritebackClean, MemCmd::CleanEvict, MemCmd::WriteClean}) {
        auto excluded = packet(cmd);
        policy.observe(excluded.get(), 0, 1, false);
        policy.reset(data, excluded.get());
    }
    // Prefetches may reach lower caches as ordinary reads. The Request flag
    // must exclude both the request and its response even with a valid PC.
    for (auto cmd : {MemCmd::ReadReq, MemCmd::ReadResp}) {
        auto excluded = packet(cmd);
        excluded->req->setFlags(Request::PREFETCH);
        policy.observe(excluded.get(), 0, 1, false);
        policy.reset(data, excluded.get());
    }
    auto missing = packet(MemCmd::ReadReq, false);
    policy.observe(missing.get(), 0, 2, false);
    policy.reset(data, missing.get());
    EXPECT_EQ(stat(policy, "samplerAccesses"), 0);
    EXPECT_EQ(stat(policy, "predictionQueries"), 0);
    EXPECT_EQ(stat(policy, "excludedAccesses"), 9);
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
    EXPECT_NE(policy.getVictim(candidates, prefetch.get()), nullptr);
    EXPECT_NE(policy.getVictim(candidates), nullptr);
    EXPECT_EQ(stat(policy, "bypasses"), 2);
}

TEST_F(SDBPInterface, InvalidationDoesNotForgetIndependentSampler)
{
    auto params = testParams();
    SDBP policy(params);
    auto demand = packet();
    auto data = policy.instantiateEntry();
    policy.observe(demand.get(), 0, 1, false);
    policy.reset(data, demand.get());
    policy.invalidate(data);
    policy.observe(demand.get(), 0, 1, false);
    EXPECT_EQ(stat(policy, "samplerHits"), 1);
    EXPECT_EQ(stat(policy, "liveTraining"), 1);
    EXPECT_EQ(stat(policy, "deadTraining"), 0);
}

}  // anonymous namespace
}  // namespace replacement_policy
}  // namespace gem5
