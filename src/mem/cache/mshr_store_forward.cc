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

#include "mem/cache/mshr_store_forward.hh"

#include <algorithm>
#include <cassert>

namespace gem5
{

bool
MSHRStoreForwardData::hasData() const
{
    return std::any_of(valid.begin(), valid.end(),
                       [](bool byte_valid) { return byte_valid; });
}

bool
MSHRStoreForwardData::isFull() const
{
    return !valid.empty() &&
        std::all_of(valid.begin(), valid.end(),
                    [](bool byte_valid) { return byte_valid; });
}

MSHRStoreForwardData
selectMSHRStoreForwardData(
    Addr load_addr, size_t load_size, ContextID load_context,
    InstSeqNum load_seq,
    const std::vector<MSHRStoreForwardSource> &sources)
{
    MSHRStoreForwardData result;
    result.data.resize(load_size);
    result.valid.resize(load_size, false);

    std::vector<InstSeqNum> producer_seq(load_size, 0);
    const Addr load_end = load_addr + load_size;
    for (const auto &source : sources) {
        if (source.context != load_context || source.seqNum >= load_seq) {
            continue;
        }

        const Addr store_end = source.addr + source.size;
        const Addr overlap_start = std::max(load_addr, source.addr);
        const Addr overlap_end = std::min(load_end, store_end);
        if (overlap_start >= overlap_end) {
            continue;
        }

        assert(source.data);
        assert(!source.byteEnable || source.byteEnable->empty() ||
               source.byteEnable->size() == source.size);
        for (Addr addr = overlap_start; addr < overlap_end; ++addr) {
            const size_t load_idx = addr - load_addr;
            const size_t store_idx = addr - source.addr;
            const bool byte_valid = !source.byteEnable ||
                source.byteEnable->empty() ||
                source.byteEnable->at(store_idx);
            if (byte_valid &&
                (!result.valid[load_idx] ||
                 source.seqNum >= producer_seq[load_idx])) {
                result.data[load_idx] = source.data[store_idx];
                result.valid[load_idx] = true;
                producer_seq[load_idx] = source.seqNum;
            }
        }
    }

    return result;
}

size_t
applyMSHRStoreForwardData(
    uint8_t *load_data, const MSHRStoreForwardData &forwarding)
{
    if (!forwarding.hasData()) {
        return 0;
    }

    assert(load_data);
    assert(forwarding.data.size() == forwarding.valid.size());
    size_t forwarded_bytes = 0;
    for (size_t i = 0; i < forwarding.valid.size(); ++i) {
        if (forwarding.valid[i]) {
            load_data[i] = forwarding.data[i];
            ++forwarded_bytes;
        }
    }
    return forwarded_bytes;
}

} // namespace gem5
