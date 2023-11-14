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
      ADD_STAT(pf_delta, statistics::units::Count::get(), "")
{
    pf_delta.init(0);
}

BertiPrefetcher::BertiPrefetcher(const BertiPrefetcherParams &p)
    : Queued(p),
      maxAddrListSize(p.addrlist_size),
      maxDeltaListSize(p.deltalist_size),
      maxDeltafound(p.max_deltafound),
      historyTable(p.history_table_assoc, p.history_table_entries,
                   p.history_table_indexing_policy,
                   p.history_table_replacement_policy,
                   HistoryTableEntry(maxDeltaListSize)),
      aggressive_pf(p.aggressive_pf),
      statsBerti(this)
{
}

BertiPrefetcher::HistoryTableEntry*
BertiPrefetcher::updateHistoryTable(const PrefetchInfo &pfi)
{
    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pfi.getPC()), pfi.isSecure());
    HistoryInfo new_info = {
        .lineAddr = blockIndex(pfi.getAddr()),
        .timestamp = curCycle()
    };

    if (entry) {
        historyTable.accessEntry(entry);
        DPRINTF(BertiPrefetcher, "PC=%lx, history table hit, lineAddr=%lx\n", pfi.getPC(), new_info.lineAddr);

        bool found_addr_in_hist =
            std::find(entry->history.begin(), entry->history.end(), new_info) != entry->history.end();
        if (!found_addr_in_hist) {
            if (entry->history.size() >= maxAddrListSize) {
                entry->history.erase(entry->history.begin());
            }
            entry->history.push_back(new_info);
            entry->hysteresis = true;
            return entry;
        } else {
            DPRINTF(BertiPrefetcher, "PC=%lx, line index addr %lx found in history table hit, ignore\n", pfi.getPC(),
                    new_info.lineAddr);
            return nullptr;  // ignore redundant req
        }
    } else {
        DPRINTF(BertiPrefetcher, "PC=%lx, history table miss\n", pfi.getPC());
        entry = historyTable.findVictim(pcHash(pfi.getPC()));
        if (entry->hysteresis) {
            entry->hysteresis = false;
            historyTable.insertEntry(pcHash(entry->pc), entry->isSecure(), entry, false);
        } else {
            if (entry->bestDelta.status != NO_PREF) {
                evictedBestDelta = entry->bestDelta.delta;
            }
            // only when hysteresis is false
            entry->pc = pfi.getPC();
            entry->history.clear();
            entry->history.push_back(new_info);
            historyTable.insertEntry(pcHash(pfi.getPC()), pfi.isSecure(), entry);
        }
    }
    return nullptr;
}


void BertiPrefetcher::searchTimelyDeltas(
    HistoryTableEntry &entry,
    const Cycles &latency,
    const Cycles &demand_cycle,
    const Addr &blk_addr)
{
    DPRINTF(BertiPrefetcher, "latency: %lu, demand_cycle: %lu, history count: %lu\n", latency, demand_cycle,
            entry.history.size());
    std::list<int64_t> new_deltas;
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        int64_t delta = blk_addr - it->lineAddr;
        DPRINTF(BertiPrefetcher, "delta (%x - %x) = %ld\n", blk_addr, it->lineAddr, delta);
        if (labs(delta) <= (1<<3)) {
            continue;
        }
        // if not timely, skip and continue
        if (it->timestamp + latency >= demand_cycle) {
            DPRINTF(BertiPrefetcher, "skip untimely delta: %lu + %lu <= %u : %ld\n", it->timestamp, latency,
                    demand_cycle, delta);
            continue;
        }
        assert(delta != 0);
        new_deltas.push_back(delta);
        DPRINTF(BertiPrefetcher, "Timely delta found: %d=(%x - %x)\n",
                delta, blk_addr, it->lineAddr);
        if (new_deltas.size() >= maxDeltafound) {
            break;
        }
    }

    entry.counter++;

    for (auto &delta : new_deltas) {
        bool miss = true;
        for (auto &delta_info : entry.deltas) {
            if (delta_info.coverageCounter != 0 && delta_info.delta == delta) {
                delta_info.coverageCounter++;
                DPRINTF(BertiPrefetcher, "Inc coverage for delta %d, cov = %d\n", delta, delta_info.coverageCounter);
                miss = false;
                break;
            }
        }
        // miss
        if (miss) {
            // find the smallest coverage and replace
            int replace_idx = 0;
            for (auto i = 0; i < entry.deltas.size(); i++) {
                if (entry.deltas[replace_idx].coverageCounter >= entry.deltas[i].coverageCounter) {
                    replace_idx = i;
                }
            }
            entry.deltas[replace_idx].delta = delta;
            entry.deltas[replace_idx].coverageCounter = 1;
            entry.deltas[replace_idx].status = NO_PREF;
            DPRINTF(BertiPrefetcher, "Add new delta: %d with cov = 1\n", delta);
        }
    }

    if (entry.counter >= 6) {
        entry.updateStatus();
        if (entry.counter >= 16) {
            entry.resetConfidence(false);
        }
    }
    printDeltaTableEntry(entry);
}

void
BertiPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                                   PrefetchSourceType pf_source, bool miss_repeat, Addr &local_delta_pf_addr)
{
    evictedBestDelta = 0;
    tempBestDelta = 0;

    DPRINTF(BertiPrefetcher,
            "Train prefetcher, pc: %lx, addr: %lx miss: %d last lat: [%d]\n",
            pfi.getPC(), blockAddress(pfi.getAddr()),
            pfi.isCacheMiss(), lastFillLatency);

    // if (!pfi.isCacheMiss()) {
    //     HistoryTableEntry *hist_entry = historyTable.findEntry(
    //         pcHash(pfi.getPC()), pfi.isSecure());
    //     if (hist_entry) {
    //         searchTimelyDeltas(*hist_entry, lastFillLatency,
    //                            curCycle(),
    //                            blockIndex(pfi.getAddr()));
    //     }
    // }

    /** 1.train: update history table */
    auto entry = updateHistoryTable(pfi);
    /** 2.prefetch: search table of deltas, issue prefetch request */
    if (entry) {
        DPRINTF(BertiPrefetcher, "Delta table hit, pc: %lx\n", pfi.getPC());
        if (aggressive_pf) {
            for (auto &delta_info : entry->deltas) {
                if (delta_info.status != NO_PREF) {
                    DPRINTF(BertiPrefetcher, "Using delta %d to prefetch\n",
                            delta_info.delta);
                    int64_t delta = delta_info.delta;
                    statsBerti.pf_delta.sample(delta);
                    Addr pf_addr =
                        (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
                    sendPFWithFilter(pf_addr, addresses, 32, PrefetchSourceType::Berti);
                }
            }
        } else {
            if (entry->bestDelta.status != NO_PREF) {
                DPRINTF(BertiPrefetcher, "Using best delta %d to prefetch\n",
                        entry->bestDelta.delta);
                statsBerti.pf_delta.sample(entry->bestDelta.delta);
                Addr pf_addr = (blockIndex(pfi.getAddr()) + entry->bestDelta.delta) << lBlkSize;
                sendPFWithFilter(pf_addr, addresses, 32, PrefetchSourceType::Berti);
                if (entry->bestDelta.coverageCounter > 6) {
                    local_delta_pf_addr = pf_addr;
                } else {
                    local_delta_pf_addr = 0;
                }
            }
        }

        if (entry->bestDelta.coverageCounter >= 8) {
            tempBestDelta = entry->bestDelta.delta;
        }
    }
}

bool
BertiPrefetcher::sendPFWithFilter(Addr addr, std::vector<AddrPriority> &addresses, int prio,
                                        PrefetchSourceType src)
{
    if (filter->contains(addr)) {
        DPRINTF(BertiPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(BertiPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}


void
BertiPrefetcher::notifyFill(const PacketPtr &pkt)
{
    if (pkt->req->isInstFetch() ||
        !pkt->req->hasVaddr() || !pkt->req->hasPC()) {
        DPRINTF(BertiPrefetcher, "Skip packet: %s\n", pkt->print());
        return;
    }

    DPRINTF(BertiPrefetcher,
            "Cache Fill: %s isPF: %d, pc: %lx\n",
            pkt->print(), pkt->req->isPrefetch(), pkt->req->getPC());

    if (pkt->req->isPrefetch()) {
        return;
    }

    // fill latency
    // Cycles latency = ticksToCycles(curTick() - pkt->req->time());
    Cycles latency = Cycles(1);
    lastFillLatency = latency;

    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pkt->req->getPC()), pkt->req->isSecure());
    if (!entry)
        return;

    /** Search history table, find deltas. */
    Cycles demand_cycle = ticksToCycles(pkt->req->time());

    DPRINTF(BertiPrefetcher, "Search delta for PC %lx\n", pkt->req->getPC());
    searchTimelyDeltas(*entry, latency, demand_cycle,
                       blockIndex(pkt->req->getVaddr()));

    DPRINTF(BertiPrefetcher, "Updating table of deltas, latency [%d]\n",
            latency);
}

}
}
