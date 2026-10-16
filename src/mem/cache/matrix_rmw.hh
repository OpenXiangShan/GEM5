#ifndef __MEM_CACHE_MATRIX_RMW_HH__
#define __MEM_CACHE_MATRIX_RMW_HH__

#include <cstdint>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/cache_blk.hh"
#include "mem/cache/matrix_rmw_intent.hh"

namespace gem5::matrix
{

/** Bounded hints on real blocks. Cache coherence remains the permission owner. */
class RmwController : public statistics::Group
{
  public:
    enum class Revoke { Probe, Upstream, Invalidate, Store };

    RmwController(statistics::Group *parent, bool retention,
                  unsigned sets, unsigned max_ways);

    void access(PacketPtr pkt, CacheBlk *blk);
    bool service(PacketPtr pkt, CacheBlk *blk, bool temporary);
    bool victimEligible(CacheBlk *blk);
    void revoke(CacheBlk *blk, Revoke reason);
    void startStore(CacheBlk *blk, int port, uint64_t id);
    void finishStore(int port, uint64_t id);
    bool retained(const CacheBlk *blk) const;
    unsigned retainedCount() const { return blocks.size(); }

  private:
    struct Entry
    {
        Addr tag;
        bool secure;
        uint64_t generation;
        Tick since;
    };
    const bool enableRetention;
    const unsigned maxWays;
    uint64_t nextGeneration = 0;
    std::vector<unsigned> perSet;
    std::unordered_map<const CacheBlk *, Entry> blocks;
    // Only active store handoffs, removed at final response delivery.
    std::map<std::pair<int, uint64_t>, uint64_t> stores;

    statistics::Scalar acquireHits;
    statistics::Scalar acquireUpgrades;
    statistics::Scalar acquireMisses;
    statistics::Scalar services;
    statistics::Scalar admissions;
    statistics::Scalar repeated;
    statistics::Scalar denied;
    statistics::Scalar temporaryDenied;
    statistics::Scalar occupancy;
    statistics::Scalar occupancyMax;
    statistics::Scalar maxWaysPerSet;
    statistics::Scalar victimExcluded;
    statistics::Scalar probeRevoked;
    statistics::Scalar upstreamRevoked;
    statistics::Scalar invalidated;
    statistics::Scalar residenceTicks;
    statistics::Scalar storeHandoffs;
    statistics::Scalar storeCompleted;
    statistics::Scalar storeReacquires;
};

} // namespace gem5::matrix

#endif // __MEM_CACHE_MATRIX_RMW_HH__
