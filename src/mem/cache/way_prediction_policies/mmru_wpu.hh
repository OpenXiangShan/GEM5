/**
 * @file
 * Declaration of a MMRU way prediction unit (WPU).
 */

#ifndef __MEM_CACHE_WPU_MMRU_WPU_HH__
#define __MEM_CACHE_WPU_MMRU_WPU_HH__

#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "mem/cache/way_prediction_policies/base_wpu.hh"
#include "params/MMRUWpu.hh"

namespace gem5
{

namespace way_prediction_policy
{

/**
 * Multi-MRU or MMRU is a virtual "selective-DM" extension of
 * the Most Recently Used (MRU) way-prediction.
 */
class MMRUWpu : public BaseWpu
{
  private:
    const uint64_t n_tags;
    // MMRU way for each set (using uint8_t to save space)
    std::vector<std::vector<uint8_t>> mmru_ways;

  public:
    MMRUWpu(const MMRUWpuParams &params);

    uint8_t getPredictedWay(const PacketPtr pkt) override;
    void update(const PacketPtr pkt, const uint8_t way) override;
};

} // namespace way_prediction_policy
} // namespace gem5

#endif // __MEM_CACHE_WPU_MMRU_WPU_HH__
