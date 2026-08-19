#include <algorithm>

#include "ftq.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

int
FetchTargetQueue::getTargetTid()
{
    std::array<bool, MaxThreads> eligible{};
    eligible.fill(true);
    return getTargetTid(eligible, nullptr);
}

int
FetchTargetQueue::getTargetTid(const std::array<bool, MaxThreads> &eligible,
                               unsigned *ineligibleSkips)
{
    for (int i = roundRobinPtr; i < numThreads + roundRobinPtr; ++i) {
        ThreadID tid = i % numThreads;
        if (!queue[tid].cap.empty() && hasTarget(fetchId(tid), tid)) {
            if (!eligible[tid]) {
                if (ineligibleSkips) {
                    ++(*ineligibleSkips);
                }
                continue;
            }
            roundRobinPtr = (tid + 1) % numThreads;
            FetchTarget& target = get(queue[tid].fetchptr, tid);
            return target.tid;
        }
    }
    return -1;
}

int
FetchTargetQueue::getTargetTidByFetchQueueSize(const std::array<bool, MaxThreads> &eligible,
                                               unsigned *ineligibleSkips,
                                               const std::array<unsigned, MaxThreads> &fetchQueueSizes)
{
    int selectedTid = -1;
    unsigned minFetchQueueSize = UINT_MAX;

    // Find eligible thread with minimum fetch queue entries
    // Use round-robin order to break ties when queue sizes are equal
    for (int i = roundRobinPtr; i < numThreads + roundRobinPtr; ++i) {
        ThreadID tid = i % numThreads;
        if (!eligible[tid]) {
            if (ineligibleSkips) {
                ++(*ineligibleSkips);
            }
            continue;
        }

        if (!queue[tid].cap.empty() && hasTarget(fetchId(tid), tid)) {
            unsigned fqSize = fetchQueueSizes[tid];
            if (fqSize < minFetchQueueSize) {
                minFetchQueueSize = fqSize;
                selectedTid = tid;
            }
        }
    }

    // Update round-robin pointer for next call
    if (selectedTid != -1) {
        roundRobinPtr = (selectedTid + 1) % numThreads;
    }

    return selectedTid;
}

void
FetchTargetQueue::insert(FetchTarget& target)
{
    ThreadID tid = target.tid;
    assert(queue[tid].cap.size() < ftqSize[tid]);
    queue[tid].cap.push_back(target);
}

void
FetchTargetQueue::finishTarget(ThreadID tid)
{
    queue[tid].fetchptr++;
}

void
FetchTargetQueue::commitTarget(ThreadID tid)
{
    queue[tid].cap.pop_front();
    queue[tid].baseTargetId++;
}

void
FetchTargetQueue::squashAfter(FetchTargetId squashId, ThreadID tid)
{
    while (!empty(tid) && backId(tid) > squashId) {
        queue[tid].cap.pop_back();
    }
    queue[tid].fetchptr = squashId + 1;
}

void
FetchTargetQueue::clear(ThreadID tid)
{
    const FetchTargetId nextTargetId = std::max(
        queue[tid].fetchptr,
        queue[tid].baseTargetId +
            static_cast<FetchTargetId>(queue[tid].cap.size()));

    queue[tid].cap.clear();
    queue[tid].baseTargetId = nextTargetId;
    queue[tid].fetchptr = nextTargetId;
}


}
}
}
