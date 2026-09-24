/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_CORE_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_CORE_HH__

#include <cstdint>
#include <string>
#include <vector>

namespace gem5
{
namespace replacement_policy
{

/** Independent sampling partial-tag array and skewed dead-block predictor. */
class SDBPCore
{
  public:
    struct Config
    {
        unsigned numSets = 2048;
        unsigned samplerNum = 32;
        unsigned samplerAssoc = 12;
        unsigned predictorTables = 3;
        unsigned predictorEntries = 4096;
        unsigned counterBits = 2;
        unsigned deadThreshold = 8;
        unsigned partialTagBits = 15;
        unsigned partialPcBits = 15;
        unsigned pcShift = 1;
        uint64_t pcHashSeed = 0;
        std::string pcHashType = "xor_fold";
        std::string indexHashType = "mixed";
        std::vector<uint64_t> tableHashSeeds;
        bool crcLiveUpdate = true;
    };

    struct AccessResult
    {
        bool sampled = false;
        bool hit = false;
        bool evicted = false;
        bool previousDead = false;
        uint64_t previousSignature = 0;
    };

    explicit SDBPCore(const Config &config);
    unsigned numSets() const { return cfg.numSets; }
    unsigned samplerSets() const { return 1U << halfSetBits; }
    unsigned halfBits() const { return halfSetBits; }
    /** Return -1 for an unsampled set, otherwise a dense sampler index. */
    int sampleIndex(unsigned set) const;
    uint64_t signature(uint64_t pc) const;
    unsigned index(uint64_t signature, unsigned table) const;
    unsigned confidence(uint64_t signature) const;
    bool predict(uint64_t signature) const;
    void train(uint64_t signature, bool dead);
    AccessResult access(unsigned set, uint64_t tag, bool secure, uint64_t pc);

  private:
    struct Entry
    {
        uint64_t tag = 0;
        uint64_t signature = 0;
        uint64_t lastUse = 0;
        bool valid = false;
        bool secure = false;
        bool dead = false;
    };

    Config cfg;
    unsigned halfSetBits = 1;
    unsigned matchBits = 0;
    unsigned counterMax = 0;
    uint64_t sequence = 0;
    std::vector<std::vector<uint8_t>> tables;
    std::vector<std::vector<Entry>> sampler;
};

}  // namespace replacement_policy
}  // namespace gem5

#endif  // __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_CORE_HH__
