#ifndef __MEM_CACHE_PREFETCH_COMPOSITE_WITH_WORKER_L3_HH__
#define __MEM_CACHE_PREFETCH_COMPOSITE_WITH_WORKER_L3_HH__

#include <vector>

#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"
#include "params/L3CompositeWithWorkerPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

/**
 * L3 worker that trains one CMC instance and consumes L2 worker hints.
 *
 * The parent owns the worker queue and shared filter.  CMC keeps its temporal
 * sequence buffer, so the wrapper only needs to arbitrate that buffer when
 * Queued is using its optional prefetch buffer.
 */
class L3CompositeWithWorkerPrefetcher : public CompositeWithWorkerPrefetcher
{
  public:
    L3CompositeWithWorkerPrefetcher(
        const L3CompositeWithWorkerPrefetcherParams &p);

    // The two-argument entry point is unused; L3 training uses the extended
    // entry point so that CMC sees late-hit and source information.
    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override {}

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType source,
                           bool miss_repeat) override;

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *cache,
                       unsigned blk_size) override;

    bool GetPFRequestsFromBuffer(
        std::vector<AddrPriority> &addresses) override;
    bool hasPFRequestsInBuffer() override;

  private:
    CMCPrefetcher *cmc;
    const bool enableCMC;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_COMPOSITE_WITH_WORKER_L3_HH__
