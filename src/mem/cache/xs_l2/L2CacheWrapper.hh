#ifndef __MEM_CACHE_L2_CACHE_WRAPPER_HH__
#define __MEM_CACHE_L2_CACHE_WRAPPER_HH__

#include <deque>
#include <list>
#include <queue>
#include <random>
#include <vector>

#include "mem/cache/xs_l2/CacheWrapper.hh"
#include "mem/cache/xs_l2/L2MainPipe.hh"
#include "mem/cache/xs_l2/RequestBuffer.hh"
#include "mem/packet.hh"
#include "params/L2CacheWrapper.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"

namespace gem5
{

class L2CacheWrapper : public CacheWrapper
{
  public:
    L2CacheWrapper(const L2CacheWrapperParams &p);

  protected:
    // For request buffering logic
    RequestBuffer requestBuffer;
    // is the inner cache blocked?
    bool inner_cache_blocked = false;
    // should we send retry to L1?
    bool pending_l1_retry = false;
    EventFunctionWrapper trySendEvent;

    // For response pipeline logic
    std::list<PacketPtr> pending_l3_requests;
    std::deque<PacketPtr> ready_responses;
    EventFunctionWrapper processResponsesEvent;
    EventFunctionWrapper tickMainPipeEvent;

    friend class L2MainPipe;
    L2MainPipe mainPipe;

    bool cpuSidePortRecvTimingReq(PacketPtr pkt) override;
    void innerCpuPortRecvReqRetry() override;
    bool innerMemPortRecvTimingReq(PacketPtr pkt) override;
    bool memSidePortRecvTimingResp(PacketPtr pkt) override;
    void innerMemPortRecvRespRetry() override;

    void trySendFromBuffer();
    void processResponses();
    void tickMainPipe();
};

} // namespace gem5

#endif // __MEM_CACHE_L2_CACHE_WRAPPER_HH__
