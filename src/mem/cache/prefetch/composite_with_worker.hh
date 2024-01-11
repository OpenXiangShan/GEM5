#ifndef __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_HH__
#define __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_HH__

#include "mem/cache/prefetch/cdp.hh"
#include "mem/cache/prefetch/worker.hh"
#include "params/CompositeWithWorkerPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class CompositeWithWorkerPrefetcher: public WorkerPrefetcher
{
  public:
    CompositeWithWorkerPrefetcher(const CompositeWithWorkerPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addresses) override;

    void rxHint(BaseMMU::Translation *dpp) override;
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override;

    bool hasHintsWaiting() override { return !localBuffer.empty(); }

    void setCache(BaseCache *_cache) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
  private:

    CDP *cdp;

    bool offloadLowAccuracy = true;

};

} // namespace prefetch
} // namespace gem5


#endif // __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_HH__