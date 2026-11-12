/*
 * Copyright (c) 2026 XiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_KPC_SAMPLER_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_KPC_SAMPLER_HH__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <vector>

namespace gem5
{
namespace replacement_policy
{

/** Independent true-LRU shadow cache used by KPC-R (ASPLOS 2017, sec. 3.2).
 *
 * The actual cache's RRIP evictions never remove or train sampler entries.
 * Entries retain their allocation type across all subsequent request types.
 */
class KPCSampler
{
  public:
    enum class Training
    {
        None, DemandReuse, PrefetchReuse, DemandDead, PrefetchDead
    };
    struct Result
    {
        bool sampled = false;
        bool hit = false;
        Training training = Training::None;
    };

  private:
    struct Entry
    {
        uint64_t block = 0;
        bool secure = false;
        bool prefetch = false;
        bool used = false;
        bool valid = false;
    };

    const unsigned ways;
    const unsigned maximum;
    unsigned demand;
    unsigned prefetch;
    std::vector<int> sampleIndex;
    // MRU first, LRU last. Invalid entries naturally remain at the tail.
    std::vector<Entry> entries;

    static uint32_t
    mix(uint32_t value)
    {
        value ^= value >> 16;
        value *= 0x7feb352dU;
        value ^= value >> 15;
        value *= 0x846ca68bU;
        return value ^ (value >> 16);
    }

  public:
    KPCSampler(unsigned sets, unsigned num_ways, unsigned sampled_sets,
               unsigned bits, unsigned initial)
      : ways(num_ways), maximum((1U << bits) - 1), demand(initial),
        prefetch(initial), sampleIndex(sets, -1),
        entries(sampled_sets * num_ways)
    {
        assert(sets && ways && sampled_sets && sampled_sets <= sets);
        assert(bits && bits <= 16 && initial <= maximum);
        // Fixed pseudo-random selection, independent of gem5's random stream
        // and of library-specific shuffle implementations.
        std::vector<unsigned> order(sets);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [](unsigned a, unsigned b) {
            return mix(a ^ 0x4b504352U) < mix(b ^ 0x4b504352U);
        });
        for (unsigned i = 0; i < sampled_sets; ++i) {
            sampleIndex[order[i]] = i;
        }
    }

    bool sampled(unsigned set) const { return sampleIndex.at(set) >= 0; }
    unsigned hysteresis(bool pf) const { return pf ? prefetch : demand; }
    uint8_t insertionRRPV(bool pf) const
    {
        return hysteresis(pf) == maximum ? 3 : 2;
    }

    Result
    access(unsigned set, uint64_t block, bool secure, bool pf)
    {
        const int index = sampleIndex.at(set);
        if (index < 0) {
            return {};
        }
        auto first = entries.begin() + index * ways;
        auto last = first + ways;
        auto hit = std::find_if(first, last, [=](const Entry &e) {
            return e.valid && e.block == block && e.secure == secure;
        });
        Result result{true, hit != last, Training::None};
        Entry next;
        if (hit != last) {
            next = *hit;
            if (!next.used) {
                unsigned &counter = next.prefetch ? prefetch : demand;
                if (counter) {
                    --counter;
                }
                result.training = next.prefetch ? Training::PrefetchReuse :
                                                 Training::DemandReuse;
                next.used = true;
            }
        } else {
            hit = last - 1;
            if (hit->valid && !hit->used) {
                unsigned &counter = hit->prefetch ? prefetch : demand;
                counter = std::min(counter + 1, maximum);
                result.training = hit->prefetch ? Training::PrefetchDead :
                                                 Training::DemandDead;
            }
            next = {block, secure, pf, false, true};
        }
        while (hit != first) {
            *hit = *(hit - 1);
            --hit;
        }
        *first = next;
        return result;
    }
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_KPC_SAMPLER_HH__
