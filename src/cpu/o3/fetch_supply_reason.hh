/*
 * Copyright (c) 2026 Mao Weiming
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

#ifndef __CPU_O3_FETCH_SUPPLY_REASON_HH__
#define __CPU_O3_FETCH_SUPPLY_REASON_HH__

#include <cstddef>
#include <vector>

namespace gem5
{
namespace o3
{

// Why the fetch buffer is short this cycle. Kept separate from Fetch so the
// accounting rules can be checked without constructing an O3 CPU.
enum class FetchSupplyCut
{
    None,
    Stream, // taken branch or the FTQ stream ran out
    Buf,    // PC walked off a still-valid fetch buffer
    Icache  // buffer invalid; may be a real I$ miss or a not-yet-translated refill
};

// Slot written into a stall-reason vector. Mapped onto StallReason in fetch.cc.
enum class SupplySlot
{
    ITlb,
    Icache,
    Stream,
    Buf,
    FetchFrag,
    OtherFetch,
    KeepAlready
};

// Zero instructions reached decode. checkMemoryNeeds() latches FetchCut::Icache
// as soon as the buffer is invalid, including every cycle an ITLB walk is still
// outstanding. The request status is the real reason; the cut is only a fallback.
inline SupplySlot
zeroFetchSupplySlot(bool tlbWait, FetchSupplyCut cut, bool alreadyNoStall)
{
    if (tlbWait)
        return SupplySlot::ITlb;
    switch (cut) {
      case FetchSupplyCut::Stream:
        return SupplySlot::Stream;
      case FetchSupplyCut::Buf:
        return SupplySlot::Buf;
      case FetchSupplyCut::Icache:
        return SupplySlot::Icache;
      case FetchSupplyCut::None:
        break;
    }
    return alreadyNoStall ? SupplySlot::OtherFetch : SupplySlot::KeepAlready;
}

// Unused slots after a partial transfer. A taken/FTQ cut still explains those
// slots. An I$ latch does not, while translation has not finished.
inline SupplySlot
partialFetchSupplySlot(bool tlbWait, FetchSupplyCut cut)
{
    if (tlbWait && (cut == FetchSupplyCut::None || cut == FetchSupplyCut::Icache))
        return SupplySlot::ITlb;
    switch (cut) {
      case FetchSupplyCut::Stream:
        return SupplySlot::Stream;
      case FetchSupplyCut::Buf:
        return SupplySlot::Buf;
      case FetchSupplyCut::Icache:
        return SupplySlot::Icache;
      case FetchSupplyCut::None:
        return SupplySlot::FetchFrag;
    }
    return SupplySlot::FetchFrag;
}

// Decode/rename clear their stall vector to NoStall at the start of tick().
// With no selected thread and no backend block, that clear used to be what
// dispatch counted: a drained I-cache miss became a full-width NoStall.
// Copy the upstream vector instead. A short source fills the tail with noStall.
template <typename Reason>
inline void
copyUpstreamStall(std::vector<Reason> &dst, const std::vector<Reason> &src,
                  Reason noStall)
{
    for (size_t i = 0; i < dst.size(); ++i)
        dst[i] = i < src.size() ? src[i] : noStall;
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FETCH_SUPPLY_REASON_HH__
