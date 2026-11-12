/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_RP_HH__

#include <functional>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/sdbp_core.hh"

namespace gem5
{

struct SDBPRPParams;

namespace replacement_policy
{

class SDBP : public Base
{
  public:
    using Params = SDBPRPParams;
    explicit SDBP(const Params &p);
    void startup() override;
    void resetStats() override;
    void bindCache(unsigned num_sets);
    using L1CopyProbe = std::function<bool(const ReplaceableEntry *)>;
    void setL1CopyProbe(L1CopyProbe probe);
    void observe(const PacketPtr pkt, unsigned set, Addr tag, bool hit);
    void observeFill(const PacketPtr pkt, unsigned set, Addr tag);
    void observeLocalPrefetch(
        const PacketPtr pkt,
        const std::shared_ptr<ReplacementData> &data);
    void notifyReplacementEviction(
        const std::shared_ptr<ReplacementData> &data);
    bool transferEntry(const std::shared_ptr<ReplacementData> &src,
                       const std::shared_ptr<ReplacementData> &dest);

    void invalidate(const std::shared_ptr<ReplacementData> &data) override;
    void touch(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData> &data) const override;
    void reset(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData> &data) const override;
    ReplaceableEntry *getVictim(const ReplacementCandidates &candidates) const override;
    ReplaceableEntry *getVictim(const ReplacementCandidates &candidates, const PacketPtr pkt) const;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
    bool isObservationOnly() const { return observationOnly; }

    // Diagnostic for SDBP metadata only: 1 for live, 2 for dead.
    static unsigned victimClass(const std::shared_ptr<ReplacementData> &data)
    {
        const auto entry = std::static_pointer_cast<SDBPReplData>(data);
        return entry->dead ? 2 : 1;
    }

  private:
    struct SDBPReplData : ReplacementData
    {
        uint64_t lastUse = 0;
        uint64_t signature = 0;
        Tick lastTouchTick = 0;
        SDBPCore::RequestType signatureType = SDBPCore::RequestType::Demand;
        SDBPCore::RequestType fillType = SDBPCore::RequestType::Demand;
        bool valid = false;
        bool dead = false;
        bool signatureValid = false;
        bool prefetchOrigin = false;
        bool prefetchDemandUsed = false;
    };

    static bool eligible(const PacketPtr pkt);
    static bool isPrefetch(const PacketPtr pkt);
    static bool isDemand(const PacketPtr pkt);
    static bool hasPc(const PacketPtr pkt);
    static Addr requestPc(const PacketPtr pkt);
    static SDBPCore::RequestType requestType(const PacketPtr pkt);
    void sample(const PacketPtr pkt, unsigned set, Addr tag);
    bool predict(const PacketPtr pkt) const;
    void updateLine(const std::shared_ptr<ReplacementData> &data,
                    const PacketPtr pkt, bool updateRecency);
    void startPrediction(SDBPReplData &entry, const PacketPtr pkt);
    void resolvePrediction(SDBPReplData &entry, bool actualDead,
                           SDBPCore::RequestType reuseType =
                               SDBPCore::RequestType::Demand);
    void censorPrediction(SDBPReplData &entry);
    SDBPCore core;
    const bool enableBypass;
    const bool l1CopyProtection;
    const bool observationOnly;
    bool bound = false;
    L1CopyProbe l1CopyProbe;
    mutable uint64_t sequence = 0;
    std::vector<std::weak_ptr<SDBPReplData>> observationEntries;

    struct SDBPStats : statistics::Group
    {
        explicit SDBPStats(statistics::Group *parent);
        statistics::Scalar lookups;
        statistics::Scalar eligibleAccesses;
        statistics::Scalar excludedAccesses;
        statistics::Scalar noPcAccesses;
        statistics::Scalar eligibleMisses;
        statistics::Scalar samplerAccesses;
        statistics::Scalar samplerBypassAccesses;
        statistics::Scalar samplerHits;
        statistics::Scalar samplerEvictions;
        statistics::Scalar liveTraining;
        statistics::Scalar deadTraining;
        statistics::Scalar samplerDeadHits;
        statistics::Scalar samplerDeadEvictions;
        statistics::Scalar predictionQueries;
        statistics::Scalar deadPredictions;
        statistics::Scalar deadHits;
        statistics::Scalar fills;
        statistics::Scalar invalidVictims;
        statistics::Scalar deadVictims;
        statistics::Scalar nonLruDeadVictims;
        statistics::Scalar lruVictims;
        statistics::Scalar bypasses;
        statistics::Scalar l1CopyChecks;
        statistics::Scalar l1CopyProtectedDead;
        statistics::Scalar l1CopyLruFallbacks;
        statistics::Scalar fullCachePredictions;
        statistics::Scalar fullCacheResolved;
        statistics::Scalar fullCacheCensored;
        statistics::Scalar fullCachePending;
        statistics::Vector samplerAccessesBySource;
        statistics::Vector samplerHitsBySource;
        statistics::Vector liveTrainingBySource;
        statistics::Vector deadTrainingBySource;
        statistics::Vector predictionQueriesBySource;
        statistics::Vector deadPredictionsBySource;
        statistics::Vector fullCachePredictionsBySource;
        statistics::Vector fullCacheCarriedInBySource;
        statistics::Vector fullCacheResolvedBySource;
        statistics::Vector fullCacheCensoredBySource;
        statistics::Vector fullCacheDeadTruePositiveBySource;
        statistics::Vector fullCacheDeadFalsePositiveBySource;
        statistics::Vector fullCacheLiveTrueNegativeBySource;
        statistics::Vector fullCacheLiveFalseNegativeBySource;
        statistics::Vector fullCachePendingBySource;
        statistics::Vector fullCacheDemandReuseBySource;
        statistics::Vector fullCachePrefetchReuseBySource;
        statistics::Vector fullCachePredDeadDemandReuseBySource;
        statistics::Vector fullCachePrefetchFillsBySource;
        statistics::Vector fullCachePrefetchCarriedInBySource;
        statistics::Vector fullCachePrefetchPendingBySource;
        statistics::Vector fullCachePrefetchDemandUsesBySource;
        statistics::Vector fullCachePrefetchUnusedEvictionsBySource;
        statistics::Vector fullCachePrefetchUnusedInvalidationsBySource;
    };
    mutable SDBPStats stats;
};

}  // namespace replacement_policy
}  // namespace gem5

#endif  // __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_RP_HH__
