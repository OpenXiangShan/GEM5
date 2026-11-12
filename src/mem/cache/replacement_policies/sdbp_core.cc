/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The optional JWAC hash is adapted from Daniel A. Jimenez's 2010 Cache
 * Replacement Championship submission. That software grants everyone
 * permission to copy, modify, and/or redistribute it.
 */

#include "mem/cache/replacement_policies/sdbp_core.hh"

#include <algorithm>
#include <cassert>
#include <set>
#include <stdexcept>

namespace gem5
{
namespace replacement_policy
{
namespace
{

bool
powerOfTwo(unsigned value)
{
    return value && !(value & (value - 1));
}

uint64_t
mask(unsigned bits)
{
    return (uint64_t(1) << bits) - 1;
}

uint64_t
mix64(uint64_t value)
{
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

uint32_t
jwacMix(uint32_t a, uint32_t b, uint32_t c)
{
    a -= b;
    a -= c;
    a ^= c >> 13;
    b -= c;
    b -= a;
    b ^= a << 8;
    c -= a;
    c -= b;
    c ^= b >> 13;
    return c;
}

}  // anonymous namespace

SDBPCore::SDBPCore(const Config &config) : cfg(config)
{
    auto require = [](bool condition, const char *message) {
        if (!condition)
            throw std::invalid_argument(message);
    };
    require(powerOfTwo(cfg.numSets) && cfg.numSets >= 4, "num_sets must be a power of two >= 4");
    if (cfg.enableSampler) {
        require(powerOfTwo(cfg.samplerNum) && cfg.samplerNum >= 2 && cfg.samplerNum <= cfg.numSets,
                "sampler_num must be a power of two in [2, num_sets]");
        require(cfg.samplerAssoc >= 1, "sampler_assoc must be positive");
    }
    require(cfg.predictorTables >= 1 && cfg.predictorTables <= 32, "predictor_tables must be in [1, 32]");
    require(powerOfTwo(cfg.predictorEntries) && cfg.predictorEntries >= 2,
            "predictor_entries must be a power of two >= 2");
    require(cfg.counterBits >= 1 && cfg.counterBits <= 8, "counter_bits must be in [1, 8]");
    counterMax = (1U << cfg.counterBits) - 1;
    require(cfg.deadThreshold <= cfg.predictorTables * counterMax, "dead_threshold exceeds maximum confidence");
    require(cfg.partialTagBits >= 1 && cfg.partialTagBits <= 63 && cfg.partialPcBits >= 1 && cfg.partialPcBits <= 63,
            "partial tag and PC widths must be in [1, 63]");
    require(cfg.pcShift <= 63, "pc_shift must be in [0, 63]");
    require(cfg.pcHashType == "xor_fold" || cfg.pcHashType == "mixed" || cfg.pcHashType == "low_bits",
            "unknown pc_hash_type");
    require(cfg.indexHashType == "mixed" || cfg.indexHashType == "jwac", "unknown index_hash_type");
    if (cfg.indexHashType == "jwac") {
        require(cfg.partialPcBits <= 32 && cfg.tableHashSeeds.empty(),
                "jwac requires <=32 signature bits and no table seeds");
    } else if (cfg.tableHashSeeds.empty()) {
        for (unsigned t = 0; t < cfg.predictorTables; ++t)
            cfg.tableHashSeeds.push_back(0x9e3779b97f4a7c15ULL * (t + 1));
    } else {
        require(cfg.tableHashSeeds.size() == cfg.predictorTables,
                "table_hash_seeds must have predictor_tables entries");
        std::set<uint64_t> seeds(cfg.tableHashSeeds.begin(), cfg.tableHashSeeds.end());
        require(seeds.size() == cfg.tableHashSeeds.size(), "table_hash_seeds must be distinct");
    }

    if (cfg.enableSampler) {
        unsigned set_bits = 0;
        for (unsigned sets = cfg.numSets; sets > 1; sets >>= 1)
            ++set_bits;
        halfSetBits = 0;
        for (unsigned sets = cfg.samplerNum; sets > 1; sets >>= 1)
            ++halfSetBits;
        matchBits = set_bits - halfSetBits;
    }
    for (auto &bank : tables) {
        bank.assign(cfg.predictorTables,
                    std::vector<uint8_t>(cfg.predictorEntries, 0));
    }
    if (cfg.enableSampler)
        sampler.assign(samplerSets(), std::vector<Entry>(cfg.samplerAssoc));
}

int
SDBPCore::sampleIndex(unsigned set) const
{
    assert(set < cfg.numSets);
    if (!cfg.enableSampler)
        return -1;
    // Fields may overlap. Equality makes each low h-bit suffix unique.
    if ((set >> halfSetBits) != (set & mask(matchBits)))
        return -1;
    return set & (samplerSets() - 1);
}

uint64_t
SDBPCore::signature(uint64_t pc) const
{
    uint64_t value = (pc >> cfg.pcShift) ^ cfg.pcHashSeed;
    if (cfg.pcHashType == "mixed") {
        value = mix64(value);
    } else if (cfg.pcHashType == "xor_fold") {
        uint64_t folded = 0;
        do {
            folded ^= value & mask(cfg.partialPcBits);
            value >>= cfg.partialPcBits;
        } while (value);
        value = folded;
    }
    return value & mask(cfg.partialPcBits);
}

unsigned
SDBPCore::index(uint64_t signature, unsigned table) const
{
    assert(table < cfg.predictorTables);
    uint64_t hash;
    if (cfg.indexHashType == "jwac") {
        const uint32_t first = jwacMix(0xfeedface, 0xdeadb10c, signature);
        const uint32_t second = jwacMix(0xc001d00d, 0xfade2b1c, signature);
        hash = uint32_t(first + (second >> table));
    } else {
        hash = mix64(signature ^ cfg.tableHashSeeds[table]);
    }
    return hash & (cfg.predictorEntries - 1);
}

unsigned
SDBPCore::confidence(uint64_t signature, RequestType type) const
{
    const auto source = static_cast<unsigned>(type);
    assert(source < NumRequestTypes);
    unsigned sum = 0;
    for (unsigned t = 0; t < cfg.predictorTables; ++t)
        sum += tables[source][t][index(signature, t)];
    return sum;
}

bool
SDBPCore::predict(uint64_t signature, RequestType type) const
{
    return confidence(signature, type) >= cfg.deadThreshold;
}

void
SDBPCore::train(uint64_t signature, bool dead, RequestType type)
{
    const auto source = static_cast<unsigned>(type);
    assert(source < NumRequestTypes);
    for (unsigned t = 0; t < cfg.predictorTables; ++t) {
        auto &counter = tables[source][t][index(signature, t)];
        if (dead) {
            if (counter < counterMax)
                ++counter;
        } else if (cfg.crcLiveUpdate && (t & 1)) {
            counter >>= 1;
        } else if (counter) {
            --counter;
        }
    }
}

SDBPCore::AccessResult
SDBPCore::access(unsigned set, uint64_t tag, RequestType type,
                 bool has_pc, uint64_t pc)
{
    AccessResult result;
    // Filter before lookup so rejected requests cannot train or alter LRU.
    if (!cfg.enableSampler || !has_pc ||
        (cfg.samplerDemandPcOnly && type != RequestType::Demand)) {
        return result;
    }
    const int sample = sampleIndex(set);
    if (sample < 0)
        return result;
    result.sampled = true;
    auto &entries = sampler[sample];
    tag &= mask(cfg.partialTagBits);
    Entry *selected = nullptr;
    for (auto &entry : entries) {
        if (entry.valid && entry.tag == tag) {
            selected = &entry;
            result.hit = true;
            break;
        }
    }
    if (!selected) {
        for (auto &entry : entries) {
            if (!entry.valid) {
                selected = &entry;
                break;
            }
        }
        if (!selected && cfg.samplerDeadFirst) {
            // Protect the N youngest positions across all entries, preserving
            // original way order among eligible dead candidates. Global LRU
            // remains the fallback, even when every position is protected.
            for (auto &entry : entries) {
                if (!entry.dead)
                    continue;
                if (cfg.samplerProtectYoungest) {
                    const auto younger = std::count_if(
                        entries.begin(), entries.end(),
                        [&entry](const Entry &other) {
                            return other.lastUse > entry.lastUse;
                        });
                    if (younger < cfg.samplerProtectYoungest)
                        continue;
                }
                selected = &entry;
                break;
            }
        }
        if (!selected) {
            selected = &*std::min_element(entries.begin(), entries.end(),
                                          [](const Entry &a, const Entry &b) { return a.lastUse < b.lastUse; });
        }
        result.evicted = selected->valid;
    }

    if (selected->valid) {
        result.previousDead = selected->dead;
        result.previousSignatureValid = selected->signatureValid;
        result.previousSignature = selected->signature;
        result.previousType = selected->type;
        if (selected->signatureValid)
            train(selected->signature, !result.hit, selected->type);
    }

    selected->valid = true;
    selected->tag = tag;
    selected->type = type;
    selected->signatureValid = has_pc;
    if (selected->signatureValid) {
        selected->signature = signature(pc);
        selected->dead = predict(selected->signature, type);
    } else {
        selected->dead = false;
    }
    selected->lastUse = ++sequence;
    return result;
}

}  // namespace replacement_policy
}  // namespace gem5
