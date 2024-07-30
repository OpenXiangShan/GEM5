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
      depth_threshold(3),
      throttle_aggressiveness(p.throttle_aggressiveness),
      enable_thro(false),
      l3_miss_info(0, 0),
      byteOrder(p.sys->getGuestByteOrder()),
      cdpStats(this)
{
    for (int i = 0; i < PrefetchSourceType::NUM_PF_SOURCES; i++) {
        enable_prf_filter.push_back(false);
    }
    prefetchStatsPtr = &prefetchStats;
    pfLRUFilter = new boost::compute::detail::lru_cache<Addr, Addr>(128);
}

CDP::CDPStats::CDPStats(statistics::Group *parent)
    : statistics::Group(parent),
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
      ADD_STAT(dataNotifyNoData, statistics::units::Count::get(),
               "Number of times the prefetcher exited hitNotify due to no data"),
      ADD_STAT(missNotifyCalled, statistics::units::Count::get(),
               "Number of times the prefetcher was called in missNotify"),
      ADD_STAT(passedFilter, statistics::units::Count::get(), "Number of prefetch requests passed the filter"),
      ADD_STAT(inserted, statistics::units::Count::get(), "Number of prefetches inserted")
{
}

void
CDP::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    Addr addr = pfi.getAddr();
    bool miss = pfi.isCacheMiss();
    int page_offset, vpn0, vpn1, vpn2;
    PrefetchSourceType pf_source = pfi.getXsMetadata().prefetchSource;
    int pf_depth = pfi.getXsMetadata().prefetchDepth;
    bool is_prefetch =
        system->getRequestorName(pfi.getRequestorId()).find("dcache.prefetcher") != std::string::npos;
    if (!miss && pfi.getDataPtr() != nullptr) {
        if (is_prefetch && enable_prf_filter[pf_source]) {
            return;
        }
        DPRINTF(CDPdepth, "HIT Depth: %d\n", pfi.getXsMetadata().prefetchDepth);
        if (((pf_depth == 4 || pf_depth == 2))) {
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
                vpnTable.update(vpn2, vpn1, enable_thro);
                sendPFWithFilter(blockAddress(pt_addr), addresses, 30, PrefetchSourceType::CDP, 1);
                cdpStats.triggeredInCalcPf++;
            }
        }
    } else if (miss) {
        cdpStats.missNotifyCalled++;
        DPRINTF(CDPUseful, "Miss addr: %#llx\n", addr);
    }
    if (!is_prefetch) {
        addToVpnTable(pfi.getAddr());
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

        float trueAccuracy = 1;
        if (prefetchStatsPtr->pfIssued_srcs[PrefetchSourceType::CDP].value() > 100) {
            trueAccuracy = (prefetchStatsPtr->pfUseful_srcs[PrefetchSourceType::CDP].value() * 1.0) /
                           (prefetchStatsPtr->pfIssued_srcs[PrefetchSourceType::CDP].value());
        }
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
            if (trueAccuracy < 0.05) {
                align_bit = BITS(test_addr, 10, 0);
            } else if (trueAccuracy < 0.01) {
                align_bit = BITS(test_addr, 11, 0);
            }
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
            if ((check_bit != 0) || (vpn0 == 0) || (align_bit != 0) || (!vpnTable.search(vpn2, vpn1))) {
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
                vpnTable.update(vpn2, vpn1, enable_thro);
                sendPFWithFilter(blockAddress(test_addr2), addresses, 29 + next_depth, PrefetchSourceType::CDP,
                                 next_depth);
                if (trueAccuracy > 0.05) {
                    vpnTable.update(vpn2, vpn1, enable_thro);
                    sendPFWithFilter(blockAddress(test_addr2) + 0x40, addresses, 1, PrefetchSourceType::CDP,
                                     next_depth);
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
        addToVpnTable(pkt->req->getVaddr());
    }
}

bool
CDP::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio, PrefetchSourceType pfSource,
                      int pf_depth)
{
    if (pfLRUFilter->contains((addr))) {
        return false;
    } else {
        pfLRUFilter->insert((addr), 0);
        AddrPriority addr_prio = AddrPriority(addr, prio, pfSource);
        addr_prio.depth = pf_depth;
        addresses.push_back(addr_prio);
        cdpStats.passedFilter++;
        return true;
    }
    return false;
}

void
CDP::addToVpnTable(Addr addr)
{
    int page_offset, vpn0, vpn1, vpn2;
    vpn2 = BITS(addr, 38, 30);
    vpn1 = BITS(addr, 29, 21);
    vpn0 = BITS(addr, 20, 12);
    page_offset = BITS(addr, 11, 0);
    vpnTable.add(vpn2, vpn1);
    vpnTable.resetConfidence(throttle_aggressiveness, enable_thro);
    DPRINTF(CDPdebug, "Sv39, ADDR:%#llx, vpn2:%#llx, vpn1:%#llx, vpn0:%#llx, page offset:%#llx\n", addr, Addr(vpn2),
            Addr(vpn1), Addr(vpn0), Addr(page_offset));
}

}  // namespace prefetch
}  // namespace gem5
