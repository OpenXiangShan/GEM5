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

#ifndef __CPU_O3_TRACE_TRACE_RECOVERY_RULES_HH__
#define __CPU_O3_TRACE_TRACE_RECOVERY_RULES_HH__

#include <cstdint>
#include <map>
#include <optional>

namespace gem5
{
namespace o3
{

/**
 * Pure decision rules and data for the trace-mode squash/recovery paths so
 * that the regression tests anchor the production logic itself instead of
 * a mirrored copy (the legacy trace_mode_regression.test.cc duplicated
 * these algorithms inline, so production-side semantic changes could not
 * fail the tests).
 *
 * Anchored production sites:
 *   - TraceFetch::rollbackTraceReader         (guards commit 064029e6ef)
 *   - TraceFetch::applyTraceRecoveryAction    (guards commit 090d525ed3)
 *   - TraceFetch::classifyWrongPathInstSquash (guards commit add587d180)
 *   - decode.cc trace-return early exit       (guards commit 803b6ec090)
 *   - exit/rollback ordering                  (guards commit 1d9e0ef5b0)
 */
namespace TraceRecoveryRules
{

/** Recovery mode decided by the squash classifiers. */
enum class TraceRecoveryMode
{
    Hold,
    Rollback
};

/** Recovery plan produced by the classifiers, consumed by
 *  TraceFetch::applyTraceRecoveryAction. Plain data so the anchored tests
 *  can construct plans directly. */
struct TraceRecoveryAction
{
    TraceRecoveryMode mode = TraceRecoveryMode::Hold;
    uint64_t rollbackSeqNum = 0;
    uint64_t rollbackTraceIndex = 0;
    bool useTraceIndex = false;
    bool squashItself = false;
    uint64_t targetPc = 0;
    const char *exitWrongPathReason = nullptr;
    const char *debugReason = nullptr;
};

/**
 * What to do with the buffered expected stream after a rollback attempt.
 *
 * Tombstone (090d525ed3 "Repair trace-replay squash recovery desync"):
 * the pre-fix code unconditionally cleared traceExpectedStream after a
 * squash even when the trace reader could NOT be repositioned
 * (seqNum->traceIndex lookup miss). The buffered entries had already been
 * pulled from the reader, so clearing them silently dropped instructions
 * and desynchronized trace replay (the startup TC squash dropped the
 * first 16 trace instructions this way). Only a repositioned reader may
 * be paired with a cleared buffer; an unrepositioned reader must keep
 * the buffer and reconcile it against the squash target instead.
 */
enum class ExpectedStreamDisposition
{
    Clear,      // reader repositioned: buffer refills from the new position
    Reconcile   // reader stuck: buffer is the only truth left; keep it
};

inline ExpectedStreamDisposition
expectedStreamDisposition(bool reader_repositioned)
{
    return reader_repositioned ? ExpectedStreamDisposition::Clear
                               : ExpectedStreamDisposition::Reconcile;
}

/**
 * Whether a recovery action carries a complete rollback context.
 *
 * Tombstone (1d9e0ef5b0 "move wrong-path exit after rollback seq"):
 * exitTraceWrongPath cleared traceWrongPathBranchSeqNum before the
 * non-inst squash path used it to set the rollback seqNum, losing the
 * wrong-path boundary. The structural fix was to classify first and exit
 * from the classified action: whenever an action both exits wrong-path
 * AND rolls back, the rollback context must already be captured in the
 * action itself — never re-read from wrong-path member state that the
 * exit has just cleared. This predicate pins that contract; it is
 * exercised by the anchored tests and documents the ordering invariant
 * the apply path must preserve (classify -> exit -> rollback, with the
 * action as the only carrier).
 */
inline bool
rollbackContextComplete(const TraceRecoveryAction &action)
{
    if (action.mode != TraceRecoveryMode::Rollback)
        return true;  // nothing to roll back, context trivially complete
    return action.useTraceIndex || action.rollbackSeqNum != 0;
}

/**
 * Resolve the soft-seek cursor for a seqNum-rollback of the trace reader.
 *
 * Mirrors TraceFetch::rollbackTraceReader: given the seqNum -> traceIndex
 * map (1-based indices), find the index to roll back to so the next
 * getNextInstruction() returns the instruction at that index, then convert
 * it into the 0-based soft-seek cursor.
 *
 * Rules (guards 064029e6ef "Fix trace recovery replay"):
 *   - a direct seqNum mapping anchors at (index - 1);
 *   - an unmapped seqNum uses the NEAREST OLDER predecessor mapping
 *     (predecessor.index when squashing the instruction itself,
 *     predecessor.index + 1 otherwise) — never a silent cursor 0, which
 *     restarted replay from the beginning of the trace;
 *   - no predecessor at all fails (nullopt) instead of succeeding at 0.
 */
inline std::optional<uint64_t>
resolveRollbackSeekCursor(const std::map<uint64_t, uint64_t> &seqToIndex,
                          uint64_t seqNum, bool squash_itself)
{
    bool need_to_decrement_index = squash_itself;
    // Find trace index to rollback to (1-based). We want the next
    // getNextInstruction() to return the instruction at 'index'.
    // A direct lookup miss and a mapped 0-index are indistinguishable here;
    // trace indices are 1-based so a mapped entry is never 0.
    uint64_t index = 0;
    auto it = seqToIndex.find(seqNum);
    bool found = (it != seqToIndex.end());
    if (found) {
        index = it->second;
    } else {
        uint64_t prev_seq = 0;
        uint64_t prev_index = 0;
        for (const auto &entry : seqToIndex) {
            if (entry.first < seqNum &&
                (prev_index == 0 || entry.first > prev_seq)) {
                prev_seq = entry.first;
                prev_index = entry.second;
            }
        }
        if (prev_index != 0) {
            if (squash_itself) {
                index = prev_index;
                need_to_decrement_index = false;
            } else {
                index = prev_index + 1;
            }
            found = true;
        } else {
            return std::nullopt;
        }
    }

    if (need_to_decrement_index) {
        // If squashing the instruction itself, go back one more instruction.
        if (index > 0) {
            --index;
        } else {
            return std::nullopt;
        }
    }

    // Soft-seek cursor is 0-based; index is 1-based.
    const uint64_t seek_cursor = (index > 0) ? (index - 1) : 0;
    return seek_cursor;
}

/**
 * Clear a stale predicted-taken flag on a non-control wrong-path boundary
 * instruction after the trace reader rejoins the correct path.
 *
 * Mirrors the guard in TraceFetch::classifyWrongPathInstSquash (guards
 * add587d180 "Fix trace non-control wrong-path"): a store that the BPU
 * predicted taken keeps its stale predTaken/predTarg through recovery, so
 * IEW re-classifies it as a branch misprediction after the decode squash.
 * Control instructions are left to the normal misprediction path.
 *
 * Duck-typed on the instruction type: production passes DynInst, the unit
 * tests pass a minimal fake exposing isControl/readPredTaken/setPredTaken/
 * setPredTarg.
 */
template <typename Inst, typename PC>
inline void
applyNonControlPredCorrection(Inst &inst, const PC &new_pc)
{
    if (!inst.isControl() && inst.readPredTaken()) {
        inst.setPredTaken(false);
        inst.setPredTarg(new_pc);
    }
}

/**
 * Decide whether decode must resteer an unpredicted return: the trace
 * branch target already equals the BPU-preserved predicted target.
 *
 * Mirrors the early-exit comparison in decode.cc's trace return path
 * (guards 803b6ec090 "Fix trace return target recovery"): without it, a
 * correct RAS-preserved target was still counted as a misprediction and
 * squashed. The comparison is full PC-state equality (pc AND npc); a
 * pc-only match must still resteer.
 */
template <typename PCTrace, typename PCPred>
inline bool
returnTargetMatchesPrediction(const PCTrace &trace_target,
                               const PCPred &pred_target)
{
    return trace_target == pred_target;
}

} // namespace TraceRecoveryRules
} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACE_RECOVERY_RULES_HH__
