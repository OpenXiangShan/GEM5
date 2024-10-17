#ifndef __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__
#define __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__

#include <vector>

#include "mem/cache/prefetch/bop.hh"
#include "mem/cache/prefetch/cdp.hh"
#include "mem/cache/prefetch/cmc.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"
#include "params/L2CompositeWithWorkerPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class L2CompositeWithWorkerPrefetcher : public CompositeWithWorkerPrefetcher
{
  public:
    L2CompositeWithWorkerPrefetcher(const L2CompositeWithWorkerPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override {}

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType source, bool miss_repeat) override;

    void addHintDownStream(Base *down_stream) override
    {
        hintDownStream = down_stream;
        cdp->addHintDownStream(down_stream);
    }
    void rxHint(BaseMMU::Translation *dpp) override;
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override;

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;

    void notifyFill(const PacketPtr &pkt) override;
    void notifyIns(int ins_num) override { cdp->notifyIns(ins_num); }

  private:
    CDP *cdp;
    BOP* largeBOP;
    BOP* smallBOP;
    CMCPrefetcher* cmc;

    const bool enableBOP;
    const bool enableCDP;
    const bool enableCMC;

    bool offloadLowAccuracy = true;
};

}  // namespace prefetch
}  // namespace gem5


#endif  // __MEM_CACHE_PREFETCH_COMPOITE_WITH_WORKER_L2_HH__
