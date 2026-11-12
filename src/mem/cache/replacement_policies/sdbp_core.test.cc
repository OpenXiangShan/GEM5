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
        for (unsigned half = 1; half <= bits; ++half) {
            SDBPCore::Config cfg;
            cfg.numSets = 1U << bits;
            cfg.samplerNum = 1U << half;
            SDBPCore core(cfg);
            EXPECT_EQ(core.samplerSets(), cfg.samplerNum);
            EXPECT_EQ(core.halfBits(), half);
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

TEST(SDBPCore, SamplingRejectsNonPowerOfTwo)
{
    SDBPCore::Config cfg;
    cfg.samplerNum = 24;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg.samplerNum = 25;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
    cfg.samplerNum = 31;
    EXPECT_THROW(SDBPCore{cfg}, std::invalid_argument);
}

TEST(SDBPCore, FullCacheModeDisablesIndependentSampler)
{
    SDBPCore::Config cfg;
    cfg.enableSampler = false;
    SDBPCore core(cfg);

    EXPECT_EQ(core.samplerSets(), 0);
    EXPECT_EQ(core.sampleIndex(0), -1);
    const auto result = core.access(
        0, 1, SDBPCore::RequestType::Demand, true, 0x1000);
    EXPECT_FALSE(result.sampled);
    EXPECT_EQ(core.confidence(core.signature(0x1000)), 0);
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
    const auto initial = core.access(0, 5, 0x1000);
    EXPECT_TRUE(initial.sampled);
    EXPECT_FALSE(initial.evicted);
    EXPECT_EQ(core.confidence(0), 0U);
    core.train(first, true);
    const auto hit = core.access(0, 5, 0x2000);
    EXPECT_TRUE(hit.hit);
    EXPECT_EQ(hit.previousSignature, first);
    EXPECT_EQ(core.confidence(first), 0U);
    const auto evicted = core.access(0, 6, 0x3000);
    EXPECT_TRUE(evicted.evicted);
    EXPECT_EQ(evicted.previousSignature, second);
    EXPECT_EQ(core.confidence(second), 3U);
}

TEST(SDBPCore, UnsampledSetsDoNotTrain)
{
    SDBPCore core{SDBPCore::Config()};
    EXPECT_EQ(core.sampleIndex(1), -1);
    for (unsigned tag = 0; tag < 100; ++tag)
        EXPECT_FALSE(core.access(1, tag, 0x1000).sampled);
    EXPECT_EQ(core.confidence(core.signature(0x1000)), 0U);
}

TEST(SDBPCore, DemandPcOnlyAllocatesDemandWithPcIncludingZero)
{
    SDBPCore::Config cfg;
    cfg.samplerDemandPcOnly = true;
    cfg.samplerAssoc = 1;
    SDBPCore core(cfg);
    for (auto type : {SDBPCore::RequestType::Demand,
                      SDBPCore::RequestType::L1Prefetch,
                      SDBPCore::RequestType::L2Prefetch}) {
        for (bool hasPc : {false, true}) {
            if (type == SDBPCore::RequestType::Demand && hasPc)
                continue;
            const auto rejected = core.access(0, 1, type, hasPc, 0x1000);
            EXPECT_FALSE(rejected.sampled);
            EXPECT_FALSE(rejected.hit);
            EXPECT_FALSE(rejected.evicted);
        }
    }
    const auto inserted = core.access(0, 1, 0);
    EXPECT_TRUE(inserted.sampled);
    EXPECT_FALSE(inserted.hit);
    EXPECT_FALSE(inserted.evicted);
    const auto evicted = core.access(0, 2, 0x2000);
    EXPECT_TRUE(evicted.evicted);
    EXPECT_TRUE(evicted.previousSignatureValid);
    EXPECT_EQ(evicted.previousSignature, core.signature(0));
    EXPECT_EQ(core.confidence(core.signature(0)), 3U);
}

TEST(SDBPCore, DemandPcOnlyPreservesTrainingMetadataAndRecency)
{
    for (auto type : {SDBPCore::RequestType::Demand,
                      SDBPCore::RequestType::L1Prefetch,
                      SDBPCore::RequestType::L2Prefetch}) {
        for (bool hasPc : {false, true}) {
            if (type == SDBPCore::RequestType::Demand && hasPc)
                continue;
            SDBPCore::Config cfg;
            cfg.samplerDemandPcOnly = true;
            cfg.samplerDeadFirst = false;
            cfg.samplerAssoc = 2;
            cfg.deadThreshold = 3;
            SDBPCore core(cfg);
            const auto first = core.signature(0x1000);
            const auto second = core.signature(0x2000);
            core.train(first, true);
            core.access(0, 1, 0x1000);
            core.access(0, 2, 0x2000);
            const auto firstConfidence = core.confidence(first);
            const auto secondConfidence = core.confidence(second);
            // A rejected hit cannot train live or refresh LRU. A rejected
            // miss cannot train dead, allocate, or replace an old signature.
            for (unsigned tag : {1U, 3U}) {
                const auto rejected = core.access(0, tag, type, hasPc, 0x3000);
                EXPECT_FALSE(rejected.sampled);
                EXPECT_FALSE(rejected.hit);
                EXPECT_FALSE(rejected.evicted);
                EXPECT_EQ(core.confidence(first), firstConfidence);
                EXPECT_EQ(core.confidence(second), secondConfidence);
            }
            const auto victim = core.access(0, 3, 0x3000);
            EXPECT_TRUE(victim.evicted);
            EXPECT_TRUE(victim.previousDead);
            EXPECT_TRUE(victim.previousSignatureValid);
            EXPECT_EQ(victim.previousSignature, first);
            EXPECT_EQ(core.confidence(first), firstConfidence + 3);
        }
    }
}

TEST(SDBPCore, DemandPcOnlyRetainsLiveAndDeadTrainingOfOldSignatures)
{
    SDBPCore::Config cfg;
    cfg.samplerDemandPcOnly = true;
    cfg.samplerAssoc = 1;
    cfg.crcLiveUpdate = false;
    SDBPCore core(cfg);
    const auto first = core.signature(0x1000);
    const auto second = core.signature(0x2000);
    core.train(first, true);
    core.access(0, 1, 0x1000);
    const auto hit = core.access(0, 1, 0x2000);
    EXPECT_TRUE(hit.hit);
    EXPECT_TRUE(hit.previousSignatureValid);
    EXPECT_EQ(hit.previousSignature, first);
    EXPECT_EQ(core.confidence(first), 0U);
    const auto evicted = core.access(0, 2, 0x3000);
    EXPECT_TRUE(evicted.evicted);
    EXPECT_TRUE(evicted.previousSignatureValid);
    EXPECT_EQ(evicted.previousSignature, second);
    EXPECT_EQ(core.confidence(second), 3U);
}

TEST(SDBPCore, DefaultSamplerAdmitsEveryPcSourceAndTrainsItsVictims)
{
    for (auto type : {SDBPCore::RequestType::Demand,
                      SDBPCore::RequestType::L1Prefetch,
                      SDBPCore::RequestType::L2Prefetch}) {
        SDBPCore::Config cfg;
        cfg.samplerAssoc = 1;
        SDBPCore core(cfg);
        const auto first = core.signature(0x1000);
        const auto second = core.signature(0x2000);
        core.access(0, 1, 0x1000);
        const auto admitted = core.access(0, 2, type, true, 0x2000);
        EXPECT_TRUE(admitted.sampled);
        EXPECT_TRUE(admitted.evicted);
        EXPECT_EQ(admitted.previousSignature, first);
        EXPECT_EQ(core.confidence(first), 3U);
        const auto evicted = core.access(0, 3, 0x3000);
        EXPECT_TRUE(evicted.previousSignatureValid);
        EXPECT_EQ(evicted.previousSignature, second);
        EXPECT_EQ(evicted.previousType, type);
        EXPECT_EQ(core.confidence(second, type), 3U);
    }
}

TEST(SDBPCore, MissingPcCannotTrainReplaceOrRefreshAnySource)
{
    for (auto type : {SDBPCore::RequestType::Demand,
                      SDBPCore::RequestType::L1Prefetch,
                      SDBPCore::RequestType::L2Prefetch}) {
        SDBPCore::Config cfg;
        cfg.samplerAssoc = 2;
        cfg.samplerDeadFirst = false;
        SDBPCore core(cfg);
        core.access(0, 1, 0x1000);
        core.access(0, 2, 0x2000);
        for (unsigned tag : {1U, 3U}) {
            const auto rejected = core.access(0, tag, type, false, 0);
            EXPECT_FALSE(rejected.sampled);
        }
        const auto victim = core.access(0, 3, 0x3000);
        EXPECT_EQ(victim.previousSignature, core.signature(0x1000));
        EXPECT_EQ(core.confidence(core.signature(0x1000)), 3U);
    }
}

TEST(SDBPCore, PredictorBanksAreIndependentWithIdenticalOrganizations)
{
    for (auto source : {SDBPCore::RequestType::Demand,
                        SDBPCore::RequestType::L1Prefetch,
                        SDBPCore::RequestType::L2Prefetch}) {
        SDBPCore core{SDBPCore::Config()};
        const auto signature = core.signature(0x1000);
        for (unsigned update = 0; update < 10; ++update)
            core.train(signature, true, source);
        for (auto other : {SDBPCore::RequestType::Demand,
                           SDBPCore::RequestType::L1Prefetch,
                           SDBPCore::RequestType::L2Prefetch}) {
            EXPECT_EQ(core.confidence(signature, other),
                      other == source ? 9U : 0U);
            EXPECT_EQ(core.predict(signature, other), other == source);
        }
        core.train(signature, false, source);
        EXPECT_EQ(core.confidence(signature, source), 5U);
    }
}

TEST(SDBPCore, EverySourceTransitionTrainsOldHitThenStoresNewSignature)
{
    for (auto old : {SDBPCore::RequestType::Demand,
                     SDBPCore::RequestType::L1Prefetch,
                     SDBPCore::RequestType::L2Prefetch}) {
        for (auto current : {SDBPCore::RequestType::Demand,
                             SDBPCore::RequestType::L1Prefetch,
                             SDBPCore::RequestType::L2Prefetch}) {
            SDBPCore::Config cfg;
            cfg.samplerAssoc = 1;
            cfg.crcLiveUpdate = false;
            SDBPCore core(cfg);
            const auto first = core.signature(0x1000);
            const auto second = core.signature(0x2000);
            core.train(first, true, old);
            core.train(first, true, old);
            core.access(0, 1, old, true, 0x1000);
            const auto hit = core.access(0, 1, current, true, 0x2000);
            EXPECT_TRUE(hit.hit);
            EXPECT_EQ(hit.previousType, old);
            EXPECT_EQ(hit.previousSignature, first);
            EXPECT_EQ(core.confidence(first, old), 3U);
            const auto evicted = core.access(0, 2, 0x3000);
            EXPECT_TRUE(evicted.evicted);
            EXPECT_EQ(evicted.previousType, current);
            EXPECT_EQ(evicted.previousSignature, second);
            EXPECT_EQ(core.confidence(second, current), 3U);
        }
    }
}

TEST(SDBPCore, NewSamplerSignatureUsesItsOwnSourcePrediction)
{
    for (auto source : {SDBPCore::RequestType::Demand,
                        SDBPCore::RequestType::L1Prefetch,
                        SDBPCore::RequestType::L2Prefetch}) {
        SDBPCore::Config cfg;
        cfg.samplerAssoc = 1;
        SDBPCore core(cfg);
        const auto old = source == SDBPCore::RequestType::Demand ?
            SDBPCore::RequestType::L1Prefetch : SDBPCore::RequestType::Demand;
        const auto signature = core.signature(0x1000);
        for (unsigned update = 0; update < 3; ++update)
            core.train(signature, true, source);
        core.access(0, 1, old, true, 0x1000);
        const auto hit = core.access(0, 1, source, true, 0x1000);
        EXPECT_FALSE(hit.previousDead);
        const auto victim = core.access(0, 2, 0x2000);
        EXPECT_TRUE(victim.previousDead);
        EXPECT_EQ(victim.previousType, source);
    }
}

TEST(SDBPCore, EverySourceReplacementTrainsOnlyTheVictimBank)
{
    for (auto old : {SDBPCore::RequestType::Demand,
                     SDBPCore::RequestType::L1Prefetch,
                     SDBPCore::RequestType::L2Prefetch}) {
        for (auto current : {SDBPCore::RequestType::Demand,
                             SDBPCore::RequestType::L1Prefetch,
                             SDBPCore::RequestType::L2Prefetch}) {
            SDBPCore::Config cfg;
            cfg.samplerAssoc = 1;
            SDBPCore core(cfg);
            const auto signature = core.signature(0x1000);
            core.access(0, 1, old, true, 0x1000);
            const auto evicted = core.access(0, 2, current, true, 0x2000);
            EXPECT_TRUE(evicted.evicted);
            EXPECT_EQ(evicted.previousType, old);
            EXPECT_EQ(evicted.previousSignature, signature);
            for (auto bank : {SDBPCore::RequestType::Demand,
                              SDBPCore::RequestType::L1Prefetch,
                              SDBPCore::RequestType::L2Prefetch}) {
                EXPECT_EQ(core.confidence(signature, bank),
                          bank == old ? 3U : 0U);
            }
        }
    }
}

TEST(SDBPCore, InvalidBeforeDeadThenLru)
{
    SDBPCore::Config cfg;
    cfg.samplerAssoc = 2;
    cfg.deadThreshold = 0;
    SDBPCore core(cfg);
    core.access(0, 1, 0x1000);
    EXPECT_FALSE(core.access(0, 2, 0x2000).evicted);
    // Both dead: choose first way even though it has just been touched.
    core.access(0, 1, 0x1000);
    EXPECT_EQ(core.access(0, 3, 0x3000).previousSignature, core.signature(0x1000));

    cfg.deadThreshold = 8;
    SDBPCore lru(cfg);
    lru.access(0, 1, 0x1000);
    lru.access(0, 2, 0x2000);
    lru.access(0, 1, 0x1000);
    EXPECT_EQ(lru.access(0, 3, 0x3000).previousSignature, lru.signature(0x2000));
}

TEST(SDBPCore, YoungestTwoGuardPreservesDeadCandidateOrder)
{
    for (unsigned ways : {6U, 7U, 8U}) {
        SDBPCore::Config cfg;
        cfg.samplerAssoc = ways;
        cfg.samplerProtectYoungest = 2;
        cfg.deadThreshold = 0;
        SDBPCore core(cfg);
        for (unsigned way = 0; way < ways; ++way)
            core.access(0, way + 1, 0x1000 + way * 0x100);
        core.access(0, 3, 0x1200);
        core.access(0, 1, 0x1000);
        core.access(0, 2, 0x1100);

        // Ways 1 and 0 are youngest. Way 2 is the first eligible dead,
        // although way 3 is older. Preserve original candidate ordering.
        const auto victim = core.access(0, ways + 1, 0x8000);
        EXPECT_EQ(victim.previousSignature, core.signature(0x1200));
        EXPECT_TRUE(victim.previousDead);
        for (unsigned way : {0U, 1U}) {
            const auto hit = core.access(0, way + 1, 0x1000 + way * 0x100);
            EXPECT_TRUE(hit.hit);
            EXPECT_TRUE(hit.previousDead);
        }
    }
}

TEST(SDBPCore, YoungestTwoGuardSkipsEitherProtectedDeadPosition)
{
    for (unsigned ways : {6U, 7U, 8U}) {
        for (unsigned age : {1U, 2U}) {
            for (bool protect : {false, true}) {
                SDBPCore::Config cfg;
                cfg.samplerAssoc = ways;
                cfg.samplerProtectYoungest = protect ? 2 : 0;
                SDBPCore core(cfg);
                const auto deadSignature = core.signature(0x8000);
                for (unsigned i = 0; i < 3; ++i)
                    core.train(deadSignature, true);
                for (unsigned way = 0; way < ways; ++way) {
                    const auto pc = way == ways - age ? 0x8000 : 0x1000 + way * 0x100;
                    core.access(0, way + 1, pc);
                }
                // With no eligible dead, replace the older live LRU entry.
                const auto victim = core.access(0, ways + 1, 0xa000);
                EXPECT_EQ(victim.previousSignature,
                          protect ? core.signature(0x1000) : deadSignature);
                EXPECT_EQ(victim.previousDead, !protect);
            }
        }
    }
}

TEST(SDBPCore, YoungestTwoGuardFallsBackWhenBothDeadEntriesAreProtected)
{
    for (unsigned ways : {6U, 7U, 8U}) {
        SDBPCore::Config cfg;
        cfg.samplerAssoc = ways;
        cfg.samplerProtectYoungest = 2;
        SDBPCore core(cfg);
        const auto deadSignature = core.signature(0x8000);
        for (unsigned i = 0; i < 3; ++i)
            core.train(deadSignature, true);
        for (unsigned way = 0; way < ways; ++way)
            core.access(0, way + 1, way >= ways - 2 ? 0x8000 : 0x1000 + way * 0x100);
        const auto victim = core.access(0, ways + 1, 0xa000);
        EXPECT_EQ(victim.previousSignature, core.signature(0x1000));
        EXPECT_FALSE(victim.previousDead);
        for (unsigned tag : {ways - 1, ways}) {
            const auto hit = core.access(0, tag, 0x8000);
            EXPECT_TRUE(hit.hit);
            EXPECT_TRUE(hit.previousDead);
        }
    }
}

TEST(SDBPCore, YoungestTwoGuardKeepsInvalidPriority)
{
    SDBPCore::Config cfg;
    cfg.samplerAssoc = 6;
    cfg.samplerProtectYoungest = 2;
    cfg.deadThreshold = 0;
    SDBPCore core(cfg);
    for (unsigned tag = 1; tag <= cfg.samplerAssoc; ++tag)
        EXPECT_FALSE(core.access(0, tag, 0x1000).evicted);
}

TEST(SDBPCore, YoungestTwoGuardRefreshesPrefetchHitRecency)
{
    SDBPCore::Config cfg;
    cfg.samplerAssoc = 6;
    cfg.samplerProtectYoungest = 2;
    cfg.deadThreshold = 0;
    SDBPCore core(cfg);
    for (unsigned way = 0; way < cfg.samplerAssoc; ++way)
        core.access(0, way + 1, 0x1000 + way * 0x100);
    core.access(0, 3, 0x1200);
    core.access(0, 1, 0x1000);
    core.access(0, 2, 0x1100);
    EXPECT_TRUE(core.access(0, 3, SDBPCore::RequestType::L1Prefetch, true, 0x9000).hit);
    const auto victim = core.access(0, 7, 0xa000);
    EXPECT_EQ(victim.previousSignature, core.signature(0x1000));
    const auto hit = core.access(0, 3, 0xa000);
    EXPECT_TRUE(hit.hit);
    EXPECT_EQ(hit.previousSignature, core.signature(0x9000));
    EXPECT_EQ(hit.previousType, SDBPCore::RequestType::L1Prefetch);
}

TEST(SDBPCore, YoungestTwoGuardAllowsGlobalLruFallbackInSmallSamplers)
{
    for (unsigned ways : {1U, 2U}) {
        SDBPCore::Config cfg;
        cfg.samplerAssoc = ways;
        cfg.samplerProtectYoungest = 2;
        cfg.deadThreshold = 0;
        SDBPCore core(cfg);
        for (unsigned way = 0; way < ways; ++way)
            core.access(0, way + 1, 0x1000 + way * 0x100);
        EXPECT_EQ(core.access(0, ways + 1, 0x8000).previousSignature, core.signature(0x1000));
    }
}

TEST(SDBPCore, NumericYoungestGuardAndPureLru)
{
    for (bool deadFirst : {false, true}) {
        for (unsigned guard : {0U, 1U, 2U, 3U, 6U, 8U}) {
            SDBPCore::Config cfg;
            cfg.samplerAssoc = 6;
            cfg.deadThreshold = 0;
            cfg.samplerDeadFirst = deadFirst;
            cfg.samplerProtectYoungest = guard;
            SDBPCore core(cfg);
            for (unsigned tag = 1; tag <= 6; ++tag)
                EXPECT_FALSE(core.access(0, tag, tag * 0x100).evicted);
            // Way order is the reverse of recency: tag 1 is youngest.
            for (unsigned tag = 6; tag >= 1; --tag)
                EXPECT_TRUE(core.access(0, tag, tag * 0x100).hit);
            const unsigned victim = deadFirst && guard < 6 ? guard + 1 : 6;
            const auto result = core.access(0, 7, 0x700);
            EXPECT_TRUE(result.evicted);
            EXPECT_EQ(result.previousSignature,
                      core.signature(victim * 0x100));
            // Choosing LRU must still train the evicted signature dead.
            EXPECT_EQ(core.confidence(result.previousSignature), 3);
        }
    }
}

TEST(SDBPCore, PartialTagsAlias)
{
    SDBPCore::Config cfg;
    cfg.partialTagBits = 3;
    SDBPCore core(cfg);
    core.access(0, 1, 0x1000);
    EXPECT_TRUE(core.access(0, 9, 0x2000).hit);
    EXPECT_TRUE(core.access(0, 1, 0x4000).hit);
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
        core.access(0, tag, 0x1000);
    EXPECT_TRUE(core.predict(signature));
    const auto confidence = core.confidence(signature);
    EXPECT_FALSE(core.access(1, 123, 0x1000).sampled);
    EXPECT_EQ(core.confidence(signature), confidence);
    const auto hit = core.access(0, 7, 0x1000);
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
