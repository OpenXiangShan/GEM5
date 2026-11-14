/*
 * Copyright (c) 2026 XiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_KPC_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_KPC_RP_HH__

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/kpc_sampler.hh"

namespace gem5
{

struct KPCReplStateParams;
struct KPCRPParams;

namespace replacement_policy
{

/** Per-core training state, shared by every slice of the private L2. */
class KPCReplState : public SimObject
{
  private:
    std::unique_ptr<KPCSampler> sampler;
    const uint8_t liveRRPV;
    struct KPCStateStats : public statistics::Group
    {
        KPCStateStats(statistics::Group *parent);
        statistics::Scalar demandAccesses;
        statistics::Scalar prefetchAccesses;
        statistics::Scalar samplerHits;
        statistics::Scalar samplerMisses;
        statistics::Scalar demandFirstReuse;
        statistics::Scalar prefetchFirstReuse;
        statistics::Scalar demandDead;
        statistics::Scalar prefetchDead;
        statistics::Scalar dghValue;
        statistics::Scalar pghValue;
    } stats;

  public:
    const unsigned numSets;
    const unsigned numWays;
    const unsigned blockSize;
    using Params = KPCReplStateParams;
    KPCReplState(const Params &p);

    void access(unsigned set, const PacketPtr pkt, bool prefetch);
    uint8_t insertionRRPV(bool pf) const
    {
        return sampler->insertionRRPV(pf) == 3 ? 3 : liveRRPV;
    }
};

/** KPC-R insertion and replacement, adapted from LLC to private L2. */
class KPC : public Base
{
  protected:
    struct KPCReplData : ReplacementData
    {
        uint8_t rrpv = 3;
        bool valid = false;
    };
    KPCReplState *const state;
    const unsigned numSlices;
    const unsigned sliceIndex;
    const bool promotePrefetchHits;
    const bool promoteWritebackHits;

    mutable struct KPCStats : public statistics::Group
    {
        KPCStats(statistics::Group *parent);
        statistics::Scalar demandInsertions0;
        statistics::Scalar demandInsertions1;
        statistics::Scalar demandInsertions2;
        statistics::Scalar demandInsertions3;
        statistics::Scalar prefetchInsertions0;
        statistics::Scalar prefetchInsertions1;
        statistics::Scalar prefetchInsertions2;
        statistics::Scalar prefetchInsertions3;
        statistics::Scalar writebackPromotions;
        statistics::Scalar demandPromotions;
        statistics::Scalar prefetchPromotions;
        statistics::Scalar prefetchNoPromotion;
    } stats;

    static bool isPrefetch(const PacketPtr pkt);

  public:
    using Params = KPCRPParams;
    KPC(const Params &p);

    void observeAccess(const PacketPtr pkt,
                       const BaseIndexingPolicy *indexing) override;
    void observePrefetch(const PacketPtr pkt,
                         const BaseIndexingPolicy *indexing,
                         const std::shared_ptr<ReplacementData> &data) override;
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

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_KPC_RP_HH__
