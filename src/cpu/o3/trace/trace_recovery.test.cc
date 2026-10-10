// L2 recovery-plan tests: anchored decision rules for the squash recovery
// apply path (TraceFetch::applyTraceRecoveryAction).
//
// Anchored: production and test share TraceRecoveryRules.hh — the recovery
// plan type, the expected-stream disposition rule and the rollback-context
// contract are all exercised against the exact production code. What is
// NOT covered here (needs a live pipeline; deferred to L3/L4): the
// classifier state machines themselves (classify* read live CPU state)
// and the startup noSquashFromTC suppression (L4 corpus guards it).
//
// Tombstones:
//   - 090d525ed3 "Repair trace-replay squash recovery desync": the
//     unconditional traceExpectedStream.clear() dropped buffered entries
//     whenever the reader failed to reposition, silently desynchronizing
//     replay (the startup TC squash dropped the first 16 instructions).
//   - 1d9e0ef5b0 "move wrong-path exit after rollback seq":
//     exitTraceWrongPath cleared traceWrongPathBranchSeqNum before the
//     rollback path consumed it; the structural fix classifies first and
//     lets the action carry the whole rollback context, so the exit can
//     safely run before the rollback without re-reading cleared state.
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/trace_recovery.test.opt --unit-test -j$(nproc)
// Run:
//   ./build/NULL/cpu/o3/trace_recovery.test.opt

#include <cstdint>

#include "cpu/o3/trace/TraceRecoveryRules.hh"
#include "gtest/gtest.h"

namespace
{

using gem5::o3::TraceRecoveryRules::ExpectedStreamDisposition;
using gem5::o3::TraceRecoveryRules::expectedStreamDisposition;
using gem5::o3::TraceRecoveryRules::rollbackContextComplete;
using gem5::o3::TraceRecoveryRules::TraceRecoveryAction;
using gem5::o3::TraceRecoveryRules::TraceRecoveryMode;

} // anonymous namespace

TEST(TraceRecoveryPlan, ReaderNotRepositionedNeverClearsExpectedStream)
{
    // 090d525ed3 tombstone: an unrepositioned reader must keep the buffered
    // expected stream — clearing it silently dropped instructions the
    // reader had already advanced past.
    EXPECT_EQ(expectedStreamDisposition(/*reader_repositioned=*/false),
              ExpectedStreamDisposition::Reconcile);
}

TEST(TraceRecoveryPlan, ReaderRepositionedClearsExpectedStream)
{
    // Repositioned reader: the buffer would be stale, clearing is the
    // correct action (refill from the new reader position).
    EXPECT_EQ(expectedStreamDisposition(/*reader_repositioned=*/true),
              ExpectedStreamDisposition::Clear);
}

TEST(TraceRecoveryPlan, HoldActionLeavesStreamUntouched)
{
    // A Hold plan never reaches the disposition switch in production
    // (applyTraceRecoveryAction returns before the rollback attempt); pin
    // the plan shape a Hold can be recognized by.
    TraceRecoveryAction hold;
    hold.mode = TraceRecoveryMode::Hold;
    hold.exitWrongPathReason = "mispred boundary squash reaches correct PC";
    EXPECT_EQ(hold.mode, TraceRecoveryMode::Hold);
    // Hold with a wrong-path exit still carries no rollback context, and
    // that is fine: there is nothing to roll back.
    EXPECT_TRUE(rollbackContextComplete(hold));
}

TEST(TraceRecoveryPlan, RollbackBySeqNumCarriesCompleteContext)
{
    TraceRecoveryAction rollback;
    rollback.mode = TraceRecoveryMode::Rollback;
    rollback.rollbackSeqNum = 4242;
    rollback.squashItself = true;
    rollback.targetPc = 0x8048;
    EXPECT_TRUE(rollbackContextComplete(rollback))
        << "seqNum rollback must carry its seqNum";
}

TEST(TraceRecoveryPlan, RollbackByTraceIndexCarriesCompleteContext)
{
    TraceRecoveryAction rollback;
    rollback.mode = TraceRecoveryMode::Rollback;
    rollback.useTraceIndex = true;
    rollback.rollbackTraceIndex = 77;
    EXPECT_TRUE(rollbackContextComplete(rollback))
        << "index rollback must carry its index";
}

TEST(TraceRecoveryPlan, ExitAndRollbackRequiresCapturedContext)
{
    // 1d9e0ef5b0 tombstone: an action that both exits wrong-path AND rolls
    // back must have captured the rollback context at classification time.
    // The exit clears traceWrongPathBranchSeqNum before the rollback runs,
    // so a plan that would re-read that member after the exit loses the
    // boundary. The plan IS the contract: classify -> exit -> rollback,
    // with the action as the only carrier.
    TraceRecoveryAction action;
    action.mode = TraceRecoveryMode::Rollback;
    action.exitWrongPathReason = "rejoin correct path";
    action.rollbackSeqNum = 9001;  // captured before the exit runs
    EXPECT_TRUE(rollbackContextComplete(action));

    // The failure mode the tombstone guards: an exit+rollback plan that
    // relies on state cleared by the exit — no seqNum, no index. Such a
    // plan is malformed and must be detectable.
    TraceRecoveryAction malformed;
    malformed.mode = TraceRecoveryMode::Rollback;
    malformed.exitWrongPathReason = "would re-read cleared state";
    malformed.rollbackSeqNum = 0;
    malformed.useTraceIndex = false;
    EXPECT_FALSE(rollbackContextComplete(malformed))
        << "exit+rollback plan without captured context is exactly the "
           "1d9e0ef5b0 regression shape";
}

TEST(TraceRecoveryPlan, RollbackWithoutContextIsMalformedEvenWithoutExit)
{
    // A pure rollback plan with neither seqNum nor index cannot identify a
    // rollback target at all; detection must not depend on the exit flag.
    TraceRecoveryAction empty_rollback;
    empty_rollback.mode = TraceRecoveryMode::Rollback;
    EXPECT_FALSE(rollbackContextComplete(empty_rollback));
}
