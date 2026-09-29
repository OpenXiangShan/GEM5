#ifndef __MEM_CACHE_PREFETCH_QUALITY_CONTROL_HH__
#define __MEM_CACHE_PREFETCH_QUALITY_CONTROL_HH__

#include <algorithm>
#include <array>
#include <cstdint>

#include "mem/request.hh"

namespace gem5::prefetch
{

inline bool
allowLldpChainTrigger(PrefetchSourceType source, unsigned chain_depth,
                      unsigned max_depth)
{
    if (!max_depth || chain_depth >= max_depth)
        return false;
    if (source == PrefetchSourceType::LLDPC)
        return chain_depth > 0;
    return chain_depth == 0 &&
        (source == PrefetchSourceType::LLDP ||
         source == PrefetchSourceType::LLDPS ||
         source == PrefetchSourceType::LLDPT);
}

// A bounded, four-way controller shared by LLDP-family mechanisms.  All
// updates are indexed by a generation-checked handle; a replaced PC cannot
// receive feedback from an old in-flight candidate.
class PrefetchQualityControl
{
  public:
    enum class Outcome { Useful, Unused, DemandMerged, Collision, Dropped };
    struct Key
    {
        PrefetchSourceType source{PrefetchSourceType::PF_NONE};
        Addr producerPC{0};
        Addr consumerPC{0};
        ContextID context{InvalidContextID};
    };
    struct Handle
    {
        unsigned index{0};
        uint32_t generation{0};
    };
    struct Policy
    {
        unsigned minSamples{8};
        unsigned minAccuracyPct{10};
        unsigned maxLatePct{25};
        unsigned probeInterval{16};
        unsigned maxOutstanding{4};
        unsigned maxPressurePct{75};
    };

  private:
    static constexpr unsigned Ways = 4;
    static constexpr unsigned Entries = 32;
    struct Entry
    {
        bool valid{false};
        Key key{};
        uint32_t generation{0};
        uint8_t rrpv{3};
        uint8_t issued{0};
        uint8_t outstanding{0};
        uint8_t useful{0};
        uint8_t unused{0};
        uint8_t merged{0};
        uint8_t collisions{0};
        uint8_t opportunities{0};
        bool boosted{false};
    };
    std::array<Entry, Entries> entries{};
    uint32_t nextGeneration{0};

    static bool same(const Key &a, const Key &b)
    {
        return a.source == b.source && a.producerPC == b.producerPC &&
            a.consumerPC == b.consumerPC && a.context == b.context;
    }
    static unsigned set(const Key &key)
    {
        const uint64_t hash = uint64_t(key.producerPC) ^
            (uint64_t(key.consumerPC) * UINT64_C(0x9e3779b97f4a7c15)) ^
            (uint64_t(key.context) << 8) ^ uint64_t(key.source);
        return (hash ^ (hash >> 23) ^ (hash >> 41)) & (Entries / Ways - 1);
    }
    Entry *entry(Handle handle)
    {
        auto &item = entries[handle.index];
        return item.valid && item.generation == handle.generation ? &item : nullptr;
    }
    const Entry *entry(Handle handle) const
    {
        const auto &item = entries[handle.index];
        return item.valid && item.generation == handle.generation ? &item : nullptr;
    }
    static unsigned completed(const Entry &item)
    { return item.useful + item.unused + item.merged; }
    static bool healthy(const Entry &item, const Policy &policy)
    {
        const unsigned samples = completed(item);
        if (samples < policy.minSamples)
            return false;
        const unsigned timely = item.useful + item.merged;
        return 100 * item.useful >= policy.minAccuracyPct * samples &&
            (!timely || 100 * item.merged <= policy.maxLatePct * timely);
    }

  public:
    Handle touch(const Key &key)
    {
        const unsigned first = set(key) * Ways;
        for (unsigned way = 0; way < Ways; ++way) {
            auto &item = entries[first + way];
            if (item.valid && same(item.key, key)) {
                item.rrpv = 0;
                return {first + way, item.generation};
            }
        }
        unsigned victim = first;
        for (unsigned way = 0; way < Ways; ++way) {
            if (!entries[first + way].valid) {
                victim = first + way;
                break;
            }
            if (entries[first + way].rrpv > entries[victim].rrpv)
                victim = first + way;
        }
        auto &item = entries[victim];
        item = {};
        item.valid = true;
        item.key = key;
        item.generation = ++nextGeneration;
        item.rrpv = 0;
        return {victim, item.generation};
    }

    bool admit(Handle handle, const Policy &policy, unsigned pressurePct)
    {
        auto *item = entry(handle);
        if (!item || pressurePct >= policy.maxPressurePct ||
            item->outstanding >= policy.maxOutstanding)
            return false;
        item->opportunities = std::min<unsigned>(255, item->opportunities + 1);
        if (healthy(*item, policy))
            return true;
        if (item->opportunities == 1)
            return true;
        return policy.probeInterval &&
            item->opportunities % policy.probeInterval == 0;
    }

    void issued(Handle handle)
    {
        if (auto *item = entry(handle)) {
            item->issued = std::min<unsigned>(63, item->issued + 1);
            item->outstanding = std::min<unsigned>(15, item->outstanding + 1);
        }
    }

    void observe(Handle handle, Outcome outcome)
    {
        auto *item = entry(handle);
        if (!item)
            return;
        if (item->outstanding)
            --item->outstanding;
        if (completed(*item) >= 60) {
            item->useful = (item->useful + 1) / 2;
            item->unused = (item->unused + 1) / 2;
            item->merged = (item->merged + 1) / 2;
            item->collisions = (item->collisions + 1) / 2;
        }
        switch (outcome) {
          case Outcome::Useful: ++item->useful; break;
          case Outcome::Unused: ++item->unused; break;
          case Outcome::DemandMerged: ++item->merged; break;
          case Outcome::Collision: ++item->collisions; break;
          case Outcome::Dropped: break;
        }
    }

    // Returns +1 to boost, -1 to revoke, 0 if unchanged.
    int updateBoost(Handle handle, const Policy &policy)
    {
        auto *item = entry(handle);
        if (!item)
            return 0;
        const bool good = item->useful >= 2 && healthy(*item, policy);
        if (good == item->boosted)
            return 0;
        item->boosted = good;
        return good ? 1 : -1;
    }

    bool valid(Handle handle) const { return entry(handle) != nullptr; }
    unsigned outstanding(Handle handle) const
    {
        const auto *item = entry(handle);
        return item ? item->outstanding : 0;
    }
};

} // namespace gem5::prefetch
#endif
