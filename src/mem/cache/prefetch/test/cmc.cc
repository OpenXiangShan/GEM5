#include "mem/cache/prefetch/test/cmc.hh"

#include "mem/cache/prefetch/test/common/simple_associative_set.hh"

namespace gem5
{
namespace prefetch
{
namespace test
{

CMCPrefetcher::CMCPrefetcher(const CMCPrefetcherParams &p)
:   recorder(new Recorder(p.nr_entry)),
    storage(p.storage_entries / p.storage_assoc, p.storage_assoc),
    cacheLevel(p.cache_level),
    degree(p.degree),
    trigger(STACK_SIZE),
    filter(p.filter_size),
    blockSize(p.block_size)
{
}

void
CMCPrefetcher::doPrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool is_first_shot)
{
    bool can_prefetch = cacheLevel == 1 ? (!pfi.isWrite() && pfi.hasPC()) : true;
    if (!can_prefetch) {
        return;
    }
    Addr pc = pfi.hasPC() ? pfi.getPC() : 0;

    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
    bool is_secure = pfi.isSecure();
    auto prefetchSource = pf_source;

    // Miss Statistics
    if (false) {
        static uint64_t missCnt = 0;
        static std::unordered_map<uint64_t, uint64_t> counts;
        Addr pc = pfi.getPC();
        // printf("=== PC: %lx\n", pc);
        // if (pc == 0x10b84) {
        //     printf("0x%lx\n", pfi.getAddr());
        // }
        // DPRINTF(CMCPrefetcher, "0x%lx\n", block_addr);
        counts[pc]++;
        missCnt++;
        if (missCnt > 300000) {
            for (const auto& pair : counts) {
                printf("pc: %lx, count: %ld\n", pair.first, pair.second);
            }
            assert(false);
        }
    }

    printf_wrapper("CMC train: pc: %lx, addr: %lx\n", pc, block_addr);

    // not covered by other prefetcher
    bool nocovered = (pfi.isCacheMiss() && (!late)) ||
            (pf_source == PrefetchSourceType::CMC); // if cmc send pf to l2/3, this code line doesn't actually work

    // Prefetch: check if there is a match
    CMCStorageEntry *match_entry = storage.findEntry(hash(block_addr >> blockSize, pc));
    if (nocovered && match_entry) {
        storage.accessEntry(match_entry);
        // prefetch on cache miss only
        printf_wrapper("Storage hit, trigger pc: %lx, addr: %lx\n",
                pc, block_addr);
        // printf("=== Storage hit, trigger addr: %lx\n", block_addr);
        match_entry->refcnt++;
        int priority = recorder->nr_entry;
        uint32_t id = match_entry->id;

        int num_send = 0;
        for (auto addr: match_entry->addresses) {
            // addresses.push_back(AddrPriority(addr, mixedNum, PrefetchSourceType::CMC));
            if (sendPFWithFilter(pfi, addr, addresses, priority, PrefetchSourceType::CMC)) {
                num_send++;
                if (num_send > 24) {
                    addresses.back().pfahead = true;
                    addresses.back().pfahead_host = 3;
                } else if (num_send > 4) {
                    addresses.back().pfahead = true;
                    addresses.back().pfahead_host = 2;
                }
            }
            // if (enableDB) {
            //     prefetchTraceManager->write_record(
            //         PrefetchTrace(addr, id, priority)
            //     );
            // }
            priority--;
        }
    }
    else if (match_entry) {
        // if storage entry can be covered by other prefetcher, shall we need to remove this entry?
        storage.invalidate(match_entry);
        printf_wrapper("Storage hit, but unused, trigger addr: %lx\n",
                block_addr);
    }

    // Train: update temporal access chain
    bool finished = false;

    /* 1. Train trigger */
    bool sms_hit = !pfi.isCacheMiss() &&
                    (prefetchSource == PrefetchSourceType::SStream || prefetchSource == PrefetchSourceType::SPht);
    bool train_trigger =
        (trigger.size() < 1 || match_entry) && !trigger.full();
    bool do_training =
        !train_trigger && !trigger.empty() && nocovered;
    if (train_trigger) {
        printf_wrapper("train_trigger index: %d, addr: %lx\n",
                trigger.size()-1, block_addr);
        assert(!trigger.full());

        trigger.push_back(RecordEntry(pc, block_addr, is_secure));
    }

    /* 2. Train entry */
    if (do_training) {
        bool trained = recorder->train_entry(block_addr, is_secure, &finished);
        auto &trigger_head = trigger.front();
        if (trained) {
            printf_wrapper("trained %x\n", block_addr);
        }
        if (finished) {
            printf_wrapper("trigger train finished, pc: %lx, addr: %lx\n",
                    trigger_head.pc, trigger_head.addr);

            CMCStorageEntry *entry = storage.findEntry(hash(trigger_head.addr >> blockSize, trigger_head.pc));
            if (entry) {
                // storage.accessEntry(entry); do not update replacement
                printf_wrapper("CMC: enter the same trigger, pc: %lx, addr: %lx\n",
                                    trigger_head.pc, trigger_head.addr);
                entry->addresses = recorder->entries;

                entry->refcnt++;
                entry->id = acc_id;
            } else {
                entry = storage.findVictim(hash(trigger_head.addr >> blockSize, trigger_head.pc));
                entry->addresses = recorder->entries;

                entry->refcnt = 0;
                entry->id = acc_id;

                storage.insertEntry(
                    hash(trigger_head.addr >> blockSize, trigger_head.pc),
                    entry
                );
            }

            for (auto addr: recorder->entries) {
                printf_wrapper("entry addr: 0x%lx\n",
                        addr);
            }
            trigger.pop_front();

            recorder->reset();
            acc_id++;

            // if (enableDB) {
            //     triggerTraceManager->write_record(
            //         TriggerTrace(trigger_head.pc, trigger_head.addr)
            //     );
            //     entryTraceManager->write_record(
            //         EntryTrace(
            //             trigger_head.pc,
            //             trigger_head.addr,
            //             acc_id,
            //             &recorder->entries
            //         )
            //     );
            // }
        }
    }
}

void
CMCPrefetcher::Recorder::reset() {
    index = 0;
    entries.clear();
}

bool
CMCPrefetcher::Recorder::train_entry(
    Addr addr,
    bool is_secure,
    bool *finished
) {
    if (index == 0) {
        // first entry
        assert(entry_empty());
        entries.push_back(addr);
        index++;
        return true;
    }

    assert(!entry_empty());
    // enqueue entry
    if (index >= nr_entry) {
        // entry full
        entries.push_back(addr);
        index++;
        *finished = true;
    } else {
        entries.push_back(addr);
        index++;
    }
    return true;
}

bool
CMCPrefetcher::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                                PrefetchSourceType src)
{
    if (filter.contains(addr)) {
        printf_wrapper("Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        printf_wrapper("CMC: send pf: %lx\n", addr);
        filter.insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
    return false;
}

void
CMCPrefetcher::CMCStorageEntry::invalidate() {
    if (false) {
        if (this->isValid()) {
            printf("entry victim: refcnt = %d\n", this->refcnt);
        }
    }
    TestEntry::invalidate();
}

} // namespace test
} // namespace prefetch
} // namespace gem5
