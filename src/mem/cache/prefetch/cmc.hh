#ifndef GEM5_NEXTLINE_HH
#define GEM5_NEXTLINE_HH

#include "base/types.hh"
#include "cpu/pred/general_arch_db.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "mem/packet.hh"
#include "params/CMCPrefetcher.hh"

namespace gem5
{
struct CMCPrefetcherParams;
GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);

namespace prefetch
{

class RecordEntry
{
    public:
        Addr addr;
        Addr pc;
        bool is_secure;
        bool valid;
        RecordEntry(Addr a, bool s, bool v)
            : addr(a), is_secure(s), valid(v) {}
        RecordEntry() : addr(0), is_secure(true), valid(false) {}
};

class CMCPrefetcher : public Queued
{
    class Recorder
    {
        public:
            std::vector<RecordEntry> entries;
            int index;
            const int degree;
            Recorder(int d) : entries(), index(0), degree(d) {}
            bool entry_empty() { return entries.empty(); }
            Addr get_base_addr() { return entries[0].addr; }

            bool train_entry(Addr, bool, bool*);
            void reset();
            const int nr_entry = 16;
        private:
    };

    class StorageEntry : public TaggedEntry
    {
        public:
            std::vector<Addr> addresses;
            int refcnt;
            uint64_t id;
            void invalidate() override;
    };

    Recorder *recorder;
    AssociativeSet<StorageEntry> storage;
    const int degree;
    uint64_t acc_id = 1;

    bool enableDB;
    DataBase db;
    TraceManager *trainTraceManager;
    TraceManager *triggerTraceManager;
    TraceManager *entryTraceManager;
    TraceManager *prefetchTraceManager;

    public:
        CMCPrefetcher(const CMCPrefetcherParams &p);
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override
    {
        panic("not implemented");
    };

    void doPrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool miss_repeat);
    private:
        static const int STACK_SIZE = 4;
        RecordEntry trigger_stack[STACK_SIZE];
};

struct TriggerTrace : public Record
{
    TriggerTrace(uint64_t pc, uint64_t addr)
    {
        _tick = curTick();
        _uint64_data["triggerPC"] = pc;
        _uint64_data["triggerAddr"] = addr;
    }
};

struct TrainTrace : public Record
{
    TrainTrace(uint64_t pc, uint64_t addr, uint64_t is_miss, uint64_t source)
    {
        _tick = curTick();
        _uint64_data["trainPC"] = pc;
        _uint64_data["trainVAddr"] = addr;
        _uint64_data["isMiss"] = is_miss;
        _uint64_data["prefetchSource"] = source;
    }
};

struct EntryTrace : public Record
{
    EntryTrace(
        uint64_t pc,
        uint64_t trigger,
        uint64_t id,
        std::vector<RecordEntry> *entries
    ) {
        _tick = curTick();
        _uint64_data["triggerPC"] = pc;
        _uint64_data["triggerAddr"] = trigger;
        _uint64_data["entryID"] = id;
        int i = 0;
        for (auto recorder_entry: *entries) {
            auto sIndex =
                std::string(2-std::to_string(i).length(), '0') +
                std::to_string(i);
            _uint64_data["entryAddr_" + sIndex] = recorder_entry.addr;
            i++;
        }
        for (; i <= 34; i++) {
            auto sIndex =
                std::string(2-std::to_string(i).length(), '0') +
                std::to_string(i);
            _uint64_data["entryAddr_" + std::to_string(i)] = 0;
        }
    }
};

struct PrefetchTrace : public Record
{
    PrefetchTrace(uint64_t vaddr, uint64_t id, uint64_t priority)
    {
        _tick = curTick();
        _uint64_data["pfVaddr"] = vaddr;
        _uint64_data["pfID"] = id;
        _uint64_data["pfPriority"] = priority;
    }
};

}  // namespace prefetch
}  // namespace gem5

#endif  // GEM5_SMS_HH
