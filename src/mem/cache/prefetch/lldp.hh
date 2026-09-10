// Load-load dependency learning and miss-data driven prefetch generation.
#ifndef __MEM_CACHE_PREFETCH_LLDP_HH__
#define __MEM_CACHE_PREFETCH_LLDP_HH__

#include <array>
#include <deque>
#include <optional>

#include "cpu/o3/dyn_inst_xsmeta.hh"
#include "mem/cache/prefetch/queued.hh"

namespace gem5
{
class BaseCPU;
struct LLDPrefetcherParams;
namespace prefetch
{
class LLDPrefetcher : public Queued
{
  private:
    static constexpr unsigned TableEntries = 64;
    static constexpr unsigned SubEntries = 4;
    struct SubEntry
    {
        bool valid{false};
        Addr consumerPC{0};
        lldp::Chain chain;
        int64_t loadImm{0};
        uint8_t immConf{0};
        uint8_t cConf{0};
    };
    struct Entry
    {
        bool valid{false};
        Addr producerPC{0};
        ContextID context{InvalidContextID};
        uint64_t generation{0};
        uint8_t pConf{0};
        std::array<SubEntry, SubEntries> consumers{};
        lldp::PLRU<SubEntries> replacement;
    };
    struct Training
    {
        o3::XsDynInstMetaPtr owner;
        lldp::Chain chain;
        Addr consumerPC;
        ContextID context;
        int64_t loadImm;
        int producer{-1};
        int consumer{-1};
        uint64_t version{0};
    };
    struct Update
    {
        Training training;
        unsigned row;
        Entry entry;
    };
    std::array<Entry, TableEntries> table{};
    lldp::PLRU<TableEntries> replacement;
    uint64_t generation{0};
    uint64_t version{0};
    std::deque<Training> input;
    std::optional<Training> s0;
    std::optional<Update> s1;
    EventFunctionWrapper learningEvent;
    const unsigned trainingQueueSize;
    const uint8_t maxConf;
    const uint8_t initialConf;
    const uint8_t producerInitialConf;
    const uint8_t producerThreshold;
    const uint8_t consumerThreshold;
    const uint8_t immediateThreshold;
    BaseCPU *trainingCPU;

    class DependenceListener : public ProbeListenerArgBase<o3::XsDynInstMetaPtr>
    {
        LLDPrefetcher &parent;
      public:
        DependenceListener(LLDPrefetcher &p, std::string name)
            : ProbeListenerArgBase(std::move(name)), parent(p) {}
        void notify(const o3::XsDynInstMetaPtr &meta) override
        { parent.dependenceTrain(meta); }
    };
    ProbeListenerPtr<DependenceListener> dependenceListener;

    struct LLDPStats : public statistics::Group
    {
        LLDPStats(statistics::Group *parent);
        statistics::Scalar trainAccepted, trainDropped, trainSquashed;
        statistics::Scalar dualSourceTrainRejected;
        statistics::Scalar producerWrites, producerReplacements;
        statistics::Scalar producerMatches, pipelineBypasses;
        statistics::Scalar lengthChanges, opChanges, immChanges;
        statistics::Scalar hints, hitHintsDiscarded, returnedHints, staleHints;
        statistics::Scalar candidates, duplicates, unsupported;
        statistics::Vector childrenAtReplacement, childrenAtDump;
        statistics::Scalar validProducers;
    } stats;

    int findProducer(Addr pc, ContextID context) const;
    int findConsumer(const Entry &entry, Addr pc) const;
    void learningTick();
    Update makeUpdate(Training training);
    void commitUpdate(const Update &update);
    unsigned validChildren(const Entry &entry) const;
    lldp::Hint pfHint(const PacketPtr &pkt);

  public:
    LLDPrefetcher(const LLDPrefetcherParams &p);
    void regProbeListeners() override;
    void preDumpStats() override;
    void dependenceTrain(const o3::XsDynInstMetaPtr &meta);
    lldp::Hint loadTrain(const PacketPtr &pkt, bool miss) override;
    void hintData(const lldp::Hint &hint, const PacketPtr &demand,
                  const uint8_t *data, unsigned size) override;
    void calculatePrefetch(const PrefetchInfo &,
                           std::vector<AddrPriority> &) override {}
    void addToQueue(std::list<DeferredPacket> &queue,
                    DeferredPacket &dpp) override;
    void rxHint(BaseMMU::Translation *dpp) override;
};
} // namespace prefetch
} // namespace gem5
#endif // __MEM_CACHE_PREFETCH_LLDP_HH__
