#ifndef __MEM_CACHE_L2_CACHE_WRAPPER_HH__
#define __MEM_CACHE_L2_CACHE_WRAPPER_HH__

#include "mem/cache/CacheWrapper.hh"
#include "params/L2CacheWrapper.hh"

namespace gem5
{

class L2CacheWrapper : public CacheWrapper
{
  public:
    L2CacheWrapper(const L2CacheWrapperParams &p);
};

} // namespace gem5

#endif // __MEM_CACHE_L2_CACHE_WRAPPER_HH__
