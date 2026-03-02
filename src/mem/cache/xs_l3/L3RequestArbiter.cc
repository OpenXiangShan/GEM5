#include "mem/cache/xs_l3/L3RequestArbiter.hh"

#include "base/trace.hh"
#include "debug/L3CacheSlice.hh"
#include "mem/cache/xs_l3/L3CacheSlice.hh"
#include "mem/cache/xs_l3/L3MainPipe.hh"

namespace gem5
{

L3RequestArbiter::L3RequestArbiter(L3CacheSlice* owner_ptr)
  : owner(owner_ptr)
{
}

bool
L3RequestArbiter::arbitrate(L3TaskSource task_source, Cycles now)
{
    if (now != _cycle) {
        reset();
        _cycle = now;
    }

    bool success = false;
    switch (task_source) {
        case L3TaskSource::L3MSHRGrant:
            _has_L3MSHR_grant = true;
            success = true;
            break;
        case L3TaskSource::L2WQ:
            _has_L2WQ_req = true;
            success = !_has_L3MSHR_grant;
            break;
        case L3TaskSource::L2MSHR:
        case L3TaskSource::L3PF:
            _has_L2MSHR_req = true;
            success = !_has_L3MSHR_grant && !_has_L2WQ_req;
            break;
        default:
            panic("Invalid task source");
    }

    if ((task_source == L3TaskSource::L2MSHR) && !success) {
        owner->stats.l2ReqArbFail++;
    }
    return success;
}

void
L3RequestArbiter::reset()
{
    _has_L2WQ_req = false;
    _has_L2MSHR_req = false;
    _has_L3MSHR_grant = false;
}

} // namespace gem5
