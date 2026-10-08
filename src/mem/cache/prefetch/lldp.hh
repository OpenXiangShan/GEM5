// Load-load dependency learning and miss-data driven prefetch generation.
#ifndef __MEM_CACHE_PREFETCH_LLDP_HH__
#define __MEM_CACHE_PREFETCH_LLDP_HH__

#include <array>
#include <deque>
#include <fstream>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/dyn_inst_xsmeta.hh"
#include "mem/cache/prefetch/candidate_table.hh"
#include "mem/cache/prefetch/demand_pair_history.hh"
#include "mem/cache/prefetch/quality_control.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/cache/prefetch/spatial_feedback.hh"
#include "mem/cache/prefetch/temporal_control.hh"

namespace gem5
{
class BaseCPU;
struct LLDPrefetcherParams;
namespace prefetch
{
class LLDPrefetcher : public Queued
{
  private:
    using SpatialFeedbackHandler =
        std::function<void(const SpatialFeedback &)>;
    static constexpr unsigned TableEntries = 64;
    static constexpr unsigned SubEntries = 4;
    struct SubEntry
    {
        bool valid{false};
        Addr consumerPC{0};
        uint64_t generation{0};
        lldp::Chain chain;
        int64_t loadImm{0};
        int64_t immLine{0};
        int64_t loadLine{0};
        uint8_t immOffset{0};
        uint8_t loadOffset{0};
        uint8_t loadSize{0};
        bool loadSigned{false};
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
        uint64_t unusedCount{0};
        uint64_t collisionCount{0};
        DependencyBenefit benefit;
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
        bool loadSigned{false};
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
    uint64_t consumerGeneration{0};

    static constexpr unsigned AddressTableWays = 4;
    static constexpr unsigned SamplerEntries = 256;
    static constexpr unsigned SamplerSets =
        SamplerEntries / AddressTableWays;
    static constexpr unsigned MetaEntries = 1024;
    static constexpr unsigned MetaSets = MetaEntries / AddressTableWays;
    // A 32-entry PC-pair trust table plus a 16-bit rank in each existing
    // Sampler entry uses about 672 bytes of new predictor state.  It protects
    // representative exact pairs across long scans without adding a request
    // path or widening dynamic-instruction metadata.
    static constexpr unsigned PairHintEntries = 32;
    static constexpr unsigned PairHintSets =
        PairHintEntries / AddressTableWays;
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
        uint16_t reservoirRank{UINT16_MAX};
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
        bool probation{false};
        uint8_t rrpv{3};
        uint32_t lastTrainEpoch{0};
        uint32_t lastUsefulEpoch{0};
        uint32_t probePhase{0};
        uint8_t unusedStreak{0};
    };
    struct PairHintEntry
    {
        bool valid{false};
        uint32_t keyHash{0};
        uint8_t temporalConf{0};
        uint8_t rrpv{3};
    };
    std::array<std::array<SamplerEntry, AddressTableWays>, SamplerSets>
        samplerTable{};
    std::array<lldp::PLRU<AddressTableWays>, SamplerSets>
        samplerReplacement{};
    std::array<std::array<MetaEntry, AddressTableWays>, MetaSets>
        metaTable{};
    std::array<lldp::PLRU<AddressTableWays>, MetaSets> metaReplacement{};
    std::array<std::array<PairHintEntry, AddressTableWays>, PairHintSets>
        pairHintTable{};
    uint64_t generation{0};
    uint64_t version{0};
    std::deque<Training> input;
    static constexpr unsigned TlbFilterEntries = 64;
    std::deque<Addr> tlbFilter;
    std::unordered_set<Addr> tlbFilterSet;
    std::unordered_map<Addr, Addr> tlbFilterTranslations;
    uint32_t tableEpoch{0};
    struct CandidateOwner : CandidateLifecycle
    {
        unsigned row;
        unsigned col;
        uint64_t generation;
        uint64_t consumerGeneration;
        Addr consumerPC;
        PrefetchSourceType source;
        bool meta{false};
        unsigned metaSet{0};
        unsigned metaWay{0};
        uint32_t metaGeneration{0};
        Addr feedbackPC{0};
        PrefetchSourceType feedbackSource{PrefetchSourceType::PF_NONE};
        PrefetchQualityControl::Handle qualityHandle{};
        bool hasQualityHandle{false};
        bool metaReserved{false};
        bool metaSpentToken{false};
        bool hasDependency{false};
        Addr producerPC{0}, candidateAddress{0};
        ContextID context{InvalidContextID};
    };
    CandidateTable<CandidateOwner> candidateOwners;
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
    const bool enableSpatialFeedback;
    const unsigned maxLldpcChainDepth;
    const bool enableQualityControl;
    const bool enableLldpcQuality;
    const bool enableLldpsFeedbackQuality;
    const bool enableLldpShadow;
    const bool enableLldpAdmission;
    const bool enableDemandPairTraining;
    const bool enableLldtBenefit;
    const bool enableTemporalRecovery;
    const unsigned temporalProbeInterval;
    const unsigned pairTrustThreshold;
    const unsigned samplerRetentionEpochs;
    DemandPairHistory<> demandPairHistory;
    const PrefetchQualityControl::Policy qualityPolicy;
    const PrefetchQualityControl::Policy lldpAdmissionPolicy;
    const PrefetchQualityControl::Policy lldpsFeedbackPolicy;
    PrefetchQualityControl qualityControl;
    PrefetchQualityControl lldpAdmissionControl;
    std::array<SpatialFeedback, 32> feedbackSlots{};
    std::array<uint32_t, 32> feedbackQualityGenerations{};
    uint64_t feedbackGeneration{0};
    const bool enableCandidateTrace;
    const unsigned traceSampleInterval;
    const Addr traceProducerPC;
    std::ofstream candidateTrace;
    BaseCPU *trainingCPU;
    SpatialFeedbackHandler spatialFeedbackHandler;

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
    class CommitListener : public ProbeListenerArgBase<o3::DynInstPtr>
    {
        LLDPrefetcher &parent;
      public:
        CommitListener(LLDPrefetcher &p, std::string name)
            : ProbeListenerArgBase(std::move(name)), parent(p) {}
        void notify(const o3::DynInstPtr &inst) override
        { parent.observeCommittedLoad(inst); }
    };
    ProbeListenerPtr<CommitListener> commitListener;
    void observeCommittedLoad(const o3::DynInstPtr &inst);

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
            returnedHints, staleHints, staleHintConsumers;
        statistics::Scalar candidates, filtered, duplicates, unsupported;
        statistics::Scalar samplerOutputsToMeta, metaTableHits,
            metaTablePrefetches, samplerValidEntries, metaValidEntries;
        statistics::Scalar samplerTargetMismatch, samplerRepromotions,
            metaInvalidations, metaTargetSwitches, metaTokenStalls,
            metaFallbacks, samplerReservoirAdmissions,
            samplerReservoirBypasses, pairTrustPromotions,
            pairTrustReplacements, pairTrustProbes;
        statistics::Scalar spatialFeedbackSignals, spatialFeedbackValid;
        statistics::Scalar chainTriggerAccepted, chainStoppedBySource,
            chainStoppedByDepth;
        statistics::Scalar qualityRejected, qualityBoosts, qualityRevokes;
        statistics::Scalar qualityEvictionRevokes, qualityPressureRevokes;
        statistics::Scalar ownerFull, ownerCurrent, ownerPeak,
            ownerStaleFeedback, ownerDuplicateIssue;
        statistics::Vector terminalBySource, demandMergedBySource,
            cacheCollisionBySource, pfMshrCollisionBySource,
            demandMshrCollisionBySource, wbCollisionBySource,
            queueDroppedBySource;
        statistics::Vector qualityDecisions, lldpShadowDecisions;
        statistics::Scalar lldpAdmissionRejected, feedbackStaleGeneration;
        statistics::Scalar demandPairObserved, demandPairMissing,
            demandPairTrained, replayPairObserved;
        statistics::Scalar metaRecoveryProbes, samplerStaleAdmissions,
            benefitVictimChanges;
        statistics::Vector pfqCurrent, pfqPeak, pfqOccupancySamples;
        statistics::Scalar pfqSamples;
        statistics::Scalar lldpDuplicateRejected;
        statistics::Vector pairObserved, pairTrustedCount, sampleRetained,
            metaAllocated, metaHit, metaAdmissionReject;
        statistics::Vector consumerUnused, consumerCollision;
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
    unsigned pairHintSet(uint32_t key_hash) const;
    unsigned pairHintVictim(unsigned set);
    static uint32_t pairHintHash(Addr producer_pc, Addr consumer_pc,
                                 ContextID context);
    static uint16_t samplerRank(Addr addr_p, Addr producer_pc,
                                Addr consumer_pc, ContextID context);
    bool pairTrusted(Addr producer_pc, Addr consumer_pc,
                     ContextID context) const;
    void recordPairEvidence(Addr producer_pc, Addr consumer_pc,
                            ContextID context);
    void trainAddressPair(Addr addr_p, Addr addr_c, Addr producer_pc,
                          Addr consumer_pc, ContextID context);
    void updateMetaTable(const SamplerEntry &sample);
    void installTrustedMeta(Addr addr_p, Addr addr_c, Addr producer_pc,
                            Addr consumer_pc, ContextID context);
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
    void emitSpatialFeedback(const PacketPtr &demand, bool valid);
    void sendSpatialFeedback(PrefetchQualityControl::Handle handle, bool valid);
    PrefetchQualityControl::Handle touchQuality(const PrefetchQualityControl::Key &key);
    unsigned qualityPressure() const;
    void revokePressureBoosts();
    void observeQuality(uint64_t candidate_id,
                        PrefetchQualityControl::Outcome outcome);
    bool finishCandidate(uint64_t candidate_id,
                         PrefetchQualityControl::Outcome outcome, int meta_result);
    void traceCandidate(uint64_t id, const char *event, const CandidateOwner &owner);
    void samplePfq();
    PrefetchQualityControl::Key qualityKey(PrefetchSourceType source,
                                           Addr producer_pc, Addr consumer_pc,
                                           ContextID context) const;
    bool queueCandidate(const PacketPtr &demand, const lldp::Hint &hint,
                        Addr addr_p, Addr addr_c,
                        PrefetchSourceType source,
                        std::optional<unsigned> consumer,
                        std::optional<MetaHit> meta_hit = std::nullopt,
                        std::optional<uint8_t> data_offset = std::nullopt);
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
                     uint64_t candidate_id, bool has_demand = false) override;
    void pfHitInWB(PrefetchSourceType source) override;
    void pfHitInWB(PrefetchSourceType source,
                   uint64_t candidate_id) override;
    void recordIssuedPrefetchStats(const PacketPtr &pkt) override;
    void notifyCandidateEvent(const Request::XsMetadata &metadata,
                              Addr address, unsigned event) override;
    // Report the result of an LLDPS trigger to the spatial prefetcher that
    // supplied the original demand.  The callback is intentionally value-only
    // so no packet or dynamic-instruction lifetime crosses this interface.
    void spatialFeedback(const SpatialFeedback &feedback);
    void setSpatialFeedbackHandler(
        SpatialFeedbackHandler handler)
    { spatialFeedbackHandler = std::move(handler); }
    void calculatePrefetch(const PrefetchInfo &,
                           std::vector<AddrPriority> &) override {}
    void addToQueue(std::list<DeferredPacket> &queue,
                    DeferredPacket &dpp) override;
    void rxHint(BaseMMU::Translation *dpp) override;
};
} // namespace prefetch
} // namespace gem5
#endif // __MEM_CACHE_PREFETCH_LLDP_HH__
