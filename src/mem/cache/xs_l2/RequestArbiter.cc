#include "mem/cache/xs_l2/RequestArbiter.hh"

#include "base/trace.hh"
#include "debug/L2CacheSlice.hh"
#include "mem/cache/xs_l2/L2CacheSlice.hh"
#include "mem/cache/xs_l2/L2MainPipe.hh"

namespace gem5
{

RequestArbiter::RequestArbiter(L2CacheSlice* owner_ptr)
  : owner(owner_ptr)
{
}

bool
RequestArbiter::arbitrate(TaskSource task_source, Cycles now)
{
    if (now != _cycle) {
        reset();
        _cycle = now;
    }

    bool success = false;
    switch (task_source) {
        case TaskSource::L2MSHRGrant:
            _has_L2MSHR_grant = true;
            success = true;
            break;
        case TaskSource::L1WQ:
            _has_L1WQ_req = true;
            success = !_has_L2MSHR_grant;
            break;
        case TaskSource::L1MSHR:
        case TaskSource::L2PF:
            _has_L1MSHR_req = true;
            success = !_has_L2MSHR_grant && !_has_L1WQ_req;
            break;
        default:
            panic("Invalid task source");
    }

    if ((task_source == TaskSource::L1MSHR) && !success) {
        owner->stats.l1ReqArbFail++;
    }
    return success;
}

void
RequestArbiter::reset()
{
    _has_L1WQ_req = false;
    _has_L1MSHR_req = false;
    _has_L2MSHR_grant = false;
}

} // namespace gem5
