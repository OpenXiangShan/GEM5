#include "mem/cache/LLC/L3CacheSlice.hh"

#include "base/trace.hh"
#include "debug/L3CacheSlice.hh"
#include "mem/cache/cache.hh"
#include "mem/packet.hh"
#include "params/CacheWrapper.hh"
#include "sim/eventq.hh"

namespace gem5
{

// L2缓存分片构造函数
L3CacheSlice::L3CacheSlice(const L3CacheSliceParams &p)
    : CacheWrapper(p),                    // 调用基类构造函数
      requestBuffer(p.buffer_size),       // 初始化请求缓冲区
      reqArb(this),                       // 初始化请求仲裁器
      // 初始化各种调度事件：
      trySendEvent([this]{ trySendFromBuffer(); }, name()),                   // 尝试发送缓冲区请求事件
      processResponsesEvent([this]{ processResponses(); }, name(), false,     // 处理响应事件
                            processResponsesPri),
      tickMainPipeEvent([this]{ tickMainPipe(); }, name(), false,             // 主流水线推进事件
                        tickMainPipePri),
      arbFailRetryEvent([this]{ innerCpuPortRecvReqRetry(); }, name(), false, // 仲裁失败重试事件
                        arbFailRetryPri),
      mainPipe(this, p.pipeline_depth)    // 初始化主流水线
{
}

// 调度主流水线推进事件
void
L3CacheSlice::scheduleTickMainPipe()
{
    // 当流水线有工作且未调度时，在下一周期调度
    if (mainPipe.hasWork() && !tickMainPipeEvent.scheduled()) {
        schedule(tickMainPipeEvent, nextCycle());
    }
}

/* Memory-Side internal logic 内存侧逻辑 */

// 处理来自内存侧的请求
bool
L3CacheSlice::innerMemPortRecvTimingReq(PacketPtr pkt)
{
    // 调用基类方法转发请求到MEM
    bool success = CacheWrapper::innerMemPortRecvTimingReq(pkt);
    return success;
}

// 处理来自内存侧的响应
bool
L3CacheSlice::memSidePortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(L3CacheSlice, "Got resp from MEM for addr: %#x\n", pkt->getAddr());

    ready_responses.push_back(pkt); // 加入就绪响应队列

    // 若未调度，安排处理响应事件
    if (!processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextCycle());
    }

    return true;
}

// 处理就绪响应队列
void
L3CacheSlice::processResponses()
{
    mainPipe.advance(curCycle()); // 推进流水线

    // 当有响应、仲裁成功且流水线可处理时，
    // 放到ready_responses.front()上构建任务并安排流水线推进
    // 建立流水线启动流水线
    // 移除ready_responses front.pop_front();
    if (!ready_responses.empty() &&
        reqArb.arbitrate(TaskSource::L3MSHRGrant, curCycle()) &&
        mainPipe.isTaskAvailable(TaskSource::L3MSHRGrant))
    {
        DPRINTF(L3CacheSlice, "Buidling L3 MSHR grant task for addr: %#x\n", ready_responses.front()->getAddr());
        PacketPtr pkt = ready_responses.front();
        mainPipe.buildTask(pkt, TaskSource::L3MSHRGrant); // 在流水线构建任务
        scheduleTickMainPipe();      // 调度流水线推进
        ready_responses.pop_front(); // 移除已处理响应
    }

    // 如果还有响应未处理，安排下一周期继续
    if (!ready_responses.empty() && !processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextCycle());
    }
}

// 主流水线推进函数
void
L3CacheSlice::tickMainPipe()
{
    mainPipe.advance(curCycle()); // 推进流水线
    scheduleTickMainPipe();       // 可能安排下次推进
}

// 内存侧响应重试处理（此处不应发生）
void
L3CacheSlice::innerMemPortRecvRespRetry()
{
    panic("L3CacheSlice should not receive resp retry from inner L3");
}

/* CPU-Side internal logic CPU侧逻辑 */

// 处理来自CPU(L2(RN))侧的请求
bool
L3CacheSlice::cpuSidePortRecvTimingReq(PacketPtr pkt)
{
    assert(!pending_l2_retry);
    DPRINTF(L3CacheSlice, "Got req from CPU side for addr: %#x\n", pkt->getAddr());

    bool is_prefetch = pkt->cmd.isHWPrefetch(); // 判断是否为预取请求

    // Snoop packets
    // 快速嗅探请求直接转发
    if (pkt->isExpressSnoop()) {
        DPRINTF(L3CacheSlice, "Express snoop request, forwarding directly to inner cache\n");
        return CacheWrapper::cpuSidePortRecvTimingReq(pkt);
    }

    // WriteBack/CleanEvict etc
    // 处理非响应类请求（如WriteBack/CleanEvict etc).
    // 不同buffer it，直接forward到inner cache
    if (!pkt->needsResponse() && !is_prefetch) {
        // 如果内部缓存阻塞或缓冲区非空，拒绝请求
        if (inner_cache_blocked || !requestBuffer.empty()) {
            DPRINTF(L3CacheSlice, "Inner cache busy, rejecting WQ request from CPU side\n");
            pending_l2_retry = true; // 阻塞时标记需要重试
            return false;
        }
        // 尝试直接发送: 从L3CalshSlice to inner L3 Cache
        bool success = innerCpuPortSendTimingReq(pkt, TaskSource::L2WQ);
        if (!success) {
            DPRINTF(L3CacheSlice, "Inner cache busy, rejecting WQ request from CPU side\n");
            inner_cache_blocked = true;
            pending_l2_retry = true;
        } else {
            DPRINTF(L3CacheSlice, "WQ request forwarded to inner cache\n");
        }
        return success;
    }

    // L2 MSHR(ReadEx/ReadShare etc.) or L3 PF
    // L2 MSHR（读请求）或L3预取请求
    // 可以buffer
    if (inner_cache_blocked || !requestBuffer.empty()) {
        // 内部缓存阻塞或缓冲区非空时尝试缓冲
        if (requestBuffer.isFull()) {
            DPRINTF(L3CacheSlice, "Buffer full, rejecting request from CPU side\n");
            pending_l2_retry = true;
            return false;
        }
        requestBuffer.push(pkt); // 加入缓冲区
        DPRINTF(L3CacheSlice, "Request buffered, current buffer size: %d\n", requestBuffer.size());
        return true;
    }

    // If the Wrapper is not waiting for inner cache's retry and
    // there is no pending request in the buffer,
    // we can try to forward it directly to inner cache
    // 无阻塞时尝试直接发送
    TaskSource source = is_prefetch ? TaskSource::L3PF : TaskSource::L2MSHR;
    // L3CacheSlice to inner L3 Cache
    if (!innerCpuPortSendTimingReq(pkt, source)) {
        inner_cache_blocked = true; // 发送失败则阻塞
        DPRINTF(L3CacheSlice, "Inner cache busy, try buffering request and blocking\n");
        if (requestBuffer.isFull()) {
            DPRINTF(L3CacheSlice, "Buffer full, rejecting request from CPU side\n");
            pending_l2_retry = true;
            return false;
        }
        requestBuffer.push(pkt); // 加入缓冲区
        DPRINTF(L3CacheSlice, "Request buffered, buffer size: %d\n", requestBuffer.size());
        return true;
    }
    DPRINTF(L3CacheSlice, "Request forwarded directly to inner cache\n");
    return true;
}

// CPU侧内部端口接收重试请求， inner L3 Cache 发给 L3CacheSlice
// 此时说明inner L3 Cache已准备好接收请求
// 从缓冲区尝试发送请求
void
L3CacheSlice::innerCpuPortRecvReqRetry()
{
    DPRINTF(L3CacheSlice, "Got req retry from inner cache\n");
    assert(inner_cache_blocked);
    assert(!requestBuffer.empty() || pending_l2_retry);

    inner_cache_blocked = false; // 清除阻塞标志
    trySendFromBuffer();         // 尝试发送缓冲区请求
}

// 尝试从缓冲区发送请求
// 如果缓冲区为空且有挂起的重试，则向L2发送重试
// 如果缓冲区非空或内部缓存阻塞，则跳过
// 否则尝试发送缓冲区头部请求到内部缓存
// 如果发送失败, 不移除当前请求
// 如果发送成功, 移除当前请求
// 如果缓冲区当前还是非空，说明还有retry数据或是刚才发送失败了，安排下次尝试
// 如果缓冲区为空且无阻塞且有挂起的重试，则向L2发送重试,调用CPU端口的sendRetryReq方法
void
L3CacheSlice::trySendFromBuffer()
{
    // 当缓冲区空且有挂起的重试时，向L2发送重试
    if (requestBuffer.empty() && !inner_cache_blocked && pending_l2_retry) {
        DPRINTF(L3CacheSlice, "No pending request in buffer, send retry to L2\n");
        pending_l2_retry = false;
        cpu_side_port.sendRetryReq();
        return;
    }

    // 无需处理的情况
    if (requestBuffer.empty() || inner_cache_blocked) {
        DPRINTF(L3CacheSlice, "No pending request in buffer or inner cache is blocked, skipping\n");
        return;
    }

    DPRINTF(L3CacheSlice, "Attempting to send delayed request from buffer, buffer size: %d\n", requestBuffer.size());
    PacketPtr pkt = requestBuffer.front();

    // 尝试发送队列头部请求
    if (!innerCpuPortSendTimingReq(pkt, TaskSource::L2MSHR)) {
        DPRINTF(L3CacheSlice, "Send delayed request failed, blocking again\n");
        inner_cache_blocked = true;
    } else {
        requestBuffer.pop(); // 发送成功则出队
        DPRINTF(L3CacheSlice, "Send delayed request successful, popping from buffer,
                                buffer size: %d\n", requestBuffer.size());
        // 若缓冲区非空，安排下次尝试
        if (!requestBuffer.empty() && !trySendEvent.scheduled()) {
            schedule(trySendEvent, nextCycle());
        }
    }

    // 发送后检查是否需向L2发送重试
    if (requestBuffer.empty() && !inner_cache_blocked && pending_l2_retry) {
        DPRINTF(L3CacheSlice, "Buffer empty, sending retry to L2\n");
        pending_l2_retry = false;
        cpu_side_port.sendRetryReq();
    }
}

/* 向内部缓存CPU端口发送请求 */
bool
L3CacheSlice::innerCpuPortSendTimingReq(PacketPtr pkt, TaskSource source)
{
    // 仲裁成功且流水线有空闲
    if (reqArb.arbitrate(source, curCycle()) &&
        mainPipe.isTaskAvailable(source))
    {
        DPRINTF(L3CacheSlice, "Request arbitration succeeded, sending request to inner cache\n");
        bool success = CacheWrapper::cpuSidePortRecvTimingReq(pkt);
        if (success) {
            mainPipe.buildTask(pkt, source); // 构建流水线任务
            scheduleTickMainPipe();           // 调度流水线推进
        }
        return success;
    } else {
        DPRINTF(L3CacheSlice, "Request arbitration failed, scheduling retry event\n");
        schedule(arbFailRetryEvent, nextCycle()); // 安排仲裁重试
        return false;
    }
}

} // namespace gem5