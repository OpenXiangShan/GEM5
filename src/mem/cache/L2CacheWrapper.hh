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
  protected:
    std::deque<PacketPtr> request_buffer;
    const unsigned buffer_size;
    // is the inner cache blocked?
    bool inner_cache_blocked = false;
    // should we send retry to L1?
    bool pending_l1_retry = false;
    EventFunctionWrapper trySendEvent;

    bool cpuSidePortRecvTimingReq(PacketPtr pkt) override;
    void innerCpuPortRecvReqRetry() override;

    void trySendFromBuffer();
};

} // namespace gem5

#endif // __MEM_CACHE_L2_CACHE_WRAPPER_HH__
