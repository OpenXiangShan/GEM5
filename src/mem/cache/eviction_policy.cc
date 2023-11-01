#include "eviction_policy.hh"

namespace gem5
{

EvictionPolicy::EvictionPolicy(const EvictionPolicyParams &p)
    : ClockedObject(p), policy(p.policy)
{
}

bool
EvictionPolicy::needWriteback(CacheBlk *blk) {
    if (policy == 1) {
        return true;
    }
    if (policy == 2) {
        return false;
    }
    if (policy == 3) {
        assert(blk->isValid());
        return !blk->wasPrefetched();
    }
    assert("Invalid eviction policy");
    return false;
}

} // namespace gem5
