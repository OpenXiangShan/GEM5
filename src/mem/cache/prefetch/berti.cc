//
// Created by linjiawei on 22-10-31.
//

#include "mem/cache/prefetch/berti.hh"

#include "base/output.hh"
#include "debug/BertiPrefetcher.hh"
#include "mem/cache/base.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

BertiPrefetcher::BertiStats::BertiStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(trainOnHit, statistics::units::Count::get(), "Count of train on hit"),
      ADD_STAT(trainOnMiss, statistics::units::Count::get(), "Count of train on miss"),
      ADD_STAT(notifySkippedCond1, statistics::units::Count::get(), "Count of notify skipped on mixed condition"),
      ADD_STAT(notifySkippedIsPF, statistics::units::Count::get(), "Count of notify skipped isPF"),
      ADD_STAT(notifySkippedNoEntry, statistics::units::Count::get(), "Count of notify skipped no Berti entry"),
      ADD_STAT(entryEvicts, statistics::units::Count::get(), "Count of Berti entry evicted")
{
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
      aggressivePF(p.aggressive_pf),
      useByteAddr(p.use_byte_addr),
      triggerPht(p.trigger_pht),
      statsBerti(this),
      trainBlockFilter(8),
      dumpTopDeltas(p.dump_top_deltas)
{
    registerExitCallback([this]() {
        if (this->dumpTopDeltas) {
            std::vector<std::pair<int64_t, uint64_t>> top_delta_vec;
            for (const auto &entry: topDeltas) {
                top_delta_vec.push_back(entry);
            }
            std::sort(top_delta_vec.begin(), top_delta_vec.end(),
                      [](const std::pair<int64_t, uint64_t> &a, const std::pair<int64_t, uint64_t> &b) {
                          return a.second > b.second;
                      });

            auto out_handle = simout.create("topBertiDeltas.txt", false, true);
            *out_handle->stream() << "Delta" << " " << "count" << std::endl;
            unsigned count = 0;
            for (const auto &it: top_delta_vec) {
                *out_handle->stream() << it.first << " " << it.second << std::endl;
                count++;
                if (count >= 1000) {
                    break;
                }
            }

            simout.close(out_handle);

            out_handle = simout.create("topBertiEvictDeltas.txt", false, true);
            *out_handle->stream() << "EvcitedDelta" << " " << "count" << std::endl;
            count = 0;
            for (const auto &it: evictedDeltas) {
                *out_handle->stream() << it.first << " " << it.second << std::endl;
                count++;
                if (count >= 100) {
                    break;
                }
            }

            simout.close(out_handle);
        }
    });
}

BertiPrefetcher::HistoryTableEntry*
BertiPrefetcher::updateHistoryTable(const PrefetchInfo &pfi)
{
    Addr training_addr = useByteAddr ? pfi.getAddr() : blockIndex(pfi.getAddr());
    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pfi.getPC()), pfi.isSecure());
    HistoryInfo new_info = {
        .vAddr = training_addr,
        .timestamp = curCycle()
    };

    if (entry) {
        historyTable.accessEntry(entry);
        DPRINTF(BertiPrefetcher, "PC=%lx, history table hit, Addr=%lx\n", pfi.getPC(), training_addr);

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
            DPRINTF(BertiPrefetcher, "PC=%lx, addr %lx found in history table hit, ignore\n", pfi.getPC(),
                    training_addr);
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
                int64_t blk_delta =
                    (int64_t)blockIndex(pfi.getAddr() + entry->bestDelta.delta) - blockIndex(pfi.getAddr());
                if (!useByteAddr) {
                    evictedBestDelta = entry->bestDelta.delta;
                } else {
                    evictedBestDelta = blk_delta;
                }
                statsBerti.entryEvicts++;
                evictedDeltas[blk_delta] = evictedDeltas.count(blk_delta) ? evictedDeltas[blk_delta] + 1 : 1;
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
    const Cycles &search_latency,
    const Cycles &demand_cycle,
    const Addr &trigger_addr)
{
    DPRINTF(BertiPrefetcher, "latency: %lu, demand_cycle: %lu, history count: %lu\n", search_latency, demand_cycle,
            entry.history.size());
    std::list<int64_t> new_deltas;
    int delta_thres = useByteAddr ? blkSize : 8;
    for (auto it = entry.history.rbegin(); it != entry.history.rend(); it++) {
        int64_t delta = trigger_addr - it->vAddr;
        DPRINTF(BertiPrefetcher, "delta (%x - %x) = %ld\n", trigger_addr, it->vAddr, delta);

        // skip short deltas
        if (labs(delta) <= delta_thres) {
            continue;
        }

        // if not timely, skip and continue
        if (it->timestamp + search_latency >= demand_cycle) {
            DPRINTF(BertiPrefetcher, "skip untimely delta: %lu + %lu <= %u : %ld\n", it->timestamp, search_latency,
                    demand_cycle, delta);
            continue;
        }
        assert(delta != 0);
        new_deltas.push_back(delta);
        DPRINTF(BertiPrefetcher, "Timely delta found: %d=(%x - %x)\n", delta, trigger_addr, it->vAddr);
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
    // reset learned delta
    evictedBestDelta = 0;
    lastUsedBestDelta = 0;

    DPRINTF(BertiPrefetcher,
            "Train prefetcher, pc: %lx, addr: %lx miss: %d last lat: [%d]\n",
            pfi.getPC(), blockAddress(pfi.getAddr()),
            pfi.isCacheMiss(), hitSearchLatency);

    trainBlockFilter.insert(blockIndex(pfi.getAddr()), 0);

    if (!pfi.isCacheMiss()) {
        HistoryTableEntry *hist_entry = historyTable.findEntry(pcHash(pfi.getPC()), pfi.isSecure());
        if (hist_entry) {
            searchTimelyDeltas(*hist_entry, hitSearchLatency, curCycle(),
                               useByteAddr ? pfi.getAddr() : blockIndex(pfi.getAddr()));
            statsBerti.trainOnHit++;
        }
    }

    /** 1.train: update history table and compute learned delta*/
    auto entry = updateHistoryTable(pfi);

    /** 2.prefetch: search table of deltas, issue prefetch request */
    if (entry) {
        DPRINTF(BertiPrefetcher, "Delta table hit, pc: %lx\n", pfi.getPC());
        if (aggressivePF) {
            for (auto &delta_info : entry->deltas) {
                if (delta_info.status != NO_PREF) {
                    DPRINTF(BertiPrefetcher, "Using delta %d to prefetch\n", delta_info.delta);
                    int64_t delta = delta_info.delta;
                    Addr pf_addr =
                        useByteAddr ? pfi.getAddr() + delta : (blockIndex(pfi.getAddr()) + delta) << lBlkSize;
                    sendPFWithFilter(pfi, pf_addr, addresses, 32, PrefetchSourceType::Berti,
                                     delta == entry->bestDelta.delta && entry->bestDelta.coverageCounter >= 8);
                }
            }
        } else {
            if (entry->bestDelta.status != NO_PREF) {
                DPRINTF(BertiPrefetcher, "Using best delta %d to prefetch\n", entry->bestDelta.delta);
                Addr pf_addr = useByteAddr ? pfi.getAddr() + entry->bestDelta.delta
                                           : (blockIndex(pfi.getAddr()) + entry->bestDelta.delta) << lBlkSize;
                sendPFWithFilter(pfi, pf_addr, addresses, 32, PrefetchSourceType::Berti,
                                 entry->bestDelta.coverageCounter >= 8);
                if (triggerPht && entry->bestDelta.coverageCounter > 5) {
                    local_delta_pf_addr = pf_addr;
                }
            }
        }
    }
}

bool
BertiPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                                  PrefetchSourceType src, bool using_best_delta_and_confident)
{
    if (archDBer && cache->level() == 1) {
        archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
    }
    if (using_best_delta_and_confident) {
        lastUsedBestDelta = blockIndex(addr) - blockIndex(pfi.getAddr());
    }
    if (filter->contains(addr)) {
        DPRINTF(BertiPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        int64_t blk_delta = (int64_t)blockIndex(addr) - blockIndex(pfi.getAddr());
        topDeltas[blk_delta] = topDeltas.count(blk_delta) ? topDeltas[blk_delta] + 1 : 1;
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
        statsBerti.notifySkippedCond1++;
        return;
    }

    DPRINTF(BertiPrefetcher,
            "Cache Fill: %s isPF: %d, pc: %lx\n",
            pkt->print(), pkt->req->isPrefetch(), pkt->req->getPC());

    if (pkt->req->isPrefetch()) {
        statsBerti.notifySkippedIsPF++;
        return;
    }

    // fill latency
    Cycles miss_refill_search_lat = Cycles(0);
    hitSearchLatency = Cycles(0);

    HistoryTableEntry *entry =
        historyTable.findEntry(pcHash(pkt->req->getPC()), pkt->req->isSecure());
    if (!entry) {
        statsBerti.notifySkippedNoEntry++;
        return;
    }

    /** Search history table, find deltas. */
    Cycles demand_cycle = ticksToCycles(pkt->req->time());

    DPRINTF(BertiPrefetcher, "Search delta for PC %lx\n", pkt->req->getPC());
    searchTimelyDeltas(*entry, miss_refill_search_lat, demand_cycle,
                       useByteAddr ? pkt->req->getVaddr() : blockIndex(pkt->req->getVaddr()));
    statsBerti.trainOnMiss++;

    DPRINTF(BertiPrefetcher, "Updating table of deltas, latency [%d]\n",
            miss_refill_search_lat);
}

}
}
