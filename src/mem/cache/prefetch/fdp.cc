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

#include "mem/cache/prefetch/fdp.hh"

#include <algorithm>

#include "debug/HWPrefetch.hh"
#include "mem/request.hh"
#include "params/FetchDirectedPrefetcher.hh"

namespace gem5
{

namespace prefetch
{

FetchDirectedPrefetcher::FetchDirectedPrefetcher(
    const FetchDirectedPrefetcherParams &p)
    : Base(p),
      cpu(p.cpu),
      markReqAsPrefetch(p.mark_req_as_prefetch),
      squashPrefetches(p.squash_prefetches),
      latency(cyclesToTicks(p.latency)),
      pfqSize(p.pfq_size),
      tqSize(p.tq_size),
      cacheSnoop(p.cache_snoop),
      maxBlocksPerTarget(p.max_blocks_per_target),
      skipTargetStartBlock(p.skip_target_start_block),
      minTargetDistance(p.min_target_distance),
      stats(this, p.pfq_size, p.tq_size)
{}

FetchDirectedPrefetcher::~FetchDirectedPrefetcher()
{
    for (auto *listener : listeners) {
        delete listener;
    }

    for (auto &pr : pfq) {
        delete pr.pkt;
    }
    for (auto &pr : translationq) {
        delete pr.pkt;
    }
}

FetchDirectedPrefetcher::FdipListener::FdipListener(
    FetchDirectedPrefetcher &_parent, ProbeManager *pm,
    const std::string &name, bool _insert)
    : ProbeListenerArgBase<FdipFetchTargetPtr>(pm, name),
      parent(_parent),
      insert(_insert)
{}

void
FetchDirectedPrefetcher::FdipListener::notify(const FdipFetchTargetPtr &ft)
{
    if (insert) {
        parent.notifyFTQInsert(ft);
    } else {
        parent.notifyFTQRemove(ft);
    }
}

void
FetchDirectedPrefetcher::regProbeListeners()
{
    Base::regProbeListeners();

    if (cpu == nullptr) {
        warn("FetchDirectedPrefetcher: no CPU to listen from\n");
        return;
    }

    listeners.push_back(
        new FdipListener(*this, cpu->getProbeManager(), "FTQInsert", true));
    listeners.push_back(
        new FdipListener(*this, cpu->getProbeManager(), "FTQRemove", false));
}

void
FetchDirectedPrefetcher::notifyFTQInsert(const FdipFetchTargetPtr &ft)
{
    stats.fdipInsertions++;

    if (ft->distanceFromFetchHead < minTargetDistance) {
        stats.targetTooNear++;
        return;
    }

    Addr end_pc = ft->predEndPC;
    if (end_pc <= ft->startPC) {
        end_pc = ft->startPC + 1;
    }

    Addr blk_addr = blockAddress(ft->startPC);
    const Addr end_blk_addr = blockAddress(end_pc - 1);
    if (skipTargetStartBlock) {
        blk_addr += blkSize;
    }

    unsigned blocks = 0;
    while (blk_addr <= end_blk_addr &&
           (maxBlocksPerTarget == 0 || blocks < maxBlocksPerTarget)) {
        auto pfq_it = std::find_if(
            pfq.begin(), pfq.end(),
            [blk_addr](const PrefetchRequest &pr) {
                return pr.sameBlock(blk_addr);
            });
        if (pfq_it != pfq.end()) {
            DPRINTF(HWPrefetch, "%#x already in FDP prefetch queue\n",
                    blk_addr);
            stats.pfInPFQ++;
            blk_addr += blkSize;
            blocks++;
            continue;
        }

        auto tq_it = std::find_if(
            translationq.begin(), translationq.end(),
            [blk_addr](const PrefetchRequest &pr) {
                return pr.sameBlock(blk_addr);
            });
        if (tq_it != translationq.end()) {
            DPRINTF(HWPrefetch, "%#x already in FDP translation queue\n",
                    blk_addr);
            stats.pfInTQ++;
            blk_addr += blkSize;
            blocks++;
            continue;
        }

        stats.pfIdentified++;

        if (translationq.size() >= tqSize) {
            DPRINTF(HWPrefetch,
                    "FDP translation queue full, dropping %#x\n", blk_addr);
            stats.tqDrops++;
            blk_addr += blkSize;
            blocks++;
            continue;
        }

        translationq.emplace_back(*this, blk_addr, ft->tid, ft->id);
        DPRINTF(HWPrefetch, "FDP starts translation for %#x ftq=%lu\n",
                blk_addr, ft->id);
        translationq.back().startTranslation();
        stats.tqInserts++;
        stats.tqSizeDistAtNotify.sample(translationq.size());
        stats.pfqSizeDistAtNotify.sample(pfq.size());

        blk_addr += blkSize;
        blocks++;
    }
}

void
FetchDirectedPrefetcher::notifyFTQRemove(const FdipFetchTargetPtr &ft)
{
    stats.fdipRemovals++;

    if (!squashPrefetches) {
        return;
    }

    for (auto &pr : translationq) {
        if (pr.ftid == ft->id) {
            pr.markCanceled();
            stats.pfSquashed++;
        }
    }

    auto it = pfq.begin();
    while (it != pfq.end()) {
        if (it->ftid == ft->id) {
            delete it->pkt;
            it = pfq.erase(it);
            stats.pfSquashed++;
        } else {
            ++it;
        }
    }
}

void
FetchDirectedPrefetcher::translationComplete(PrefetchRequest *pfr, bool failed)
{
    auto it = translationq.begin();
    while (it != translationq.end()) {
        if (&(*it) == pfr) {
            break;
        }
        ++it;
    }
    assert(it != translationq.end());

    if (failed) {
        DPRINTF(HWPrefetch, "FDP translation of %#x failed\n", it->addr);
        stats.translationFail++;
    } else {
        stats.translationSuccess++;
        const Addr paddr = it->req->getPaddr();
        const bool secure = it->req->isSecure();

        if (it->isCanceled()) {
            DPRINTF(HWPrefetch,
                    "FDP drops %#x after FTQ removal during translation\n",
                    it->addr);
        } else if (it->req->isUncacheable()) {
            DPRINTF(HWPrefetch, "FDP drops uncacheable request %#x\n",
                    it->addr);
        } else if (!system->isMemAddr(paddr)) {
            DPRINTF(HWPrefetch, "FDP drops non-memory paddr %#x\n", paddr);
        } else if (cacheSnoop &&
                   (inCache(paddr, secure) || inMissQueue(paddr, secure))) {
            stats.pfInCache++;
            if (hasBeenPrefetched(paddr, secure)) {
                stats.pfInCachePrefetched++;
            }
            DPRINTF(HWPrefetch,
                    "FDP drops redundant cache/MSHR candidate %#x\n", paddr);
        } else if (pfq.size() < pfqSize) {
            it->createPkt();
            it->readyTime = curTick() + latency;
            stats.pfPacketsCreated++;
            stats.pfCandidatesAdded++;
            pfq.push_back(*it);
            stats.pfqInserts++;
            DPRINTF(HWPrefetch,
                    "FDP queued prefetch VA %#x PA %#x ftq=%lu pfq=%lu\n",
                    it->addr, paddr, it->ftid, pfq.size());
        } else {
            DPRINTF(HWPrefetch,
                    "FDP prefetch queue full, dropping %#x\n", it->addr);
            stats.pfqDrops++;
        }
    }

    translationq.erase(it);
    stats.tqPops++;
}

PacketPtr
FetchDirectedPrefetcher::getPacket()
{
    if (pfq.empty()) {
        return nullptr;
    }

    PacketPtr pkt = pfq.front().pkt;
    DPRINTF(HWPrefetch, "FDP issues prefetch PA %#x VA %#x ftq=%lu\n",
            pkt->getAddr(), pfq.front().addr, pfq.front().ftid);

    pfq.pop_front();
    stats.pfqPops++;
    prefetchStats.pfIssued++;
    prefetchStats.pfIssued_srcs[pkt->req->getXsMetadata().prefetchSource]++;
    issuedPrefetches++;

    return pkt;
}

FetchDirectedPrefetcher::PrefetchRequest::PrefetchRequest(
    FetchDirectedPrefetcher &_owner, Addr _addr, ThreadID tid,
    FdipTargetId _ftid)
    : owner(_owner),
      addr(_addr),
      ftid(_ftid),
      req(nullptr),
      pkt(nullptr),
      readyTime(MaxTick),
      canceled(false)
{
    auto *tc = owner.cpu->getContext(tid);
    req = std::make_shared<Request>(addr, owner.blkSize, Request::INST_FETCH,
                                    owner.requestorId, addr,
                                    tc->contextId());
    if (owner.markReqAsPrefetch) {
        req->setFlags(Request::PREFETCH);
    }
    req->setXsMetadata(Request::XsMetadata(PrefetchSourceType::PF_NONE, 0));
    req->setPFSource(PrefetchSourceType::PF_NONE);
    req->setPFDepth(0);
}

void
FetchDirectedPrefetcher::PrefetchRequest::createPkt()
{
    req->taskId(context_switch_task_id::Prefetcher);
    pkt = new Packet(req, MemCmd::HardPFReq);
    pkt->allocate();
}

void
FetchDirectedPrefetcher::PrefetchRequest::startTranslation()
{
    fatal_if(owner.tlb == nullptr,
             "FetchDirectedPrefetcher requires an instruction TLB\n");
    auto *tc = owner.system->threads[req->contextId()];
    owner.tlb->translateTiming(req, tc, this, BaseMMU::Execute);
}

void
FetchDirectedPrefetcher::PrefetchRequest::finish(
    const Fault &fault, const RequestPtr &req, ThreadContext *tc,
    BaseMMU::Mode mode)
{
    owner.translationComplete(this, fault != NoFault);
}

FetchDirectedPrefetcher::Stats::Stats(
    statistics::Group *parent, int pfq_size, int tq_size)
    : statistics::Group(parent),
      ADD_STAT(fdipInsertions, statistics::units::Count::get(),
               "Number of FTQ insert notifications observed by FDP"),
      ADD_STAT(fdipRemovals, statistics::units::Count::get(),
               "Number of FTQ remove notifications observed by FDP"),
      ADD_STAT(targetTooNear, statistics::units::Count::get(),
               "Number of FTQ targets skipped by FDP distance filter"),
      ADD_STAT(pfIdentified, statistics::units::Count::get(),
               "Number of FDP prefetch candidates identified"),
      ADD_STAT(pfSquashed, statistics::units::Count::get(),
               "Number of FDP prefetches squashed by FTQ removal"),
      ADD_STAT(pfInPFQ, statistics::units::Count::get(),
               "Number of FDP candidates already in the prefetch queue"),
      ADD_STAT(pfInTQ, statistics::units::Count::get(),
               "Number of FDP candidates already in the translation queue"),
      ADD_STAT(pfInCache, statistics::units::Count::get(),
               "Number of FDP candidates dropped by cache/MSHR snoop"),
      ADD_STAT(pfInCachePrefetched, statistics::units::Count::get(),
               "Number of FDP snoop drops on already-prefetched blocks"),
      ADD_STAT(pfPacketsCreated, statistics::units::Count::get(),
               "Number of FDP HardPF packets created"),
      ADD_STAT(pfCandidatesAdded, statistics::units::Count::get(),
               "Number of FDP candidates added to the prefetch queue"),
      ADD_STAT(translationFail, statistics::units::Count::get(),
               "Number of FDP translations that failed"),
      ADD_STAT(translationSuccess, statistics::units::Count::get(),
               "Number of FDP translations that succeeded"),
      ADD_STAT(pfqSizeDistAtNotify, statistics::units::Count::get(),
               "Distribution of FDP prefetch queue size at notification"),
      ADD_STAT(tqSizeDistAtNotify, statistics::units::Count::get(),
               "Distribution of FDP translation queue size at notification"),
      ADD_STAT(pfqInserts, statistics::units::Count::get(),
               "Number of insertions into the FDP prefetch queue"),
      ADD_STAT(pfqPops, statistics::units::Count::get(),
               "Number of pops from the FDP prefetch queue"),
      ADD_STAT(pfqDrops, statistics::units::Count::get(),
               "Number of FDP candidates dropped because PFQ was full"),
      ADD_STAT(tqInserts, statistics::units::Count::get(),
               "Number of insertions into the FDP translation queue"),
      ADD_STAT(tqPops, statistics::units::Count::get(),
               "Number of pops from the FDP translation queue"),
      ADD_STAT(tqDrops, statistics::units::Count::get(),
               "Number of FDP candidates dropped because TQ was full")
{
    pfqSizeDistAtNotify.init(0, pfq_size, 4);
    tqSizeDistAtNotify.init(0, tq_size, 4);
}

} // namespace prefetch
} // namespace gem5
