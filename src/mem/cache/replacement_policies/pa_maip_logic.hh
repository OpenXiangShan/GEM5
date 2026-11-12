/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_PA_MAIP_LOGIC_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_PA_MAIP_LOGIC_HH__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace gem5
{
namespace replacement_policy
{
namespace pa_maip
{

// UMOA, Section 3.3 and Table 2: first prefetch consumption is not reuse.
enum State : uint8_t { Init, Prefetched, Accessed, Reused };

inline State
nextState(State state, bool prefetch)
{
    if (prefetch) {
        return state == Init ? Prefetched : state;
    }
    return state == Accessed || state == Reused ? Reused : Accessed;
}

inline bool
highPriority(State state)
{
    return state == Accessed || state == Reused;
}

/** Bounded set-associative access maps; recency is independent of cache state. */
class AccessMap
{
  private:
    struct Entry
    {
        uint64_t zone = 0;
        uint64_t lastUse = 0;
        bool valid = false;
        bool secure = false;
        std::vector<State> states;
        explicit Entry(unsigned blocks) : states(blocks, Init) {}
    };
    const unsigned assoc;
    const unsigned numSets;
    const unsigned blockSize;
    const unsigned zoneSize;
    uint64_t sequence = 0;
    std::vector<Entry> entries;

  public:
    AccessMap(unsigned count, unsigned ways, unsigned zone_bytes,
              unsigned block_bytes)
      : assoc(ways), numSets(count / ways), blockSize(block_bytes),
        zoneSize(zone_bytes)
    {
        assert(count && ways && count % ways == 0);
        assert(zone_bytes && block_bytes && zone_bytes % block_bytes == 0);
        entries.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            entries.emplace_back(zone_bytes / block_bytes);
        }
    }

    State
    access(uint64_t address, bool secure, bool prefetch)
    {
        const uint64_t zone = address / zoneSize;
        const unsigned offset = (address % zoneSize) / blockSize;
        const unsigned first = (zone % numSets) * assoc;
        Entry *entry = nullptr;
        Entry *victim = &entries[first];
        for (unsigned way = 0; way < assoc; ++way) {
            auto &candidate = entries[first + way];
            if (candidate.valid && candidate.zone == zone &&
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
            entry->zone = zone;
            entry->secure = secure;
            entry->valid = true;
            std::fill(entry->states.begin(), entry->states.end(), Init);
        }
        entry->lastUse = ++sequence;
        const State previous = entry->states[offset];
        entry->states[offset] = nextState(previous, prefetch);
        return previous;
    }
};

/** A 10-bit DRRIP selector, sampled by disjoint cache-set leaders. */
class Dueling
{
  public:
    enum Team : uint8_t { Srrip, Brrip, Follower };

  private:
    unsigned lowBits = 0;
    unsigned mask = 0;
    unsigned selector = 512;

  public:
    explicit Dueling(unsigned num_sets)
    {
        assert(num_sets >= 4 && (num_sets & (num_sets - 1)) == 0);
        unsigned bits = 0;
        for (unsigned sets = num_sets; sets > 1; sets >>= 1) {
            bits++;
        }
        lowBits = bits / 2;
        mask = (1U << lowBits) - 1;
    }

    Team
    team(unsigned set) const
    {
        const unsigned low = set & mask;
        const unsigned high = (set >> lowBits) & mask;
        if (low == high) {
            return Srrip;
        }
        if (low == ((~high) & mask)) {
            return Brrip;
        }
        return Follower;
    }

    bool
    useBrrip(Team team) const
    {
        return team == Brrip || (team == Follower && selector >= 512);
    }

    void
    sample(Team team)
    {
        if (team == Srrip && selector < 1023) {
            selector++;
        } else if (team == Brrip && selector > 0) {
            selector--;
        }
    }

    unsigned value() const { return selector; }
};

struct Line
{
    uint8_t rrpv = 3;
    bool valid = false;
    bool unusedPrefetch = false;

    bool
    consumeDemand()
    {
        const bool first = unusedPrefetch;
        unusedPrefetch = false;
        return first;
    }

    // Return whether this hit promoted the line. MSHR consumption is recorded
    // separately, as an ordinary demand fill must retain its insertion RRPV.
    bool
    touch(bool prefetch, bool suppress_first)
    {
        if (prefetch) {
            return false;
        }
        const bool first = consumeDemand();
        if (first && suppress_first) {
            return false;
        }
        rrpv = 0;
        return true;
    }
};

} // namespace pa_maip
} // namespace replacement_policy
} // namespace gem5
#endif
