/*
 * Copyright (c) 2014-2015 ARM Limited
 * All rights reserved
 */

#include "mem/cache/prefetch/fdip.hh"

#include <cassert>

#include "base/trace.hh"
#include "debug/FDIPPrefetch.hh"
#include "params/FDIPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FDIP::FDIP(const FDIPPrefetcherParams &p)
    : Base(p), numPrefetchMSHR(p.numPrefetchMSHR), prefetchLatency(p.prefetchLatency)
{
}

void
FDIP::PrefetchTranslation::finish(const Fault &fault, const RequestPtr &req,
                                  gem5::ThreadContext *tc,
                                  BaseMMU::Mode mode)
{
    fdip->finishPrefetchTranslation(fault, req);
    delete this;
}

void
FDIP::notify(const PacketPtr &pkt)
{
    assert(pkt->isFromFetchPF());
    if (pkt->cmd == MemCmd::PFFetchReq) {
        RequestPtr mem_req = std::make_shared<Request>(*(pkt->req));
        auto *trans = new PrefetchTranslation(this);
        DPRINTF(FDIPPrefetch, "receive PFFetchReq vaddr:%#x\n",
                mem_req->getVaddr());
        tlb->translateTiming(mem_req,
            system->threads[mem_req->contextId()], trans, BaseMMU::Execute);
    } else {
        DPRINTF(FDIPPrefetch, "receive PFFlushReq\n");
        flush();
    }
}

void
FDIP::finishPrefetchTranslation(const Fault &fault, const RequestPtr &mem_req)
{
    if (fault != NoFault) {
        DPRINTF(FDIPPrefetch, "FDIP translation fault on vaddr:%#x\n",
                mem_req->getVaddr());
        return;
    }

    insert(mem_req);
}

void
FDIP::insert(RequestPtr req)
{
    if (prefetchMSHR.size() >= numPrefetchMSHR) {
        DPRINTF(FDIPPrefetch, "prefetchMSHR full, drop paddr:%#x\n", req->getPaddr());
        return;
    }

    prefetchMSHR.emplace_back(req, curTick() + cyclesToTicks(prefetchLatency));
}

void
FDIP::flush()
{
    prefetchMSHR.clear();
}

PacketPtr
FDIP::getPacket()
{
    if (prefetchMSHR.empty()) {
        DPRINTF(FDIPPrefetch, "No hardware prefetches available.\n");
        return nullptr;
    }

    RequestPtr req = prefetchMSHR.front().req;
    req->setFlags(Request::PREFETCH);
    req->setXsMetadata(Request::XsMetadata(PrefetchSourceType::FDIP));
    PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);
    prefetchMSHR.pop_front();
    prefetchStats.pfIssued++;
    return pkt;
}

Tick
FDIP::nextPrefetchReadyTime() const
{
    if (!prefetchMSHR.empty()) {
        return prefetchMSHR.front().readyTime > curTick() ?
            prefetchMSHR.front().readyTime : curTick() + cyclesToTicks(Cycles(1));
    }

    return MaxTick;
}

bool
FDIP::enable()
{
    return prefetchMSHR.size() < numPrefetchMSHR;
}

} // namespace prefetch
} // namespace gem5