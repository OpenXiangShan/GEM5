#include "mem/cache/xs_l2/L2CacheWrapper.hh"

#include "base/trace.hh"
#include "debug/L2CacheWrapper.hh"
#include "mem/cache/cache.hh"
#include "mem/packet.hh"
#include "params/CacheWrapper.hh"
#include "sim/eventq.hh"

namespace gem5
{

L2CacheWrapper::L2CacheWrapper(const L2CacheWrapperParams &p)
    : CacheWrapper(p),
      requestBuffer(p.buffer_size),
      trySendEvent([this]{ trySendFromBuffer(); }, name()),
      processResponsesEvent([this]{ processResponses(); }, name(), false,
                            Event::Maximum_Pri),
      tickMainPipeEvent([this]{ tickMainPipe(); }, name()),
      mainPipe(this, p.pipeline_depth)
{
    srand(time(NULL));
}

bool
L2CacheWrapper::innerMemPortRecvTimingReq(PacketPtr pkt)
{
    // If the request needs a response, track it
    bool enqueue = false;
    if (pkt->needsResponse()) {
        DPRINTF(L2CacheWrapper, "Tracking request to L3 for addr: %#x\n", pkt->getAddr());
        pending_l3_requests.push_back(pkt);
        enqueue = true;
    }

    // First, call the base class implementation to forward the request
    bool success = CacheWrapper::innerMemPortRecvTimingReq(pkt);

    // If the request was not successfully sent, remove it from the pending list
    if (!success && enqueue) {
        pending_l3_requests.pop_back();
    }

    return success;
}

bool
L2CacheWrapper::memSidePortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(L2CacheWrapper, "Got resp from memory side for addr: %#x\n", pkt->getAddr());

    auto it = std::find_if(pending_l3_requests.begin(), pending_l3_requests.end(),
        [&](const PacketPtr& pending_pkt) {
            return pending_pkt->getAddr() == pkt->getAddr();
        });

    if (it == pending_l3_requests.end()) {
        // TODO: Is this case possible?
        // we didn't find the request in pending_l3_requests, forward it directly.
        DPRINTF(L2CacheWrapper, "Response for addr %#x is not a tracked L2 miss, "
                                "forwarding directly. %s\n", pkt->getAddr(), pkt->print());
        return CacheWrapper::memSidePortRecvTimingResp(pkt);
    }

    DPRINTF(L2CacheWrapper, "Found matching tracked request for addr: %#x. Queueing for pipeline.\n", pkt->getAddr());
    pending_l3_requests.erase(it);

    ready_responses.push_back(pkt);

    if (!processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextWrapperCycle());
    }

    return true;
}

void
L2CacheWrapper::processResponses()
{
    // advance pipeline
    mainPipe.advance(curCycle());

    // we want to build a L2 MSHR grant task
    if (!ready_responses.empty() && mainPipe.isTaskAvailable(TaskSource::L2MSHRGrant)) {
        DPRINTF(L2CacheWrapper, "Building L2 MSHR grant task for addr: %#x\n", ready_responses.front()->getAddr());
        PacketPtr pkt = ready_responses.front();
        mainPipe.buildTask(pkt, TaskSource::L2MSHRGrant);
        ready_responses.pop_front();
    }

    // Reschedule for the next cycle if there is more work to do
    if (!ready_responses.empty() && !processResponsesEvent.scheduled()) {
        schedule(processResponsesEvent, nextWrapperCycle());
    }

    if (mainPipe.hasWork() && !tickMainPipeEvent.scheduled()) {
        schedule(tickMainPipeEvent, nextWrapperCycle());
    }
}

void
L2CacheWrapper::tickMainPipe()
{
    mainPipe.advance(curCycle());
    if (mainPipe.hasWork() && !tickMainPipeEvent.scheduled()) {
        schedule(tickMainPipeEvent, nextWrapperCycle());
    }
}

void
L2CacheWrapper::innerMemPortRecvRespRetry()
{
    panic("L2CacheWrapper should not receive resp retry from inner L2");
}

bool
L2CacheWrapper::cpuSidePortRecvTimingReq(PacketPtr pkt)
{
    // Express snoop packets should bypass any flow control,
    // so always let express snoop packets through even if blocked
    if (pkt->isExpressSnoop()) {
        DPRINTF(L2CacheWrapper, "Express snoop request, forwarding directly to inner cache\n");
        return CacheWrapper::cpuSidePortRecvTimingReq(pkt);
    }
    // If the request is from write_queue(WriteBackClean/CleanEvict etc.),
    // we cannot buffer it and just forward it to inner cache
    if (!pkt->needsResponse()) {
        if (inner_cache_blocked || !requestBuffer.empty()) {
            DPRINTF(L2CacheWrapper, "Inner cache busy, rejecting WQ request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        // directly send to inner cache
        bool success = CacheWrapper::cpuSidePortRecvTimingReq(pkt);
        if (!success) {
            DPRINTF(L2CacheWrapper, "Inner cache busy, rejecting WQ request from CPU side\n");
            inner_cache_blocked = true;
            pending_l1_retry = true;
        }
        DPRINTF(L2CacheWrapper, "WQ request forwarded to inner cache\n");
        return success;
    }

    // Then if the request is from L1 MSHR(ReadEx/ReadShare etc.),
    // we can buffer it
    if (inner_cache_blocked || !requestBuffer.empty()) {
        // If the Wrapper is waiting for inner cache's retry or some pending requestes
        // are in the buffer, we cannot forward it directly to inner cache
        if (requestBuffer.isFull()) {
            DPRINTF(L2CacheWrapper, "Buffer full, rejecting request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        requestBuffer.push(pkt);
        DPRINTF(L2CacheWrapper, "Request buffered, buffer size: %d\n", requestBuffer.size());
        return true;
    }
    // If the Wrapper is not waiting for inner cache's retry and
    // there is no pending request in the buffer,
    // we can try to forward it directly to inner cache
    if (!inner_cpu_port.sendTimingReq(pkt)) {
        inner_cache_blocked = true;
        DPRINTF(L2CacheWrapper, "Inner cache busy, try buffering request and blocking\n");
        if (requestBuffer.isFull()) {
            DPRINTF(L2CacheWrapper, "Buffer full, rejecting request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        requestBuffer.push(pkt);
        DPRINTF(L2CacheWrapper, "Request buffered, buffer size: %d\n", requestBuffer.size());
        return true;
    }
    DPRINTF(L2CacheWrapper, "Request forwarded directly to inner cache\n");
    return true;
}

void
L2CacheWrapper::innerCpuPortRecvReqRetry()
{
    DPRINTF(L2CacheWrapper, "Got req retry from inner cache\n");
    assert(inner_cache_blocked);
    assert(!requestBuffer.empty() || pending_l1_retry);

    // inner cache is not blocked anymore
    inner_cache_blocked = false;

    // resend the request from the buffer
    trySendFromBuffer();
}

void
L2CacheWrapper::trySendFromBuffer()
{
    if (requestBuffer.empty() && !inner_cache_blocked && pending_l1_retry) {
        DPRINTF(L2CacheWrapper, "No pending request in buffer, send retry to L1\n");
        pending_l1_retry = false;
        cpu_side_port.sendRetryReq();
        return;
    }

    if (requestBuffer.empty() || inner_cache_blocked) {
        DPRINTF(L2CacheWrapper, "No pending request in buffer or inner cache is blocked, skipping\n");
        return;
    }

    DPRINTF(L2CacheWrapper, "Attempting to send delayed request from buffer, buffer size: %d\n",
            requestBuffer.size());

    PacketPtr pkt = requestBuffer.front();

    if (!inner_cpu_port.sendTimingReq(pkt)) {
        DPRINTF(L2CacheWrapper, "Send delayed request failed, blocking again\n");
        inner_cache_blocked = true;
    } else {
        requestBuffer.pop();
        DPRINTF(L2CacheWrapper, "Send delayed request successful, popping from buffer, buffer size: %d\n",
                requestBuffer.size());

        if (!requestBuffer.empty()) {
            // schedule trySendFromBuffer next cycle
            if (!trySendEvent.scheduled()) {
                schedule(trySendEvent, nextWrapperCycle());
            }
        } else if (pending_l1_retry) {
            DPRINTF(L2CacheWrapper, "No pending request in buffer, send retry to L1\n");
            pending_l1_retry = false;
            cpu_side_port.sendRetryReq();
        }
    }
}

} // namespace gem5
