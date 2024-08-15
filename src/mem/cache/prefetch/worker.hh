/**
 * @file
 * Describes a prefetcher.
 */

#ifndef __MEM_CACHE_PREFETCH_WORKER_HH__
#define __MEM_CACHE_PREFETCH_WORKER_HH__

#include <list>
#include <string>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/WorkerPrefetcher.hh"

namespace gem5
{

struct WorkerPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class WorkerPrefetcher : public Queued
{
    bool firstCall = false;
    Event *transferEvent;

  public:
    WorkerPrefetcher(const WorkerPrefetcherParams &p);

    // dummy
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override {}

    virtual void transfer();

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override
    {
        if (!firstCall) {
            firstCall = true;
            schedule(transferEvent, nextCycle());
        }
    };

    void rxHint(BaseMMU::Translation *dpp) override;
    std::pair<long, long> rxMembusRatio(RequestorID requestorId) override
    {
        // long totalMissCount = cache->stats.cmd[MemCmd::ReadExReq]->misses.total() +
        //                       cache->stats.cmd[MemCmd::ReadSharedReq]->misses.total();
        // long missCount = cache->stats.cmd[MemCmd::ReadExReq]->misses[requestorId].value() +
        //                  cache->stats.cmd[MemCmd::ReadSharedReq]->misses[requestorId].value();
        // return std::pair<long, long>(missCount, totalMissCount);
        return std::pair<long, long>(0, 0);
    };
    void notifyIns(int ins_num) override
    {
        if (hasHintDownStream()) {
            hintDownStream->notifyIns(ins_num);
        }
    }

    // dummy
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override { return; }

    bool hasHintsWaiting() override { return !localBuffer.empty(); }

    struct WorkerStats : public statistics::Group
    {
        WorkerStats(statistics::Group *parent);

        statistics::Scalar hintsReceived;
        statistics::Scalar hintsOffloaded;

    } workerStats;

  protected:
    boost::compute::detail::lru_cache<Addr, Addr> pfLRUFilter;

    std::list<DeferredPacket> localBuffer;

    unsigned depth{4};
};

}  // namespace prefetch
}  // namespace gem5

#endif  // __MEM_CACHE_PREFETCH_WORKER_HH__
