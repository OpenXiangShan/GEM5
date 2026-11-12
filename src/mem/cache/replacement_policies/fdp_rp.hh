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
        FDPReplData(std::shared_ptr<fdp::Stack> set, unsigned w)
          : stack(set), way(w) {}
        fdp::Stack::Line &line() const { return stack->lines[way]; }
    };

    const unsigned numWays;
    unsigned entryCount = 0;
    std::shared_ptr<fdp::Stack> currentSet;
    fdp::Feedback feedback;

    mutable struct FDPStats : public statistics::Group
    {
        explicit FDPStats(statistics::Group *parent);
        statistics::Scalar demandMisses;
        statistics::Scalar pollutionMisses;
        statistics::Scalar demandEvictedByPrefetch;
        statistics::Scalar usefulEvictions;
        statistics::Scalar unusedPrefetchEvictions;
        statistics::Scalar usefulPrefetches;
        statistics::Scalar intervals;
        statistics::Scalar pollutionRate;
        statistics::Vector prefetchInsertions;
        statistics::Vector intervalPositions;
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
