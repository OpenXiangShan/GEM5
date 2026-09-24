/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>

#include <set>
#include <stdexcept>

#include "mem/cache/replacement_policies/sdbp_core.hh"

namespace gem5
{
namespace replacement_policy
{

TEST(SDBPCore, SamplingMapsEveryIndexExactlyOnce)
{
    for (unsigned bits = 2; bits <= 14; ++bits) {
        for (unsigned half = 1; half < bits; ++half) {
            SDBPCore::Config cfg;
            cfg.numSets = 1U << bits;
            cfg.samplerNum = 1U << half;
            SDBPCore core(cfg);
            std::set<int> indices;
            unsigned count = 0;
            for (unsigned set = 0; set < cfg.numSets; ++set) {
                int index = core.sampleIndex(set);
                if (index >= 0) {
                    ++count;
                    EXPECT_LT(unsigned(index), cfg.samplerNum);
                    indices.insert(index);
                }
            }
            EXPECT_EQ(count, cfg.samplerNum);
            EXPECT_EQ(indices.size(), count);
        }
    }
}

TEST(SDBPCore, SamplingRoundsToNearestAndTiesDown)
{
    SDBPCore::Config cfg;
    cfg.samplerNum = 24;
    EXPECT_EQ(SDBPCore(cfg).samplerSets(), 16U);
    cfg.samplerNum = 25;
    EXPECT_EQ(SDBPCore(cfg).samplerSets(), 32U);
    cfg.samplerNum = 31;
    EXPECT_EQ(SDBPCore(cfg).samplerSets(), 32U);
}

TEST(SDBPCore, CounterSaturationAndCrcLiveUpdate)
{
    SDBPCore core{SDBPCore::Config()};
    EXPECT_EQ(core.confidence(42), 0U);
    core.train(42, true);
    EXPECT_EQ(core.confidence(42), 3U);
    core.train(42, true);
    EXPECT_FALSE(core.predict(42));
    core.train(42, true);
    EXPECT_TRUE(core.predict(42));
    for (unsigned i = 0; i < 100; ++i)
        core.train(42, true);
    EXPECT_EQ(core.confidence(42), 9U);
    core.train(42, false);
    EXPECT_EQ(core.confidence(42), 5U);
    EXPECT_FALSE(core.predict(42));
    for (unsigned i = 0; i < 100; ++i)
        core.train(42, false);
    EXPECT_EQ(core.confidence(42), 0U);
}

TEST(SDBPCore, UniformLiveUpdateAndEightBitCounters)
{
    SDBPCore::Config cfg;
    cfg.crcLiveUpdate = false;
    cfg.counterBits = 8;
    cfg.deadThreshold = 765;
    SDBPCore core(cfg);
    for (unsigned i = 0; i < 300; ++i)
        core.train(12, true);
    EXPECT_EQ(core.confidence(12), 765U);
    EXPECT_TRUE(core.predict(12));
    core.train(12, false);
    EXPECT_EQ(core.confidence(12), 762U);
    EXPECT_FALSE(core.predict(12));
}

TEST(SDBPCore, TrainsOldSignatureAndNeverInvalidEntries)
{
    SDBPCore::Config cfg;
    cfg.samplerAssoc = 1;
    SDBPCore core(cfg);
    const auto first = core.signature(0x1000);
    const auto second = core.signature(0x2000);
    const auto initial = core.access(0, 5, false, 0x1000);
    EXPECT_TRUE(initial.sampled);
    EXPECT_FALSE(initial.evicted);
    EXPECT_EQ(core.confidence(0), 0U);
    core.train(first, true);
    const auto hit = core.access(0, 5, false, 0x2000);
    EXPECT_TRUE(hit.hit);
    EXPECT_EQ(hit.previousSignature, first);
    EXPECT_EQ(core.confidence(first), 0U);
    const auto evicted = core.access(0, 6, false, 0x3000);
    EXPECT_TRUE(evicted.evicted);
    EXPECT_EQ(evicted.previousSignature, second);
    EXPECT_EQ(core.confidence(second), 3U);
}

TEST(SDBPCore, UnsampledSetsDoNotTrain)
{
    SDBPCore core{SDBPCore::Config()};
    EXPECT_EQ(core.sampleIndex(1), -1);
    for (unsigned tag = 0; tag < 100; ++tag)
        EXPECT_FALSE(core.access(1, tag, false, 0x1000).sampled);
    EXPECT_EQ(core.confidence(core.signature(0x1000)), 0U);
}

TEST(SDBPCore, InvalidBeforeDeadThenLru)
{
    SDBPCore::Config cfg;
    cfg.samplerAssoc = 2;
    cfg.deadThreshold = 0;
    SDBPCore core(cfg);
    core.access(0, 1, false, 0x1000);
    EXPECT_FALSE(core.access(0, 2, false, 0x2000).evicted);
    // Both dead: choose first way even though it has just been touched.
    core.access(0, 1, false, 0x1000);
    EXPECT_EQ(core.access(0, 3, false, 0x3000).previousSignature, core.signature(0x1000));

    cfg.deadThreshold = 8;
    SDBPCore lru(cfg);
    lru.access(0, 1, false, 0x1000);
    lru.access(0, 2, false, 0x2000);
    lru.access(0, 1, false, 0x1000);
    EXPECT_EQ(lru.access(0, 3, false, 0x3000).previousSignature, lru.signature(0x2000));
}

TEST(SDBPCore, PartialTagsAliasButSecurityDomainsDoNot)
{
    SDBPCore::Config cfg;
    cfg.partialTagBits = 3;
    SDBPCore core(cfg);
    core.access(0, 1, false, 0x1000);
    EXPECT_TRUE(core.access(0, 9, false, 0x2000).hit);
    EXPECT_FALSE(core.access(0, 1, true, 0x3000).hit);
    EXPECT_TRUE(core.access(0, 1, false, 0x4000).hit);
    EXPECT_TRUE(core.access(0, 1, true, 0x5000).hit);
}

TEST(SDBPCore, HashUsesHighPcBitsAndDistinctTableMappings)
{
    SDBPCore::Config cfg;
    for (const char *hash : {"xor_fold", "mixed"}) {
        cfg.pcHashType = hash;
        SDBPCore core(cfg);
        EXPECT_NE(core.signature(0x1000), core.signature(0x1000 | (uint64_t(1) << 48)));
        std::set<unsigned> indices;
        for (unsigned table = 0; table < cfg.predictorTables; ++table)
            indices.insert(core.index(core.signature(0x12345678), table));
        EXPECT_EQ(indices.size(), 3U);
    }
    cfg.pcHashType = "low_bits";
    SDBPCore core(cfg);
    EXPECT_EQ(core.signature(0x1000), core.signature(0x1000 | (uint64_t(1) << 48)));
}

TEST(SDBPCore, RejectsInvalidConfiguration)
{
    const SDBPCore::Config defaults;
    auto cfg = defaults;
    cfg.numSets = 3;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.samplerNum = 1;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.samplerAssoc = 0;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.predictorTables = 33;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.predictorEntries = 3;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.counterBits = 9;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.deadThreshold = 10;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.partialPcBits = 64;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.pcShift = 64;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.pcHashType = "unknown";
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.indexHashType = "unknown";
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.tableHashSeeds = {1, 1, 1};
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg.tableHashSeeds = {1, 2};
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg = defaults;
    cfg.indexHashType = "jwac";
    cfg.partialPcBits = 33;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
}

TEST(SDBPCore, JwacHashMatchesPublishedSingleThreadReference)
{
    SDBPCore::Config cfg;
    cfg.pcHashType = "low_bits";
    cfg.pcShift = 0;
    cfg.partialPcBits = 16;
    cfg.indexHashType = "jwac";
    SDBPCore core(cfg);
    // Expected values are computed by the CRC f1(x) + (f2(x) >> t).
    const unsigned expected[][3] = {
        {1256, 2908, 1686},
        {744, 2269, 984},
        {2200, 3601, 206},
        {2792, 2911, 923},
    };
    const unsigned signatures[] = {0, 1, 0x1234, 0xffff};
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned t = 0; t < 3; ++t)
            EXPECT_EQ(core.index(signatures[i], t), expected[i][t]);
    }
    EXPECT_EQ(core.signature(0x12345678), 0x5678U);
}

TEST(SDBPCore, TrainingPropagatesToUnsampledSetsAndRecoversOnReuse)
{
    SDBPCore::Config cfg;
    cfg.samplerAssoc = 1;
    SDBPCore core(cfg);
    const auto signature = core.signature(0x1000);
    for (unsigned tag = 0; tag < 8; ++tag)
        core.access(0, tag, false, 0x1000);
    EXPECT_TRUE(core.predict(signature));
    const auto confidence = core.confidence(signature);
    EXPECT_FALSE(core.access(1, 123, false, 0x1000).sampled);
    EXPECT_EQ(core.confidence(signature), confidence);
    const auto hit = core.access(0, 7, false, 0x1000);
    EXPECT_TRUE(hit.hit);
    EXPECT_TRUE(hit.previousDead);
    EXPECT_FALSE(core.predict(signature));
}

TEST(SDBPCore, PcShiftSeedAndExtremeWidths)
{
    SDBPCore::Config cfg;
    cfg.pcShift = 63;
    cfg.pcHashSeed = 5;
    cfg.partialPcBits = 63;
    SDBPCore core(cfg);
    EXPECT_EQ(core.signature(uint64_t(1) << 63), 4U);
    EXPECT_EQ(core.signature(0), 5U);
    cfg.pcShift = 0;
    cfg.pcHashSeed = 0;
    cfg.partialPcBits = 1;
    SDBPCore parity(cfg);
    EXPECT_EQ(parity.signature(uint64_t(1) << 63), 1U);
    EXPECT_EQ(parity.signature((uint64_t(1) << 63) | 1), 0U);
}

}  // namespace replacement_policy
}  // namespace gem5
