#include "mem/cache/xs_l3/L3MainPipe.hh"

#include <algorithm>
#include <cstdint>

#include "base/trace.hh"
#include "debug/L3MainPipe.hh"
#include "mem/cache/xs_l3/L3CacheSlice.hh"
#include "mem/cache/xs_l3/L3PipelineResources.hh"
#include "mem/packet.hh"

namespace gem5
{

L3MainPipe::L3MainPipe(L3CacheSlice* _owner, unsigned depth)
    : owner(_owner),
      cur_cycle(0)
{
    scoreboardResources.resize(depth, L3PipelineResources::Free);
    scoreboardTasks.resize(depth, PipelineTask(L3TaskSource::NoWhere, nullptr));

    // construct the taskResourceMap
    taskResourceMap[L3TaskSource::L2MSHR]        = L3PipelineResources::DirRead;
    taskResourceMap[L3TaskSource::L2WQ]          = L3PipelineResources::DirRead  |
                                                 L3PipelineResources::DirWrite |
                                                 L3PipelineResources::DataWrite|
                                                 L3PipelineResources::GrantBuf;
    taskResourceMap[L3TaskSource::MemSnoop]       = L3PipelineResources::DirRead;
    taskResourceMap[L3TaskSource::L3MSHRGrant]   = L3PipelineResources::DirRead  |
                                                 L3PipelineResources::DataRead |
                                                 L3PipelineResources::GrantBuf;
    taskResourceMap[L3TaskSource::L3MSHRRelease] = L3PipelineResources::DataWrite;
    taskResourceMap[L3TaskSource::L3PF]          = L3PipelineResources::DirRead;
}

inline uint64_t
L3MainPipe::getDirWriteStage() const
{
    // -1 is to get the index of scoreboardTasks & scoreboardResources
    return owner->pipeDirWriteStage - 1;
}

inline L3PipelineResources
L3MainPipe::getPipelineResources(PacketPtr pkt, L3TaskSource source) const
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
    scoreboardResources.emplace_front(L3PipelineResources::Free);
    scoreboardTasks.emplace_front(L3TaskSource::NoWhere, nullptr);
}

L3PipelineResources
L3MainPipe::getExtraResources(PacketPtr pkt, L3TaskSource source) const
{
    L3PipelineResources extra = L3PipelineResources::Free;
    bool hit = owner->cache_accessor->findBlock(pkt->getAddr(), pkt->isSecure()) != nullptr;
    if (source == L3TaskSource::L2MSHR) {
        // acquire from L1 should write directory when hit in L2
        if (hit) {
            extra |= L3PipelineResources::DirWrite;

            // upgrade req does not need data sram reading
            // otherwise, the req should read from data sram
            if (!pkt->isUpgrade()) {
                extra |= L3PipelineResources::DataRead;
            }
        }
    } else if (source == L3TaskSource::MemSnoop) {
        // snoop should write directory and read data sram when hit in L2
        if (hit) {
            extra |= L3PipelineResources::DirWrite | L3PipelineResources::DataRead;
        }
    }
    return extra;
}

bool
L3MainPipe::hasMCP2Stall(L3PipelineResources resource, PacketPtr pkt) const
{
    // Data is multi cycle path 2,
    // so if last cycle needs to read or write data,
    // this cycle is not available to read or write data
    // With DataSram banking: only stall if accessing the same bank
    bool stall = false;
    if (scoreboardResources.size() < 2) {
        return false;
    }
    if (resource & (L3PipelineResources::DataRead | L3PipelineResources::DataWrite)) {
        bool prevCycleHasDataAccess = (scoreboardResources[1] &
                                      (L3PipelineResources::DataRead |
                                       L3PipelineResources::DataWrite)) != 0;
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
L3MainPipe::hasDirSramStall(L3PipelineResources resource, PacketPtr pkt) const
{
    // Dir is SRAM, read and write should not be available at the same time
    // With DirSram banking: only stall if accessing the same bank
    if (scoreboardResources.empty()) {
        return false;
    }
    const auto dirWriteStage = getDirWriteStage();
    if (dirWriteStage >= scoreboardResources.size()) {
        return false;
    }
    if (resource & L3PipelineResources::DirRead) {
        if ((scoreboardResources[dirWriteStage] &
                L3PipelineResources::DirWrite) != 0) {
            // Check if accessing the same DirSram bank
            bool sameBank = owner->getDirBankIdx(pkt->getAddr()) ==
                            owner->getDirBankIdx(scoreboardTasks[dirWriteStage].addr);
            return sameBank;
        }
    }
    return false;
}

bool
L3MainPipe::isResourceAvailable(L3PipelineResources resource, PacketPtr pkt) const
{
    return !hasMCP2Stall(resource, pkt) && !hasDirSramStall(resource, pkt);
}

bool
L3MainPipe::setBlockByDir(PacketPtr pkt, L3TaskSource source) const
{
    if (source == L3TaskSource::L2MSHR) {
        if (scoreboardTasks.size() <= 1) {
            return false;
        }
        const auto max_stage = std::min<uint64_t>(getDirWriteStage(),
                                                  scoreboardTasks.size() - 1);
        for (uint64_t i = 1; i <= max_stage; i++) {
            bool valid = scoreboardTasks[i].source != L3TaskSource::NoWhere;
            bool sameSet = owner->getSetIdx(pkt->getAddr()) == owner->getSetIdx(scoreboardTasks[i].addr);
            if (valid && sameSet) {
                return true;
            }
        }
    }
    return false;
}

bool
L3MainPipe::isTaskAvailable(PacketPtr pkt, L3TaskSource source) const
{
    L3PipelineResources resources = getPipelineResources(pkt, source);
    bool setBlock = setBlockByDir(pkt, source);
    if (owner->dirReadBypass) {
        const auto dirWriteStage = getDirWriteStage();
        if (dirWriteStage < scoreboardTasks.size()) {
            bool sameSet = owner->getSetIdx(pkt->getAddr()) ==
                           owner->getSetIdx(scoreboardTasks[dirWriteStage].addr);
            if ((source == L3TaskSource::L2MSHR) && sameSet) {
                // Cancel DirRead to model a same-set bypass path.
                resources &= ~L3PipelineResources::DirRead;
            }
        }
        setBlock = false;
    }
    bool available = (scoreboardTasks[0].source == L3TaskSource::NoWhere) && !setBlock &&
                     isResourceAvailable(resources, pkt);

    // record stats
    if (source == L3TaskSource::L2MSHR) {
        if (!available) {
            owner->stats.l2ReqEnterPipeFail++;
        }
        if (setBlock) {
            owner->stats.l2ReqPipeSetConflict++;
        }
        if (hasMCP2Stall(resources, pkt)) {
            owner->stats.l2ReqPipeMCP2Stall++;
        }
        if (hasDirSramStall(resources, pkt)) {
            owner->stats.l2ReqPipeDirSramStall++;
        }
    }
    return available;
}

void
L3MainPipe::buildTask(PacketPtr pkt, L3TaskSource source)
{
    scoreboardTasks[0].source = source;
    scoreboardTasks[0].pkt = pkt;
    scoreboardTasks[0].addr = pkt->getAddr();
    scoreboardResources[0] |= getPipelineResources(pkt, source);
}

void
L3MainPipe::sendMSHRGrantPkt()
{
    if (scoreboardTasks.size() <= 2) {
        return;
    }
    // Find from the latest stage to S3.
    // Later pipeline stages have higher grant priority
    for (int i = static_cast<int>(scoreboardTasks.size()) - 1; i >= 2; i--) {
        bool isGrant = scoreboardTasks[i].source == L3TaskSource::L3MSHRGrant;
        bool needGrantBuf = scoreboardResources[i] & L3PipelineResources::GrantBuf;
        if (isGrant && needGrantBuf) {
            PacketPtr pkt = scoreboardTasks[i].pkt;
            if (!owner->inner_mem_port.sendTimingResp(pkt)) {
                panic("L3 cache recvTimingResp failed");
            } else {
                scoreboardResources[i] &= ~L3PipelineResources::GrantBuf;
            }
            break;
        }
    }
}

bool
L3MainPipe::hasWork() const
{
    return std::any_of(scoreboardTasks.begin(), scoreboardTasks.end(),
                       [](PipelineTask s){ return s.source != L3TaskSource::NoWhere; });
}

} // namespace gem5
