#include "mem/cache/xs_l2/L2MainPipe.hh"

#include <algorithm>

#include "mem/cache/xs_l2/L2CacheWrapper.hh"
#include "mem/packet.hh"

namespace gem5
{

L2MainPipe::L2MainPipe(L2CacheWrapper* _owner, unsigned depth)
    : owner(_owner),
      cur_cycle(0)
{
    scoreboardResources.resize(depth, PipelineResources::ResFree);
    scoreboardTasks.resize(depth, PipelineTask(TaskSource::NoWhere, nullptr));

    // construct the taskResourceMap
    taskResourceMap[TaskSource::L1MSHR]        = PipelineResources::ResDirRead  |
                                                 PipelineResources::ResDataRead;
    taskResourceMap[TaskSource::L1WQ]          = PipelineResources::ResDirRead  |
                                                 PipelineResources::ResDirWrite |
                                                 PipelineResources::ResGrantBuf;
    taskResourceMap[TaskSource::L3Snoop]       = PipelineResources::ResDirRead  |
                                                 PipelineResources::ResDataRead;
    taskResourceMap[TaskSource::L2MSHRGrant]   = PipelineResources::ResDirRead  |
                                                 PipelineResources::ResDataRead |
                                                 PipelineResources::ResGrantBuf;
    taskResourceMap[TaskSource::L2MSHRRelease] = PipelineResources::ResDataWrite;
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
    scoreboardResources.emplace_front(PipelineResources::ResFree);
    scoreboardTasks.emplace_front(TaskSource::NoWhere, nullptr);
}

bool
L2MainPipe::isResourceAvailable(PipelineResources resource) const
{
    // Data is muti cycle path 2,
    // so if last cycle needs to read or write data,
    // this cycle is not available to read or write data
    if (resource & PipelineResources::ResDataRead) {
        return (scoreboardResources[1] &
               (PipelineResources::ResDataRead |
                PipelineResources::ResDataWrite)) == 0;
    }
    if (resource & PipelineResources::ResDataWrite) {
        return (scoreboardResources[1] &
               (PipelineResources::ResDataRead |
                PipelineResources::ResDataWrite)) == 0;
    }
    // Dir is SRAM, read and write should not be available at the same time
    if (resource & PipelineResources::ResDirRead) {
        return (scoreboardResources[2] &
               (PipelineResources::ResDirWrite)) == 0;
    }
    return true;
}

bool
L2MainPipe::isTaskAvailable(TaskSource source) const
{
    return isResourceAvailable(taskResourceMap.at(source));
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
        bool needGrantBuf = scoreboardResources[i] & PipelineResources::ResGrantBuf;
        if (isGrant && needGrantBuf) {
            PacketPtr pkt = scoreboardTasks[i].pkt;
            if (!owner->inner_mem_port.sendTimingResp(pkt)) {
                panic("L2 cache recvTimingResp failed");
            } else {
                scoreboardResources[i] &= ~PipelineResources::ResGrantBuf;
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
