#ifndef __MEM_CACHE_WAY_PREDICTION_POLICIES_INDEX_MRU_WPU_HH__
#define __MEM_CACHE_WAY_PREDICTION_POLICIES_INDEX_MRU_WPU_HH__

#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "mem/cache/way_prediction_policies/base_wpu.hh"
#include "params/IndexMRUWpu.hh"

namespace gem5
{
namespace way_prediction_policy
{

/**
 * Index-MRU way predictor.
 */
class IndexMRUWpu : public BaseWpu
{
  private:
    class waypreEntry : public TaggedEntry
    {
      public:
        Addr index;
        Addr way;
        waypreEntry() : TaggedEntry(), index(0), way(0) {}
        void _setSecure(bool is_secure)
        {
            if (is_secure)
                TaggedEntry::setSecure();
        }
    };
    AssociativeSet<waypreEntry> indexWayPreTable;

  public:
    IndexMRUWpu(const IndexMRUWpuParams &p);

    uint8_t getPredictedWay(const PacketPtr pkt) override;
    void update(const PacketPtr pkt, const uint8_t way) override;
};

} // namespace way_prediction_policy
} // namespace gem5

#endif // __MEM_CACHE_WAY_PREDICTION_POLICIES_INDEX_MRU_WPU_HH__
