/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_FDP_LOGIC_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_FDP_LOGIC_HH__

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace gem5
{
namespace replacement_policy
{
namespace fdp
{

// Exact LRU stack: rank zero is MRU; invalid ways have no rank.
class Stack
{
  public:
    struct Line
    {
        unsigned rank = 0;
        bool valid = false;
        bool unusedPrefetch = false;
        bool demandFetched = false;
    };

    std::vector<Line> lines;

    explicit Stack(unsigned ways) : lines(ways) {}

    void
    invalidate(unsigned way)
    {
        auto &line = lines.at(way);
        if (line.valid) {
            for (auto &other : lines) {
                if (other.valid && other.rank > line.rank)
                    --other.rank;
            }
        }
        line.valid = false;
    }

    void
    insert(unsigned way, bool prefetch, unsigned rank)
    {
        invalidate(way);
        const unsigned valid = std::count_if(lines.begin(), lines.end(),
            [](const Line &line) { return line.valid; });
        rank = std::min(rank, valid);
        for (auto &line : lines) {
            if (line.valid && line.rank >= rank)
                ++line.rank;
        }
        lines[way] = {rank, true, prefetch, !prefetch};
    }

    void
    touch(unsigned way, bool demand)
    {
        auto &line = lines.at(way);
        assert(line.valid);
        const unsigned old_rank = line.rank;
        for (auto &other : lines) {
            if (other.valid && other.rank < old_rank)
                ++other.rank;
        }
        line.rank = 0;
        if (demand)
            line.unusedPrefetch = false;
    }
};

// Sections 3.1.3, 3.2 and 3.3.2 of Srinath et al., HPCA 2007.
class Feedback
{
  public:
    enum Position { Mid, Quarter, Lru };

  private:
    const unsigned blockBits;
    const unsigned filterBits;
    const uint64_t interval;
    const double lowThreshold;
    const double highThreshold;
    // Keep security domains independent while retaining the paper's hash.
    std::array<std::vector<bool>, 2> filter;
    const bool tagged;
    std::array<std::vector<uint64_t>, 2> tags;
    uint64_t aliases = 0;
    uint64_t demand = 0;
    uint64_t pollution = 0;
    uint64_t evictions = 0;
    double previousDemand = 0;
    double previousPollution = 0;
    Position position = Mid;

    unsigned
    index(uint64_t addr) const
    {
        const uint64_t block = addr >> blockBits;
        return (block ^ (block >> filterBits)) & (filter[0].size() - 1);
    }

  public:
    Feedback(unsigned block_bits, unsigned filter_bits, uint64_t period,
             double low, double high, bool use_tags = false)
      : blockBits(block_bits), filterBits(filter_bits), interval(period),
        lowThreshold(low), highThreshold(high), tagged(use_tags)
    {
        for (auto &domain : filter)
            domain.resize(uint64_t(1) << filterBits, false);
        if (tagged) {
            for (auto &domain : tags)
                domain.resize(uint64_t(1) << filterBits);
        }
    }

    bool
    demandMiss(uint64_t addr, bool secure)
    {
        ++demand;
        const unsigned slot = index(addr);
        const bool match = !tagged || tags[secure][slot] == (addr >> blockBits);
        const bool polluted = filter[secure][slot] && match;
        aliases += filter[secure][slot] && !match;
        pollution += polluted;
        // A tagged record describes one eviction, not all future misses.
        if (tagged && polluted)
            filter[secure][slot] = false;
        return polluted;
    }

    void
    prefetchFill(uint64_t addr, bool secure)
    {
        const unsigned slot = index(addr);
        if (!tagged || tags[secure][slot] == (addr >> blockBits))
            filter[secure][slot] = false;
    }

    void
    demandFill(uint64_t addr, bool secure)
    {
        // A new residency ends the lifetime of the previous eviction record.
        if (tagged)
            prefetchFill(addr, secure);
    }

    uint64_t rejectedAliases() const { return aliases; }

    bool
    evict(uint64_t addr, bool secure, bool demand_fetched, bool useful,
          bool incoming_prefetch)
    {
        if (demand_fetched && incoming_prefetch) {
            const unsigned slot = index(addr);
            filter[secure][slot] = true;
            if (tagged)
                tags[secure][slot] = addr >> blockBits;
        }
        if (!useful || ++evictions < interval)
            return false;

        previousDemand = (previousDemand + demand) / 2;
        previousPollution = (previousPollution + pollution) / 2;
        if (previousDemand > 0) {
            const double rate = previousPollution / previousDemand;
            position = rate < lowThreshold ? Mid :
                (rate > highThreshold ? Lru : Quarter);
        }
        demand = pollution = evictions = 0;
        return true;
    }

    Position getPosition() const { return position; }
    double getPollution() const
    {
        return previousDemand > 0 ? previousPollution / previousDemand : 0;
    }

    unsigned
    insertionRank(unsigned ways) const
    {
        // Positions are counted from LRU starting at one in the paper.
        switch (position) {
          case Mid:
            return ways - std::max(1U, ways / 2);
          case Quarter:
            return ways - std::max(1U, ways / 4);
          case Lru:
            return ways - 1;
        }
        return ways - 1;
    }
};

} // namespace fdp
} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_FDP_LOGIC_HH__
