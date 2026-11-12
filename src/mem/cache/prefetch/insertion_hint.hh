/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __MEM_CACHE_PREFETCH_INSERTION_HINT_HH__
#define __MEM_CACHE_PREFETCH_INSERTION_HINT_HH__

#include <cassert>
#include <cstdint>

#include "mem/request.hh"

namespace gem5
{
namespace prefetch
{

/** Attach a snapshot only when it describes the translated request's block. */
inline bool
attachInsertionHint(Request &request, const void *owner, uint8_t state,
                    Addr trained_address, bool virtual_address,
                    unsigned block_size)
{
    assert(block_size);
    if (!owner || (!virtual_address &&
        (!request.hasPaddr() || trained_address / block_size !=
                               request.getPaddr() / block_size))) {
        return false;
    }
    request.setInsertionHint(owner, state);
    return true;
}

} // namespace prefetch
} // namespace gem5
#endif
