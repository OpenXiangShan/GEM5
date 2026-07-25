#ifndef GEM5_NEXTLINE_HH
#define GEM5_NEXTLINE_HH

#include <boost/circular_buffer.hpp>
#include <boost/compute/detail/lru_cache.hpp>
#include <list>
#include <memory>
#include <vector>

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


class CMCPrefetcher : public Queued
{
  public:
        using TriggerInfo = PFTriggerInfo;
    class StorageEntry;
    class RecordEntry
    {
        public:
            Addr pc;
            Addr addr;
            bool is_secure;
            ContextID contextId;
            RecordEntry(Addr p, Addr a, bool s, ContextID context_id)
                : pc(p), addr(a), is_secure(s), contextId(context_id) {}
            RecordEntry()
                : addr(0), is_secure(true), contextId(InvalidContextID)
            {}
    };
    class Recorder
    {
        public:
            std::vector<Addr> entries;
            int index;
            const int degree;
            Recorder(int d) : entries(), index(0), degree(d) {}
            bool entry_empty() { return entries.empty(); }
            Addr get_base_addr() { return entries[0]; }

            bool train_entry(Addr, bool, bool*);
            void reset();
            static constexpr int nrEntry = 12;
        private:
    };

    class StorageEntry : public TaggedEntry
    {
        public:
            std::vector<Addr> addresses;
            int refcnt;
            uint64_t id;
            ContextID contextId;
            void invalidate() override;
            std::unique_ptr<TriggerInfo> trigger;
            StorageEntry()
                : addresses(), refcnt(0), id(0),
                  contextId(InvalidContextID), trigger(nullptr)
            {}

            // copy constructor
            StorageEntry(const StorageEntry &other)
                : TaggedEntry(other),
                  addresses(other.addresses),
                  refcnt(other.refcnt),
                  id(other.id),
                  contextId(other.contextId)
            {
                if (other.trigger) {
                    trigger = std::make_unique<TriggerInfo>(*(other.trigger));
                }
            }

            // copy assignment
            StorageEntry& operator=(const StorageEntry &other)
            {
                if (this != &other) {
                    TaggedEntry::operator=(other);
                    addresses = other.addresses;
                    refcnt = other.refcnt;
                    id = other.id;
                    contextId = other.contextId;
                    if (other.trigger) {
                        trigger = std::make_unique<TriggerInfo>(*(other.trigger));
                    } else {
                        trigger.reset();
                    }
                }
                return *this;
            }

            StorageEntry(StorageEntry &&) noexcept = default;
            StorageEntry& operator=(StorageEntry &&) noexcept = default;
            ~StorageEntry() = default;
    };
  private:
    Recorder recorder;
    AssociativeSet<StorageEntry> storage;
    uint64_t acc_id = 1;

    struct CMCStats : public statistics::Group
    {
        CMCStats(statistics::Group *parent);

        statistics::Scalar storageHits;
        statistics::Scalar storageMisses;
        statistics::Scalar storageUnusedHits;
        statistics::Scalar triggersCreated;
        statistics::Scalar triggerStackFull;
        statistics::Scalar trainingSamples;
        statistics::Scalar trainingContextMismatches;
        statistics::Scalar trainingCompletions;
        statistics::Scalar storageInserts;
        statistics::Scalar storageUpdates;
        statistics::Scalar dataQueueEnqueues;
        statistics::Scalar dataQueueDrops;
        statistics::Scalar queuedCandidatesSent;
    } statsCMC;

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

    boost::compute::detail::lru_cache<Addr, Addr> *filter;

    void doPrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late,
                           PrefetchSourceType pf_source, bool is_first_shot);
  private:
    uint64_t hash(Addr addr, Addr pc) {
        return addr ^ (pc<<8);
    }

    bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                          PrefetchSourceType src);

    static const int STACK_SIZE = 4;
    boost::circular_buffer<RecordEntry> trigger;
    protected:
    std::list<StorageEntry> tpDataQueue;
    const int maxTpDataQueueSize = 8;
    StorageEntry sendingEntry;
    int sendIDX_PTR = 0; // Points to the next address in sendingEntry.
    void InsertPFRequestToBuffer(const AddrPriority &addr_prio) override;
    public:
    bool GetPFRequestsFromBuffer(std::vector<AddrPriority> &addresses) override;
    bool hasPFRequestsInBuffer() override;
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
        std::vector<CMCPrefetcher::RecordEntry> *entries
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
