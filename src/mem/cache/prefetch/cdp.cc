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
      enableLruFilterBeforePfBuffer(p.enable_lru_filter_before_pf_buffer),
      depth_threshold(1),
      degree(p.cdp_degree),
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
    for (int i = 0; i < PrefetchSourceType::NUM_PF_SOURCES; i++) {
        enable_prf_filter.push_back(false);
    }
    prefetchStatsPtr = &prefetchStats;
    pfLRUFilter = new boost::compute::detail::lru_cache<Addr, Addr>(128);
    // filterEntryGranularity should be power of 2, and greater than cache block size
    assert((p.filter_entry_granularity % 2) == 0 && p.filter_entry_granularity >= 64);
    assert(filterRegionBlks % 2 == 0);
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
      ADD_STAT(inserted, statistics::units::Count::get(), "Number of prefetches inserted"),
      ADD_STAT(vpnCandidateChecked, statistics::units::Count::get(),
               "Number of pointer-sized words checked as CDP VPN candidates"),
      ADD_STAT(vpnCandidateValidAddr, statistics::units::Count::get(),
               "Number of CDP VPN candidates that pass high-bit, vpn0, and alignment checks"),
      ADD_STAT(vpnCandidateInvalidHigh, statistics::units::Count::get(),
               "Number of CDP VPN candidates with non-zero high address bits"),
      ADD_STAT(vpnCandidateInvalidVpn0, statistics::units::Count::get(),
               "Number of CDP VPN candidates with zero vpn0"),
      ADD_STAT(vpnCandidateInvalidAlign, statistics::units::Count::get(),
               "Number of CDP VPN candidates with non-zero low alignment bits"),
      ADD_STAT(vpnSearchAccess, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups after address-format checks"),
      ADD_STAT(vpnSearchHit, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups that hit a hot sub-entry"),
      ADD_STAT(vpnSearchMiss, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups that do not hit a hot sub-entry"),
      ADD_STAT(vpnLookupMainHit, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups that hit the main entry tag"),
      ADD_STAT(vpnLookupMainMiss, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups that miss the main entry tag"),
      ADD_STAT(vpnLookupSubHit, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups that hit an existing sub-entry"),
      ADD_STAT(vpnLookupSubMiss, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups that miss the sub-entry"),
      ADD_STAT(vpnLookupHot, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups whose sub-entry is hot"),
      ADD_STAT(vpnLookupHitNotHot, statistics::units::Count::get(),
               "Number of CDP vpnTable lookups that hit an existing sub-entry but it is not hot"),
      ADD_STAT(vpnTrainAccess, statistics::units::Count::get(),
               "Number of CDP vpnTable train accesses"),
      ADD_STAT(vpnTrainPfHitCDP, statistics::units::Count::get(),
               "Number of CDP vpnTable trains caused by a CDP prefetch first hit"),
      ADD_STAT(vpnTrainAllocMain, statistics::units::Count::get(),
               "Number of CDP vpnTable trains that allocate a main entry"),
      ADD_STAT(vpnTrainAllocSub, statistics::units::Count::get(),
               "Number of CDP vpnTable trains that allocate a sub-entry in an existing main entry"),
      ADD_STAT(vpnTrainNoAlloc, statistics::units::Count::get(),
               "Number of CDP vpnTable trains that update an existing sub-entry"),
      ADD_STAT(vpnTrainRefresh, statistics::units::Count::get(),
               "Number of CDP vpnTable confidence refresh events"),
      ADD_STAT(vpnTrainRefreshEntries, statistics::units::Count::get(),
               "Number of valid CDP vpnTable main entries visited by confidence refresh"),
      ADD_STAT(vpnTrainUpdateAccess, statistics::units::Count::get(),
               "Number of CDP vpnTable update attempts from generated prefetch targets"),
      ADD_STAT(vpnTrainUpdateHit, statistics::units::Count::get(),
               "Number of CDP vpnTable update attempts that hit an existing sub-entry"),
      ADD_STAT(vpnTrainUpdateMiss, statistics::units::Count::get(),
               "Number of CDP vpnTable update attempts that miss an existing sub-entry"),
      ADD_STAT(filterLookupAccess, statistics::units::Count::get(),
               "Number of CDP filterTable lookup accesses"),
      ADD_STAT(filterLookupHit, statistics::units::Count::get(),
               "Number of CDP filterTable lookup accesses that hit an entry"),
      ADD_STAT(filterLookupMiss, statistics::units::Count::get(),
               "Number of CDP filterTable lookup accesses that miss an entry"),
      ADD_STAT(filterDropBySaturated, statistics::units::Count::get(),
               "Number of CDP prefetch requests dropped by a saturated filterTable counter"),
      ADD_STAT(filterPassByNotSaturated, statistics::units::Count::get(),
               "Number of CDP filterTable entry hits that pass because the counter is not saturated"),
      ADD_STAT(filterTrainAccess, statistics::units::Count::get(),
               "Number of CDP filterTable train accesses"),
      ADD_STAT(filterTrainUseful, statistics::units::Count::get(),
               "Number of useful CDP filterTable train accesses"),
      ADD_STAT(filterTrainUnused, statistics::units::Count::get(),
               "Number of unused CDP filterTable train accesses"),
      ADD_STAT(filterTrainAlloc, statistics::units::Count::get(),
               "Number of CDP filterTable train accesses that allocate an entry"),
      ADD_STAT(filterTrainUpdate, statistics::units::Count::get(),
               "Number of CDP filterTable train accesses that update an existing entry"),
      ADD_STAT(filterTrainUsefulHit, statistics::units::Count::get(),
               "Number of useful CDP filterTable trains that hit an existing entry"),
      ADD_STAT(filterTrainUnusedHit, statistics::units::Count::get(),
               "Number of unused CDP filterTable trains that hit an existing entry"),
      ADD_STAT(filterTrainUnusedAlloc, statistics::units::Count::get(),
               "Number of unused CDP filterTable trains that allocate an entry"),
      ADD_STAT(cdpReqGenerated, statistics::units::Count::get(),
               "Number of CDP prefetch requests generated before the local duplicate filter"),
      ADD_STAT(cdpReqDropByLRU, statistics::units::Count::get(),
               "Number of CDP prefetch requests dropped by the local LRU duplicate filter"),
      ADD_STAT(cdpReqDropByBusyPFQ, statistics::units::Count::get(),
               "Number of buffered CDP prefetch requests dropped because the final PFQ is not empty"),
      ADD_STAT(cdpReqPassedLRU, statistics::units::Count::get(),
               "Number of CDP prefetch requests that pass the local LRU duplicate filter"),
      ADD_STAT(cdpReqInsertedToAddresses, statistics::units::Count::get(),
               "Number of CDP prefetch requests inserted into the outgoing address vector"),
      ADD_STAT(cdpReqGeneratedByPath, statistics::units::Count::get(),
               "CDP prefetch requests generated by trigger path"),
      ADD_STAT(cdpReqDropLruByPath, statistics::units::Count::get(),
               "CDP prefetch requests dropped by the local LRU duplicate filter by trigger path"),
      ADD_STAT(cdpReqPassedByPath, statistics::units::Count::get(),
               "CDP prefetch requests that pass the local LRU duplicate filter by trigger path"),
      ADD_STAT(cdpReqGeneratedByTriggerReqType, statistics::units::Count::get(),
               "CDP prefetch requests generated by trigger request type"),
      ADD_STAT(cdpReqDropLruByTriggerReqType, statistics::units::Count::get(),
               "CDP prefetch requests dropped by the local LRU duplicate filter by trigger request type"),
      ADD_STAT(cdpReqPassedByTriggerReqType, statistics::units::Count::get(),
               "CDP prefetch requests that pass the local LRU duplicate filter by trigger request type"),
      ADD_STAT(cdpReqGeneratedByTriggerBlkSource, statistics::units::Count::get(),
               "CDP prefetch requests generated by source of the trigger block"),
      ADD_STAT(cdpReqDropLruByTriggerBlkSource, statistics::units::Count::get(),
               "CDP prefetch requests dropped by the local LRU duplicate filter by source of the trigger block"),
      ADD_STAT(cdpReqPassedByTriggerBlkSource, statistics::units::Count::get(),
               "CDP prefetch requests that pass the local LRU duplicate filter by source of the trigger block")
{
    ThrottlingActionDist.init(0, 5, 1).flags(statistics::nozero);
    cdpReqGeneratedByPath
        .init(CDP_TRIGGER_PATHS)
        .subname(CDP_TRIGGER_CALCULATE, "calculate")
        .subname(CDP_TRIGGER_REFILL, "refill")
        .subname(CDP_TRIGGER_PF_HIT, "pfhit")
        .flags(statistics::total);
    cdpReqDropLruByPath
        .init(CDP_TRIGGER_PATHS)
        .subname(CDP_TRIGGER_CALCULATE, "calculate")
        .subname(CDP_TRIGGER_REFILL, "refill")
        .subname(CDP_TRIGGER_PF_HIT, "pfhit")
        .flags(statistics::total);
    cdpReqPassedByPath
        .init(CDP_TRIGGER_PATHS)
        .subname(CDP_TRIGGER_CALCULATE, "calculate")
        .subname(CDP_TRIGGER_REFILL, "refill")
        .subname(CDP_TRIGGER_PF_HIT, "pfhit")
        .flags(statistics::total);

    cdpReqGeneratedByTriggerReqType
        .init(CDP_TRIGGER_REQ_TYPES)
        .subname(CDP_TRIGGER_DEMAND, "demand")
        .subname(CDP_TRIGGER_PREFETCH, "prefetch")
        .flags(statistics::total);
    cdpReqDropLruByTriggerReqType
        .init(CDP_TRIGGER_REQ_TYPES)
        .subname(CDP_TRIGGER_DEMAND, "demand")
        .subname(CDP_TRIGGER_PREFETCH, "prefetch")
        .flags(statistics::total);
    cdpReqPassedByTriggerReqType
        .init(CDP_TRIGGER_REQ_TYPES)
        .subname(CDP_TRIGGER_DEMAND, "demand")
        .subname(CDP_TRIGGER_PREFETCH, "prefetch")
        .flags(statistics::total);

    cdpReqGeneratedByTriggerBlkSource
        .init(PrefetchSourceType::NUM_PF_SOURCES)
        .flags(statistics::total);
    cdpReqDropLruByTriggerBlkSource
        .init(PrefetchSourceType::NUM_PF_SOURCES)
        .flags(statistics::total);
    cdpReqPassedByTriggerBlkSource
        .init(PrefetchSourceType::NUM_PF_SOURCES)
        .flags(statistics::total);
}

void
CDP::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();
    int page_offset, vpn0, vpn1, vpn2;
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
                vpn2 = BITS(pt_addr, 38, 30);
                vpn1 = BITS(pt_addr, 29, 21);
                updateVpnTableWithStats(vpn2, vpn1);
                sendPFWithFilter(pfi, blockAddress(pt_addr), addresses, 30, PrefetchSourceType::CDP, 1);
                for (int i = 1; i < degree; i++) {
                    Addr next_pf_addr = blockAddress(pt_addr) + (i * 0x40);
                    updateVpnTableWithStats(BITS(next_pf_addr, 38, 30),
                                            BITS(next_pf_addr, 29, 21));
                    sendPFWithFilter(pfi, next_pf_addr, addresses, 1, PrefetchSourceType::CDP, 1);
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
CDP::notifyWithData(const PacketPtr &pkt, bool is_l1_use, std::vector<AddrPriority> &addresses, int trace_site)
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
            int filter_bit = BITS(test_addr, 5, 0);
            int page_offset, vpn0, vpn1, vpn1_addr, vpn2, vpn2_addr, check_bit;
            check_bit = BITS(test_addr, 63, 39);
            vpn2 = BITS(test_addr, 38, 30);
            vpn2_addr = BITS(pkt->req->getVaddr(), 38, 30);
            vpn1 = BITS(test_addr, 29, 21);
            vpn1_addr = BITS(pkt->req->getVaddr(), 29, 21);
            vpn0 = BITS(test_addr, 20, 12);
            page_offset = BITS(test_addr, 11, 0);
            bool flag = true;
            cdpStats.vpnCandidateChecked++;
            if (check_bit != 0) {
                cdpStats.vpnCandidateInvalidHigh++;
            }
            if (vpn0 == 0) {
                cdpStats.vpnCandidateInvalidVpn0++;
            }
            if (align_bit != 0) {
                cdpStats.vpnCandidateInvalidAlign++;
            }
            if ((check_bit != 0) || (vpn0 == 0) || (align_bit != 0)) {
                flag = false;
            } else {
                cdpStats.vpnCandidateValidAddr++;
                if (!vpnTableSearchWithStats(vpn2, vpn1)) {
                    flag = false;
                }
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
                auto trigger_info = makeTriggerInfo(
                    trace_site == 2 ? CDP_TRIGGER_PF_HIT : CDP_TRIGGER_REFILL,
                    pf_source, pkt->cmd.isHWPrefetch());
                updateVpnTableWithStats(vpn2, vpn1);
                sendPFWithFilter(pkt, blockAddress(test_addr2), addresses, 29 + next_depth, PrefetchSourceType::CDP,
                                 next_depth, trace_site, of, test_addr2, 0, trigger_info);
                for (int i = 1; i < degree; i++) {
                    Addr next_pf_addr = blockAddress(test_addr2) + (i * 0x40);
                    updateVpnTableWithStats(BITS(next_pf_addr, 38, 30),
                                            BITS(next_pf_addr, 29, 21));
                    sendPFWithFilter(pkt, next_pf_addr, addresses, 1, PrefetchSourceType::CDP,
                                 next_depth, trace_site, of, test_addr2, i, trigger_info);
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
    notifyWithData(pkt, true, addresses, 2);
    if (pkt->req->hasVaddr()) {
        addToVpnTable(pkt->req->getVaddr(), false);
    }
}

CDP::CdpTriggerInfo
CDP::makeTriggerInfo(CdpTriggerPath path, PrefetchSourceType blk_source,
                     bool is_prefetch) const
{
    CdpTriggerInfo info;
    info.path = path;
    info.reqType = is_prefetch ? CDP_TRIGGER_PREFETCH : CDP_TRIGGER_DEMAND;
    info.blkSource = blk_source;
    return info;
}

void
CDP::recordCdpReqGeneratedTrigger(const CdpTriggerInfo &trigger_info)
{
    cdpStats.cdpReqGeneratedByPath[trigger_info.path]++;
    cdpStats.cdpReqGeneratedByTriggerReqType[trigger_info.reqType]++;
    cdpStats.cdpReqGeneratedByTriggerBlkSource[trigger_info.blkSource]++;
}

void
CDP::recordCdpReqDropLruTrigger(const CdpTriggerInfo &trigger_info)
{
    cdpStats.cdpReqDropLruByPath[trigger_info.path]++;
    cdpStats.cdpReqDropLruByTriggerReqType[trigger_info.reqType]++;
    cdpStats.cdpReqDropLruByTriggerBlkSource[trigger_info.blkSource]++;
}

void
CDP::recordCdpReqPassedTrigger(const CdpTriggerInfo &trigger_info)
{
    cdpStats.cdpReqPassedByPath[trigger_info.path]++;
    cdpStats.cdpReqPassedByTriggerReqType[trigger_info.reqType]++;
    cdpStats.cdpReqPassedByTriggerBlkSource[trigger_info.blkSource]++;
}

bool
CDP::dropByLruBeforePfBuffer(Addr addr)
{
    if (!enableLruFilterBeforePfBuffer) {
        return false;
    }

    Addr blk_addr = blockAddress(addr);
    if (pfLRUFilter->contains(blk_addr)) {
        cdpStats.cdpReqDropByLRU++;
        return true;
    }

    pfLRUFilter->insert(blk_addr, 0);
    return false;
}

bool
CDP::dropPFRequestByBusyPFQ()
{
    if (PFRequestBuffer.empty()) {
        return false;
    }

    PFRequestBuffer.pop_front();
    cdpStats.cdpReqDropByBusyPFQ++;
    return true;
}

bool
CDP::sendPFWithFilter(const PacketPtr &pkt, Addr addr, std::vector<AddrPriority> &addresses,
    int prio, PrefetchSourceType pfSource, int pf_depth)
{
    auto trigger_info = makeTriggerInfo(CDP_TRIGGER_REFILL, PrefetchSourceType::PF_NONE,
                                        pkt->cmd.isHWPrefetch());
    return sendPFWithFilter(pkt, addr, addresses, prio, pfSource, pf_depth, -1, -1, 0, -1,
                            trigger_info);
}

bool
CDP::sendPFWithFilter(const PacketPtr &pkt, Addr addr, std::vector<AddrPriority> &addresses,
    int prio, PrefetchSourceType pfSource, int pf_depth, int trace_site, int scan_word_offset,
    Addr candidate, int degree_idx, const CdpTriggerInfo &trigger_info)
{
    (void)prio;
    //fake a PrefetchInfo, thus this reill pf will use Queued::PFSendEventWrapper to send out pf req
    PrefetchInfo pfi(pkt, pkt->req->getVaddr(), false);
    pfi.setTriggerInfo(pkt);

    prefetchStats.pfGenerated++;
    cdpStats.cdpReqGenerated++;
    recordCdpReqGeneratedTrigger(trigger_info);
    if (dropByLruBeforePfBuffer(addr)) {
        recordCdpReqDropLruTrigger(trigger_info);
        return false;
    }
    int cdp_prio = 1;
    if (!enableLruFilterBeforePfBuffer && pfLRUFilter->contains((addr))) {
        cdpStats.cdpReqDropByLRU++;
        recordCdpReqDropLruTrigger(trigger_info);
        return false;
    } else {
        if (!enableLruFilterBeforePfBuffer) {
            pfLRUFilter->insert((addr), 0);
        }
        AddrPriority addr_prio = AddrPriority(addr, cdp_prio, pfSource, pfi.trigger_info);
        addr_prio.depth = pf_depth;
        InsertPFRequestToBuffer(addr_prio);
        if (!useParentPFBuffer) {
            addresses.push_back(addr_prio);
            cdpStats.cdpReqInsertedToAddresses++;
        }
        cdpStats.cdpReqPassedLRU++;
        recordCdpReqPassedTrigger(trigger_info);
        cdpStats.passedFilter++;
        cdpStats.inserted++;
        if (archDBer && trace_site >= 0) {
            bool has_pc = pkt->req->hasPC();
            Addr trigger_paddr = pkt->req->hasPaddr() ? pkt->req->getPaddr() : pkt->getAddr();
            archDBer->cdpTraceWrite(curTick(), trace_site, has_pc, has_pc ? pkt->req->getPC() : 0,
                                    pkt->req->hasVaddr() ? pkt->req->getVaddr() : 0, trigger_paddr,
                                    scan_word_offset, candidate, addr, degree_idx, pf_depth);
        }
        return true;
    }
    return false;
}

bool
CDP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
    int prio, PrefetchSourceType pfSource, int pf_depth)
{
    auto trigger_info = makeTriggerInfo(CDP_TRIGGER_CALCULATE, pfi.getXsMetadata().prefetchSource,
                                        false);
    return sendPFWithFilter(pfi, addr, addresses, prio, pfSource, pf_depth, -1, -1, 0, -1,
                            trigger_info);
}

bool
CDP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
    int prio, PrefetchSourceType pfSource, int pf_depth, int trace_site, int scan_word_offset,
    Addr candidate, int degree_idx, const CdpTriggerInfo &trigger_info)
{
    (void)prio;
    prefetchStats.pfGenerated++;
    cdpStats.cdpReqGenerated++;
    recordCdpReqGeneratedTrigger(trigger_info);
    if (dropByLruBeforePfBuffer(addr)) {
        recordCdpReqDropLruTrigger(trigger_info);
        return false;
    }
    int cdp_prio = 1;
    if (!enableLruFilterBeforePfBuffer && pfLRUFilter->contains((addr))) {
        cdpStats.cdpReqDropByLRU++;
        recordCdpReqDropLruTrigger(trigger_info);
        return false;
    } else {
        if (!enableLruFilterBeforePfBuffer) {
            pfLRUFilter->insert((addr), 0);
        }
        AddrPriority addr_prio = AddrPriority(addr, cdp_prio, pfSource, pfi.trigger_info);
        addr_prio.depth = pf_depth;
        InsertPFRequestToBuffer(addr_prio);
        if (!useParentPFBuffer) {
            addresses.push_back(addr_prio);
            cdpStats.cdpReqInsertedToAddresses++;
        }
        cdpStats.cdpReqPassedLRU++;
        recordCdpReqPassedTrigger(trigger_info);
        cdpStats.passedFilter++;
        cdpStats.inserted++;
        if (archDBer && trace_site >= 0) {
            bool has_pc = pfi.hasPC();
            archDBer->cdpTraceWrite(curTick(), trace_site, has_pc, has_pc ? pfi.getPC() : 0,
                                    pfi.getAddr(), pfi.getPaddr(), scan_word_offset, candidate,
                                    addr, degree_idx, pf_depth);
        }
        return true;
    }
    return false;
}

void
CDP::recordVpnLookupStats(const VpnTable<VpnEntry>::SearchResult &result)
{
    cdpStats.vpnSearchAccess++;
    if (result.entryHit) {
        cdpStats.vpnLookupMainHit++;
    } else {
        cdpStats.vpnLookupMainMiss++;
    }
    if (result.subEntryHit) {
        cdpStats.vpnLookupSubHit++;
    } else {
        cdpStats.vpnLookupSubMiss++;
    }
    if (result.hot) {
        cdpStats.vpnLookupHot++;
        cdpStats.vpnSearchHit++;
    } else {
        cdpStats.vpnSearchMiss++;
        if (result.subEntryHit) {
            cdpStats.vpnLookupHitNotHot++;
        }
    }
}

bool
CDP::vpnTableSearchWithStats(int vpn2, int vpn1)
{
    auto result = vpnTable.lookup(vpn2, vpn1);
    recordVpnLookupStats(result);
    return result.hot;
}

void
CDP::updateVpnTableWithStats(int vpn2, int vpn1)
{
    cdpStats.vpnTrainUpdateAccess++;
    if (vpnTable.update(vpn2, vpn1, enable_thro, isLowConfidence())) {
        cdpStats.vpnTrainUpdateHit++;
    } else {
        cdpStats.vpnTrainUpdateMiss++;
    }
}

void
CDP::addToVpnTable(Addr addr, bool pf_hit_cdp)
{
    int page_offset, vpn0, vpn1, vpn2;
    vpn2 = BITS(addr, 38, 30);
    vpn1 = BITS(addr, 29, 21);
    vpn0 = BITS(addr, 20, 12);
    page_offset = BITS(addr, 11, 0);
    cdpStats.vpnTrainAccess++;
    if (pf_hit_cdp) {
        cdpStats.vpnTrainPfHitCDP++;
    }
    auto add_result = vpnTable.add(vpn2, vpn1, pf_hit_cdp);
    if (add_result.allocMain) {
        cdpStats.vpnTrainAllocMain++;
    }
    if (add_result.allocSub) {
        cdpStats.vpnTrainAllocSub++;
    }
    if (add_result.noAlloc) {
        cdpStats.vpnTrainNoAlloc++;
    }
    auto reset_result = vpnTable.resetConfidence(throttle_aggressiveness, enable_thro, isLowConfidence());
    if (reset_result.refreshed) {
        cdpStats.vpnTrainRefresh++;
        cdpStats.vpnTrainRefreshEntries += reset_result.entries;
    }
    DPRINTF(CDPdebug, "Sv39, ADDR:%#llx, vpn2:%#llx, vpn1:%#llx, vpn0:%#llx, page offset:%#llx\n", addr, Addr(vpn2),
            Addr(vpn1), Addr(vpn0), Addr(page_offset));
}

void
CDP::insertFilterTable(Addr addr, bool useful)
{
    cdpStats.filterTrainAccess++;
    if (useful) {
        cdpStats.filterTrainUseful++;
    } else {
        cdpStats.filterTrainUnused++;
    }

    Addr filter_addr = filterTableAddr(addr);
    Addr filter_tag = filter_addr / filterRegionBlks;

    FilterTableEntry* entry = filterTable.findEntry(filter_tag, true);

    if (entry) {
        cdpStats.filterTrainUpdate++;
        filterTable.accessEntry(entry);
        if (useful) {
            cdpStats.filterTrainUsefulHit++;
            entry->unSetFilter(filter_addr);
        } else {
            cdpStats.filterTrainUnusedHit++;
            entry->setFilter(filter_addr);
        }
    } else if (!useful) {
        cdpStats.filterTrainAlloc++;
        cdpStats.filterTrainUnusedAlloc++;
        entry = filterTable.findVictim(filter_tag);
        entry->reset();
        entry->setFilter(filter_addr);
        filterTable.insertEntry(filter_tag, true, entry);
    }
}

bool
CDP::needFilter(Addr addr)
{
    cdpStats.filterLookupAccess++;
    Addr filter_addr = filterTableAddr(addr);
    Addr filter_tag = filter_addr / filterRegionBlks;

    FilterTableEntry* entry = filterTable.findEntry(filter_tag, true);

    if (entry) {
        cdpStats.filterLookupHit++;
        bool f = entry->needFilter(filter_addr);
        if (f) {
            cdpStats.actualFilted++;
            cdpStats.filterDropBySaturated++;
        } else {
            cdpStats.filterPassByNotSaturated++;
        }
        return f;
    } else {
        cdpStats.filterLookupMiss++;
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
