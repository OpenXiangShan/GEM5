/**
 * @file
 * Declaration of a MRU way prediction unit (WPU).
 */

#ifndef __MEM_CACHE_WPU_MRU_WPU_HH__
#define __MEM_CACHE_WPU_MRU_WPU_HH__

#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "mem/cache/way_prediction_policies/base_wpu.hh"
#include "params/MRUWpu.hh"

namespace gem5
{

namespace way_prediction_policy
{

/**
 * Most Recently Used (MRU) way predictor.
 */
class MRUWpu : public BaseWpu
{
  private:
    // MRU way for each set (using uint8_t to save space)
    std::vector<uint8_t> mru_ways;

  public:
    MRUWpu(const MRUWpuParams &params);

    uint8_t getPredictedWay(const PacketPtr pkt) override;
    void update(const PacketPtr pkt, const uint8_t way) override;
};

} // namespace way_prediction_policy
} // namespace gem5

#endif // __MEM_CACHE_WPU_MRU_WPU_HH__
