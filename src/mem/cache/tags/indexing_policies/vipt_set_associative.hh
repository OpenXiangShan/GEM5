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
 * Declaration of a set associative indexing policy.
 */

#ifndef __MEM_CACHE_INDEXING_POLICIES_VIPT_SET_ASSOCIATIVE_HH__
#define __MEM_CACHE_INDEXING_POLICIES_VIPT_SET_ASSOCIATIVE_HH__

#include <cstdint>
#include <vector>

#include "base/intmath.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "params/VIPTSetAssociative.hh"

namespace gem5
{

class ReplaceableEntry;


/**
 * A vipt set associative indexing policy.
 * @sa  \ref gem5MemorySystem "gem5 Memory System"
 *
 * It's worth noting that vipt set associative uses virtual address as set index,
 * and is generally only used for L1 dcache.
 *
 * If the sum of set index bits and block offset bits in the virtual address exceeds the page offset bits,
 * the extra bits are alias bits. This means that the alias may differ between virtual and physical
 * addresses after address translation, and the block may exist in any of the alias sets.
 *
 * virtual address division(4KB page, 64KB cache block size):
 *   0000000000000000000000000  00     000000   000000
 *  ┌─────────────────────────┬──────────────┬─────────┐
 *  │          tag            │     set      │ offset  │
 *  ├─────────────────────────┴────┬─────────┴─────────┤
 *  │               vpn            │      offset       │
 *  └─────────────────────────┴────┴───────────────────┘
 *                            ┴──┬─┴
 *                               │ alias
 *                               └───────┘
 *
 * For example, let's assume virtual address A maps to set 0 on way 0. This policy
 * makes so that A is also mappable to set 0 on every other way or its alias set 4 on every other way.
 * Visually, the possible locations of A are, for a table with 4 ways and 8 sets with 1bit alias:
 *    Way 0   1   2   3
 *  Set   _   _   _   _
 *    0  |X| |X| |X| |X|
 *    1  |_| |_| |_| |_|
 *    2  |_| |_| |_| |_|
 *    3  |_| |_| |_| |_|
 *    4  |X| |X| |X| |X|
 *    5  |_| |_| |_| |_|
 *    6  |_| |_| |_| |_|
 *    7  |_| |_| |_| |_|
 *
 */
class VIPTSetAssociative : public SetAssociative
{
  protected:
    uint64_t aliasBits;

  public:
    /**
     * Convenience typedef.
     */
    typedef VIPTSetAssociativeParams Params;

    /**
     * Construct and initialize this policy.
     */
    VIPTSetAssociative(const Params &p);


    /**
     * Regenerate an entry's address from its tag and assigned set.
     *
     * @param tag The tag bits.
     * @param entry The entry.
     * @return the entry's original addr value.
     */
    Addr regenerateAddr(const Addr tag, const ReplaceableEntry* entry) const override;

    /**
     * Generate the tag from the given address.
     * VIPT stores all the physical address above page offset bits as tag.
     *
     * @param addr The address to get the tag from.
     * @return The tag of the address.
     */
    Addr extractTag(const Addr addr) const override;

    /**
     * Get the alias bits.
     *
     * @return The alias bits.
     */
    uint64_t getAliasBits() const override { return aliasBits; }
};

} // namespace gem5

#endif //__MEM_CACHE_INDEXING_POLICIES_VIPT_SET_ASSOCIATIVE_HH__
