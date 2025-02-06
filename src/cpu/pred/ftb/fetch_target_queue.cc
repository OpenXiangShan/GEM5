#include "cpu/pred/ftb/fetch_target_queue.hh"

#include "base/trace.hh"
#include "debug/DecoupleBP.hh"
#include "debug/DecoupleBPProbe.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

FetchTargetQueue::FetchTargetQueue(unsigned size) :
 ftqSize(size)
{
    fetchTargetEnqState.pc = 0x80000000;
    fetchDemandTargetId = 0;
    supplyFetchTargetState.valid = false;
    currentLoopIter = 0;
}

void
FetchTargetQueue::squash(FetchTargetId new_enq_target_id,
                         FetchStreamId new_enq_stream_id, Addr new_enq_pc)
{
    ftq.clear();    // 清空ftq
    // Because we squash the whole ftq, head and tail should be the same 头尾相同
    auto new_fetch_demand_target_id = new_enq_target_id;

    fetchTargetEnqState.nextEnqTargetId = new_enq_target_id;
    fetchTargetEnqState.streamId = new_enq_stream_id;
    fetchTargetEnqState.pc = new_enq_pc;    // 入队状态更新

    supplyFetchTargetState.valid = false;  // squah 时候设置供应状态无效
    supplyFetchTargetState.entry = nullptr;  // 供应状态entry为空
    fetchDemandTargetId = new_fetch_demand_target_id;  // 需求id更新
    currentLoopIter = 0;
    DPRINTF(DecoupleBP,
            "FTQ demand stream ID update to %lu, ftqEnqPC update to "
            "%#lx, fetch demand target Id updated to %lu\n",
            new_enq_stream_id, new_enq_pc, fetchDemandTargetId);
}

bool
FetchTargetQueue::fetchTargetAvailable() const
{
    return supplyFetchTargetState.valid &&
           supplyFetchTargetState.targetId == fetchDemandTargetId;  // 供应id等于需求id
}

FtqEntry&
FetchTargetQueue::getTarget()
{
    assert(fetchTargetAvailable());  // 确保供应状态有效
    return *supplyFetchTargetState.entry;  // 返回当前的FTQ条目
}

void
FetchTargetQueue::finishCurrentFetchTarget()
{

    ++fetchDemandTargetId;  // 更新需求id，下一个要处理的fetch目标直接+1，对应ftqid
    ftq.erase(supplyFetchTargetState.targetId);  // 删除供应id对应的ftq条目
    supplyFetchTargetState.valid = false;  // 设置供应状态无效
    supplyFetchTargetState.entry = nullptr;  // 设置供应entry为空
    currentLoopIter = 0;  // 设置当前loop迭代为0
    DPRINTF(DecoupleBP,
            "Finish current fetch target: %lu, inc demand to %lu\n",
            supplyFetchTargetState.targetId, fetchDemandTargetId);
}

bool
FetchTargetQueue::trySupplyFetchWithTarget(Addr fetch_demand_pc, bool &in_loop)
{
    // 当供应状态无效或供应id不等于需求id时,需要从FTQ中获取新target， 也就是分支预测跳转了
    if (!supplyFetchTargetState.valid ||
        supplyFetchTargetState.targetId != fetchDemandTargetId) {  // 供应/响应fetch目标状态无效或供应id不等于需求id
        auto it = ftq.find(fetchDemandTargetId);  // 查找需求id对应的ftq条目
        if (it != ftq.end()) {  // 找到
            if (M5_UNLIKELY(fetch_demand_pc >= it->second.endPC)) {  // 少见：需求pc已经超过ftq条目结束pc
                // This is a special case where the fetch demand pc is
                // already past the end of the ftq entry.
                // In this case, we should just finish the current ftq
                // entry and supply the fetch with the next ftq entry.
                // 这是特殊情况，需求pc已经超过ftq条目结束pc，则跳过当前ftq条目，并供应下一个ftq条目
                DPRINTF(DecoupleBP,
                        "Skip ftq entry %lu: [%#lx, %#lx),", it->first,
                        it->second.startPC, it->second.endPC);

                ++fetchDemandTargetId;
                it = ftq.erase(it);
                if (it == ftq.end()) {
                    in_loop = false;
                    return false;
                }
                DPRINTFR(DecoupleBP,
                        " use %lu: [%#lx, %#lx) instead. because demand pc "
                        "past the first entry.\n",
                        it->first, it->second.startPC, it->second.endPC); // 使用下一个ftq条目替代
            }
            // 正常情况，找到ftq, 返回
            DPRINTF(DecoupleBP,
                    "Found ftq entry with id %lu, writing to "
                    "fetchReadFtqEntryBuffer\n",
                    fetchDemandTargetId);  // 打印找到的ftq条目id
            supplyFetchTargetState.valid = true;  // 设置供应状态有效
            supplyFetchTargetState.targetId = fetchDemandTargetId;  // 设置供应id
            supplyFetchTargetState.entry = &(it->second);  // 设置供应entry
            in_loop = it->second.inLoop;  // 设置是否在loop中
            return true;
        } else {  // 没找到
            DPRINTF(DecoupleBP, "Target id %lu not found\n",
                    fetchDemandTargetId);
            if (!ftq.empty()) {  // 检查ftq是否为空
                // sanity check
                --it;
                DPRINTF(DecoupleBP, "Last entry of target queue: %lu\n",
                        it->first);
                if (it->first > fetchDemandTargetId) {
                    dump("targets in buffer goes beyond demand\n");
                }
                assert(it->first < fetchDemandTargetId);
            }
            in_loop = false;
            return false;
        }
    }
    DPRINTF(DecoupleBP,
            "FTQ supplying, valid: %u, supply id: %u, demand id: %u\n",
            supplyFetchTargetState.valid, supplyFetchTargetState.targetId,
            fetchDemandTargetId);   // 打印FTQ供应状态: 有效,供应id,需求id
    in_loop = supplyFetchTargetState.entry->inLoop;
    return true;
}


std::pair<bool, FetchTargetQueue::FTQIt>
FetchTargetQueue::getDemandTargetIt()
{
    FTQIt it = ftq.find(fetchDemandTargetId);
    return std::make_pair(it != ftq.end(), it);
}

void
FetchTargetQueue::enqueue(FtqEntry entry)
{
    DPRINTF(DecoupleBP, "Enqueueing target %lu with pc %#x and stream %lu\n",
            fetchTargetEnqState.nextEnqTargetId, entry.startPC, entry.fsqID);
    ftq[fetchTargetEnqState.nextEnqTargetId] = entry;  // 将entry插入FTQ中
    ++fetchTargetEnqState.nextEnqTargetId;  // 更新ftq的id, ftqid = fsqid - 1, 没区别了
}

void
FetchTargetQueue::dump(const char* when)
{
    DPRINTF(DecoupleBPProbe, "%s, dump FTQ\n", when);
    for (auto it = ftq.begin(); it != ftq.end(); ++it) {
        DPRINTFR(DecoupleBPProbe, "FTQ entry: %lu, start pc: %#x, end pc: %#lx, stream ID: %lu\n",
                 it->first, it->second.startPC, it->second.endPC, it->second.fsqID);
    }
}

bool
FetchTargetQueue::validSupplyFetchTargetState() const
{
    return supplyFetchTargetState.valid;
}

void
FetchTargetQueue::resetPC(Addr new_pc)
{
    supplyFetchTargetState.valid = false;
    fetchTargetEnqState.pc = new_pc;
}

}  // namespace stream_pred

}  // namespace branch_prediction

}  // namespace gem5