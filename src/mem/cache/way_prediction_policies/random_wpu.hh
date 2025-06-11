/**
 * @file
 * Declaration of a Random way prediction unit (WPU).
 */

#ifndef __MEM_CACHE_WPU_RANDOM_WPU_HH__
#define __MEM_CACHE_WPU_RANDOM_WPU_HH__

#include <random>

#include "base/types.hh"
#include "mem/cache/way_prediction_policies/base_wpu.hh"
#include "params/RandomWpu.hh"

namespace gem5
{

namespace way_prediction_policy
{

/**
 * Random way predictor.
 * Randomly selects a way based on the cache associativity.
 */
class RandomWpu : public BaseWpu
{
  private:
    // Random number generator
    mutable std::random_device rd;
    mutable std::mt19937 gen;
    mutable std::uniform_int_distribution<> dis;

  public:
    RandomWpu(const RandomWpuParams &params);

    uint8_t getPredictedWay(const PacketPtr pkt) override;
    void update(const PacketPtr pkt, const uint8_t way) override;
};

} // namespace way_prediction_policy
} // namespace gem5

#endif // __MEM_CACHE_WPU_RANDOM_WPU_HH__
