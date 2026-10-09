// L0 pure-function tests for the trace metadata cleanup guard.
//
// Anchored tests: production (TraceFetch.cc) and this test both include
// TraceMetaGuard.hh, so a semantic change in the production decision
// logic fails here at compile time or run time — no mirrored copy to
// drift (unlike the legacy inline-duplicated regression suites).
//
// Tombstone: commit e87d6b5db4 "cpu-o3: Anchor trace metadata cleanup
// guard on commit". Pre-fix behavior: an empty in-flight list
// (getOldestInFlightSeqNum() == MAX) together with an inactive wrong path
// made keep_min == MAX, so the threshold subtraction wrapped around and
// the cleanup wiped every seqNumToTraceIndex / traceInstMap entry —
// including the metadata an in-flight backend squash still needed,
// tripping the "trace squash target PC ... not in the buffered expected
// stream" panic. The guard width was also widened 256 -> 4096 to cover
// the range TraceReader can soft-replay.
//
// Build (NULL ISA):
//   scons build/NULL/cpu/o3/trace/trace_meta_guard.test.opt --unit-test -j$(nproc)
// Run:
//   ./build/NULL/cpu/o3/trace/trace_meta_guard.test.opt

#include <cstdint>
#include <limits>

#include "cpu/o3/trace/TraceMetaGuard.hh"
#include "gtest/gtest.h"

namespace
{

constexpr uint64_t MAX_SEQ = std::numeric_limits<uint64_t>::max();

} // anonymous namespace

TEST(TraceMetaGuard, BothAnchorsEmptyFallsBackToLastCommitted)
{
    // e87d6b5db4: drained in-flight list + inactive wrong path must anchor
    // to the just-committed seqNum, never to MAX (which wrapped the
    // threshold and wiped the whole map).
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeMetaKeepMin(MAX_SEQ, MAX_SEQ, 12345),
              12345u);
}

TEST(TraceMetaGuard, BothAnchorsEmptyZeroCommitAnchorsAtZero)
{
    // Startup edge: nothing has committed yet. keep_min 0 keeps everything.
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeMetaKeepMin(MAX_SEQ, MAX_SEQ, 0), 0u);
}

TEST(TraceMetaGuard, OldestInflightAnchorsWindow)
{
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeMetaKeepMin(1000, MAX_SEQ, 5000),
              1000u);
}

TEST(TraceMetaGuard, WrongPathBoundaryTakesPrecedenceWhenOlder)
{
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeMetaKeepMin(1000, 700, 5000), 700u);
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeMetaKeepMin(700, 1000, 5000), 700u);
}

TEST(TraceMetaGuard, EmptyInflightWithActiveWrongPathUsesWrongPath)
{
    // In-flight list drained while wrong path is active: the wrong-path
    // boundary anchors the window (it is the metadata the exit path needs).
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeMetaKeepMin(MAX_SEQ, 800, 5000), 800u);
}

TEST(TraceMetaGuard, GuardSubtractionNeverUnderflows)
{
    // Anchor inside the guard window keeps everything (threshold 0); the
    // pre-fix wrap-around class of failure cannot recur through subtraction.
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeSafeThreshold(0), 0u);
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeSafeThreshold(4095), 0u);
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeSafeThreshold(4096), 0u);
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeSafeThreshold(4097), 1u);
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeSafeThreshold(100000), 95904u);
    // Custom guard width is honored (the 256-era width remains testable).
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeSafeThreshold(100000, 256), 99744u);
}

TEST(TraceMetaGuard, DrainedPipelineKeepsRecentMetadata)
{
    // End-to-end decision for the e87d6b5db4 scenario: pipeline drained
    // (backend squash in flight) right after committing seqNum 600000.
    // Pre-fix this wiped every entry; now the window retains >= 595904.
    const uint64_t keep_min =
        gem5::o3::TraceMetaGuard::computeMetaKeepMin(MAX_SEQ, MAX_SEQ, 600000);
    EXPECT_EQ(keep_min, 600000u);
    EXPECT_EQ(gem5::o3::TraceMetaGuard::computeSafeThreshold(keep_min), 600000u - 4096);
}

TEST(TraceMetaGuard, GuardWidthMatchesReaderHistoryCapacity)
{
    // Must equal TraceReader::HISTORY_CAPACITY so retention covers exactly
    // the range the reader can soft-replay. This tombstone pins the
    // intended value; the true cross-check against the reader header runs
    // in champsim_trace_reader.test.cc, which links the reader sources.
    EXPECT_EQ(gem5::o3::TraceMetaGuard::TRACE_META_GUARD, 4096u);
}
