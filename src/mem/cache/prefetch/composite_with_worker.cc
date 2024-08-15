#include "mem/cache/prefetch/composite_with_worker.hh"

#include "debug/HWPrefetch.hh"

namespace gem5
{
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

CompositeWithWorkerPrefetcher::CompositeWithWorkerPrefetcher(const CompositeWithWorkerPrefetcherParams &p)
    : WorkerPrefetcher(p)
{
}

void
CompositeWithWorkerPrefetcher::rxHint(BaseMMU::Translation *dpp)
{
    WorkerPrefetcher::rxHint(dpp);
}

void
CompositeWithWorkerPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    WorkerPrefetcher::notify(pkt, pfi);
    Queued::notify(pkt, pfi);
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

            if (hasEverBeenPrefetched(trigger_pkt->getAddr(), trigger_pkt->isSecure())) {
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
CompositeWithWorkerPrefetcher::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size)
{
    Base::setParentInfo(sys, pm, _cache, blk_size);
}

}  // namespace prefetch
}  // namespace gem5