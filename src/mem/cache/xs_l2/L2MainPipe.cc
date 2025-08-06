#include "mem/cache/xs_l2/L2MainPipe.hh"

#include <algorithm>
#include <cstdint>

#include "PipelineResources.hh"
#include "base/trace.hh"
#include "debug/L2MainPipe.hh"
#include "mem/cache/xs_l2/L2CacheSlice.hh"
#include "mem/cache/xs_l2/PipelineResources.hh"
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
    taskResourceMap[TaskSource::L1MSHR]        = PipelineResources::DirRead;
    taskResourceMap[TaskSource::L1WQ]          = PipelineResources::DirRead  |
                                                 PipelineResources::DirWrite |
                                                 PipelineResources::DataWrite|
                                                 PipelineResources::GrantBuf;
    taskResourceMap[TaskSource::L3Snoop]       = PipelineResources::DirRead;
    taskResourceMap[TaskSource::L2MSHRGrant]   = PipelineResources::DirRead  |
                                                 PipelineResources::DataRead |
                                                 PipelineResources::GrantBuf;
    taskResourceMap[TaskSource::L2MSHRRelease] = PipelineResources::DataWrite;
    taskResourceMap[TaskSource::L2PF]          = PipelineResources::DirRead;
}

inline uint64_t
L2MainPipe::getDirWriteStage() const
{
    // -1 is to get the index of scoreboardTasks & scoreboardResources
    return owner->pipeDirWriteStage - 1;
}

inline PipelineResources
L2MainPipe::getPipelineResources(PacketPtr pkt, TaskSource source) const
{
    return taskResourceMap.at(source) | getExtraResources(pkt, source);
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

PipelineResources
L2MainPipe::getExtraResources(PacketPtr pkt, TaskSource source) const
{
    PipelineResources extra = PipelineResources::Free;
    bool hit = owner->cache_accessor->findBlock(pkt->getAddr(), pkt->isSecure()) != nullptr;
    if (source == TaskSource::L1MSHR) {
        // acquire from L1 should write directory when hit in L2
        if (hit) {
            extra |= PipelineResources::DirWrite;

            // upgrade req does not need data sram reading
            // otherwise, the req should read from data sram
            if (!pkt->isUpgrade()) {
                extra |= PipelineResources::DataRead;
            }
        }
    } else if (source == TaskSource::L3Snoop) {
        // snoop should write directory and read data sram when hit in L2
        if (hit) {
            extra |= PipelineResources::DirWrite | PipelineResources::DataRead;
        }
    }
    return extra;
}

bool
L2MainPipe::hasMCP2Stall(PipelineResources resource, PacketPtr pkt) const
{
    // Data is multi cycle path 2,
    // so if last cycle needs to read or write data,
    // this cycle is not available to read or write data
    // With DataSram banking: only stall if accessing the same bank
    bool stall = false;
    if (resource & (PipelineResources::DataRead | PipelineResources::DataWrite)) {
        bool prevCycleHasDataAccess = (scoreboardResources[1] &
                                      (PipelineResources::DataRead |
                                       PipelineResources::DataWrite)) != 0;
        if (prevCycleHasDataAccess) {
            // Check if accessing the same DataSram bank
            bool sameBank = owner->getDataBankIdx(pkt->getAddr()) ==
                            owner->getDataBankIdx(scoreboardTasks[1].addr);
            stall |= sameBank;
        }
    }
    return stall;
}

bool
L2MainPipe::hasDirSramStall(PipelineResources resource, PacketPtr pkt) const
{
    // Dir is SRAM, read and write should not be available at the same time
    // With DirSram banking: only stall if accessing the same bank
    if (resource & PipelineResources::DirRead) {
        if ((scoreboardResources[getDirWriteStage()] &
                PipelineResources::DirWrite) != 0) {
            // Check if accessing the same DirSram bank
            bool sameBank = owner->getDirBankIdx(pkt->getAddr()) ==
                            owner->getDirBankIdx(scoreboardTasks[getDirWriteStage()].addr);
            return sameBank;
        }
    }
    return false;
}

bool
L2MainPipe::isResourceAvailable(PipelineResources resource, PacketPtr pkt) const
{
    return !hasMCP2Stall(resource, pkt) && !hasDirSramStall(resource, pkt);
}

bool
L2MainPipe::setBlockByDir(PacketPtr pkt, TaskSource source) const
{
    if (source == TaskSource::L1MSHR) {
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
L2MainPipe::isTaskAvailable(PacketPtr pkt, TaskSource source) const
{
    PipelineResources resources = getPipelineResources(pkt, source);
    bool setBlock = setBlockByDir(pkt, source);
    if (owner->dirReadBypass) {
        bool sameSet = owner->getSetIdx(pkt->getAddr()) == owner->getSetIdx(scoreboardTasks[getDirWriteStage()].addr);
        if ((source == TaskSource::L1MSHR) && sameSet) {
            // here cancel the DirRead resource, to skip the directory read&write check
            resources &= ~PipelineResources::DirRead;
        }
        setBlock = false;
    }
    bool available = (scoreboardTasks[0].source == TaskSource::NoWhere) && !setBlock &&
                     isResourceAvailable(resources, pkt);

    // record stats
    if (source == TaskSource::L1MSHR) {
        if (!available) {
            owner->stats.l1ReqEnterPipeFail++;
        }
        if (setBlock) {
            owner->stats.l1ReqPipeSetConflict++;
        }
        if (hasMCP2Stall(resources, pkt)) {
            owner->stats.l1ReqPipeMCP2Stall++;
        }
        if (hasDirSramStall(resources, pkt)) {
            owner->stats.l1ReqPipeDirSramStall++;
        }
    }
    return available;
}

void
L2MainPipe::buildTask(PacketPtr pkt, TaskSource source)
{
    scoreboardTasks[0].source = source;
    scoreboardTasks[0].pkt = pkt;
    scoreboardTasks[0].addr = pkt->getAddr();
    scoreboardResources[0] |= getPipelineResources(pkt, source);
}

void
L2MainPipe::sendMSHRGrantPkt()
{
    // Find from S5 to S3
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
