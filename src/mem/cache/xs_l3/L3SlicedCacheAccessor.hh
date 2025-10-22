#ifndef __MEM_CACHE_xs_l3_SLICED_CACHE_ACCESSOR_HH__
#define __MEM_CACHE_xs_l3_SLICED_CACHE_ACCESSOR_HH__

#include <vector>

#include "base/types.hh"
#include "mem/cache/cache_probe_arg.hh"

namespace gem5
{

class L3CacheWrapper;

class L3SlicedCacheAccessor : public CacheAccessor
{
private:
    L3CacheWrapper* l3_wrapper;

    CacheAccessor* getSlice(Addr addr) const;

public:
    L3SlicedCacheAccessor(L3CacheWrapper* l3_wrapper)
        : l3_wrapper(l3_wrapper)
    {}

    bool inCache(Addr addr, bool is_secure) const override;
    unsigned level() const override;
    bool hasBeenPrefetched(Addr addr, bool is_secure) const override;
    bool hasBeenPrefetched(Addr addr, bool is_secure, RequestorID requestor) const override;
    bool hasEverBeenPrefetched(Addr addr, bool is_secure) const override;
    Request::XsMetadata getHitBlkXsMetadata(PacketPtr pkt) override;
    bool inMissQueue(Addr addr, bool is_secure) const override;
    bool coalesce() const override;
    const uint8_t* findBlock(Addr addr, bool is_secure) const override;
};

} // namespace gem5

#endif // __MEM_CACHE_xs_l3_SLICED_CACHE_ACCESSOR_HH__