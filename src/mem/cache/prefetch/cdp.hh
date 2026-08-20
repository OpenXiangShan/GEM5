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
#include "debug/CDPHotVpns.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
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
    bool enableCoordinate;
    int depth_threshold;
    int degree;
    bool useDynamicDegree;
    float accuracyThreshold;
    bool useAccuracyDependentAlignment;
    float throttle_aggressiveness;
    bool enable_thro;
    /** Byte order used to access the cache */
    /** Update the RR right table after a prefetch fill */
    class SubVpnEntry
    {
      private:
        u_int64_t refCnt;
        u_int64_t prevRefCnt;
        uint64_t resetPeriod;
        bool exist;
        bool hot;
      public:
        uint64_t debug_vpn1{0};
        uint64_t debug_vpn2{0};
        SubVpnEntry(uint64_t _rstPeriod)
            : refCnt(0),
              prevRefCnt(0),
              resetPeriod(_rstPeriod),
              exist(false),
              hot(false)
        {}
        void init(uint64_t _debug_vpn1, uint64_t _debug_vpn2, bool pf_hit_cdp)
        {
            if (pf_hit_cdp) {
                refCnt = 4;
            } else {
                refCnt = 1;
            }
            prevRefCnt = 0;
            hot = false;
            exist = true;
            debug_vpn1 = _debug_vpn1;
            debug_vpn2 = _debug_vpn2;
        }
        void discard()
        {
            refCnt = 0;
            prevRefCnt = 0;
            hot = false;
            exist = false;
            debug_vpn1 = 0;
            debug_vpn2 = 0;
        }
        void access(bool pf_hit_cdp)
        {
            if (pf_hit_cdp) {
                refCnt += 4;
            } else {
                refCnt++;
            }
        }
        void updateHot(bool low_conf)
        {
            if (low_conf) {
                if (prevRefCnt > (resetPeriod / 16)) {
                    hot = true;
                } else {
                    hot = false;
                }
            } else {
                if (prevRefCnt > 2) {
                    hot = true;
                } else {
                    hot = false;
                }
            }
        }
        void decr(bool low_conf)
        {
            prevRefCnt = prevRefCnt == 0 ? 0 : (prevRefCnt - 1);
            updateHot(low_conf);
        }
        void periodReset(float throttle_aggressiveness, bool enable_thro, bool low_conf)
        {
            if (enable_thro) {
                prevRefCnt = throttle_aggressiveness * refCnt;
            } else {
                prevRefCnt = 0.2 * prevRefCnt + 0.8 * refCnt;
            }

            updateHot(low_conf);
            refCnt = 0;
        }
        bool getHot() { return hot; }
        bool getExist() { return exist; }
        u_int64_t getPrefRefCnt() { return prevRefCnt; }
        std::string name() { return std::string("SubVpnEntry"); }
    };

    class VpnEntry : public TaggedEntry
    {
      private:
        std::vector<SubVpnEntry> subEntries;
        int subEntryNum;
        int subEntryBits;
        uint64_t resetPeriod;
      public:
        VpnEntry(int num, uint64_t _rstPeriod)
            : TaggedEntry(),
              subEntryNum(num),
              resetPeriod(_rstPeriod)
        {
            for (int i = 0; i < subEntryNum; i++) {
                subEntries.emplace_back(SubVpnEntry(resetPeriod));
            }
            subEntryBits = ceil(log2(subEntryNum));
        }
        void discard()
        {
            for (int i = 0; i < subEntryNum; i++) {
                subEntries[i].discard();
            }
        }
        void init(uint64_t vpn1, uint64_t vpn2, bool pf_hit_cdp)
        {
            uint64_t sub_idx = ((vpn2 << 9) | vpn1) & ((1UL << subEntryBits) - 1);
            assert(sub_idx < subEntryNum);
            subEntries[sub_idx].init(vpn1, vpn2, pf_hit_cdp);
        }
        void access(uint64_t idx, bool pf_hit_cdp)
        {
            subEntries[idx].access(pf_hit_cdp);
        }
        void decr(uint64_t idx, bool low_conf)
        {
            subEntries[idx].decr(low_conf);
        }
        void periodReset(float throttle_aggressiveness, bool enable_thro, bool low_conf)
        {
            for (int i = 0; i < subEntryNum; i++) {
                if (subEntries[i].getExist()) {
                    subEntries[i].periodReset(throttle_aggressiveness, enable_thro, low_conf);
                }
            }
        }
        bool getExist(uint64_t idx) { return subEntries[idx].getExist(); }
        bool getHot(uint64_t idx) { return subEntries[idx].getHot(); }
        u_int64_t getPrefRefCnt(uint64_t idx) { return subEntries[idx].getPrefRefCnt(); }
        u_int64_t getDebugVpn1(uint64_t idx) { return subEntries[idx].debug_vpn1; }
        u_int64_t getDebugVpn2(uint64_t idx) { return subEntries[idx].debug_vpn2; }
        std::string name() { return std::string("VpnEntry"); }
    };

    template<class Entry>
    class VpnTable
    {
        private:
            AssociativeSet<Entry> table;
            const uint64_t _resetPeriod;
            uint64_t _resetCounter;
            uint64_t _subEntryNum;
            uint64_t _subEntryBits;

        public:
            VpnTable(int assoc, int num_entries, BaseIndexingPolicy *idx_policy,
            replacement_policy::Base *rpl_policy, int sub_entries,
            uint64_t _rst_period, Entry const &init_val = Entry())
                : table(assoc, num_entries, idx_policy, rpl_policy, init_val),
                  _resetPeriod(_rst_period),
                  _resetCounter(0),
                  _subEntryNum(sub_entries)
            {
                assert(_subEntryNum % 2 == 0 && _subEntryNum < 512);
                _subEntryBits = ceil(log2(_subEntryNum));
            }
            void add(int vpn2, int vpn1, bool pf_hit_cdp)
            {
                _resetCounter++;
                Addr cat_addr = (vpn2 << 9) | vpn1;
                uint64_t sub_idx = cat_addr & ((1UL << _subEntryBits) - 1);
                assert(sub_idx < _subEntryNum);
                // mask lower bits
                cat_addr = cat_addr >> _subEntryBits;
                Entry *entry = table.findEntry(cat_addr, true);
                if (entry) {
                    table.accessEntry(entry);
                    if (entry->getExist(sub_idx)) {
                        entry->access(sub_idx, pf_hit_cdp);
                    } else {
                        entry->init(vpn1, vpn2, pf_hit_cdp);
                    }
                } else {
                    entry = table.findVictim(cat_addr);
                    entry->discard();
                    entry->init(vpn1, vpn2, pf_hit_cdp);
                    table.insertEntry(cat_addr, true, entry);
                }
            }
            void resetConfidence(float throttle_aggressiveness, bool enable_thro, bool low_conf)
            {
                if (_resetCounter < _resetPeriod)
                    return;
                auto it = table.begin();
                while (it != table.end()) {
                    if (it->isValid()) {
                        it->periodReset(throttle_aggressiveness, enable_thro, low_conf);
                    }
                    it++;
                }
                _resetCounter = 0;
                showHotVpns();
            }
            bool search(int vpn2, int vpn1) const
            {
                Addr cat_addr = (vpn2 << 9) | vpn1;
                uint64_t sub_idx = cat_addr & ((1UL << _subEntryBits) - 1);
                assert(sub_idx < _subEntryNum);
                cat_addr = cat_addr >> _subEntryBits;
                Entry *entry = table.findEntry(cat_addr, true);
                if (entry) {
                    if (entry->getExist(sub_idx)) {
                        return entry->getHot(sub_idx);
                    } else {
                        return false;
                    }
                } else {
                    return false;
                }
                return false;
            }
            void update(int vpn2, int vpn1, bool enable_thro, bool low_conf)
            {
                Addr cat_addr = (vpn2 << 9) | vpn1;
                uint64_t sub_idx = cat_addr & ((1UL << _subEntryBits) - 1);
                assert(sub_idx < _subEntryNum);
                cat_addr = cat_addr >> _subEntryBits;
                Entry *entry = table.findEntry(cat_addr, true);
                if (entry && enable_thro) {
                    if (entry->getExist(sub_idx)) {
                        entry->decr(sub_idx, low_conf);
                    }
                }
            }
            void showHotVpns()
            {
                if (GEM5_UNLIKELY(::gem5::debug::CDPHotVpns)) {
                    uint64_t valid_cnt = 0;
                    DPRINTFN("------------------------------------\n");
                    auto it = table.begin();
                    while (it != table.end()) {
                        if (it->isValid()) {
                            for (int i = 0; i < _subEntryNum; i++) {
                                if (it->getExist(i) && it->getPrefRefCnt(i) > 0) {
                                    valid_cnt++;
                                    DPRINTFN("Table entry(%#llx, %#llx): %#llx\n",\
                                    it->getDebugVpn2(i), it->getDebugVpn1(i), it->getPrefRefCnt(i));
                                }
                            }
                        }
                        it++;
                    }
                    DPRINTFN("%ld valid entries in hotVpns\n", valid_cnt);
                    DPRINTFN("------------------------------------\n");
                }
            }
            std::string name() { return std::string("VpnTable"); }
    };

    VpnTable<VpnEntry> vpnTable;

    class FilterTableEntry : public TaggedEntry
    {
        private:
            std::vector<SatCounter8> bit_map;
        public:
            FilterTableEntry(uint64_t size)
            {
                assert(size % 2 == 0);
                bit_map.assign(size, SatCounter8{2, 2});
            }

            void reset()
            {
                bit_map.assign(bit_map.size(), SatCounter8{2, 2});
            }

            uint64_t getInnerAddr(Addr addr) const
            {
                // Note:
                // addr is the `blockIndex(block address)`
                return addr % bit_map.size();
            }

            bool needFilter(Addr addr) const
            {
                uint64_t idx = getInnerAddr(addr);
                return bit_map[idx].isSaturated();
            }

            uint64_t getValue(Addr addr)
            {
                uint64_t idx = getInnerAddr(addr);
                return bit_map[idx].rawCounter();
            }

            void setFilter(Addr addr)
            {
                uint64_t idx = getInnerAddr(addr);
                bit_map[idx]++;
            }

            void unSetFilter(Addr addr)
            {
                uint64_t idx = getInnerAddr(addr);
                bit_map[idx]--;
            }
    };

    AssociativeSet<FilterTableEntry> filterTable;

    const uint64_t filterRegionBlks;
    const uint64_t filterEntryGranularityBits;

  public:
    StatGroup *prefetchStatsPtr = nullptr;
    RequestorID parentRid;
    std::pair<long, long> l3_miss_info;  // (cdp_l3_miss,l3_total_miss_num)
    float mpki = 1;
    float rivalCoverage = 1;
    // hyperparameters: Coverage & Accuracy Threshold
    const float Tcoverage = 0.16;
    const float Alow = 0.26;
    const float Ahigh = 0.5;
    const float RivalTcoverage = 0.5;

    void setStatsPtr(StatGroup *ptr) { prefetchStatsPtr = ptr; }
    void notifyIns(int ins_num) override
    {
        if (l3_miss_info.second != 0) {
            mpki = l3_miss_info.second * 1000.0 / ins_num;
        }
    }
    void recvRivalCoverage(CustomPfInfo& info)
    {
        rivalCoverage = info.coverage;
    }
    bool sendPFWithFilter(const PacketPtr &pkt, Addr addr, std::vector<AddrPriority> &addresses,
        int prio, PrefetchSourceType pfSource, int pf_depth);
    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses,
        int prio, PrefetchSourceType pfSource, int pf_depth);

    bool shouldIssueDegreeExtension(float accuracy) const;

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

    void addToVpnTable(Addr vaddr, bool pf_hit_cdp);

    void insertFilterTable(Addr addr, bool useful);
    bool needFilter(Addr addr);
    void recordUsedPrefetch(Addr addr);
    void recordUnusedPrefetch(Addr addr);

    Addr filterTableAddr(Addr addr)
    {
        return addr >> filterEntryGranularityBits;
    }

    float getCdpTrueAccuracy() const
    {
        float trueAccuracy = 1;
        if (prefetchStatsPtr->pfDequeued_srcs[PrefetchSourceType::CDP].value() > 100) {
            trueAccuracy = (prefetchStatsPtr->pfUseful_srcs[PrefetchSourceType::CDP].value() * 1.0) /
                            (prefetchStatsPtr->pfDequeued_srcs[PrefetchSourceType::CDP].value());
        }
        return trueAccuracy;
    }

    float getCdpTrueCoverage() const
    {
        float trueCoverage = 1;
        if (prefetchStatsPtr->pfUseful_srcs[PrefetchSourceType::CDP].value() > 100) {
            trueCoverage = (prefetchStatsPtr->pfUseful_srcs[PrefetchSourceType::CDP].value() * 1.0) /
                            (prefetchStatsPtr->pfUseful_srcs[PrefetchSourceType::CDP].value() +
                                prefetchStatsPtr->demandMshrMisses.value());
        }
        return trueCoverage;
    }

    bool isLowConfidence() const
    {
        static bool lowConf{false};
        float cdpAccuracy = getCdpTrueAccuracy();
        float cdpCoverage = getCdpTrueCoverage();
        int throAction{0};

        if (enableCoordinate) {
            if (cdpCoverage >= Tcoverage) {
                throAction = 0;
                lowConf = false;
            } else if (cdpAccuracy < Alow) {
                throAction = 1;
                lowConf = true;
            } else if (cdpAccuracy < Ahigh) {
                if (rivalCoverage >= RivalTcoverage) {
                    throAction = 2;
                    lowConf = true;
                } else {
                    throAction = 3;
                    lowConf = false;
                }
            } else if (cdpAccuracy >= Ahigh) {
                if (rivalCoverage < RivalTcoverage) {
                    throAction = 4;
                    lowConf = false;
                } else {
                    throAction = 5;
                }
            }
        }
        cdpStats.ThrottlingActionDist.sample(throAction);
        return lowConf;
    }

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
        mutable statistics::Distribution ThrottlingActionDist;
        statistics::Scalar triggeredInRxNotify;
        statistics::Scalar triggeredInCalcPf;
        statistics::Scalar dataNotifyCalled;
        statistics::Scalar dataNotifyExitBlockNotFound;
        statistics::Scalar dataNotifyExitFilter;
        statistics::Scalar dataNotifyExitDepth;
        statistics::Scalar dataNotifyNoAddrFound;
        statistics::Scalar dataNotifyNoVA;
        statistics::Scalar usefulInsertFilter;
        statistics::Scalar unusefulInsertFilter;
        statistics::Scalar actualFilted;
        statistics::Scalar dataNotifyNoData;
        statistics::Scalar missNotifyCalled;
        statistics::Scalar pfHitCDP;
        statistics::Scalar passedFilter;
        statistics::Scalar inserted;
    } cdpStats;
};

}  // namespace prefetch
}  // namespace gem5

#endif /* __MEM_CACHE_PREFETCH_IPCP_FIRST_LEVEL_HH__ */
