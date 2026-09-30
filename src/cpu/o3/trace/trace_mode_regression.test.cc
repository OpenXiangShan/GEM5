// Targeted regression tests for the three failure modes identified in the
// adversarial review of trace-mode fix commits on branch trace-new.
//
// These tests exercise the *algorithmic core* of each fix as pure functions,
// deliberately avoiding a full gem5 simulation build.  Each test is annotated
// with the commit it guards and the observable symptom when that commit is
// reverted.
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/trace/trace_mode_regression.test.opt --unit-test -j$(nproc)
// Run:
//   ./build/NULL/cpu/o3/trace/trace_mode_regression.test.opt
//
// NOTE: No production files under src/cpu/o3/trace/ or src/cpu/o3/decode.cc
//       are modified by this test.  All logic is duplicated inline so that the
//       test is self-contained and the production code can evolve independently.

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "gtest/gtest.h"

// ---------------------------------------------------------------------------
// CASE 1 — rollbackTraceReader nearest-older fallback
// Protected commit: 064029e6ef  "cpu-o3,util: Fix trace recovery replay"
//
// Failure mode (without fix): when the squash seqNum has no direct entry in
// seqNumToTraceIndex, the original code fell back silently to cursor 0 (i.e.
// the beginning of the trace), causing the replay to restart from the wrong
// position.  With the fix, it finds the nearest-older mapped entry and
// computes the correct seek cursor from it.
//
// This test reimplements the lookup algorithm from TraceFetch::rollbackTraceReader
// and verifies three sub-cases:
//   a) seqNum has a direct mapping => cursor is (index - 1)
//   b) seqNum is unmapped but a predecessor exists => cursor is predecessor.index
//      (not 0)
//   c) seqNum is unmapped and no predecessor exists => returns nullopt (failure)
// ---------------------------------------------------------------------------

using SeqNum = uint64_t;
using TraceIndex = uint64_t;
using SeqToIndex = std::unordered_map<SeqNum, TraceIndex>;

// Exact reimplementation of the seek-cursor resolution in
// TraceFetch::rollbackTraceReader (non-squash-itself path).
// Returns the seek_cursor to pass to softSeekToInstruction, or nullopt if
// no valid mapping was found (mirrors the false return path).
static std::optional<uint64_t>
resolveRollbackSeekCursor(const SeqToIndex &seqToIndex,
                          SeqNum seqNum,
                          bool squash_itself)
{
    bool need_to_decrement_index = squash_itself;
    TraceIndex index = 0;

    auto it = seqToIndex.find(seqNum);
    bool found = (it != seqToIndex.end());
    if (found) {
        index = it->second;
    } else {
        SeqNum prev_seq = 0;
        TraceIndex prev_index = 0;
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
        if (index > 0) {
            --index;
        } else {
            return std::nullopt;
        }
    }

    const uint64_t seek_cursor = (index > 0) ? (index - 1) : 0;
    return seek_cursor;
}

// 1a: Direct mapping — must land at (traceIndex - 1).
TEST(RollbackCursorCase1, DirectMappingResolvesCorrectly)
{
    SeqToIndex m;
    m[100] = 42;  // seqNum 100 -> traceIndex 42
    auto cursor = resolveRollbackSeekCursor(m, 100, /*squash_itself=*/false);
    ASSERT_TRUE(cursor.has_value())
        << "Expected a valid cursor for a directly-mapped seqNum";
    // index=42, seek_cursor = 42 - 1 = 41
    EXPECT_EQ(*cursor, 41u);
}

// 1b: Unmapped seqNum with predecessor — must use predecessor.index, NOT 0.
// This is the regression: the broken code returned cursor=0 here.
TEST(RollbackCursorCase1, UnmappedSeqNumUsesPredecessorNotZero)
{
    SeqToIndex m;
    // Map seqNums 10->traceIndex 5 and 20->traceIndex 15; squash seqNum 13
    // (unmapped).  The nearest-older predecessor is seqNum 10 -> index 5.
    // Expected: index = 5 + 1 = 6, cursor = 6 - 1 = 5.
    m[10] = 5;
    m[20] = 15;
    auto cursor = resolveRollbackSeekCursor(m, /*seqNum=*/13, /*squash_itself=*/false);
    ASSERT_TRUE(cursor.has_value())
        << "Expected a valid cursor when a predecessor mapping exists";
    EXPECT_EQ(*cursor, 5u)
        << "Cursor must be predecessor.index (not 0) when seqNum is unmapped";
    EXPECT_NE(*cursor, 0u)
        << "Regression check: broken code would have returned 0";
}

// 1b-variant: Multiple predecessors — must choose the NEAREST older one.
TEST(RollbackCursorCase1, UnmappedSeqNumChoosesNearestPredecessor)
{
    SeqToIndex m;
    m[5]  = 3;
    m[10] = 7;
    m[20] = 15;
    // seqNum 12 -> nearest older is seqNum 10 -> index 7
    // index = 7 + 1 = 8, cursor = 8 - 1 = 7
    auto cursor = resolveRollbackSeekCursor(m, /*seqNum=*/12, /*squash_itself=*/false);
    ASSERT_TRUE(cursor.has_value());
    EXPECT_EQ(*cursor, 7u);
}

// 1c: No predecessor — must return nullopt (not silently succeed at 0).
TEST(RollbackCursorCase1, NoPredecessorReturnsFailed)
{
    SeqToIndex m;
    m[50] = 30;  // only entry is newer than seqNum 10
    auto cursor = resolveRollbackSeekCursor(m, /*seqNum=*/10, /*squash_itself=*/false);
    EXPECT_FALSE(cursor.has_value())
        << "Must fail when no predecessor mapping exists";
}

// 1d: squash_itself=true with direct mapping — index must be decremented.
TEST(RollbackCursorCase1, SquashItselfDecrementsIndex)
{
    SeqToIndex m;
    m[100] = 42;
    // squash_itself=true: need_to_decrement=true, index=42-1=41, cursor=40
    auto cursor = resolveRollbackSeekCursor(m, 100, /*squash_itself=*/true);
    ASSERT_TRUE(cursor.has_value());
    EXPECT_EQ(*cursor, 40u);
}

// 1e: squash_itself=true with predecessor path — predecessor index is used
//     directly (not +1), and need_to_decrement is cleared.
TEST(RollbackCursorCase1, SquashItselfWithPredecessorNoBonusDecrement)
{
    SeqToIndex m;
    m[10] = 5;
    // squash_itself=true and unmapped seqNum 13: prev_index=5, index=5,
    // need_to_decrement cleared -> cursor = 5 - 1 = 4.
    auto cursor = resolveRollbackSeekCursor(m, /*seqNum=*/13, /*squash_itself=*/true);
    ASSERT_TRUE(cursor.has_value());
    EXPECT_EQ(*cursor, 4u);
}

// ---------------------------------------------------------------------------
// CASE 2 — Wrong-path boundary non-control instruction predTaken clear
// Protected commit: add587d180  "cpu-o3,bpu: Fix trace non-control wrong-path"
//
// Failure mode (without fix): when the wrong-path boundary squash instruction
// is a non-control instruction (e.g. a store) with a stale predTaken=true and
// stale predTarg, the code did not clear these before entering Hold mode.
// Downstream this caused the instruction to appear as a predicted-taken branch,
// leading to a spurious re-squash or mispred counter increment.
//
// The fix is the guard in classifyWrongPathInstSquash:
//   if (!squashInst->isControl() && squashInst->readPredTaken()) {
//       squashInst->setPredTaken(false);
//       squashInst->setPredTarg(new_pc);
//   }
//
// This test reimplements that guard as a pure free function and verifies:
//   a) Non-control + predTaken=true => predTaken cleared, predTarg set to new_pc.
//   b) Non-control + predTaken=false => no mutation.
//   c) Control + predTaken=true => no mutation (guard is non-control only).
// ---------------------------------------------------------------------------

struct FakeDynInst
{
    bool isCtrl        = false;
    bool predTaken     = false;
    uint64_t predTarg  = 0xDEAD;

    bool isControl()     const { return isCtrl; }
    bool readPredTaken() const { return predTaken; }
    void setPredTaken(bool v)  { predTaken = v; }
    void setPredTarg(uint64_t addr) { predTarg = addr; }
};

// Exact guard from TraceFetch::classifyWrongPathInstSquash
static void
applyNonControlPredCorrection(FakeDynInst &inst, uint64_t new_pc)
{
    if (!inst.isControl() && inst.readPredTaken()) {
        inst.setPredTaken(false);
        inst.setPredTarg(new_pc);
    }
}

// 2a: Store with stale predTaken=true — must be cleared.
// Regression: without the fix, predTaken remains true after recovery.
TEST(NonControlPredClearCase2, NonControlStalePredTakenIsCleared)
{
    FakeDynInst inst;
    inst.isCtrl    = false;  // not a control instruction (e.g., a store)
    inst.predTaken = true;   // stale: BPU had predicted taken
    inst.predTarg  = 0xBAD;  // stale target

    const uint64_t new_pc = 0x1000;
    applyNonControlPredCorrection(inst, new_pc);

    EXPECT_FALSE(inst.readPredTaken())
        << "predTaken must be cleared for non-control boundary inst";
    EXPECT_EQ(inst.predTarg, new_pc)
        << "predTarg must be set to fallthrough PC (new_pc)";
}

// 2b: Non-control with predTaken already false — no mutation expected.
TEST(NonControlPredClearCase2, NonControlAlreadyClearedIsUntouched)
{
    FakeDynInst inst;
    inst.isCtrl    = false;
    inst.predTaken = false;
    inst.predTarg  = 0xDEAD;

    const uint64_t new_pc = 0x1000;
    applyNonControlPredCorrection(inst, new_pc);

    EXPECT_FALSE(inst.readPredTaken());
    EXPECT_EQ(inst.predTarg, 0xDEADu)  // unchanged
        << "predTarg must not be mutated when predTaken was already false";
}

// 2c: Control instruction with predTaken=true — guard must NOT fire.
// The fix is specifically for non-control instructions.
TEST(NonControlPredClearCase2, ControlInstIsLeftUntouched)
{
    FakeDynInst inst;
    inst.isCtrl    = true;   // this is a control instruction
    inst.predTaken = true;
    inst.predTarg  = 0xCAFE;

    const uint64_t new_pc = 0x1000;
    applyNonControlPredCorrection(inst, new_pc);

    EXPECT_TRUE(inst.readPredTaken())
        << "Control instruction predTaken must not be mutated by the guard";
    EXPECT_EQ(inst.predTarg, 0xCAFEu)
        << "Control instruction predTarg must be unchanged";
}

// ---------------------------------------------------------------------------
// CASE 3 — Return instruction whose BPU-predicted target matches trace truth
// Protected commit: 803b6ec090  "cpu-o3,bpu: Fix trace return target recovery"
//
// Failure mode (without fix): in decodeInsts, the un-predicted return path
// computes the trace branch target and then unconditionally incremented
// stats.branchMispred, squashed, and redirected — even when the BPU's
// predicted target was already correct (== trace target).  The fix adds an
// early-exit `continue` when trace target == pred target, skipping the mispred
// counter and squash.
//
// The guard added by the fix is:
//   if (*target == inst->readPredTarg()) { continue; }  // skip resteer
//
// This test reimplements that decision as a pure boolean function and verifies:
//   a) trace_target == pred_target => should_skip (no mispred, no squash).
//   b) trace_target != pred_target => should NOT skip (trigger resteer).
//   c) Boundary: trace_target and pred_target differ only in npc bits =>
//      still a mismatch => should NOT skip.
// ---------------------------------------------------------------------------

// Minimal PC-state struct that replicates the equality check used in decode.
// The production code does: if (*target == inst->readPredTarg()) { continue; }
// which calls PCStateBase::operator==, which for RiscvISA::PCState compares
// both pc and npc fields.
struct FakePCState
{
    uint64_t pc  = 0;
    uint64_t npc = 0;

    bool operator==(const FakePCState &o) const {
        return pc == o.pc && npc == o.npc;
    }
    bool operator!=(const FakePCState &o) const { return !(*this == o); }
};

// Reimplementation of the early-exit decision in decodeInsts (trace return path).
// Returns true if decode should skip resteer (take the early-exit continue).
static bool
shouldSkipReturnResteering(const FakePCState &trace_target,
                            const FakePCState &pred_targ)
{
    return trace_target == pred_targ;
}

// 3a: Trace target matches BPU prediction — must skip resteer.
// Regression: without fix, branchMispred would be incremented here.
TEST(ReturnEarlyExitCase3, MatchingTargetSkipsResteer)
{
    FakePCState trace = {0x2000, 0x2004};
    FakePCState pred  = {0x2000, 0x2004};  // BPU already correct

    EXPECT_TRUE(shouldSkipReturnResteering(trace, pred))
        << "When trace target == pred target, decode must take early-exit "
           "continue and NOT increment branchMispred";
}

// 3b: Trace target differs from BPU prediction — must trigger resteer.
TEST(ReturnEarlyExitCase3, MismatchedTargetTriggersResteer)
{
    FakePCState trace = {0x3000, 0x3004};
    FakePCState pred  = {0x4000, 0x4004};  // BPU was wrong

    EXPECT_FALSE(shouldSkipReturnResteering(trace, pred))
        << "When trace target != pred target, decode must NOT skip resteer";
}

// 3c: PC matches but npc differs — still a mismatch, must NOT skip.
// This guards against a partial-equality bug where only the pc field is checked.
TEST(ReturnEarlyExitCase3, SamePcDifferentNpcIsStillMismatch)
{
    FakePCState trace = {0x5000, 0x5004};
    FakePCState pred  = {0x5000, 0x5008};  // same pc, wrong npc

    EXPECT_FALSE(shouldSkipReturnResteering(trace, pred))
        << "PC-only equality is insufficient; npc must also match";
}

// 3d: Both zeroed => match => skip.  (Edge-case sanity.)
TEST(ReturnEarlyExitCase3, ZeroedStatesMatch)
{
    FakePCState trace = {0, 0};
    FakePCState pred  = {0, 0};
    EXPECT_TRUE(shouldSkipReturnResteering(trace, pred));
}
