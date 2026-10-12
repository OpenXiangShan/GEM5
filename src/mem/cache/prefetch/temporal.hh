#ifndef __MEM_CACHE_PREFETCH_TEMPORAL_HH__
#define __MEM_CACHE_PREFETCH_TEMPORAL_HH__

#include <deque>
#include <unordered_map>

#include "cpu/o3/dyn_inst_xsmeta.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/temporal_logic.hh"

namespace gem5
{
class BaseCPU;
struct TemporalPrefetcherParams;
namespace prefetch
{
class TemporalPrefetcher : public Queued
{
    temporal::Engine engine;
    BaseCPU *trainingCPU;
    const unsigned throttleCycles;
    Tick nextSend{0};
    Tick nextIssue{0};
    uint64_t candidateId{0};
    struct Owner { Addr pc; ContextID context; };
    std::unordered_map<uint64_t, Owner> owners;
    std::deque<std::pair<ContextID, InstSeqNum>> observedLoads;
    std::deque<uint64_t> ownerOrder;
    class DependenceListener : public ProbeListenerArgBase<o3::XsDynInstMetaPtr>
    {
        TemporalPrefetcher &parent;
      public:
        DependenceListener(TemporalPrefetcher &p)
            : ProbeListenerArgBase("dependenceTrain"), parent(p) {}
        void notify(const o3::XsDynInstMetaPtr &meta) override;
    };
    ProbeListenerPtr<DependenceListener> dependenceListener;
    struct TPStats : public statistics::Group
    {
        TPStats(statistics::Group *parent);
        statistics::Scalar trains, filterHits, samplerHits, pairs, records,
            recorderReplacements, metaHits, metaBlocked, issueBlocked,
            issueProbes, metaProbes, candidates, queued, useful, late, dropped,
            ownerExpired;
        statistics::Vector trainRoles, recordRoles;
    } stats;
    void feedback(uint64_t id, bool late);

  public:
    explicit TemporalPrefetcher(const TemporalPrefetcherParams &p);

    using Queued::notifyPrefetchUseful;
    using Queued::prefetchUnused;

    void regProbeListeners() override;
    void preDumpStats() override;
    lldp::Hint loadTrain(const PacketPtr &pkt, bool miss) override;
    void calculatePrefetch(const PrefetchInfo &,
                          std::vector<AddrPriority> &) override {}
    void addToQueue(std::list<DeferredPacket> &queue,
                    DeferredPacket &dpp) override;
    PacketPtr getPacket() override;
    Tick nextPrefetchReadyTime() const override;
    bool hasPendingPacket() override;
    void prefetchDropped(const DeferredPacket &dpp) override;
    void notifyPrefetchUseful(PrefetchSourceType source, uint64_t id) override;
    void pfHitInCache(PrefetchSourceType source, uint64_t id) override;
    void pfHitInWB(PrefetchSourceType source, uint64_t id) override;
    void pfHitInMSHR(PrefetchSourceType source, uint64_t id) override;
    void prefetchUnused(PrefetchSourceType source, uint64_t id) override;
    void prefetchUnused(Addr paddr, PrefetchSourceType source,
                        uint64_t id) override;
    void notifyPrefetchMerged(uint64_t id) override;
    void rxHint(BaseMMU::Translation *) override {}
};
} // namespace prefetch
} // namespace gem5
#endif
