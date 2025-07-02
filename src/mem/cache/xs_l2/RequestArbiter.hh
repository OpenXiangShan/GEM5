#ifndef __MEM_CACHE_XS_L2_REQUEST_ARBITER_HH__
#define __MEM_CACHE_XS_L2_REQUEST_ARBITER_HH__

#include "base/types.hh"
#include "mem/cache/xs_l2/L2MainPipe.hh"

namespace gem5
{

class L2CacheSlice;

class RequestArbiter
{
public:
    RequestArbiter(L2CacheSlice* owner_ptr);
    bool arbitrate(TaskSource task_source, Cycles now);

private:
    L2CacheSlice* owner;

    // arbitration meta data
    Cycles _cycle;
    bool _has_L1WQ_req = false;
    bool _has_L1MSHR_req = false;
    bool _has_L2MSHR_grant = false;

    void reset();
};

} // namespace gem5

#endif // __MEM_CACHE_XS_L2_REQUEST_ARBITER_HH__
