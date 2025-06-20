#ifndef __MEM_CACHE_L2_CACHE_WRAPPER_HH__
#define __MEM_CACHE_L2_CACHE_WRAPPER_HH__

#include <deque>
#include <list>
#include <queue>
#include <random>
#include <vector>

#include "mem/cache/CacheWrapper.hh"
#include "mem/packet.hh"
#include "params/L2CacheWrapper.hh"

namespace gem5
{

class L2CacheWrapper : public CacheWrapper
{
  public:
    L2CacheWrapper(const L2CacheWrapperParams &p);
  protected:
    // For request buffering logic
    std::deque<PacketPtr> request_buffer;
    const unsigned buffer_size;
    // is the inner cache blocked?
    bool inner_cache_blocked = false;
    // should we send retry to L1?
    bool pending_l1_retry = false;
    EventFunctionWrapper trySendEvent;

    // For response delaying logic
    struct DelayedResp
    {
        PacketPtr pkt;
        Tick readyTick;
    };

    struct DelayedRespCompare
    {
        bool operator()(const DelayedResp& a, const DelayedResp& b) const {
            return a.readyTick > b.readyTick;
        }
    };

    std::list<PacketPtr> pending_l3_requests;
    std::priority_queue<DelayedResp, std::vector<DelayedResp>, DelayedRespCompare> delayed_responses;
    bool response_port_blocked = false;
    EventFunctionWrapper processDelayedResponsesEvent;
    const Cycles min_response_latency;
    const Cycles max_response_latency;

    bool cpuSidePortRecvTimingReq(PacketPtr pkt) override;
    void innerCpuPortRecvReqRetry() override;
    bool innerMemPortRecvTimingReq(PacketPtr pkt) override;
    bool memSidePortRecvTimingResp(PacketPtr pkt) override;
    void innerMemPortRecvRespRetry() override;

    void trySendFromBuffer();
    void processDelayedResponses();
};

} // namespace gem5

#endif // __MEM_CACHE_L2_CACHE_WRAPPER_HH__
