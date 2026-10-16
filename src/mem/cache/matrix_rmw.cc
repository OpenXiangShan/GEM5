#include "mem/cache/matrix_rmw.hh"

#include <algorithm>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/MatrixRmw.hh"
#include "sim/cur_tick.hh"

namespace gem5::matrix
{

RmwController::RmwController(statistics::Group *parent, bool retention,
                             unsigned sets, unsigned max_ways)
    : statistics::Group(parent, "matrixRmw"),
      enableRetention(retention), maxWays(max_ways), perSet(sets, 0),
      ADD_STAT(acquireHits, statistics::units::Count::get(),
               "C-read requests arriving at a writable L2 block"),
      ADD_STAT(acquireUpgrades, statistics::units::Count::get(),
               "C-read requests arriving at a shared L2 block"),
      ADD_STAT(acquireMisses, statistics::units::Count::get(),
               "C-read requests arriving without a real L2 block"),
      ADD_STAT(services, statistics::units::Count::get(),
               "C-reads serviced with actual L2 write permission"),
      ADD_STAT(admissions, statistics::units::Count::get(),
               "New retained real blocks"),
      ADD_STAT(repeated, statistics::units::Count::get(),
               "C-read services on an already retained block"),
      ADD_STAT(denied, statistics::units::Count::get(),
               "Services without retention because the set quota is full"),
      ADD_STAT(temporaryDenied, statistics::units::Count::get(),
               "Services from tempBlock, which cannot be retained"),
      ADD_STAT(occupancy, statistics::units::Count::get(),
               "Currently retained real blocks"),
      ADD_STAT(occupancyMax, statistics::units::Count::get(),
               "Maximum retained real blocks"),
      ADD_STAT(maxWaysPerSet, statistics::units::Count::get(),
               "Configured retained-way quota per set"),
      ADD_STAT(victimExcluded, statistics::units::Count::get(),
               "Retained candidates excluded before replacement selection"),
      ADD_STAT(probeRevoked, statistics::units::Count::get(),
               "Retention revoked by a downstream coherence snoop"),
      ADD_STAT(upstreamRevoked, statistics::units::Count::get(),
               "Retention revoked by an upstream caching request"),
      ADD_STAT(invalidated, statistics::units::Count::get(),
               "Retention revoked by cache invalidation"),
      ADD_STAT(residenceTicks, statistics::units::Tick::get(),
               "Retained lifetime before revocation or StoreC handoff"),
      ADD_STAT(storeHandoffs, statistics::units::Count::get(),
               "Retained blocks handed to a StoreC short transaction"),
      ADD_STAT(storeCompleted, statistics::units::Count::get(),
               "Store handoffs released at final response delivery"),
      ADD_STAT(storeReacquires, statistics::units::Count::get(),
               "StoreC accesses lacking current L2 write permission")
{
    maxWaysPerSet = maxWays;
}

bool
RmwController::retained(const CacheBlk *blk) const
{
    auto it = blocks.find(blk);
    return it != blocks.end() && blk->isValid() &&
           it->second.tag == blk->getTag() &&
           it->second.secure == blk->isSecure();
}

void
RmwController::access(PacketPtr pkt, CacheBlk *blk)
{
    if (isRmwCRead(pkt)) {
        if (!blk || !blk->isValid()) {
            acquireMisses++;
        } else if (blk->isSet(CacheBlk::WritableBit)) {
            acquireHits++;
        } else {
            acquireUpgrades++;
        }
    } else if (pkt->req->hasXsMetadata()) {
        const auto metadata = pkt->req->getXsMetadata();
        if (metadata.matrixTxnKind == Request::XsMetadata::MatrixTxnKind::Store &&
            metadata.matrixTxnPhase == Request::XsMetadata::MatrixTxnPhase::Access &&
            (!blk || !blk->isSet(CacheBlk::WritableBit))) {
            storeReacquires++;
        }
    }
    if (pkt->req->isCacheInvalidate()) {
        revoke(blk, Revoke::Invalidate);
    } else if (pkt->fromCache() && pkt->needsResponse()) {
        revoke(blk, Revoke::Upstream);
    }
}

bool
RmwController::service(PacketPtr pkt, CacheBlk *blk, bool temporary)
{
    panic_if(!blk || !blk->isValid() ||
                 !blk->isSet(CacheBlk::WritableBit) ||
                 !blk->isSet(CacheBlk::ReadableBit),
             "Matrix C-read serviced without actual L2 write permission");
    services++;
    DPRINTF(MatrixRmw, "C-read service addr %#lx writable 1 temporary %d\n",
            pkt->getAddr(), temporary);
    if (!enableRetention) {
        return false;
    }
    if (temporary) {
        temporaryDenied++;
        return false;
    }
    if (retained(blk)) {
        repeated++;
        return false;
    }
    panic_if(blocks.count(blk), "Stale Matrix retention on reused block");
    auto &count = perSet.at(blk->getSet());
    if (count >= maxWays) {
        denied++;
        return false;
    }
    panic_if(++nextGeneration == 0, "Matrix block generation wrapped");
    blocks.emplace(blk, Entry{blk->getTag(), blk->isSecure(),
                             nextGeneration, curTick()});
    ++count;
    admissions++;
    occupancy = blocks.size();
    occupancyMax = std::max<double>(occupancyMax.value(), blocks.size());
    DPRINTF(MatrixRmw, "Retain addr %#lx set %u way %u generation %llu\n",
            pkt->getAddr(), blk->getSet(), blk->getWay(), nextGeneration);
    return true;
}

bool
RmwController::victimEligible(CacheBlk *blk)
{
    if (retained(blk)) {
        victimExcluded++;
        DPRINTF(MatrixRmw, "Exclude real victim set %u way %u\n",
                blk->getSet(), blk->getWay());
        return false;
    }
    return true;
}

void
RmwController::revoke(CacheBlk *blk, Revoke reason)
{
    auto it = blocks.find(blk);
    if (it == blocks.end()) {
        return;
    }
    panic_if(!retained(blk), "Matrix retention generation/tag mismatch");
    auto &count = perSet.at(blk->getSet());
    panic_if(!count, "Matrix retained-set count underflow");
    --count;
    residenceTicks += curTick() - it->second.since;
    DPRINTF(MatrixRmw, "Revoke set %u way %u generation %llu reason %u\n",
            blk->getSet(), blk->getWay(), it->second.generation,
            static_cast<unsigned>(reason));
    blocks.erase(it);
    occupancy = blocks.size();
    switch (reason) {
      case Revoke::Probe: probeRevoked++; break;
      case Revoke::Upstream: upstreamRevoked++; break;
      case Revoke::Invalidate: invalidated++; break;
      case Revoke::Store: break;
    }
}

void
RmwController::startStore(CacheBlk *blk, int port, uint64_t id)
{
    if (!retained(blk)) {
        return;
    }
    const auto generation = blocks.at(blk).generation;
    panic_if(!stores.emplace(std::make_pair(port, id), generation).second,
             "Duplicate Matrix store handoff");
    storeHandoffs++;
    revoke(blk, Revoke::Store);
}

void
RmwController::finishStore(int port, uint64_t id)
{
    // Do not touch a block here: it may have been invalidated or reused,
    // or retained anew after its old generation was handed off.
    if (stores.erase(std::make_pair(port, id))) {
        storeCompleted++;
    }
}

} // namespace gem5::matrix
