/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

#include "mem/cache/prefetch/cdp.hh"

#include <cstdint>
#include <queue>

#include "base/stats/group.hh"
#include "base/trace.hh"
#include "debug/CDPFilter.hh"
#include "debug/CDPUseful.hh"
#include "debug/CDPdebug.hh"
#include "debug/CDPdepth.hh"
#include "debug/HWPrefetch.hh"
#include "debug/WorkerPref.hh"
#include "mem/cache/base.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "params/CDP.hh"
#include "sim/byteswap.hh"

// similar to x[hi:lo] in verilog

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{
CDP::CDP(const CDPParams &p)
    : Queued(p),
      enableCoordinate(p.enable_coordinate),
      depth_threshold(1),
      degree(3),
      useDynamicDegree(p.use_dynamic_degree),
      accuracyThreshold(p.accuracy_threshold),
      useAccuracyDependentAlignment(p.use_accuracy_dependent_alignment),
      useSv48(p.cdp_use_sv48),
      throttle_aggressiveness(p.throttle_aggressiveness),
      enable_thro(false),
      vpnTable(p.vpn_assoc, p.vpn_entries, p.vpn_indexing_policy,
          p.vpn_replacement_policy, p.vpn_sub_entries, p.vpn_reset_period,
          VpnEntry(p.vpn_sub_entries, p.vpn_reset_period)),
      filterTable(p.filter_table_assoc, p.filter_table_entries,
          p.filter_table_indexing_policy, p.filter_table_replacement_policy,
          FilterTableEntry(p.filter_entry_region_blks)),
      filterRegionBlks(p.filter_entry_region_blks),
      filterEntryGranularityBits(ceil(log2(p.filter_entry_granularity))),
      l3_miss_info(0, 0),
      byteOrder(p.sys->getGuestByteOrder()),
      cdpStats(this)
{
    fatal_if(accuracyThreshold < 0.0 || accuracyThreshold > 1.0,
             "CDP accuracy_threshold must be in [0, 1]");

    for (int i = 0; i < NUM_PF_SOURCES; i++) {
        enable_prf_filter.push_back(false);
    }
    prefetchStatsPtr = &prefetchStats;
    pfLRUFilter = new boost::compute::detail::lru_cache<Addr, Addr>(128);
    // filterEntryGranularity should be power of 2, and greater than cache block size
    assert((p.filter_entry_granularity % 2) == 0 && p.filter_entry_granularity >= 64);
    assert(filterRegionBlks % 2 == 0);
}

bool
CDP::shouldIssueDegreeExtension(float accuracy) const
{
    if (!useDynamicDegree) {
        return true;
    }

    return accuracy > accuracyThreshold;
}

Addr
CDP::cdpVpnKey(Addr addr) const
{
    const unsigned vpn_key_bits = cdpVaddrBits() - CdpVpnKeyShift;
    const Addr vpn_key_mask = (Addr(1) << vpn_key_bits) - 1;

    return (addr >> CdpVpnKeyShift) & vpn_key_mask;
}

bool
CDP::cdpHighBitsAreZero(Addr addr) const
{
    return (addr >> cdpVaddrBits()) == 0;
}

CDP::CDPStats::CDPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(ThrottlingActionDist, statistics::units::Count::get(),
               "Distribution of Throttling Action"),
      ADD_STAT(triggeredInRxNotify, statistics::units::Count::get(),
               "Number of times the prefetcher was triggered in rxNotify"),
      ADD_STAT(triggeredInCalcPf, statistics::units::Count::get(),
               "Number of times the prefetcher was triggered in calculatePrefetch"),
      ADD_STAT(dataNotifyCalled, statistics::units::Count::get(),
               "Number of times the prefetcher was called in hitNotify"),
      ADD_STAT(dataNotifyExitBlockNotFound, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to block not found"),
      ADD_STAT(dataNotifyExitFilter, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to filter"),
      ADD_STAT(dataNotifyExitDepth, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to depth"),
      ADD_STAT(dataNotifyNoAddrFound, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to no address found"),
      ADD_STAT(dataNotifyNoVA, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to no VA"),
      ADD_STAT(usefulInsertFilter, statistics::units::Count::get(),
               "Number of useful prefetch hit decrs filterTable"),
      ADD_STAT(unusefulInsertFilter, statistics::units::Count::get(),
               "Number of not useful prefetch hit incrs filterTable"),
      ADD_STAT(actualFilted, statistics::units::Count::get(),
               "Number of times prefetch req is filted by filterTable"),
      ADD_STAT(dataNotifyNoData, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to no data"),
      ADD_STAT(missNotifyCalled, statistics::units::Count::get(),
               "Number of times the prefetcher was called in missNotify"),
      ADD_STAT(pfHitCDP, statistics::units::Count::get(),
               "Number of times demand access hits cdp prefetched block"),
      ADD_STAT(passedFilter, statistics::units::Count::get(), "Number of prefetch requests passed the filter"),
      ADD_STAT(inserted, statistics::units::Count::get(), "Number of prefetches inserted")
{
    ThrottlingActionDist.init(0, 5, 1).flags(statistics::nozero);
}

void
CDP::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();
    PrefetchSourceType pf_source = pfi.getXsMetadata().prefetchSource;
    int pf_depth = pfi.getXsMetadata().prefetchDepth;
    bool is_l1_prefetch =
        system->getRequestorName(pfi.getRequestorId()).find("dcache.prefetcher") != std::string::npos;
    bool is_l2_prefetch =
        system->getRequestorName(pfi.getRequestorId()).find("l2_caches.prefetcher") != std::string::npos;
    // demand access hits a block which is fetched by cdp previously
    bool pf_hit_cdp = !is_l1_prefetch && !is_l2_prefetch && \
                       pfi.isPfFirstHit() && (pf_source == PrefetchSourceType::CDP);
    if (pf_hit_cdp) {
        cdpStats.pfHitCDP++;
    }
    if (!miss && pfi.getDataPtr() != nullptr) {
        if ((is_l1_prefetch || is_l2_prefetch) && enable_prf_filter[pf_source]) {
            return;
        }
        DPRINTF(CDPdepth, "HIT Depth: %d\n", pfi.getXsMetadata().prefetchDepth);
        if (((pf_depth == 4 || pf_depth == 1))) {
            uint64_t *test_addrs = pfi.getDataPtr();
            std::queue<std::pair<CacheBlk *, Addr>> pt_blks;
            std::vector<uint64_t> addrs;
            switch (byteOrder) {
                case ByteOrder::big:
                    for (int of = 0; of < 8; of++) {
                        addrs.push_back(Addr(betoh(*(uint64_t *)(test_addrs + of))));
                    }
                    break;

                case ByteOrder::little:
                    for (int of = 0; of < 8; of++) {
                        addrs.push_back(Addr(letoh(*(uint64_t *)(test_addrs + of))));
                    }
                    break;

                default:
                    panic(
                        "Illegal byte order in \
                            CDP::notifyFill(const PacketPtr &pkt)\n");
            }
            for (Addr pt_addr : scanPointer(addr, addrs)) {
                vpnTable.update(cdpVpnKey(pt_addr), enable_thro, isLowConfidence());
                sendPFWithFilter(pfi, blockAddress(pt_addr), addresses, 30, PrefetchSourceType::CDP, 1);
                for (int i = 1; i < degree; i++) {
                    if (shouldIssueDegreeExtension(getCdpTrueAccuracy())) {
                        Addr next_pf_addr = blockAddress(pt_addr) + (i * 0x40);
                        vpnTable.update(cdpVpnKey(next_pf_addr), enable_thro, isLowConfidence());
                        sendPFWithFilter(pfi, next_pf_addr, addresses, 1, PrefetchSourceType::CDP, 1);
                    }
                }
                cdpStats.triggeredInCalcPf++;
            }
        }
    } else if (miss) {
        cdpStats.missNotifyCalled++;
        DPRINTF(CDPUseful, "Miss addr: %#llx\n", addr);
    }
    if (!is_l1_prefetch && !is_l2_prefetch) {
        recordUsedPrefetch(pfi.getPaddr());
        addToVpnTable(pfi.getAddr(), pf_hit_cdp);
    }
    return;
}

void
CDP::notifyFill(const PacketPtr &pkt, std::vector<AddrPriority> &addresses)
{
    // on refill
    notifyWithData(pkt, false, addresses);
}

void
CDP::notifyWithData(const PacketPtr &pkt, bool is_l1_use, std::vector<AddrPriority> &addresses)
{
    cdpStats.dataNotifyCalled++;
    assert(pkt);
    assert(cache);
    uint64_t test_addr = 0;
    std::vector<uint64_t> addrs;
    float trueAccuracy = getCdpTrueAccuracy();
    if (pkt->hasData() && pkt->req->hasVaddr()) {
        DPRINTF(CDPdebug, "Notify with data received for addr: %#llx, pkt size: %lu\n", pkt->req->getVaddr(),
                pkt->getSize());

        auto *blk_data = cache->findBlock(pkt->getAddr(), pkt->isSecure());
        if (!blk_data) {
            cdpStats.dataNotifyExitBlockNotFound++;
            return;
        }
        Request::XsMetadata pkt_meta = cache->getHitBlkXsMetadata(pkt);
        size_t prefetch_type = system->getRequestorName(pkt->req->requestorId()).find("dcache.prefetcher");
        int pf_depth = pkt_meta.prefetchDepth;
        PrefetchSourceType pf_source = pkt_meta.prefetchSource;
        if (!is_l1_use && prefetch_type != std::string::npos) {
            if (enable_prf_filter[pkt->req->getXsMetadata().prefetchSource]) {
                cdpStats.dataNotifyExitFilter++;
                return;
            }
        }
        const uint64_t *test_addr_start = (const uint64_t *)blk_data;
        unsigned max_offset = blkSize / sizeof(uint64_t);
        switch (byteOrder) {
            case ByteOrder::big:
                for (unsigned of = 0; of < max_offset; of++) {
                    addrs.push_back(Addr(betoh(*(uint64_t *)(test_addr_start + of))));
                }
                break;

            case ByteOrder::little:
                for (unsigned of = 0; of < max_offset; of++) {
                    addrs.push_back(Addr(letoh(*(uint64_t *)(test_addr_start + of))));
                }
                break;

            default:
                panic(
                    "Illegal byte order in \
                        CDP::notifyFill(const PacketPtr &pkt)\n");
        };

        if (hasHintDownStream())
            l3_miss_info = hintDownStream->rxMembusRatio(parentRid);
        if (mpki < 1)
            return;
        if (l3_miss_info.second > 100) {
            float membus_ratio = l3_miss_info.first * 1.0 / l3_miss_info.second;
            if (membus_ratio > 0.4 && mpki < 100) {
                if (trueAccuracy < 0.2) {
                    enable_thro = true;
                } else {
                    enable_thro = false;
                }
                if (mpki < 2) {
                    return;
                }
            } else {
                enable_thro = false;
            }
        }
        unsigned sentCount = 0;
        for (int of = 0; of < max_offset; of++) {
            test_addr = addrs[of];
            int align_bit = BITS(test_addr, 1, 0);
            if (useAccuracyDependentAlignment) {
                if (trueAccuracy < 0.05) {
                    align_bit = BITS(test_addr, 10, 0);
                } else if (trueAccuracy < 0.01) {
                    align_bit = BITS(test_addr, 11, 0);
                }
            }
            int vpn0 = BITS(test_addr, 20, 12);
            Addr vpn_key = cdpVpnKey(test_addr);
            bool flag = true;
            if (!cdpHighBitsAreZero(test_addr) || vpn0 == 0 ||
                align_bit != 0 || !vpnTable.search(vpn_key)) {
                flag = false;
            }
            Addr test_addr2 = Addr(test_addr);
            if (flag) {
                if (pf_depth >= depth_threshold) {
                    cdpStats.dataNotifyExitDepth++;
                    return;
                }
                int next_depth = 0;
                if (pf_depth == 0) {
                    next_depth = 4;
                } else {
                    next_depth = pf_depth + 1;
                }
                vpnTable.update(vpn_key, enable_thro, isLowConfidence());
                sendPFWithFilter(pkt, blockAddress(test_addr2), addresses, 29 + next_depth, PrefetchSourceType::CDP,
                                 next_depth);
                for (int i = 1; i < degree; i++) {
                    if (shouldIssueDegreeExtension(trueAccuracy)) {
                        Addr next_pf_addr = blockAddress(test_addr2) + (i * 0x40);
                        vpnTable.update(cdpVpnKey(next_pf_addr), enable_thro, isLowConfidence());
                        sendPFWithFilter(pkt, next_pf_addr, addresses, 1, PrefetchSourceType::CDP,
                                     next_depth);
                    }
                }
                cdpStats.triggeredInRxNotify++;
                sentCount++;
            }
        }
        if (sentCount == 0) {
            cdpStats.dataNotifyNoAddrFound++;
        }
    }
    if (!pkt->req->hasVaddr())
        cdpStats.dataNotifyNoVA++;
    if (!pkt->hasData())
        cdpStats.dataNotifyNoData++;
    return;
}

void
CDP::pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt,
                 std::vector<AddrPriority> &addresses)
{
    if (accuracy < 0.1) {
        enable_prf_filter[pf_source] = true;
    } else {
        enable_prf_filter[pf_source] = false;
    }
    notifyWithData(pkt, true, addresses);
    if (pkt->req->hasVaddr()) {
        addToVpnTable(pkt->req->getVaddr(), false);
    }
}

bool
CDP::sendPFWithFilter(const PacketPtr &pkt, Addr addr, std::vector<AddrPriority> &addresses,
    int prio, PrefetchSourceType pfSource, int pf_depth)
{
    //fake a PrefetchInfo, thus this reill pf will use Queued::PFSendEventWrapper to send out pf req
    PrefetchInfo pfi(pkt, pkt->req->getVaddr(), false);
    pfi.setTriggerInfo(pkt);

    InsertPFRequestToBuffer(AddrPriority(addr, prio, pfSource, pfi.trigger_info));
    Addr filter_key = sharedFilterKey(pfi, addr);
    if (pfLRUFilter->contains(filter_key)) {
        return false;
    } else {
        pfLRUFilter->insert(filter_key, 0);
        AddrPriority addr_prio = AddrPriority(addr, prio, pfSource);
        addr_prio.depth = pf_depth;
        addresses.push_back(addr_prio);
        cdpStats.passedFilter++;
        return true;
    }
    return false;
}

bool
CDP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
    int prio, PrefetchSourceType pfSource, int pf_depth)
{
    InsertPFRequestToBuffer(AddrPriority(addr, prio, pfSource, pfi.trigger_info));
    Addr filter_key = sharedFilterKey(pfi, addr);
    if (pfLRUFilter->contains(filter_key)) {
        return false;
    } else {
        pfLRUFilter->insert(filter_key, 0);
        AddrPriority addr_prio = AddrPriority(addr, prio, pfSource);
        addr_prio.depth = pf_depth;
        addresses.push_back(addr_prio);
        cdpStats.passedFilter++;
        return true;
    }
    return false;
}

void
CDP::addToVpnTable(Addr addr, bool pf_hit_cdp)
{
    Addr vpn_key = cdpVpnKey(addr);
    int vpn0 = BITS(addr, 20, 12);
    int page_offset = BITS(addr, 11, 0);
    vpnTable.add(vpn_key, pf_hit_cdp);
    vpnTable.resetConfidence(throttle_aggressiveness, enable_thro, isLowConfidence());
    DPRINTF(CDPdebug, "Sv%u, ADDR:%#llx, vpn key:%#llx, vpn0:%#llx, page offset:%#llx\n",
            cdpVaddrBits(), addr, vpn_key, Addr(vpn0), Addr(page_offset));
}

void
CDP::insertFilterTable(Addr addr, bool useful)
{
    Addr filter_addr = filterTableAddr(addr);
    Addr filter_tag = filter_addr / filterRegionBlks;

    FilterTableEntry* entry = filterTable.findEntry(filter_tag, true);

    if (entry) {
        filterTable.accessEntry(entry);
        if (useful) {
            entry->unSetFilter(filter_addr);
        } else {
            entry->setFilter(filter_addr);
        }
    } else if (!useful) {
        entry = filterTable.findVictim(filter_tag);
        entry->reset();
        entry->setFilter(filter_addr);
        filterTable.insertEntry(filter_tag, true, entry);
    }
}

bool
CDP::needFilter(Addr addr)
{
    Addr filter_addr = filterTableAddr(addr);
    Addr filter_tag = filter_addr / filterRegionBlks;

    FilterTableEntry* entry = filterTable.findEntry(filter_tag, true);

    if (entry) {
        bool f = entry->needFilter(filter_addr);
        if (f) {
            cdpStats.actualFilted++;
        }
        return f;
    } else {
        return false;
    }
}

void
CDP::recordUsedPrefetch(Addr addr)
{
    // prefetch hit a cdp prefetched block
    // decrement the confidence of related region (+1)
    cdpStats.usefulInsertFilter++;
    DPRINTF(CDPFilter, "CDP [Y]: %#llx\n", blockAddress(addr));
    insertFilterTable(addr, true);
}

void
CDP::recordUnusedPrefetch(Addr addr)
{
    // cache evicts a cdp prefetched block
    // decrement the confidence of related region (-1)
    cdpStats.unusefulInsertFilter++;
    DPRINTF(CDPFilter, "CDP [X]: %#llx\n", blockAddress(addr));
    insertFilterTable(addr, false);
}


}  // namespace prefetch
}  // namespace gem5
