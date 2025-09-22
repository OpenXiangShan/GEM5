#include "mem/cache/LLC/L3MainPipe.hh"

#include <algorithm>
#include <cstdint>

#include "PipelineResources.hh"
#include "base/trace.hh"
#include "debug/L3MainPipe.hh"
#include "mem/cache/LLC/L3CacheSlice.hh"
#include "mem/cache/LLC/PipelineResources.hh"
#include "mem/packet.hh"

namespace gem5
{

L3MainPipe::L3MainPipe(L3CacheSlice* _owner, unsigned depth)
    : owner(_owner),
      cur_cycle(0)
{
    scoreboardResources.resize(depth, PipelineResources::Free);
    scoreboardTasks.resize(depth, PipelineTask(TaskSource::NoWhere, nullptr));

    // construct the taskResourceMap
    taskResourceMap[TaskSource::L2MSHR]        = PipelineResources::DirRead;
    taskResourceMap[TaskSource::L2WQ]          = PipelineResources::DirRead  |
                                                 PipelineResources::DirWrite |
                                                 PipelineResources::DataWrite|
                                                 PipelineResources::GrantBuf;
    //taskResourceMap[TaskSource::L3Snoop]       = PipelineResources::DirRead;
    taskResourceMap[TaskSource::L3MSHRGrant]   = PipelineResources::DirRead  |
                                                 PipelineResources::DataRead |
                                                 PipelineResources::GrantBuf;
    taskResourceMap[TaskSource::L3MSHRRelease] = PipelineResources::DataWrite;
    taskResourceMap[TaskSource::L3PF]          = PipelineResources::DirRead;
}

inline uint64_t
L3MainPipe::getDirWriteStage() const
{
    // -1 is to get the index of scoreboardTasks & scoreboardResources
    return owner->pipeDataWriteStage - 1;
}

inline PipelineResources
L3MainPipe::getPipelineResources(PacketPtr pkt, TaskSource source) const
{
    return taskResourceMap.at(source) | getExtraResources(pkt, source);
}

void
L3MainPipe::advance(Cycles now)
{
    if (now > cur_cycle) {
        // assert(now == cur_cycle + 1);
        advance();
        cur_cycle = now;
    }
}

void
L3MainPipe::advance()
{
    // pipeline logic
    sendMSHRGrantPkt();

    // scoreboard update
    scoreboardResources.pop_back();
    scoreboardTasks.pop_back();
    scoreboardResources.emplace_front(PipelineResources::Free);
    scoreboardTasks.emplace_front(TaskSource::NoWhere, nullptr);
}

PipelineResources
L3MainPipe::getExtraResources(PacketPtr pkt, TaskSource source) const
{
    PipelineResources extra = PipelineResources::Free;
    bool hit = owner->cache_accessor->findBlock(pkt->getAddr(), pkt->isSecure()) != nullptr;
    if (source == TaskSource::L2MSHR) {
        // acquire from L2 should write directory when hit in L3
        if (hit) {
            extra |= PipelineResources::DirWrite;

            // upgrade req does not need data sram reading
            // otherwise, the req should read from data sram
            if (!pkt->isUpgrade()) {
                extra |= PipelineResources::DataRead;
            }
        }
    }
    // } else if (source == TaskSource::L3Snoop) {
    //     // snoop should write directory and read data sram when hit in L2
    //     if (hit) {
    //         extra |= PipelineResources::DirWrite | PipelineResources::DataRead;
    //     }
    // }
    return extra;
}

bool
L3MainPipe::hasMCP2Stall(PipelineResources resource) const
{
    // Data is muti cycle path 2,
    // so if last cycle needs to read or write data,
    // this cycle is not available to read or write data
    bool stall = false;
    if (resource & PipelineResources::DataRead) {
        stall |= (scoreboardResources[1] &
                 (PipelineResources::DataRead |
                  PipelineResources::DataWrite)) != 0;
    }
    if (resource & PipelineResources::DataWrite) {
        stall |= (scoreboardResources[1] &
                 (PipelineResources::DataRead |
                  PipelineResources::DataWrite)) != 0;
    }
    return stall;
}

bool
L3MainPipe::hasDirSramStall(PipelineResources resource) const
{
    // Dir is SRAM, read and write should not be available at the same time
    if (resource & PipelineResources::DirRead) {
        return (scoreboardResources[getDirWriteStage()] &
                PipelineResources::DirWrite) != 0;
    }
    return false;
}

bool
L3MainPipe::isResourceAvailable(PipelineResources resource) const
{
    return !hasMCP2Stall(resource) && !hasDirSramStall(resource);
}

bool
L3MainPipe::setBlockByDir(PacketPtr pkt, TaskSource source) const
{
    if (source == TaskSource::L2MSHR) {
        for (int i = 1; i <= getDirWriteStage(); i++) {
            bool valid = scoreboardTasks[i].source != TaskSource::NoWhere;
            bool sameSet = owner->getSetIdx(pkt->getAddr()) == owner->getSetIdx(scoreboardTasks[i].addr);
            if (valid && sameSet) {
                return true;
            }
        }
    }
    return false;
}

bool
L3MainPipe::isTaskAvailable(PacketPtr pkt, TaskSource source) const
{
    PipelineResources resources = getPipelineResources(pkt, source);
    bool setBlock = setBlockByDir(pkt, source);
    if (owner->dirReadBypass) {
        bool sameSet = owner->getSetIdx(pkt->getAddr()) == owner->getSetIdx(scoreboardTasks[getDirWriteStage()].addr);
        if ((source == TaskSource::L2MSHR) && sameSet) {
            // here cancel the DirRead resource, to skip the directory read&write check
            resources &= ~PipelineResources::DirRead;
        }
        setBlock = false;
    }
    bool available = (scoreboardTasks[0].source == TaskSource::NoWhere) && !setBlock &&
                     isResourceAvailable(resources);

    // record stats
    if (source == TaskSource::L2MSHR) {
        if (!available) {
            owner->stats.l2ReqEnterPipeFail++;
        }
        if (setBlock) {
            owner->stats.l2ReqPipeSetConflict++;
        }
        if (hasMCP2Stall(resources)) {
            owner->stats.l2ReqPipeMCP2Stall++;
        }
        if (hasDirSramStall(resources)) {
            owner->stats.l2ReqPipeDirSramStall++;
        }
    }
    return available;
}

void
L3MainPipe::buildTask(PacketPtr pkt, TaskSource source)
{
    scoreboardTasks[0].source = source;
    scoreboardTasks[0].pkt = pkt;
    scoreboardTasks[0].addr = pkt->getAddr();
    scoreboardResources[0] |= getPipelineResources(pkt, source);
}

void
L3MainPipe::sendMSHRGrantPkt()
{
    // Find from S5 to S3
    // Later pipeline stages have higher grant priority
    for (int i = 4; i >= 2; i--) {
        bool isGrant = scoreboardTasks[i].source == TaskSource::L3MSHRGrant;
        bool needGrantBuf = scoreboardResources[i] & PipelineResources::GrantBuf;
        if (isGrant && needGrantBuf) {
            PacketPtr pkt = scoreboardTasks[i].pkt;
            if (!owner->inner_mem_port.sendTimingResp(pkt)) {
                panic("L3 cache recvTimingResp failed");
            } else {
                scoreboardResources[i] &= ~PipelineResources::GrantBuf;
            }
            break;
        }
    }
}

bool
L3MainPipe::hasWork() const
{
    return std::any_of(scoreboardTasks.begin(), scoreboardTasks.end(),
                       [](PipelineTask s){ return s.source != TaskSource::NoWhere; });
}

} // namespace gem5