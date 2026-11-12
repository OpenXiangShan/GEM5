/*
 * Copyright (c) 2026
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ICP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ICP_RP_HH__

#include <vector>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"

namespace gem5
{

struct ICPRPParams;

namespace replacement_policy
{

/**
 * Feedback-directed LRU insertion using ICP's accuracy predictor and EPF.
 * Only insertion changes by default; first-use demotion is independently
 * selectable. Accuracy can be tracked per source or per source/PC bucket,
 * since gem5's composite prefetchers do not expose a common stream identifier.
 */
class ICP : public Base
{
  protected:
    struct ICPReplData : ReplacementData
    {
        unsigned set = 0;
        unsigned rank = 0; // Zero is MRU; the largest rank is LRU.
        bool valid = false;
        bool prefetched = false;
        bool predictedLow = false;
        Addr address = 0;
        bool secure = false;
        unsigned source = 0;
        unsigned predictor = 0;
    };

    struct Ghost
    {
        Addr address = 0;
        bool secure = false;
        bool valid = false;
        unsigned source = 0;
        unsigned predictor = 0;
        uint64_t sequence = 0;
    };

    const unsigned numSets;
    const unsigned numWays;
    const unsigned blockSize;
    const unsigned pcEntries;
    const unsigned counterMax;
    const unsigned threshold;
    const unsigned highPriority;
    const bool demoteOnFirstUse;
    const unsigned epfAssoc;
    const unsigned epfSets;
    unsigned entryCount = 0;
    mutable uint64_t sequence = 0;
    mutable std::vector<unsigned> accuracy;
    std::vector<std::vector<ICPReplData *>> sets;
    mutable std::vector<Ghost> ghosts;

    void position(ICPReplData &data, unsigned rank) const;
    void train(unsigned predictor, bool useful) const;
    unsigned sourceOf(const PacketPtr pkt) const;
    unsigned predictorOf(const PacketPtr pkt) const;
    unsigned ghostSet(Addr address, bool secure) const;
    void remember(const ICPReplData &data) const;
    void forget(Addr address, bool secure, bool demand) const;
    static bool isPrefetch(const PacketPtr pkt);
    static bool isDemand(const PacketPtr pkt);

    mutable struct ICPStats : public statistics::Group
    {
        explicit ICPStats(statistics::Group *parent);
        statistics::Scalar prefetchFills;
        statistics::Scalar pcPrefetchFills;
        statistics::Scalar fallbackPrefetchFills;
        statistics::Scalar highPriorityFills;
        statistics::Scalar lowPriorityFills;
        statistics::Scalar usefulPrefetches;
        statistics::Scalar unusedPrefetches;
        statistics::Scalar epfHits;
        statistics::Scalar epfReplacements;
        statistics::Scalar epfSuperseded;
        statistics::Scalar ignoredPrefetchHits;
        statistics::Vector fillsBySource;
        statistics::Vector lowFillsBySource;
        statistics::Vector usefulBySource;
        statistics::Vector epfHitsBySource;
    } stats;

  public:
    using Params = ICPRPParams;
    explicit ICP(const Params &p);

    void notifyAccess(const PacketPtr pkt, bool hit) override;
    void notifyPrefetchFill(const std::shared_ptr<ReplacementData> &data,
                            bool demand_merged) override;
    void invalidate(const std::shared_ptr<ReplacementData> &data) override;
    void touch(const std::shared_ptr<ReplacementData> &data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData> &data) const override;
    void reset(const std::shared_ptr<ReplacementData> &data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData> &data) const override;
    ReplaceableEntry *getVictim(
        const ReplacementCandidates &candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_ICP_RP_HH__
