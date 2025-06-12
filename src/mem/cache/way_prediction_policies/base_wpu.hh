/**
 * @file
 * Declaration of a base way prediction unit (WPU).
 */

#ifndef __MEM_CACHE_WPU_BASE_HH__
#define __MEM_CACHE_WPU_BASE_HH__

#include <cstdint>

#include "mem/packet.hh"
#include "params/BaseWpu.hh"
#include "sim/sim_object.hh"

namespace gem5
{

namespace way_prediction_policy
{

/**
 * A WPU is responsible for predicting the next cache way to be accessed.
 */
class BaseWpu : public SimObject
{
  protected:
    const bool use_virtual;
    const bool miss_train;
    const uint64_t assoc;
    const Cycles cycle_reduction;
    uint64_t set_shift;
    uint64_t set_mask;
    uint64_t tag_shift;
    uint64_t tag_mask;

    Addr getWpuAddr(const PacketPtr pkt) const;
    Addr getWpuSet(const Addr addr) const;
    Addr getWpuTag(const Addr addr) const;

  public:
    BaseWpu(const BaseWpuParams &params);
    virtual ~BaseWpu() = default;

    /**
     * Get the predicted way.
     *
     * @param pkt The packet causing the access.
     * @return The predicted way index.
     */
    virtual uint8_t getPredictedWay(const PacketPtr pkt) = 0;

    /**
     * Update the predictor's state directly.
     *
     * @param pkt The packet causing the access.
     * @param way The actual way that was accessed.
     */
    virtual void update(const PacketPtr pkt, const uint8_t way) = 0;

    /**
     * Try to update the predictor's state.
     * If the conditions are met, update the predictor's state.
     *
     * @param pkt The packet causing the access.
     * @param predicted_way The way that was predicted.
     * @param actual_way The way that was actually accessed.
     * @param hit Whether the access was a cache hit.
     * @return True if the WPU should be updated.
     */
    virtual void
    tryUpdateWpu(const PacketPtr pkt, const uint8_t predicted_way,
                    const uint8_t actual_way, const bool hit)
    {
        if (hit && (predicted_way != actual_way)) {
            update(pkt, actual_way);
        }
        if (!hit && miss_train) {
            // if using cache miss information to train the WPU,
            // WPU will have the ability to predict cache miss
            update(pkt, std::numeric_limits<uint8_t>::max());
        }
    }

    /**
     * Get the latency reduction in cycles when way prediction is correct.
     *
     * @return The latency reduction in cycles.
     */
    Cycles getCycleReduction() const { return cycle_reduction; }
};

} // namespace way_prediction_policy
} // namespace gem5

#endif // __MEM_CACHE_WPU_BASE_HH__
