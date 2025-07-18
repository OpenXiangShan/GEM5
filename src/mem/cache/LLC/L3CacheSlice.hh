#ifndef __MEM_CACHE_LLC_L3_CACHE_SLICE_HH__
#define __MEM_CACHE_LLC_L3_CACHE_SLICE_HH__

#include <deque>
#include <list>
#include <queue>
#include <random>
#include <vector>

#include "mem/cache/LLC/CacheWrapper.hh"    // 缓存包装器基类
#include "mem/cache/LLC/L3MainPipe.hh"       // L3主流水线逻辑
#include "mem/cache/LLC/RequestArbiter.hh"   // 请求仲裁器
#include "mem/cache/LLC/RequestBuffer.hh"    // 请求缓冲区
#include "mem/packet.hh"                       // 数据包定义
#include "params/L3CacheSlice.hh"             // 参数定义
#include "sim/cur_tick.hh"                    // 当前时间戳
#include "sim/eventq.hh"                      // 事件队列

namespace gem5
{

// L3缓存切片类，继承自CacheWrapper
class L3CacheSlice : public CacheWrapper
{
  public:
    // 构造函数，接受参数对象
    L3CacheSlice(const L3CacheSliceParams &p);

  protected:
    // 请求缓冲区，用于暂存无法立即处理的请求
    RequestBuffer requestBuffer;
    // 请求仲裁器，用于选择哪个请求可以进入流水线
    friend class RequestArbiter;  // 声明为友元以访问私有成员
    RequestArbiter reqArb;
    // 标记内部缓存是否阻塞（无法接受新请求）
    bool inner_cache_blocked = false;
    // 标记是否需要在适当时机向上层（L2）发送重试信号
    bool pending_l2_retry = false;
    // 事件包装器：用于尝试发送缓冲区中的请求
    EventFunctionWrapper trySendEvent;

    // 等待主存MEM响应的请求队列
    std::list<PacketPtr> pending_MEM_requests; // pending_L2_requests
    // 已准备好向上层（L2）发送的响应队列
    std::deque<PacketPtr> ready_responses;

    // 事件优先级定义（数值越小优先级越高）
    const Event::Priority processResponsesPri = Event::Minimum_Pri;     // 处理响应事件优先级（最高）
    const Event::Priority tickMainPipePri = Event::Minimum_Pri + 1;     // 主流水线处理事件优先级
    const Event::Priority arbFailRetryPri = Event::Minimum_Pri + 2;     // 仲裁失败重试事件优先级（最低）

    // 事件包装器：处理响应队列
    EventFunctionWrapper processResponsesEvent;
    // 事件包装器：驱动主流水线执行
    EventFunctionWrapper tickMainPipeEvent;
    // 事件包装器：仲裁失败后重试
    EventFunctionWrapper arbFailRetryEvent;

    // 声明友元以便主流水线访问本类成员
    friend class L3MainPipe;
    // L3主流水线实例，负责缓存核心逻辑（标签检查、数据访问等）
    L3MainPipe mainPipe;

    // 重写基类方法：处理来自CPU端（L2）的请求
    bool cpuSidePortRecvTimingReq(PacketPtr pkt) override;
    // 重写基类方法：当内部CPU端口准备好接收重试时调用
    void innerCpuPortRecvReqRetry() override;

    /**
     * 内部CPU端口发送时序请求
     * @param pkt 要发送的数据包
     * @param source 请求来源（用于仲裁）
     * @return 发送是否成功
     */
    bool innerCpuPortSendTimingReq(PacketPtr pkt, TaskSource source);

    // 重写基类方法：处理来自主存MEM的请求（用于维护操作等），发往MEM
    bool innerMemPortRecvTimingReq(PacketPtr pkt) override;
    // 重写基类方法：处理来自主存MEM的响应
    bool memSidePortRecvTimingResp(PacketPtr pkt) override;
    // 重写基类方法：当内存端口准备好接收重试时调用
    void innerMemPortRecvRespRetry() override;

    // 尝试从请求缓冲区发送请求到主流水线
    void trySendFromBuffer();
    // 处理响应队列（将准备好的响应发送给L2）
    void processResponses();
    // 安排主流水线处理事件（确保在合适时间调用tickMainPipe）
    void scheduleTickMainPipe();
    // 主流水线处理函数（驱动缓存核心逻辑）
    void tickMainPipe();
};

} // namespace gem5

#endif // __MEM_CACHE_LLC_L3_CACHE_SLICE_HH__