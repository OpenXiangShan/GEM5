/*
 * Copyright (c) 2022-2023 The University of Edinburgh
 * Copyright (c) 2025 Arm Limited
 * All rights reserved
 *
 * The license below extends only to copyright in this file.  You may use this
 * file subject to the license terms below provided that you ensure that this
 * notice is replicated unmodified and in its entirety in all distributions,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer; redistributions in binary
 * form must reproduce the above copyright notice, this list of conditions and
 * the following disclaimer in the documentation and/or other materials
 * provided with the distribution; neither the name of the copyright holders nor
 * the names of its contributors may be used to endorse or promote products
 * derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __MEM_CACHE_PREFETCH_FDP_HH__
#define __MEM_CACHE_PREFETCH_FDP_HH__

#include <list>
#include <vector>

#include "arch/generic/mmu.hh"
#include "cpu/base.hh"
#include "cpu/pred/btb/fdip_target.hh"
#include "mem/cache/prefetch/base.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

struct FetchDirectedPrefetcherParams;

namespace prefetch
{

class FetchDirectedPrefetcher : public Base
{
  public:
    FetchDirectedPrefetcher(const FetchDirectedPrefetcherParams &p);
    ~FetchDirectedPrefetcher();

    void regProbeListeners() override;
    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override {}
    PacketPtr getPacket() override;

    bool
    hasPendingPacket() override
    {
        return !pfq.empty();
    }

    Tick
    nextPrefetchReadyTime() const override
    {
        return pfq.empty() ? MaxTick : pfq.front().readyTime;
    }

    void
    rxHint(BaseMMU::Translation *dpp) override
    {}

    void
    pfHitNotify(float accuracy, PrefetchSourceType pf_source,
                const PacketPtr &pkt) override
    {}

  private:
    using FdipFetchTargetPtr =
        branch_prediction::btb_pred::FdipFetchTargetPtr;
    using FdipTargetId = branch_prediction::btb_pred::FetchTargetId;

    class FdipListener : public ProbeListenerArgBase<FdipFetchTargetPtr>
    {
      public:
        FdipListener(FetchDirectedPrefetcher &_parent, ProbeManager *pm,
                     const std::string &name, bool _insert);
        void notify(const FdipFetchTargetPtr &ft) override;

      private:
        FetchDirectedPrefetcher &parent;
        const bool insert;
    };

    std::vector<FdipListener *> listeners;

    BaseCPU *cpu;

    const bool markReqAsPrefetch;
    const bool squashPrefetches;
    const Tick latency;
    const unsigned pfqSize;
    const unsigned tqSize;
    const bool cacheSnoop;
    const unsigned maxBlocksPerTarget;
    const bool skipTargetStartBlock;
    const unsigned minTargetDistance;

    struct PrefetchRequest : public BaseMMU::Translation
    {
        PrefetchRequest(FetchDirectedPrefetcher &_owner, Addr _addr,
                        ThreadID tid, FdipTargetId _ftid);

        FetchDirectedPrefetcher &owner;
        const Addr addr;
        const FdipTargetId ftid;
        RequestPtr req;
        PacketPtr pkt;
        Tick readyTime;
        bool canceled;

        bool sameBlock(Addr block_addr) const { return addr == block_addr; }

        void createPkt();
        void startTranslation();
        void markDelayed() override {}
        void finish(const Fault &fault, const RequestPtr &req,
                    ThreadContext *tc, BaseMMU::Mode mode) override;

        void markCanceled() { canceled = true; }
        bool isCanceled() const { return canceled; }
    };

    std::list<PrefetchRequest> pfq;
    std::list<PrefetchRequest> translationq;

    void notifyFTQInsert(const FdipFetchTargetPtr &ft);
    void notifyFTQRemove(const FdipFetchTargetPtr &ft);
    void translationComplete(PrefetchRequest *pf_req, bool failed);

  protected:
    struct Stats : public statistics::Group
    {
        Stats(statistics::Group *parent, int pfq_size, int tq_size);

        statistics::Scalar fdipInsertions;
        statistics::Scalar fdipRemovals;
        statistics::Scalar targetTooNear;
        statistics::Scalar pfIdentified;
        statistics::Scalar pfSquashed;
        statistics::Scalar pfInPFQ;
        statistics::Scalar pfInTQ;
        statistics::Scalar pfInCache;
        statistics::Scalar pfInCachePrefetched;
        statistics::Scalar pfPacketsCreated;
        statistics::Scalar pfCandidatesAdded;
        statistics::Scalar translationFail;
        statistics::Scalar translationSuccess;
        statistics::Distribution pfqSizeDistAtNotify;
        statistics::Distribution tqSizeDistAtNotify;
        statistics::Scalar pfqInserts;
        statistics::Scalar pfqPops;
        statistics::Scalar pfqDrops;
        statistics::Scalar tqInserts;
        statistics::Scalar tqPops;
        statistics::Scalar tqDrops;
    } stats;
};

} // namespace prefetch
} // namespace gem5

#endif // __MEM_CACHE_PREFETCH_FDP_HH__
