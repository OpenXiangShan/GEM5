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

bool
Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc, unsigned ftqIndex)
{
    assert(!cpu->switchedOut());

    // Check for interrupts that would prevent fetch
    if (checkInterrupt(pc) && !delayedCommit[tid]) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, interrupt pending\n", tid);
        setAllFetchStalls(StallReason::IntStall);
        return false;
    }

    DPRINTF(Fetch, "[tid:%i] Fetching cache line %#x for addr %#x, pc=%#lx\n",
            tid, vaddr, vaddr, pc);

    // Check if ICacheHandler can accept the request
    if (icacheHandler->hasPendingCacheRequests(tid)) {
        DPRINTF(Fetch, "[tid:%i] Can't fetch cache line, ICacheHandler has pending requests\n", tid);
        setAllFetchStalls(StallReason::IcacheStall);
        return false;
    }

    // Mark this FTQ as active when we start fetching
    fetch2Coord[tid].ftqActive[ftqIndex] = true;

    DPRINTF(Fetch, "[tid:%i] Setting ftqActive[%d] = true for cache request (addr=%#x)\n",
            tid, ftqIndex, vaddr);
    DPRINTF(Fetch, "[tid:%i] FTQ state: ftqActive[0]=%d, ftqActive[1]=%d\n",
            tid, fetch2Coord[tid].ftqActive[0], fetch2Coord[tid].ftqActive[1]);

    // Transition to WaitingCache state when initiating cache access
    setThreadStatus(tid, WaitingCache);

    // set fetchBufferPC to vaddr
    getFetchBuffer(tid, ftqIndex).startPC = vaddr;

    // Use ICacheHandler for cache interaction with callback mechanism
    icacheHandler->fetch(vaddr, pc, fetchBufferSize, tid, ftqIndex,
        [this](CacheRequestStatus status, const FetchCallbackData& data) {
            this->onFetchCompleted(status, data);
        });

    return true;
}

void
Fetch::onFetchCompleted(CacheRequestStatus status, const FetchCallbackData& data)
{
    ThreadID tid = 0;   // no SMT now, so tid is always 0
    unsigned ftqIndex = data.ftqIndex;

    DPRINTF(Fetch, "[tid:%i][ftq:%d] onFetchCompleted: status=%d\n",
            tid, ftqIndex, static_cast<int>(status));

    // Check if request has been squashed
    if (fetchStatus[tid] == Squashing) {
        DPRINTF(Fetch, "[tid:%i] Ignoring fetch completion after squash\n", tid);
        if (data.pkt) delete data.pkt;
        if (data.mergedData) delete[] data.mergedData;
        return;
    }

    switch (status) {
        case AccessComplete: {
            DPRINTF(Fetch, "[tid:%i][ftq:%d] Cache fetch completed successfully\n", tid, ftqIndex);

            assert(!cpu->switchedOut());

            // Use merged data from ICacheHandler
            if (data.mergedData && data.dataSize > 0) {
                FetchBuffer& bufferRef = getFetchBuffer(tid, ftqIndex);
                memcpy(bufferRef.data, data.mergedData, data.dataSize);
                bufferRef.valid = true;

                DPRINTF(Fetch, "[tid:%i][ftq:%d] Data copied to fetchBuffer: size=%d\n",
                        tid, ftqIndex, data.dataSize);

                // Clean up the temporary merged data buffer
                delete[] data.mergedData;
            } else {
                DPRINTF(Fetch, "[tid:%i][ftq:%d] ERROR: No merged data received from ICacheHandler\n",
                        tid, ftqIndex);
            }

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
                    setAllFetchStalls(StallReason::NoStall);
                }
            } else {
                DPRINTF(Fetch, "[tid:%i] FTQ %d completed, but other FTQs still pending\n", tid, ftqIndex);
            }
            break;
        }

        case AccessFailed: {
            DPRINTF(FetchFault, "[tid:%i] Translation fault: %s\n", tid, data.fault->name());

            // Don't send an instruction to decode if we can't handle it.
            if (!(numInst < fetchWidth) || !(fetchQueue[tid].size() < fetchQueueSize)) {
                // Defer fault handling to next cycle
                DPRINTF(Fetch, "[tid:%i] Deferring fault handling to next cycle\n", tid);
                return;
            }

            // Send the fault to commit. This thread will not do anything
            // until commit handles the fault. The only other way it can
            // wake up is if a squash comes along and changes the PC.
            const PCStateBase &fetch_pc = *pc[tid];

            DPRINTF(Fetch, "[tid:%i] Translation faulted, building noop.\n", tid);
            // We will use a nop in order to carry the fault.
            DynInstPtr instruction = buildInst(tid, nopStaticInstPtr, nullptr,
                    fetch_pc, fetch_pc, false);
            instruction->setVersion(localSquashVer);
            instruction->setNotAnInst();

            instruction->setPredTarg(fetch_pc);
            instruction->fault = data.fault;
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
                    tid, data.fault->name(), *pc[tid]);
            break;
        }

        case CacheWaitRetry: {
            DPRINTF(Fetch, "[tid:%i] Cache request needs retry, setting stalls\n", tid);
            setAllFetchStalls(StallReason::IcacheStall);
            // Request will be retried by ICacheHandler internally
            break;
        }

        case Cancelled: {
            DPRINTF(Fetch, "[tid:%i] Cache request was cancelled, ignoring\n", tid);
            // Nothing to do, request was cancelled
            break;
        }

        default: {
            // Intermediate states (CacheIdle, TlbWait, CacheWaitResponse) should not trigger callbacks
            panic("onFetchCompleted called with intermediate cache status %d", static_cast<int>(status));
            break;
        }
    }

    _status = updateFetchStatus();
}

bool
Fetch::canFetchInstructions(ThreadID tid) const
{
    // Thread must be in Running state
    if (fetchStatus[tid] != Running) {
        return false;  // Covers Idle, Squashing, Blocked, TrapPending
    }

    // Delegate to ICacheHandler for cache status check
    return !icacheHandler->hasPendingCacheRequests(tid);
}

bool
Fetch::hasPendingCacheRequests(ThreadID tid) const
{
    // Delegate to ICacheHandler for state management
    return icacheHandler->hasPendingCacheRequests(tid);
}

CacheRequestStatus
Fetch::getOverallCacheStatus(ThreadID tid) const
{
    // Delegate to ICacheHandler for state management
    return icacheHandler->getOverallCacheStatus(tid);
}

} // namespace o3
} // namespace gem5
