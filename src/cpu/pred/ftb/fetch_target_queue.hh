#ifndef __CPU_PRED_FTB_FETCH_TARGET_QUEUE_HH__
#define __CPU_PRED_FTB_FETCH_TARGET_QUEUE_HH__

#include "cpu/pred/ftb/stream_struct.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

struct FetchTargetEnqState
{
    Addr pc;
    FetchStreamId streamId;
    FetchTargetId nextEnqTargetId;
    FetchTargetEnqState() : pc(0), streamId(1), nextEnqTargetId(0) {}
};

struct FetchTargetReadState
{
    bool valid;
    FetchTargetId targetId;
    FtqEntry *entry;
};

class FetchTargetQueue
{
    // todo: move fetch target buffer here
    // 1. enqueue from fetch stream buffer
    // 2. supply fetch with fetch target head
    // 3. redirect fetch target head after squash
    using FTQ = std::map<FetchTargetId, FtqEntry>;
    using FTQIt = FTQ::iterator;
    FTQ ftq;
    unsigned ftqSize;
    FetchTargetId ftqId{0};  // this is a queue ptr for ftq itself

    // The supply/responsing fetch target state
    FetchTargetReadState supplyFetchTargetState;
    // The demanded fetch target ID to send to fetch
    FetchTargetId fetchDemandTargetId{0};

    FetchTargetEnqState fetchTargetEnqState;

    int currentLoopIter{0};

    std::string _name;

  public:
    FetchTargetQueue(unsigned size);

    void squash(FetchTargetId new_enq_target_id,
                FetchStreamId new_enq_stream_id, Addr new_enq_pc);

    bool fetchTargetAvailable() const;
    bool fetchTargetAvailable(int nextN) const;

    FtqEntry &getTarget();
    FtqEntry &getTarget(int nextN);

    FetchTargetEnqState &getEnqState() { return fetchTargetEnqState; }

    FetchTargetId getSupplyingTargetId()
    {
        if (supplyFetchTargetState.valid) {
            return supplyFetchTargetState.targetId;
        } else {
            return fetchDemandTargetId;
        }
    }

    FetchStreamId getSupplyingStreamId()
    {
        if (supplyFetchTargetState.valid) {
            return supplyFetchTargetState.entry->fsqID;
        } else if (!ftq.empty()) {
            return ftq.begin()->second.fsqID;
        } else {
            return fetchTargetEnqState.streamId;
        }
    }

    void finishCurrentFetchTarget();
    void finishCurrentFetchTarget(int N);

    bool trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &in_loop);


    bool empty() const { return ftq.empty(); }

    unsigned size() const { return ftq.size(); }

    bool full() const { return ftq.size() >= ftqSize; }

    std::pair<bool, FTQIt> getDemandTargetIt();

    void enqueue(FtqEntry entry);

    void dump(const char *when);

    const std::string &name() const { return _name; }

    void setName(const std::string &parent) { _name = parent + ".ftq"; }

    bool validSupplyFetchTargetState() const;

    FtqEntry &getLastInsertedEntry() { return ftq.rbegin()->second; }

    int getCurrentLoopIter() { return currentLoopIter; }

    void incCurrentLoopIter(int totalIter) {
        if (currentLoopIter <= totalIter) {
            currentLoopIter++;
        } else {
            currentLoopIter = 0;
        }
    }

    // bool lastEntryIncomplete() const
    // {
    //     if (ftq.empty())
    //         return false;
    //     const auto &last_entry = ftq.rbegin()->second;
    //     return last_entry.miss() && !last_entry.filledUp();
    // }

    void resetPC(Addr new_pc);
};

}
}
}

#endif  // __CPU_PRED_FTB_FETCH_TARGET_QUEUE_HH__
