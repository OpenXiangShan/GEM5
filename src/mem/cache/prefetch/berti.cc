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

BertiPrefetcher::BertiStats::BertiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(num_train_hit, statistics::units::Count::get(), ""),
      ADD_STAT(num_train_miss, statistics::units::Count::get(), ""),
      ADD_STAT(train_pc, statistics::units::Count::get(), ""),
      ADD_STAT(num_fill_prefetch, statistics::units::Count::get(), ""),
      ADD_STAT(num_fill_miss, statistics::units::Count::get(), ""),
      ADD_STAT(fill_pc, statistics::units::Count::get(), ""),
      ADD_STAT(fill_latency, statistics::units::Count::get(), ""),
      ADD_STAT(pf_delta, statistics::units::Count::get(), "")
{
    train_pc.init(0);
    fill_pc.init(0);
    fill_latency.init(0);
    pf_delta.init(0);
}

BertiPrefetcher::BertiPrefetcher(const BertiPrefetcherParams &p)
    : Queued(p),
      historyTable(p.history_table_assoc, p.history_table_entries,
                   p.history_table_indexing_policy,
                   p.history_table_replacement_policy, HistoryTableEntry()),
      tableOfDeltas(p.table_of_deltas_entries, p.table_of_deltas_entries,
                    p.table_of_deltas_indexing_policy,
                    p.table_of_deltas_replacement_policy,
                    TableOfDeltasEntry()),
      aggressive_pf(p.aggressive_pf),
      statsBerti(this)
{
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
        DPRINTF(BertiPrefetcher, "History table miss, ip: [%lx]\n",
                pfi.getPC());
        entry = historyTable.findVictim(pfi.getPC());
        historyTable.invalidate(entry);
        entry->history.clear();
        entry->history.push_back(new_info);
        historyTable.insertEntry(pfi.getPC(), pfi.isSecure(), entry);
    }
}

void
BertiPrefetcher::updateTableOfDeltas(
    const Addr pc, const bool isSecure,
    const std::vector<int64_t> &new_deltas)
{
    if (new_deltas.empty())
        return;

    TableOfDeltasEntry *entry =
        tableOfDeltas.findEntry(pc, isSecure);
    if (!entry) {
        entry = tableOfDeltas.findVictim(pc);
        tableOfDeltas.invalidate(entry);
        entry->resetConfidence(true);
        tableOfDeltas.insertEntry(pc, isSecure, entry);
    }

    entry->counter++;
    for (auto &delta : new_deltas) {
        bool miss = true;
        for (auto &delta_info : entry->deltas) {
            if (delta_info.coverageCounter != 0 && delta_info.delta == delta) {
                delta_info.coverageCounter++;
                miss = false;
                break;
            }
        }
        // miss
        if (miss) {
            int replace_idx = 0;
            for (auto i = 1; i < entry->deltas.size(); i++) {
                if (entry->deltas[replace_idx].coverageCounter
                    >= entry->deltas[i].coverageCounter) {
                    replace_idx = i;
                }
            }
            entry->deltas[replace_idx].delta = delta;
            entry->deltas[replace_idx].coverageCounter = 1;
            entry->deltas[replace_idx].status = NO_PREF;
        }
    }

    if (entry->counter >= 8) {
        entry->updateStatus();
        if (entry->counter == 16) {
            /** Start a new learning phase. */
            entry->resetConfidence(false);
        }
    }
    printDeltaTableEntry(*entry);
}

void
BertiPrefetcher::calculatePrefetch(
    const PrefetchInfo &pfi,
    std::vector<AddrPriority> &addressed)
{
    DPRINTF(BertiPrefetcher,
            "Train prefetcher, ip: [%lx] "
            "lineAddr: [%d] miss: %d last lat: [%d]\n",
            pfi.getPC(), blockIndex(pfi.getAddr()),
            pfi.isCacheMiss(), lastFillLatency);

    if (pfi.isCacheMiss()) {
        statsBerti.num_train_miss++;
    } else {
        statsBerti.num_train_hit++;
        HistoryTableEntry *hist_entry = historyTable.findEntry(
            pfi.getPC(), pfi.isSecure());
        if (hist_entry) {
           std::vector<int64_t> deltas;
            searchTimelyDeltas(*hist_entry, lastFillLatency,
                               curCycle(),
                               blockIndex(pfi.getAddr()), deltas);
            updateTableOfDeltas(pfi.getPC(), pfi.isSecure(), deltas);
        }
    }
    statsBerti.train_pc.sample(pfi.getPC());

    /** 1.train: update history table */
    updateHistoryTable(pfi);

    /** 2.prefetch: search table of deltas, issue prefetch request */
    TableOfDeltasEntry *entry =
        tableOfDeltas.findEntry(pfi.getPC(), pfi.isSecure());
    if (entry) {
        DPRINTF(BertiPrefetcher, "Delta table hit, ip: [%lx]\n", pfi.getPC());
        tableOfDeltas.accessEntry(entry);
        if (aggressive_pf) {
            for (auto &delta_info : entry->deltas) {
                if (delta_info.status == L2_PREF) {
                    DPRINTF(BertiPrefetcher, "Using delta [%d] to prefetch\n",
                            delta_info.delta);
                    int64_t delta = delta_info.delta;
                    statsBerti.pf_delta.sample(delta);
                    Addr pf_addr =
                        (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
                    addressed.push_back(AddrPriority(pf_addr, 0));
                }
            }
        } else {
            if (entry->best_delta != 0) {
                DPRINTF(BertiPrefetcher, "Using delta [%d] to prefetch\n",
                        entry->best_delta);
                statsBerti.pf_delta.sample(entry->best_delta);
                Addr pf_addr = (blockIndex(pfi.getAddr()) +
                                entry->best_delta) << lBlkSize;
                addressed.push_back(AddrPriority(pf_addr, 0));
            }
        }
    }

    return;
}

void BertiPrefetcher::searchTimelyDeltas(
    const HistoryTableEntry &entry,
    const Cycles &latency,
    const Cycles &demand_cycle,
    const Addr &blk_addr,
    std::vector<int64_t> &deltas)
{
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        // if not timely, skip and continue
        if (it->timestamp + latency > demand_cycle)
            continue;
        int64_t delta = blk_addr - it->lineAddr;
        if (delta != 0) {
            deltas.push_back(delta);
            DPRINTF(BertiPrefetcher, "Timely delta found: [%d](%d - %d)\n",
                    delta, blk_addr, it->lineAddr);
            // We don't want to many deltas
            if (deltas.size() == 8)
                break;
        }
    }
}

void
BertiPrefetcher::notifyFill(const PacketPtr &pkt)
{
    DPRINTF(BertiPrefetcher,
            "Cache Fill: %s isPF: %d\n",
            pkt->print(), pkt->req->isPrefetch());

    if (pkt->req->isPrefetch()) {
        statsBerti.num_fill_prefetch++;
        return;
    } else {
        statsBerti.num_fill_miss++;
    }

    Cycles latency = ticksToCycles(curTick() - pkt->req->time());
    // update lastFillLatency for prefetch on hit
    lastFillLatency = latency;

    assert(pkt->req->hasPC() && pkt->req->hasVaddr());

    statsBerti.fill_pc.sample(pkt->req->getPC());

    HistoryTableEntry *entry =
        historyTable.findEntry(pkt->req->getPC(), pkt->req->isSecure());
    if (!entry)
        return;

    /** Search history table, find deltas. */
    Cycles demand_cycle = ticksToCycles(pkt->req->time());
    Cycles wrappedLatency;
    if (latency > 500){
        wrappedLatency = Cycles(500);
    } else if (latency % 10 == 0) {
        wrappedLatency = Cycles((latency / 10) * 10);
    } else {
        wrappedLatency = Cycles( ((latency / 10) + 1) * 10 );
    }
    statsBerti.fill_latency.sample(wrappedLatency);

    DPRINTF(BertiPrefetcher, "Updating table of deltas, latency [%d]\n",
            latency);

    std::vector<int64_t> timely_deltas = std::vector<int64_t>();
    searchTimelyDeltas(*entry, latency, demand_cycle,
                       blockIndex(pkt->req->getVaddr()),
                       timely_deltas);

    /** Update table of deltas. */
    updateTableOfDeltas(pkt->req->getPC(), pkt->req->isSecure(),
                        timely_deltas);
}

}
}
