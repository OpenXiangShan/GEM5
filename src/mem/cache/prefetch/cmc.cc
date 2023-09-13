#include "mem/cache/prefetch/cmc.hh"

#include "debug/CMCPrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"

namespace gem5
{
namespace prefetch
{

CMCPrefetcher::CMCPrefetcher(const CMCPrefetcherParams &p)
: Queued(p),
    recorder(new Recorder(p.degree)),
    storage(p.storage_entries, p.storage_entries, p.storage_indexing_policy,
            p.storage_replacement_policy, StorageEntry()),
    degree(p.degree), enableDB(p.enablePrefetchDB)
{
    for (int i = 0; i < STACK_SIZE; i++) {
        trigger_stack[i].valid = false;
    }
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
            db.save_db("cmc.db");
        }
    });
}

void
CMCPrefetcher::doPrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat)
{
    bool can_prefetch = !pfi.isWrite() && pfi.hasPC();
    if (!can_prefetch) {
        return;
    }

    Addr vaddr = pfi.getAddr();
    Addr block_addr = blockAddress(vaddr);
    bool is_secure = pfi.isSecure();
    Addr pc = pfi.getPC();
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
                printf("pc: %lx, count: %ld\n", pair.first, pair.second);
            }
            assert(false);
        }
    }

    DPRINTF(CMCPrefetcher, "CMC train: addr: %x\n", block_addr);

    // Prefetch: check if there is a match
    StorageEntry *match_entry = storage.findEntry(block_addr>>6, is_secure);
    if (pfi.isCacheMiss() && match_entry) {
        // prefetch on cache miss only
        DPRINTF(CMCPrefetcher, "Storage hit, trigger addr: %lx\n",
                block_addr);
        // printf("=== Storage hit, trigger addr: %lx\n", block_addr);
        match_entry->refcnt++;
        int priority = recorder->nr_entry;
        uint32_t id = match_entry->id;

        for (auto addr: match_entry->addresses) {
            /* !Perf degradation feature!
                if (addr == block_addr) break;
            */
            uint32_t mixedNum = (id << 7);
            mixedNum += priority;
            addresses.push_back(AddrPriority(addr, mixedNum, PrefetchSourceType::CMC));
            if (enableDB) {
                prefetchTraceManager->write_record(
                    PrefetchTrace(addr, id, priority)
                );
            }
            priority--;
        }
        return; // do not train on cmc hit?
    }

    // Train: update temporal access chain
    bool finished = false;

    /* 1. Train trigger */
    bool sms_hit = !pfi.isCacheMiss() && (prefetchSource == PrefetchSourceType::SStream || prefetchSource == PrefetchSourceType::SPht);
    bool train_trigger =
        (!trigger_stack[1].valid || match_entry) && !trigger_stack[3].valid;
    //(trigger_stack.size() <= 1 || match_entry) && !trigger_stack.full()
    bool do_training =
        !train_trigger && trigger_stack[0].valid; // && !sms_hit;
    // !train_trigger && !trigger_stack.empty()
    if (train_trigger) {
        int i = 0;
        for (i = 0; i < STACK_SIZE; i++) {
            if (!trigger_stack[i].valid) {
                trigger_stack[i].valid = true;
                trigger_stack[i].addr = block_addr;
                trigger_stack[i].pc = pc;
                trigger_stack[i].is_secure = is_secure;
                for (int j = i+1; j < STACK_SIZE; j++) {
                    assert(!trigger_stack[j].valid);
                }
                break;
            }
        }
        DPRINTF(CMCPrefetcher, "train_trigger index: %d, addr: %lx\n",
                i, block_addr);
    }

    /* 2. Train entry */
    if (do_training) {
        bool trained = recorder->train_entry(block_addr, is_secure, &finished);
        if (trained) {
            DPRINTF(CMCPrefetcher, "trained %x\n", block_addr);
        }
        if (finished) {
            // TODO
            StorageEntry *entry = storage.findVictim(trigger_stack[0].addr>>6);
            for (auto recorder_entry: recorder->entries) {
                entry->addresses.push_back(recorder_entry.addr);
            }
            entry->refcnt = 0;
            entry->id = acc_id;

            DPRINTF(CMCPrefetcher, "storage insert, trigger addr: %lx\n",
                    trigger_stack[0].addr);

            if (enableDB) {
                triggerTraceManager->write_record(
                    TriggerTrace(trigger_stack[0].pc, trigger_stack[0].addr)
                );
                entryTraceManager->write_record(
                    EntryTrace(
                        trigger_stack[0].pc,
                        trigger_stack[0].addr,
                        acc_id,
                        &recorder->entries
                    )
                );
            }

            for (auto recorder_entry: recorder->entries) {
                DPRINTF(CMCPrefetcher, "entry addr: 0x%lx\n",
                        recorder_entry.addr);
            }
            storage.insertEntry(
                trigger_stack[0].addr>>6,
                trigger_stack[0].is_secure,
                entry
            );

            for (int i = 0; i < STACK_SIZE-1; i++) {
                trigger_stack[i] = trigger_stack[i+1];
            }
            trigger_stack[STACK_SIZE-1].valid = false;
            recorder->reset();
            acc_id++;
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
        entries.push_back(RecordEntry(addr, is_secure, true));
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
        entries.push_back(RecordEntry(addr, is_secure, true));
        index++;
    }
    return true;
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

}  // prefetch
}  // gem5
