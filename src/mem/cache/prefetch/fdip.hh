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

#ifndef __MEM_CACHE_PREFETCH_FDIP_HH__
#define __MEM_CACHE_PREFETCH_FDIP_HH__

#include <cstdint>
#include <list>
#include <utility>

#include "arch/generic/mmu.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/base.hh"
#include "mem/packet.hh"

namespace gem5
{

struct FDIPPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class FDIP : public Base
{
  private:
    uint64_t numPIQEntry;
    Cycles piq_latency;

  public:
    FDIP(const FDIPPrefetcherParams &p);

    void notify(const PacketPtr &pkt) override;

    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override {
      // nothing for ppHit or ppMiss
    }

    PacketPtr getPacket() override;

    Tick nextPrefetchReadyTime() const override;

    void rxHint(BaseMMU::Translation *dpp) override
    {
      panic("FDIP: rxHint not implemented");
    }
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt) override {
    }
    bool hasPendingPacket() override {
        panic("FDIP: hasPendingPacket not implemented");
    };

    bool enable() override;

  protected:
    struct PIQEntry
    {
      RequestPtr req;
      /** Time when this prefetch becomes ready */
      Tick readyTime;

      PIQEntry(RequestPtr _req, Tick _tick)
      {
        req = _req;
        readyTime = _tick;
      }
    };

    class PrefetchTranslation : public BaseMMU::Translation
    {
      protected:
        FDIP *fdip;

      public:
        PrefetchTranslation(FDIP *_fdip) : fdip(_fdip) {}

        void markDelayed() {}

        void
        finish(const Fault &fault, const RequestPtr &req,
            gem5::ThreadContext *tc, BaseMMU::Mode mode)
        {
            fdip->finishPrefetchTranslation(fault, req);
            delete this;
        }
    };

  private:
    std::list<PIQEntry> piq;

    void finishPrefetchTranslation(const Fault &fault,
                                   const RequestPtr &mem_req);

    void insert(RequestPtr req);

    void flush();

};

} // namespace prefetch
} // namespace gem5
#endif //__MEM_CACHE_PREFETCH_FDIP_HH__
