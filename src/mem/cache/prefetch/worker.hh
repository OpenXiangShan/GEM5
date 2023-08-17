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
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/WorkerPrefetcher.hh"

namespace gem5
{

struct WorkerPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class WorkerPrefetcher: public Queued
{
    bool first_call = false;
    Event *transfer_event;
  public:
    WorkerPrefetcher(const WorkerPrefetcherParams &p);

    // dummy
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override {}

    void transfer();

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override
    {
        if (!first_call) {
            first_call = true;
            schedule(transfer_event, nextCycle());
        }
    };

    void rxHint(BaseMMU::Translation *dpp) override;

    bool hasHintsWaiting() override { return !localBuffer.empty(); }

  protected:
    boost::compute::detail::lru_cache<Addr, Addr> pfLRUFilter;

    std::list<DeferredPacket> localBuffer;

    unsigned depth{4};

};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_WORKER_HH__
