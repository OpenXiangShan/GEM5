/*
 * Copyright (c) 2013-2014 ARM Limited
 * All rights reserved.
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
 * Copyright (c) 2005 The Regents of The University of Michigan
 * All rights reserved.
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

/**
 * @file
 * Hardware Prefetcher Definition.
 */

#include "mem/cache/prefetch/base.hh"

#include <cassert>
#include <cstring>

#include "base/intmath.hh"
#include "debug/HWPrefetch.hh"
#include "mem/cache/base.hh"
#include "params/BasePrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

Base::PrefetchInfo::PrefetchInfo(PacketPtr pkt, Addr addr, bool miss)
  : address(addr), pc(pkt->req->hasPC() ? pkt->req->getPC() : 0),
    requestorId(pkt->req->requestorId()),
    _contextId(pkt->req->hasContextId() ?
        pkt->req->contextId() : InvalidContextID),
    validContextId(pkt->req->hasContextId()),
    validPC(pkt->req->hasPC()),
    secure(pkt->isSecure()), size(pkt->req->getSize()), write(pkt->isWrite()),
    paddress(pkt->req->getPaddr()), cacheMiss(miss)
{
    unsigned int req_size = pkt->req->getSize();
    if (!write && miss) {
        data = nullptr;
        data_ptr = nullptr;
    } else if (pkt->isStorePFTrain()) {
        data = nullptr;
        data_ptr = nullptr;
    } else {
        data = new uint8_t[req_size];
        Addr offset = pkt->req->getPaddr() - pkt->getAddr();
        std::memcpy(data, &(pkt->getConstPtr<uint8_t>()[offset]), req_size);
        data_ptr=(uint64_t*)pkt->getPtr<uint64_t>();
    }
}

Base::PrefetchInfo::PrefetchInfo(
    PacketPtr pkt, Addr addr, bool miss,
    Request::XsMetadata xsMeta
) : address(addr), pc(pkt->req->hasPC() ? pkt->req->getPC() : 0),
    requestorId(pkt->req->requestorId()),
    _contextId(pkt->req->hasContextId() ?
        pkt->req->contextId() : InvalidContextID),
    validContextId(pkt->req->hasContextId()),
    validPC(pkt->req->hasPC()),
    secure(pkt->isSecure()), size(pkt->req->getSize()), write(pkt->isWrite()),
    paddress(pkt->req->getPaddr()), cacheMiss(miss), xsMetadata(xsMeta)
{
    unsigned int req_size = pkt->req->getSize();
    if (!write && miss) {
        data = nullptr;
        data_ptr = nullptr;
    } else if (pkt->isStorePFTrain()) {
        data = nullptr;
        data_ptr = nullptr;
    } else {
        data = new uint8_t[req_size];
        Addr offset = pkt->req->getPaddr() - pkt->getAddr();
        std::memcpy(data, &(pkt->getConstPtr<uint8_t>()[offset]), req_size);
        data_ptr=(uint64_t*)pkt->getPtr<uint64_t>();
    }
}

Base::PrefetchInfo::PrefetchInfo(PrefetchInfo const &pfi, Addr addr)
  : address(addr), pc(pfi.pc), requestorId(pfi.requestorId),
    _contextId(pfi._contextId), validContextId(pfi.validContextId),
    validPC(pfi.validPC), secure(pfi.secure), size(pfi.size),
    write(pfi.write), paddress(pfi.paddress), cacheMiss(pfi.cacheMiss),
    data(nullptr),data_ptr(nullptr)
{
}
Base::PrefetchInfo::PrefetchInfo(PrefetchInfo_old const &pfi)
  : address(pfi.address), pc(pfi.pc), requestorId(pfi.requestorId),
    _contextId(pfi._contextId), validContextId(pfi.validContextId),
    validPC(pfi.validPC), secure(pfi.secure), size(pfi.size),
    write(pfi.write), paddress(pfi.paddress), cacheMiss(pfi.cacheMiss),
    data(nullptr),data_ptr(nullptr)
{
}
Base::PrefetchInfo_old::PrefetchInfo_old(PacketPtr pkt, Addr addr, bool miss)
  : address(addr), pc(pkt->req->hasPC() ? pkt->req->getPC() : 0),
    requestorId(pkt->req->requestorId()),
    _contextId(pkt->req->hasContextId() ?
        pkt->req->contextId() : InvalidContextID),
    validContextId(pkt->req->hasContextId()),
    validPC(pkt->req->hasPC()),
    secure(pkt->isSecure()), size(pkt->req->getSize()), write(pkt->isWrite()),
    paddress(pkt->req->getPaddr()), cacheMiss(miss)
{
    unsigned int req_size = pkt->req->getSize();
    if (!write && miss) {
        data = nullptr;
        data_ptr = nullptr;
    } else if (pkt->isStorePFTrain()) {
        data = nullptr;
        data_ptr = nullptr;
    } else {
        data = new uint8_t[req_size];
        Addr offset = pkt->req->getPaddr() - pkt->getAddr();
        std::memcpy(data, &(pkt->getConstPtr<uint8_t>()[offset]), req_size);
        data_ptr=(uint64_t*)pkt->getPtr<uint64_t>();
    }
}

Base::PrefetchInfo_old::PrefetchInfo_old(
    PacketPtr pkt, Addr addr, bool miss,
    Request::XsMetadata xsMeta
) : address(addr), pc(pkt->req->hasPC() ? pkt->req->getPC() : 0),
    requestorId(pkt->req->requestorId()),
    _contextId(pkt->req->hasContextId() ?
        pkt->req->contextId() : InvalidContextID),
    validContextId(pkt->req->hasContextId()),
    validPC(pkt->req->hasPC()),
    secure(pkt->isSecure()), size(pkt->req->getSize()), write(pkt->isWrite()),
    paddress(pkt->req->getPaddr()), cacheMiss(miss), xsMetadata(xsMeta)
{
    unsigned int req_size = pkt->req->getSize();
    if (!write && miss) {
        data = nullptr;
        data_ptr = nullptr;
    } else if (pkt->isStorePFTrain()) {
        data = nullptr;
        data_ptr = nullptr;
    } else {
        data = new uint8_t[req_size];
        Addr offset = pkt->req->getPaddr() - pkt->getAddr();
        std::memcpy(data, &(pkt->getConstPtr<uint8_t>()[offset]), req_size);
        data_ptr=(uint64_t*)pkt->getPtr<uint64_t>();
    }
}
Base::PrefetchInfo_old::PrefetchInfo_old(PrefetchInfo_old const &other)
  : address(other.address), pc(other.pc), requestorId(other.requestorId),
    _contextId(other._contextId), validContextId(other.validContextId),
    validPC(other.validPC), secure(other.secure), size(other.size),
    write(other.write), paddress(other.paddress), cacheMiss(other.cacheMiss),
    data(nullptr),data_ptr(nullptr)
{

}
Base::PrefetchInfo_old::PrefetchInfo_old(PrefetchInfo_old const &pfi, Addr addr)
  : address(addr), pc(pfi.pc), requestorId(pfi.requestorId),
    _contextId(pfi._contextId), validContextId(pfi.validContextId),
    validPC(pfi.validPC), secure(pfi.secure), size(pfi.size),
    write(pfi.write), paddress(pfi.paddress), cacheMiss(pfi.cacheMiss),
    data(nullptr),data_ptr(nullptr)
{
}
Base::PrefetchInfo_old::PrefetchInfo_old(PrefetchInfo const &pfi)
  : address(pfi.address), pc(pfi.pc), requestorId(pfi.requestorId),
    _contextId(pfi._contextId), validContextId(pfi.validContextId),
    validPC(pfi.validPC), secure(pfi.secure), size(pfi.size),
    write(pfi.write), paddress(pfi.paddress), cacheMiss(pfi.cacheMiss),
    data(nullptr),data_ptr(nullptr)
{
}
void
Base::PrefetchListener::notify(const PacketPtr &pkt)
{
    if (coreDirectNotify) {
        parent.coreDirectAddrNotify(pkt);
    } else if (isFill) {
        parent.notifyFill(pkt);
    } else {
        parent.probeNotify(pkt, miss);
    }
}

Base::Base(const BasePrefetcherParams &p)
    : ClockedObject(p),
      listeners(),
      trainingBufferSize(p.training_buffer_size),
      cycleEvent([this]{ processCycle(); }, name()),  // TrainFilter cycle event
      isSubPrefetcher(p.is_sub_prefetcher),
      archDBer(p.arch_db), blkSize(p.block_size),
      lBlkSize(floorLog2(blkSize)), onMiss(p.on_miss), onRead(p.on_read),
      onWrite(p.on_write), onData(p.on_data), onInst(p.on_inst),
      requestorId(p.sys->getRequestorId(this)),
      pageBytes(p.page_bytes),
      prefetchTrain(p.prefetch_train),
      prefetchOnAccess(p.prefetch_on_access),
      prefetchOnPfHit(p.prefetch_on_pf_hit),
      useVirtualAddresses(p.use_virtual_addresses),
      prefetchStats(this), issuedPrefetches(0),
      usefulPrefetches(0), streamlatenum(0),tlb(nullptr)
{
}

void
Base::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor* _cache, unsigned blk_size)
{
    assert(!cache && !system && !probeManager);
    system = sys;
    probeManager = pm;
    cache = _cache;

    // If the cache has a different block size from the system's, save it
    blkSize = blk_size;
    lBlkSize = floorLog2(blkSize);
}

Base::StatGroup::StatGroup(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(demandMshrMisses, statistics::units::Count::get(),
        "demands not covered by prefetchs"),
    ADD_STAT(pfDequeued, statistics::units::Count::get(),
        "number of prefetches dequeued from the local prefetch queue"),
    ADD_STAT(pfDequeued_srcs, statistics::units::Count::get(),
        "number of prefetches dequeued from the local prefetch queue"),
    ADD_STAT(pfIssued, statistics::units::Count::get(),
        "number of prefetches reaching the cache issue boundary"),
    ADD_STAT(pfIssued_srcs, statistics::units::Count::get(),
        "number of prefetches reaching the cache issue boundary"),
    ADD_STAT(pfOffloaded, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfaheadOffloaded, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfaheadProcess, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfUnused, statistics::units::Count::get(),
             "number of HardPF blocks evicted w/o reference"),
    ADD_STAT(pfUnused_srcs, statistics::units::Count::get(),
             "number of HardPF blocks evicted w/o reference"),
    ADD_STAT(pfBad, statistics::units::Count::get(),
             "number of cache miss requests hitting the PFBad table"),
    ADD_STAT(pfBad_srcs, statistics::units::Count::get(),
             "number of cache miss requests hitting the PFBad table "
             "by evictor source"),
    ADD_STAT(pfUseful, statistics::units::Count::get(),
        "number of useful prefetch"),
    ADD_STAT(pfUseful_srcs, statistics::units::Count::get(),
        "number of useful prefetch"),
    ADD_STAT(pfHitInCache_srcs, statistics::units::Count::get(),
        "number of prefetches hitting in cache"),
    ADD_STAT(pfHitInMSHR_srcs, statistics::units::Count::get(),
        "number of prefetches hitting in a MSHR"),
    ADD_STAT(pfHitInWB_srcs, statistics::units::Count::get(),
        "number of prefetches hit in the Write Buffer"),
    ADD_STAT(late_srcs, statistics::units::Count::get(),
        "number of prefetches late"),
    ADD_STAT(pfUsefulButMiss, statistics::units::Count::get(),
        "number of hit on prefetch but cache block is not in an usable "
        "state"),
    ADD_STAT(accuracy, statistics::units::Count::get(),
        "accuracy of the prefetcher"),
    ADD_STAT(coverage, statistics::units::Count::get(),
    "coverage brought by this prefetcher"),
    ADD_STAT(pfHitInCache, statistics::units::Count::get(),
        "number of prefetches hitting in cache"),
    ADD_STAT(pfHitInMSHR, statistics::units::Count::get(),
        "number of prefetches hitting in a MSHR"),
    ADD_STAT(pfHitInWB, statistics::units::Count::get(),
        "number of prefetches hit in the Write Buffer"),
    ADD_STAT(pfGenerated, statistics::units::Count::get(),
        "number of prefetch requests generated by prefetcher"),
    ADD_STAT(pfFiltered, statistics::units::Count::get(),
        "number of prefetch requests filtered before issuing"),
    ADD_STAT(trainFilterContextAliases, statistics::units::Count::get(),
        "same virtual blocks retained for different ContextIDs"),
    ADD_STAT(pfLate, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)")
{
    using namespace statistics;

    pfDequeued_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfIssued_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);

    pfUnused.flags(nozero);
    pfUnused_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfBad.flags(nozero);
    pfBad_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfUseful_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);

    pfHitInCache_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfHitInMSHR_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    pfHitInWB_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);
    late_srcs
        .init(NUM_PF_SOURCES)
        .flags(total | nozero);

    for (unsigned source = 0; source < NUM_PF_SOURCES; ++source) {
        const auto source_name = prefetchSourceTypeName(source);
        pfDequeued_srcs.subname(source, source_name);
        pfIssued_srcs.subname(source, source_name);
        pfUnused_srcs.subname(source, source_name);
        pfBad_srcs.subname(source, source_name);
        pfUseful_srcs.subname(source, source_name);
        pfHitInCache_srcs.subname(source, source_name);
        pfHitInMSHR_srcs.subname(source, source_name);
        pfHitInWB_srcs.subname(source, source_name);
        late_srcs.subname(source, source_name);
    }


    accuracy.flags(total);
    accuracy = pfUseful / pfIssued;

    coverage.flags(total);
    coverage = pfUseful / (pfUseful + demandMshrMisses);

    pfLate = pfHitInCache + pfHitInMSHR + pfHitInWB;
}

bool
Base::observeAccess(const PacketPtr &pkt, bool miss) const
{
    bool fetch = pkt->req->isInstFetch();
    bool read = pkt->isRead();
    bool inv = pkt->isInvalidate();

    // Filter L1 prefetcher requests from training L2 prefetcher
    if (pkt->req->isPrefetch() && !prefetchTrain) {
        return false;
    }
    if (!miss) {
        if (prefetchOnPfHit)
            return hasEverBeenPrefetched(pkt->getAddr(), pkt->isSecure());
        if (!prefetchOnAccess)
            return false;
    }
    if (pkt->req->isUncacheable()) return false;
    if (fetch && !onInst) return false;
    if (!fetch && !onData) return false;
    if (!fetch && read && !onRead) return false;
    if (!fetch && !read && !onWrite) return false;
    if (!fetch && !read && inv) return false;
    if (pkt->cmd == MemCmd::CleanEvict) return false;

    if (onMiss) {
        return miss;
    }

    return true;
}

bool
Base::inCache(Addr addr, bool is_secure) const
{
    return cache->inCache(addr, is_secure);
}

bool
Base::inMissQueue(Addr addr, bool is_secure) const
{
    return cache->inMissQueue(addr, is_secure);
}

bool
Base::hasBeenPrefetched(Addr addr, bool is_secure) const
{
    return cache->hasBeenPrefetched(addr, is_secure);
}

bool
Base::hasEverBeenPrefetched(Addr addr, bool is_secure) const
{
    return cache->hasEverBeenPrefetched(addr, is_secure);
}

bool
Base::samePage(Addr a, Addr b) const
{
    return roundDown(a, pageBytes) == roundDown(b, pageBytes);
}

Addr
Base::blockAddress(Addr a) const
{
    return a & ~((Addr)blkSize-1);
}

Addr
Base::blockIndex(Addr a) const
{
    return a >> lBlkSize;
}

Addr
Base::pageAddress(Addr a) const
{
    return roundDown(a, pageBytes);
}

Addr
Base::pageOffset(Addr a) const
{
    return a & (pageBytes - 1);
}

Addr
Base::pageIthBlockAddress(Addr page, uint32_t blockIndex) const
{
    return page + (blockIndex << lBlkSize);
}

void
Base::nofityHitToDownStream(const PacketPtr &pkt)
{
    // allow non-demand notify for downstream
    PrefetchSourceType pf_source = cache->getHitBlkXsMetadata(pkt).prefetchSource;
    float acc = (prefetchStats.pfUseful_srcs[pf_source].value()) /
        (prefetchStats.pfDequeued_srcs[pf_source].value());
    DPRINTF(HWPrefetch, "Notify data read resp pkt to down stream prefetch, especially for CDP\n");
    hintDownStream->pfHitNotify(acc, pf_source, pkt);
}
void
Base::probeNotify(const PacketPtr &pkt, bool miss)
{
    DPRINTF(HWPrefetch, "ProbeNotify: %s for %s\n", miss ? "miss" : "hit",
            pkt->print());
    // Don't notify prefetcher on SWPrefetch, HWPrefetch, cache maintenance
    // operations or for writes that we are coaslescing.
    if (pkt->cmd.isSWPrefetch()) return;
    if (pkt->cmd.isHWPrefetch()) return;
    if (pkt->req->isCacheMaintenance()) return;

    if (!pkt->isDemand() && !pkt->cmd.isHWPrefetch()) {
        DPRINTF(HWPrefetch, "Skip pf calc because not demand\n");
        return;
    }

    if (pkt->req->isFirstReqAfterSquash()) {
        squashMark = true;
    }

    if (pkt->isWrite() && cache != nullptr && cache->coalesce()) return;
    if (!pkt->req->hasPaddr()) {
        panic("Request must have a physical address");
    }

    DPRINTF(HWPrefetch, "Reach condition checked\n");

    if (pkt->isDemand()) {
        notifyDemandAccess(pkt->getAddr(), pkt->isSecure(), miss);
    }

    if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
        usefulPrefetches += 1;
        prefetchStats.pfUseful++;
        PrefetchSourceType pf_source = cache->getHitBlkXsMetadata(pkt).prefetchSource;
        prefetchStats.pfUseful_srcs[pf_source]++;
        notifyPrefetchUseful(pf_source);
        if (miss)
            // This case happens when a demand hits on a prefetched line
            // that's not in the requested coherency state.
            prefetchStats.pfUsefulButMiss++;
    }

    // Some prefetchers require the uncoalesced demand-access stream even
    // when the normal prefetch-training policy only observes misses or
    // prefetch hits.  Keep this hook before observeAccess() so the latter's
    // prefetch_on_access/prefetch_on_pf_hit settings remain legacy-policy
    // controls.  Hardware-prefetch requests are excluded explicitly even if
    // a packet type also reports itself as demand.
    if (pkt->isDemand() && !pkt->req->isPrefetch() &&
        !pkt->req->isUncacheable()) {
        observeRawDemandAccess(pkt, miss);
    }

    // Verify this access type is observed by prefetcher
    if (observeAccess(pkt, miss)) {
        PrefetchSourceType pf_source;
        int pf_depth;
        if (!miss) {
            pf_source = cache->getHitBlkXsMetadata(pkt).prefetchSource;
            pf_depth = cache->getHitBlkXsMetadata(pkt).prefetchDepth;
        } else {  // miss & late
            pf_source = pkt->getPFSource();
            pf_depth = pkt->getPFDepth();
        }
        if (!useVirtualAddresses || pkt->req->hasVaddr()) {
            // condition1:  useVirtualAddresses && pkt->req->hasVaddr()
            // condition2: !useVirtualAddresses

            Addr addr = pkt->req->hasVaddr() ? pkt->req->getVaddr() : pkt->req->getPaddr();
            Request::XsMetadata xsMetadata(pf_source, pf_depth);

            // Query and save all state information needed for training
            bool everPrefetched = hasEverBeenPrefetched(pkt->getAddr(), pkt->isSecure());
            bool pfFirstHit = !miss && hasBeenPrefetched(pkt->getAddr(), pkt->isSecure());
            bool pfHit = !miss && everPrefetched;
            bool currentSquashMark = squashMark;
            squashMark = false;

            // TrainFilter: Collect training requests into temporary buffers
            if (useTrainingBuffer()) {
                // Extract ROB sequence number from packet metadata
                InstSeqNum seqNum = getSeqNum(pkt);
                Addr blockAddr = getBlockAddr(addr);
                bool isLoad = isLoadRequest(pkt);

                // Collect into Load or Store temporary buffer based on request type
                if (isLoad) {
                    currentCycleLoads.emplace_back(
                        pkt, addr, miss, xsMetadata,
                        everPrefetched, pfFirstHit, pfHit, currentSquashMark,
                        seqNum, blockAddr, isLoad
                    );
                    DPRINTF(HWPrefetch, "TrainFilter: Collected Load [seq=%lu, blk=%#x]\n",
                            seqNum, blockAddr);
                } else {
                    currentCycleStores.emplace_back(
                        pkt, addr, miss, xsMetadata,
                        everPrefetched, pfFirstHit, pfHit, currentSquashMark,
                        seqNum, blockAddr, isLoad
                    );
                    DPRINTF(HWPrefetch, "TrainFilter: Collected Store [seq=%lu, blk=%#x]\n",
                            seqNum, blockAddr);
                }

                if (!cycleEvent.scheduled()) {
                    schedule(cycleEvent, clockEdge(Cycles(1)));
                    DPRINTF(HWPrefetch, "TrainFilter: Scheduled processCycle for next cycle\n");
                }
            } else {
                // When not using buffer, create PrefetchInfo immediately and train
                PrefetchInfo pfi(pkt, addr, miss, xsMetadata);
                pfi.setReqAfterSquash(currentSquashMark);
                pfi.setEverPrefetched(everPrefetched);
                pfi.setPfFirstHit(pfFirstHit);
                pfi.setPfHit(pfHit);
                notify(pkt, pfi);
            }
        } else {
            DPRINTF(HWPrefetch, "Skip req addr %x, has vaddr: %i\n",
                    pkt->req->hasVaddr() ? pkt->req->getVaddr() : pkt->req->getPaddr(), pkt->req->hasVaddr());
        }
    } else {
        DPRINTF(HWPrefetch, "Skip req addr %x, miss: %x for prefetcher\n",
                pkt->req->hasVaddr() ? pkt->req->getVaddr() : pkt->req->getPaddr(), miss);
    }
}

void
Base::processCycle()
{
    DPRINTF(HWPrefetch, "=== TrainFilter Cycle @ Tick %lu ===\n", curTick());

    // Step 1: Flush previous cycle's collected requests into trainingBuffer
    if (!currentCycleLoads.empty() || !currentCycleStores.empty()) {
        flushCurrentCycleRequests();
    }

    // Step 2: Train one request from trainingBuffer (if available)
    if (!trainingBuffer.empty()) {
        processTraining();
    }

    bool hasWork = !currentCycleLoads.empty() ||
                   !currentCycleStores.empty() ||
                   !trainingBuffer.empty();

    if (hasWork && !cycleEvent.scheduled()) {
        schedule(cycleEvent, clockEdge(Cycles(1)));
        DPRINTF(HWPrefetch, "TrainFilter: Rescheduled (pending work: %d loads, %d stores, %d in buffer)\n",
                currentCycleLoads.size(), currentCycleStores.size(), trainingBuffer.size());
    } else if (!hasWork) {
        DPRINTF(HWPrefetch, "TrainFilter: No work remaining, stopping cycle event\n");
    }
}

void
Base::flushCurrentCycleRequests()
{
    if (currentCycleLoads.empty() && currentCycleStores.empty()) {
        return;
    }

    DPRINTF(HWPrefetch, "TrainFilter: Flushing %d Loads, %d Stores\n",
            currentCycleLoads.size(), currentCycleStores.size());

    // Step 1: Sort Load group by ROB order (oldest first)
    std::sort(currentCycleLoads.begin(), currentCycleLoads.end(),
              [](const TrainingRequest &a, const TrainingRequest &b) {
                  return a.seqNum < b.seqNum;  // Ascending order (oldest first)
              });

    // Step 2: Sort Store group by ROB order (oldest first)
    std::sort(currentCycleStores.begin(), currentCycleStores.end(),
              [](const TrainingRequest &a, const TrainingRequest &b) {
                  return a.seqNum < b.seqNum;
              });

    // Step 3: Merge into [Loads..., Stores...] sequence
    std::vector<TrainingRequest> sortedRequests;
    sortedRequests.reserve(currentCycleLoads.size() + currentCycleStores.size());

    for (auto &req : currentCycleLoads) {
        sortedRequests.push_back(std::move(req));
    }

    for (auto &req : currentCycleStores) {
        sortedRequests.push_back(std::move(req));
    }

    DPRINTF(HWPrefetch, "TrainFilter: Reordered sequence: ");
    for (const auto &req : sortedRequests) {
        DPRINTFR(HWPrefetch, "[%s%lu] ", req.isLoad ? "L" : "S", req.seqNum);
    }
    DPRINTFR(HWPrefetch, "\n");

    // Step 4: Filter and insert into trainingBuffer
    for (auto &req : sortedRequests) {
        Addr blockAddr = req.blockAddr;
        ContextID context_id = req.req->hasContextId() ?
            req.req->contextId() : InvalidContextID;
        Addr block_key = contextKey(blockAddr, context_id);

        if (trainingBufferBlockAddrs.count(block_key) > 0) {
            DPRINTF(HWPrefetch,
                    "  TrainFilter: Drop [%s%lu, %#x, ctx=%d] - in buffer\n",
                    req.isLoad ? "L" : "S", req.seqNum, blockAddr,
                    context_id);
            continue;
        }

        for (const auto &buffered : trainingBuffer) {
            ContextID buffered_context = buffered.req->hasContextId() ?
                buffered.req->contextId() : InvalidContextID;
            if (buffered.blockAddr == blockAddr &&
                buffered_context != context_id) {
                prefetchStats.trainFilterContextAliases++;
                break;
            }
        }

        if (trainingBuffer.size() >= trainingBufferSize) {
            DPRINTF(HWPrefetch,
                    "  TrainFilter: Drop [%s%lu, %#x, ctx=%d] - buffer full\n",
                    req.isLoad ? "L" : "S", req.seqNum, blockAddr,
                    context_id);
            continue;
        }

        bool isLoad = req.isLoad;
        InstSeqNum seqNum = req.seqNum;

        trainingBuffer.push_back(std::move(req));
        trainingBufferBlockAddrs.insert(block_key);

        DPRINTF(HWPrefetch,
                "  TrainFilter: Enqueue [%s%lu, %#x, ctx=%d] "
                "(buffer: %d)\n",
                isLoad ? "L" : "S", seqNum, blockAddr, context_id,
                trainingBuffer.size());
    }

    currentCycleLoads.clear();
    currentCycleStores.clear();
}

void
Base::processTraining()
{
    if (trainingBuffer.empty()) {
        return;
    }

    TrainingRequest &req = trainingBuffer.front();

    DPRINTF(HWPrefetch, ">>> TrainFilter: Training [%s%lu, %#x] (remaining: %d)\n",
            req.isLoad ? "L" : "S", req.seqNum, req.blockAddr,
            trainingBuffer.size() - 1);

    PacketPtr temp_pkt = new Packet(req.req, req.cmd);

    bool isWrite = temp_pkt->isWrite();
    bool willAccessData = (isWrite || !req.miss) && !temp_pkt->isStorePFTrain();

    if (req.dataCopy != nullptr) {
        temp_pkt->dataDynamic(req.dataCopy);

        const_cast<TrainingRequest&>(req).dataCopy = nullptr;

        DPRINTF(HWPrefetch, "  TrainFilter: Packet with data (%d bytes)\n", req.dataSize);
    } else if (willAccessData) {
        DPRINTF(HWPrefetch, "  TrainFilter: WARNING - Creating dummy data buffer "
                "(original packet had no data, miss=%d, isWrite=%d)\n",
                req.miss, isWrite);

        uint8_t *dummyData = new uint8_t[req.dataSize];
        std::memset(dummyData, 0, req.dataSize);
        temp_pkt->dataDynamic(dummyData);
    } else {
        DPRINTF(HWPrefetch, "  TrainFilter: Packet without data (miss=%d, isWrite=%d)\n",
                req.miss, isWrite);
    }

    PrefetchInfo pfi(temp_pkt, req.addr, req.miss, req.xsMetadata);
    pfi.setReqAfterSquash(req.squashMark);
    pfi.setEverPrefetched(req.everPrefetched);
    pfi.setPfFirstHit(req.pfFirstHit);
    pfi.setPfHit(req.pfHit);
    notify(temp_pkt, pfi);

    delete temp_pkt;

    ContextID context_id = req.req->hasContextId() ?
        req.req->contextId() : InvalidContextID;
    trainingBufferBlockAddrs.erase(contextKey(req.blockAddr, context_id));

    trainingBuffer.pop_front();
}

InstSeqNum
Base::getSeqNum(const PacketPtr &pkt) const
{
    // Try to get seqNum from XsMeta data
    if (pkt->req->getXsMetadata().validXsMetadata &&
        pkt->req->getXsMetadata().instXsMetadata) {
        return pkt->req->getXsMetadata().instXsMetadata->seqNum;
    }

    panic("cannot get valid seqNum\n");

}

bool
Base::isLoadRequest(const PacketPtr &pkt) const
{
    return pkt->isRead() && !pkt->isWrite();
}

void
Base::coreDirectAddrNotify(const PacketPtr& pkt)
{
    assert(pkt->isStorePFTrain());

    DPRINTF(HWPrefetch, "prefetch train request from store\n");

    PrefetchSourceType pf_source = PrefetchSourceType::StoreStream;
    bool miss = true;
    PrefetchInfo pfi(pkt, pkt->req->hasVaddr() ? pkt->req->getVaddr() : pkt->req->getPaddr(), miss,
                     Request::XsMetadata(pf_source));
    pkt->missOnLatePf = true;
    pkt->pfSource = pf_source;
    pfi.setReqAfterSquash(false);
    pfi.setEverPrefetched(false);
    pfi.setPfFirstHit(false);
    pfi.setPfHit(false);
    pfi.setStorePftrain(true);
    notify(pkt, pfi);
}


void
Base::regProbeListeners()
{
    /**
     * If no probes were added by the configuration scripts, connect to the
     * parent cache using the probe "Miss". Also connect to "Hit", if the
     * cache is configured to prefetch on accesses.
     */
    if (listeners.empty() && !isSubPrefetcher && probeManager != nullptr) {
        listeners.push_back(new PrefetchListener(*this, probeManager, "StorePFtrain", false, true, true));
        listeners.push_back(new PrefetchListener(*this, probeManager, "Miss", false, true, false));
        listeners.push_back(new PrefetchListener(*this, probeManager, "Fill", true, false, false));
        listeners.push_back(new PrefetchListener(*this, probeManager, "Hit", false, false, false));
    }
}

void
Base::addEventProbe(SimObject *obj, const char *name)
{
    ProbeManager *pm(obj->getProbeManager());
    listeners.push_back(new PrefetchListener(*this, pm, name));
}

void
Base::addTLB(BaseTLB *t, bool functional)
{
    // tlb is allowed to be non-null, because of taking over
    tlb = t;
    functionalTLB = functional;
}

} // namespace prefetch
} // namespace gem5
