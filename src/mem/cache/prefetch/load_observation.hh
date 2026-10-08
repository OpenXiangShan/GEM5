// Per-dynamic-load observation, independent of prefetch policy decisions.
#ifndef __MEM_CACHE_PREFETCH_LOAD_OBSERVATION_HH__
#define __MEM_CACHE_PREFETCH_LOAD_OBSERVATION_HH__

#include <array>
#include <cstdint>

namespace gem5::prefetch
{

struct LoadObservation
{
    std::array<uint64_t, 8> lines{};
    unsigned fragments{0};
    uint64_t hitSources{0};
    bool sawMiss{false};
    bool overflow{false};
    bool woken{false};

    void observe(uint64_t line, bool hit, unsigned source)
    {
        for (unsigned i = 0; i < fragments; ++i)
            if (lines[i] == line)
                return; // The first tag outcome survives a replay.
        if (fragments == lines.size()) {
            overflow = true;
            return;
        }
        lines[fragments++] = line;
        sawMiss |= !hit;
        if (hit && source < 64)
            hitSources |= uint64_t(1) << source;
    }

    bool complete(unsigned expected) const
    { return !overflow && expected && fragments == expected; }

    bool allHit(unsigned expected) const
    { return complete(expected) && !sawMiss; }
};

} // namespace gem5::prefetch
#endif
