/*
 * Copyright (c) 2014-2015 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/cache/prefetch/fdip.hh"

#include <cassert>

#include "arch/generic/tlb.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/FDIPPrefetch.hh"
#include "mem/cache/base.hh"
#include "mem/request.hh"
#include "params/FDIPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

FDIP::FDIP(const FDIPPrefetcherParams &p)
  : Base(p),
    numPIQEntry(p.numPIQEntry),
    piq_latency(p.piq_latency)
{
}


void
FDIP::notify(const PacketPtr &pkt)
{
  assert(pkt->isFromFetchPF());
  if (pkt->cmd == MemCmd::PFFetchReq)
  {
    RequestPtr mem_req = std::make_shared<Request>(*(pkt->req));
    PrefetchTranslation *trans = new PrefetchTranslation(this);
    DPRINTF(FDIPPrefetch, "receive PFFetchReq vaddr:%#x.\n", mem_req->getVaddr());
    DPRINTF(FDIPPrefetch, "send tlb req vaddr:%#x\n", mem_req->getVaddr());
    tlb->translateTiming(mem_req, system->threads[mem_req->contextId()], trans, BaseMMU::Execute);
  } else {
    DPRINTF(FDIPPrefetch, "receive PFFlushReq\n");
    flush();
  }
}


PacketPtr
FDIP::getPacket()
{
  if (piq.empty()) {
    DPRINTF(FDIPPrefetch, "No hardware prefetches available.\n");
    return nullptr;
  }

  RequestPtr req = piq.front().req;
  req->setFlags(Request::PREFETCH);
  req->setXsMetadata(Request::XsMetadata(PrefetchSourceType::FDIP));
  PacketPtr pkt = new Packet(req, MemCmd::HardPFReq);

  DPRINTF(FDIPPrefetch, "Get packet paddr:%#x readyTime:%#u\n", req->getPaddr(), piq.front().readyTime);
  piq.pop_front();

  prefetchStats.pfIssued++;

  return pkt;
}


Tick
FDIP::nextPrefetchReadyTime() const
{
  if (!piq.empty()) {
    return piq.front().readyTime > curTick() ? \
           piq.front().readyTime : curTick() + cyclesToTicks(Cycles(1));
  } else {
    return MaxTick;
  }
}


void
FDIP::finishPrefetchTranslation(const Fault &fault, const RequestPtr &mem_req)
{
  DPRINTF(FDIPPrefetch, "recv tlb resp vaddr:%#x\n", mem_req->getVaddr());
  if ((fault == NoFault) && !mem_req->isUncacheable() && mem_req->hasPaddr()) {
    insert(mem_req);
  }
}


void
FDIP::insert(RequestPtr req)
{
  PIQEntry entry = PIQEntry(req, curTick() + cyclesToTicks(piq_latency));
  DPRINTF(FDIPPrefetch, "Insert prefetch req paddr:%#x readyTime:%#u\n", req->getPaddr(), entry.readyTime);
  piq.push_back(entry);
}


void
FDIP::flush()
{
  piq.clear();
}


bool
FDIP::enable()
{
  return piq.size() < numPIQEntry;
}


} // namespace prefetch
} // namespace gem5
