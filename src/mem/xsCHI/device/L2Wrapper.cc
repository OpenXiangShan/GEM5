#include "L2Wrapper.hh"

namespace gem5
{
namespace xsCHI
{
    bool
    L2Wrapper::CpuSidePort::recvTimingSnoopResp(PacketPtr pkt)
    {
        // Snoops shouldn't happen when bypassing caches
        assert(!cache->system->bypassCaches());

        assert(pkt->isResponse());

        // Express snoop responses from requestor to responder, e.g., from L1 to L2
        cache->recvTimingSnoopResp(pkt);
        return true;
    }


    bool
    L2Wrapper::CpuSidePort::tryTiming(PacketPtr pkt)
    {
        if (cache->system->bypassCaches() || pkt->isExpressSnoop()
            || pkt->isStorePFTrain()) {
            // always let express snoop packets through even if blocked
            return true;
        } else if (blocked || mustSendRetry) {
            // either already committed to send a retry, or blocked
            mustSendRetry = true;
            return false;
        }
        if (!cache->tryAccessTag(pkt)) {
            DPRINTF(TagReadFail, "tryAccessTag fails addr: %lx\n", pkt->getAddr());
            return false;
        }
        int sliceidx = cache->getSliceIdx(pkt->getAddr());
        if (sliceidx >= 0 && cache->cacheLevel != 1) {
            if (cache->checkSLiceBusy(pkt, sliceidx)) {
                //no more buffer
                if (sendRetryEvent.scheduled()) {
                    owner.reschedule(sendRetryEvent, cache->clockEdge());
                } else {
                    owner.schedule(sendRetryEvent, cache->clockEdge());
                }
                return false;
            }
        }
        mustSendRetry = false;
        return true;
    }

    bool
    L2Wrapper::CpuSidePort::recvTimingReq(PacketPtr pkt)
    {
        assert(pkt->isRequest());

        if (cache->system->bypassCaches()) {
            // Just forward the packet if caches are disabled.
            // @todo This should really enqueue the packet rather
            [[maybe_unused]] bool success = cache->memSidePort.sendTimingReq(pkt);
            assert(success);
            return true;
        } else if (tryTiming(pkt)) {
            cache->recvTimingReq(pkt);
            return true;
        }
        return false;
    }

    Tick
    L2Wrapper::CpuSidePort::recvAtomic(PacketPtr pkt)
    {
        if (cache->system->bypassCaches()) {
            // Forward the request if the system is in cache bypass mode.
            return cache->memSidePort.sendAtomic(pkt);
        } else {
            return cache->recvAtomic(pkt);
        }
    }

    void
    L2Wrapper::CpuSidePort::recvFunctional(PacketPtr pkt)
    {
        if (cache->system->bypassCaches()) {
            // The cache should be flushed if we are in cache bypass mode,
            // so we don't need to check if we need to update anything.
            cache->memSidePort.sendFunctional(pkt);
            return;
        }

        // functional request
        cache->functionalAccess(pkt, true);
    }

    AddrRangeList
    L2Wrapper::CpuSidePort::getAddrRanges() const
    {
        return cache->getAddrRanges();
    }


    L2Wrapper::
    CpuSidePort::CpuSidePort(const std::string &_name, L2Wrapper *_cache,
                            const std::string &_label)
        : CacheResponsePort(_name, _cache, _label), cache(_cache)
    {
    }
    ReqPtr
    L2Wrapper::CreateRequest(PacketPtr pkt)
    {
        CHI_OP_TYPE cmd;


    }
}
}
