#include "mem/cache/L2CacheWrapper.hh"

#include "base/trace.hh"
#include "cache.hh"
#include "debug/L2CacheWrapper.hh"
#include "params/CacheWrapper.hh"

namespace gem5
{

L2CacheWrapper::L2CacheWrapper(const L2CacheWrapperParams &p)
    : CacheWrapper(p),
      buffer_size(p.buffer_size),
      trySendEvent([this]{ trySendFromBuffer(); }, name()),
      processDelayedResponsesEvent([this]{ processDelayedResponses(); }, name()),
      min_response_latency(p.min_response_latency),
      max_response_latency(p.max_response_latency)
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

    DPRINTF(L2CacheWrapper, "Found matching tracked request for addr: %#x. Applying delay.\n", pkt->getAddr());
    pending_l3_requests.erase(it);

    Cycles delay_cycles{0};
    if (max_response_latency > min_response_latency) {
        Tick min_ticks = cyclesToTicks(min_response_latency);
        Tick max_ticks = cyclesToTicks(max_response_latency);
        delay_cycles = ticksToCycles(min_ticks + (Tick)(rand() % (max_ticks - min_ticks + 1)));
    } else {
        delay_cycles = min_response_latency;
    }

    Tick ready_tick = curTick() + cyclesToTicks(delay_cycles);
    DPRINTF(L2CacheWrapper, "Response for addr %#x will be delayed by %d cycles, "
                            "ready at tick %d\n", pkt->getAddr(), delay_cycles, ready_tick);

    // Get the ready tick of the next response to be processed before adding the new one
    Tick next_ready_tick = delayed_responses.empty() ? MaxTick : delayed_responses.top().readyTick;

    delayed_responses.push({pkt, ready_tick});

    // If the new response is ready sooner than any other pending response,
    // or if the queue was empty, we need to schedule/reschedule the event.
    if (ready_tick < next_ready_tick) {
        if (processDelayedResponsesEvent.scheduled()) {
            deschedule(processDelayedResponsesEvent);
        }
        schedule(processDelayedResponsesEvent, ready_tick);
    }

    return true;
}

void
L2CacheWrapper::processDelayedResponses()
{
    if (delayed_responses.empty() || response_port_blocked) {
        return;
    }

    const DelayedResp& resp_to_send = delayed_responses.top();

    if (curTick() < resp_to_send.readyTick) {
        if (processDelayedResponsesEvent.scheduled()) {
             deschedule(processDelayedResponsesEvent);
        }
        schedule(processDelayedResponsesEvent, resp_to_send.readyTick);
        return;
    }

    DPRINTF(L2CacheWrapper, "Attempting to send delayed response "
                            "for addr: %#x to inner L2\n", resp_to_send.pkt->getAddr());

    if (!inner_mem_port.sendTimingResp(resp_to_send.pkt)) {
        DPRINTF(L2CacheWrapper, "Inner L2 is busy, cannot send response. Blocking.\n");
        response_port_blocked = true;
    } else {
        DPRINTF(L2CacheWrapper, "Successfully sent delayed response "
                                "for addr: %#x to inner L2\n", resp_to_send.pkt->getAddr());
        delayed_responses.pop();

        if (!delayed_responses.empty()) {
            Tick next_ready = delayed_responses.top().readyTick;
            if (processDelayedResponsesEvent.scheduled()) {
                 deschedule(processDelayedResponsesEvent);
            }
            schedule(processDelayedResponsesEvent, std::max(next_ready, nextCycle()));
        }
    }
}

void
L2CacheWrapper::innerMemPortRecvRespRetry()
{
    DPRINTF(L2CacheWrapper, "Got resp retry from inner L2. Unblocking.\n");
    assert(response_port_blocked);
    response_port_blocked = false;

    if (!processDelayedResponsesEvent.scheduled() && !delayed_responses.empty()) {
        schedule(processDelayedResponsesEvent, nextCycle());
    }
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
        if (inner_cache_blocked || !request_buffer.empty()) {
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
    if (inner_cache_blocked || !request_buffer.empty()) {
        // If the Wrapper is waiting for inner cache's retry or some pending requestes
        // are in the buffer, we cannot forward it directly to inner cache
        if (request_buffer.size() >= buffer_size) {
            DPRINTF(L2CacheWrapper, "Buffer full, rejecting request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        request_buffer.push_back(pkt);
        DPRINTF(L2CacheWrapper, "Request buffered, buffer size: %d\n", request_buffer.size());
        return true;
    }
    // If the Wrapper is not waiting for inner cache's retry and
    // there is no pending request in the buffer,
    // we can try to forward it directly to inner cache
    if (!inner_cpu_port.sendTimingReq(pkt)) {
        inner_cache_blocked = true;
        DPRINTF(L2CacheWrapper, "Inner cache busy, try buffering request and blocking\n");
        if (request_buffer.size() >= buffer_size) {
            DPRINTF(L2CacheWrapper, "Buffer full, rejecting request from CPU side\n");
            pending_l1_retry = true;
            return false;
        }
        request_buffer.push_back(pkt);
        DPRINTF(L2CacheWrapper, "Request buffered, buffer size: %d\n", request_buffer.size());
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
    assert(!request_buffer.empty() || pending_l1_retry);

    // inner cache is not blocked anymore
    inner_cache_blocked = false;

    // resend the request from the buffer
    trySendFromBuffer();
}

void
L2CacheWrapper::trySendFromBuffer()
{
    if (request_buffer.empty() && !inner_cache_blocked && pending_l1_retry) {
        DPRINTF(L2CacheWrapper, "No pending request in buffer, send retry to L1\n");
        pending_l1_retry = false;
        cpu_side_port.sendRetryReq();
        return;
    }

    if (request_buffer.empty() || inner_cache_blocked) {
        DPRINTF(L2CacheWrapper, "No pending request in buffer or inner cache is blocked, skipping\n");
        return;
    }

    DPRINTF(L2CacheWrapper, "Attempting to send delayed request from buffer, buffer size: %d\n",
            request_buffer.size());

    PacketPtr pkt = request_buffer.front();

    if (!inner_cpu_port.sendTimingReq(pkt)) {
        DPRINTF(L2CacheWrapper, "Send delayed request failed, blocking again\n");
        inner_cache_blocked = true;
    } else {
        request_buffer.pop_front();
        DPRINTF(L2CacheWrapper, "Send delayed request successful, popping from buffer, buffer size: %d\n",
                request_buffer.size());

        if (!request_buffer.empty()) {
            // schedule trySendFromBuffer next cycle
            if (!trySendEvent.scheduled()) {
                schedule(trySendEvent, nextCycle());
            }
        } else if (pending_l1_retry) {
            DPRINTF(L2CacheWrapper, "No pending request in buffer, send retry to L1\n");
            pending_l1_retry = false;
            cpu_side_port.sendRetryReq();
        }
    }
}

} // namespace gem5
