/*
 * Copyright (c) 2010-2012, 2014 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_FETCH_HH__
#define __CPU_O3_FETCH_HH__

#include <cstring>
#include <utility>

#include "arch/generic/decoder.hh"
#include "arch/generic/mmu.hh"
#include "base/statistics.hh"
#include "config/the_isa.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pc_event.hh"
#include "cpu/pred/bpred_unit.hh"
#include "cpu/pred/btb/decoupled_bpred.hh"
#include "cpu/pred/ftb/decoupled_bpred.hh"
#include "cpu/pred/stream/decoupled_bpred.hh"
#include "cpu/timebuf.hh"
#include "cpu/translation.hh"
#include "enums/SMTFetchPolicy.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "sim/eventq.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

/**
 * Fetch class handles both single threaded and SMT fetch. Its
 * width is specified by the parameters; each cycle it tries to fetch
 * that many instructions. It supports using a branch predictor to
 * predict direction and targets.
 * It supports the idling functionality of the CPU by indicating to
 * the CPU when it is active and inactive.
 */
class Fetch
{
  public:
    /**
     * IcachePort class for instruction fetch.
     */
    class IcachePort : public RequestPort
    {
      protected:
        /** Pointer to fetch. */
        Fetch *fetch;

      public:
        /** Default constructor. */
        IcachePort(Fetch *_fetch, CPU *_cpu);

      protected:

        /** Timing version of receive.  Handles setting fetch to the
         * proper status to start fetching. */
        virtual bool recvTimingResp(PacketPtr pkt);

        /** Handles doing a retry of a failed fetch. */
        virtual void recvReqRetry();
    };

    class FetchTranslation : public BaseMMU::Translation
    {
      protected:
        Fetch *fetch;

      public:
        FetchTranslation(Fetch *_fetch) : fetch(_fetch) {}

        void markDelayed() {}

        void
        finish(const Fault &fault, const RequestPtr &req,
            gem5::ThreadContext *tc, BaseMMU::Mode mode)
        {
            assert(mode == BaseMMU::Execute);
            fetch->finishTranslation(fault, req);
            delete this;
        }
    };

  private:
    /* Event to delay delivery of a fetch translation result in case of
     * a fault and the nop to carry the fault cannot be generated
     * immediately */
    class FinishTranslationEvent : public Event
    {
      private:
        Fetch *fetch;
        Fault fault;
        RequestPtr req;

      public:
        FinishTranslationEvent(Fetch *_fetch)
            : fetch(_fetch), req(nullptr)
        {}

        void setFault(Fault _fault) { fault = _fault; }
        void setReq(const RequestPtr &_req) { req = _req; }
        RequestPtr getReq() { return req; }

        /** Process the delayed finish translation */
        void
        process()
        {
            assert(fetch->numInst < fetch->fetchWidth);
            fetch->finishTranslation(fault, req);
        }

        const char *
        description() const
        {
            return "CPU FetchFinishTranslation";
        }
      };

  public:
    /** Overall fetch status. Used to determine if the CPU can
     * deschedule itsef due to a lack of activity.
     */
    enum FetchStatus
    {
        Active,
        Inactive
    };

    /** Individual thread status. */
    enum ThreadStatus
    {
        Running,
        Idle,
        Squashing,
        Blocked,
        TrapPending,
        WaitingCache,
        NumFetchStatus
    };

    std::map<Fetch::ThreadStatus, const char*> fetchStatusStr = {
        {Running, "Running"},
        {Idle, "Idle"},
        {Squashing, "Squashing"},
        {Blocked, "Blocked"},
        {TrapPending, "TrapPending"},
        {WaitingCache, "WaitingCache"}
    };

    /** Cache request status for new state management system.
     * Manages the lifecycle of individual cache access requests.
     */
    enum CacheRequestStatus
    {
        CacheIdle,              // No active request
        TlbWait,               // Waiting for TLB translation completion
        CacheWaitResponse,     // Waiting for cache data return
        CacheWaitRetry,        // Waiting for cache retry opportunity
        AccessComplete,        // Access completed, data available
        AccessFailed,          // Access failed (invalid address etc.)
        Cancelled,             // Request cancelled (squash etc.)
        NumCacheRequestStatus
    };

  private:
    /** Fetch status. */
    FetchStatus _status;

    /** Per-thread status. */
    ThreadStatus fetchStatus[MaxThreads];

    /** Fetch policy. */
    SMTFetchPolicy fetchPolicy;

    /** List that has the threads organized by priority. */
    std::list<ThreadID> priorityList;

    /** Probe points. */
    ProbePointArg<DynInstPtr> *ppFetch;
    /** To probe when a fetch request is successfully sent. */
    ProbePointArg<RequestPtr> *ppFetchRequestSent;

  public:
    /** Fetch constructor. */
    Fetch(CPU *_cpu, const BaseO3CPUParams &params);

    /** Returns the name of fetch. */
    std::string name() const;


    /** Registers probes. */
    void regProbePoints();

    /** Sets the main backwards communication time buffer pointer. */
    void setTimeBuffer(TimeBuffer<TimeStruct> *time_buffer);

    /** Sets pointer to list of active threads. */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Sets pointer to time buffer used to communicate to the next stage. */
    void setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr);

    /** Initialize stage. */
    void startupStage();

    /** Clear all thread-specific states*/
    void clearStates(ThreadID tid);

    /** Handles retrying the fetch access. */
    void recvReqRetry();

    /** Processes cache completion event. */
    void processCacheCompletion(PacketPtr pkt);

    /** Resume after a drain. */
    void drainResume();

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Has the stage drained? */
    bool isDrained() const;

    /** Takes over from another CPU's thread. */
    void takeOverFrom();

    /**
     * Stall the fetch stage after reaching a safe drain point.
     *
     * The CPU uses this method to stop fetching instructions from a
     * thread that has been drained. The drain stall is different from
     * all other stalls in that it is signaled instantly from the
     * commit stage (without the normal communication delay) when it
     * has reached a safe point to drain from.
     */
    void drainStall(ThreadID tid);

    /** Tells fetch to wake up from a quiesce instruction. */
    void wakeFromQuiesce();

    /** For priority-based fetch policies, need to keep update priorityList */
    void deactivateThread(ThreadID tid);
  private:
    /** Reset this pipeline stage */
    void resetStage();

    /** Changes the status of this stage to active, and indicates this
     * to the CPU.
     */
    void switchToActive();

    /** Changes the status of this stage to inactive, and indicates
     * this to the CPU.
     */
    void switchToInactive();

    /**
     * initialize the state for this tick cycle
     * @return whether there is a status change
     */
    bool initializeTickState();

    /**
     * execute fetch and process instructions
     * @param status_change status change flag
     */
    void fetchAndProcessInstructions(bool status_change);

    /**
     * handle interrupts and perform related operations
     */
    void handleInterrupts();

    /**
     * send instructions to decode stage
     * update stall reasons and measure frontend bubbles
     */
    void sendInstructionsToDecode();

    /**
     * update stall reasons based on fetch status
     * @param insts_to_decode number of instructions to send to decode stage
     * @param tid thread ID
     */
    void updateStallReasons(unsigned insts_to_decode, ThreadID tid);

    /**
     * measure frontend performance bubbles
     * @param insts_to_decode number of instructions to send to decode stage
     * @param tid thread ID
     */
    void measureFrontendBubbles(unsigned insts_to_decode, ThreadID tid);

    /**
     * update branch predictors
     */
    void updateBranchPredictors();

    /**
     * Looks up the branch predictor, gets a prediction, and updates the PC.
     * @param inst The dynamic instruction object.
     * @param next_pc The PC state to update with the prediction.
     * @return true if a branch was predicted taken.
     */
    bool lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc);

    /**
     * Fetches the cache line that contains the fetch PC.  Returns any
     * fault that happened.  Puts the data into the class variable
     * fetchBuffer, which may not hold the entire fetched cache line.
     * @param vaddr The memory address that is being fetched from.
     * @param ret_fault The fault reference that will be set to the result of
     * the icache access.
     * @param tid Thread id.
     * @param pc The actual PC of the current instruction.
     * @return Any fault that occured.
     */
    bool fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc);

    /**
     * Send a pipelined I-cache access request for the next FTQ entry.
     * @param tid Thread ID
     * @param pc_state The PC state of the current instruction.
     */
    void sendNextCacheRequest(ThreadID tid, const PCStateBase &pc_state);

    void finishTranslation(const Fault &fault, const RequestPtr &mem_req);

    /** Validate if a translation request is expected and should be processed.
     * @param tid Thread ID
     * @param mem_req The memory request to validate
     * @return true if request should be processed, false if should be ignored
     */
    bool validateTranslationRequest(ThreadID tid, const RequestPtr &mem_req);

    /** Handle successful translation and initiate cache access.
     * @param tid Thread ID
     * @param mem_req The memory request
     * @param fetchPC The fetch PC address
     */
    void handleSuccessfulTranslation(ThreadID tid, const RequestPtr &mem_req, Addr fetchPC);

    /** Handle translation fault by building a noop instruction.
     * @param tid Thread ID
     * @param mem_req The memory request that faulted
     * @param fault The translation fault
     */
    void handleTranslationFault(ThreadID tid, const RequestPtr &mem_req, const Fault &fault);

    /** Handle multi-cacheline fetch that spans two cache lines.
     * Creates and sends two separate cache requests.
     * @param vaddr Starting virtual address
     * @param tid Thread ID
     * @param pc Program counter
     * @return true if requests were successfully initiated
     */
    bool handleMultiCacheLineFetch(Addr vaddr, ThreadID tid, Addr pc);

    /** Process multi-cacheline fetch completion when both packets have arrived.
     * Merges data from both cache lines into the fetch buffer.
     * @param tid Thread ID
     * @param pkt Most recently arrived packet
     * @return true if all packets have arrived and data is merged, false if still waiting
     */
    bool processMultiCacheLineCompletion(ThreadID tid, PacketPtr pkt);


    /** Check if an interrupt is pending and that we need to handle
     */
    bool checkInterrupt(Addr pc) { return interruptPending; }

    /** Squashes a specific thread and resets the PC. */
    void doSquash(PCStateBase &new_pc, const DynInstPtr squashInst, const InstSeqNum seqNum,
            ThreadID tid);

    /** Squashes a specific thread and resets the PC. Also tells the CPU to
     * remove any instructions between fetch and decode
     *  that should be sqaushed.
     */
    void squashFromDecode(PCStateBase &new_pc,
                          const DynInstPtr squashInst,
                          const InstSeqNum seq_num, ThreadID tid);

    /** Checks if a thread is stalled. */
    bool checkStall(ThreadID tid) const;

    /** Updates overall fetch stage status; to be called at the end of each
     * cycle. */
    FetchStatus updateFetchStatus();

  public:
    /** Squashes a specific thread and resets the PC. Also tells the CPU to
     * remove any instructions that are not in the ROB. The source of this
     * squash should be the commit stage.
     */
    void squash(PCStateBase &new_pc, const InstSeqNum seq_num,
                DynInstPtr squashInst, ThreadID tid);

    /** Ticks the fetch stage, processing all inputs signals and fetching
     * as many instructions as possible.
     */
    void tick();

    /** Checks all input signals and updates the status as necessary.
     *  @return: Returns if the status has changed due to input signals.
     */
    bool checkSignalsAndUpdate(ThreadID tid);

    /** Handles commit signals including squash and update operations.
     *  @return: Returns true if squash occurred and immediate return needed.
     */
    bool handleCommitSignals(ThreadID tid);

    /** Handles decode squash signals.
     *  @return: Returns true if squash occurred and immediate return needed.
     */
    bool handleDecodeSquash(ThreadID tid);

    /** Does the actual fetching of instructions and passing them on to the
     * next stage.
     * @param status_change fetch() sets this variable if there was a status
     * change (ie switching to IcacheMissStall).
     */
    void fetch(bool &status_change);

    /** The decoder. */
    InstDecoder *decoder[MaxThreads];

    RequestPort &getInstPort() { return icachePort; }

    branch_prediction::BPredUnit * getBp() { return branchPred; }

    void flushFetchBuffer();

    Addr getPreservedReturnAddr(const DynInstPtr &dynInst);

  private:
    DynInstPtr buildInst(ThreadID tid, StaticInstPtr staticInst,
            StaticInstPtr curMacroop, const PCStateBase &this_pc,
            const PCStateBase &next_pc, bool trace);

    /** Returns the appropriate thread to fetch, given the fetch policy. */
    ThreadID getFetchingThread();

    /** Returns the appropriate thread to fetch using a round robin policy. */
    ThreadID roundRobin();

    /** Returns the appropriate thread to fetch using the IQ count policy. */
    ThreadID iqCount();

    /** Returns the appropriate thread to fetch using the LSQ count policy. */
    ThreadID lsqCount();

    /** Returns the appropriate thread to fetch using the branch count
     * policy. */
    ThreadID branchCount();

    /** Pipeline the next I-cache access to the current one. */
    void pipelineIcacheAccesses(ThreadID tid);

    /** Profile the reasons of fetch stall. */
    void profileStall(ThreadID tid);


    bool ftqEmpty() { return isDecoupledFrontend() && usedUpFetchTargets; }

    /** Set the reasons of all fetch stalls. */
    void setAllFetchStalls(StallReason stall);

    /** Select the thread to fetch from.
     * @return Thread ID to fetch from, or InvalidThreadID if none available
     */
    ThreadID selectFetchThread();

    /** Check decoupled frontend (FTQ) availability.
     * @param tid Thread ID
     * @return true if frontend is ready for fetch, false otherwise
     */
    bool checkDecoupledFrontend(ThreadID tid);

    /** Prepare fetch address and handle status transitions.
     * @param tid Thread ID
     * @param status_change Reference to status change flag
     * @return true if ready to fetch, false if stalled/idle
     */
    bool prepareFetchAddress(ThreadID tid, bool &status_change);

    /**
     * The main instruction fetching logic, which processes instructions
     * for a given thread up to the fetch width.
     * @param tid The thread ID to fetch for.
     */
    void performInstructionFetch(ThreadID tid);

    /**
     * Processes a single instruction, including decoding, building the
     * dynamic instruction, handling branch prediction, and updating the PC.
     *
     * @param tid The thread ID of the instruction.
     * @param pc The current program counter state (will be updated).
     * @param curMacroop The current macro-op being processed (if any).
     * @return true if a branch was predicted.
     */
    bool
    processSingleInstruction(ThreadID tid, PCStateBase &pc,
                             StaticInstPtr &curMacroop);

    /**
     * Checks if the decoder requires more memory to proceed and fetches
     * a cache line if necessary.
     * @param tid The thread ID to check for.
     * @param this_pc Current PC state
     * @param curMacroop Current macroop (if any)
     * @return StallReason if stalled, NoStall otherwise
     */
    StallReason checkMemoryNeeds(ThreadID tid, const PCStateBase &this_pc,
                                 const StaticInstPtr &curMacroop);


    /**
     * Looks up the branch predictor, gets a prediction, and updates the PC.
     * @param inst The dynamic instruction object.
     * @param next_pc The PC state to update with the prediction.
     * @param predictedBranch Flag indicating if a branch was predicted.
     * @param newMacro Flag indicating if we are moving to a new macro-op.
     */
    void
    lookupAndUpdateNextPC(const DynInstPtr &inst, PCStateBase &next_pc,
                         bool &predictedBranch, bool &newMacro);

  private:
    /** Pointer to the O3CPU. */
    CPU *cpu;

    /** Time buffer interface. */
    TimeBuffer<TimeStruct> *timeBuffer;

    /** Wire to get decode's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromDecode;

    /** Wire to get rename's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromRename;

    /** Wire to get iew's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromIEW;

    /** Wire to get commit's information from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromCommit;

    //Might be annoying how this name is different than the queue.
    /** Wire used to write any information heading to decode. */
    TimeBuffer<FetchStruct>::wire toDecode;

    /** BPredUnit. */
    branch_prediction::BPredUnit *branchPred;
    
    branch_prediction::stream_pred::DecoupledStreamBPU *dbsp;

    branch_prediction::ftb_pred::DecoupledBPUWithFTB *dbpftb;

    branch_prediction::btb_pred::DecoupledBPUWithBTB *dbpbtb;

    /** PC of each thread. */
    std::unique_ptr<PCStateBase> pc[MaxThreads];

    /** Macroop of each thread. */
    StaticInstPtr macroop[MaxThreads];

    /** Can the fetch stage redirect from an interrupt on this instruction? */
    bool delayedCommit[MaxThreads];

    /** Variable that tracks if fetch has written to the time buffer this
     * cycle. Used to tell CPU if there is activity this cycle.
     */
    bool wroteToTimeBuffer;

    /** Tracks how many instructions has been fetched this cycle. */
    int numInst;

    /** Source of possible stalls. */
    struct Stalls
    {
        bool decode; // stall due to decode
        bool drain; // stall due to drain
    };

    /** Tracks which stages are telling fetch to stall. */
    Stalls stalls[MaxThreads];

    /** Decode to fetch delay. */
    Cycles decodeToFetchDelay;

    /** Rename to fetch delay. */
    Cycles renameToFetchDelay;

    /** IEW to fetch delay. */
    Cycles iewToFetchDelay;

    /** Commit to fetch delay. */
    Cycles commitToFetchDelay;

    /** The width of fetch in instructions. */
    unsigned fetchWidth;

    /** The width of decode in instructions. */
    unsigned decodeWidth;

    /** Is the cache blocked?  If so no threads can access it. */
    bool cacheBlocked;

    /** The packet that is waiting to be retried. */
    std::vector<PacketPtr> retryPkt;

    /** The thread that is waiting on the cache to tell fetch to retry. */
    ThreadID retryTid;

    /** Cache block size. */
    unsigned int cacheBlkSize;

    /**
     * Fetch buffer structure to encapsulate instruction fetch data.
     * Encapsulates buffer data, PC tracking, validity state, and size.
     * Designed to prepare for 2fetch implementation with potential multi-stream support.
     */
    struct FetchBuffer
    {
        /** Pointer to the fetch data buffer */
        uint8_t *data;

        /** PC of the first instruction loaded into the fetch buffer */
        Addr startPC;

        /** Whether the fetch buffer data is valid */
        bool valid;

        /** Size of the fetch buffer in bytes. Set by Fetch class during init. */
        unsigned size;

        /** Constructor initializes buffer with default size */
        FetchBuffer() : data(nullptr), startPC(0), valid(false), size(0) {
        }

        /** Destructor is not needed as Fetch class manages memory */
        ~FetchBuffer() {
        }

        /** Reset buffer state */
        void reset() {
            valid = false;
            startPC = 0;
            // No need to clear data as it will be overwritten
        }

        /** Check if a PC is within the current buffer range */
        bool contains(Addr pc) const {
            return valid && (pc >= startPC) && (pc < startPC + size);
        }

        /** Get offset of PC within the buffer */
        unsigned getOffset(Addr pc) const {
            assert(contains(pc));
            return pc - startPC;
        }

        /** Set buffer data and update metadata */
        void setData(Addr pc, const uint8_t* src_data, unsigned bytes_copied) {
            startPC = pc;
            valid = true;
            memcpy(data, src_data, bytes_copied);
        }

        /** Get end PC of the buffer */
        Addr getEndPC() const {
            return startPC + size;
        }
    };

    /** Fetch buffer for each thread */
    FetchBuffer fetchBuffer[MaxThreads];

    /** The size of the fetch buffer in bytes. Default is 66 bytes,
    *  make sure we could decode tail 4bytes if it is in [62, 66)
     */
    unsigned fetchBufferSize;

    // Constants for misaligned fetch handling
    static constexpr unsigned CACHE_LINE_SIZE_BYTES = 64;

    /**
     * Unified cache request structure to handle multiple cacheline accesses.
     * Replaces multiple separate state variables for cleaner state management.
     * Supports extensibility for future 2fetch implementation.
     */
    struct CacheRequest
    {
        /** Vector of packet pointers for multiple cache line requests */
        std::vector<PacketPtr> packets;

        /** Vector of corresponding request pointers */
        std::vector<RequestPtr> requests;

        /** Vector of status for each cache request (NEW) */
        std::vector<CacheRequestStatus> requestStatus;

        /** Base address of the fetch request */
        Addr baseAddr;

        /** Total size of the fetch request in bytes */
        unsigned totalSize;

        /** Number of completed packets received */
        unsigned completedPackets;

        /** Constructor */
        CacheRequest() : baseAddr(0), totalSize(0), completedPackets(0) {}

        /** Check if all packets have been completed */
        bool allCompleted() const {
            return completedPackets >= packets.size() && packets.size() > 0;
        }

        /** Check if any request has failed (NEW) */
        bool anyFailed() const {
            for (const auto& status : requestStatus) {
                if (status == AccessFailed) return true;
            }
            return false;
        }

        /** Check if all requests are ready for processing (NEW) */
        bool allReady() const {
            if (requestStatus.empty()) return false;
            for (const auto& status : requestStatus) {
                if (status != AccessComplete) return false;
            }
            return true;
        }

        /** Get overall status of the cache request group (NEW) */
        CacheRequestStatus getOverallStatus() const {
            if (requestStatus.empty()) return CacheIdle;

            // Check for specific priority states first
            for (const auto& status : requestStatus) {
                if (status == AccessFailed) return AccessFailed;
                if (status == CacheWaitRetry) return CacheWaitRetry;
                if (status == TlbWait) return TlbWait;
                if (status == CacheWaitResponse) return CacheWaitResponse;
            }

            // Check if all are completed
            if (allReady()) return AccessComplete;

            return CacheIdle;
        }

        /** Reset the cache request state */
        void reset() {
            packets.clear();
            requests.clear();
            requestStatus.clear();
            baseAddr = 0;
            totalSize = 0;
            completedPackets = 0;
        }

        /** Add a new request */
        void addRequest(RequestPtr req) {
            requests.push_back(req);
            packets.push_back(nullptr);  // Initialize with null packet
            requestStatus.push_back(CacheIdle);  // Initialize status
        }

        /** Mark a specific request as failed (NEW) */
        void markRequestFailed(size_t index) {
            if (index < requestStatus.size()) {
                requestStatus[index] = AccessFailed;
            }
        }

        /** Cancel all active requests (NEW) */
        void cancelAllRequests() {
            for (auto& status : requestStatus) {
                if (status != AccessComplete && status != AccessFailed) {
                    status = Cancelled;
                }
            }
        }

        /** Update status for a specific request by index (NEW) */
        void updateRequestStatus(size_t index, CacheRequestStatus status) {
            if (index < requestStatus.size()) {
                requestStatus[index] = status;
            }
        }

        /** Find request index by RequestPtr (NEW) */
        size_t findRequestIndex(const RequestPtr& req) const {
            for (size_t i = 0; i < requests.size(); ++i) {
                if (requests[i] == req) {
                    return i;
                }
            }
            return SIZE_MAX;  // Not found
        }

        /** Get status summary string for debugging (NEW) */
        std::string getStatusSummary() const {
            std::string summary = "CacheRequest[";
            for (size_t i = 0; i < requestStatus.size(); ++i) {
                if (i > 0) summary += ",";
                switch (requestStatus[i]) {
                    case CacheIdle: summary += "Idle"; break;
                    case TlbWait: summary += "TlbWait"; break;
                    case CacheWaitResponse: summary += "CacheWait"; break;
                    case CacheWaitRetry: summary += "Retry"; break;
                    case AccessComplete: summary += "Complete"; break;
                    case AccessFailed: summary += "Failed"; break;
                    case Cancelled: summary += "Cancelled"; break;
                    default: summary += "Unknown"; break;
                }
            }
            summary += "]";
            return summary;
        }

        /** Mark a packet as completed by matching request */
        bool markCompletedAndStorePacket(PacketPtr pkt) {
          // return false if the packet is not found in the requests
          bool found_packet = false;
            // Find and mark the packet as completed by matching the request
            for (size_t i = 0; i < requests.size(); ++i) {
                if (requests[i] == pkt->req) {
                    // Only increment count if this packet wasn't already stored
                    if (packets[i] == nullptr) {
                        packets[i] = pkt;  // Store the packet
                        completedPackets++;
                        found_packet = true;
                        // Update status to AccessComplete
                        if (i < requestStatus.size()) {
                            requestStatus[i] = AccessComplete;
                        }
                    }
                }
            }
            if (!found_packet) {
                return false;
            }
            return true;
        }

        /** Get the number of pending packets */
        unsigned getPendingCount() const {
            return packets.size() - completedPackets;
        }
    };

    /** Cache request for each thread, replacing multiple redundant state variables */
    CacheRequest cacheReq[MaxThreads];

    /** The size of the fetch queue in micro-ops */
    unsigned fetchQueueSize;

    /** Queue of fetched instructions. Per-thread to prevent HoL blocking. */
    std::deque<DynInstPtr> fetchQueue[MaxThreads];

    unsigned currentLoopIter{0};  // todo: remove this

    /** Icache stall statistics. */
    Counter lastIcacheStall[MaxThreads];

    /** List of Active Threads */
    std::list<ThreadID> *activeThreads;

    /** Number of threads. */
    ThreadID numThreads;

    /** Number of threads that are actively fetching. */
    ThreadID numFetchingThreads;

    /** Thread ID being fetched. */
    ThreadID threadFetched;

    /** Checks if there is an interrupt pending.  If there is, fetch
     * must stop once it is not fetching PAL instructions.
     */
    bool interruptPending;

    /** Instruction port. Note that it has to appear after the fetch stage. */
    IcachePort icachePort;

    /** Event used to delay fault generation of translation faults */
    FinishTranslationEvent finishTranslationEvent;

    /** Decoupled frontend related */
    bool isDecoupledFrontend() { return branchPred->isDecoupled(); }

    bool isStreamPred() { return branchPred->isStream(); }

    bool isFTBPred() { return branchPred->isFTB(); }

    bool isBTBPred() {return branchPred->isBTB(); }

    bool usedUpFetchTargets;

    /** fetch stall reasons */
    std::vector<StallReason> stallReason;

    bool currentFetchTargetInLoop{false};

    std::pair<Addr, std::vector<branch_prediction::ftb_pred::LoopBuffer::InstDesc>> currentFtqEntryInsts;

    bool notTakenBranchEncountered{false};

    /** Check if we need a new FTQ entry for fetch */
    bool needNewFTQEntry(ThreadID tid);

    /** Get the start PC of the next FTQ entry and update fetchBufferPC */
    Addr getNextFTQStartPC(ThreadID tid);

    /**
     * Check if the thread can fetch instructions
     * @param tid Thread ID
     * @return true if thread can fetch instructions
     */
    bool canFetchInstructions(ThreadID tid) const;

    /**
     * Check if there are pending cache requests for this thread
     * @param tid Thread ID
     * @return true if there are active cache requests
     */
    bool hasPendingCacheRequests(ThreadID tid) const;



    /**
     * State setting interfaces for new state system
     */

    /**
     * Set the thread status with unified interface
     * @param tid Thread ID
     * @param status New thread status
     */
    void setThreadStatus(ThreadID tid, ThreadStatus status);

    /**
     * Update cache request status for specific request
     * @param tid Thread ID
     * @param reqIndex Request index
     * @param status New cache request status
     */
    void updateCacheRequestStatus(ThreadID tid, size_t reqIndex, CacheRequestStatus status);

    /**
     * Helper function to update cache request status by RequestPtr
     * Combines findRequestIndex + updateCacheRequestStatus + warn pattern
     * @param tid Thread ID
     * @param req Request pointer to find and update
     * @param status New cache request status
     */
    void updateCacheRequestStatusByRequest(ThreadID tid, const RequestPtr& req,
                                          CacheRequestStatus status);

    /**
     * Cancel all cache requests for thread (used in squash)
     * @param tid Thread ID
     */
    void cancelAllCacheRequests(ThreadID tid);

  protected:
    struct FetchStatGroup : public statistics::Group
    {
        FetchStatGroup(CPU *cpu, Fetch *fetch);
        // @todo: Consider making these
        // vectors and tracking on a per thread basis.
        /** Stat for total number of cycles stalled due to an icache miss. */
        statistics::Scalar icacheStallCycles;
        /** Stat for total number of fetched instructions. */
        statistics::Scalar insts;
        /** Total number of fetched branches. */
        statistics::Scalar branches;
        /** Stat for total number of predicted branches. */
        statistics::Scalar predictedBranches;
        /** Stat for total number of cycles spent fetching. */
        statistics::Scalar cycles;
        /** Stat for total number of cycles spent squashing. */
        statistics::Scalar squashCycles;
        /** Stat for total number of cycles spent waiting for translation */
        statistics::Scalar tlbCycles;
        /** Stat for total number of cycles
         *  spent blocked due to other stages in
         * the pipeline.
         */
        statistics::Scalar idleCycles;
        /** Total number of cycles spent blocked. */
        statistics::Scalar blockedCycles;
        /** Total number of cycles spent in any other state. */
        statistics::Scalar miscStallCycles;
        /** Total number of cycles spent in waiting for drains. */
        statistics::Scalar pendingDrainCycles;
        /** Total number of stall cycles caused by no active threads to run. */
        statistics::Scalar noActiveThreadStallCycles;
        /** Total number of stall cycles caused by pending traps. */
        statistics::Scalar pendingTrapStallCycles;
        /** Total number of stall cycles
         *  caused by pending quiesce instructions. */
        statistics::Scalar pendingQuiesceStallCycles;
        /** Total number of stall cycles caused by I-cache wait retrys. */
        statistics::Scalar icacheWaitRetryStallCycles;
        /** Stat for total number of fetched cache lines. */
        statistics::Scalar cacheLines;
        /** Total number of outstanding icache accesses that were dropped
         * due to a squash.
         */
        statistics::Scalar icacheSquashes;
        /** Total number of outstanding tlb accesses that were dropped
         * due to a squash.
         */
        statistics::Scalar tlbSquashes;
        /** Distribution of number of instructions fetched each cycle. */
        statistics::Distribution nisnDist;
        /** Rate of how often fetch was idle. */
        statistics::Formula idleRate;
        /** Number of branch fetches per cycle. */
        statistics::Formula branchRate;
        /** Number of instruction fetched per cycle. */
        statistics::Formula rate;
        /** Distribution of fetch status */
        statistics::Vector fetchStatusDist;
        /** Number of decode stalls */
        statistics::Scalar decodeStalls;
        /** Number of decode stalls per cycle */
        statistics::Formula decodeStallRate;
        /** Unutilized issue-pipeline slots while there is no backend-stall */
        statistics::Scalar fetchBubbles;
        /** Cycles that fetch 0 instruction while there is no backend-stall */
        statistics::Scalar fetchBubbles_max;
        /** Frontend Bound */
        statistics::Formula frontendBound;
        /** Frontend Latency Bound */
        statistics::Formula frontendLatencyBound;
        /** Frontend Bandwidth Bound */
        statistics::Formula frontendBandwidthBound;
    } fetchStats;

    SquashVersion localSquashVer;

public:
    const FetchStatGroup &getFetchStats() { return fetchStats; }

  private:

    bool waitForVsetvl = false;
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_FETCH_HH__
