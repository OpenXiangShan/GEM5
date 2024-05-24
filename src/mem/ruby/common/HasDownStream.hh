/*
 * Copyright (c) 2024 Beijing Institute of Open Source Chip
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
 *
 * Author: Yaoyang Zhou
 *
 */
#ifndef __MEM_RUBY_COMMON_HASDOWNSTREAM_HH__
#define __MEM_RUBY_COMMON_HASDOWNSTREAM_HH__

#include "base/addr_range.hh"
#include "base/addr_range_map.hh"
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/common/NetDest.hh"

namespace gem5
{

namespace ruby
{

class HasDownStream
{
  protected:
    std::unordered_map<MachineType, AddrRangeMap<MachineID, 3>>
      downstreamAddrMap;

    NetDest downstreamDestinations;
  public:
    /**
     * Maps an address to the correct dowstream MachineID (i.e. the component
     * in the next level of the cache hierarchy towards memory)
     *
     * This function uses the local list of possible destinations instead of
     * querying the network.
     *
     * @param the destination address
     * @param the type of the destination (optional)
     * @return the MachineID of the destination
     */
    MachineID mapAddressToDownstreamMachine(Addr addr,
                                    MachineType mtype = MachineType_NUM) const
    {
        if (mtype == MachineType_NUM) {
            // map to the first match
            for (const auto &i : downstreamAddrMap) {
                const auto mapping = i.second.contains(addr);
                if (mapping != i.second.end())
                    return mapping->second;
            }
        } else {
            const auto i = downstreamAddrMap.find(mtype);
            if (i != downstreamAddrMap.end()) {
                const auto mapping = i->second.contains(addr);
                if (mapping != i->second.end())
                    return mapping->second;
            }
        }
        fatal("Couldn't find mapping for address %x mtype=%s\n", addr, mtype);
    }

    /** List of downstream destinations (towards memory) */
    const NetDest& allDownstreamDest() const { return downstreamDestinations; }
};
}
}

#endif // __MEM_RUBY_COMMON_HASDOWNSTREAM_HH__