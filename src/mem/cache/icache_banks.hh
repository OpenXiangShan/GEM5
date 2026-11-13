// Copyright (c) 2026 Beijing Institute of Open Source Chip (BOSC)
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __MEM_CACHE_ICACHE_BANKS_HH__
#define __MEM_CACHE_ICACHE_BANKS_HH__

#include <cstdint>

namespace gem5::icache_banks
{

// A line fragment used only to probe the paired Data SRAM read constraints.
struct ReadRange
{
    uint64_t vaddr;
    uint64_t paddr;
    unsigned size;
    bool secure;
};

inline uint64_t
bankMask(uint64_t addr, unsigned size, unsigned line_bytes, unsigned bank_bytes)
{
    const unsigned first = (addr % line_bytes) / bank_bytes;
    const unsigned last = ((addr % line_bytes) + size - 1) / bank_bytes;
    const uint64_t high = last == 63 ? ~uint64_t(0) : (uint64_t(1) << (last + 1)) - 1;
    return high & ~((uint64_t(1) << first) - 1);
}

inline bool
conflict(uint64_t mask0, uint64_t mask1, uint64_t way0, uint64_t way1, unsigned set0, unsigned set1)
{
    // Match ICacheMainPipe, including equality of two zero (miss) waymasks.
    return (mask0 & mask1) && way0 == way1 && set0 != set1;
}

}  // namespace gem5::icache_banks
#endif
