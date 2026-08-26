#include "mem/cache/prefetch/l2_composite_with_worker.hh"

#include "debug/CDPFilter.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"

namespace gem5
{
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

namespace
{

const char *
directQualityOutcomeName(DirectQualityGate::TraceOutcome outcome)
{
    switch (outcome) {
      case DirectQualityGate::TraceOutcome::UsefulDemand:
        return "useful";
      case DirectQualityGate::TraceOutcome::UnusedExpiry:
        return "unused";
      case DirectQualityGate::TraceOutcome::UnknownFeedbackReplacement:
        return "unknown_feedback_replacement";
      case DirectQualityGate::TraceOutcome::UnknownOwnerReplaced:
        return "unknown_owner_replaced";
    }
    panic("Unknown direct-quality trace outcome\n");
}

} // anonymous namespace

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
    largeBOP->sharePCValidationConfidenceWith(*smallBOP);
    largeBOP->shareDirectQualityGateWith(*smallBOP);
    if (archDBer && archDBer->dumpBopDirectQualityTrace)
        largeBOP->setDirectQualityTraceSink(this);
    cmc->filter = &pfLRUFilter;
    despacitoStream->filter = &pfLRUFilter;
    cdp->parentRid = p.sys->getRequestorId(this);
}

void
L2CompositeWithWorkerPrefetcher::directQualityTraceConfig(
    const DirectQualityGate::Config &config)
{
    if (archDBer) {
        archDBer->bopDirectQualityMetaTraceWrite(config);
    }
}

void
L2CompositeWithWorkerPrefetcher::directQualityTraceCandidate(
    uint64_t event_sequence, Addr pc, uint8_t kind,
    Addr trigger_line, Addr candidate_line, DirectQualityGate::State state,
    bool allowed, bool sampled)
{
    if (archDBer) {
        archDBer->bopDirectQualityCandidateTraceWrite(
            curTick(), event_sequence, pc, kind, trigger_line, candidate_line,
            static_cast<uint8_t>(state), allowed, sampled);
    }
}

void
L2CompositeWithWorkerPrefetcher::directQualityTraceIssue(
    uint64_t event_sequence, uint64_t feedback_id,
    uint64_t candidate_demand_sequence, Addr line, uint8_t kind)
{
    if (archDBer) {
        // ArchDB preserves the historical "Issue" table name.  The gate
        // invokes this before local filtering and PFQ admission.
        archDBer->bopDirectQualityIssueTraceWrite(
            curTick(), event_sequence, feedback_id, candidate_demand_sequence,
            line, kind);
    }
}

void
L2CompositeWithWorkerPrefetcher::directQualityTraceDemand(
    uint64_t event_sequence, uint64_t demand_sequence, Addr line)
{
    if (archDBer) {
        archDBer->bopDirectQualityDemandTraceWrite(
            curTick(), event_sequence, demand_sequence, line);
    }
}

void
L2CompositeWithWorkerPrefetcher::directQualityTraceOutcome(
    uint64_t event_sequence, uint64_t feedback_id,
    uint64_t resolve_demand_sequence, Addr line,
    DirectQualityGate::TraceOutcome outcome)
{
    if (archDBer) {
        archDBer->bopDirectQualityOutcomeTraceWrite(
            curTick(), event_sequence, feedback_id, resolve_demand_sequence,
            line, directQualityOutcomeName(outcome));
    }
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
    }
}

PacketPtr
L2CompositeWithWorkerPrefetcher::getPacket()
{
    return Queued::getPacket();
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
    if (pkt->isDemand() && pkt->isRead() && !pkt->isWrite())
        // Match BOP raw candidates and L2 replay demand labels on pfi's
        // address domain, rather than the packet's translated cache address.
        largeBOP->notifyDirectQualityDemand(pfi.getAddr());
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

    if (usePFBuffer) {
        if (cdp->hasPFRequestsInBuffer() && !PFReqSendEvent.scheduled()) {
            schedule(PFReqSendEvent, nextCycle());
        }
    } else if (addressGenBuffer.size()) {
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

    if (usePFBuffer) {
        if (cdp->hasPFRequestsInBuffer() && !PFReqSendEvent.scheduled()) {
            schedule(PFReqSendEvent, nextCycle());
        }
    } else if (addressGenBuffer.size()) {
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
