#include "mem/cache/prefetch/composite_with_worker.hh"

#include "debug/HWPrefetch.hh"

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
    cdp->pfHitNotify(accuracy, pf_source, pkt, addressGenBuffer);
    if (addressGenBuffer.size()) {
        assert(pkt->req->hasVaddr());
        postNotifyInsert(pkt, addressGenBuffer);
    }
    addressGenBuffer.clear();
}

void
CompositeWithWorkerPrefetcher::postNotifyInsert(const PacketPtr &trigger_pkt, std::vector<AddrPriority> &addresses)
{
    PrefetchInfo pfi(trigger_pkt, trigger_pkt->req->getVaddr(), false);
    size_t max_pfs = getMaxPermittedPrefetches(addresses.size());
    // Queue up generated prefetches
    size_t num_pfs = 0;
    for (AddrPriority &addr_prio : addresses) {

        // Block align prefetch address
        addr_prio.addr = blockAddress(addr_prio.addr);

        if (!samePage(addr_prio.addr, pfi.getAddr())) {
            statsQueued.pfSpanPage += 1;

            if (hasBeenPrefetched(trigger_pkt->getAddr(), trigger_pkt->isSecure())) {
                statsQueued.pfUsefulSpanPage += 1;
            }
        }

        bool can_cross_page = (tlb != nullptr);
        if (can_cross_page || samePage(addr_prio.addr, pfi.getAddr())) {
            PrefetchInfo new_pfi(pfi, addr_prio.addr);
            new_pfi.setXsMetadata(Request::XsMetadata(addr_prio.pfSource, addr_prio.depth));
            statsQueued.pfIdentified++;
            DPRINTF(HWPrefetch, "Found a pf candidate addr: %#x, inserting into prefetch queue.\n", new_pfi.getAddr());
            // Create and insert the request
            insert(trigger_pkt, new_pfi, addr_prio);
            num_pfs += 1;
            if (num_pfs == max_pfs) {
                break;
            }
        } else {
            DPRINTF(HWPrefetch, "Ignoring page crossing prefetch.\n");
        }
    }
}

void
CompositeWithWorkerPrefetcher::setCache(BaseCache *_cache)
{
    Base::setCache(_cache);
    cdp->setCache(_cache);
}

void
CompositeWithWorkerPrefetcher::notifyFill(const PacketPtr &pkt)
{
    cdp->notifyFill(pkt, addressGenBuffer);
    if (addressGenBuffer.size()) {
        assert(pkt->req->hasVaddr());
        postNotifyInsert(pkt, addressGenBuffer);
    }
    addressGenBuffer.clear();
}

}  // namespace prefetch
}  // namespace gem5