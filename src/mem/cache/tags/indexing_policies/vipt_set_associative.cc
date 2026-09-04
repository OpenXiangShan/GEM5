/*
 * Copyright (c) 2018 Inria
 * Copyright (c) 2012-2014,2017 ARM Limited
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2003-2005,2014 The Regents of The University of Michigan
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

/**
 * @file
 * Definitions of a vipt set associative indexing policy.
 */

#include "mem/cache/tags/indexing_policies/vipt_set_associative.hh"

#include <cassert>

#include "base/logging.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"

namespace gem5
{

namespace
{

// XiangShan's hashBitPairs(vaddr, hi=47, lo=12, step=2).
constexpr unsigned DCACHE_HASH_LOW_BIT = 12;
constexpr unsigned DCACHE_HASH_HIGH_BIT = 47;
constexpr unsigned DCACHE_HASH_WIDTH = 2;
constexpr uint32_t DCACHE_HASH_MASK = (1U << DCACHE_HASH_WIDTH) - 1;

uint32_t
hash_dcache_alias(const Addr addr)
{
    uint32_t hash = 0;
    for (unsigned bit = DCACHE_HASH_LOW_BIT; bit <= DCACHE_HASH_HIGH_BIT;
         bit += DCACHE_HASH_WIDTH) {
        hash ^= static_cast<uint32_t>((addr >> bit) & DCACHE_HASH_MASK);
    }
    return hash;
}

} // anonymous namespace

VIPTSetAssociative::VIPTSetAssociative(const Params &p)
    : SetAssociative(p), useHashIndex(p.use_hash_index)
{
    assert(sliceShift == 0);
    if (tagShift > floorLog2(p.page_size)) {
        aliasBits = tagShift - floorLog2(p.page_size);
    } else {
        aliasBits = 0;
    }

    assert(tagShift > aliasBits);
    assert(p.page_size % 2 == 0);

    // The RTL function hashes two-bit lanes from vaddr[47:12].  Refuse an
    // unsupported geometry instead of silently using a different policy.
    fatal_if(useHashIndex && p.page_size != (1U << DCACHE_HASH_LOW_BIT),
             "VIPT hashed index requires a 4 KiB page, got %d bytes",
             p.page_size);
    fatal_if(useHashIndex && setShift != 6,
             "VIPT hashed index requires 64 B cache blocks, got %u B",
             1U << setShift);
    fatal_if(useHashIndex && aliasBits > DCACHE_HASH_WIDTH,
             "VIPT hashed index provides %u alias bits, but this cache "
             "requires %llu",
             DCACHE_HASH_WIDTH,
             static_cast<unsigned long long>(aliasBits));
}

uint32_t
VIPTSetAssociative::extractSet(const Addr addr) const
{
    if (!useHashIndex || aliasBits == 0) {
        return SetAssociative::extractSet(addr);
    }

    // Keep the page-offset (non-alias) part of the set index unchanged, and
    // replace only the virtual-page-dependent alias part with the RTL hash.
    const uint32_t directSet = SetAssociative::extractSet(addr);
    const uint32_t nonAliasMask = setMask >> aliasBits;
    const uint32_t aliasMask = (1U << aliasBits) - 1;
    const uint32_t aliasShift = tagShift - aliasBits - setShift;
    const uint32_t hashedAlias = hash_dcache_alias(addr) & aliasMask;

    return (directSet & nonAliasMask) | (hashedAlias << aliasShift);
}

Addr
VIPTSetAssociative::regenerateAddr(const Addr tag, const ReplaceableEntry* entry) const
{
    return (tag << (tagShift - aliasBits)) | ((entry->getSet() & (setMask >> aliasBits)) << setShift);
}

Addr
VIPTSetAssociative::extractTag(const Addr addr) const
{
    return addr >> (tagShift - aliasBits);
}

} // namespace gem5
