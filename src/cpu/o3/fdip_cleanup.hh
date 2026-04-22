#ifndef __CPU_O3_FDIP_CLEANUP_HH__
#define __CPU_O3_FDIP_CLEANUP_HH__

#include <cassert>
#include <cstddef>
#include <deque>
#include <vector>

#include "base/types.hh"

namespace gem5
{

namespace o3
{

struct FdipPartialStateCleanupSummary
{
    unsigned outstandingLines = 0;
    size_t removedPendingRequests = 0;
    size_t clearedProbeHints = 0;
};

template <typename ThreadState, typename PendingRequest, typename ProbeHint>
inline FdipPartialStateCleanupSummary
cleanupFdipPartialState(ThreadID tid, ThreadState &state,
                        std::vector<PendingRequest> &pendingReqs,
                        std::deque<ProbeHint> &probeHints,
                        unsigned outstandingLines)
{
    state.reset();

    size_t removed_pending = 0;
    for (auto it = pendingReqs.begin(); it != pendingReqs.end();) {
        if (it->tid == tid) {
            if (it->outstanding) {
                assert(outstandingLines > 0);
                --outstandingLines;
            }
            ++removed_pending;
            it = pendingReqs.erase(it);
        } else {
            ++it;
        }
    }

    const size_t cleared_hints = probeHints.size();
    probeHints.clear();

    return FdipPartialStateCleanupSummary{
        outstandingLines, removed_pending, cleared_hints};
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FDIP_CLEANUP_HH__
