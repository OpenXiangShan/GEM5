#include "mem/cache/xs_l3/L3SlicedCacheAccessor.hh"

#include "mem/cache/xs_l3/L3CacheWrapper.hh"

namespace gem5
{

CacheAccessor*
L3SlicedCacheAccessor::getSlice(Addr addr) const
{
    return l3_wrapper->cache_accessors[l3_wrapper->getSliceId(addr)];
}

bool
L3SlicedCacheAccessor::inCache(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->inCache(addr, is_secure);
}

unsigned
L3SlicedCacheAccessor::level() const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return l3_wrapper->cache_accessors[0]->level();
}

bool
L3SlicedCacheAccessor::hasBeenPrefetched(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasBeenPrefetched(addr, is_secure);
}

bool
L3SlicedCacheAccessor::hasBeenPrefetched(Addr addr, bool is_secure, RequestorID requestor) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasBeenPrefetched(addr, is_secure, requestor);
}

bool
L3SlicedCacheAccessor::hasEverBeenPrefetched(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasEverBeenPrefetched(addr, is_secure);
}

Request::XsMetadata
L3SlicedCacheAccessor::getHitBlkXsMetadata(PacketPtr pkt)
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(pkt->getAddr())->getHitBlkXsMetadata(pkt);
}

bool
L3SlicedCacheAccessor::inMissQueue(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->inMissQueue(addr, is_secure);
}

bool
L3SlicedCacheAccessor::coalesce() const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return l3_wrapper->cache_accessors[0]->coalesce();
}

const uint8_t*
L3SlicedCacheAccessor::findBlock(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->findBlock(addr, is_secure);
}

} // namespace gem5