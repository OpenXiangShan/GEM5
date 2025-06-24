/**
 * @file
 * Declaration of a UTag way prediction unit (WPU).
 */

#ifndef __MEM_CACHE_WPU_UTAG_WPU_HH__
#define __MEM_CACHE_WPU_UTAG_WPU_HH__

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "mem/cache/way_prediction_policies/base_wpu.hh"
#include "params/UTagWpu.hh"

namespace gem5
{

namespace way_prediction_policy
{

/**
 * UTag-based way predictor.
 * This predictor uses a hashed address to create a "U-Tag" for indexing.
 */
class UTagWpu : public BaseWpu
{
  private:
    struct UTagEntry
    {
        bool valid;
        uint64_t utag;

        UTagEntry() : valid(false), utag(0) {}
    };
    std::vector<std::vector<UTagEntry>> utag_table;
    const uint64_t utag_bits;

    Addr hash(Addr addr) const;
    void invalidate(const PacketPtr pkt, const uint8_t way);

  public:
    UTagWpu(const UTagWpuParams &params);

    uint8_t getPredictedWay(const PacketPtr pkt) override;
    void update(const PacketPtr pkt, const uint8_t way) override;
    void tryUpdateWpu(const PacketPtr pkt, const uint8_t predicted_way,
                      const uint8_t actual_way, const bool hit) override;
};

} // namespace way_prediction_policy
} // namespace gem5

#endif // __MEM_CACHE_WPU_UTAG_WPU_HH__
