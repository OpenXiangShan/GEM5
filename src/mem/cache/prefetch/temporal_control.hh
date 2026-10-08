// Recovery uses the existing Meta token state; it is not a second quality gate.
#ifndef __MEM_CACHE_PREFETCH_TEMPORAL_CONTROL_HH__
#define __MEM_CACHE_PREFETCH_TEMPORAL_CONTROL_HH__

#include <algorithm>
#include <cstdint>

namespace gem5::prefetch
{

struct TemporalControl
{
    // The address-training confidence always remains independent of outcomes.
    template <typename Entry>
    static bool admit(Entry &entry, bool recovery, unsigned interval, bool &probe)
    {
        probe = false;
        if ((entry.trainConf < 3 && !entry.probation) || entry.outstanding)
            return false;
        if (entry.qualityConf >= 2 && entry.tokens)
            return true;
        if (!recovery || !interval)
            return false;
        probe = entry.probePhase >= interval - 1;
        entry.probePhase = probe ? 0 : entry.probePhase + 1;
        return probe;
    }

    template <typename Entry>
    static void unused(Entry &entry, bool recovery)
    {
        if (recovery) {
            // Two consecutive unused completions drain a token. Useful/merge
            // feedback resets the streak; neither path erases training.
            entry.unusedStreak = std::min<unsigned>(3, entry.unusedStreak + 1);
            if (entry.unusedStreak >= 2) {
                entry.qualityConf = entry.qualityConf > 1 ? entry.qualityConf - 2 : 0;
                if (entry.tokens)
                    --entry.tokens;
            }
        } else {
            entry.qualityConf = entry.qualityConf > 1 ? entry.qualityConf - 2 : 0;
            entry.tokens = 0;
            if (!entry.qualityConf)
                entry.valid = false;
        }
        if (entry.timelyConf)
            --entry.timelyConf;
    }
};

// A small recency-weighted replacement benefit, separate from dependency
// confidence. Duplicate addresses supply no evidence of a wrong dependency.
struct DependencyBenefit
{
    uint8_t useful{0};
    uint8_t unused{0};
    uint8_t merged{0};

    void observe(unsigned outcome)
    {
        if (outcome > 2)
            return;
        if (unsigned(useful) + unused + merged >= 15) {
            useful = (useful + 1) / 2;
            unused = (unused + 1) / 2;
            merged = (merged + 1) / 2;
        }
        if (outcome == 0)
            ++useful;
        else if (outcome == 1)
            ++unused;
        else if (outcome == 2)
            ++merged;
    }

    int score() const { return 2 * int(useful) + merged - unused; }
};

} // namespace gem5::prefetch
#endif
