/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 *
 * Sampled reuse-oriented Leeway replacement policy for classic L2 caches.
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_LEEWAY_L2_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_LEEWAY_L2_RP_HH__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/base.hh"
#include "mem/packet.hh"

namespace gem5
{

struct LeewayL2RPParams;

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

class LeewayL2 : public Base
{
  public:
    enum class Mode
    {
        LRU,
        NRU
    };

  protected:
    static constexpr uint32_t NoSig = 0;
    static constexpr uint32_t PrefetchSig = 1;
    static constexpr uint32_t WritebackSig = 2;
    static constexpr uint32_t SpecialSigs = 3;
    static constexpr int16_t BypassLd = -1;

    struct LDPTEntry
    {
        int16_t stableLd;
        uint8_t varianceConf;
        int8_t varianceDir;

        LDPTEntry() : stableLd(0), varianceConf(0), varianceDir(0) {}
    };

    struct LeewayL2ReplData : ReplacementData
    {
        bool valid;
        unsigned setId;
        unsigned wayId;
        bool isSampledSet;
        uint8_t nruVal;
        int16_t predictedLd;
        uint32_t signature;
        int16_t currentLd;

        LeewayL2ReplData();
    };

    const Mode mode;
    const unsigned numSets;
    const unsigned numWays;
    const unsigned ldptEntries;
    const unsigned sampleSets;
    const unsigned nruBits;
    const bool requirePcSignature;
    const unsigned ldIncreaseThreshold;
    const unsigned ldDecreaseThreshold;
    const uint8_t maxNruVal;
    const int16_t maxLd;

    unsigned entryCount;
    mutable std::vector<LDPTEntry> ldpt;
    std::vector<std::vector<LeewayL2ReplData*>> entriesBySet;

    mutable struct LeewayStats : public statistics::Group
    {
        LeewayStats(statistics::Group *parent);

        statistics::Scalar deadVictims;
        statistics::Scalar fallbackVictims;
        statistics::Scalar leaderTrainings;
        statistics::Scalar noPcSkipTrain;
        statistics::Scalar lruPromotions;
        statistics::Scalar nruAgingEvents;
        statistics::Scalar sampledSetRefills;
        statistics::Scalar nonSampledSetRefills;
    } stats;

    static Mode parseMode(const std::string &mode);
    bool isSampledSet(unsigned set_id) const;
    uint32_t getSignature(const PacketPtr pkt) const;
    LDPTEntry &getEntry(uint32_t sig) const;
    void updateEntry(uint32_t sig, int16_t current_ld) const;
    void train(const std::shared_ptr<LeewayL2ReplData> &data) const;
    void promote(const std::shared_ptr<LeewayL2ReplData> &data);
    void promoteLRU(LeewayL2ReplData &data);
    ReplaceableEntry *findOldest(const ReplacementCandidates &candidates) const;
    void ageNRUSet(const ReplacementCandidates &candidates) const;

  public:
    typedef LeewayL2RPParams Params;
    LeewayL2(const Params &p);
    ~LeewayL2() = default;

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

    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates) const
        override;

    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_LEEWAY_L2_RP_HH__
