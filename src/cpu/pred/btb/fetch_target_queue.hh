#ifndef __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__
#define __CPU_PRED_BTB_FETCH_TARGET_QUEUE_HH__

#include <deque>

#include "cpu/pred/btb/stream_struct.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

struct FetchTargetEntry : FetchStream
{
};

class FetchTargetQueue
{
  public:
    FetchTargetQueue(unsigned size) : size(size) {}

    std::deque<FetchTargetEntry> queue;
    FetchTargetId fetchTargetBaseId{1};  // ID of fetchTargetQueue.front()
    unsigned size;

    FetchTargetId ftqId{1};           // next FTQ id to allocate (monotonic)
    FetchTargetId fetchHeadFtqId{1};  // next FTQ id to be consumed by fetch

    bool isEmpty() const { return queue.empty(); }

    bool targetQueueFull() const { return queue.size() >= size; }

    void consumeFetchTarget(unsigned ftq_id, unsigned fsq_id, unsigned fetched_inst_num);

    bool hasTargetEntry(FetchTargetId id) const
    {
        return !queue.empty() && id >= fetchTargetBaseId &&
               id < fetchTargetBaseId + queue.size();
    }

    FetchTargetEntry& getTarget(FetchTargetId id)
    {
        assert(hasTargetEntry(id));
        return queue[id - fetchTargetBaseId];
    }

    const FetchTargetEntry& getTargetEntry(FetchTargetId id) const
    {
        assert(hasTargetEntry(id));
        return queue[id - fetchTargetBaseId];
    }

    FetchTargetId frontTargetId() const
    {
        assert(!queue.empty());
        return fetchTargetBaseId;
    }

    FetchTargetId backTargetId() const
    {
        assert(!queue.empty());
        return fetchTargetBaseId + queue.size() - 1;
    }

    // Fetch-facing interface: consume FTQ head directly (RTL-like single queue).
    bool ftqHasHead() const { return hasTargetEntry(fetchHeadFtqId); }
    FetchTargetId ftqHeadId() const
    {
        assert(ftqHasHead());
        return fetchHeadFtqId;
    }
    const FetchTargetEntry& ftqHead() const
    {
        assert(ftqHasHead());
        return getTargetEntry(fetchHeadFtqId);
    }
    FetchTargetId ftqHeadFtqId() const
    {
        assert(ftqHasHead());
        return fetchHeadFtqId - 1;
    }
};

}
}
}

#endif
