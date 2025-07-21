#include "cpu/o3/fetch/icache_handler.hh"

#include <cstring>
#include <map>

#include "base/types.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "debug/Activity.hh"
#include "debug/Fetch.hh"
#include "debug/FetchFault.hh"
#include "debug/O3CPU.hh"
#include "mem/packet.hh"
#include "params/BaseO3CPU.hh"
#include "sim/byteswap.hh"
#include "sim/core.hh"
#include "sim/eventq.hh"
#include "sim/full_system.hh"
#include "sim/system.hh"

namespace gem5
{

namespace o3
{

ICacheHandler::ICacheHandler(CPU *_cpu)
    : cpu(_cpu), icachePort(this, _cpu), finishTranslationEvent(this)
{
    // Initialize cache configuration from CPU parameters
    cacheBlkSize = cpu->cacheLineSize();
    fetchBufferSize = 66; // Default fetch buffer size

    // Initialize retry state
    cacheBlocked = false;
    retryTid = InvalidThreadID;
}

ICacheHandler::~ICacheHandler()
{
    // Clean up any pending packets
    for (auto& pkt : retryPkt) {
        delete pkt;
    }
    retryPkt.clear();
}

void
ICacheHandler::fetch(Addr vaddr, Addr pc, unsigned size, ThreadID tid,
                     unsigned ftqIndex, FetchCallback callback)
{
    DPRINTF(Fetch, "[tid:%i][ftq:%d] ICacheHandler::fetch for addr %#x, pc=%#lx\n",
            tid, ftqIndex, vaddr, pc);

    // Store the callback for this request
    PendingRequest pendingReq;
    pendingReq.callback = callback;
    pendingReq.tid = tid;
    pendingReq.vaddr = vaddr;
    pendingReq.pc = pc;
    pendingReq.size = size;
    pendingReq.ftqIndex = ftqIndex;

    pendingRequests[{tid, ftqIndex}] = pendingReq;

    // Check for blocking conditions
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n", tid);
        // Return retry status, the request will be handled when unblocked
        FetchCallbackData data = {nullptr, NoFault, nullptr, ftqIndex, nullptr, 0};
        callback(CacheWaitRetry, data);
        return;
    }

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x, pc=%#lx\n",
            tid, vaddr, vaddr, pc);

    // Reset cache request state
    resetCacheReq(tid, ftqIndex);
    CacheRequest& cacheReq = getCacheReq(tid, ftqIndex);
    cacheReq.baseAddr = vaddr;
    cacheReq.totalSize = size;

    // With 66-byte fetchBufferSize, we always need to access 2 cache lines
    handleMultiCacheLineFetch(vaddr, tid, pc, ftqIndex);
}

bool
ICacheHandler::handleMultiCacheLineFetch(Addr vaddr, ThreadID tid, Addr pc, unsigned ftqIndex)
{
    DPRINTF(Fetch, "[tid:%i][ftq:%d] Handling multi-cacheline fetch for addr %#x, pc=%#lx\n",
            tid, ftqIndex, vaddr, pc);

    // Get cache request reference
    CacheRequest& cacheReq = getCacheReq(tid, ftqIndex);

    Addr fetchPC = vaddr;
    unsigned fetchSize = cacheBlkSize - fetchPC % cacheBlkSize;  // Size for first cache line

    DPRINTF(Fetch, "[tid:%i][ftq:%d] Creating first cache line request: addr=%#x, size=%d\n",
            tid, ftqIndex, fetchPC, fetchSize);

    // Create and send first request (tail of first cache line)
    RequestPtr first_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    first_mem_req->taskId(cpu->taskId());
    first_mem_req->setMisalignedFetch();
    first_mem_req->setReqNum(1);

    cacheReq.addRequest(first_mem_req);

    // Initiate translation for first request
    FetchTranslation *trans = new FetchTranslation(this, ftqIndex);
    cpu->mmu->translateTiming(first_mem_req, cpu->thread[tid]->getTC(),
                              trans, BaseMMU::Execute);

    // Prepare second request (head of second cache line)
    fetchPC += fetchSize;  // Move to start of next cache line
    assert(fetchPC % cacheBlkSize == 0);
    fetchSize = fetchBufferSize - fetchSize;  // Remaining size

    DPRINTF(Fetch, "[tid:%i][ftq:%d] Creating second cache line request: addr=%#x, size=%d\n",
            tid, ftqIndex, fetchPC, fetchSize);

    // Create and send second request
    RequestPtr second_mem_req = std::make_shared<Request>(
        fetchPC, fetchSize,
        Request::INST_FETCH, cpu->instRequestorId(), pc,
        cpu->thread[tid]->contextId());

    second_mem_req->taskId(cpu->taskId());
    second_mem_req->setMisalignedFetch();
    second_mem_req->setReqNum(2);

    cacheReq.addRequest(second_mem_req);

    DPRINTF(Fetch, "[tid:%i][ftq:%d] Initiating translation for second cache line\n", tid, ftqIndex);

    // Initiate translation for second request
    FetchTranslation *trans2 = new FetchTranslation(this, ftqIndex);
    cpu->mmu->translateTiming(second_mem_req, cpu->thread[tid]->getTC(),
                              trans2, BaseMMU::Execute);
    return true;
}

void
ICacheHandler::finishTranslation(const Fault &fault, const RequestPtr &mem_req, unsigned ftqIndex)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());

    DPRINTF(Fetch, "[tid:%i] ICacheHandler::finishTranslation for addr %#lx\n",
            tid, mem_req->getVaddr());

    // Check if request is still pending
    auto pendingIt = pendingRequests.find({tid, ftqIndex});
    if (pendingIt == pendingRequests.end()) {
        DPRINTF(Fetch, "[tid:%i] Ignoring translation completed after request cancelled\n", tid);
        return;
    }

    // Handle translation result
    if (fault == NoFault) {
        // Update status from TlbWait to CacheWaitResponse
        updateRequestStatus(tid, ftqIndex, mem_req, CacheWaitResponse);
        handleSuccessfulTranslation(tid, mem_req, pendingIt->second.pc, ftqIndex);
    } else {
        // Update status to AccessFailed
        updateRequestStatus(tid, ftqIndex, mem_req, AccessFailed);
        handleTranslationFault(tid, mem_req, fault, ftqIndex);
    }
}

void
ICacheHandler::handleSuccessfulTranslation(ThreadID tid, const RequestPtr &mem_req, Addr fetchPC, unsigned ftqIndex)
{
    // Check that we're not going off into random memory
    if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
        DPRINTF(Fetch, "Address %#x is outside of physical memory, stopping fetch\n",
                mem_req->getPaddr());

        // Notify callback of failure
        auto pendingIt = pendingRequests.find({tid, ftqIndex});
        if (pendingIt != pendingRequests.end()) {
            FetchCallbackData data = {nullptr, NoFault, mem_req, ftqIndex, nullptr, 0};
            pendingIt->second.callback(AccessFailed, data);
            pendingRequests.erase(pendingIt);
        }
        return;
    }

    // Build packet here.
    PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
    data_pkt->dataDynamic(new uint8_t[mem_req->getSize()]);  // Use request size, not fetchBufferSize
    // All requests are multi-cacheline, always set send right away
    data_pkt->setSendRightAway();

    DPRINTF(Fetch, "[tid:%i] Fetching data for addr %#x, pc=%#lx\n",
                tid, mem_req->getVaddr(), fetchPC);

    // Access the cache.
    if (!icachePort.sendTimingReq(data_pkt)) {
        DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

        data_pkt->setRetriedPkt();
        DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                mem_req->getVaddr());
        retryPkt.push_back(data_pkt);
        retryTid = tid;
        cacheBlocked = true;

        // Notify callback of retry needed
        auto pendingIt = pendingRequests.find({tid, ftqIndex});
        if (pendingIt != pendingRequests.end()) {
            FetchCallbackData data = {nullptr, NoFault, mem_req, ftqIndex, nullptr, 0};
            pendingIt->second.callback(CacheWaitRetry, data);
        }
    } else {
        DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
        DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache response.\n", tid);
    }
}

void
ICacheHandler::handleTranslationFault(ThreadID tid, const RequestPtr &mem_req, const Fault &fault, unsigned ftqIndex)
{
    DPRINTF(FetchFault, "fault, mem_req.addr=%#lx\n", mem_req->getVaddr());

    // Notify callback of fault
    auto pendingIt = pendingRequests.find({tid, ftqIndex});
    if (pendingIt != pendingRequests.end()) {
        FetchCallbackData data = {nullptr, fault, mem_req, ftqIndex, nullptr, 0};
        pendingIt->second.callback(AccessFailed, data);
        pendingRequests.erase(pendingIt);
    }
}

void
ICacheHandler::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());
    assert(pkt->req->isMisalignedFetch() && "Only multi-cacheline fetch is supported");

    // Find which FTQ this packet belongs to
    unsigned ftqIndex = determineFTQIndex(tid, pkt);
    if (ftqIndex >= 2) {
        DPRINTF(Fetch, "[tid:%i] Packet doesn't belong to active requests, ignoring\n", tid);
        delete pkt;
        return;
    }

    // Let processMultiCacheLineCompletion handle the callback
    bool allCompleted = processMultiCacheLineCompletion(tid, pkt, ftqIndex);
    // processMultiCacheLineCompletion now handles callback and cleanup
}

bool
ICacheHandler::processMultiCacheLineCompletion(ThreadID tid, PacketPtr pkt, unsigned ftqIndex)
{
    DPRINTF(Fetch, "[tid:%i][ftq:%d] Processing dual cacheline fetch completion.\n", tid, ftqIndex);

    // Mark this packet as completed in the cache request
    CacheRequest& cacheReq = getCacheReq(tid, ftqIndex);
    bool found_packet = cacheReq.markCompletedAndStorePacket(pkt);
    if (!found_packet) {
        DPRINTF(Fetch, "[tid:%i][ftq:%d] Packet doesn't match current requests, deleting pkt %#lx\n",
                tid, ftqIndex, pkt->getAddr());
        return false;
    }

    // Check if we're still waiting for other packets
    if (!cacheReq.allCompleted()) {
        DPRINTF(Fetch, "[tid:%i][ftq:%d] Waiting for remaining packets. Completed: %d, Total: %d\n",
                tid, ftqIndex, cacheReq.completedPackets, cacheReq.packets.size());

        // Handle retry case - need to send the missing request
        if (pkt->isRetriedPkt()) {
            handleRetryPkt(tid, pkt);
        }

        return false;  // Return false to indicate we're still waiting
    }

    // All packets have arrived - merge data and prepare for callback
    DPRINTF(Fetch, "[tid:%i] All packets arrived, merging data.\n", tid);

    // Find the packets by request number
    PacketPtr firstPkt = nullptr;
    PacketPtr secondPkt = nullptr;

    for (size_t i = 0; i < cacheReq.packets.size(); i++) {
        if (cacheReq.requests[i]->getReqNum() == 1) {
            firstPkt = cacheReq.packets[i];
        } else if (cacheReq.requests[i]->getReqNum() == 2) {
            secondPkt = cacheReq.packets[i];
        }
    }

    assert(firstPkt && secondPkt);

    // Allocate buffer for merged data
    unsigned totalSize = firstPkt->getSize() + secondPkt->getSize();
    uint8_t* mergedData = new uint8_t[totalSize];

    // Copy data from both packets in order
    memcpy(mergedData, firstPkt->getConstPtr<uint8_t>(), firstPkt->getSize());
    memcpy(mergedData + firstPkt->getSize(), secondPkt->getConstPtr<uint8_t>(), secondPkt->getSize());

    DPRINTF(Fetch, "[tid:%i] Data merged successfully: first_size=%d, second_size=%d, total_size=%d\n",
            tid, firstPkt->getSize(), secondPkt->getSize(), totalSize);

    // Prepare callback with merged data
    auto pendingIt = pendingRequests.find({tid, ftqIndex});
    if (pendingIt != pendingRequests.end()) {
        FetchCallbackData data;
        data.pkt = firstPkt; // Keep reference to first packet for compatibility
        data.fault = NoFault;
        data.req = firstPkt->req;
        data.ftqIndex = ftqIndex;
        data.mergedData = mergedData;
        data.dataSize = totalSize;

        // Call the callback with merged data
        pendingIt->second.callback(AccessComplete, data);
        pendingRequests.erase(pendingIt);

        // Clean up packets after callback
        delete firstPkt;
        delete secondPkt;
    } else {
        // No pending request found, clean up
        delete[] mergedData;
        delete firstPkt;
        delete secondPkt;
    }

    DPRINTF(Fetch, "[tid:%i] Dual cacheline fetch completion processed successfully.\n", tid);
    return true;
}

void
ICacheHandler::recvReqRetry()
{
    if (retryPkt.size() == 0) {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
        return;
    }
    assert(cacheBlocked);

    for (auto it = retryPkt.begin(); it != retryPkt.end();) {
        if (icachePort.sendTimingReq(*it)) {
            it = retryPkt.erase(it);
        } else {
            it++;
        }
    }

    if (retryPkt.size() == 0) {
        retryTid = InvalidThreadID;
        cacheBlocked = false;
    }
}

void
ICacheHandler::handleRetryPkt(ThreadID tid, PacketPtr pkt)
{
    DPRINTF(Fetch, "[tid:%i] Retried pkt.\n", tid);

    // Find which FTQ index this packet belongs to
    unsigned ftqIndex = determineFTQIndex(tid, pkt);
    if (ftqIndex >= 2) {
        DPRINTF(Fetch, "[tid:%i] Retry packet doesn't belong to active fetch requests\n", tid);
        return;
    }

    // Find the missing request that needs to be sent
    RequestPtr missingReq = nullptr;
    CacheRequest& cacheReq = getCacheReq(tid, ftqIndex);
    for (size_t i = 0; i < cacheReq.requests.size(); i++) {
        if (cacheReq.packets[i] == nullptr) {  // This request hasn't completed yet
            missingReq = cacheReq.requests[i];
            break;
        }
    }

    if (missingReq) {
        DPRINTF(Fetch, "[tid:%i] send next pkt, addr: %#x, size: %d\n",
                tid, missingReq->getVaddr(), missingReq->getSize());

        FetchTranslation *trans = new FetchTranslation(this, ftqIndex);
        cpu->mmu->translateTiming(missingReq, cpu->thread[tid]->getTC(),
                                  trans, BaseMMU::Execute);
    }
}

unsigned
ICacheHandler::determineFTQIndex(ThreadID tid, PacketPtr pkt)
{
    // Simple implementation - check which cache request this packet belongs to
    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        auto cacheReqIt = cacheRequests.find({tid, ftqIndex});
        if (cacheReqIt != cacheRequests.end()) {
            for (const auto& req : cacheReqIt->second.requests) {
                if (req == pkt->req) {
                    return ftqIndex;
                }
            }
        }
    }
    return 2; // Invalid FTQ index
}

void
ICacheHandler::cancelRequests(ThreadID tid)
{
    DPRINTF(Fetch, "[tid:%i] ICacheHandler::cancelRequests\n", tid);

    // Cancel all pending requests for this thread
    for (auto it = pendingRequests.begin(); it != pendingRequests.end(); ) {
        if (it->second.tid == tid) {
            // Notify callback of cancellation
            FetchCallbackData data = {nullptr, NoFault, nullptr, it->second.ftqIndex, nullptr, 0};
            it->second.callback(Cancelled, data);
            it = pendingRequests.erase(it);
        } else {
            ++it;
        }
    }

    // Clear cache requests for this thread
    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        resetCacheReq(tid, ftqIndex);
    }

    // Clean up retry packets for this thread
    for (auto it = retryPkt.begin(); it != retryPkt.end(); ) {
        ThreadID pktTid = cpu->contextToThread((*it)->req->contextId());
        if (pktTid == tid) {
            delete *it;
            it = retryPkt.erase(it);
        } else {
            ++it;
        }
    }

    if (retryTid == tid) {
        retryTid = InvalidThreadID;
        if (retryPkt.empty()) {
            cacheBlocked = false;
        }
    }
}

ICacheHandler::CacheRequest&
ICacheHandler::getCacheReq(ThreadID tid, unsigned ftqIndex)
{
    return cacheRequests[{tid, ftqIndex}];
}

void
ICacheHandler::resetCacheReq(ThreadID tid, unsigned ftqIndex)
{
    cacheRequests[{tid, ftqIndex}].reset();
}

void
ICacheHandler::updateRequestStatus(ThreadID tid, unsigned ftqIndex, const RequestPtr& req, CacheRequestStatus status)
{
    CacheRequest& cacheReq = getCacheReq(tid, ftqIndex);
    for (size_t i = 0; i < cacheReq.requests.size(); ++i) {
        if (cacheReq.requests[i] == req) {
            cacheReq.requestStatus[i] = status;
            DPRINTF(Fetch, "[tid:%i][ftq:%d] updateRequestStatus[%d]: -> %s\n",
                    tid, ftqIndex, i,
                    (status == TlbWait ? "TlbWait" :
                     status == CacheWaitResponse ? "CacheWaitResponse" :
                     status == AccessComplete ? "AccessComplete" :
                     status == AccessFailed ? "AccessFailed" : "Unknown"));
            return;
        }
    }
    DPRINTF(Fetch, "[tid:%i][ftq:%d] updateRequestStatus: request not found\n", tid, ftqIndex);
}

void
ICacheHandler::FinishTranslationEvent::process()
{
    // This should be handled by the callback mechanism now
    // But keeping for compatibility if needed
    DPRINTF(Fetch, "ICacheHandler FinishTranslationEvent::process\n");
}

// IcachePort implementation
ICacheHandler::IcachePort::IcachePort(ICacheHandler *_handler, CPU *_cpu) :
    RequestPort(_cpu->name() + ".icache_port", _cpu), handler(_handler)
{}

bool
ICacheHandler::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "ICacheHandler received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));

    DPRINTF(Fetch, "received pkt addr=%#lx, req addr=%#lx\n", pkt->getAddr(),
            pkt->req->getVaddr());

    handler->processCacheCompletion(pkt);

    return true;
}

void
ICacheHandler::IcachePort::recvReqRetry()
{
    handler->recvReqRetry();
}

// Status mapping and query functions

CacheRequestStatus
ICacheHandler::getOverallCacheStatus(ThreadID tid) const
{
    assert(tid < MaxThreads);

    // Check all active FTQ indices to determine overall cache status
    // Priority: Failed > Retry > TlbWait > CacheWaitResponse > Complete > Idle
    bool hasRetry = false;
    bool hasTlbWait = false;
    bool hasCacheWait = false;
    bool hasComplete = false;
    bool hasActive = false;

    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        auto it = cacheRequests.find({tid, ftqIndex});
        if (it != cacheRequests.end() && !it->second.requests.empty()) {
            hasActive = true;
            CacheRequestStatus status = it->second.getOverallStatus();

            switch (status) {
                case AccessFailed:
                    return AccessFailed;  // Highest priority
                case CacheWaitRetry:
                    hasRetry = true;
                    break;
                case TlbWait:
                    hasTlbWait = true;
                    break;
                case CacheWaitResponse:
                    hasCacheWait = true;
                    break;
                case AccessComplete:
                    hasComplete = true;
                    break;
                default:
                    break;
            }
        }
    }

    // Return status by priority
    if (hasRetry) return CacheWaitRetry;
    if (hasTlbWait) return TlbWait;
    if (hasCacheWait) return CacheWaitResponse;
    if (hasComplete) return AccessComplete;

    return hasActive ? CacheIdle : CacheIdle;
}

bool
ICacheHandler::allActiveFTQCompleted(ThreadID tid) const
{
    // Check if all ICacheHandler requests are completed
    // Since we don't have direct access to fetch2Coord, we check all possible FTQ indices
    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        auto it = cacheRequests.find({tid, ftqIndex});
        if (it != cacheRequests.end() && !it->second.requests.empty()) {
            if (!it->second.allCompleted()) {
                return false;
            }
        }
    }
    return true;
}

bool
ICacheHandler::hasPendingCacheRequests(ThreadID tid) const
{
    // Check for any active cache operations (excluding terminal states)
    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        auto it = cacheRequests.find({tid, ftqIndex});
        if (it != cacheRequests.end() && !it->second.requests.empty()) {
            CacheRequestStatus overallStatus = it->second.getOverallStatus();
            if (overallStatus == TlbWait ||
                overallStatus == CacheWaitResponse ||
                overallStatus == CacheWaitRetry) {
                return true;
            }
        }
    }
    return false;
}

} // namespace o3
} // namespace gem5
