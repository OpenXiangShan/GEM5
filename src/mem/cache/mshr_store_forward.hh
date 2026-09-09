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

#ifndef __MEM_CACHE_MSHR_STORE_FORWARD_HH__
#define __MEM_CACHE_MSHR_STORE_FORWARD_HH__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5
{

struct MSHRStoreForwardData
{
    std::vector<uint8_t> data;
    std::vector<bool> valid;

    bool hasData() const;
    bool isFull() const;
};

struct MSHRStoreForwardSource
{
    Addr addr;
    size_t size;
    ContextID context;
    InstSeqNum seqNum;
    const uint8_t *data;
    const std::vector<bool> *byteEnable;
};

MSHRStoreForwardData selectMSHRStoreForwardData(
    Addr load_addr, size_t load_size, ContextID load_context,
    InstSeqNum load_seq,
    const std::vector<MSHRStoreForwardSource> &sources);

size_t applyMSHRStoreForwardData(
    uint8_t *load_data, const MSHRStoreForwardData &forwarding);

} // namespace gem5

#endif // __MEM_CACHE_MSHR_STORE_FORWARD_HH__
