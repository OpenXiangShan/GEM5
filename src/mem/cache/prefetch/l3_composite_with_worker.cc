#include "mem/cache/prefetch/l3_composite_with_worker.hh"

namespace gem5
{
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

L3CompositeWithWorkerPrefetcher::L3CompositeWithWorkerPrefetcher(
    const L3CompositeWithWorkerPrefetcherParams &p)
    : CompositeWithWorkerPrefetcher(p),
      cmc(p.cmc),
      enableCMC(p.enable_cmc)
{
    // CMC shares the parent's bounded filter with the worker.  Qualifying the
    // key with ContextID prevents one SMT context from suppressing another.
    setSharedFilterContextQualified(true);
    cmc->setSharedFilterContextQualified(true);
    cmc->filter = &pfLRUFilter;
}

void
L3CompositeWithWorkerPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
    PrefetchSourceType source, bool miss_repeat)
{
    if (enableCMC) {
        cmc->doPrefetch(pfi, addresses, late, source, false);
    }
}

void
L3CompositeWithWorkerPrefetcher::setParentInfo(
    System *sys, ProbeManager *pm, CacheAccessor *cache, unsigned blk_size)
{
    cmc->setParentInfo(sys, pm, cache, blk_size);
    CompositeWithWorkerPrefetcher::setParentInfo(sys, pm, cache, blk_size);
}

bool
L3CompositeWithWorkerPrefetcher::GetPFRequestsFromBuffer(
    std::vector<AddrPriority> &addresses)
{
    // Queued's PF event sends at most one candidate per cycle.  Do not consume
    // another buffered candidate while the ready queue has no free slot.
    if (pfq.size() >= queueSize) {
        return false;
    }

    // Worker::transfer() already filled the ready queue this cycle.  Keep
    // worker hints ahead of the CMC stream, matching the L2 composite policy.
    if (ticksToCycles(latestTransferTick) == ticksToCycles(curTick())) {
        return false;
    }

    // Keep the base buffer usable for future components; CMC is the only
    // component that currently owns a delayed sequence in this composite.
    if (Queued::hasPFRequestsInBuffer()) {
        return Queued::GetPFRequestsFromBuffer(addresses);
    }

    // CMC owns the delayed temporal sequence, so let it supply the next
    // candidate here.
    return cmc->GetPFRequestsFromBuffer(addresses);
}

bool
L3CompositeWithWorkerPrefetcher::hasPFRequestsInBuffer()
{
    return Queued::hasPFRequestsInBuffer() || cmc->hasPFRequestsInBuffer();
}

} // namespace prefetch
} // namespace gem5
