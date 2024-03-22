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

/**
 * Implementation of the Content-Directed Data Prefetching
 *
 * References:
 * Stateless, Content-Directed Data Prefetching Mechanism
 * Robert Cooksey, Stephan Jourdan
 */

#ifndef __MEM_CACHE_PREFETCH_CDP_HH__
#define __MEM_CACHE_PREFETCH_CDP_HH__

#define BITMASK(bits) ((1ull << (bits)) - 1)
#define BITS(x, hi, lo) (((x) >> (lo)) & BITMASK((hi) - (lo) + 1))
#include <list>
#include <map>
#include <string>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/types.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/CDP.hh"
#include "sim/system.hh"

#ifdef SIG_DEBUG_PRINT
#define SIG_DP(x) x
#else
#define SIG_DP(x)
#endif
namespace gem5
{

struct CDPParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{
class CDP : public Queued
{


    std::vector<bool> enable_prf_filter;
    std::vector<bool> enable_prf_filter2;
    int depth_threshold;
    float throttle_aggressiveness;
    bool enable_thro;
    /** Byte order used to access the cache */
    /** Update the RR right table after a prefetch fill */

    class VpnTable
    {
      public:
        std::map<int, std::map<int, int>> vpns;
        std::map<int, std::map<int, int>> hotVpns;
        int counter{0};
        void add(int vpn2, int vpn1)
        {
            counter++;
            if (vpns.find(vpn2) == vpns.end()) {
                std::map<int, int> sub_map;
                sub_map[vpn1] = 1;
                vpns[vpn2] = sub_map;
            } else if (vpns[vpn2].find(vpn1) == vpns[vpn2].end()) {
                vpns[vpn2][vpn1] = 1;
            } else {
                vpns[vpn2][vpn1] += 1;
            }
        }
        void resetConfidence(float throttle_aggressiveness, bool enable_thro)
        {
            if (counter < 128)
                return;
            hotVpns.clear();
            for (auto pair2 : vpns) {
                for (auto pair1 : pair2.second) {
                    if (pair1.second > counter / 16 || enable_thro) {
                        hotVpns[pair2.first][pair1.first] = pair1.second * throttle_aggressiveness;
                    }
                }
            }
            counter = 0;
            vpns.clear();
        }
        bool search(int vpn2, int vpn1)
        {
            if (hotVpns.find(vpn2) != hotVpns.end() && hotVpns[vpn2].find(vpn1) != hotVpns[vpn2].end()) {
                if (hotVpns[vpn2][vpn1] > 0) {
                    return true;
                }
            }
            return false;
        }
        void update(int vpn2, int vpn1, bool enable_thro)
        {
            if (enable_thro) {
                hotVpns[vpn2][vpn1]--;
            }
        }
        VpnTable() { resetConfidence(2, false); }
    } vpnTable;

  public:
    StatGroup *prefetchStatsPtr = nullptr;
    RequestorID parentRid;
    std::pair<long, long> l3_miss_info;  // (cdp_l3_miss,l3_total_miss_num)
    float mpki = 1;
    void setStatsPtr(StatGroup *ptr) { prefetchStatsPtr = ptr; }
    void notifyIns(int ins_num) override
    {
        if (l3_miss_info.second != 0) {
            mpki = l3_miss_info.second * 1000.0 / ins_num;
        }
    }
    bool sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio, PrefetchSourceType pfSource,
                          int pf_depth);

    CDP(const CDPParams &p);

    ~CDP()
    {
        // Delete the pfLRUFilter pointer to release memory
        Queued::~Queued();
        delete pfLRUFilter;
    }

    ByteOrder byteOrder;

    using Queued::notifyFill;
    void notifyFill(const PacketPtr &pkt, std::vector<AddrPriority> &addresses);

    void notifyWithData(const PacketPtr &pkt, bool is_l1_use, std::vector<AddrPriority> &addresses);

    using Queued::pfHitNotify;
    void pfHitNotify(float accuracy, PrefetchSourceType pf_source, const PacketPtr &pkt,
                     std::vector<AddrPriority> &addresses);

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override;

    void addToVpnTable(Addr vaddr);

    std::vector<Addr> scanPointer(Addr addr, std::vector<uint64_t> addrs)
    {
        uint64_t test_addr;
        std::vector<Addr> ans;
        for (int of = 0; of < 8; of++) {
            test_addr = addrs[of];
            int align_bit = BITS(test_addr, 1, 0);
            int filter_bit = BITS(test_addr, 5, 0);
            int page_offset, vpn0, vpn1, vpn2, check_bit;
            check_bit = BITS(test_addr, 63, 39);
            vpn2 = BITS(test_addr, 38, 30);
            vpn1 = BITS(test_addr, 29, 21);
            vpn0 = BITS(test_addr, 20, 12);
            page_offset = BITS(test_addr, 11, 0);
            bool flag = true;
            if ((check_bit != 0) || (!vpnTable.search(vpn2, vpn1)) || (vpn0 == 0) || (align_bit != 0)) {
                flag = false;
            }
            Addr test_addr2 = Addr(test_addr);
            if (flag) {
                ans.push_back(test_addr2);
            }
        }
        return ans;
    };


    boost::compute::detail::lru_cache<Addr, Addr> *pfLRUFilter;
    std::list<DeferredPacket> localBuffer;
    unsigned depth{4};

    struct CDPStats : public statistics::Group
    {
        CDPStats(statistics::Group *parent);
        // STATS
        statistics::Scalar triggeredInRxNotify;
        statistics::Scalar triggeredInCalcPf;
        statistics::Scalar dataNotifyCalled;
        statistics::Scalar dataNotifyExitBlockNotFound;
        statistics::Scalar dataNotifyExitFilter;
        statistics::Scalar dataNotifyExitDepth;
        statistics::Scalar dataNotifyNoAddrFound;
        statistics::Scalar dataNotifyNoVA;
        statistics::Scalar dataNotifyNoData;
        statistics::Scalar missNotifyCalled;
        statistics::Scalar passedFilter;
        statistics::Scalar inserted;
    } cdpStats;
};

}  // namespace prefetch
}  // namespace gem5

#endif /* __MEM_CACHE_PREFETCH_IPCP_FIRST_LEVEL_HH__ */
