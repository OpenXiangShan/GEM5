#ifndef __CPU_PRED_BTB_FTQ_HH__
#define __CPU_PRED_BTB_FTQ_HH__

#include <array>

#include "base/types.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/common.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{



class FetchTargetQueue
{
    static constexpr int MaxThreads = o3::MaxThreads;

    uint32_t numThreads;
    uint32_t ftqSize[MaxThreads];

    struct
    {
        std::deque<FetchTarget> cap;
        FetchTargetId baseTargetId = 1;
        FetchTargetId fetchptr = 1;
    } queue[MaxThreads];

    uint32_t roundRobinPtr = 0;
public:
    FetchTargetQueue(uint32_t numThreads_, const uint32_t ftqSize_)
        : numThreads(numThreads_)
    {
        assert(numThreads <= MaxThreads);
        for (uint32_t i = 0; i < numThreads; ++i) {
            ftqSize[i] = ftqSize_;
        }
    }

    // targetid == 0 means no target
    inline FetchTargetId frontId(ThreadID tid) const { return queue[tid].baseTargetId; }
    inline FetchTargetId backId(ThreadID tid) const { return queue[tid].baseTargetId + queue[tid].cap.size() - 1; }
    inline FetchTargetId fetchId(ThreadID tid) const { return queue[tid].fetchptr; }
    inline FetchTarget& front(ThreadID tid) { return queue[tid].cap.front(); }
    inline FetchTarget& back(ThreadID tid) { return queue[tid].cap.back(); }
    inline FetchTarget& fetching(ThreadID tid) { return get(queue[tid].fetchptr, tid); }
    // Uop-cache probes only inspect FTQ boundaries.  A const accessor makes
    // that read-only contract explicit and prevents lookup code from
    // accidentally changing predictor-owned state.
    inline const FetchTarget& fetching(ThreadID tid) const { return get(queue[tid].fetchptr, tid); }
    inline FetchTarget& get(FetchTargetId targetId, ThreadID tid) {
        assert(targetId >= queue[tid].baseTargetId &&
               targetId < queue[tid].baseTargetId + queue[tid].cap.size());
        return queue[tid].cap[targetId - queue[tid].baseTargetId];
    }
    // Const counterpart used by read-only frontend consumers such as decoded
    // block lookup and diagnostics.
    inline const FetchTarget& get(FetchTargetId targetId, ThreadID tid) const {
        assert(targetId >= queue[tid].baseTargetId &&
               targetId < queue[tid].baseTargetId + queue[tid].cap.size());
        return queue[tid].cap[targetId - queue[tid].baseTargetId];
    }
    inline bool hasTarget(FetchTargetId targetId, ThreadID tid) const {
        return targetId >= queue[tid].baseTargetId &&
               targetId < queue[tid].baseTargetId + queue[tid].cap.size();
    }
    inline bool empty(ThreadID tid) const { return queue[tid].cap.empty(); }
    inline bool full(ThreadID tid) const { return queue[tid].cap.size() >= ftqSize[tid]; }
    inline bool anyEmpty() const {
        for (uint32_t i = 0; i < numThreads; ++i) {
            if (!queue[i].cap.empty())
                return false;
        }
        return true;
    }
    inline bool allFull() const {
        for (uint32_t i = 0; i < numThreads; ++i) {
            if (queue[i].cap.size() < ftqSize[i])
                return false;
        }
        return true;
    }
    inline uint32_t size(ThreadID tid) const { return queue[tid].cap.size(); }

    int getTargetTid();
    int getTargetTid(const std::array<bool, MaxThreads> &eligible,
                     unsigned *ineligibleSkips);
    void insert(FetchTarget& target);
    void finishTarget(ThreadID tid);
    void commitTarget(ThreadID tid);
    void squashAfter(FetchTargetId targetId, ThreadID tid);
    void clear(ThreadID tid);
};

}
}
}
#endif
