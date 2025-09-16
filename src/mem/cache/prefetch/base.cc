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

#include "base/intmath.hh"
#include "debug/HWPrefetch.hh"
#include "debug/PrefetchFilter.hh"
#include "mem/cache/base.hh"
#include "params/BasePrefetcher.hh"
#include "sim/system.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

Base::PrefetchInfo::PrefetchInfo(PacketPtr pkt, bool miss)
  : validVaddr(pkt->req->hasVaddr()),
    vaddress(validVaddr ? pkt->req->getVaddr() : 0),
    validPaddr(pkt->req->hasPaddr()),
    paddress(validPaddr ? pkt->req->getPaddr() : 0),
    validPC(bool(pkt->req->hasPC())),
    pc(validPC ? pkt->req->getPC() : 0),
    requestorId(pkt->req->requestorId()),
    secure(pkt->isSecure()),
    size(pkt->req->getSize()),
    isWriteReq(pkt->isWrite()),
    isMiss(miss)
{
    unsigned int req_size = pkt->req->getSize();
    if (!isWriteReq && miss) {
        data = nullptr;
        data_ptr = nullptr;
    } else if (pkt->isStorePFTrain()) {
        data = nullptr;
        data_ptr = nullptr;
    } else {
        data = new uint8_t[req_size];
        Addr offset = pkt->req->getPaddr() - pkt->getAddr();
        std::memcpy(data, &(pkt->getConstPtr<uint8_t>()[offset]), req_size);
        data_ptr=(uint64_t*)pkt->getConstPtr<uint64_t>();
    }
}

Base::PrefetchInfo::PrefetchInfo(PrefetchInfo const &pfi, Addr addr, bool isVaddr)
  : validVaddr(isVaddr),
    vaddress(validVaddr ? addr : 0),
    validPaddr(!isVaddr),
    paddress(validPaddr ? addr : 0),
    validPC(pfi.validPC),
    pc(pfi.pc),
    requestorId(pfi.requestorId),
    secure(pfi.secure),
    size(pfi.size),
    isWriteReq(pfi.isWriteReq),
    isMiss(pfi.isMiss),
    data(nullptr),
    data_ptr(nullptr)
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
      requestorId(p.sys->getRequestorId(this)),
      isSubPrefetcher(p.is_sub_prefetcher),
      isCompositePrefetcher(p.is_composite_prefetcher),
      blkSize(p.block_size),
      lBlkSize(floorLog2(blkSize)),
      pageSize(p.page_bytes),
      pfOnHitNormalLine(p.on_hit_normal_line),
      pfOnHitPrefetchedLine(p.on_hit_prefetched_line),
      pfOnMiss(p.on_miss),
      pfOnRead(p.on_read),
      pfOnWrite(p.on_write),
      pfOnData(p.on_data),
      pfOnInst(p.on_inst),
      useVirtualAddresses(p.use_virtual_addresses),
      prefetchStats(this),
      archDBer(p.arch_db),
      issuedPrefetches(0),
      usefulPrefetches(0),
      streamlatenum(0)
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
    ADD_STAT(pfIssued, statistics::units::Count::get(),
        "number of hwpf issued"),
    ADD_STAT(pfIssued_srcs, statistics::units::Count::get(),
        "number of hwpf issued"),
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
    ADD_STAT(pfLate, statistics::units::Count::get(),
        "number of late prefetches (hitting in cache, MSHR or WB)")
{
    using namespace statistics;

    pfIssued_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);

    pfUnused.flags(nozero);
    pfUnused_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfUseful_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);

    pfHitInCache_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfHitInMSHR_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);
    pfHitInWB_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);
    late_srcs
        .init(NUM_PF_SOURCES)
        .flags(total);


    accuracy.flags(total);
    accuracy = pfUseful / pfIssued;

    coverage.flags(total);
    coverage = pfUseful / (pfUseful + demandMshrMisses);

    pfLate = pfHitInCache + pfHitInMSHR + pfHitInWB;
}

bool
Base::observeAccess(const PacketPtr &pkt, bool miss) const
{
    bool isInstFetch = pkt->req->isInstFetch();
    bool isRead = pkt->isRead();
    bool isInvalid = pkt->isInvalidate();

    if (!miss) {
        if (pfOnHitPrefetchedLine)
            return hasEverBeenPrefetched(pkt->getAddr(), pkt->isSecure());
        if (!pfOnHitNormalLine)
            return false;
    }

    if (miss && !pfOnMiss) return false;

    // Access Type Checks
    if (pkt->req->isUncacheable()) return false;
    if (isInstFetch  && !pfOnInst) return false;
    if (!isInstFetch && !pfOnData) return false;
    if (!isInstFetch && isRead && !pfOnRead) return false;
    if (!isInstFetch && !isRead && !pfOnWrite) return false;
    if (!isInstFetch && !isRead && isInvalid) return false;
    if (pkt->cmd == MemCmd::CleanEvict) return false;

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
Base::samePage(const Addr a, const Addr b) const
{
    return roundDown(a, pageSize) == roundDown(b, pageSize);
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
    return roundDown(a, pageSize);
}

Addr
Base::pageOffset(Addr a) const
{
    return a & (pageSize - 1);
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
    float acc = (prefetchStats.pfUseful_srcs[pf_source].value()) / (prefetchStats.pfIssued_srcs[pf_source].value());
    DPRINTF(HWPrefetch, "Notify data read resp pkt to down stream prefetch, especially for CDP\n");
    hintDownStream->pfHitNotify(acc, pf_source, pkt);
}

void
Base::probeNotify(const PacketPtr &pkt, bool miss)
{
    DPRINTF(HWPrefetch, "ProbeNotify: %s for %s\n", miss ? "miss" : "hit",
            pkt->print());

    // Don't notify prefetcher on SWPrefetch, cache maintenance
    // operations or for writes that we are coaslescing.
    if (pkt->cmd.isSWPrefetch()) return;
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

    if (hasBeenPrefetched(pkt->getAddr(), pkt->isSecure())) {
        usefulPrefetches += 1;
        prefetchStats.pfUseful++;
        PrefetchSourceType pf_source = cache->getHitBlkXsMetadata(pkt).prefetchSource;
        prefetchStats.pfUseful_srcs[pf_source]++;
        if (miss)
            // This case happens when a demand hits on a prefetched line
            // that's not in the requested coherency state.
            prefetchStats.pfUsefulButMiss++;
    }

    // Verify this access type is observed by prefetcher
    if (observeAccess(pkt, miss)) {
        PrefetchSourceType pf_source = PrefetchSourceType::PF_NONE;
        int pf_depth = 0;
        if (!miss) {    // if hit on cache get the block's prefetch infomation
            pf_source = cache->getHitBlkXsMetadata(pkt).prefetchSource;
            pf_depth = cache->getHitBlkXsMetadata(pkt).prefetchDepth;
        } else {  // if miss or late, get the packet's prefetch infomation
            pf_source = pkt->getPFSource();
            pf_depth = pkt->getPFDepth();
        }
        if (isCompositePrefetcher || !useVirtualAddresses || pkt->req->hasVaddr()) {
        // if (!useVirtualAddresses || pkt->req->hasVaddr()) {
            // condition1: isCompositePrefetcher
            // condition2: !isCompositePrefetcher && !useVirtualAddresses
            // condition3: !isCompositePrefetcher && useVirtualAddresses && pkt->req->hasVaddr()
            PrefetchInfo pfi(pkt, miss);
            pfi.setXsMetadata(Request::XsMetadata(pf_source, pf_depth));
            pfi.setReqAfterSquash(squashMark);
            pfi.setEverPrefetched(hasEverBeenPrefetched(pkt->getAddr(), pkt->isSecure()));
            pfi.setPfFirstHit(!miss && hasBeenPrefetched(pkt->getAddr(), pkt->isSecure()));
            pfi.setPfHit(!miss && hasEverBeenPrefetched(pkt->getAddr(), pkt->isSecure()));
            squashMark = false;
            notify(pkt, pfi);
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
Base::coreDirectAddrNotify(const PacketPtr& pkt)
{
    assert(pkt->isStorePFTrain());

    DPRINTF(HWPrefetch, "prefetch train request from store\n");

    PrefetchSourceType pf_source = PrefetchSourceType::StoreStream;
    bool miss = true;

    pkt->missOnLatePf = true;
    pkt->pfSource = pf_source;

    PrefetchInfo pfi(pkt, miss);
    pfi.setXsMetadata(Request::XsMetadata(pf_source));
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

template<typename Key>
void Base::PrefetchFilter::LRUCache<Key>::insert(const Key& key)
{
    auto it = cache_map.find(key);
    if (it != cache_map.end()) {
        // remove existing key and move it to the front
        cache_list.erase(it->second);
        cache_map.erase(it);
    } else if (cache_list.size() >= capacity) {
        // if capacity is reached, remove the oldest element
        Key oldest = cache_list.back();
        cache_list.pop_back();
        cache_map.erase(oldest);
    }

    // insert the new key at the front
    cache_list.push_front(key);
    cache_map[key] = cache_list.begin();
}

template<typename Key>
bool Base::PrefetchFilter::LRUCache<Key>::contains(const Key& key) const
{
    return cache_map.find(key) != cache_map.end();
}

template<typename Key>
void Base::PrefetchFilter::LRUCache<Key>::clear()
{
    cache_list.clear();
    cache_map.clear();
}

Base::PrefetchFilter::PrefetchFilter(size_t capacity)
    : physicalAddrFilter(capacity),
      virtualAddrFilter(capacity)
{
    // DPRINTF(PrefetchFilter, "Created PrefetchFilter with each capacity %zu\n", each_capacity);
}

Base::PrefetchFilter::PrefetchFilter(size_t physical_capacity, size_t virtual_capacity)
    : physicalAddrFilter(physical_capacity),
      virtualAddrFilter(virtual_capacity)
{
    // DPRINTF(PrefetchFilter, "Created PrefetchFilter with physical capacity %zu and virtual capacity %zu\n",
    //         physical_capacity, virtual_capacity);
}

bool
Base::PrefetchFilter::contains(Addr addr, bool isVA) const
{
    bool ret = isVA ? virtualAddrFilter.contains(addr) : physicalAddrFilter.contains(addr);
    // DPRINTF(PrefetchFilter, "Checking if address %s is in %s filter: %s\n",
    //         isVA ? "virtual" : "physical", addr, ret ? "yes" : "no");
    return ret;
}

void
Base::PrefetchFilter::insert(Addr addr, bool isVA)
{
    if (isVA) {
        virtualAddrFilter.insert(addr);
        // DPRINTF(PrefetchFilter, "Inserted virtual address %s into filter\n", addr);
    } else {
        physicalAddrFilter.insert(addr);
        // DPRINTF(PrefetchFilter, "Inserted physical address %s into filter\n", addr);
    }
}

void
Base::PrefetchFilter::clear(bool isVA)
{
    if (isVA) {
        virtualAddrFilter.clear();
        // DPRINTF(PrefetchFilter, "Cleared virtual address filter\n");
    } else {
        physicalAddrFilter.clear();
        // DPRINTF(PrefetchFilter, "Cleared physical address filter\n");
    }
}




} // namespace prefetch
} // namespace gem5


