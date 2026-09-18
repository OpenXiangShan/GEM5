// Load-load dependency learning and miss-data driven prefetch generation.
#ifndef __MEM_CACHE_PREFETCH_LLDP_HH__
#define __MEM_CACHE_PREFETCH_LLDP_HH__

#include <array>
#include <deque>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
        int64_t immLine{0};
        int64_t loadLine{0};
        uint8_t immOffset{0};
        uint8_t loadSize{0};
        bool loadAddressValid{false};
        uint8_t immConf{0};
        uint8_t lineConf{0};
        uint8_t offsetConf{0};
        uint8_t cConf{0};
        uint64_t updateCount{0};
        uint64_t replacementCount{0};
        uint64_t exactImmStableCount{0};
        uint64_t lineImmStableCount{0};
        uint64_t candidateCount{0};
        uint64_t usefulCount{0};
        uint64_t lateCount{0};
        uint64_t demandHitCount{0};
    };
    struct Entry
    {
        bool valid{false};
        Addr producerPC{0};
        ContextID context{InvalidContextID};
        uint64_t generation{0};
        uint8_t pConf{0};
        uint64_t updateCount{0};
        uint64_t replacementCount{0};
        uint64_t candidateCount{0};
        uint64_t usefulCount{0};
        uint64_t lateCount{0};
        uint64_t activePeak{0};
        uint64_t consumerReplacement{0};
        uint64_t consumerOverflow{0};
        Addr lastDemandPC{0};
        uint64_t lastDemandChainId{0};
        PrefetchSourceType lastDemandSource{PrefetchSourceType::PF_NONE};
        uint64_t demandHitCount{0};
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
        uint8_t loadSize{0};
        int64_t loadLine{0};
        uint8_t loadOffset{0};
        bool loadAddressValid{false};
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

    static constexpr unsigned AddressTableWays = 4;
    static constexpr unsigned SamplerEntries = 256;
    static constexpr unsigned SamplerSets =
        SamplerEntries / AddressTableWays;
    static constexpr unsigned MetaEntries = 1024;
    static constexpr unsigned MetaSets = MetaEntries / AddressTableWays;
    static constexpr uint8_t SamplerThreshold = 3;
    struct SamplerEntry
    {
        bool valid{false};
        Addr addrP{0};
        Addr producerPC{0};
        Addr consumerPC{0};
        ContextID context{InvalidContextID};
        Addr addrC{0};
        uint8_t stableCount{0};
        uint8_t mismatchCount{0};
        uint8_t rrpv{3};
        uint8_t matchesSincePromotion{0};
        uint32_t lastSeenEpoch{0};
        uint32_t promotionVersion{0};
    };
    struct MetaEntry
    {
        bool valid{false};
        Addr addrP{0};
        Addr addrC{0};
        Addr producerPC{0};
        Addr consumerPC{0};
        ContextID context{InvalidContextID};
        uint32_t generation{0};
        uint8_t trainConf{0};
        uint8_t qualityConf{0};
        uint8_t timelyConf{0};
        uint8_t tokens{0};
        uint8_t outstanding{0};
        uint8_t rrpv{3};
        uint32_t lastTrainEpoch{0};
        uint32_t lastUsefulEpoch{0};
    };
    std::array<std::array<SamplerEntry, AddressTableWays>, SamplerSets>
        samplerTable{};
    std::array<lldp::PLRU<AddressTableWays>, SamplerSets>
        samplerReplacement{};
    std::array<std::array<MetaEntry, AddressTableWays>, MetaSets>
        metaTable{};
    std::array<lldp::PLRU<AddressTableWays>, MetaSets> metaReplacement{};
    uint64_t generation{0};
    uint64_t candidateId{0};
    uint64_t version{0};
    std::deque<Training> input;
    static constexpr unsigned TlbFilterEntries = 64;
    std::deque<Addr> tlbFilter;
    std::unordered_set<Addr> tlbFilterSet;
    std::unordered_map<Addr, Addr> tlbFilterTranslations;
    uint32_t tableEpoch{0};
    struct CandidateOwner
    {
        unsigned row;
        unsigned col;
        uint64_t generation;
        Addr consumerPC;
        PrefetchSourceType source;
        bool meta{false};
        unsigned metaSet{0};
        unsigned metaWay{0};
        uint32_t metaGeneration{0};
    };
    std::unordered_map<uint64_t, CandidateOwner> candidateOwners;
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
        statistics::Scalar spatialLoadTrain, spatialHints;
        statistics::Scalar lengthChanges, opChanges, immChanges;
        statistics::Scalar exactImmStable, lineImmStable, offsetImmStable;
        statistics::Scalar sameLineImmUpdates, nextLineImmUpdates,
            otherLineImmUpdates;
        statistics::Vector immValueHist, lineDeltaHist, offsetDeltaHist,
            byteOffsetHist;
        statistics::Scalar hints, hitHintsDiscarded, hitHintsRetained,
            returnedHints, staleHints;
        statistics::Scalar candidates, filtered, duplicates, unsupported;
        statistics::Scalar samplerOutputsToMeta, metaTableHits,
            metaTablePrefetches, samplerValidEntries, metaValidEntries;
        statistics::Scalar samplerTargetMismatch, samplerRepromotions,
            metaInvalidations, metaTokenStalls, metaFallbacks;
        statistics::Vector samplerReplacementCnt;
        statistics::Scalar candidateGenerated, candidateQueued, candidateIssued,
            candidateDropped, candidateMerged, candidateUseful, candidateUnused,
            candidateLate;
        statistics::Scalar candidateCacheHit, candidateMshrHit, candidateWbHit;
        statistics::Vector childrenAtReplacement, childrenAtDump;
        statistics::Vector pcpActiveHistogram;
        statistics::Vector pcpActivePeak, pcpConsumerReplacement,
            pcpConsumerOverflow, pcpCandidate, pcpUseful, pcpLate;
        statistics::Vector pcpProducerPC, pcpContext, pcpValid, pcpConf;
        statistics::Vector pcpDemandPC, pcpDemandChainId, pcpDemandSource,
            pcpDemandHits;
        statistics::Vector consumerValid, consumerPC, consumerLength,
            consumerOp1, consumerOp2, consumerImmLoad, consumerImmConf,
            consumerLineConf, consumerOffsetConf, consumerConf,
            consumerUpdates, consumerReplacements, consumerExactImmStable,
            consumerLineImmStable,
            consumerCandidates, consumerUseful, consumerLate,
            consumerDemandHits;
        statistics::Scalar validProducers;
    } stats;

    int findProducer(Addr pc, ContextID context) const;
    int findConsumer(const Entry &entry, Addr pc) const;
    void learningTick();
    Update makeUpdate(Training training);
    void commitUpdate(const Update &update);
    unsigned validChildren(const Entry &entry) const;
    lldp::Hint pfHint(const PacketPtr &pkt);
    bool isSpatialPrefetch(const PacketPtr &pkt) const;
    static bool isLldpSource(PrefetchSourceType source);
    unsigned samplerSet(Addr addr_p) const;
    unsigned metaSet(Addr addr_p) const;
    unsigned samplerVictim(unsigned set);
    unsigned metaVictim(unsigned set);
    void trainAddressPair(Addr addr_p, Addr addr_c, Addr producer_pc,
                          Addr consumer_pc, ContextID context);
    void updateMetaTable(const SamplerEntry &sample);
    struct MetaHit
    {
        Addr addrC{0};
        unsigned set{0};
        unsigned way{0};
        uint32_t generation{0};
    };
    std::optional<MetaHit> lookupMetaTable(Addr addr_p, Addr producer_pc,
                                           Addr consumer_pc, ContextID context);
    void ageMetaTable();
    void updateMetaOwner(uint64_t candidate_id, int result);
    bool queueCandidate(const PacketPtr &demand, const lldp::Hint &hint,
                        Addr addr_p, Addr addr_c,
                        PrefetchSourceType source,
                        std::optional<unsigned> consumer,
                        std::optional<MetaHit> meta_hit = std::nullopt);
    bool filterCandidate(Addr line);
    bool rejectTranslatedPrefetch(const DeferredPacket &dpp,
                                  Addr paddr) override;
    bool rejectPrefetchCandidate(const PrefetchInfo &pfi,
                                 const AddrPriority &addr_prio) override;
    void prefetchDropped(const DeferredPacket &dpp) override;

  public:
    LLDPrefetcher(const LLDPrefetcherParams &p);
    void regProbeListeners() override;
    void preDumpStats() override;
    void dependenceTrain(const o3::XsDynInstMetaPtr &meta);
    lldp::Hint loadTrain(const PacketPtr &pkt, bool miss) override;
    void hintData(const lldp::Hint &hint, const PacketPtr &demand,
                  Addr addr_p, const uint8_t *data, unsigned size) override;
    void notifyPrefetchUseful(PrefetchSourceType source) override;
    void notifyPrefetchUseful(PrefetchSourceType source,
                              uint64_t candidate_id) override;
    void prefetchUnused(PrefetchSourceType source) override;
    void prefetchUnused(PrefetchSourceType source,
                        uint64_t candidate_id) override;
    void prefetchUnused(Addr paddr, PrefetchSourceType source,
                        uint64_t candidate_id) override;
    void notifyPrefetchMerged(uint64_t candidate_id) override;
    void notifyCandidateDemand(uint64_t candidate_id,
                               const PacketPtr &demand) override;
    void pfHitInCache(PrefetchSourceType source) override;
    void pfHitInCache(PrefetchSourceType source,
                      uint64_t candidate_id) override;
    void pfHitInMSHR(PrefetchSourceType source) override;
    void pfHitInMSHR(PrefetchSourceType source,
                     uint64_t candidate_id) override;
    void pfHitInWB(PrefetchSourceType source) override;
    void pfHitInWB(PrefetchSourceType source,
                   uint64_t candidate_id) override;
    void recordIssuedPrefetchStats(const PacketPtr &pkt) override;
    void calculatePrefetch(const PrefetchInfo &,
                           std::vector<AddrPriority> &) override {}
    void addToQueue(std::list<DeferredPacket> &queue,
                    DeferredPacket &dpp) override;
    void rxHint(BaseMMU::Translation *dpp) override;
};
} // namespace prefetch
} // namespace gem5
#endif // __MEM_CACHE_PREFETCH_LLDP_HH__
