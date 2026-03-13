#include "mem/cache/xs_l2/SlicedCacheAccessor.hh"

#include "mem/cache/xs_l2/L2CacheWrapper.hh"
#include "mem/cache/xs_l2/L3CacheWrapper.hh"

namespace gem5
{

CacheAccessor*
SlicedCacheAccessor::getSlice(Addr addr) const
{
    return l2_wrapper->cache_accessors[l2_wrapper->getSliceId(addr)];
}

bool
SlicedCacheAccessor::inCache(Addr addr, bool is_secure) const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->inCache(addr, is_secure);
}

unsigned
SlicedCacheAccessor::level() const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return l2_wrapper->cache_accessors[0]->level();
}

bool
SlicedCacheAccessor::hasBeenPrefetched(Addr addr, bool is_secure) const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasBeenPrefetched(addr, is_secure);
}

bool
SlicedCacheAccessor::hasBeenPrefetched(Addr addr, bool is_secure, RequestorID requestor) const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasBeenPrefetched(addr, is_secure, requestor);
}

bool
SlicedCacheAccessor::hasEverBeenPrefetched(Addr addr, bool is_secure) const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasEverBeenPrefetched(addr, is_secure);
}

Request::XsMetadata
SlicedCacheAccessor::getHitBlkXsMetadata(PacketPtr pkt)
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(pkt->getAddr())->getHitBlkXsMetadata(pkt);
}

bool
SlicedCacheAccessor::inMissQueue(Addr addr, bool is_secure) const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->inMissQueue(addr, is_secure);
}

bool
SlicedCacheAccessor::coalesce() const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return l2_wrapper->cache_accessors[0]->coalesce();
}

const uint8_t*
SlicedCacheAccessor::findBlock(Addr addr, bool is_secure) const
{
    fatal_if(l2_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->findBlock(addr, is_secure);
}

CacheAccessor*
SlicedCacheAccessorL3::getSlice(Addr addr) const
{
    return l3_wrapper->cache_accessors[l3_wrapper->getSliceId(addr)];
}

bool
SlicedCacheAccessorL3::inCache(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->inCache(addr, is_secure);
}

unsigned
SlicedCacheAccessorL3::level() const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return l3_wrapper->cache_accessors[0]->level();
}

bool
SlicedCacheAccessorL3::hasBeenPrefetched(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasBeenPrefetched(addr, is_secure);
}

bool
SlicedCacheAccessorL3::hasBeenPrefetched(Addr addr, bool is_secure, RequestorID requestor) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasBeenPrefetched(addr, is_secure, requestor);
}

bool
SlicedCacheAccessorL3::hasEverBeenPrefetched(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->hasEverBeenPrefetched(addr, is_secure);
}

Request::XsMetadata
SlicedCacheAccessorL3::getHitBlkXsMetadata(PacketPtr pkt)
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(pkt->getAddr())->getHitBlkXsMetadata(pkt);
}

bool
SlicedCacheAccessorL3::inMissQueue(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->inMissQueue(addr, is_secure);
}

bool
SlicedCacheAccessorL3::coalesce() const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return l3_wrapper->cache_accessors[0]->coalesce();
}

const uint8_t*
SlicedCacheAccessorL3::findBlock(Addr addr, bool is_secure) const
{
    fatal_if(l3_wrapper->cache_accessors.empty(), "No slice accessors available.");
    return getSlice(addr)->findBlock(addr, is_secure);
}
} // namespace gem5
