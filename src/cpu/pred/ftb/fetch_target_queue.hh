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
    Addr pc;  // 入队PC
    FetchStreamId streamId;  // fsq的id
    FetchTargetId nextEnqTargetId;  // ftq的id
    FetchTargetEnqState() : pc(0), streamId(1), nextEnqTargetId(0) {}
};  // 入队状态, bp 写入ftq，相当于head

struct FetchTargetReadState
{
    bool valid;  // 有效
    FetchTargetId targetId;  // fetch目标id
    FtqEntry *entry;  // ftq的entry
};  // 出队状态, o3 在fetch阶段读取ftq，相当于tail

class FetchTargetQueue
{
    // todo: move fetch target buffer here
    // 1. enqueue from fetch stream buffer
    // 2. supply fetch with fetch target head
    // 3. redirect fetch target head after squash
    // 1. 从fetch stream buffer中入队
    // 2. 从FTQ中供应fetch目标
    // 3. 在squash后重定向fetch目标
    using FTQ = std::map<FetchTargetId, FtqEntry>;  // <id, FtqEntry>， map 字典，有64项
    using FTQIt = FTQ::iterator;
    FTQ ftq;    // <id, FtqEntry>， map 字典，有64项, 用fetchDemandTargetId索引
    unsigned ftqSize;
    FetchTargetId ftqId{0};  // this is a queue ptr for ftq itself

    // The supply/responsing fetch target state  供应状态！
    FetchTargetReadState supplyFetchTargetState;  // 供应fetch目标状态
    // The demanded fetch target ID to send to fetch  需求状态，下一个要处理的fetch目标
    FetchTargetId fetchDemandTargetId{0};  // 发送给fetch的需求fetch目标ID，新target

    FetchTargetEnqState fetchTargetEnqState;  // 入队状态

    int currentLoopIter{0};

    std::string _name;

  public:
    FetchTargetQueue(unsigned size);

    void squash(FetchTargetId new_enq_target_id,
                FetchStreamId new_enq_stream_id, Addr new_enq_pc);

    bool fetchTargetAvailable() const;

    FtqEntry &getTarget();

    FetchTargetEnqState &getEnqState() { return fetchTargetEnqState; }

    // 获取当前供应的目标ID, ftqid
    // 如果有有效的供应状态，返回该状态的目标ID
    // 否则返回需求的目标ID
    FetchTargetId getSupplyingTargetId()
    {
        if (supplyFetchTargetState.valid) {
            // 如果当前有有效的供应状态，返回其目标ID
            return supplyFetchTargetState.targetId;
        } else {
            // 否则返回需求的目标ID（新的fetch目标）
            return fetchDemandTargetId;
        }
    }

    // 获取当前供应的流ID，fsqid
    // 按优先级依次检查：1.当前供应状态 2.FTQ队列头 3.入队状态
    FetchStreamId getSupplyingStreamId()
    {
        if (supplyFetchTargetState.valid) {
            // 1. 如果有有效的供应状态，返回其对应条目的流ID, 最常见
            return supplyFetchTargetState.entry->fsqID;
        } else if (!ftq.empty()) {
            // 2. 如果FTQ非空，返回队列头部条目的流ID
            return ftq.begin()->second.fsqID;
        } else {
            // 3. 如果以上都没有，返回入队状态中的流ID
            return fetchTargetEnqState.streamId;
        }
    }

    void finishCurrentFetchTarget();

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
