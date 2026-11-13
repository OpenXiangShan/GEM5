/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_FDP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_FDP_RP_HH__

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/fdp_logic.hh"

namespace gem5
{

struct FDPRPParams;

namespace replacement_policy
{

class FDP : public Base
{
  protected:
    struct FDPReplData : ReplacementData
    {
        std::shared_ptr<fdp::Stack> stack;
        unsigned way;
        fdp::SampledInsertion::Team sample;
        unsigned source = 0;
        unsigned confidenceIndex = 0;
        FDPReplData(std::shared_ptr<fdp::Stack> set, unsigned w,
                    fdp::SampledInsertion::Team team)
          : stack(set), way(w), sample(team) {}
        fdp::Stack::Line &line() const { return stack->lines[way]; }
    };

    const unsigned numWays;
    const int midRank;
    unsigned entryCount = 0;
    std::shared_ptr<fdp::Stack> currentSet;
    fdp::Feedback feedback;
    const bool sourceFeedback;
    const bool sourceUtility;
    const unsigned pcEntries;
    const bool prefetchHitGuard;
    fdp::SourceConfidence confidence;
    const bool lruFallback;
    fdp::SampledInsertion insertion;

    void sampleDemandFill(const std::shared_ptr<FDPReplData> &data);

    mutable struct FDPStats : public statistics::Group
    {
        FDPStats(statistics::Group *parent, unsigned ways);
        statistics::Scalar demandMisses;
        statistics::Scalar pollutionMisses;
        statistics::Scalar filterAliasesRejected;
        statistics::Scalar demandEvictedByPrefetch;
        statistics::Scalar usefulEvictions;
        statistics::Scalar unusedPrefetchEvictions;
        statistics::Scalar usefulPrefetches;
        statistics::Scalar latePrefetchUses;
        statistics::Scalar ignoredPrefetchHits;
        statistics::Scalar pcIndexedPrefetches;
        statistics::Scalar sourceOnlyPrefetches;
        statistics::Scalar pcIndexedUseful;
        statistics::Scalar pcIndexedUnused;
        statistics::Scalar intervals;
        statistics::Scalar pollutionRate;
        statistics::Scalar fdpSampleFills;
        statistics::Scalar lruSampleFills;
        statistics::Scalar fallbackPrefetches;
        statistics::Scalar fallbackSelector;
        statistics::Vector prefetchInsertions;
        statistics::Vector prefetchInsertionRanks;
        statistics::Vector intervalPositions;
        statistics::Vector protectedBySource;
        statistics::Vector pollutionBySource;
        statistics::Vector usefulBySource;
    } fdpStats;

    static bool isPrefetch(const PacketPtr pkt);

  public:
    using Params = FDPRPParams;
    explicit FDP(const Params &p);
    void invalidate(const std::shared_ptr<ReplacementData> &data) override;
    void touch(const std::shared_ptr<ReplacementData> &data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData> &data) const override;
    void reset(const std::shared_ptr<ReplacementData> &data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData> &data) const override;
    void notifyDemandUse(const std::shared_ptr<ReplacementData> &data)
                         override;
    void notifyMiss(const PacketPtr pkt) override;
    void notifyEviction(const std::shared_ptr<ReplacementData> &data,
                        Addr addr, bool secure, const PacketPtr incoming)
                        override;
    ReplaceableEntry *getVictim(const ReplacementCandidates &candidates)
                               const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_FDP_RP_HH__
