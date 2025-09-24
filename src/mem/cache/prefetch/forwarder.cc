#include "mem/cache/prefetch/forwarder.hh"

#include "mem/packet.hh"

namespace gem5
{

namespace prefetch
{

PrefetcherForwarder::PrefetcherForwarder(const PrefetcherForwarderParams &p)
    : Base(p), real_pf(nullptr)
{
}

bool
PrefetcherForwarder::observeAccess(const PacketPtr &pkt, bool miss) const
{
    if (real_pf) {
        return real_pf->observeAccess(pkt, miss);
    }
    return false;
}

void
PrefetcherForwarder::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    if (real_pf) {
        real_pf->notify(pkt, pfi);
    }
}

void
PrefetcherForwarder::probeNotify(const PacketPtr &pkt, bool miss)
{
    if (real_pf) {
        real_pf->probeNotify(pkt, miss);
    }
}

void
PrefetcherForwarder::notifyFill(const PacketPtr &pkt)
{
    if (real_pf) {
        real_pf->notifyFill(pkt);
    }
}

void
PrefetcherForwarder::coreDirectAddrNotify(const PacketPtr &pkt)
{
    if (real_pf) {
        real_pf->coreDirectAddrNotify(pkt);
    }
}

void
PrefetcherForwarder::rxHint(BaseMMU::Translation *dpp)
{
    if (real_pf) {
        real_pf->rxHint(dpp);
    }
}

void
PrefetcherForwarder::notifyIns(int ins_num)
{
    if (real_pf) {
        real_pf->notifyIns(ins_num);
    }
}

void
PrefetcherForwarder::pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt)
{
    if (real_pf) {
        real_pf->pfHitNotify(accuracy, pf_source, pkt);
    }
}

void
PrefetcherForwarder::sendCustomInfoToDownStream()
{
    if (real_pf) {
        real_pf->sendCustomInfoToDownStream();
    }
}

void
PrefetcherForwarder::recvCustomInfoFrmUpStream(CustomPfInfo &info)
{
    if (real_pf) {
        real_pf->recvCustomInfoFrmUpStream(info);
    }
}

void
PrefetcherForwarder::offloadToDownStream()
{
    if (real_pf) {
        real_pf->offloadToDownStream();
    }
}

bool
PrefetcherForwarder::hasHintsWaiting()
{
    if (real_pf) {
        return real_pf->hasHintsWaiting();
    }
    return false;
}

bool
PrefetcherForwarder::hasHintDownStream() const
{
    if (real_pf) {
        return real_pf->hasHintDownStream();
    }
    return false;
}

void
PrefetcherForwarder::nofityHitToDownStream(const PacketPtr &pkt)
{
    if (real_pf) {
        real_pf->nofityHitToDownStream(pkt);
    }
}

void
PrefetcherForwarder::recvPrefetchFromCache(const PacketPtr &pkt)
{
    prefetch_queue.push(pkt);
}

PacketPtr
PrefetcherForwarder::getPacket()
{
    if (prefetch_queue.empty()) {
        return nullptr;
    }
    PacketPtr pkt = prefetch_queue.front();
    prefetch_queue.pop();
    return pkt;
}

bool
PrefetcherForwarder::hasPendingPacket()
{
    return !prefetch_queue.empty();
}

Tick
PrefetcherForwarder::nextPrefetchReadyTime() const
{
    if (prefetch_queue.empty()) {
        return MaxTick;
    }
    // always ready
    return curTick();
}

void
PrefetcherForwarder::incrDemandMhsrMisses()
{
    if (real_pf) {
        real_pf->incrDemandMhsrMisses();
    }
}

void
PrefetcherForwarder::prefetchUnused(PrefetchSourceType pf_type)
{
    if (real_pf) {
        real_pf->prefetchUnused(pf_type);
    }
}

void
PrefetcherForwarder::pfHitInMSHR(PrefetchSourceType pf_type)
{
    if (real_pf) {
        real_pf->pfHitInMSHR(pf_type);
    }
}

void
PrefetcherForwarder::pfHitInCache(PrefetchSourceType pf_type)
{
    if (real_pf) {
        real_pf->pfHitInCache(pf_type);
    }
}

void
PrefetcherForwarder::pfHitInWB(PrefetchSourceType pf_type)
{
    if (real_pf) {
        real_pf->pfHitInWB(pf_type);
    }
}

} // namespace prefetch
} // namespace gem5
