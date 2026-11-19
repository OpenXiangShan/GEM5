/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __MEM_CACHE_PREFETCH_SMS_AGT_HH__
#define __MEM_CACHE_PREFETCH_SMS_AGT_HH__

#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>

#include "base/types.hh"

namespace gem5::prefetch
{

struct SmsAgtRegion
{
    Addr base;
    uint64_t bits;
};

// The RTL AGT adds/subtracts act_stride in cache-line units, then marks
// from that offset to the region boundary in the selected direction.
inline std::optional<SmsAgtRegion>
smsAgtRegion(Addr address, unsigned block_size, unsigned region_blocks,
             unsigned lookahead, bool decrement)
{
    assert(block_size && region_blocks && region_blocks <= 64);
    const Addr block = address / block_size;
    const Addr max_block = std::numeric_limits<Addr>::max() / block_size;
    if ((decrement && block < lookahead) ||
        (!decrement && max_block - block < lookahead)) {
        return std::nullopt;
    }
    const Addr target = decrement ? block - lookahead : block + lookahead;
    const unsigned offset = target % region_blocks;
    uint64_t bits = 0;
    for (unsigned i = 0; i < region_blocks; ++i) {
        if (decrement ? i <= offset : i >= offset)
            bits |= uint64_t(1) << i;
    }
    return SmsAgtRegion{(target - offset) * block_size, bits};
}

} // namespace gem5::prefetch

#endif // __MEM_CACHE_PREFETCH_SMS_AGT_HH__
