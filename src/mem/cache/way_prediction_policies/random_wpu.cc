#include "mem/cache/way_prediction_policies/random_wpu.hh"

#include <limits>

namespace gem5
{

namespace way_prediction_policy
{

RandomWpu::RandomWpu(const RandomWpuParams &params)
    : BaseWpu(params), gen(rd()), dis(1, assoc + 1)
{
}

uint8_t
RandomWpu::getPredictedWay(const PacketPtr pkt)
{
    // Generate random number in range [1, assoc+1]
    int random_value = dis(gen);

    // If random value <= assoc, return it as predicted way (0-indexed)
    if (random_value <= static_cast<int>(assoc)) {
        return static_cast<uint8_t>(random_value - 1);  // Convert to 0-indexed
    } else {
        // If random value == assoc+1, return max value to indicate miss
        return std::numeric_limits<uint8_t>::max();
    }
}

void
RandomWpu::update(const PacketPtr pkt, const uint8_t way)
{
    // Random predictor doesn't need to update based on previous access patterns
    // This function is required by the interface but doesn't need to do anything
}

} // namespace way_prediction_policy
} // namespace gem5
