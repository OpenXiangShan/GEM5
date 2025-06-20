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
      trySendEvent([this]{ trySendFromBuffer(); }, name())
{
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
