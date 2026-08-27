#include "mem/cache/prefetch/l2_composite_with_worker.hh"

#include "debug/CDPFilter.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"

namespace gem5
{
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

L2CompositeWithWorkerPrefetcher::L2CompositeWithWorkerPrefetcher(const L2CompositeWithWorkerPrefetcherParams &p)
    : CompositeWithWorkerPrefetcher(p),
      cdp(p.cdp),
      largeBOP(p.bop_large),
      smallBOP(p.bop_small),
      cmc(p.cmc),
      despacitoStream(p.despacito_stream),
      enableBOP(p.enable_bop),
      enableCDP(p.enable_cdp),
      enableCMC(p.enable_cmc),
      enableDespacitoStream(p.enable_despacito_stream)
{
    setSharedFilterContextQualified(true);
    cdp->setSharedFilterContextQualified(true);
    largeBOP->setSharedFilterContextQualified(true);
    smallBOP->setSharedFilterContextQualified(true);
    cmc->setSharedFilterContextQualified(true);
    despacitoStream->setSharedFilterContextQualified(true);
    cdp->pfLRUFilter = &pfLRUFilter;
    largeBOP->filter = &pfLRUFilter;
    smallBOP->filter = &pfLRUFilter;
    cmc->filter = &pfLRUFilter;
    despacitoStream->filter = &pfLRUFilter;
    cdp->parentRid = p.sys->getRequestorId(this);
}

void
L2CompositeWithWorkerPrefetcher::prefetchUnused(Addr paddr, PrefetchSourceType pfSource)
{
    Base::prefetchUnused(pfSource);
    if (pfSource == PrefetchSourceType::CDP) {
        cdp->recordUnusedPrefetch(paddr);
    }
}

void
L2CompositeWithWorkerPrefetcher::addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp)
{
    if (&queue == &pfq) {
        // Check whether the cdp prefetch request needs to be filtered out
        if (dpp.pkt->req->getXsMetadata().prefetchSource == PrefetchSourceType::CDP) {
            if (cdp->needFilter(dpp.pkt->req->getPaddr())) {
                completeDeferredStagedPrefetch(dpp, false);
                delete dpp.pkt;
                return;
            }
        }
    }
    Queued::addToQueue(queue, dpp);
}

void
L2CompositeWithWorkerPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses,
                                                   bool late, PrefetchSourceType pf_source, bool miss_repeat)
{
    if (enableCMC) {
        cmc->doPrefetch(pfi, addresses, late, pf_source, false);
    }
    if (enableCDP) {
        cdp->calculatePrefetch(pfi, addresses);
    }
    if (enableBOP) {
        largeBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);
        smallBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);
    }
    if (enableDespacitoStream) {
        despacitoStream->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::DespacitoStream);
    }
}

void
L2CompositeWithWorkerPrefetcher::rxHint(BaseMMU::Translation *dpp)
{
    auto ptr = reinterpret_cast<DeferredPacket *>(dpp);
    // A STEP L2 target is a terminal placement decision, not merely an
    // initial hint. Keep the request in this L2 even when the existing
    // low-accuracy policy would otherwise forward it to L3.
    if (ptr->pfahead && ptr->pfahead_host == cache->level() &&
        ptr->pfInfo.getXsMetadata().prefetchSource == PrefetchSourceType::STEP) {
        WorkerPrefetcher::rxHint(dpp);
        return;
    }

    if (offloadLowAccuracy) {
        float cdp_ratio =
            (prefetchStats.pfDequeued_srcs[PrefetchSourceType::CDP].value()) /
            (prefetchStats.pfDequeued.value());
        float acc = (prefetchStats.pfUseful_srcs[ptr->pfInfo.getXsMetadata().prefetchSource].value()) /
                    (prefetchStats.pfDequeued_srcs[ptr->pfInfo.getXsMetadata().prefetchSource].value());

        if (hasHintDownStream() && cdp_ratio > 0.5 && acc < 0.5) {
            hintDownStream->rxHint(dpp);
            return;
        }
    }
    // don't offload or accurate enough
    WorkerPrefetcher::rxHint(dpp);
}

void
L2CompositeWithWorkerPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    WorkerPrefetcher::notify(pkt, pfi);
    Queued::notify(pkt, pfi);
}

void
L2CompositeWithWorkerPrefetcher::recvCustomInfoFrmUpStream(CustomPfInfo& info)
{
    cdp->recvRivalCoverage(info);
}

void
L2CompositeWithWorkerPrefetcher::pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt)
{
    if (enableCDP) {
        cdp->pfHitNotify(accuracy, pf_source, pkt, addressGenBuffer);
    }

    if (addressGenBuffer.size()) {
        assert(pkt->req->hasVaddr());
        postNotifyInsert(pkt, addressGenBuffer);
    }
    addressGenBuffer.clear();
}

void
L2CompositeWithWorkerPrefetcher::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size)
{
    cdp->setParentInfo(sys, pm, _cache, blk_size);
    cdp->setStatsPtr(&prefetchStats);
    largeBOP->setParentInfo(sys, pm, _cache, blk_size);
    smallBOP->setParentInfo(sys, pm, _cache, blk_size);
    cmc->setParentInfo(sys, pm, _cache, blk_size);
    despacitoStream->setParentInfo(sys, pm, _cache, blk_size);
    CompositeWithWorkerPrefetcher::setParentInfo(sys, pm, _cache, blk_size);
}

void
L2CompositeWithWorkerPrefetcher::notifyFill(const PacketPtr &pkt)
{
    if (enableCDP) {
        cdp->notifyFill(pkt, addressGenBuffer);
    }

    if (addressGenBuffer.size()) {
        assert(pkt->req->hasVaddr());
        postNotifyInsert(pkt, addressGenBuffer);
    }
    addressGenBuffer.clear();
}
bool 
L2CompositeWithWorkerPrefetcher::GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) 
{
    //here we decide which to send for this cycle
    //L1 Streamstride>berti>SMS>CMC
    //L2 Streamstride>SMS>vBOP>pbop>TP
    if(pfq.size() == queueSize) {
        return false;
    }
    bool L2PFsent = false;
    L2PFsent = ticksToCycles(latestTransferTick) == ticksToCycles(curTick());
    if (!L2PFsent && largeBOP->hasPFRequestsInBuffer()){
        L2PFsent = largeBOP->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && smallBOP->hasPFRequestsInBuffer()){
        L2PFsent = smallBOP->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && despacitoStream->hasPFRequestsInBuffer()){
        L2PFsent = despacitoStream->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && cdp->hasPFRequestsInBuffer()){
        L2PFsent = cdp->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && cmc->hasPFRequestsInBuffer()){
        L2PFsent = cmc->GetPFRequestsFromBuffer(addresses);
    }
    // For now we dont have L3PF
    // bool L3PFsent = false;
    // L3PFsent = stridestream_pfFilter_l2l3.GetPFAddrL3(addresses);
    // if (!L3PFsent){
    //     L3PFsent = sms_pfFilter.GetPFAddrL3(addresses);
    // }
    return L2PFsent;
}
bool L2CompositeWithWorkerPrefetcher::hasPFRequestsInBuffer() {
    return  largeBOP->hasPFRequestsInBuffer() ||
            smallBOP->hasPFRequestsInBuffer() ||
            cmc->hasPFRequestsInBuffer() ||
            cdp->hasPFRequestsInBuffer() ||
            despacitoStream->hasPFRequestsInBuffer();
        }
}  // namespace prefetch
}  // namespace gem5
