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
  public:
    WorkerPrefetcher(const WorkerPrefetcherParams &p);

    // dummy
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override {}

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    void rxHint(BaseMMU::Translation *dpp) override;

  protected:
    std::list<DeferredPacket> localBuffer;

    unsigned depth{2};

};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_WORKER_HH__
