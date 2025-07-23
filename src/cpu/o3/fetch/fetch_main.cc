#include <cstring>
#include <list>

#include "base/random.hh"
#include "base/types.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fetch/fetch.hh"
#include "debug/Activity.hh"
#include "debug/Fetch.hh"
#include "debug/FetchVerbose.hh"
#include "debug/O3PipeView.hh"
#include "sim/full_system.hh"

namespace gem5
{

namespace o3
{

Fetch::FetchStatus
Fetch::updateFetchStatus()
{
    //Check Running
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if ((canFetchInstructions(tid) && !checkStall(tid)) || fetchStatus[tid] == Squashing ||
            icacheHandler->getOverallCacheStatus(tid) == AccessComplete) {

            if (_status == Inactive) {
                DPRINTF(Activity, "[tid:%i] Activating stage.\n",tid);

                if (icacheHandler->getOverallCacheStatus(tid) == AccessComplete) {
                    DPRINTF(Activity, "[tid:%i] Activating fetch due to cache"
                            "completion\n",tid);
                }

                cpu->activateStage(CPU::FetchIdx);
            }

            return Active;
        }
    }

    // Stage is switching from active to inactive, notify CPU of it.
    if (_status == Active) {
        DPRINTF(Activity, "Deactivating stage.\n");

        cpu->deactivateStage(CPU::FetchIdx);
    }

    return Inactive;
}

void
Fetch::tick()
{
    // Initialize state for this tick cycle
    bool status_change = initializeTickState();

    // Perform fetch operations and instruction delivery
    fetchAndProcessInstructions(status_change, 0);

    // Handle branch prediction updates
    updateBranchPredictors();
}

bool
Fetch::initializeTickState()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();
    bool status_change = false;

    wroteToTimeBuffer = false;

    // get the distribution of fetch status
    fetchStats.fetchStatusDist[fetchStatus[0]]++;

    // Check signal updates for all active threads
    while (threads != end) {
        ThreadID tid = *threads++;

        // Check the signals for each thread to determine the proper status
        // for each thread.
        bool updated_status = checkSignalsAndUpdate(tid);
        status_change =  status_change || updated_status;
    }

    DPRINTF(Fetch, "Running stage.\n");

    if (fromCommit->commitInfo[0].emptyROB) {
        waitForVsetvl = false;
    }

    return status_change;
}

void
Fetch::fetchAndProcessInstructions(bool status_change, unsigned ftqIndex)
{
    // Fetch instructions from active threads
    for (threadFetched = 0; threadFetched < numFetchingThreads;
         threadFetched++) {
        // Fetch each of the actively fetching threads.
        fetch(status_change);
    }

    // Pass stall reasons to decode stage
    toDecode->fetchStallReason = stallReason;

    // Record number of instructions fetched this cycle for distribution.
    fetchStats.nisnDist.sample(numInst);

    if (status_change) {
        // Change the fetch stage status if there was a status change.
        _status = updateFetchStatus();
    }

    // Handle interrupt processing in full system mode
    handleInterrupts();

    // Send instructions to decode stage, update stall reasons and measure frontend bubbles.
    sendInstructionsToDecode();
}

void
Fetch::handleInterrupts()
{
    if (FullSystem) {
        if (fromCommit->commitInfo[0].interruptPending) {
            DPRINTF(Fetch, "Set interrupt pending.\n");
            interruptPending = true;
        }

        if (fromCommit->commitInfo[0].clearInterrupt) {
            DPRINTF(Fetch, "Clear interrupt pending.\n");
            interruptPending = false;
        }
    }
}

void
Fetch::sendInstructionsToDecode()
{
    // Send instructions enqueued into the fetch queue to decode.
    // Limit rate by fetchWidth.  Stall if decode is stalled.
    unsigned insts_to_decode = 0;
    unsigned available_insts = 0;

    // Count available instructions across all active threads
    for (auto tid : *activeThreads) {
        if (!stalls[tid].decode) {
            available_insts += fetchQueue[tid].size();
        }
    }

    // Pick a random thread to start trying to grab instructions from
    auto tid_itr = activeThreads->begin();
    std::advance(tid_itr,
            random_mt.random<uint8_t>(0, activeThreads->size() - 1));

    // Collect instructions from fetch queues until decode width is reached
    while (available_insts != 0 && insts_to_decode < decodeWidth) {
        ThreadID tid = *tid_itr;
        if (!stalls[tid].decode && !fetchQueue[tid].empty()) {
            const auto& inst = fetchQueue[tid].front();
            toDecode->insts[toDecode->size++] = inst;
            DPRINTF(Fetch, "[tid:%i] [sn:%llu] Sending instruction to decode "
                    "from fetch queue. Fetch queue size: %i.\n",
                    tid, inst->seqNum, fetchQueue[tid].size());

            wroteToTimeBuffer = true;
            fetchQueue[tid].pop_front();
            insts_to_decode++;
            available_insts--;
        }

        tid_itr++;
        // Wrap around if at end of active threads list
        if (tid_itr == activeThreads->end())
            tid_itr = activeThreads->begin();
    }

    // Update stall reasons based on fetch/decode status
    updateStallReasons(insts_to_decode, *tid_itr);

    // Intel TopDown method for measuring frontend bubbles
    measureFrontendBubbles(insts_to_decode, *tid_itr);

    // If there was activity this cycle, inform the CPU of it
    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    // Reset the number of instructions we've fetched
    numInst = 0;
}

void
Fetch::updateStallReasons(unsigned insts_to_decode, ThreadID tid)
{
    // fetch totally stalled
    if (stalls[tid].decode) {
        // If decode stalled, use decode's stall reason
        setAllFetchStalls(fromDecode->decodeInfo[tid].blockReason);
    } else if (insts_to_decode == 0) {
        // fetch stalled
        if (stallReason[0] != StallReason::NoStall) {
            // previously set stall reason
            setAllFetchStalls(stallReason[0]);
        } else {
            setAllFetchStalls(StallReason::OtherFetchStall);
        }
    } else {
        // fetch partially stalled or no stall
        for (int i = 0; i < stallReason.size(); i++) {
            if (i < insts_to_decode)
                stallReason[i] = StallReason::NoStall;
            else {
                stallReason[i] = StallReason::FetchFragStall;
            }
        }
    }

    toDecode->fetchStallReason = stallReason;
}

void
Fetch::measureFrontendBubbles(unsigned insts_to_decode, ThreadID tid)
{
    // Intel TopDown method for measuring frontend bubbles
    // Count unutilized issue slots when backend is not stalled (decode not stalled)
    // For N-wide machine, if frontend supplies 0 instructions:
    // - fetchBubbles += N (count total empty slots)
    // - fetchBubbles_max += 1 (count occurrence of all slots being empty)
    if (!stalls[tid].decode && !fromCommit->commitInfo[tid].robSquashing) {
        // backend not stalled
        int unused_slots = decodeWidth - insts_to_decode;
        if (unused_slots > 0) {
            // has empty slots
            fetchStats.fetchBubbles += unused_slots; // add number of empty slots
            if (unused_slots == decodeWidth) {
                // all slots empty, insts_to_decode == 0
                fetchStats.fetchBubbles_max++; // count max bubble occurrence
            }
        }
    }

    if (stalls[tid].decode) {
        fetchStats.decodeStalls++;
    }
}

DynInstPtr
Fetch::buildInst(ThreadID tid, StaticInstPtr staticInst,
        StaticInstPtr curMacroop, const PCStateBase &this_pc,
        const PCStateBase &next_pc, bool trace)
{
    // Get a sequence number.
    InstSeqNum seq = cpu->getAndIncrementInstSeq();

    DynInst::Arrays arrays;
    arrays.numSrcs = staticInst->numSrcRegs();
    arrays.numDests = staticInst->numDestRegs();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, staticInst, curMacroop, this_pc, next_pc, seq, cpu);

    cpu->perfCCT->createMeta(instruction);
    cpu->perfCCT->updateInstPos(instruction->seqNum, PerfRecord::AtFetch);

    instruction->setTid(tid);

    instruction->setThreadState(cpu->thread[tid]);

    DPRINTF(Fetch, "[tid:%i] Instruction PC %s created [sn:%lli].\n",
            tid, this_pc, seq);

    DPRINTF(Fetch, "[tid:%i] Instruction is: %s\n", tid,
            instruction->staticInst->disassemble(this_pc.instAddr()));

    DPRINTF(Fetch, "Is nop: %i, is move: %i\n", instruction->isNop(),
            instruction->isMov());
    if (isDecoupledFrontend()) {
        if (isStreamPred()) {
            DPRINTF(DecoupleBP, "Set instruction %lu with stream id %lu, fetch id %lu\n",
                    instruction->seqNum, dbsp->getSupplyingStreamId(), dbsp->getSupplyingTargetId());
            instruction->setFsqId(dbsp->getSupplyingStreamId());
            instruction->setFtqId(dbsp->getSupplyingTargetId());
        } else if (isFTBPred()) {
            DPRINTF(DecoupleBP, "Set instruction %lu with stream id %lu, fetch id %lu\n",
                    instruction->seqNum, dbpftb->getSupplyingStreamId(), dbpftb->getSupplyingTargetId());
            instruction->setFsqId(dbpftb->getSupplyingStreamId());
            instruction->setFtqId(dbpftb->getSupplyingTargetId());
        } else if (isBTBPred()) {
            DPRINTF(DecoupleBP, "Set instruction %lu with stream id %lu, fetch id %lu\n",
                    instruction->seqNum, dbpbtb->getSupplyingStreamId(), dbpbtb->getSupplyingTargetId());
            instruction->setFsqId(dbpbtb->getSupplyingStreamId());
            instruction->setFtqId(dbpbtb->getSupplyingTargetId());
        }
    }

#if TRACING_ON
    if (trace) {
        instruction->traceData =
            cpu->getTracer()->getInstRecord(curTick(), cpu->tcBase(tid),
                    instruction->staticInst, this_pc, curMacroop);
    }
#else
    instruction->traceData = NULL;
#endif

    // Add instruction to the CPU's list of instructions.
    instruction->setInstListIt(cpu->addInst(instruction));

    // Write the instruction to the first slot in the queue
    // that heads to decode.
    assert(numInst < fetchWidth);
    fetchQueue[tid].push_back(instruction);
    assert(fetchQueue[tid].size() <= fetchQueueSize);
    DPRINTF(Fetch, "[tid:%i] Fetch queue entry created (%i/%i).\n",
            tid, fetchQueue[tid].size(), fetchQueueSize);
    //toDecode->insts[toDecode->size++] = instruction;

    // Keep track of if we can take an interrupt at this boundary
    delayedCommit[tid] = instruction->isDelayedCommit();

    instruction->fallThruPC = this_pc.getFallThruPC();

    return instruction;
}

bool
Fetch::prepareFetchAddress(ThreadID tid, bool &status_change, unsigned ftqIndex)
{
    DPRINTF(Fetch, "Attempting to fetch from [tid:%i]\n", tid);

    // The current PC - directly use the actual instruction address
    PCStateBase &this_pc = *pc[tid];

    // Handle status transitions and cache access
    if (icacheHandler->getOverallCacheStatus(tid) == AccessComplete) {
        DPRINTF(Fetch, "[tid:%i] Icache miss is complete.\n", tid);
        setThreadStatus(tid, Running);
        setAllFetchStalls(StallReason::NoStall);
        status_change = true;
        return true;
    } else if (canFetchInstructions(tid)) {
        // Check if we need to fetch from icache based on FTQ entry status
        // For RISC-V, we don't need ROM microcode, only check FTQ status and macroop
        if (needNewFTQEntry(tid) && !macroop[tid]) {
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled due to need new FTQ entry\n", tid);
            return true;    // to send icache request in performInstructionFetch!
        } else if (checkInterrupt(this_pc.instAddr()) && !delayedCommit[tid]) {
            // Stall CPU if an interrupt is posted
            ++fetchStats.miscStallCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is stalled!\n", tid);
            return false;
        }
        return true;
    } else {
        if (fetchStatus[tid] == Idle) {
            ++fetchStats.idleCycles;
            DPRINTF(Fetch, "[tid:%i] Fetch is idle!\n", tid);
        }
        // Status is Idle, so fetch should do nothing.
        return false;
    }
}

void
Fetch::fetch(bool &status_change)
{
    //////////////////////////////////////////
    // Start actual fetch
    //////////////////////////////////////////
    ThreadID tid = selectFetchThread();
    if (tid == InvalidThreadID) {
        return;
    }

    if (!checkDecoupledFrontend(tid)) {
        return;
    }

    unsigned ftqIndex = 0;  // Default FTQ index for single fetch mode
    if (!prepareFetchAddress(tid, status_change, ftqIndex)) {
        return;
    }

    ++fetchStats.cycles;

    performInstructionFetch(tid, ftqIndex);
}

StallReason
Fetch::checkMemoryNeeds(ThreadID tid, const PCStateBase &this_pc,
                        const StaticInstPtr &curMacroop, unsigned ftqIndex)
{
    // If we are in the middle of a macro-op, the decoder does not need
    // more memory bytes. It will continue processing the existing instruction.
    if (curMacroop) {
        return StallReason::NoStall;
    }

    Addr fetch_pc = this_pc.instAddr();

    // Check if fetch buffer is valid and contains this PC
    if (!fetchBuffer[tid][ftqIndex].valid) {
        DPRINTF(Fetch, "[tid:%i] Fetch buffer invalid, stalling on ICache\n", tid);
        return StallReason::IcacheStall;
    }

    // Check if the fetch buffer contains enough bytes for this instruction
    // We need at least 4 bytes to decode any RISC-V instruction (including compressed)
    if (fetch_pc < fetchBuffer[tid][ftqIndex].startPC ||
        fetch_pc + 4 > fetchBuffer[tid][ftqIndex].startPC + fetchBuffer[tid][ftqIndex].validBytes) {
        DPRINTF(Fetch, "[tid:%i] PC %#x outside valid buffer range [%#x, %#x), stalling on ICache\n",
                tid, fetch_pc, fetchBuffer[tid][ftqIndex].startPC,
                fetchBuffer[tid][ftqIndex].startPC + fetchBuffer[tid][ftqIndex].validBytes);
        return StallReason::IcacheStall;
    }

    // Supply bytes to decoder - always provide 4 bytes for RISC-V
    auto *dec_ptr = decoder[tid];
    Addr offset_in_buffer = fetch_pc - fetchBuffer[tid][ftqIndex].startPC;
    memcpy(dec_ptr->moreBytesPtr(),
           fetchBuffer[tid][ftqIndex].data + offset_in_buffer, 4);

    DPRINTF(Fetch, "[tid:%i] Supplying 4 bytes from fetchBuffer at PC %#x (offset %d)\n",
            tid, fetch_pc, offset_in_buffer);

    // Call decoder with the actual instruction PC
    decoder[tid]->moreBytes(this_pc, fetch_pc);

    return StallReason::NoStall;
}

bool
Fetch::processSingleInstruction(ThreadID tid, PCStateBase &pc,
                               StaticInstPtr &curMacroop, unsigned ftqIndex)
{
    auto *dec_ptr = decoder[tid];
    bool predictedBranch = false;
    bool newMacroop = false;

    // Create a copy of the current PC state to calculate the next PC.
    std::unique_ptr<PCStateBase> next_pc(pc.clone());

    // Decode the instruction, handling macro-op transitions.
    StaticInstPtr staticInst = nullptr;
    if (!curMacroop) {
        // Decode a new instruction if not currently in a macro-op.
        staticInst = dec_ptr->decode(pc);
        ++fetchStats.insts;

        if (staticInst->isMacroop()) {
            curMacroop = staticInst;
            DPRINTF(Fetch, "[tid:%i] Macroop instruction decoded\n", tid);
        }
    }
    if (curMacroop) {
        // Fetch the next micro-op from the current macro-op.
        staticInst = curMacroop->fetchMicroop(pc.microPC());
        DPRINTF(Fetch, "[tid:%i] Fetched macroop microop\n", tid);
        // Check if this is the last micro-op.
        newMacroop = staticInst->isLastMicroop();
    }

    // Build the dynamic instruction and add it to the fetch queue
    DynInstPtr instruction = buildInst(tid, staticInst, curMacroop, pc, *next_pc, true);

    // Special handling for RISC-V vector configuration instructions.
    if (staticInst->isVectorConfig()) {
        waitForVsetvl = dec_ptr->stall();
        DPRINTF(Fetch, "[tid:%i] Vector config instruction, waitForVsetvl=%d\n",
                tid, waitForVsetvl);
    }

    instruction->setVersion(localSquashVer);
    ppFetch->notify(instruction);
    numInst++;

#if TRACING_ON
    if (debug::O3PipeView) {
        instruction->fetchTick = curTick();
    }
#endif

    // Save current PC to next_pc first
    set(next_pc, pc);

    // Handle branch prediction for non-decoupled frontend
    if (!isDecoupledFrontend()) {
        predictedBranch = pc.branching();
    } else { // decoupled frontend
        predictedBranch = lookupAndUpdateNextPC(instruction, *next_pc, ftqIndex);
    }

    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Branch detected with PC = %s, target = %s\n",
                instruction->threadNumber, pc, *next_pc);
    }

    // A new macro-op also begins if the PC changes discontinuously.
    newMacroop |= pc.instAddr() != next_pc->instAddr();
    if (newMacroop) {
        curMacroop = NULL;
        DPRINTF(Fetch, "[tid:%i] New macroop transition, PC=%s\n",
                tid, pc);
    }

    // Update the main PC state for the next instruction.
    set(pc, *next_pc);

    return predictedBranch;
}

void
Fetch::performInstructionFetch(ThreadID tid, unsigned ftqIndex)
{
    // Initialize local variables
    PCStateBase &pc_state = *pc[tid];
    StaticInstPtr &curMacroop = macroop[tid];
    bool predictedBranch = false;

    // Determine which FTQ indices to process
    std::vector<unsigned> activeFTQs;
    if (hasPendingDualFetch(tid)) {
        // Dual fetch mode: process all active FTQs with valid buffers
        for (unsigned i = 0; i < 2; ++i) {
            if (fetch2Coord[tid].ftqActive[i] && fetchBuffer[tid][i].valid) {
                activeFTQs.push_back(i);
            }
        }
        DPRINTF(Fetch, "[tid:%i] Dual fetch mode: processing %d active FTQs\n",
                tid, activeFTQs.size());
    } else {
        // Single fetch mode: only process the specified ftqIndex
        activeFTQs.push_back(ftqIndex);
        DPRINTF(Fetch, "[tid:%i] Single fetch mode: processing FTQ %d\n", tid, ftqIndex);
    }

    // Process each active FTQ in sequence
    StallReason stall = StallReason::NoStall;
    for (unsigned currentFTQ : activeFTQs) {
        DPRINTF(Fetch, "[tid:%i] Processing FTQ %d, numInst=%d\n", tid, currentFTQ, numInst);

        // Main instruction fetch loop for current FTQ - reuse existing logic
        while (numInst < fetchWidth && fetchQueue[tid].size() < fetchQueueSize &&
               !predictedBranch && !waitForVsetvl) {

            // Check memory needs and supply bytes to decoder if required
            stall = checkMemoryNeeds(tid, pc_state, curMacroop, currentFTQ);
            if (stall != StallReason::NoStall) {
                break;
            }

            // Inner loop: extract as many instructions as possible from buffered
            // memory. This is primarily for macro-op instructions, which decode
            // into multiple micro-ops.
            do {
                // Process a single instruction, from decoding to PC update.
                predictedBranch = processSingleInstruction(tid, pc_state, curMacroop, currentFTQ);

            } while (curMacroop &&
                     numInst < fetchWidth &&
                     fetchQueue[tid].size() < fetchQueueSize);
        }

        if (usedUpFetchTargets) { // if we have used up the current FTQ entry, finish it
            finishCurrentFetchTarget();
            if (isBTBPred()) {
                bool in_loop = false;
                bool got_target = dbpbtb->trySupplyFetchWithTarget(pc[tid]->instAddr(), in_loop);
                if (got_target) {
                    usedUpFetchTargets = false;
                }
            }
        }

        // If we hit limits or predicted branch, stop processing other FTQs
        if (numInst >= fetchWidth || fetchQueue[tid].size() >= fetchQueueSize) {
            DPRINTF(Fetch, "[tid:%i] Stopping FTQ processing due to: numInst=%d/%d, "
                    "queueSize=%d/%d\n", tid, numInst, fetchWidth, fetchQueue[tid].size(), fetchQueueSize);
            break;
        }
        // clear predictedBranch flag
        predictedBranch = false;
    }

    // Debug output for fetch queue contents
    DPRINTF(FetchVerbose, "FetchQue start dumping\n");
    for (auto it : fetchQueue[tid]) {
        DPRINTF(FetchVerbose, "inst: %s\n", it->staticInst->disassemble(it->pcState().instAddr()));
    }

    // Handle stall conditions and update statistics
    if (stall != StallReason::NoStall) {
        setAllFetchStalls(stall);
    }

    // Log why fetch stopped
    if (predictedBranch) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, predicted branch instruction encountered.\n", tid);
    } else if (numInst >= fetchWidth) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, reached fetch bandwidth for this cycle.\n", tid);
    } else if (stall != StallReason::NoStall) {
        DPRINTF(Fetch, "[tid:%i] Done fetching, stalled due to %s.\n", tid,
                stall == StallReason::IcacheStall ? "ICache" : "other reasons");
    } else {
        DPRINTF(Fetch, "[tid:%i] Done fetching, no more instructions to fetch.\n", tid);
    }

    // Update persistent state
    macroop[tid] = curMacroop;

    if (numInst > 0) {
        wroteToTimeBuffer = true;
    }

    assert(fetchStatus[tid] == Running && "Fetch should be running");
    sendNextCacheRequest(tid, pc_state);
}


} // namespace o3
} // namespace gem5
