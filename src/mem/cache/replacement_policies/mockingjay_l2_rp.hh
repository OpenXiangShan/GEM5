/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 *
 * Mockingjay reuse-distance replacement policy for an aligned L2 slice.
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_MOCKINGJAY_L2_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_MOCKINGJAY_L2_RP_HH__

#include <cstdint>
#include <memory>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/packet.hh"

namespace gem5
{

struct MockingjayL2RPParams;

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

/**
 * PC-based, per-set reuse-distance replacement policy from Mockingjay.
 *
 * Each SimObject instance owns its sampled cache and predictor. The aligned
 * L2 configuration creates one instance per slice, so no learning state is
 * shared between slices. sliceBits removes the interleaved slice selector
 * before sampled-cache address fields are extracted.
 */
class MockingjayL2 : public Base
{
  protected:
    static constexpr uint32_t NoPcSignature = UINT32_MAX;

    struct RdpEntry
    {
        bool valid;
        uint16_t reuseDistance;

        RdpEntry() : valid(false), reuseDistance(0) {}
    };

    struct SampledEntry
    {
        bool valid;
        uint64_t tag;
        uint32_t signature;
        uint16_t timestamp;

        SampledEntry()
          : valid(false), tag(0), signature(NoPcSignature), timestamp(0)
        {
        }
    };

    struct MockingjayReplData : ReplacementData
    {
        bool valid;
        unsigned setId;
        unsigned wayId;
        int16_t etr;

        MockingjayReplData()
          : valid(false), setId(0), wayId(0), etr(0)
        {
        }
    };

    const unsigned numSets;
    const unsigned numWays;
    const unsigned blockBits;
    const unsigned sliceBits;
    const unsigned historyMultiplier;
    const unsigned agingGranularity;
    const unsigned sampledSets;
    const unsigned sampledCacheSetsPerSet;
    const unsigned sampledCacheWays;
    const unsigned sampledTagBits;
    const unsigned rdpEntries;
    const unsigned temporalDifferenceThreshold;
    const unsigned scanThresholdMargin;
    const unsigned prefetchPenaltyPercent;
    const unsigned timestampBits;
    unsigned setBits;
    unsigned sampledCacheSetBits;

    uint16_t infRd;
    uint16_t maxRd;
    int16_t infEtr;
    uint16_t timestampModulo;
    uint64_t sampledTagMask;

    unsigned entryCount;
    mutable std::vector<RdpEntry> rdp;
    mutable std::vector<uint8_t> setClocks;
    mutable std::vector<uint16_t> sampledTimestamps;
    std::vector<int> sampledSetSlots;
    mutable std::vector<std::vector<SampledEntry>> sampledCache;
    std::vector<std::vector<MockingjayReplData*>> entriesBySet;

    mutable struct MockingjayStats : public statistics::Group
    {
        MockingjayStats(statistics::Group *parent);

        statistics::Scalar sampledHits;
        statistics::Scalar sampledMisses;
        statistics::Scalar reuseTrainings;
        statistics::Scalar scanTrainings;
        statistics::Scalar rdpLookups;
        statistics::Scalar rdpHits;
        statistics::Scalar rdpMisses;
        statistics::Scalar noPcSignatures;
        statistics::Scalar promotions;
        statistics::Scalar insertions;
        statistics::Scalar writebackInsertions;
        statistics::Scalar agingEvents;
        statistics::Scalar bypasses;
        statistics::Scalar positiveEtrVictims;
        statistics::Scalar negativeEtrVictims;
        statistics::Scalar invalidVictims;
    } stats;

    static uint64_t hash(uint64_t value);

    RdpEntry& rdpEntry(uint32_t signature) const;

    bool isSampledSet(unsigned set_id) const;
    bool isPrefetch(const PacketPtr pkt) const;
    uint32_t getSignature(const PacketPtr pkt, bool hit) const;
    uint16_t elapsed(uint16_t current, uint16_t previous) const;
    uint64_t sampledTag(Addr addr) const;
    unsigned sampledCacheIndex(unsigned set_id, Addr addr) const;
    uint16_t prefetchAdjustedDistance(uint16_t distance,
                                      const PacketPtr pkt) const;

    void trainReuse(uint32_t signature, uint16_t distance) const;
    void trainScan(uint32_t signature) const;
    void processSampledAccess(const MockingjayReplData &data,
                              const PacketPtr pkt, uint32_t signature) const;
    void processBypassedFill(unsigned set_id, const PacketPtr pkt,
                             uint32_t signature) const;
    int16_t predictEtr(uint32_t signature) const;
    void ageSet(unsigned set_id,
                const MockingjayReplData *accessed_data) const;
    ReplaceableEntry* selectVictim(
        const ReplacementCandidates& candidates) const;
    void recordVictim(const MockingjayReplData &data) const;

  public:
    typedef MockingjayL2RPParams Params;
    MockingjayL2(const Params &p);
    ~MockingjayL2() = default;

    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
        override;

    void touch(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;
    void touch(const std::shared_ptr<ReplacementData>& replacement_data) const
        override;

    void reset(const std::shared_ptr<ReplacementData>& replacement_data,
               const PacketPtr pkt) override;
    void reset(const std::shared_ptr<ReplacementData>& replacement_data) const
        override;

    ReplaceableEntry* getVictim(
        const ReplacementCandidates& candidates) const override;
    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates,
                                const PacketPtr pkt) const override;

    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_MOCKINGJAY_L2_RP_HH__
