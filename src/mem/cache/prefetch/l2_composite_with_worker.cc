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
    cdp->pfLRUFilter = &pfLRUFilter;
    largeBOP->filter = &pfLRUFilter;
    smallBOP->filter = &pfLRUFilter;
    largeBOP->sharePCValidationConfidenceWith(*smallBOP);
    largeBOP->shareDirectQualityGateWith(*smallBOP);
    cmc->filter = &pfLRUFilter;
    despacitoStream->filter = &pfLRUFilter;
    cdp->parentRid = p.sys->getRequestorId(this);
}

void
L2CompositeWithWorkerPrefetcher::prefetchUnused(Addr paddr, PrefetchSourceType pfSource)
{
    Base::prefetchUnused(pfSource);
    if (archDBer && pfSource == PrefetchSourceType::HWP_BOP) {
        archDBer->bopValidationOutcomeTraceWrite(
            curTick(), "unused", paddr, 0,
            static_cast<int>(pfSource), false, false);
    }
    if (pfSource == PrefetchSourceType::HWP_BOP) {
        largeBOP->notifyGlobalBOPOutcome(false);
        largeBOP->notifyDirectQualityOutcome(paddr, false);
    }
    if (pfSource == PrefetchSourceType::CDP) {
        cdp->recordUnusedPrefetch(paddr);
    }
}

void
L2CompositeWithWorkerPrefetcher::prefetchUseful(
    Addr paddr, PrefetchSourceType pfSource)
{
    if (pfSource == PrefetchSourceType::HWP_BOP) {
        largeBOP->notifyGlobalBOPOutcome(true);
        largeBOP->notifyDirectQualityOutcome(paddr, true);
    }
}

PacketPtr
L2CompositeWithWorkerPrefetcher::getPacket()
{
    PacketPtr pkt = Queued::getPacket();
    if (!pkt || !pkt->req->hasXsMetadata())
        return pkt;

    const auto metadata = pkt->req->getXsMetadata();
    if (metadata.prefetchSource == PrefetchSourceType::HWP_BOP &&
        metadata.directQualityTokenValid) {
        largeBOP->notifyDirectQualityIssued(
            pkt->getAddr(), metadata.directQualityKind,
            metadata.directQualitySet, metadata.directQualityWay,
            metadata.directQualityGeneration);
    }
    return pkt;
}

void
L2CompositeWithWorkerPrefetcher::addToQueue(std::list<DeferredPacket> &queue, DeferredPacket &dpp)
{
    if (&queue == &pfq) {
        // Check whether the cdp prefetch request needs to be filtered out
        if (dpp.pkt->req->getXsMetadata().prefetchSource == PrefetchSourceType::CDP) {
            if (cdp->needFilter(dpp.pkt->req->getPaddr())) {
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
        largeBOP->calculatePrefetch(
            pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP,
            activeBOPReplayEventId);
        smallBOP->calculatePrefetch(
            pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP,
            activeBOPReplayEventId);
        largeBOP->commitPCValidationConfidence();
    }
    if (enableDespacitoStream) {
        despacitoStream->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::DespacitoStream);
    }
}

void
L2CompositeWithWorkerPrefetcher::rxHint(BaseMMU::Translation *dpp)
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
    // don't offload or accurate enough
    WorkerPrefetcher::rxHint(dpp);
}

void
L2CompositeWithWorkerPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    if (pkt->isDemand() && pkt->isRead() && !pkt->isWrite())
        largeBOP->notifyDirectQualityDemand();
    if (archDBer && archDBer->dumpBopReplayTrace) {
        activeBOPReplayEventId = ++nextBOPReplayEventId;
        if (pkt->isDemand() && pkt->isRead() && !pkt->isWrite()) {
            const Addr pc = pfi.hasPC() ? pfi.getPC() : 0;
            archDBer->bopReplayDemandTraceWrite(
                activeBOPReplayEventId, curTick(), blockAddress(pfi.getAddr()),
                pc, pfi.hasPC(), pfi.isCacheMiss(),
                static_cast<int>(pfi.getXsMetadata().prefetchSource),
                pfi.isPfFirstHit(), pfi.isPfHit());
        }
    }
    WorkerPrefetcher::notify(pkt, pfi);
    Queued::notify(pkt, pfi);
    activeBOPReplayEventId = 0;
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
