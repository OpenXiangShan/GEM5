/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_PA_MAIP_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_PA_MAIP_RP_HH__

#include <memory>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/cache/replacement_policies/pa_maip_logic.hh"
#include "params/AMPMHintTable.hh"
#include "params/PAMAIPRP.hh"

namespace gem5
{
namespace replacement_policy
{

class AMPMHintTable : public SimObject
{
  private:
    std::unique_ptr<pa_maip::AccessMap> maps;
    const bool externalHints;
    struct HintStats : statistics::Group
    {
        explicit HintStats(statistics::Group *parent);
        statistics::Vector demandStates;
        statistics::Vector prefetchStates;
    } stats;

  public:
    using Params = AMPMHintTableParams;
    explicit AMPMHintTable(const Params &p);
    void observe(const PacketPtr pkt);
    void capture(const PacketPtr pkt, uint8_t state);
    void recordPrefetchHint(uint8_t state);
    bool isExternal() const { return externalHints; }
    bool getHint(const PacketPtr pkt, pa_maip::State &state) const;
};

/** L2 adaptation of UMOA PA-MAIP's map-state insertion and promotion rules.
 * Implements Table 2's normal-IP column; IP bypass and DRAM scheduling are
 * outside this policy. Existing prefetch generation is left unchanged.
 */
class PAMAIP : public Base
{
  private:
    struct PAMAIPReplData : ReplacementData, pa_maip::Line {};
    AMPMHintTable *const hints;
    const unsigned lowInsertPeriod;
    const bool suppressFirstHit;
    const bool protectDemand;
    unsigned lowCounter = 0;
    struct PolicyStats : statistics::Group
    {
        explicit PolicyStats(statistics::Group *parent);
        statistics::Vector insertions;
        statistics::Scalar prefetchFills;
        statistics::Scalar prefetchHintFills;
        statistics::Scalar firstUses;
        statistics::Scalar suppressedFirstUses;
        statistics::Scalar prefetchHits;
        statistics::Scalar promotions;
        statistics::Scalar fillConsumptions;
        statistics::Scalar missingHints;
        statistics::Scalar protectedDemandFills;
    } stats;

  public:
    using Params = PAMAIPRPParams;
    explicit PAMAIP(const Params &p);
    void notifyAccess(const PacketPtr pkt) override;
    void notifyDataUse(const std::shared_ptr<ReplacementData>& data,
                       const PacketPtr pkt) override;
    void invalidate(const std::shared_ptr<ReplacementData>& data) override;
    void touch(const std::shared_ptr<ReplacementData>& data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData>& data) const override;
    void reset(const std::shared_ptr<ReplacementData>& data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData>& data) const override;
    ReplaceableEntry *getVictim(
        const ReplacementCandidates& candidates) const override;
    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5
#endif
