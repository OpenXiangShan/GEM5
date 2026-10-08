// Bounded committed-load address history for genuine dependency-pair evidence.
#ifndef __MEM_CACHE_PREFETCH_DEMAND_PAIR_HISTORY_HH__
#define __MEM_CACHE_PREFETCH_DEMAND_PAIR_HISTORY_HH__

#include <array>
#include <cstdint>
#include <optional>

namespace gem5::prefetch
{

template <unsigned Capacity = 2048>
class DemandPairHistory
{
    static_assert(Capacity && !(Capacity & (Capacity - 1)));
    struct Entry
    {
        uint64_t sequence{0};
        uint64_t pc{0};
        uint64_t address{0};
        int context{-1};
        bool valid{false};
    };
    std::array<Entry, Capacity> entries{};

  public:
    void record(uint64_t sequence, uint64_t pc, uint64_t address, int context)
    {
        entries[sequence & (Capacity - 1)] = {sequence, pc, address, context, true};
    }

    std::optional<uint64_t> lookup(uint64_t sequence, uint64_t pc, int context) const
    {
        const auto &entry = entries[sequence & (Capacity - 1)];
        if (!entry.valid || entry.sequence != sequence || entry.pc != pc ||
            entry.context != context)
            return std::nullopt;
        return entry.address;
    }
};

} // namespace gem5::prefetch
#endif
