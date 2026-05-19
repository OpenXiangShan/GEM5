/*
 * Copyright (c) 2014-2015 ARM Limited
 * All rights reserved
 */

#ifndef __MEM_CACHE_PREFETCH_FDIP_HH__
#define __MEM_CACHE_PREFETCH_FDIP_HH__

#include <cstdint>
#include <list>

#include "arch/generic/mmu.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/base.hh"

namespace gem5
{

struct FDIPPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class FDIP : public Base
{
  private:
    uint64_t numPrefetchMSHR;
    Cycles prefetchLatency;

  public:
    FDIP(const FDIPPrefetcherParams &p);

    void notify(const PacketPtr &pkt) override;
    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override {}
    PacketPtr getPacket() override;
    Tick nextPrefetchReadyTime() const override;

    void rxHint(BaseMMU::Translation *dpp) override
    {
        panic("FDIP: rxHint not implemented");
    }

    void pfHitNotify(float accuracy, PrefetchSourceType pf_source,
                     const PacketPtr &pkt) override {}

    bool hasPendingPacket() override { return !prefetchMSHR.empty(); }
    bool enable() override;

  protected:
    struct PrefetchEntry
    {
        RequestPtr req;
        Tick readyTime;

        PrefetchEntry(RequestPtr _req, Tick _tick)
          : req(_req), readyTime(_tick)
        {}
    };

    class PrefetchTranslation : public BaseMMU::Translation
    {
      protected:
        FDIP *fdip;

      public:
        PrefetchTranslation(FDIP *_fdip) : fdip(_fdip) {}
        void markDelayed() override {}
        void finish(const Fault &fault, const RequestPtr &req,
                    gem5::ThreadContext *tc, BaseMMU::Mode mode) override;
    };

  private:
    std::list<PrefetchEntry> prefetchMSHR;

    void finishPrefetchTranslation(const Fault &fault,
                                   const RequestPtr &mem_req);
    void insert(RequestPtr req);
    void flush();
};

} // namespace prefetch
} // namespace gem5

#endif