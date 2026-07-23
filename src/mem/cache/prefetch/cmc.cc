#include "mem/cache/prefetch/cmc.hh"

#include <algorithm>
#include <memory>

#include "base/output.hh"
#include "debug/CMCPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

CMCPrefetcher::CMCPrefetcher(const CMCPrefetcherParams &p)
: Queued(p),
    storage(p.storage_entries, p.storage_entries, p.storage_indexing_policy,
            p.storage_replacement_policy, StorageEntry()),
    degree(p.degree),
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

CMCPrefetcher::Recorder &
CMCPrefetcher::recorderFor(ContextID context_id)
{
    auto &recorder = recorders[context_id];
    if (!recorder) {
        recorder = std::make_unique<Recorder>(degree);
    }
    return *recorder;
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
    auto &recorder = recorderFor(context_id);
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
        storage.accessEntry(match_entry);
        // prefetch on cache miss only
        DPRINTF(CMCPrefetcher, "Storage hit, trigger pc: %lx, addr: %lx\n",
                pc, block_addr);
        // printf("=== Storage hit, trigger addr: %lx\n", block_addr);
        match_entry->refcnt++;
        int priority = Recorder::nrEntry;
        uint32_t id = match_entry->id;
        //create a copy , insert to tpdataqueue
        StorageEntry entry_copy = StorageEntry(*match_entry);
        entry_copy.trigger = std::make_unique<TriggerInfo>(pfi.trigger_info);
        if( tpDataQueue.size() >= maxTpDataQueueSize){
            tpDataQueue.pop_front();
        }
        tpDataQueue.push_back(entry_copy);
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
            if (enableDB) {
                prefetchTraceManager->write_record(
                    PrefetchTrace(addr, id, priority)
                );
            }
            priority--;
        }
    }
    else if (match_entry) {
        // if storage entry can be covered by other prefetcher, shall we need to remove this entry?
        storage.invalidate(match_entry);
        DPRINTF(CMCPrefetcher, "Storage hit, but unused, trigger addr: %lx\n",
                block_addr);
    }

    // Train: update temporal access chain
    bool finished = false;

    /* 1. Train trigger */
    bool sms_hit = !pfi.isCacheMiss() && (prefetchSource == PrefetchSourceType::SStream || prefetchSource == PrefetchSourceType::SPht);
    auto trigger_it = std::find_if(
        trigger.begin(), trigger.end(),
        [context_id](const RecordEntry &entry) {
            return entry.contextId == context_id;
        });
    bool has_context_trigger = trigger_it != trigger.end();
    bool train_trigger =
        (!has_context_trigger || match_entry) && !trigger.full();
    bool do_training =
        !train_trigger && has_context_trigger && nocovered;
    if (train_trigger) {
        DPRINTF(CMCPrefetcher, "train_trigger index: %d, addr: %lx\n",
                trigger.size()-1, block_addr);
        assert(!trigger.full());

        trigger.push_back(
            RecordEntry(pc, block_addr, is_secure, context_id));
    }

    /* 2. Train entry */
    if (do_training) {
        bool trained = recorder.train_entry(block_addr, is_secure, &finished);
        trigger_it = std::find_if(
            trigger.begin(), trigger.end(),
            [context_id](const RecordEntry &entry) {
                return entry.contextId == context_id;
            });
        assert(trigger_it != trigger.end());
        auto &trigger_head = *trigger_it;
        if (trained) {
            DPRINTF(CMCPrefetcher, "trained %x\n", block_addr);
        }
        if (finished) {
            DPRINTF(CMCPrefetcher, "trigger train finished, pc: %lx, addr: %lx\n",
                    trigger_head.pc, trigger_head.addr);

            Addr trigger_key = contextKey(
                hash(trigger_head.addr >> 6, trigger_head.pc), context_id);
            StorageEntry *entry =
                storage.findEntry(trigger_key, trigger_head.is_secure);
            if (entry) {
                // storage.accessEntry(entry); do not update replacement
                DPRINTF(CMCPrefetcher, "CMC: enter the same trigger, pc: %lx, addr: %lx\n",
                                    trigger_head.pc, trigger_head.addr);
                entry->addresses = recorder.entries;

                entry->refcnt++;
                entry->id = acc_id;
                entry->contextId = context_id;
            } else {
                entry = storage.findVictim(trigger_key);
                entry->addresses = recorder.entries;

                entry->refcnt = 0;
                entry->id = acc_id;
                entry->contextId = context_id;

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
            trigger.erase(trigger_it);

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
    if (index >= nrEntry) {
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

    ContextID context_id = pfi.hasContextId() ?
        pfi.contextId() : InvalidContextID;
    Addr filter_key = contextKey(addr, context_id);
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
                    Recorder::nrEntry - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC,
                    *(sendingEntry.trigger)));
            } else {
                addresses.push_back(AddrPriority(addr,
                    Recorder::nrEntry - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC));
            }
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
                    Recorder::nrEntry - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC,
                    *(sendingEntry.trigger)));
            } else {
                addresses.push_back(AddrPriority(addr,
                    Recorder::nrEntry - sendIDX_PTR + 1,
                    PrefetchSourceType::CMC));
            }
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
