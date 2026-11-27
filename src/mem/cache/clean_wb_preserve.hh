/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of Sciences
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

#ifndef __MEM_CACHE_CLEAN_WB_PRESERVE_HH__
#define __MEM_CACHE_CLEAN_WB_PRESERVE_HH__

#include <array>

#include "mem/packet.hh"

namespace gem5
{

enum class CleanWbAction
{
    SendUncached,
    SendPreservedBlockCached,
    Drop,
};

enum class CleanWbProbeKind
{
    NotCachedAbove,
    L1IOnly,
    L1IAndL1DClean,
    L1DCleanOnly,
    DirtyAbove,
    WbCleanOnly,
    OpaqueCachedAbove,
};

/**
 * Carries upper-cache hit provenance collected while probing upward in
 * Cache::isCachedAbove() for a WritebackClean eviction.
 */
struct CleanWbProbeState : public Packet::SenderState
{
    bool cachedInL1I = false;
    bool cachedInOtherNonWbClean = false;
    bool cachedInWbClean = false;
    bool cachedDirtyAbove = false;
    bool cachedOpaqueAbove = false;

    void
    recordUpperHit(bool is_l1i, bool writes_back_clean, bool is_dirty)
    {
        if (is_dirty) {
            cachedDirtyAbove = true;
        } else if (writes_back_clean) {
            cachedInWbClean = true;
        } else if (is_l1i) {
            cachedInL1I = true;
        } else {
            cachedInOtherNonWbClean = true;
        }
    }

    void
    recordOpaqueHit()
    {
        cachedOpaqueAbove = true;
    }

    bool
    anyCachedAbove() const
    {
        return cachedInL1I || cachedInOtherNonWbClean ||
               cachedInWbClean || cachedDirtyAbove || cachedOpaqueAbove;
    }

    CleanWbProbeKind
    classify(bool cached_above) const
    {
        if (!cached_above) {
            return CleanWbProbeKind::NotCachedAbove;
        }
        if (cachedDirtyAbove) {
            return CleanWbProbeKind::DirtyAbove;
        }
        if (cachedOpaqueAbove) {
            return CleanWbProbeKind::OpaqueCachedAbove;
        }
        if (cachedInL1I && cachedInOtherNonWbClean) {
            return CleanWbProbeKind::L1IAndL1DClean;
        }
        if (cachedInL1I) {
            return CleanWbProbeKind::L1IOnly;
        }
        if (cachedInOtherNonWbClean) {
            return CleanWbProbeKind::L1DCleanOnly;
        }
        if (cachedInWbClean) {
            return CleanWbProbeKind::WbCleanOnly;
        }
        return CleanWbProbeKind::OpaqueCachedAbove;
    }
};

struct CleanWbDecision
{
    CleanWbAction action = CleanWbAction::Drop;
    CleanWbProbeKind kind = CleanWbProbeKind::NotCachedAbove;
};

/**
 * Decide how a cache with writeback_clean=True should handle an evicted
 * WritebackClean packet when upper caches may still retain a copy.
 *
 * Clean copies in writeback_clean=False caches (such as L1I or clean L1D
 * lines) only emit data-less CleanEvict packets upon later eviction, so
 * dropping WritebackClean prevents those lines from populating a downstream
 * mostly_excl cache (L3). Preserving WritebackClean with BLOCK_CACHED lets
 * clean data install in L3 while keeping the L2 snoop-filter holder bit
 * intact until upper copies evict. Dirty upper copies must never trigger
 * preservation because L2's clean data is stale and L1D will emit
 * WritebackDirty.
 */
inline CleanWbDecision
decidePreservedWritebackClean(bool preserve_enabled, bool cached_above,
                              const CleanWbProbeState *probe)
{
    if (!cached_above) {
        return {CleanWbAction::SendUncached,
                CleanWbProbeKind::NotCachedAbove};
    }
    if (!preserve_enabled || !probe) {
        return {CleanWbAction::Drop, CleanWbProbeKind::OpaqueCachedAbove};
    }

    const CleanWbProbeKind kind = probe->classify(cached_above);
    switch (kind) {
      case CleanWbProbeKind::L1IOnly:
      case CleanWbProbeKind::L1IAndL1DClean:
      case CleanWbProbeKind::L1DCleanOnly:
        return {CleanWbAction::SendPreservedBlockCached, kind};
      case CleanWbProbeKind::DirtyAbove:
      case CleanWbProbeKind::WbCleanOnly:
      case CleanWbProbeKind::OpaqueCachedAbove:
      case CleanWbProbeKind::NotCachedAbove:
      default:
        return {CleanWbAction::Drop, kind};
    }
}

enum CleanWbStatIdx
{
    cleanWbPreservedL1IOnly = 0,
    cleanWbPreservedL1IAndL1DClean,
    cleanWbPreservedL1DCleanOnly,
    cleanWbDroppedDirtyAbove,
    cleanWbDroppedWbCleanOnly,
    cleanWbDroppedOpaque,
    cleanWbStatCount
};

inline constexpr std::array<const char *, cleanWbStatCount>
    CleanWbStatNames{{
        "cleanWbPreservedL1IOnly",
        "cleanWbPreservedL1IAndL1DClean",
        "cleanWbPreservedL1DCleanOnly",
        "cleanWbDroppedDirtyAbove",
        "cleanWbDroppedWbCleanOnly",
        "cleanWbDroppedOpaque",
    }};

/**
 * Pure helper mirroring CoherentXBar::sinkPacket's clean-eviction sink gate.
 * CleanEvict carrying BLOCK_CACHED is always sunk, while WritebackClean
 * carrying BLOCK_CACHED is only forwarded when sink_cached_writeback_clean
 * is false AND the packet was explicitly tagged PRESERVE_CLEAN_WB by sender.
 */
inline bool
shouldSinkCleanEviction(bool is_block_cached, bool is_clean_evict,
                        bool is_writeback_clean,
                        bool is_preserve_clean_wb,
                        bool sink_cached_writeback_clean)
{
    return is_block_cached &&
        (is_clean_evict ||
         (is_writeback_clean &&
          (sink_cached_writeback_clean || !is_preserve_clean_wb)));
}

} // namespace gem5

#endif // __MEM_CACHE_CLEAN_WB_PRESERVE_HH__
