#include <cstring>
#include <map>

#include "base/types.hh"
#include "cpu/nop_static_inst.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fetch/fetch.hh"
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

Fetch::IcachePort::IcachePort(Fetch *_fetch, CPU *_cpu) :
    RequestPort(_cpu->name() + ".icache_port", _cpu), fetch(_fetch)
{}

bool
Fetch::handleMultiCacheLineFetch(Addr vaddr, ThreadID tid, Addr pc, unsigned ftqIndex)
{
    assert(ftqIndex < 2);
    DPRINTF(Fetch, "[tid:%i][ftq:%d] Handling multi-cacheline fetch for addr %#x, pc=%#lx\n",
            tid, ftqIndex, vaddr, pc);
    // Transition to WaitingCache state when initiating cache access
    setThreadStatus(tid, WaitingCache);

    // Reset cache request state for this thread and FTQ index
    CacheRequest& cacheReqRef = getCacheReq(tid, ftqIndex);
    cacheReqRef.reset();
    cacheReqRef.baseAddr = vaddr;
    cacheReqRef.totalSize = fetchBufferSize;

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

    cacheReqRef.addRequest(first_mem_req); // packet will be created later

    // Initiate translation for first request
    updateCacheRequestStatusByRequest(tid, first_mem_req, TlbWait, ftqIndex);
    setAllFetchStalls(StallReason::ITlbStall);
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

    cacheReqRef.addRequest(second_mem_req);  // Add second request to cache request

    // Since we always have dual cacheline fetches now, check for retry state
    if (cacheReqRef.getOverallStatus() == CacheWaitRetry) {
        return true;
    }

    DPRINTF(Fetch, "[tid:%i][ftq:%d] Initiating translation for second cache line\n", tid, ftqIndex);

    // Initiate translation for second request
    updateCacheRequestStatusByRequest(tid, second_mem_req, TlbWait, ftqIndex);
    setAllFetchStalls(StallReason::ITlbStall);
    FetchTranslation *trans2 = new FetchTranslation(this, ftqIndex);
    cpu->mmu->translateTiming(second_mem_req, cpu->thread[tid]->getTC(),
                              trans2, BaseMMU::Execute);
    return true;
}

bool
Fetch::processMultiCacheLineCompletion(ThreadID tid, PacketPtr pkt, unsigned ftqIndex)
{
    assert(ftqIndex < 2);
    DPRINTF(Fetch, "[tid:%i][ftq:%d] Processing dual cacheline fetch completion.\n", tid, ftqIndex);

    // Mark this packet as completed in the cache request (this also stores the packet)
    CacheRequest& cacheReqRef = getCacheReq(tid, ftqIndex);
    bool found_packet = cacheReqRef.markCompletedAndStorePacket(pkt);
    if (!found_packet) {
        DPRINTF(Fetch, "[tid:%i][ftq:%d] Packet doesn't match current requests, deleting pkt %#lx\n",
                tid, ftqIndex, pkt->getAddr());
        return false;
    } else {
        DPRINTF(Fetch, "[tid:%i][ftq:%d] updateCacheRequestStatus[0] to %s, updateCacheRequestStatus[1] to %s\n",
                tid, ftqIndex,
                cacheRequestStatusStr[cacheReqRef.requestStatus[0]],
                cacheRequestStatusStr[cacheReqRef.requestStatus[1]]);
    }

    // Check if we're still waiting for other packets
    if (!cacheReqRef.allCompleted()) {
        DPRINTF(Fetch, "[tid:%i][ftq:%d] Waiting for remaining packets. Completed: %d, Total: %d\n",
                tid, ftqIndex, cacheReqRef.completedPackets, cacheReqRef.packets.size());

        // Handle retry case - need to send the missing request
        if (pkt->isRetriedPkt()) {
            handleRetryPkt(tid, pkt);
        }

        return false;  // Return false to indicate we're still waiting
    }

    // All packets have arrived - merge them directly into fetchBuffer
    DPRINTF(Fetch, "[tid:%i] All packets arrived, merging data into fetchBuffer.\n", tid);

    // Find the packets by request number
    PacketPtr firstPkt = nullptr;
    PacketPtr secondPkt = nullptr;

    for (size_t i = 0; i < cacheReqRef.packets.size(); i++) {
        if (cacheReqRef.requests[i]->getReqNum() == 1) {
            firstPkt = cacheReqRef.packets[i];
        } else if (cacheReqRef.requests[i]->getReqNum() == 2) {
            secondPkt = cacheReqRef.packets[i];
        }
    }

    assert(firstPkt && secondPkt);

    // Copy merged data directly into fetchBuffer
    FetchBuffer& bufferRef = getFetchBuffer(tid, ftqIndex);
    memcpy(bufferRef.data, firstPkt->getConstPtr<uint8_t>(), firstPkt->getSize());
    memcpy(bufferRef.data + firstPkt->getSize(), secondPkt->getConstPtr<uint8_t>(), secondPkt->getSize());
    bufferRef.valid = true;

    // Clean up the packets
    delete firstPkt;
    delete secondPkt;

    DPRINTF(Fetch, "[tid:%i] Dual cacheline fetch completion processed successfully.\n", tid);
    return true;
}

void
Fetch::processCacheCompletion(PacketPtr pkt)
{
    ThreadID tid = cpu->contextToThread(pkt->req->contextId());
    assert(pkt->req->isMisalignedFetch() && "Only multi-cacheline fetch is supported");

    // Use determineFTQIndex() to find which FTQ this packet belongs to
    unsigned ftqIndex = determineFTQIndex(tid, pkt);
    if (ftqIndex >= 2) {
        DPRINTF(Fetch, "[tid:%i] Packet doesn't belong to active requests, ignoring\n", tid);
        ++fetchStats.icacheSquashes;
        delete pkt;
        return;
    }

    bool allCompleted = processMultiCacheLineCompletion(tid, pkt, ftqIndex);
    // If we're still waiting for another packet, return early
    if (!allCompleted) {
        return;
    }

    // Check if this completion should be processed
    // Either thread is waiting for cache, or cache just completed
    CacheRequestStatus cacheStatus = cacheReq[tid][ftqIndex].getOverallStatus();
    if (!hasPendingCacheRequests(tid) && cacheStatus != AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Thread not waiting for cache and no completion, ignoring\n", tid);
        ++fetchStats.icacheSquashes;
        return;
    }

    // Data has been merged into fetchBuffer, we can proceed
    DPRINTF(Fetch, "[tid:%i] All misaligned packets received and merged.\n", tid);

    assert(!cpu->switchedOut());

    // Reset usedUpFetchTargets flag when we get new fetch data
    // This allows fetch to continue with the current FTQ entry
    if (usedUpFetchTargets) {
        DPRINTF(Fetch, "[tid:%i][ftq:%d] Resetting usedUpFetchTargets after cache completion, "
                "fetchBufferPC=%#x\n", tid, ftqIndex, getFetchBuffer(tid, ftqIndex).startPC);
        usedUpFetchTargets = false;
    }

    // Verify fetchBufferPC alignment with FTQ for decoupled frontend
    FetchBuffer& bufferRef = getFetchBuffer(tid, ftqIndex);
    if (isDecoupledFrontend() && bufferRef.valid) {
        if (isBTBPred() && hasDualFTQEntries(tid)) {
            auto [ftq0_pc, ftq1_pc] = getDualFTQPCs(tid);
            if (ftqIndex == 0 && bufferRef.startPC != ftq0_pc) {
                panic("fetchBufferPC %#x should be aligned with FTQ startPC %#x",
                      bufferRef.startPC, ftq0_pc);
            } else if (ftqIndex == 1 && bufferRef.startPC != ftq1_pc) {
                panic("fetchBufferPC %#x should be aligned with FTQ startPC %#x",
                      bufferRef.startPC, ftq1_pc);
            }
            DPRINTF(Fetch, "[tid:%i][ftq:%d] Verified fetchBufferPC %#x matches FTQ startPC %#x\n",
                    tid, ftqIndex, bufferRef.startPC, ftqIndex == 0 ? ftq0_pc : ftq1_pc);
        } else if (isFTBPred() && dbpftb->fetchTargetAvailable()) {
            auto& ftq_entry = dbpftb->getSupplyingFetchTarget();
            if (bufferRef.startPC != ftq_entry.startPC) {
                panic("fetchBufferPC %#x should be aligned with FTQ startPC %#x",
                      bufferRef.startPC, ftq_entry.startPC);
            }
            DPRINTF(Fetch, "[tid:%i][ftq:%d] Verified fetchBufferPC %#x matches FTQ startPC %#x\n",
                    tid, ftqIndex, bufferRef.startPC, ftq_entry.startPC);
        }
    }

    // Check if all active FTQs have completed their cache requests
    if (allActiveFTQCompleted(tid)) {
        DPRINTF(Fetch, "[tid:%i] All active FTQs completed\n", tid);
        // Wake up the CPU and switch to active
        cpu->wakeCPU();
        DPRINTF(Activity, "[tid:%i] Activating fetch due to cache completion\n", tid);
        switchToActive();

        // Complete cache request and transition to appropriate state
        if (checkStall(tid)) {
            setThreadStatus(tid, Blocked);
        } else {
            // Transition from WaitingCache back to Running when cache access completes
            setThreadStatus(tid, Running);
        }
    } else {
        DPRINTF(Fetch, "[tid:%i] FTQ %d completed, but other FTQs still pending\n", tid, ftqIndex);
    }
}

bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc, unsigned ftqIndex)
{
    assert(!cpu->switchedOut());

    // Check for blocking conditions
    if (cacheBlocked) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, cache blocked\n", tid);
        setAllFetchStalls(StallReason::IcacheStall);
        return false;
    } else if (checkInterrupt(pc) && !delayedCommit[tid]) {
        // Hold off fetch from getting new instructions when:
        // Cache is blocked, or
        // while an interrupt is pending and we're not in PAL mode, or
        // fetch is switched out.
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n", tid);
        setAllFetchStalls(StallReason::IntStall);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x, pc=%#lx\n",
            tid, vaddr, vaddr, pc);

    // Mark this FTQ as active when we start fetching
    fetch2Coord[tid].ftqActive[ftqIndex] = true;

    DPRINTF(Fetch, "[tid:%i] Setting ftqActive[%d] = true for cache request\n",
            tid, ftqIndex);

    // With 66-byte fetchBufferSize, we always need to access 2 cache lines
    return handleMultiCacheLineFetch(vaddr, tid, pc, ftqIndex);
}

bool
Fetch::validateTranslationRequest(ThreadID tid, const RequestPtr &mem_req, unsigned ftqIndex)
{
    // Check if this request belongs to current cache request
    bool isExpectedReq = false;
    for (size_t i = 0; i < cacheReq[tid][ftqIndex].requests.size(); i++) {
        if (mem_req == cacheReq[tid][ftqIndex].requests[i]) {
            isExpectedReq = true;
            break;
        }
    }

    // Check if request should be processed using new state system
    if (!isExpectedReq || !hasPendingCacheRequests(tid)) {
        DPRINTF(Fetch, "[tid:%i] Ignoring translation completed after squash or unexpected request\n", tid);
        DPRINTF(Fetch, "[tid:%i] Ignoring req addr=%#lx\n", tid, mem_req->getVaddr());
        ++fetchStats.tlbSquashes;
        return false;
    }

    return true;
}

void
Fetch::handleSuccessfulTranslation(ThreadID tid, const RequestPtr &mem_req, Addr fetchPC, unsigned ftqIndex)
{
    // Check that we're not going off into random memory
    if (!cpu->system->isMemAddr(mem_req->getPaddr())) {
        DPRINTF(Fetch, "Address %#x is outside of physical memory, stopping fetch, %lu\n",
                mem_req->getPaddr(), curTick());

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed, ftqIndex);
        setAllFetchStalls(StallReason::OtherFetchStall);
        // Note: Don't reset here, let the caller handle cleanup based on overall status
        return;
    }

    // Build packet here.
    PacketPtr data_pkt = new Packet(mem_req, MemCmd::ReadReq);
    data_pkt->dataDynamic(new uint8_t[fetchBufferSize]);
    // All requests are multi-cacheline, always set send right away
    data_pkt->setSendRightAway();

    DPRINTF(Fetch, "[tid:%i] Fetching data for addr %#x, pc=%#lx\n",
                tid, mem_req->getVaddr(), fetchPC);

    FetchBuffer& bufferRef = getFetchBuffer(tid, ftqIndex);
    bufferRef.startPC = fetchPC;
    bufferRef.valid = false;
    DPRINTF(Fetch, "Fetch: Doing instruction read.\n");

    fetchStats.cacheLines++;

    // Access the cache.
    if (!icachePort.sendTimingReq(data_pkt)) {
        DPRINTF(Fetch, "[tid:%i] Out of MSHRs!\n", tid);

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitRetry, ftqIndex);
        data_pkt->setRetriedPkt();
        DPRINTF(Fetch, "[tid:%i] mem_req.addr=%#lx needs retry.\n", tid,
                mem_req->getVaddr());
        setAllFetchStalls(StallReason::IcacheStall);
        retryPkt.push_back(data_pkt);
        retryTid = tid;
        cacheBlocked = true;
    } else {
        DPRINTF(Fetch, "[tid:%i] Doing Icache access.\n", tid);
        DPRINTF(Activity, "[tid:%i] Activity: Waiting on I-cache response.\n", tid);
        lastIcacheStall[tid] = curTick();

        // Update cache request status using new interface
        updateCacheRequestStatusByRequest(tid, mem_req, CacheWaitResponse, ftqIndex);
        setAllFetchStalls(StallReason::IcacheStall);
        // Notify Fetch Request probe when a packet containing a fetch request is successfully sent
        ppFetchRequestSent->notify(mem_req);
    }
}

void
Fetch::handleTranslationFault(ThreadID tid, const RequestPtr &mem_req, const Fault &fault, unsigned ftqIndex)
{
    DPRINTF(FetchFault, "fault, mem_req.addr=%#lx\n", mem_req->getVaddr());

    // Don't send an instruction to decode if we can't handle it.
    if (!(numInst < fetchWidth) || !(fetchQueue[tid].size() < fetchQueueSize)) {
        if (finishTranslationEvent.scheduled() && finishTranslationEvent.getReq() != mem_req) {
            DPRINTF(FetchFault, "fault, finishTranslationEvent.getReq().addr=%#lx, mem_req.addr=%#lx\n",
                    finishTranslationEvent.getReq()->getVaddr(), mem_req->getVaddr());
            return;
        }
        assert(!finishTranslationEvent.scheduled());
        finishTranslationEvent.setFault(fault);
        finishTranslationEvent.setReq(mem_req);
        cpu->schedule(finishTranslationEvent, cpu->clockEdge(Cycles(1)));
        return;
    }

    DPRINTF(Fetch, "[tid:%i] Got back req with addr %#x but expected base addr %#x\n",
            tid, mem_req->getVaddr(), cacheReq[tid][ftqIndex].baseAddr);

    // Update new cache request status system
    updateCacheRequestStatusByRequest(tid, mem_req, AccessFailed, ftqIndex);

    // Translation faulted, icache request won't be sent.
    cacheReq[tid][ftqIndex].reset();

    // Send the fault to commit.  This thread will not do anything
    // until commit handles the fault.  The only other way it can
    // wake up is if a squash comes along and changes the PC.
    const PCStateBase &fetch_pc = *pc[tid];

    DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
    // We will use a nop in order to carry the fault.
    DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
            fetch_pc, fetch_pc, false);
    instruction->setVersion(localSquashVer);
    instruction->setNotAnInst();

    instruction->setPredTarg(fetch_pc);
    instruction->fault = fault;
    std::unique_ptr<PCStateBase> next_pc(fetch_pc.clone());
    instruction->staticInst->advancePC(*next_pc);
    set(instruction->predPC, next_pc);

    wroteToTimeBuffer = true;

    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();

    setThreadStatus(tid, TrapPending);
    setAllFetchStalls(StallReason::TrapStall);

    DPRINTF(Fetch, "[tid:%i] Blocked, need to handle the trap.\n", tid);
    DPRINTF(Fetch, "[tid:%i] fault (%s) detected @ PC %s.\n",
            tid, fault->name(), *pc[tid]);
}

void
Fetch::finishTranslation(const Fault &fault, const RequestPtr &mem_req, unsigned ftqIndex)
{
    ThreadID tid = cpu->contextToThread(mem_req->contextId());

    // For multi-cacheline fetch, use the stored base address
    // Both requests should use the same fetchBufferPC
    Addr fetchPC = cacheReq[tid][ftqIndex].baseAddr;

    assert(!cpu->switchedOut());

    // Wake up CPU if it was idle
    cpu->wakeCPU();

    DPRINTF(Fetch, "[tid:%i] Translation completed for addr %#lx\n",
            tid, mem_req->getVaddr());

    // Validate if this request should be processed
    if (!validateTranslationRequest(tid, mem_req, ftqIndex)) {
        return;
    }

    // Handle translation result
    if (fault == NoFault) {
        handleSuccessfulTranslation(tid, mem_req, fetchPC, ftqIndex);
    } else {
        handleTranslationFault(tid, mem_req, fault, ftqIndex);
    }

    _status = updateFetchStatus();
}

void
Fetch::recvReqRetry()
{
    if (retryPkt.size() == 0) {
        assert(retryTid == InvalidThreadID);
        // Access has been squashed since it was sent out.  Just clear
        // the cache being blocked.
        cacheBlocked = false;
        return;
    }
    assert(cacheBlocked);
    // assert(retryTid != InvalidThreadID);
    // assert(cacheReq[retryTid].getOverallStatus() == CacheWaitRetry);

    for (auto it = retryPkt.begin(); it != retryPkt.end();) {
        if (icachePort.sendTimingReq(*it)) {
            unsigned ftqIndex = determineFTQIndex(retryTid, *it);
            assert(ftqIndex < 2 && "retryPkt should belong to active FTQs");
            // Use new cache state management with specific RequestPtr
            updateCacheRequestStatusByRequest(retryTid, (*it)->req, CacheWaitResponse, ftqIndex);
            // Notify Fetch Request probe when a retryPkt is successfully sent.
            // Note that notify must be called before retryPkt is set to NULL.
            ppFetchRequestSent->notify((*it)->req);
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
Fetch::handleRetryPkt(ThreadID tid, PacketPtr pkt)
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
    CacheRequest& cacheReqRef = getCacheReq(tid, ftqIndex);
    for (size_t i = 0; i < cacheReqRef.requests.size(); i++) {
        if (cacheReqRef.packets[i] == nullptr) {  // This request hasn't completed yet
            missingReq = cacheReqRef.requests[i];
            break;
        }
    }

    if (missingReq) {
        DPRINTF(Fetch, "[tid:%i] send next pkt, addr: %#x, size: %d\n",
                tid, missingReq->getVaddr(), missingReq->getSize());

        updateCacheRequestStatusByRequest(tid, missingReq, TlbWait, ftqIndex);
        FetchTranslation *trans = new FetchTranslation(this, ftqIndex);
        cpu->mmu->translateTiming(missingReq, cpu->thread[tid]->getTC(),
                                  trans, BaseMMU::Execute);
    }
}

bool
Fetch::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(O3CPU, "Fetch unit received timing\n");
    // We shouldn't ever get a cacheable block in Modified state
    assert(pkt->req->isUncacheable() ||
           !(pkt->cacheResponding() && !pkt->hasSharers()));

    DPRINTF(Fetch, "received pkt addr=%#lx, req addr=%#lx\n", pkt->getAddr(),
            pkt->req->getVaddr());

    fetch->processCacheCompletion(pkt);

    return true;
}

void
Fetch::IcachePort::recvReqRetry()
{
    fetch->recvReqRetry();
}

bool
Fetch::canFetchInstructions(ThreadID tid) const
{
    // Thread must be in Running state
    if (fetchStatus[tid] != Running) {
        return false;  // Covers Idle, Squashing, Blocked, TrapPending
    }

    // Check cache status for all potentially active FTQ indices
    // If any FTQ has pending requests, we need to wait
    bool canFetch = true;
    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        if (fetch2Coord[tid].ftqActive[ftqIndex]) {
            CacheRequestStatus cacheStatus = cacheReq[tid][ftqIndex].getOverallStatus();
            if (cacheStatus != CacheIdle && cacheStatus != AccessComplete) {
                canFetch = false;
                break;
            }
        }
    }

    return canFetch;
}

bool
Fetch::hasPendingCacheRequests(ThreadID tid) const
{
    // Check for any active cache operations (excluding terminal states) for all FTQ indices
    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        if (fetch2Coord[tid].ftqActive[ftqIndex]) {
            CacheRequestStatus overallStatus = cacheReq[tid][ftqIndex].getOverallStatus();
            if (overallStatus == TlbWait ||
                overallStatus == CacheWaitResponse ||
                overallStatus == CacheWaitRetry) {
                return true;
            }
        }
    }
    return false;
}

void
Fetch::updateCacheRequestStatus(ThreadID tid, size_t reqIndex,
                               CacheRequestStatus status, unsigned ftqIndex)
{
    assert(tid < MaxThreads);
    assert(ftqIndex < 2);
    assert(reqIndex < cacheReq[tid][ftqIndex].requestStatus.size());

    DPRINTF(Fetch, "[tid:%d][ftq:%d] updateCacheRequestStatus[%d]: %s -> %s\n",
            tid, ftqIndex, reqIndex,
            cacheRequestStatusStr[cacheReq[tid][ftqIndex].requestStatus[reqIndex]],
            cacheRequestStatusStr[status]);

    cacheReq[tid][ftqIndex].requestStatus[reqIndex] = status;
}

void
Fetch::updateCacheRequestStatusByRequest(ThreadID tid, const RequestPtr& req,
                                        CacheRequestStatus status, unsigned ftqIndex)
{
    assert(tid < MaxThreads);
    assert(ftqIndex < 2);

    size_t reqIndex = cacheReq[tid][ftqIndex].findRequestIndex(req);
    if (reqIndex != SIZE_MAX) {
        updateCacheRequestStatus(tid, reqIndex, status, ftqIndex);
    } else {
        warn("Cannot find req %#x for status update to %d in FTQ %d\n", req->getVaddr(), status, ftqIndex);
    }
}

void
Fetch::cancelAllCacheRequests(ThreadID tid)
{
    assert(tid < MaxThreads);

    // Cancel all cache requests for all FTQ indices
    for (unsigned ftqIndex = 0; ftqIndex < 2; ++ftqIndex) {
        DPRINTF(Fetch, "[tid:%d][ftq:%d] cancelAllCacheRequests: status before cancel: %s\n",
                tid, ftqIndex, cacheReq[tid][ftqIndex].getStatusSummary().c_str());

        // Cancel all cache requests for this FTQ
        cacheReq[tid][ftqIndex].cancelAllRequests();

        DPRINTF(Fetch, "[tid:%d][ftq:%d] cancelAllCacheRequests: status after cancel: %s\n",
                tid, ftqIndex, cacheReq[tid][ftqIndex].getStatusSummary().c_str());
    }

    // Reset fetch2 coordinator state
    fetch2Coord[tid].reset();
}

Fetch::CacheRequestStatus
Fetch::getOverallCacheStatus(ThreadID tid) const
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
        if (fetch2Coord[tid].ftqActive[ftqIndex]) {
            hasActive = true;
            CacheRequestStatus status = cacheReq[tid][ftqIndex].getOverallStatus();

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

    // Return status based on priority
    if (hasRetry) return CacheWaitRetry;
    if (hasTlbWait) return TlbWait;
    if (hasCacheWait) return CacheWaitResponse;
    if (hasComplete) return AccessComplete;

    // If no active FTQ, or all are idle
    return CacheIdle;
}

} // namespace o3
} // namespace gem5
