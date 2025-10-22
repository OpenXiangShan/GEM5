#ifndef __MEM_CACHE_xs_l3_L3_CACHE_SLICE_HH__
#define __MEM_CACHE_xs_l3_L3_CACHE_SLICE_HH__

#include <cstdint>
#include <deque>
#include <list>
#include <queue>
#include <random>
#include <vector>

#include "mem/cache/base.hh"
#include "mem/cache/xs_l3/BaseCacheWrapper.hh"
#include "mem/cache/xs_l3/L3MainPipe.hh"
#include "mem/cache/xs_l3/L3RequestArbiter.hh"
#include "mem/cache/xs_l3/L3RequestBuffer.hh"
#include "mem/packet.hh"
#include "params/L3CacheSlice.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/stats.hh"

namespace gem5
{

class L3CacheSlice : public BaseCacheWrapper
{
  public:
    L3CacheSlice(const L3CacheSliceParams &p);

    void setCacheAccessor(BaseCache* accessor) {
        cache_accessor = accessor;
    }

    // L3CacheWrapper will call this to provide the implementation for getSetIdx.
    void setGetSetIdxFunc(std::function<Addr(Addr)> func) {
        getSetIdx = func;
    }

    void setPipeDataWriteStage(uint64_t stage) {
        pipeDataWriteStage = stage;
    }

    void setDirReadBypass(bool bypass) {
        dirReadBypass = bypass;
    }

  protected:
    // For request buffering logic
    L3RequestBuffer requestBuffer;
    // For request arbitration logic
    friend class L3RequestArbiter;
    L3RequestArbiter reqArb;
    // is the inner cache blocked?
    bool inner_cache_blocked = false;
    // should we send retry to L2?
    bool pending_l2_retry = false;
    EventFunctionWrapper trySendEvent;

    // For response pipeline logic
    std::deque<PacketPtr> ready_responses;

    CacheAccessor* cache_accessor = nullptr;

    // lower priority events are scheduled earlier in the same tick
    const Event::Priority processResponsesPri = Event::Minimum_Pri;
    const Event::Priority tickMainPipePri = Event::Minimum_Pri + 1;
    const Event::Priority arbFailRetryPri = Event::Minimum_Pri + 2;

    EventFunctionWrapper processResponsesEvent;
    EventFunctionWrapper tickMainPipeEvent;
    EventFunctionWrapper arbFailRetryEvent;

    friend class L3MainPipe;
    L3MainPipe mainPipe;

    struct L3CacheSliceStats : public statistics::Group
    {
        L3CacheSliceStats(statistics::Group *parent);

        statistics::Scalar l2ReqArbFail;
        statistics::Scalar l2ReqEnterPipeFail;
        statistics::Scalar l2ReqPipeSetConflict;
        statistics::Scalar l2ReqPipeMCP2Stall;
        statistics::Scalar l2ReqPipeDirSramStall;
    };
    L3CacheSliceStats stats;

    // This will hold the function to calculate the set index for an address.
    std::function<Addr(Addr)> getSetIdx;

    uint64_t pipeDataWriteStage = 3;
    bool dirReadBypass = false;

    bool cpuSidePortRecvTimingReq(PacketPtr pkt) override;
    void innerCpuPortRecvReqRetry() override;
    bool innerCpuPortSendTimingReq(PacketPtr pkt, TaskSource source);
    bool innerMemPortRecvTimingReq(PacketPtr pkt) override;
    bool memSidePortRecvTimingResp(PacketPtr pkt) override;
    void innerMemPortRecvRespRetry() override;

    void trySendFromBuffer();
    void processResponses();
    void scheduleTickMainPipe();
    void tickMainPipe();
};

} // namespace gem5

#endif // __MEM_CACHE_xs_l3_L3_CACHE_SLICE_HH__
