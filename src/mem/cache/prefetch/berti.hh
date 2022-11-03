//
// Created by linjiawei on 22-10-31.
//

#ifndef __MEM_CACHE_PREFETCH_BERTI_HH__
#define __MEM_CACHE_PREFETCH_BERTI_HH__

#include <vector>

#include "base/types.hh"
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
  protected:
    struct HistoryInfo
    {
        Addr lineAddr;
        Cycles timestamp;
    };

    class HistoryTableEntry : public TaggedEntry
    {
      public:
        /** FIFO of demand miss history. */
        std::vector<HistoryInfo> history;

        HistoryTableEntry() { history = std::vector<HistoryInfo>(); }
    };

    AssociativeSet<HistoryTableEntry> historyTable;

    enum DeltaStatus { L1_PREF, L2_PREF, NO_PREF };

    struct DeltaInfo
    {
        uint8_t coverageCounter;
        int64_t delta;
        DeltaStatus status;
    };

    class TableOfDeltasEntry : public TaggedEntry
    {
      public:
        std::vector<DeltaInfo> deltas;
        uint8_t counter;

        void resetConfidence(bool reset_status)
        {
            counter = 0;
            for (auto &info : deltas) {
                info.coverageCounter = 0;
                if (reset_status) {
                    info.status = NO_PREF;
                }
            }
        }

        void updateStatus()
        {
            assert(counter == 16);
            for (auto &info : deltas) {
                // TODO: L2 Prefetch?
                info.status = info.coverageCounter >= 10 ? L1_PREF : NO_PREF;
            }
        }

        TableOfDeltasEntry()
        {
            deltas = std::vector<DeltaInfo>(16);
            resetConfidence(true);
        }
    };

    AssociativeSet<TableOfDeltasEntry> tableOfDeltas;

    /** Update history table on demand miss. */
    void updateHistoryTable(const PrefetchInfo &pfi);

    /** Update table of deltas on cache fill. */
    void updateTableOfDeltas(const PacketPtr &pkt,
                             const std::vector<int64_t> &new_deltas);

    /** Search for timely deltas. */
    void notifyFill(const PacketPtr &pkt) override;

  public:
    BertiPrefetcher(const BertiPrefetcherParams &p);

    void calculatePrefetch(const PrefetchInfo &pfi,
                           std::vector<AddrPriority> &addressed) override;
};

}

}


#endif
