#ifndef __CPU_VALUEPRED_CONSTANT_LVP_POLICY_HH__
#define __CPU_VALUEPRED_CONSTANT_LVP_POLICY_HH__

#include <cassert>
#include <cstdint>

namespace gem5
{

namespace valuepred
{

namespace constant_lvp
{

inline uint16_t
updatedCriticalCounter(uint16_t current, uint64_t blocked_cycles,
        uint64_t block_cycle_factor, uint16_t maximum)
{
    assert(block_cycle_factor > 0);
    assert(current <= maximum);

    if (blocked_cycles < block_cycle_factor) {
        return current == 0 ? 0 : current - 1;
    }

    const uint64_t increment = blocked_cycles / block_cycle_factor;
    const uint64_t room = maximum - current;
    return increment >= room ? maximum : current + increment;
}

inline uint16_t
effectiveConfidenceThreshold(uint16_t base_threshold, uint16_t critical,
        unsigned critical_bits)
{
    assert(critical_bits > 0 && critical_bits <= 16);
    const uint64_t reduction_step =
        base_threshold / (uint64_t{1} << critical_bits);
    const uint64_t reduction = critical * reduction_step;
    if (reduction >= base_threshold) {
        return 1;
    }
    return static_cast<uint16_t>(base_threshold - reduction);
}

inline uint64_t
replacementScore(uint16_t confidence, uint16_t critical)
{
    return static_cast<uint64_t>(confidence) * critical;
}

} // namespace constant_lvp
} // namespace valuepred
} // namespace gem5

#endif // __CPU_VALUEPRED_CONSTANT_LVP_POLICY_HH__
