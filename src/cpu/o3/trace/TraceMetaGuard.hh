/*
 * Copyright (c) 2026 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the disclaimer; redistributions in
 * binary form must reproduce the above copyright notice, this list of
 * conditions and the disclaimer in the documentation and/or other materials
 * provided with the distribution; neither the name of the copyright holders
 * nor the names of any contributors may be used to endorse or promote
 * products derived from this software without specific written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_TRACE_TRACE_META_GUARD_HH__
#define __CPU_O3_TRACE_TRACE_META_GUARD_HH__

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gem5
{
namespace o3
{

/**
 * Pure decision functions for the commit-side trace metadata cleanup
 * window (TraceFetch::cleanupTraceMetadataOnCommit).
 *
 * Extracted as a shared header so the unit tests anchor the production
 * logic itself instead of a mirrored copy (guards commit e87d6b5db4,
 * "cpu-o3: Anchor trace metadata cleanup guard on commit").
 */
namespace TraceMetaGuard
{

/**
 * Retention window behind the guard anchor.
 *
 * Backend squashes can anchor on instructions far behind the current
 * frontier: a branch resolution, deferred MDP violation, or order
 * violation squash can reference an instruction that committed hundreds
 * of sequence numbers ago (wrong-path supplies inflate the seqNum
 * distance without occupying ROB entries; observed gaps exceed 600).
 * This must equal TraceReader::HISTORY_CAPACITY so retention covers
 * exactly the range the reader can soft-replay; a squash anchored deeper
 * than this cannot be served by history replay anyway. The cross-check
 * against the reader header runs in champsim_trace_reader.test.cc.
 */
constexpr uint64_t TRACE_META_GUARD = 4096;

/**
 * Compute the guard anchor (keep_min) for commit-side metadata cleanup.
 *
 * The window is anchored behind the oldest in-flight instruction and (if
 * active) behind the wrong-path boundary. When both anchors are empty
 * (oldest_inflight == MAX, i.e. the in-flight list is drained, and no
 * wrong path is active), the anchor falls back to the just-committed
 * instruction's seqNum instead of MAX: the in-flight list can be empty
 * while a backend squash (branch resolution, deferred MDP violation,
 * squash-after serializing commit) has already removed every younger
 * instruction but its frontend squash has not reached Fetch yet. That
 * squash rolls the trace reader back to its anchor instruction, which
 * needs the metadata of the last committed instruction (and possibly the
 * following one for target resolution). Treating the drained pipeline as
 * a license to wipe every entry (the pre-e87d6b5db4 behavior) wiped that
 * metadata, desynchronized trace replay, and tripped the
 * "trace squash target PC ... not in the buffered expected stream" panic.
 */
inline uint64_t
computeMetaKeepMin(uint64_t oldest_inflight,
                   uint64_t wrong_path_boundary,
                   uint64_t last_committed)
{
    uint64_t keep_min = std::min(oldest_inflight, wrong_path_boundary);
    if (keep_min == std::numeric_limits<uint64_t>::max()) {
        keep_min = last_committed;
    }
    return keep_min;
}

/**
 * Convert the anchor into the seqNum threshold below which metadata
 * entries may be erased (entries strictly below the threshold are
 * victims). Never underflows: an anchor within the guard window keeps
 * everything (threshold 0).
 */
inline uint64_t
computeSafeThreshold(uint64_t keep_min, uint64_t guard = TRACE_META_GUARD)
{
    return (keep_min > guard) ? (keep_min - guard) : 0;
}

} // namespace TraceMetaGuard
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACE_META_GUARD_HH__
