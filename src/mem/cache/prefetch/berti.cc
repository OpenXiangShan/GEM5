//
// Created by linjiawei on 22-10-31.
//

#include "mem/cache/prefetch/berti.hh"

#include "debug/BertiPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

BertiPrefetcher::BertiPrefetcher(const BertiPrefetcherParams &p)
    : Queued(p),
      historyTable(p.history_table_assoc, p.history_table_entries,
                   p.history_table_indexing_policy,
                   p.history_table_replacement_policy, HistoryTableEntry()),
      tableOfDeltas(p.table_of_deltas_entries, p.table_of_deltas_entries,
                    p.table_of_deltas_indexing_policy,
                    p.table_of_deltas_replacement_policy,
                    TableOfDeltasEntry()),
      fillLatency(this, "fillLatency")
{
    fillLatency.init(0);
}

void
BertiPrefetcher::updateHistoryTable(const PrefetchInfo &pfi)
{
    HistoryTableEntry *entry =
        historyTable.findEntry(pfi.getPC(), pfi.isSecure());
    HistoryInfo new_info = {blockIndex(pfi.getAddr()), curCycle()};
    if (entry) {
        DPRINTF(BertiPrefetcher,
                "History table hit, ip: [%lx] lineAddr: [%d]\n", pfi.getPC(),
                new_info.lineAddr);
        if (entry->history.size() == 16) {
            entry->history.erase(entry->history.begin());
        }
        entry->history.push_back(new_info);
    } else {
        //        DPRINTF(BertiPrefetcher, "History table miss, ip: [%lx]\n",
        //        pfi.getPC());
        entry = historyTable.findVictim(pfi.getPC());
        historyTable.invalidate(entry);
        entry->history.clear();
        entry->history.push_back(new_info);
        historyTable.insertEntry(pfi.getPC(), pfi.isSecure(), entry);
    }
}

void
BertiPrefetcher::updateTableOfDeltas(const PacketPtr &pkt,
                                     const std::vector<int64_t> &new_deltas)
{
    TableOfDeltasEntry *entry =
        tableOfDeltas.findEntry(pkt->req->getPC(), pkt->isSecure());
    if (!entry) {
        entry = tableOfDeltas.findVictim(pkt->req->getPC());
        tableOfDeltas.invalidate(entry);
        entry->resetConfidence(true);
        tableOfDeltas.insertEntry(pkt->req->getPC(), pkt->isSecure(), entry);
    }

    entry->counter++;

    /** Used to record deltas that missed in Table of Deltas. */
    std::vector<int64_t> missed_deltas = std::vector<int64_t>();
    for (auto &delta : new_deltas) {
        bool miss = true;
        for (auto &delta_info : entry->deltas) {
            if (delta_info.delta == delta) {
                delta_info.coverageCounter++;
                miss = false;
                break;
            }
        }
        if (miss) {
            missed_deltas.push_back(delta);
        }
    }

    /** Replace old deltas if necessary. */
    for (auto &delta : missed_deltas) {
        uint8_t replace_idx = 0;
        for (auto idx = 1; idx < entry->deltas.size(); idx++) {
            if (entry->deltas[idx].coverageCounter <
                entry->deltas[replace_idx].coverageCounter) {
                replace_idx = idx;
            }
        }
        entry->deltas[replace_idx].delta = delta;
        entry->deltas[replace_idx].coverageCounter = 1;
        entry->deltas[replace_idx].status = NO_PREF;
    }

    if (entry->counter == 16) {
        entry->updateStatus();
        /** Start a new learning phase. */
        entry->resetConfidence(false);
    }
}

void
BertiPrefetcher::calculatePrefetch(const PrefetchInfo &pfi,
                                   std::vector<AddrPriority> &addressed)
{

    bool pf_filter = pfi.hasPC() && !pfi.isWrite();

    /** We don't have enough information to train prefetcher, skip. */
    if (!pf_filter)
        return;

    /** 1.train: update history table */
    updateHistoryTable(pfi);

    /** 2.prefetch: search table of deltas, issue prefetch request */
    TableOfDeltasEntry *entry =
        tableOfDeltas.findEntry(pfi.getPC(), pfi.isSecure());
    if (entry) {
        tableOfDeltas.accessEntry(entry);
        for (auto &delta_info : entry->deltas) {
            if (delta_info.status == L1_PREF) {
                DPRINTF(BertiPrefetcher, "Using delta [%d] to prefetch\n",
                        delta_info.delta);
                int64_t delta = delta_info.delta;
                Addr pf_addr = (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
                addressed.push_back(AddrPriority(pf_addr, 0));
            }
        }
    }

    return;
}

void
BertiPrefetcher::notifyFill(const PacketPtr &pkt)
{
    if (!(pkt->req->hasPC() && !pkt->isWrite() && !pkt->req->isPrefetch() &&
          pkt->req->hasVaddr()))
        return;
    HistoryTableEntry *entry =
        historyTable.findEntry(pkt->req->getPC(), pkt->req->isSecure());
    if (!entry)
        return;

    //    DPRINTF(BertiPrefetcher, "Calculating latency for ip: [%d]\n",
    //    pkt->req->getPC());
    /** Search history table, find deltas. */
    Cycles latency = ticksToCycles(curTick() - pkt->req->time());
    Cycles demand_cycle = ticksToCycles(pkt->req->time());

    Cycles wrappedLatency;
    if (latency > 300){
        wrappedLatency = Cycles(300);
    } else if (latency % 10 == 0) {
        wrappedLatency = Cycles((latency / 10) * 10);
    } else {
        wrappedLatency = Cycles( ((latency / 10) + 1) * 10 );
    }
    fillLatency.sample(wrappedLatency);

    DPRINTF(BertiPrefetcher, "Updating table of deltas, latency [%d]\n",
            latency);

    std::vector<int64_t> timely_deltas = std::vector<int64_t>();
    for (auto it = entry->history.begin(); it != entry->history.end(); it++) {
        DPRINTF(BertiPrefetcher,
                "History Table: timestamp [%d] +latency: [%d]\n",
                it->timestamp, it->timestamp + latency);
        if (it->timestamp + latency > demand_cycle)
            break;
        int64_t delta = blockIndex(pkt->req->getVaddr()) - it->lineAddr;
        timely_deltas.push_back(delta);
        DPRINTF(BertiPrefetcher, "Timely delta found: [%d](%d - %d) ip: %lx\n",
                delta, blockIndex(pkt->req->getVaddr()), it->lineAddr,
                pkt->req->getPC());
    }
    /** Update table of deltas. */
    updateTableOfDeltas(pkt, timely_deltas);
}

}
}
