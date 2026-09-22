/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_CACHE_PREFETCH_SMS_ORDER_HH__
#define __MEM_CACHE_PREFETCH_SMS_ORDER_HH__

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <vector>

namespace gem5
{
namespace prefetch
{
namespace sms
{

using OrderScore = uint16_t;

constexpr unsigned OrderFractionBits = 4;
constexpr unsigned OrderEwmaShift = 2;
constexpr OrderScore InvalidOrder = std::numeric_limits<OrderScore>::max();

inline unsigned
legacyOffset(uint64_t pending, bool decr_mode)
{
    assert(pending != 0);
    if (decr_mode) {
        return 63 - __builtin_clzll(pending);
    }
    return __builtin_ctzll(pending);
}

inline unsigned
selectOffset(uint64_t pending, const std::vector<OrderScore> &order_scores,
             unsigned region_blks)
{
    assert(pending != 0);
    assert(region_blks <= 64);

    unsigned best_offset = region_blks;
    OrderScore best_order = InvalidOrder;
    const unsigned limit = std::min<unsigned>(region_blks,
                                              order_scores.size());
    for (unsigned offset = 0; offset < limit; ++offset) {
        if (!(pending & (uint64_t(1) << offset))) {
            continue;
        }
        const OrderScore order = order_scores[offset];
        if (best_offset == region_blks || order < best_order) {
            best_offset = offset;
            best_order = order;
        }
    }

    // Entries always allocate one score per region block. Keep a deterministic
    // fallback for malformed or partially initialized metadata.
    if (best_offset == region_blks) {
        return __builtin_ctzll(pending);
    }
    return best_offset;
}

inline bool
hasBestOrderTie(uint64_t pending,
                const std::vector<OrderScore> &order_scores,
                unsigned region_blks, unsigned selected_offset)
{
    if (selected_offset >= order_scores.size()) {
        return false;
    }

    const OrderScore selected_order = order_scores[selected_offset];
    const unsigned limit = std::min<unsigned>(region_blks,
                                              order_scores.size());
    for (unsigned offset = 0; offset < limit; ++offset) {
        if (offset != selected_offset &&
            (pending & (uint64_t(1) << offset)) &&
            order_scores[offset] == selected_order) {
            return true;
        }
    }
    return false;
}

inline OrderScore
updateOrderScore(OrderScore old_score, bool valid, uint8_t touch_rank)
{
    const int sample = int(touch_rank) << OrderFractionBits;
    if (!valid) {
        return static_cast<OrderScore>(sample);
    }

    const int delta = sample - int(old_score);
    return static_cast<OrderScore>(int(old_score) +
        delta / (1 << OrderEwmaShift));
}

constexpr unsigned DefaultHighConfThreshold = 6;
constexpr unsigned DefaultMedConfThreshold = 4;
constexpr unsigned DefaultLowConfThreshold = 3;

// Map a PHT saturating counter to a prefetch destination.
// 1/2/3 => L1/L2/L3, 0 => do not send.
inline int
phtDestLevel(unsigned raw, bool is_trigger, unsigned high_thres,
             unsigned med_thres, unsigned low_thres)
{
    if (raw >= high_thres) {
        return is_trigger ? 1 : 2;
    }
    if (raw >= med_thres) {
        return is_trigger ? 2 : 3;
    }
    if (is_trigger && raw >= low_thres) {
        return 3;
    }
    return 0;
}

inline void
mergeNewOffsetOrders(uint64_t existing_bits, uint64_t incoming_bits,
                     const std::vector<OrderScore> &incoming_orders,
                     std::vector<OrderScore> &stored_orders,
                     unsigned region_blks)
{
    if (stored_orders.size() != region_blks) {
        stored_orders.assign(region_blks, InvalidOrder);
    }

    const uint64_t new_bits = incoming_bits & ~existing_bits;
    const unsigned limit = std::min<unsigned>(
        region_blks, std::min<unsigned>(incoming_orders.size(), 64));
    for (unsigned offset = 0; offset < limit; ++offset) {
        if (new_bits & (uint64_t(1) << offset)) {
            stored_orders[offset] = incoming_orders[offset];
        }
    }
}

} // namespace sms
} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_SMS_ORDER_HH__
