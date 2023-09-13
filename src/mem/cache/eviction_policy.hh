#ifndef __MEM_CACHE_EVICTION_POLICY_BASE_HH__
#define __MEM_CACHE_EVICTION_POLICY_BASE_HH__

#include <memory>

#include "base/compiler.hh"
#include "mem/cache/cache_blk.hh"
#include "mem/packet.hh"
#include "params/EvictionPolicy.hh"
#include "sim/clocked_object.hh"
#include "sim/sim_object.hh"

namespace gem5
{

class EvictionPolicy : public ClockedObject
{
  public:
    EvictionPolicy(const EvictionPolicyParams &p);
    ~EvictionPolicy() = default;
    bool needWriteback(CacheBlk *blk);
  private:
    const int policy;
};

} // namespace gem5

#endif // __MEM_CACHE_EVICTION_POLICY_BASE_HH__