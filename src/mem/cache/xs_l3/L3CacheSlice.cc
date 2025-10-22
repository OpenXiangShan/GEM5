#include "mem/cache/xs_l3/L3CacheSlice.hh"

#include "base/trace.hh"
#include "debug/L3CacheSlice.hh"
#include "mem/cache/cache.hh"
#include "mem/packet.hh"
#include "params/CacheWrapper.hh"
#include "sim/eventq.hh"

namespace gem5
{

L3CacheSlice::L3CacheSlice(const L3CacheSliceParams &p)
    : BaseCacheWrapper(p),
      requestBuffer(p.buffer_size),
      reqArb(this),
      trySendEvent([this]{ trySendFromBuffer(); }, name()),
      processResponsesEvent([this]{ processResponses(); }, name(), false,
                            processResponsesPri),
      tickMainPipeEvent([this]{ tickMainPipe(); }, name(), false,
                        tickMainPipePri),
      arbFailRetryEvent([this]{ innerCpuPortRecvReqRetry(); }, name(), false,
                        arbFailRetryPri),
      mainPipe(this, p.pipeline_depth),
      stats(this)
{
}

L3CacheSlice::L3CacheSliceStats::L3CacheSliceStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(l2ReqArbFail, statistics::units::Count::get(),
             "Number of failed arbitrations in ReqArb for L2 MSHR requests"),
      ADD_STAT(l2ReqEnterPipeFail, statistics::units::Count::get(),
               "Number of failed entrances to L3MainPipe for L2 MSHR requests"),
      ADD_STAT(l2ReqPipeSetConflict, statistics::units::Count::get(),
               "Number of Set Conflicts in L3MainPipe for L2 MSHR requests"),
      ADD_STAT(l2ReqPipeMCP2Stall, statistics::units::Count::get(),
               "Number of MCP2 stalls in L3MainPipe for L2 MSHR requests"),
      ADD_STAT(l2ReqPipeDirSramStall, statistics::units::Count::get(),
               "Number of DirSram stalls in L3MainPipe for L2 MSHR requests")
{
}

void
L3CacheSlice::scheduleTickMainPipe()
{
    if (mainPipe.hasWork() && !tickMainPipeEvent.scheduled()) {
        schedule(tickMainPipeEvent, nextCycle());
    }
}

/* Memory-Side internal logic */
bool
L3CacheSlice::innerMemPortRecvTimingReq(PacketPtr pkt)
{
    return BaseCacheWrapper::innerMemPortRecvTimingReq(pkt);
}

bool
L3CacheSlice::memSidePortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(L3CacheSlice, "Got resp from memory side for addr: %#x\n", pkt->getAddr());

    ready_responses.push_back(pkt);

    if (!processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextCycle());
    }

    return true;
}

void
L3CacheSlice::processResponses()
{
    // advance pipeline
    mainPipe.advance(curCycle());

    PacketPtr pkt = ready_responses.empty() ? nullptr : ready_responses.front();

    // we want to build a L3 MSHR grant task
    if (pkt &&
        reqArb.arbitrate(TaskSource::L3MSHRGrant, curCycle()) &&
        mainPipe.isTaskAvailable(pkt, TaskSource::L3MSHRGrant))
    {
        DPRINTF(L3CacheSlice, "Building L3 MSHR grant task for addr: %#x\n", pkt->getAddr());
        mainPipe.buildTask(pkt, TaskSource::L3MSHRGrant);
        scheduleTickMainPipe();
        ready_responses.pop_front();
    }

    // Reschedule for the next cycle if there is more work to do
    if (!ready_responses.empty() && !processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextCycle());
    }
}

void
L3CacheSlice::tickMainPipe()
{
    mainPipe.advance(curCycle());
    scheduleTickMainPipe();
}

void
L3CacheSlice::innerMemPortRecvRespRetry()
{
    panic("L3CacheSlice should not receive resp retry from inner L3");
}

/* CPU-Side internal logic */
bool
L3CacheSlice::cpuSidePortRecvTimingReq(PacketPtr pkt)
{
    assert(!pending_l2_retry);
    DPRINTF(L3CacheSlice, "Got req from CPU side for addr: %#x\n", pkt->getAddr());

    bool is_prefetch = pkt->cmd.isHWPrefetch();

    // Express snoop packets should bypass any flow control,
    // so always let express snoop packets through even if blocked
    if (pkt->isExpressSnoop()) {
        DPRINTF(L3CacheSlice, "Express snoop request, forwarding directly to inner cache\n");
        return BaseCacheWrapper::cpuSidePortRecvTimingReq(pkt);
    }
    // If the request is from write_queue(WriteBackClean/CleanEvict etc.),
    // we cannot buffer it and just forward it to inner cache
    if (!pkt->needsResponse() && !is_prefetch) {
        if (inner_cache_blocked || !requestBuffer.empty()) {
            DPRINTF(L3CacheSlice, "Inner cache busy, rejecting WQ request from CPU side\n");
            pending_l2_retry = true;
            return false;
        }
        // directly send to inner cache
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

    // Then the request is from L1 MSHR(ReadEx/ReadShare etc.) or L2 PF,
    // we can buffer it
    if (inner_cache_blocked || !requestBuffer.empty()) {
        // If the Wrapper is waiting for inner cache's retry or some pending requestes
        // are in the buffer, we cannot forward it directly to inner cache
        if (requestBuffer.isFull()) {
            DPRINTF(L3CacheSlice, "Buffer full, rejecting request from CPU side\n");
            pending_l2_retry = true;
            return false;
        }
        requestBuffer.push(pkt);
        DPRINTF(L3CacheSlice, "Request buffered, buffer size: %d\n", requestBuffer.size());
        return true;
    }
    // If the Wrapper is not waiting for inner cache's retry and
    // there is no pending request in the buffer,
    // we can try to forward it directly to inner cache
    TaskSource source = is_prefetch ? TaskSource::L3PF : TaskSource::L2MSHR;
    if (!innerCpuPortSendTimingReq(pkt, source)) {
        inner_cache_blocked = true;
        DPRINTF(L3CacheSlice, "Inner cache busy, try buffering request and blocking\n");
        if (requestBuffer.isFull()) {
            DPRINTF(L3CacheSlice, "Buffer full, rejecting request from CPU side\n");
            pending_l2_retry = true;
            return false;
        }
        requestBuffer.push(pkt);
        DPRINTF(L3CacheSlice, "Request buffered, buffer size: %d\n", requestBuffer.size());
        return true;
    }
    DPRINTF(L3CacheSlice, "Request forwarded directly to inner cache\n");
    return true;
}

void
L3CacheSlice::innerCpuPortRecvReqRetry()
{
    DPRINTF(L3CacheSlice, "Got req retry from inner cache\n");
    assert(inner_cache_blocked);
    assert(!requestBuffer.empty() || pending_l2_retry);

    // inner cache is not blocked anymore
    inner_cache_blocked = false;

    // resend the request from the buffer
    trySendFromBuffer();
}

void
L3CacheSlice::trySendFromBuffer()
{
    if (requestBuffer.empty() && !inner_cache_blocked && pending_l2_retry) {
        DPRINTF(L3CacheSlice, "No pending request in buffer, send retry to L2\n");
        pending_l2_retry = false;
        cpu_side_port.sendRetryReq();
        return;
    }

    if (requestBuffer.empty() || inner_cache_blocked) {
        DPRINTF(L3CacheSlice, "No pending request in buffer or inner cache is blocked, skipping\n");
        return;
    }

    DPRINTF(L3CacheSlice, "Attempting to send delayed request from buffer, buffer size: %d\n",
            requestBuffer.size());

    PacketPtr pkt = requestBuffer.front();

    if (!innerCpuPortSendTimingReq(pkt, TaskSource::L2MSHR)) {
        DPRINTF(L3CacheSlice, "Send delayed request failed, blocking again\n");
        inner_cache_blocked = true;
    } else {
        requestBuffer.pop();
        DPRINTF(L3CacheSlice, "Send delayed request successful, popping from buffer, buffer size: %d\n",
                requestBuffer.size());
        if (!requestBuffer.empty() && !trySendEvent.scheduled()) {
            schedule(trySendEvent, nextCycle());
        }
    }

    if (requestBuffer.empty() && !inner_cache_blocked && pending_l2_retry) {
        DPRINTF(L3CacheSlice, "Buffer empty, sending retry to L2\n");
        pending_l2_retry = false;
        cpu_side_port.sendRetryReq();
    }
}

/* Send request to inner cache cpu side port */
bool
L3CacheSlice::innerCpuPortSendTimingReq(PacketPtr pkt, TaskSource source)
{
    if (reqArb.arbitrate(source, curCycle()) &&
        mainPipe.isTaskAvailable(pkt, source))
    {
        DPRINTF(L3CacheSlice, "Request arbitration succeeded, sending request to inner cache\n");
        bool success = BaseCacheWrapper::cpuSidePortRecvTimingReq(pkt);
        if (success) {
            mainPipe.buildTask(pkt, source);
            scheduleTickMainPipe();
        }
        return success;
    } else {
        DPRINTF(L3CacheSlice, "Request arbitration failed, scheduling retry event\n");
        schedule(arbFailRetryEvent, nextCycle());
        return false;
    }
}

} // namespace gem5