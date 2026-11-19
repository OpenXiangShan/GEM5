/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_PA_MAIP_REUSE_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_PA_MAIP_REUSE_HH__

#include <cassert>
#include <cstdint>
#include <vector>

namespace gem5
{
namespace replacement_policy
{
namespace pa_maip
{

/** Predict a second demand use after the first consumption of a prefetch.
 * An independent sampled LRU observes requests, including real-cache misses.
 * Negative feedback therefore does not come from this policy's own victims.
 * Its finite sampled lifetime is a training horizon, not future knowledge.
 */
class FirstUsePredictor
{
  public:
    enum Prediction : uint8_t { Unknown, Dead, Reuse };
    struct Feedback
    {
        bool sampled = false;
        bool positive = false;
        bool negative = false;
        bool unused = false;
        Prediction evaluated = Unknown;
        bool correct = false;
    };

  private:
    struct Counter
    {
        uint8_t reuse = 1;
        uint8_t samples = 0;
    };
    struct Sample
    {
        uint64_t block = 0;
        uint64_t lastUse = 0;
        unsigned signature = 0;
        uint8_t demands = 0;
        bool valid = false;
        bool secure = false;
        bool prefetchOrigin = false;
        Prediction firstPrediction = Unknown;
    };
    static constexpr unsigned Ways = 4;
    std::vector<Counter> counters;
    std::vector<Sample> sampler;
    const unsigned samplePeriod;
    const unsigned blockSize;
    const unsigned zoneSize;
    uint64_t sequence = 0;

    static uint64_t
    mix(uint64_t value)
    {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33;
        value *= 0xc4ceb9fe1a85ec53ULL;
        return value ^ (value >> 33);
    }

    unsigned
    signature(uint64_t address, bool secure) const
    {
        const uint64_t domain = secure ? 0x9e3779b97f4a7c15ULL : 0;
        return mix((address / zoneSize) ^ domain) % counters.size();
    }

    Prediction
    predictIndex(unsigned index) const
    {
        const auto &counter = counters[index];
        if (counter.samples < 3) {
            return Unknown;
        }
        return counter.reuse == 0 ? Dead :
               counter.reuse == 3 ? Reuse : Unknown;
    }

    void
    train(unsigned index, bool reuse)
    {
        auto &counter = counters[index];
        if (counter.samples < 3) {
            counter.samples++;
        }
        if (reuse && counter.reuse < 3) {
            counter.reuse++;
        } else if (!reuse && counter.reuse > 0) {
            counter.reuse--;
        }
    }

  public:
    FirstUsePredictor(unsigned entries = 1024, unsigned sample_entries = 128,
                      unsigned sample_period = 256, unsigned block_size = 64,
                      unsigned zone_size = 4096)
      : counters(entries), sampler(sample_entries),
        samplePeriod(sample_period), blockSize(block_size), zoneSize(zone_size)
    {
        assert(entries && sample_entries >= Ways &&
               sample_entries % Ways == 0 && sample_period &&
               block_size && zone_size >= block_size);
    }

    Prediction
    predict(uint64_t address, bool secure) const
    {
        return predictIndex(signature(address, secure));
    }

    Feedback
    observe(uint64_t address, bool secure, bool prefetch)
    {
        Feedback result;
        const uint64_t block = address / blockSize;
        const uint64_t hashed = mix(block);
        if (hashed % samplePeriod) {
            return result;
        }
        result.sampled = true;
        const unsigned set = (hashed / samplePeriod) %
                             (sampler.size() / Ways);
        Sample *entry = nullptr;
        Sample *victim = &sampler[set * Ways];
        for (unsigned way = 0; way < Ways; ++way) {
            auto &candidate = sampler[set * Ways + way];
            if (candidate.valid && candidate.block == block &&
                candidate.secure == secure) {
                entry = &candidate;
                break;
            }
            if (!candidate.valid ||
                (victim->valid && candidate.lastUse < victim->lastUse)) {
                victim = &candidate;
            }
        }
        if (!entry) {
            entry = victim;
            if (entry->valid && entry->prefetchOrigin) {
                if (entry->demands == 1) {
                    train(entry->signature, false);
                    result.negative = true;
                    result.evaluated = entry->firstPrediction;
                    result.correct = result.evaluated == Dead;
                } else if (entry->demands == 0) {
                    // An unused sample gives no evidence about second use.
                    result.unused = true;
                }
            }
            *entry = Sample();
            entry->valid = true;
            entry->block = block;
            entry->secure = secure;
            entry->prefetchOrigin = prefetch;
            entry->signature = signature(address, secure);
        }
        entry->lastUse = ++sequence;
        if (!prefetch && entry->prefetchOrigin && entry->demands < 2) {
            if (entry->demands == 0) {
                entry->firstPrediction = predictIndex(entry->signature);
            }
            if (++entry->demands == 2) {
                train(entry->signature, true);
                result.positive = true;
                result.evaluated = entry->firstPrediction;
                result.correct = result.evaluated == Reuse;
            }
        }
        return result;
    }
};

} // namespace pa_maip
} // namespace replacement_policy
} // namespace gem5
#endif
