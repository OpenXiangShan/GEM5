#include "mem/cache/prefetch/composite_with_worker.hh"

namespace gem5
{
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

CompositeWithWorkerPrefetcher::CompositeWithWorkerPrefetcher(const CompositeWithWorkerPrefetcherParams &p)
    : WorkerPrefetcher(p), cdp(p.cdp)
{
    cdp->pfLRUFilter = &pfLRUFilter;
}

void
CompositeWithWorkerPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    cdp->calculatePrefetch(pfi, addresses);
}

void
CompositeWithWorkerPrefetcher::rxHint(BaseMMU::Translation *dpp)
{
    if (offloadLowAccuracy) {
        auto ptr = reinterpret_cast<DeferredPacket *>(dpp);
        float cdp_ratio =
            (prefetchStats.pfIssued_srcs[PrefetchSourceType::CDP].value()) / (prefetchStats.pfIssued.total());
        float acc = (prefetchStats.pfUseful_srcs[ptr->pfInfo.getXsMetadata().prefetchSource].value()) /
                    (prefetchStats.pfIssued_srcs[ptr->pfInfo.getXsMetadata().prefetchSource].value());

        if (hasHintDownStream() && cdp_ratio > 0.5 && acc < 0.5) {
            hintDownStream->rxHint(dpp);
            return;
        }
    }

    WorkerPrefetcher::rxHint(dpp);
}

void
CompositeWithWorkerPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    WorkerPrefetcher::notify(pkt, pfi);
    Queued::notify(pkt, pfi);
}

void
CompositeWithWorkerPrefetcher::pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt)
{
    cdp->pfHitNotify(accuracy, pf_source, pkt);
}

void
CompositeWithWorkerPrefetcher::setCache(BaseCache *_cache)
{
    Base::setCache(_cache);
    cdp->setCache(_cache);
}

}  // namespace prefetch
}  // namespace gem5