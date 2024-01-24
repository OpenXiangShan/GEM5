//
// Created by linjiawei on 22-10-31.
//

#ifndef __MEM_CACHE_PREFETCH_BERTI_HH__
#define __MEM_CACHE_PREFETCH_BERTI_HH__

#include <unordered_map>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/statistics.hh"
#include "base/types.hh"
#include "debug/BertiPrefetcher.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"
#include "params/BertiPrefetcher.hh"

namespace gem5
{

struct BertiPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{

class BertiPrefetcher : public Queued
{

    int maxAddrListSize;
    int maxDeltaListSize;
    int maxDeltafound;

  protected:
    struct HistoryInfo
    {
        Addr vAddr;
        Cycles timestamp;

        bool operator == (const HistoryInfo &rhs) const
        {
            return vAddr == rhs.vAddr;
        }
    };

    enum DeltaStatus { L1_PREF, L2_PREF, NO_PREF };
    struct DeltaInfo
    {
        uint8_t coverageCounter = 0;
        int delta = 0;
        DeltaStatus status = NO_PREF;
    };

    class TableOfDeltasEntry
    {
      public:
        std::vector<DeltaInfo> deltas;
        uint8_t counter = 0;
        DeltaInfo bestDelta;

        void resetConfidence(bool reset_status)
        {
            counter = 0;
            for (auto &info : deltas) {
                info.coverageCounter = 0;
                if (reset_status) {
                    info.status = NO_PREF;
                }
            }
            if (reset_status) {
                bestDelta.delta = 0;
                bestDelta.status = NO_PREF;
            }
        }

        void updateStatus()
        {
            uint8_t max_cov = 0;
            for (auto &info : deltas) {
                info.status = (info.coverageCounter >= 2) ? L2_PREF : NO_PREF;
                info.status = (info.coverageCounter >= 4) ? L1_PREF : info.status;
                if (info.status != NO_PREF && info.coverageCounter > max_cov) {
                    max_cov = info.coverageCounter;
                    bestDelta = info;
                }
            }
            if (max_cov == 0) {
                bestDelta.delta = 0;
                bestDelta.status = NO_PREF;
            }
        }

        TableOfDeltasEntry(int size) {
            deltas.resize(size);
        }
    };

    class HistoryTableEntry : public TableOfDeltasEntry, public TaggedEntry
    {
      public:
        bool hysteresis = false;
        Addr pc;
        /** FIFO of demand miss history. */
        std::list<HistoryInfo> history;

        HistoryTableEntry(int deltaTableSize) : TableOfDeltasEntry(deltaTableSize) {}
    };

    AssociativeSet<HistoryTableEntry> historyTable;


    Cycles hitSearchLatency;
    const bool aggressivePF;
    const bool useByteAddr;
    const bool triggerPht;

    struct BertiStats : public statistics::Group
    {
        BertiStats(statistics::Group *parent);

        statistics::Scalar trainOnHit;
        statistics::Scalar trainOnMiss;
        statistics::Scalar notifySkippedCond1;
        statistics::Scalar notifySkippedIsPF;
        statistics::Scalar notifySkippedNoEntry;
        statistics::Scalar entryEvicts;
    } statsBerti;


    /** Update history table on demand miss. */
    HistoryTableEntry* updateHistoryTable(const PrefetchInfo &pfi);

    /** Search for timely deltas. */
    void searchTimelyDeltas(HistoryTableEntry &entry,
                            const Cycles &latency,
                            const Cycles &demand_cycle,
                            const Addr &blk_addr);


    void printDeltaTableEntry(const TableOfDeltasEntry &entry) {
        DPRINTF(BertiPrefetcher, "Entry Counter: %d\n", entry.counter);
        for (auto &info : entry.deltas) {
            DPRINTF(BertiPrefetcher,
                    "=>[delta: %d coverage: %d status: %d]\n",
                    info.delta, info.coverageCounter, info.status);
        }
    }

    Addr pcHash(Addr pc) {
        return (pc>>1);
    }

    int lastUsedBestDelta;
    int evictedBestDelta;

    boost::compute::detail::lru_cache<Addr, Addr> trainBlockFilter;

    std::unordered_map<int64_t, uint64_t> topDeltas;

    std::unordered_map<int64_t, uint64_t> evictedDeltas;

    const bool dumpTopDeltas;

  public:

    boost::compute::detail::lru_cache<Addr, Addr> *filter;

    BertiPrefetcher(const BertiPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };

    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat) override
    {
        panic("not implemented");
    };
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat, Addr &local_delta_pf_addr);

    int getEvictBestDelta() { return evictedBestDelta; }

    int getBestDelta() { return lastUsedBestDelta; }

    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src, bool using_best_delta_and_confident);

    void notifyFill(const PacketPtr &pkt) override;

    bool shouldTrain(bool is_miss, const PrefetchInfo &pfi) {
        if (is_miss) {
            // Currently, XSCompositePrefetcher lets multiple accesses to the same block be seen by prefetchers.
            // Maybe, we should filter them out
            // return !trainBlockFilter.contains(blockIndex(pfi.getAddr()));
            return true;
        } else {
            // This is to let multiple accessses into the same block be seen by different PC entryeis
            // return !trainBlockFilter.contains(blockIndex(pfi.getAddr())) &&
            //        historyTable.findEntry(pcHash(pfi.getPC()), pfi.isSecure()) != nullptr;
            return historyTable.findEntry(pcHash(pfi.getPC()), pfi.isSecure()) != nullptr;
        }
    }

};

}

}


#endif
