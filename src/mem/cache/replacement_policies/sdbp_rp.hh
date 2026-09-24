/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_RP_HH__

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
    void bindCache(unsigned num_sets);
    void observe(const PacketPtr pkt, unsigned set, Addr tag, bool hit);

    void invalidate(const std::shared_ptr<ReplacementData> &data) override;
    void touch(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData> &data) const override;
    void reset(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData> &data) const override;
    ReplaceableEntry *getVictim(const ReplacementCandidates &candidates) const override;
    ReplaceableEntry *getVictim(const ReplacementCandidates &candidates, const PacketPtr pkt) const;
    std::shared_ptr<ReplacementData> instantiateEntry() override;

  private:
    struct SDBPReplData : ReplacementData
    {
        uint64_t lastUse = 0;
        bool valid = false;
        bool dead = false;
    };

    static bool eligible(const PacketPtr pkt);
    bool predict(const PacketPtr pkt) const;
    SDBPCore core;
    const bool enableBypass;
    bool bound = false;
    mutable uint64_t sequence = 0;

    struct SDBPStats : statistics::Group
    {
        explicit SDBPStats(statistics::Group *parent);
        statistics::Scalar lookups;
        statistics::Scalar eligibleAccesses;
        statistics::Scalar excludedAccesses;
        statistics::Scalar noPcAccesses;
        statistics::Scalar eligibleMisses;
        statistics::Scalar samplerAccesses;
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
    };
    mutable SDBPStats stats;
};

}  // namespace replacement_policy
}  // namespace gem5

#endif  // __MEM_CACHE_REPLACEMENT_POLICIES_SDBP_RP_HH__
