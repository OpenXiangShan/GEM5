// Bounded degree feedback shared with the LLDP quality controller's slots.
#ifndef __MEM_CACHE_PREFETCH_SPATIAL_FEEDBACK_HH__
#define __MEM_CACHE_PREFETCH_SPATIAL_FEEDBACK_HH__

#include <array>
#include <cstdint>

#include "mem/request.hh"

namespace gem5::prefetch
{

struct SpatialFeedback
{
    PrefetchSourceType source{PrefetchSourceType::PF_NONE};
    Addr pc{0};
    ContextID context{InvalidContextID};
    unsigned slot{0};
    uint64_t generation{0};
    bool valid{false};
};

class SpatialFeedbackTable
{
  private:
    // These slots mirror the 32 quality entries, rather than adding a second
    // replacement policy that can leave a degree bit stuck in provider state.
    std::array<SpatialFeedback, 32> entries{};

  public:
    bool update(const SpatialFeedback &feedback)
    {
        if (feedback.slot >= entries.size() || !feedback.generation)
            return false;
        auto &entry = entries[feedback.slot];
        if (feedback.valid) {
            // Equal-generation revocation is a tombstone. An old boost must
            // not resurrect a revoked entry, including across context reuse.
            if (feedback.generation <= entry.generation)
                return false;
            entry = feedback;
        } else {
            if (entry.generation != feedback.generation ||
                entry.pc != feedback.pc || entry.context != feedback.context ||
                entry.source != feedback.source || !entry.valid)
                return false;
            entry.valid = false;
        }
        return true;
    }

    bool contains(Addr pc, ContextID context, PrefetchSourceType source) const
    {
        for (const auto &entry : entries)
            if (entry.valid && entry.pc == pc && entry.context == context &&
                entry.source == source)
                return true;
        return false;
    }
};

} // namespace gem5::prefetch
#endif
