/*
 * Copyright (c) 2012-2014 ARM Limited
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
 * Definitions of a conventional tag store.
 */

#include "mem/cache/tags/vipt_set_assoc.hh"

#include <string>

#include "base/intmath.hh"

namespace gem5
{

VIPTSetAssoc::VIPTSetAssoc(const Params &p)
    :BaseSetAssoc(p), pageShift(floorLog2(p.page_size))
    {
        assert(p.page_size % 2 == 0);
    }

CacheBlk*
VIPTSetAssoc::findBlock(Addr addr, bool is_secure) const
{
    // Extract block tag
    Addr tag = extractTag(addr);

    for (int i = 0; i < (1 << indexingPolicy->getAliasBits()); i++) {
        // Find possible entries that may contain the given physical address
        // Search each alias address
        Addr addrAlias = (addr & ~(((1 << indexingPolicy->getAliasBits()) - 1) << pageShift)) \
                         | (i << pageShift);

        const std::vector<ReplaceableEntry*> entries =
            indexingPolicy->getPossibleEntries(addrAlias);

        // Search for block
        for (const auto& location : entries) {
            CacheBlk* blk = static_cast<CacheBlk*>(location);
            int way = location->getWay();
            if (blk->matchTag(tag, is_secure)) {
                if ((blk->getWay() != way) && (blk->getWay() != DEFAULTWAYPRE))
                    panic("Unexpected way %d\n", blk->getWay());
                blk->setHitWay(way);
                return blk;
            }
        }
    }
    // Did not find block
    return nullptr;
}

CacheBlk*
VIPTSetAssoc::findVictim(PacketPtr pkt, const bool is_secure,
                         const std::size_t size,
                         std::vector<CacheBlk*>& evict_blks)
{
    // VIPT cache use the virtual address to find victim in corresponding set
    if (pkt->req->hasVaddr()) {
        return BaseSetAssoc::findVictim(pkt->req->getVaddr(), is_secure, size, evict_blks);
    } else {
        return BaseSetAssoc::findVictim(pkt->getAddr(), is_secure, size, evict_blks);
    }
}

} // namespace gem5
