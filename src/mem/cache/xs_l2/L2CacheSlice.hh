#ifndef __MEM_CACHE_XS_L2_L2_CACHE_SLICE_HH__
#define __MEM_CACHE_XS_L2_L2_CACHE_SLICE_HH__

#include <cstdint>
#include <deque>
#include <list>
#include <queue>
#include <random>
#include <vector>

#include "mem/cache/base.hh"
#include "mem/cache/xs_l2/CacheWrapper.hh"
#include "mem/cache/xs_l2/L2MainPipe.hh"
#include "mem/cache/xs_l2/RequestArbiter.hh"
#include "mem/cache/xs_l2/RequestBuffer.hh"
#include "mem/packet.hh"
#include "params/L2CacheSlice.hh"
#include "sim/cur_tick.hh"
#include "sim/eventq.hh"
#include "sim/stats.hh"

namespace gem5
{

class L2CacheSlice : public CacheWrapper
{
  public:
    L2CacheSlice(const L2CacheSliceParams &p);

    void setCacheAccessor(BaseCache* accessor) {
        cache_accessor = accessor;
    }

    // L2CacheWrapper will call this to provide the implementation for getSetIdx.
    void setGetSetIdxFunc(std::function<Addr(Addr)> func) {
        getSetIdx = func;
    }

    // L2CacheWrapper will call this to provide the implementation for getDataBankIdx.
    void setGetDataBankIdxFunc(std::function<Addr(Addr)> func) {
        getDataBankIdx = func;
    }

    // L2CacheWrapper will call this to provide the implementation for getDirBankIdx.
    void setGetDirBankIdxFunc(std::function<Addr(Addr)> func) {
        getDirBankIdx = func;
    }

    void setPipeDirWriteStage(uint64_t stage) {
        pipeDirWriteStage = stage;
    }

    void setDirReadBypass(bool bypass) {
        dirReadBypass = bypass;
    }

  protected:
    // For request buffering logic
    RequestBuffer requestBuffer;
    // For request arbitration logic
    friend class RequestArbiter;
    RequestArbiter reqArb;
    // is the inner cache blocked?
    bool inner_cache_blocked = false;
    // should we send retry to L1?
    bool pending_l1_retry = false;
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

    friend class L2MainPipe;
    L2MainPipe mainPipe;

    struct L2CacheSliceStats : public statistics::Group
    {
        L2CacheSliceStats(statistics::Group *parent);

        statistics::Scalar l1ReqArbFail;
        statistics::Scalar l1ReqEnterPipeFail;
        statistics::Scalar l1ReqPipeSetConflict;
        statistics::Scalar l1ReqPipeMCP2Stall;
        statistics::Scalar l1ReqPipeDirSramStall;
    };
    L2CacheSliceStats stats;

    // This will hold the function to calculate the set index for an address.
    std::function<Addr(Addr)> getSetIdx;
    std::function<Addr(Addr)> getDataBankIdx;
    std::function<Addr(Addr)> getDirBankIdx;

    uint64_t pipeDirWriteStage = 3;
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

#endif // __MEM_CACHE_XS_L2_L2_CACHE_SLICE_HH__
