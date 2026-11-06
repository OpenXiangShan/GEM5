#include "mem/cache/prefetch/l2_composite_with_worker.hh"

#include <algorithm>
#include <string>
#include <utility>

#include "debug/CDPFilter.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/prefetch/composite_with_worker.hh"
#include "mem/cache/prefetch/lldp.hh"

namespace gem5
{
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

L2CompositeWithWorkerPrefetcher::SlicePfqStats::SlicePfqStats(
    statistics::Group *parent, unsigned num_slices)
    : statistics::Group(parent, "slicedPfq"),
      ADD_STAT(pfqEnqueued, statistics::units::Count::get(),
               "PFQ entries enqueued by slice"),
      ADD_STAT(pfqDequeued, statistics::units::Count::get(),
               "PFQ entries dequeued by slice"),
      ADD_STAT(pfqDroppedOldest, statistics::units::Count::get(),
               "Oldest PFQ entries dropped by slice"),
      ADD_STAT(pfqInputConflict, statistics::units::Count::get(),
               "Cycles with multiple input candidates by slice"),
      ADD_STAT(pfqOccupancy, statistics::units::Count::get(),
               "Current PFQ occupancy by slice"),
      ADD_STAT(pfqMaxOccupancy, statistics::units::Count::get(),
               "Maximum PFQ occupancy by slice"),
      ADD_STAT(pfqInputWonBySource, statistics::units::Count::get(),
               "PFQ input grants by source"),
      ADD_STAT(pfqEnqueueWidth, statistics::units::Count::get(),
               "PFQ entries enqueued in one active ingress cycle")
{
    using namespace statistics;

    static constexpr const char *SourceNames[] = {
        "l1", "largeBOP", "smallBOP", "despacitoStream",
        "lldp", "cdp", "cmc"
    };
    static_assert(std::size(SourceNames) == NumInputSources);

    pfqEnqueued.init(num_slices).flags(total | nozero);
    pfqDequeued.init(num_slices).flags(total | nozero);
    pfqDroppedOldest.init(num_slices).flags(total | nozero);
    pfqInputConflict.init(num_slices).flags(total | nozero);
    pfqOccupancy.init(num_slices).flags(total);
    pfqMaxOccupancy.init(num_slices).flags(total);
    pfqInputWonBySource.init(NumInputSources).flags(total | nozero);
    pfqEnqueueWidth.init(0, num_slices, 1);

    for (unsigned slice = 0; slice < num_slices; ++slice) {
        const auto slice_name = std::to_string(slice);
        pfqEnqueued.subname(slice, slice_name);
        pfqDequeued.subname(slice, slice_name);
        pfqDroppedOldest.subname(slice, slice_name);
        pfqInputConflict.subname(slice, slice_name);
        pfqOccupancy.subname(slice, slice_name);
        pfqMaxOccupancy.subname(slice, slice_name);
    }
    for (unsigned source = 0; source < NumInputSources; ++source) {
        pfqInputWonBySource.subname(source, SourceNames[source]);
    }
}

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
      enableDespacitoStream(p.enable_despacito_stream),
      lldp(dynamic_cast<LLDPrefetcher *>(p.lldp)),
      enableLLDP(p.enable_lldp),
      offloadLowAccuracy(p.offload_low_accuracy),
      entriesPerSlice(p.pfq_entries_per_slice)
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
    assert(lldp);
    cdp->parentRid = p.sys->getRequestorId(this);
}

L2CompositeWithWorkerPrefetcher::~L2CompositeWithWorkerPrefetcher()
{
    for (auto &source_queue : sourceReady) {
        for (auto &dpp : source_queue) {
            delete dpp.pkt;
        }
    }
}

void
L2CompositeWithWorkerPrefetcher::configureSlices(
    unsigned num_slices, SliceMapper mapper)
{
    fatal_if(sliced(), "%s: PFQ slices are already configured", name());
    fatal_if(num_slices == 0, "%s: PFQ requires at least one slice", name());
    fatal_if(entriesPerSlice == 0,
             "%s: PFQ entries per slice must be non-zero", name());
    fatal_if(!mapper, "%s: PFQ slice mapper is not configured", name());
    fatal_if(queueSize != num_slices * entriesPerSlice,
             "%s: queue_size=%u does not match %u slices x %u entries",
             name(), queueSize, num_slices, entriesPerSlice);

    sliceCount = num_slices;
    sliceMapper = std::move(mapper);
    slicePfqStats = std::make_unique<SlicePfqStats>(this, sliceCount);
    maxSliceOccupancy.assign(sliceCount, 0);
    lastDequeueCycleBySlice.assign(
        sliceCount, std::numeric_limits<uint64_t>::max());

    // The L1 downstream path is one independent RTL source and may move at
    // most one request into input arbitration per cycle.
    depth = 1;

}

void
L2CompositeWithWorkerPrefetcher::resetStats()
{
    CompositeWithWorkerPrefetcher::resetStats();
    std::fill(maxSliceOccupancy.begin(), maxSliceOccupancy.end(), 0);
    updateSliceOccupancyStats();
}

void
L2CompositeWithWorkerPrefetcher::preDumpStats()
{
    updateSliceOccupancyStats();
    CompositeWithWorkerPrefetcher::preDumpStats();
}

unsigned
L2CompositeWithWorkerPrefetcher::packetSlice(
    const DeferredPacket &dpp) const
{
    assert(sliced());
    assert(dpp.pkt != nullptr);
    const unsigned slice = sliceMapper(dpp.pkt->getAddr());
    panic_if(slice >= sliceCount,
             "%s: mapped prefetch %#x to invalid slice %u",
             name(), dpp.pkt->getAddr(), slice);
    return slice;
}

L2CompositeWithWorkerPrefetcher::iterator
L2CompositeWithWorkerPrefetcher::sliceHead(unsigned slice)
{
    assert(slice < sliceCount);
    return std::find_if(pfq.begin(), pfq.end(),
        [this, slice](const DeferredPacket &dpp) {
            return packetSlice(dpp) == slice;
        });
}

L2CompositeWithWorkerPrefetcher::const_iterator
L2CompositeWithWorkerPrefetcher::sliceHead(unsigned slice) const
{
    assert(slice < sliceCount);
    return std::find_if(pfq.begin(), pfq.end(),
        [this, slice](const DeferredPacket &dpp) {
            return packetSlice(dpp) == slice;
        });
}

unsigned
L2CompositeWithWorkerPrefetcher::sliceOccupancy(unsigned slice) const
{
    assert(slice < sliceCount);
    return std::count_if(pfq.begin(), pfq.end(),
        [this, slice](const DeferredPacket &dpp) {
            return packetSlice(dpp) == slice;
        });
}

bool
L2CompositeWithWorkerPrefetcher::queuedInPfq(
    const PrefetchInfo &pfi) const
{
    return std::any_of(pfq.begin(), pfq.end(),
        [&pfi](const DeferredPacket &queued) {
            return queued.pfInfo.sameAddr(pfi);
        });
}

void
L2CompositeWithWorkerPrefetcher::updateSliceOccupancyStats()
{
    if (!sliced()) {
        return;
    }

    unsigned total = 0;
    for (unsigned slice = 0; slice < sliceCount; ++slice) {
        const unsigned occupancy = sliceOccupancy(slice);
        total += occupancy;
        maxSliceOccupancy[slice] =
            std::max(maxSliceOccupancy[slice], occupancy);
        slicePfqStats->pfqOccupancy[slice] = occupancy;
        slicePfqStats->pfqMaxOccupancy[slice] =
            maxSliceOccupancy[slice];
        assert(occupancy <= entriesPerSlice);
    }
    assert(total == pfq.size());
    assert(total <= queueSize);
}

void
L2CompositeWithWorkerPrefetcher::dropStagedFront(InputSource source)
{
    auto &queue = sourceReady[sourceIndex(source)];
    assert(!queue.empty());
    delete queue.front().pkt;
    queue.pop_front();
}

void
L2CompositeWithWorkerPrefetcher::enqueueWinner(
    InputSource source, unsigned slice)
{
    auto &source_queue = sourceReady[sourceIndex(source)];
    assert(!source_queue.empty());
    assert(packetSlice(source_queue.front()) == slice);

    if (sliceOccupancy(slice) >= entriesPerSlice) {
        auto oldest = sliceHead(slice);
        assert(oldest != pfq.end());
        const auto removed_source =
            oldest->pfInfo.getXsMetadata().prefetchSource;
        statsQueued.pfRemovedFull++;
        statsQueued.pfRemovedFull_srcs[removed_source]++;
        slicePfqStats->pfqDroppedOldest[slice]++;
        delete oldest->pkt;
        pfq.erase(oldest);
    }

    pfq.splice(pfq.end(), source_queue, source_queue.begin());
    slicePfqStats->pfqEnqueued[slice]++;
    slicePfqStats->pfqInputWonBySource[sourceIndex(source)]++;
}

void
L2CompositeWithWorkerPrefetcher::tagNewRequests(
    std::vector<AddrPriority> &addresses, size_t previous_size,
    InputSource source) const
{
    if (!sliced()) {
        return;
    }

    assert(previous_size <= addresses.size());
    for (size_t i = previous_size; i < addresses.size(); ++i) {
        addresses[i].queueTag = sourceIndex(source);
    }
}

bool
L2CompositeWithWorkerPrefetcher::pullSourceRequest(
    Queued *source, InputSource input_source,
    std::vector<AddrPriority> &addresses)
{
    const unsigned source_idx = sourceIndex(input_source);
    const bool translation_pending = std::any_of(
        pfqMissingTranslation.begin(), pfqMissingTranslation.end(),
        [source_idx](const DeferredPacket &dpp) {
            return dpp.queueTag == source_idx;
        });
    if (!sourceReady[source_idx].empty() || translation_pending) {
        return false;
    }
    if (!source->hasPFRequestsInBuffer()) {
        return false;
    }

    const size_t previous_size = addresses.size();
    const bool sent = source->GetPFRequestsFromBuffer(addresses);
    panic_if(addresses.size() > previous_size + 1,
             "%s: one source produced more than one PFQ candidate",
             name());
    panic_if(sent != (addresses.size() == previous_size + 1),
             "%s: inconsistent source-buffer result", name());
    if (sent) {
        addresses.back().queueTag = sourceIndex(input_source);
    }
    return sent;
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
                delete dpp.pkt;
                return;
            }
        }
    }

    if (!sliced() || &queue != &pfq) {
        Queued::addToQueue(queue, dpp);
        return;
    }

    if (hasHintDownStream() && dpp.pfahead &&
        dpp.pfahead_host > cache->level()) {
        hintDownStream->rxHint(&dpp);
        prefetchStats.pfaheadOffloaded++;
        return;
    }

    panic_if(dpp.queueTag >= NumInputSources,
             "%s: prefetch %#x has no valid PFQ input source",
             name(), dpp.pkt ? dpp.pkt->getAddr() : 0);
    dpp.owner = this;
    sourceReady[dpp.queueTag].emplace_back(dpp);

    // Input arbitration should not wait for the packet's output-ready tick.
    if (packetReady) {
        packetReady(curTick());
    }
}

void
L2CompositeWithWorkerPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses,
                                                   bool late, PrefetchSourceType pf_source, bool miss_repeat)
{
    if (enableCMC) {
        const size_t previous_size = addresses.size();
        cmc->doPrefetch(pfi, addresses, late, pf_source, false);
        tagNewRequests(addresses, previous_size, InputSource::CMC);
    }
    if (enableCDP) {
        const size_t previous_size = addresses.size();
        cdp->calculatePrefetch(pfi, addresses);
        tagNewRequests(addresses, previous_size, InputSource::CDP);
    }
    if (enableBOP) {
        size_t previous_size = addresses.size();
        largeBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);
        tagNewRequests(addresses, previous_size, InputSource::LargeBOP);
        previous_size = addresses.size();
        smallBOP->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::HWP_BOP);
        tagNewRequests(addresses, previous_size, InputSource::SmallBOP);
    }
    if (enableDespacitoStream) {
        const size_t previous_size = addresses.size();
        despacitoStream->calculatePrefetch(pfi, addresses, late && pf_source == PrefetchSourceType::DespacitoStream);
        tagNewRequests(
            addresses, previous_size, InputSource::DespacitoStream);
    }
}

void
L2CompositeWithWorkerPrefetcher::rxHint(BaseMMU::Translation *dpp)
{
    auto ptr = reinterpret_cast<DeferredPacket *>(dpp);

    if (sliced() && ptr->owner == lldp) {
        DeferredPacket local = *ptr;
        local.owner = this;
        local.queueTag = sourceIndex(InputSource::LLDP);
        addToQueue(pfq, local);
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
    if (sliced()) {
        ptr->queueTag = sourceIndex(InputSource::L1);
    }
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
    const size_t previous_size = addressGenBuffer.size();
    if (enableCDP) {
        cdp->pfHitNotify(accuracy, pf_source, pkt, addressGenBuffer);
    }
    tagNewRequests(addressGenBuffer, previous_size, InputSource::CDP);

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
    lldp->setParentInfo(sys, pm, _cache, blk_size);
    CompositeWithWorkerPrefetcher::setParentInfo(sys, pm, _cache, blk_size);
}

void
L2CompositeWithWorkerPrefetcher::addTLB(BaseTLB *tlb, bool functional)
{
    Base::addTLB(tlb, functional);
    lldp->addTLB(tlb, functional);
}

void
L2CompositeWithWorkerPrefetcher::regProbeListeners()
{
    CompositeWithWorkerPrefetcher::regProbeListeners();
    if (enableLLDP)
        lldp->regProbeListeners();
}

void
L2CompositeWithWorkerPrefetcher::setPacketReadyCallback(
    std::function<void(Tick)> callback)
{
    if (enableLLDP) {
        if (sliced()) {
            lldp->setPacketReadyCallback([this](Tick) {
                if (packetReady) {
                    packetReady(curTick());
                }
            });
        } else {
            lldp->setPacketReadyCallback(callback);
        }
    }
    Base::setPacketReadyCallback(std::move(callback));
}

bool
L2CompositeWithWorkerPrefetcher::hasPendingPacket()
{
    if (sliced()) {
        return Queued::hasPendingPacket();
    }
    return Queued::hasPendingPacket() ||
        (enableLLDP && lldp->hasPendingPacket());
}

PacketPtr
L2CompositeWithWorkerPrefetcher::getPacket()
{
    if (sliced()) {
        for (unsigned slice = 0; slice < sliceCount; ++slice) {
            if (nextPrefetchReadyTime(slice) <= curTick()) {
                return getPacket(slice);
            }
        }
        return nullptr;
    }

    const bool lldpReady = enableLLDP &&
        lldp->nextPrefetchReadyTime() <= curTick();
    const bool queuedReady = Queued::nextPrefetchReadyTime() <= curTick();
    if (lldpReady && (!queuedReady || preferLLDP)) {
        preferLLDP = false;
        return lldp->getPacket();
    }
    if (queuedReady) {
        preferLLDP = true;
        return Queued::getPacket();
    }
    return nullptr;
}

Tick
L2CompositeWithWorkerPrefetcher::nextPrefetchReadyTime() const
{
    if (sliced()) {
        Tick ready = MaxTick;
        for (unsigned slice = 0; slice < sliceCount; ++slice) {
            ready = std::min(ready, nextPrefetchReadyTime(slice));
        }
        return ready;
    }

    const Tick parent = Queued::nextPrefetchReadyTime();
    return enableLLDP ? std::min(parent, lldp->nextPrefetchReadyTime()) : parent;
}

bool
L2CompositeWithWorkerPrefetcher::hasPendingPacket(unsigned slice) const
{
    assert(sliced());
    return sliceHead(slice) != pfq.end();
}

Tick
L2CompositeWithWorkerPrefetcher::nextPrefetchReadyTime(
    unsigned slice) const
{
    assert(sliced());
    const auto head = sliceHead(slice);
    return head == pfq.end() ? MaxTick : head->tick;
}

PacketPtr
L2CompositeWithWorkerPrefetcher::getPacket(unsigned slice)
{
    assert(sliced());
    assert(slice < sliceCount);

    const uint64_t cycle = curCycle();
    if (lastDequeueCycleBySlice[slice] == cycle) {
        return nullptr;
    }

    const auto head = sliceHead(slice);
    if (head == pfq.end() || head->tick > curTick()) {
        return nullptr;
    }

    lastDequeueCycleBySlice[slice] = cycle;
    PacketPtr pkt = dequeuePacket(head);
    slicePfqStats->pfqDequeued[slice]++;
    updateSliceOccupancyStats();
    return pkt;
}

bool
L2CompositeWithWorkerPrefetcher::hasPendingIngress() const
{
    if (!sliced()) {
        return false;
    }
    const bool staged = std::any_of(
        sourceReady.begin(), sourceReady.end(),
        [](const auto &queue) { return !queue.empty(); });
    return staged || (enableLLDP && lldp->hasPendingPacket());
}

void
L2CompositeWithWorkerPrefetcher::processIngress()
{
    assert(sliced());
    const uint64_t cycle = curCycle();
    panic_if(lastIngressCycle == cycle,
             "%s: PFQ ingress processed more than once in cycle %llu",
             name(), cycle);
    lastIngressCycle = cycle;

    // LLDP owns its translation queue. Transfer at most one physically ready
    // request per cycle into the composite's LLDP source staging queue.
    if (enableLLDP &&
        sourceReady[sourceIndex(InputSource::LLDP)].empty()) {
        lldp->transferPacketTo(*this);
    }

    std::vector<InputSource> winner_by_slice(
        sliceCount, InputSource::NumSources);
    std::vector<unsigned> candidate_count_by_slice(sliceCount, 0);
    for (unsigned source_idx = 0;
         source_idx < NumInputSources; ++source_idx) {
        auto &source_queue = sourceReady[source_idx];
        if (source_queue.empty()) {
            continue;
        }

        const auto source = static_cast<InputSource>(source_idx);
        if (queueFilter && queuedInPfq(source_queue.front().pfInfo)) {
            statsQueued.pfBufferHit++;
            dropStagedFront(source);
            continue;
        }

        const unsigned slice = packetSlice(source_queue.front());
        candidate_count_by_slice[slice]++;
        if (winner_by_slice[slice] == InputSource::NumSources) {
            // Sources are visited in fixed priority order, so the first
            // candidate seen for a slice is its winner.
            winner_by_slice[slice] = source;
        }
    }

    unsigned enqueue_width = 0;
    for (unsigned slice = 0; slice < sliceCount; ++slice) {
        if (candidate_count_by_slice[slice] > 1) {
            slicePfqStats->pfqInputConflict[slice]++;
        }
        if (winner_by_slice[slice] != InputSource::NumSources) {
            enqueueWinner(winner_by_slice[slice], slice);
            enqueue_width++;
        }
    }
    slicePfqStats->pfqEnqueueWidth.sample(enqueue_width);

    updateSliceOccupancyStats();
}

lldp::Hint
L2CompositeWithWorkerPrefetcher::loadTrain(const PacketPtr &pkt, bool miss)
{
    return enableLLDP ? lldp->loadTrain(pkt, miss) : lldp::Hint();
}

void
L2CompositeWithWorkerPrefetcher::hintData(const lldp::Hint &hint,
                                           const PacketPtr &demand,
                                           const uint8_t *data, unsigned size)
{
    if (enableLLDP)
        lldp->hintData(hint, demand, data, size);
}

void
L2CompositeWithWorkerPrefetcher::notifyFill(const PacketPtr &pkt)
{
    const size_t previous_size = addressGenBuffer.size();
    if (enableCDP) {
        cdp->notifyFill(pkt, addressGenBuffer);
    }
    tagNewRequests(addressGenBuffer, previous_size, InputSource::CDP);

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
L2CompositeWithWorkerPrefetcher::GetPFRequestsFromBuffer(
    std::vector<AddrPriority> &addresses)
{
    if (sliced()) {
        bool sent = false;
        if (enableBOP) {
            sent |= pullSourceRequest(
                largeBOP, InputSource::LargeBOP, addresses);
            sent |= pullSourceRequest(
                smallBOP, InputSource::SmallBOP, addresses);
        }
        if (enableDespacitoStream) {
            sent |= pullSourceRequest(
                despacitoStream, InputSource::DespacitoStream, addresses);
        }
        if (enableCDP) {
            sent |= pullSourceRequest(cdp, InputSource::CDP, addresses);
        }
        if (enableCMC) {
            sent |= pullSourceRequest(cmc, InputSource::CMC, addresses);
        }
        return sent;
    }

    // Preserve the legacy single-port behavior outside the sliced L2.
    bool L2PFsent = false;
    L2PFsent = ticksToCycles(latestTransferTick) == ticksToCycles(curTick());
    if (!L2PFsent && largeBOP->hasPFRequestsInBuffer()) {
        L2PFsent = largeBOP->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && smallBOP->hasPFRequestsInBuffer()) {
        L2PFsent = smallBOP->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && despacitoStream->hasPFRequestsInBuffer()) {
        L2PFsent = despacitoStream->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && cdp->hasPFRequestsInBuffer()) {
        L2PFsent = cdp->GetPFRequestsFromBuffer(addresses);
    }
    if (!L2PFsent && cmc->hasPFRequestsInBuffer()) {
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

bool
L2CompositeWithWorkerPrefetcher::hasPFRequestsInBuffer()
{
    if (!sliced()) {
        return largeBOP->hasPFRequestsInBuffer() ||
               smallBOP->hasPFRequestsInBuffer() ||
               cmc->hasPFRequestsInBuffer() ||
               cdp->hasPFRequestsInBuffer() ||
               despacitoStream->hasPFRequestsInBuffer();
    }

    return (enableBOP && (largeBOP->hasPFRequestsInBuffer() ||
                          smallBOP->hasPFRequestsInBuffer())) ||
           (enableCMC && cmc->hasPFRequestsInBuffer()) ||
           (enableCDP && cdp->hasPFRequestsInBuffer()) ||
           (enableDespacitoStream &&
            despacitoStream->hasPFRequestsInBuffer());
}
}  // namespace prefetch
}  // namespace gem5
