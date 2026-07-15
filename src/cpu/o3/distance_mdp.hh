#ifndef __CPU_O3_DISTANCE_MDP_HH__
#define __CPU_O3_DISTANCE_MDP_HH__

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "base/types.hh"
#include "cpu/o3/mdp.hh"

namespace gem5
{
namespace o3
{

constexpr unsigned
distanceMdpLog2(unsigned value)
{
    return value <= 1 ? 0 : 1 + distanceMdpLog2(value >> 1);
}

class DistanceMDP
{
  public:
    static constexpr unsigned NumEntries = 64;
    static constexpr unsigned TagBits = 14;
    static constexpr unsigned CounterBits = 7;
    static constexpr unsigned DistanceBits = 7;
    static constexpr unsigned VAddrBits = 39;
    static constexpr uint8_t MaxCounter = (1U << CounterBits) - 1;
    static constexpr uint8_t MaxDistance = (1U << DistanceBits) - 1;
    static constexpr uint64_t StrictTimeout = 10000;

    struct Entry
    {
        bool valid = false;
        uint16_t tag = 0;
        uint8_t counter = 0;
        bool hasDistance = false;
        bool multiDistance = false;
        bool waitAllStore = false;
        uint8_t distance = 0;
        uint64_t strictExpireCycle = 0;
    };

    struct Prediction
    {
        bool hit = false;
        bool strictExpired = false;
        unsigned entryIndex = 0;
        uint16_t tag = 0;
        uint8_t counter = 0;
        bool hasDistance = false;
        bool multiDistance = false;
        bool waitAllStore = false;
        uint8_t distance = 0;
    };

    enum class TrainAction
    {
        InvalidDistance,
        AllocateInvalid,
        AllocateEviction,
        StrictUpgrade,
        StrictRefresh,
    };

    struct TrainResult
    {
        TrainAction action = TrainAction::InvalidDistance;
        bool strictExpired = false;
        bool strictFallback = false;
        bool multiDistance = false;
        bool distanceChanged = false;
        unsigned entryIndex = 0;
        uint16_t tag = 0;
        uint8_t distance = 0;
    };

    enum class FeedbackAction
    {
        Increment,
        Saturated,
        Decrement,
        ClearCounterZero,
        InvalidIndex,
        InvalidEntry,
        TagMismatch,
    };

    struct FeedbackResult
    {
        FeedbackAction action = FeedbackAction::InvalidIndex;
        unsigned entryIndex = 0;
        uint16_t tag = 0;
        uint8_t oldCounter = 0;
        uint8_t newCounter = 0;
    };

    static uint16_t hash(Addr pc);
    static std::optional<uint8_t> encodeDistance(
        size_t load_boundary, size_t store_index);
    static std::optional<size_t> decodeTarget(
        size_t load_boundary, unsigned distance);

    Prediction lookup(Addr pc, uint64_t result_cycle);
    bool commitLookup(unsigned entry_index, uint16_t tag);
    TrainResult train(Addr load_pc, size_t load_boundary,
                      size_t store_index, uint64_t cycle);
    FeedbackResult feedback(unsigned entry_index, uint16_t tag,
                            MDPFeedbackSource source);

    void clear();
    unsigned occupancy() const;

    const Entry &entry(unsigned index) const;
    unsigned replacementVictim() const;

  private:
    static_assert(NumEntries > 0 && (NumEntries & (NumEntries - 1)) == 0,
                  "DistanceMDP entries must be a power of two");
    static constexpr unsigned PlruBits = NumEntries - 1;
    static constexpr unsigned PlruLevels = distanceMdpLog2(NumEntries);

    std::array<Entry, NumEntries> entries{};
    std::bitset<PlruBits> plru;
    unsigned validEntries = 0;

    std::optional<unsigned> find(uint16_t tag) const;
    bool expireStrict(Entry &entry, uint64_t cycle);
    void touch(unsigned index);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_DISTANCE_MDP_HH__
