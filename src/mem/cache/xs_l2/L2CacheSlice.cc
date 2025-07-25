#include "mem/cache/xs_l2/L2CacheSlice.hh"

#include "base/trace.hh"
#include "debug/L2CacheSlice.hh"
#include "mem/cache/cache.hh"
#include "mem/packet.hh"
#include "params/CacheWrapper.hh"
#include "sim/eventq.hh"

namespace gem5
{

L2CacheSlice::L2CacheSlice(const L2CacheSliceParams &p)
    : CacheWrapper(p),
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

L2CacheSlice::L2CacheSliceStats::L2CacheSliceStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(l1ReqArbFail, statistics::units::Count::get(),
             "Number of failed arbitrations in ReqArb for L1 MSHR requests"),
      ADD_STAT(l1ReqEnterPipeFail, statistics::units::Count::get(),
               "Number of failed entrances to L2MainPipe for L1 MSHR requests"),
      ADD_STAT(l1ReqPipeSetConflict, statistics::units::Count::get(),
               "Number of Set Conflicts in L2MainPipe for L1 MSHR requests"),
      ADD_STAT(l1ReqPipeMCP2Stall, statistics::units::Count::get(),
               "Number of MCP2 stalls in L2MainPipe for L1 MSHR requests"),
      ADD_STAT(l1ReqPipeDirSramStall, statistics::units::Count::get(),
               "Number of DirSram stalls in L2MainPipe for L1 MSHR requests")
{
}

void
L2CacheSlice::scheduleTickMainPipe()
{
    if (mainPipe.hasWork() && !tickMainPipeEvent.scheduled()) {
        schedule(tickMainPipeEvent, nextCycle());
    }
}

/* Memory-Side internal logic */
bool
L2CacheSlice::innerMemPortRecvTimingReq(PacketPtr pkt)
{
    return CacheWrapper::innerMemPortRecvTimingReq(pkt);
}

bool
L2CacheSlice::memSidePortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(L2CacheSlice, "Got resp from memory side for addr: %#x\n", pkt->getAddr());

    ready_responses.push_back(pkt);

    if (!processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextCycle());
    }

    return true;
}

void
L2CacheSlice::processResponses()
{
    // advance pipeline
    mainPipe.advance(curCycle());

    PacketPtr pkt = ready_responses.empty() ? nullptr : ready_responses.front();

    // we want to build a L2 MSHR grant task
    if (pkt &&
        reqArb.arbitrate(TaskSource::L2MSHRGrant, curCycle()) &&
        mainPipe.isTaskAvailable(pkt, TaskSource::L2MSHRGrant))
    {
        DPRINTF(L2CacheSlice, "Building L2 MSHR grant task for addr: %#x\n", pkt->getAddr());
        mainPipe.buildTask(pkt, TaskSource::L2MSHRGrant);
        scheduleTickMainPipe();
        ready_responses.pop_front();
    }

    // Reschedule for the next cycle if there is more work to do
    if (!ready_responses.empty() && !processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextCycle());
    }
}

void
L2CacheSlice::tickMainPipe()
{
    mainPipe.advance(curCycle());
    scheduleTickMainPipe();
}

void
L2CacheSlice::innerMemPortRecvRespRetry()
{
    panic("L2CacheSlice should not receive resp retry from inner L2");
}

/* CPU-Side internal logic */
bool
L2CacheSlice::cpuSidePortRecvTimingReq(PacketPtr pkt)
{
    assert(!pending_l1_retry);
    DPRINTF(L2CacheSlice, "Got req from CPU side for addr: %#x\n", pkt->getAddr());

    bool is_prefetch = pkt->cmd.isHWPrefetch();

    // Express snoop packets should bypass any flow control,
    // so always let express snoop packets through even if blocked
    if (pkt->isExpressSnoop()) {
        DPRINTF(L2CacheSlice, "Express snoop request, forwarding directly to inner cache\n");
        return CacheWrapper::cpuSidePortRecvTimingReq(pkt);
    }
    // If the request is from write_queue(WriteBackClean/CleanEvict etc.),
    // we cannot buffer it and just forward it to inner cache
    if (!pkt->needsResponse() && !is_prefetch) {
        if (inner_cache_blocked || !requestBuffer.empty()) {
            DPRINTF(L2CacheSlice, "Inner cache busy, rejecting WQ request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        // directly send to inner cache
        bool success = innerCpuPortSendTimingReq(pkt, TaskSource::L1WQ);
        if (!success) {
            DPRINTF(L2CacheSlice, "Inner cache busy, rejecting WQ request from CPU side\n");
            inner_cache_blocked = true;
            pending_l1_retry = true;
        } else {
            DPRINTF(L2CacheSlice, "WQ request forwarded to inner cache\n");
        }
        return success;
    }

    // Then the request is from L1 MSHR(ReadEx/ReadShare etc.) or L2 PF,
    // we can buffer it
    if (inner_cache_blocked || !requestBuffer.empty()) {
        // If the Wrapper is waiting for inner cache's retry or some pending requestes
        // are in the buffer, we cannot forward it directly to inner cache
        if (requestBuffer.isFull()) {
            DPRINTF(L2CacheSlice, "Buffer full, rejecting request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        requestBuffer.push(pkt);
        DPRINTF(L2CacheSlice, "Request buffered, buffer size: %d\n", requestBuffer.size());
        return true;
    }
    // If the Wrapper is not waiting for inner cache's retry and
    // there is no pending request in the buffer,
    // we can try to forward it directly to inner cache
    TaskSource source = is_prefetch ? TaskSource::L2PF : TaskSource::L1MSHR;
    if (!innerCpuPortSendTimingReq(pkt, source)) {
        inner_cache_blocked = true;
        DPRINTF(L2CacheSlice, "Inner cache busy, try buffering request and blocking\n");
        if (requestBuffer.isFull()) {
            DPRINTF(L2CacheSlice, "Buffer full, rejecting request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        requestBuffer.push(pkt);
        DPRINTF(L2CacheSlice, "Request buffered, buffer size: %d\n", requestBuffer.size());
        return true;
    }
    DPRINTF(L2CacheSlice, "Request forwarded directly to inner cache\n");
    return true;
}

void
L2CacheSlice::innerCpuPortRecvReqRetry()
{
    DPRINTF(L2CacheSlice, "Got req retry from inner cache\n");
    assert(inner_cache_blocked);
    assert(!requestBuffer.empty() || pending_l1_retry);

    // inner cache is not blocked anymore
    inner_cache_blocked = false;

    // resend the request from the buffer
    trySendFromBuffer();
}

void
L2CacheSlice::trySendFromBuffer()
{
    if (requestBuffer.empty() && !inner_cache_blocked && pending_l1_retry) {
        DPRINTF(L2CacheSlice, "No pending request in buffer, send retry to L1\n");
        pending_l1_retry = false;
        cpu_side_port.sendRetryReq();
        return;
    }

    if (requestBuffer.empty() || inner_cache_blocked) {
        DPRINTF(L2CacheSlice, "No pending request in buffer or inner cache is blocked, skipping\n");
        return;
    }

    DPRINTF(L2CacheSlice, "Attempting to send delayed request from buffer, buffer size: %d\n",
            requestBuffer.size());

    PacketPtr pkt = requestBuffer.front();

    if (!innerCpuPortSendTimingReq(pkt, TaskSource::L1MSHR)) {
        DPRINTF(L2CacheSlice, "Send delayed request failed, blocking again\n");
        inner_cache_blocked = true;
    } else {
        requestBuffer.pop();
        DPRINTF(L2CacheSlice, "Send delayed request successful, popping from buffer, buffer size: %d\n",
                requestBuffer.size());
        if (!requestBuffer.empty() && !trySendEvent.scheduled()) {
            schedule(trySendEvent, nextCycle());
        }
    }

    if (requestBuffer.empty() && !inner_cache_blocked && pending_l1_retry) {
        DPRINTF(L2CacheSlice, "Buffer empty, sending retry to L1\n");
        pending_l1_retry = false;
        cpu_side_port.sendRetryReq();
    }
}

/* Send request to inner cache cpu side port */
bool
L2CacheSlice::innerCpuPortSendTimingReq(PacketPtr pkt, TaskSource source)
{
    if (reqArb.arbitrate(source, curCycle()) &&
        mainPipe.isTaskAvailable(pkt, source))
    {
        DPRINTF(L2CacheSlice, "Request arbitration succeeded, sending request to inner cache\n");
        bool success = CacheWrapper::cpuSidePortRecvTimingReq(pkt);
        if (success) {
            mainPipe.buildTask(pkt, source);
            scheduleTickMainPipe();
        }
        return success;
    } else {
        DPRINTF(L2CacheSlice, "Request arbitration failed, scheduling retry event\n");
        schedule(arbFailRetryEvent, nextCycle());
        return false;
    }
}

} // namespace gem5
