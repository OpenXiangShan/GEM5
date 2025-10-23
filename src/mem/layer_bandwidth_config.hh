/**
 * @file
 * Layer bandwidth configuration object for XBar.
 */

#ifndef __MEM_LAYER_BANDWIDTH_CONFIG_HH__
#define __MEM_LAYER_BANDWIDTH_CONFIG_HH__

#include <string>

#include "params/LayerBandwidthConfig.hh"
#include "sim/sim_object.hh"

namespace gem5
{

/**
 * Layer bandwidth configuration object.
 * Specifies max transactions per cycle for a specific XBar layer.
 */
class LayerBandwidthConfig : public SimObject
{
  public:
    LayerBandwidthConfig(const LayerBandwidthConfigParams &p)
        : SimObject(p), direction(p.direction), portIndex(p.port_index),
          maxPerCycle(p.max_per_cycle)
    {}

    const std::string direction;  // "req" or "resp"
    const int portIndex;           // Port index (0-based)
    const unsigned maxPerCycle;    // Max transactions per cycle
};

} // namespace gem5

#endif // __MEM_LAYER_BANDWIDTH_CONFIG_HH__
