#include "mem/cache/prefetch/cmc.hh"

#include <memory>
#include <unordered_map>

#include "base/output.hh"
#include "debug/CMCPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

CMCPrefetcher::CMCStats::CMCStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(storageHits, statistics::units::Count::get(),
               "CMC storage lookup hits used for prefetching"),
      ADD_STAT(storageMisses, statistics::units::Count::get(),
               "CMC storage lookup misses on uncovered accesses"),
      ADD_STAT(storageUnusedHits, statistics::units::Count::get(),
               "CMC storage hits invalidated because another prefetcher covered the access"),
      ADD_STAT(triggersCreated, statistics::units::Count::get(),
               "CMC temporal triggers created"),
      ADD_STAT(triggerStackFull, statistics::units::Count::get(),
               "CMC trigger creation attempts blocked by a full trigger stack"),
      ADD_STAT(trainingSamples, statistics::units::Count::get(),
               "CMC temporal samples recorded"),
      ADD_STAT(trainingContextMismatches, statistics::units::Count::get(),
               "CMC samples skipped while another context owns the recorder"),
      ADD_STAT(trainingCompletions, statistics::units::Count::get(),
               "CMC temporal recordings completed"),
      ADD_STAT(storageInserts, statistics::units::Count::get(),
               "CMC temporal sequences inserted into storage"),
      ADD_STAT(storageUpdates, statistics::units::Count::get(),
               "CMC temporal sequences updated in storage"),
      ADD_STAT(dataQueueEnqueues, statistics::units::Count::get(),
               "CMC temporal sequences enqueued for buffered sending"),
      ADD_STAT(dataQueueDrops, statistics::units::Count::get(),
               "CMC queued temporal sequences dropped when the buffer is full"),
      ADD_STAT(queuedCandidatesSent, statistics::units::Count::get(),
               "CMC buffered candidates admitted for sending")
{
}

CMCPrefetcher::CMCPrefetcher(const CMCPrefetcherParams &p)
: Queued(p),
    recorder(p.degree),
    storage(p.storage_entries, p.storage_entries, p.storage_indexing_policy,
            p.storage_replacement_policy, StorageEntry()),
    statsCMC(this),
    enableDB(p.enablePrefetchDB),
    trigger(STACK_SIZE)
{
    if (enableDB) {
        db.init_db();
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("triggerPC", UINT64),
            std::make_pair("triggerAddr", UINT64),
        };
        triggerTraceManager = db.addAndGetTrace("TRIGGERTRACE", fields_vec);
        triggerTraceManager->init_table();

        fields_vec = {
            std::make_pair("trainPC", UINT64),
            std::make_pair("trainVAddr", UINT64),
            std::make_pair("isMiss", UINT64),
            std::make_pair("prefetchSource", UINT64),
        };
        trainTraceManager = db.addAndGetTrace("TRAINTRACE", fields_vec);
        trainTraceManager->init_table();

        fields_vec = {
            std::make_pair("triggerPC", UINT64),
            std::make_pair("triggerAddr", UINT64),
            std::make_pair("entryID", UINT64),
        };
        for (int i = 0; i <= 34; i++) {
            auto sIndex =
                std::string(2-std::to_string(i).length(), '0') +
                std::to_string(i);
            fields_vec.push_back(
                std::make_pair("entryAddr_" + sIndex, UINT64)
            );
        }
        entryTraceManager = db.addAndGetTrace("ENTRYTRACE", fields_vec);
        entryTraceManager->init_table();

        fields_vec = {
            std::make_pair("pfVaddr", UINT64),
            std::make_pair("pfID", UINT64),
            std::make_pair("pfPriority", UINT64),
        };
        prefetchTraceManager = db.addAndGetTrace("PREFETCHTRACE", fields_vec);
        prefetchTraceManager->init_table();
    }
    registerExitCallback([this]() {
        for (auto e: storage) {
            if (e.isValid()) {
                // printf("final entry: refcnt = %d\n", e.refcnt);
            }
        }
        if (enableDB) {
            db.save_db(simout.resolve("cmc.db").c_str());
        }
    });
    sendingEntry.invalidate();
    sendIDX_PTR = 0;
}

void
CMCPrefetcher::doPrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool is_first_shot)
{
    bool can_prefetch = cache->level() == 1 ? (!pfi.isWrite() && pfi.hasPC()) : true;
    if (!can_prefetch) {
        return;
    }
    Addr pc = pfi.hasPC() ? pfi.getPC() : 0;

    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
    bool is_secure = pfi.isSecure();
    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    int prefetchSource = pf_source;

    // if (enableDB) {
    //     trainTraceManager->write_record(
    //         TrainTrace(pc, block_addr, pfi.isCacheMiss(), prefetchSource)
    //     );
    // }

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
                printf("pc: %llx, count: %llu\n",
                    static_cast<unsigned long long>(pair.first),
                    static_cast<unsigned long long>(pair.second));
            }
            assert(false);
        }
    }

    DPRINTF(CMCPrefetcher, "CMC train: pc: %lx, addr: %lx\n", pc, block_addr);

    // not covered by other prefetcher
    bool nocovered = (pfi.isCacheMiss() && (!late)) ||
            (pf_source == PrefetchSourceType::CMC); // if cmc send pf to l2/3, this code line doesn't actually work

    // Prefetch: check if there is a match
    Addr storage_key =
        contextKey(hash(block_addr >> 6, pc), context_id);
    StorageEntry *match_entry =
        storage.findEntry(storage_key, is_secure);
    if (nocovered && match_entry) {
        statsCMC.storageHits++;
        storage.accessEntry(match_entry);
        // prefetch on cache miss only
        DPRINTF(CMCPrefetcher, "Storage hit, trigger pc: %lx, addr: %lx\n",
                pc, block_addr);
        // printf("=== Storage hit, trigger addr: %lx\n", block_addr);
        match_entry->refcnt++;
        int priority = Recorder::NR_ENTRY;
        uint32_t id = match_entry->id;
        StorageEntry entry_copy(*match_entry);
        entry_copy.trigger =
            std::make_unique<TriggerInfo>(pfi.trigger_info);
        if (tpDataQueue.size() >= maxTpDataQueueSize) {
            tpDataQueue.pop_front();
            statsCMC.dataQueueDrops++;
        }
        tpDataQueue.push_back(entry_copy);
        statsCMC.dataQueueEnqueues++;

        int num_send = 0;
        for (auto addr: match_entry->addresses) {
            if (sendPFWithFilter(
                    pfi, addr, addresses, priority,
                    PrefetchSourceType::CMC)) {
                num_send++;
                if (num_send > 24) {
                    addresses.back().pfahead = true;
                    addresses.back().pfahead_host = 3;
                } else if (num_send > 4) {
                    addresses.back().pfahead = true;
                    addresses.back().pfahead_host = 2;
                }
            }
            if (enableDB) {
                prefetchTraceManager->write_record(
                    PrefetchTrace(addr, id, priority)
                );
            }
            priority--;
        }
    }
    else if (match_entry) {
        statsCMC.storageUnusedHits++;
        // if storage entry can be covered by other prefetcher, shall we need to remove this entry?
        storage.invalidate(match_entry);
        DPRINTF(CMCPrefetcher, "Storage hit, but unused, trigger addr: %lx\n",
                block_addr);
    } else if (nocovered) {
        statsCMC.storageMisses++;
    }

    // Train: update temporal access chain
    bool finished = false;

    /* 1. Train trigger */
    bool sms_hit = !pfi.isCacheMiss() && (prefetchSource == PrefetchSourceType::SStream || prefetchSource == PrefetchSourceType::SPht);
    // ContextID isolates ownership without creating per-context capacity.
    bool wants_new_trigger = trigger.empty() || match_entry;
    bool train_trigger =
        wants_new_trigger && !trigger.full();
    bool do_training =
        !train_trigger && !trigger.empty() && nocovered &&
        trigger.front().contextId == context_id;
    if (train_trigger) {
        statsCMC.triggersCreated++;
        DPRINTF(CMCPrefetcher, "train_trigger index: %d, addr: %lx\n",
                trigger.size()-1, block_addr);
        assert(!trigger.full());

        trigger.push_back(
            RecordEntry(pc, block_addr, is_secure, context_id));
    } else if (wants_new_trigger && trigger.full()) {
        statsCMC.triggerStackFull++;
    }
    if (!train_trigger && !trigger.empty() && nocovered &&
        trigger.front().contextId != context_id) {
        statsCMC.trainingContextMismatches++;
    }
    /* 2. Train entry */
    if (do_training) {
        statsCMC.trainingSamples++;
        // Only the oldest trigger owner may advance the shared recorder.
        bool trained = recorder.train_entry(block_addr, is_secure, &finished);
        auto trigger_head = trigger.front();
        if (trained) {
            DPRINTF(CMCPrefetcher, "trained %x\n", block_addr);
        }
        if (finished) {
            statsCMC.trainingCompletions++;
            DPRINTF(CMCPrefetcher, "trigger train finished, pc: %lx, addr: %lx\n",
                    trigger_head.pc, trigger_head.addr);

            Addr trigger_key = contextKey(
                hash(trigger_head.addr >> 6, trigger_head.pc),
                trigger_head.contextId);
            StorageEntry *entry =
                storage.findEntry(trigger_key, trigger_head.is_secure);
            if (entry) {
                statsCMC.storageUpdates++;
                // storage.accessEntry(entry); do not update replacement
                DPRINTF(CMCPrefetcher, "CMC: enter the same trigger, pc: %lx, addr: %lx\n",
                                    trigger_head.pc, trigger_head.addr);
                entry->addresses = recorder.entries;

                entry->refcnt++;
                entry->id = acc_id;
                entry->contextId = trigger_head.contextId;
            } else {
                statsCMC.storageInserts++;
                entry = storage.findVictim(trigger_key);
                entry->addresses = recorder.entries;

                entry->refcnt = 0;
                entry->id = acc_id;
                entry->contextId = trigger_head.contextId;

                storage.insertEntry(
                    trigger_key,
                    trigger_head.is_secure,
                    entry
                );
            }

            for (auto addr: recorder.entries) {
                DPRINTF(CMCPrefetcher, "entry addr: 0x%lx\n",
                        addr);
            }
            trigger.pop_front();

            recorder.reset();
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

Addr cut_offset(Addr addr, int offset)
{
    return (addr >> offset) << offset;
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
    if (index >= NR_ENTRY) {
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
    // Count generated prefetch
    prefetchStats.pfGenerated++;

    Addr filter_key = sharedFilterKey(pfi, addr);
    if (filter->contains(filter_key)) {
        DPRINTF(CMCPrefetcher, "Skip recently prefetched: %lx\n", addr);
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;
    } else {
        DPRINTF(CMCPrefetcher, "CMC: send pf: %lx\n", addr);
        filter->insert(filter_key, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
    return false;
}

void
CMCPrefetcher::Recorder::reset() {
    index = 0;
    entries.clear();
}

void
CMCPrefetcher::StorageEntry::invalidate() {
    if (false) {
        if (this->isValid()) {
            printf("entry victim: refcnt = %d\n", this->refcnt);
        }
    }
    TaggedEntry::invalidate();
}
void 
CMCPrefetcher::InsertPFRequestToBuffer(const AddrPriority &addr_prio) {
    panic("CMCPrefetcher: InsertPFRequestToBuffer not implemented");
}
bool
CMCPrefetcher::hasPFRequestsInBuffer() {
    return !tpDataQueue.empty() || sendingEntry.isValid();
}
bool
CMCPrefetcher::GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) {
    //if sendingEntry is valid, send next addr
    if(sendingEntry.isValid()){
        if(sendIDX_PTR < sendingEntry.addresses.size()){
            Addr addr = sendingEntry.addresses[sendIDX_PTR];
            sendIDX_PTR++;
            if (sendingEntry.trigger) {
                addresses.push_back(AddrPriority(addr,
                    Recorder::NR_ENTRY - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC,
                    *(sendingEntry.trigger)));
            } else {
                addresses.push_back(AddrPriority(addr,
                    Recorder::NR_ENTRY - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC));
            }
            statsCMC.queuedCandidatesSent++;
            return true;
        }else{
            //finished sending this entry
            sendingEntry = StorageEntry();
            sendingEntry.invalidate();
            sendIDX_PTR = 0;
        }
    }
    //load next entry from tpDataQueue
    if(!tpDataQueue.empty()){
        //copy front entry to sendingEntry
        sendingEntry = StorageEntry(tpDataQueue.front());
        tpDataQueue.pop_front();
        sendIDX_PTR = 0;
        if(sendIDX_PTR < sendingEntry.addresses.size()){
            Addr addr = sendingEntry.addresses[sendIDX_PTR];
            sendIDX_PTR++;
            if (sendingEntry.trigger) {
                addresses.push_back(AddrPriority(addr,
                    Recorder::NR_ENTRY - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC,
                    *(sendingEntry.trigger)));
            } else {
                addresses.push_back(AddrPriority(addr,
                    Recorder::NR_ENTRY - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC));
            }
            statsCMC.queuedCandidatesSent++;
            return true;
        }else{
            //should not happen
            panic("CMCPrefetcher: empty addresses in sendingEntry");
        }
    }
    return false;
}

}  // prefetch
}  // gem5
