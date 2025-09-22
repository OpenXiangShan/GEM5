#ifndef __MEM_CACHE_LLC_REQUEST_ARBITER_HH__
#define __MEM_CACHE_LLC_REQUEST_ARBITER_HH__

#include "base/types.hh"
#include "mem/cache/LLC/L3MainPipe.hh"

namespace gem5
{

class L3CacheSlice;

class RequestArbiter
{
public:
    RequestArbiter(L3CacheSlice* owner_ptr);
    bool arbitrate(TaskSource task_source, Cycles now);

private:
    L3CacheSlice* owner;

    // arbitration meta data
    Cycles _cycle;
    bool _has_L2WQ_req = false;
    bool _has_L2MSHR_req = false;
    bool _has_L3MSHR_grant = false;

    void reset();
};

} // namespace gem5

#endif // __MEM_CACHE_LLC_REQUEST_ARBITER_HH__
