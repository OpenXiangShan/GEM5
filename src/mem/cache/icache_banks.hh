// Copyright (c) 2026 Beijing Institute of Open Source Chip (BOSC)
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __MEM_CACHE_ICACHE_BANKS_HH__
#define __MEM_CACHE_ICACHE_BANKS_HH__

#include <cassert>
#include <cstdint>
#include <map>
#include <vector>

namespace gem5::icache_banks
{

inline uint8_t
bankMask(uint64_t addr, unsigned bytes)
{
    assert(bytes <= 64 - addr % 64);
    if (!bytes)
        return 0;
    const unsigned first = (addr % 64) / 8;
    const unsigned last = (addr % 64 + bytes - 1) / 8;
    return ((1u << (last + 1)) - 1) & ~((1u << first) - 1);
}

// Read reservations are made after tag lookup, using the resident way.
// A bank/way can broadcast one set to multiple readers in the same cycle.
// Keep future reservations as well as this cycle's accesses so delayed
// requests contend with requests arriving in later CPU cycles.
class ReadScheduler
{
    struct Read
    {
        unsigned way;
        unsigned set;
        uint8_t mask;
    };
    std::map<uint64_t, std::vector<Read>> slots;

  public:
    uint64_t reserve(uint64_t now, uint64_t earliest, unsigned way,
                     unsigned set, uint8_t mask)
    {
        slots.erase(slots.begin(), slots.lower_bound(now));
        if (!mask)
            return 0;
        uint64_t cycle = earliest;
        for (;; ++cycle) {
            bool conflict = false;
            auto it = slots.find(cycle);
            if (it != slots.end()) {
                for (const auto &read : it->second) {
                    if (read.way == way && read.set != set &&
                        (read.mask & mask)) {
                        conflict = true;
                        break;
                    }
                }
            }
            if (!conflict) {
                slots[cycle].push_back({way, set, mask});
                return cycle - earliest;
            }
        }
    }
};

} // namespace gem5::icache_banks
#endif
