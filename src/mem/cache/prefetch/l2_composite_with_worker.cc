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
      bankedQueueEnabled(p.banked_queue), bankStats(this),
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
    if (!bankedQueueEnabled) {
        bankStats.enqueued.init(1);
        bankStats.overwritten.init(1);
        bankStats.dequeued.init(1);
        bankStats.blocked.init(1);
        bankStats.occupancy.init(0, queueSize, 1);
    }
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
    if (&queue == &pfq && hasHintDownStream() && dpp.pfahead &&
        dpp.pfahead_host > cache->level()) {
        Queued::addToQueue(queue, dpp);
        return;
    }
    if (bankedQueueEnabled && &queue == &pfq) {
        assert(bankQueues && dpp.pkt);
        unsigned source = 3;
        if (dpp.pfInfo.getXsMetadata().prefetchSource ==
            PrefetchSourceType::SStream) {
            source = 0;
        } else if (dpp.pfInfo.getXsMetadata().prefetchSource ==
                   PrefetchSourceType::HWP_BOP) {
            source = dpp.priority == SmallBopInput ? 2 : 1;
        }
        auto &input = inputQueue(source);
        if (input.size() == static_cast<unsigned>(max_pf_buffer_size)) {
            delete input.front().pkt;
            input.pop_front();
            bankStats.inputRemovedFull++;
        }
        input.push_back(dpp);
        wakeBankQueues();
        return;
    }
    Queued::addToQueue(queue, dpp);
}

L2CompositeWithWorkerPrefetcher::BankStats::BankStats(statistics::Group *parent)
    : statistics::Group(parent, "bankQueue"),
      ADD_STAT(enqueued, statistics::units::Count::get(),
               "Requests enqueued in each bank FIFO"),
      ADD_STAT(overwritten, statistics::units::Count::get(),
               "Old FIFO heads overwritten in each bank"),
      ADD_STAT(dequeued, statistics::units::Count::get(),
               "Requests entering each bank's output pipeline"),
      ADD_STAT(blocked, statistics::units::Count::get(),
               "Dispatch attempts waiting for a slice or its retry"),
      ADD_STAT(arbitrationBlocked, statistics::units::Count::get(),
               "Source heads retained due to a higher priority same-bank input"),
      ADD_STAT(inputRemovedFull, statistics::units::Count::get(),
               "Ready local candidates overwritten before bank arbitration"),
      ADD_STAT(occupancy, statistics::units::Count::get(),
               "Bank FIFO occupancy sampled after enqueue")
{
}

void
L2CompositeWithWorkerPrefetcher::configureBanks(
    unsigned banks, std::function<unsigned(Addr)> select,
    std::function<void()> wakeup)
{
    fatal_if(!banks || queueSize < banks || queueSize % banks,
             "L2 queue_size must be a positive multiple of the bank count");
    fatal_if(max_pf_buffer_size <= 0, "L2 candidate buffer must be nonzero");
    bankQueues = std::make_unique<BankedQueue<DeferredPacket>>(
        banks, queueSize / banks);
    selectBank = std::move(select);
    wakeBankQueues = std::move(wakeup);
    bankStats.enqueued.init(banks);
    bankStats.overwritten.init(banks);
    bankStats.dequeued.init(banks);
    bankStats.blocked.init(banks);
    bankStats.occupancy.init(0, queueSize / banks, 1);
}

std::list<Queued::DeferredPacket> &
L2CompositeWithWorkerPrefetcher::inputQueue(unsigned source)
{
    return source == 0 ? localBuffer : readyInputs.at(source - 1);
}

bool
L2CompositeWithWorkerPrefetcher::hasBankInputs() const
{
    if (!localBuffer.empty())
        return true;
    for (const auto &input : readyInputs) {
        if (!input.empty())
            return true;
    }
    return false;
}

void
L2CompositeWithWorkerPrefetcher::transfer()
{
    if (bankedQueueEnabled) {
        wakeBankQueues();
    } else {
        WorkerPrefetcher::transfer();
    }
}

bool
L2CompositeWithWorkerPrefetcher::hasPendingPacket()
{
    if (!bankedQueueEnabled)
        return Queued::hasPendingPacket();
    if (hasBankInputs())
        return true;
    for (unsigned bank = 0; bank < bankQueues->banks(); ++bank) {
        if (!bankQueues->empty(bank))
            return true;
    }
    return false;
}

PacketPtr
L2CompositeWithWorkerPrefetcher::getPacket()
{
    panic_if(bankedQueueEnabled, "Banked L2 requires a bank-specific dequeue");
    return Queued::getPacket();
}

bool
L2CompositeWithWorkerPrefetcher::hasBankPacket(unsigned bank) const
{
    return !bankQueues->empty(bank);
}

PacketPtr
L2CompositeWithWorkerPrefetcher::getBankPacket(unsigned bank)
{
    if (bankQueues->empty(bank))
        return nullptr;
    auto dpp = bankQueues->pop(bank);
    bankStats.dequeued[bank]++;
    if (dpp.pfahead)
        prefetchStats.pfaheadProcess++;
    if (dpp.tlbMissRecorded)
        notifyPrefetchTlbMiss(dpp.pfInfo.getXsMetadata().prefetchSource);
    if (issueStatsAreAtForwarder())
        recordPrefetchDequeued(dpp.pkt);
    else
        recordIssuedPrefetch(dpp.pkt);
    return dpp.pkt;
}

void
L2CompositeWithWorkerPrefetcher::recordBankBlocked(unsigned bank)
{
    bankStats.blocked[bank]++;
}

void
L2CompositeWithWorkerPrefetcher::enqueueBankInputs()
{
    std::vector<int> offered(InputSources, -1);
    for (unsigned source = 0; source < InputSources; ++source) {
        const auto &input = inputQueue(source);
        if (!input.empty() && input.front().tick <= curTick())
            offered[source] = selectBank(input.front().pkt->getAddr());
    }
    const auto winners = BankedQueue<DeferredPacket>::select(
        bankQueues->banks(), offered);
    for (unsigned source = 0; source < InputSources; ++source) {
        const int bank = offered[source];
        if (bank < 0)
            continue;
        if (winners[bank] != static_cast<int>(source)) {
            bankStats.arbitrationBlocked++;
            continue;
        }
        auto &input = inputQueue(source);
        auto victim = bankQueues->push(bank, input.front());
        input.pop_front();
        bankStats.enqueued[bank]++;
        if (victim) {
            statsQueued.pfRemovedFull++;
            statsQueued.pfRemovedFull_srcs[
                victim->pfInfo.getXsMetadata().prefetchSource]++;
            bankStats.overwritten[bank]++;
            delete victim->pkt;
        }
        bankStats.occupancy.sample(bankQueues->size(bank));
    }
}

L2CompositeWithWorkerPrefetcher::~L2CompositeWithWorkerPrefetcher()
{
    if (!bankQueues)
        return;
    for (unsigned bank = 0; bank < bankQueues->banks(); ++bank) {
        while (!bankQueues->empty(bank))
            delete bankQueues->pop(bank).pkt;
    }
    for (unsigned source = 0; source < InputSources; ++source) {
        for (auto &dpp : inputQueue(source))
            delete dpp.pkt;
    }
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
    if (bankedQueueEnabled) {
        // Offer one head per local generator. Same-bank conflicts are handled
        // after translation, together with the L1 receiver, not at generation.
        auto collect = [&](Queued *generator, unsigned source, int identity) {
            if (inputQueue(source).size() >=
                static_cast<unsigned>(max_pf_buffer_size)) {
                return;
            }
            std::vector<AddrPriority> candidates;
            if (generator->GetPFRequestsFromBuffer(candidates)) {
                for (auto &candidate : candidates) {
                    if (source == 1 || source == 2)
                        candidate.priority = identity;
                    addresses.push_back(candidate);
                }
            }
        };
        if (enableBOP) {
            collect(largeBOP, 1, LargeBopInput);
            collect(smallBOP, 2, SmallBopInput);
        }
        if (enableDespacitoStream)
            collect(despacitoStream, 3, 0);
        if (enableCDP)
            collect(cdp, 3, 0);
        if (enableCMC)
            collect(cmc, 3, 0);
        return !addresses.empty();
    }
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
