#include "mem/cache/xs_l2/L2MainPipe.hh"

#include <algorithm>

#include "PipelineResources.hh"
#include "base/trace.hh"
#include "debug/L2MainPipe.hh"
#include "mem/cache/xs_l2/L2CacheSlice.hh"
#include "mem/packet.hh"

namespace gem5
{

L2MainPipe::L2MainPipe(L2CacheSlice* _owner, unsigned depth)
    : owner(_owner),
      cur_cycle(0)
{
    scoreboardResources.resize(depth, PipelineResources::Free);
    scoreboardTasks.resize(depth, PipelineTask(TaskSource::NoWhere, nullptr));

    // construct the taskResourceMap
    taskResourceMap[TaskSource::L1MSHR]        = PipelineResources::DirRead  |
                                                 PipelineResources::DataRead;
    taskResourceMap[TaskSource::L1WQ]          = PipelineResources::DirRead  |
                                                 PipelineResources::DirWrite |
                                                 PipelineResources::DataWrite|
                                                 PipelineResources::GrantBuf;
    taskResourceMap[TaskSource::L3Snoop]       = PipelineResources::DirRead  |
                                                 PipelineResources::DataRead;
    taskResourceMap[TaskSource::L2MSHRGrant]   = PipelineResources::DirRead  |
                                                 PipelineResources::DataRead |
                                                 PipelineResources::GrantBuf;
    taskResourceMap[TaskSource::L2MSHRRelease] = PipelineResources::DataWrite;
}

void
L2MainPipe::advance(Cycles now)
{
    if (now > cur_cycle) {
        // assert(now == cur_cycle + 1);
        advance();
        cur_cycle = now;
    }
}

void
L2MainPipe::advance()
{
    // pipeline logic
    sendMSHRGrantPkt();

    // scoreboard update
    scoreboardResources.pop_back();
    scoreboardTasks.pop_back();
    scoreboardResources.emplace_front(PipelineResources::Free);
    scoreboardTasks.emplace_front(TaskSource::NoWhere, nullptr);
}

bool
L2MainPipe::isResourceAvailable(PipelineResources resource) const
{
    // Data is muti cycle path 2,
    // so if last cycle needs to read or write data,
    // this cycle is not available to read or write data
    if (resource & PipelineResources::DataRead) {
        return (scoreboardResources[1] &
               (PipelineResources::DataRead |
                PipelineResources::DataWrite)) == 0;
    }
    if (resource & PipelineResources::DataWrite) {
        return (scoreboardResources[1] &
               (PipelineResources::DataRead |
                PipelineResources::DataWrite)) == 0;
    }
    // Dir is SRAM, read and write should not be available at the same time
    if (resource & PipelineResources::DirRead) {
        return (scoreboardResources[2] &
               (PipelineResources::DirWrite)) == 0;
    }
    return true;
}

bool
L2MainPipe::isTaskAvailable(TaskSource source) const
{
    return (scoreboardTasks[0].source == TaskSource::NoWhere) &&
           isResourceAvailable(taskResourceMap.at(source));
}

void
L2MainPipe::buildTask(PacketPtr pkt, TaskSource source)
{
    scoreboardTasks[0].source = source;
    scoreboardTasks[0].pkt = pkt;
    scoreboardResources[0] |= taskResourceMap.at(source);
}

void
L2MainPipe::sendMSHRGrantPkt()
{
    // Later pipeline stages have higher grant priority
    for (int i = 4; i >= 2; i--) {
        bool isGrant = scoreboardTasks[i].source == TaskSource::L2MSHRGrant;
        bool needGrantBuf = scoreboardResources[i] & PipelineResources::GrantBuf;
        if (isGrant && needGrantBuf) {
            PacketPtr pkt = scoreboardTasks[i].pkt;
            if (!owner->inner_mem_port.sendTimingResp(pkt)) {
                panic("L2 cache recvTimingResp failed");
            } else {
                scoreboardResources[i] &= ~PipelineResources::GrantBuf;
            }
            break;
        }
    }
}

bool
L2MainPipe::hasWork() const
{
    return std::any_of(scoreboardTasks.begin(), scoreboardTasks.end(),
                       [](PipelineTask s){ return s.source != TaskSource::NoWhere; });
}

} // namespace gem5
