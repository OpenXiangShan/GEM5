#include "mem/cache/L2CacheWrapper.hh"

#include "base/trace.hh"
#include "debug/L2CacheWrapper.hh"

namespace gem5
{

L2CacheWrapper::L2CacheWrapper(const L2CacheWrapperParams &p)
    : CacheWrapper(p)
{
}

} // namespace gem5
