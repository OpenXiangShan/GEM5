/*
 * Copyright (c) 2012, 2014 ARM Limited
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
#include "cpu/o3/decode.hh"

#include <algorithm>
#include <queue>

#include "arch/generic/pcstate.hh"
#include "arch/riscv/insts/fusion.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "debug/Activity.hh"
#include "debug/Counters.hh"
#include "debug/Decode.hh"
#include "debug/DecoupleBP.hh"
#include "debug/O3PipeView.hh"
#include "params/BaseO3CPU.hh"
#include "sim/full_system.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

namespace gem5
{

namespace o3
{

Decode::Decode(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      renameToDecodeDelay(params.renameToDecodeDelay),
      iewToDecodeDelay(params.iewToDecodeDelay),
      commitToDecodeDelay(params.commitToDecodeDelay),
      fetchToDecodeDelay(params.fetchToDecodeDelay),
      decodeToFetchDelay(params.decodeToFetchDelay),
      decodeWidth(params.decodeWidth),
      numThreads(params.numThreads),
      enableLoadFusion(params.enable_loadFusion),
      stats(_cpu)
{
    if (decodeWidth > MaxWidth)
        fatal("decodeWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             decodeWidth, static_cast<int>(MaxWidth));

    // @todo: Make into a parameter
    for (int i=0;i<numThreads;i++) {
        fixedbuffer[i] = boost::circular_buffer<DynInstPtr>(decodeWidth);
    }
    // This buffer preserves the fetch->decode pipeline contents when decode
    // stalls while TimeBuffer keeps advancing. Its depth matches the original
    // forward pipeline window; fetch is backpressured before full to absorb
    // both the decode->fetch feedback delay and the request already issued in
    // the current cycle before decode computes backpressure.
    // In SMT mode, each thread has its own stall buffer for isolation.
    const auto stallGroupDepth = fetchToDecodeDelay + 1;
    for (int i=0; i<numThreads; i++) {
        stallBuffer[i] = boost::circular_buffer<DynInstPtr>(
        decodeWidth * stallGroupDepth);
        eachstallSize[i] = boost::circular_buffer<int>(stallGroupDepth);
    }


    decodeStalls.resize(decodeWidth, StallReason::NoStall);
    statistics::registerDumpCallback([this]() {
        int idx = 0;
        for (auto it : this->fusionType) {
            this->stats.fusedInsts.subname(idx, it.first);
            this->stats.fusedInsts[idx] = it.second;
            idx++;
        }
        this->fusionType.clear();
    });
}

void
Decode::startupStage()
{
    resetStage();
}

void
Decode::clearStates(ThreadID tid)
{

}

void
Decode::resetStage()
{
    _status = Inactive;
}

std::string
Decode::name() const
{
    return cpu->name() + ".decode";
}

Decode::DecodeStats::DecodeStats(CPU *cpu)
    : statistics::Group(cpu, "decode"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is idle"),
      ADD_STAT(smtidleCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch was idle per tid"),           
      ADD_STAT(blockedCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is blocked"),
      ADD_STAT(smtblockedCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch has spent blocked per tid"),  
      ADD_STAT(smtnotactiveCycles, statistics::units::Cycle::get(),
             "Number of cycles fetch no active per tid"),                
      ADD_STAT(runCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is running"),
      ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is unblocking"),
      ADD_STAT(squashCycles, statistics::units::Cycle::get(),
               "Number of cycles decode is squashing"),
      ADD_STAT(branchResolved, statistics::units::Count::get(),
               "Number of times decode resolved a branch"),
      ADD_STAT(branchMispred, statistics::units::Count::get(),
               "Number of times decode detected a branch misprediction"),
      ADD_STAT(numFusedInsts, statistics::units::Count::get(),
               "Number of fused instructions handled by decode"),
      ADD_STAT(fusedInsts, statistics::units::Count::get(),
               "Number of times decode fused instructions by type"),
      ADD_STAT(controlMispred, statistics::units::Count::get(),
               "Number of times decode detected an instruction incorrectly "
               "predicted as a control"),
      ADD_STAT(decodedInsts, statistics::units::Count::get(),
               "Number of instructions handled by decode"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions handled by decode"),
      ADD_STAT(mispredictedByPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to pc"),
      ADD_STAT(mispredictedByNPC, statistics::units::Count::get(),
               "Number of instructions that mispredicted due to npc"),
      // Decode bubbles statistics
      ADD_STAT(decodeBubbles, statistics::units::Count::get(),
               "Unutilized decode pipeline slots while there is no backend-stall"),
      ADD_STAT(decodeBubbles_max, statistics::units::Count::get(),
               "Cycles that decode 0 instructions while there is no backend-stall"),
      ADD_STAT(smtDecodeBubbles, statistics::units::Count::get(),
               "Per-thread decode bubbles for SMT analysis"),
      ADD_STAT(smtDecodeBubbles_max, statistics::units::Count::get(),
               "Per-thread max decode bubbles for SMT analysis"),
    //   ADD_STAT(decodedInstsDist, statistics::units::Count::get(),
    //            "Distribution of decoded instructions per cycle"),
      ADD_STAT(decodeEfficiency, statistics::units::Ratio::get(),
               "Decode efficiency: actual decoded insts vs ideal width")
{
    // Get decodeWidth using helper function to work around protected member access
    
    idleCycles.prereq(idleCycles);
    blockedCycles.prereq(blockedCycles);
    runCycles.prereq(runCycles);
    unblockCycles.prereq(unblockCycles);
    squashCycles.prereq(squashCycles);
    branchResolved.prereq(branchResolved);
    branchMispred.prereq(branchMispred);
    controlMispred.prereq(controlMispred);
    decodedInsts.prereq(decodedInsts);
    squashedInsts.prereq(squashedInsts);
    mispredictedByPC.flags(statistics::total);
    mispredictedByNPC.flags(statistics::total);
    fusedInsts.init(128).flags(statistics::nozero);

    smtidleCycles
            .init(4)
            .flags(statistics::total);
    smtblockedCycles
            .init(4)
            .flags(statistics::total);    
    smtnotactiveCycles
            .init(4)
            .flags(statistics::total);          
    
    // Initialize decode bubbles statistics
    decodeBubbles
            .prereq(decodeBubbles);
    decodeBubbles_max
            .prereq(decodeBubbles_max);
    smtDecodeBubbles
            .init(4)
            .flags(statistics::total);
    smtDecodeBubbles_max
            .init(4)
            .flags(statistics::total);
    // decodedInstsDist
    //         .init(0, cpu->issueWidth, 1)  // min=0, max=decodeWidth, bucket=1
    //         .flags(statistics::nozero);
    
    // Initialize decodeEfficiency formula
    decodeEfficiency = decodedInsts / (cpu->baseStats.numCycles * cpu->issueWidth);
}

void
Decode::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to write information back to fetch.
    toFetch = timeBuffer->getWire(0);

    // Create wires to get information from proper places in time buffer.
    fromRename = timeBuffer->getWire(-renameToDecodeDelay);
    fromIEW = timeBuffer->getWire(-iewToDecodeDelay);
    fromCommit = timeBuffer->getWire(-commitToDecodeDelay);
}

void
Decode::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // Setup wire to write information to proper place in decode queue.
    toRename = decodeQueue->getWire(0);
}

void
Decode::setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)
{
    fetchQueue = fq_ptr;

    // Setup wire to read information from fetch queue.
    fromFetch = fetchQueue->getWire(-fetchToDecodeDelay);
}

void
Decode::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
Decode::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        assert(fixedbuffer[tid].empty());
    }
}

bool
Decode::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (!fixedbuffer[tid].empty())
            return false;
    }
    return true;
}

bool
Decode::checkStall(ThreadID tid) const
{
    bool ret_val = false;


    return ret_val;
}

bool
Decode::fetchInstsValid()
{
    return fromFetch->size > 0;
}

void
Decode::selfSquash(const DynInstPtr &inst, ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] [sn:%llu] Squashing due to incorrect branch "
            "prediction detected at decode.\n", tid, inst->seqNum);

    // Send back mispredict information.
    toFetch->decodeInfo[tid].branchMispredict = true;
    toFetch->decodeInfo[tid].predIncorrect = true;
    toFetch->decodeInfo[tid].mispredictInst = inst;
    toFetch->decodeInfo[tid].squash = true;
    toFetch->decodeInfo[tid].doneSeqNum = inst->seqNum;
    if (inst->isControl()) {
        if (!inst->isReturn()) {
            set(toFetch->decodeInfo[tid].nextPC, *inst->branchTarget());
        } else {
            // if it is return, the target must have already been set in pred target now
            std::unique_ptr<PCStateBase> tgt_ptr(inst->readPredTarg().clone());
            set(toFetch->decodeInfo[tid].nextPC, *tgt_ptr);
        }
    } else {
        std::unique_ptr<PCStateBase> npc_ptr(inst->pcState().clone());
        npc_ptr->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
        set(toFetch->decodeInfo[tid].nextPC, *npc_ptr);
    }

    // Looking at inst->pcState().branching()
    // may yield unexpected results if the branch
    // was predicted taken but aliased in the BTB
    // with a branch jumping to the next instruction (mistarget)
    // Using PCState::branching()  will send execution on the
    // fallthrough and this will not be caught at execution (since
    // branch was correctly predicted taken)
    toFetch->decodeInfo[tid].branchTaken = inst->readPredTaken() ||
                                           inst->isUncondCtrl();

    toFetch->decodeInfo[tid].squashInst = inst;

    InstSeqNum squash_seq_num = inst->seqNum;

    stallSig->blockFetch[tid] = true; // tell fetch don't send new insts

    fixedbuffer[tid].clear();

    // Clear per-thread stallBuffer for the squashed thread
    auto delIt = stallBuffer[tid].begin();
    for (auto it0 = eachstallSize[tid].begin(); it0 != eachstallSize[tid].end();) {
        int size = *it0;
        auto start_it = delIt;
        auto end_it = start_it + size;
        if ((*start_it)->threadNumber == tid) {
            delIt = stallBuffer[tid].erase(start_it, end_it);
            it0 = eachstallSize[tid].erase(it0);
        }
        else {
            delIt = end_it;
            it0++;
        }
    }

    // Squash instructions up until this one
    cpu->removeInstsUntil(squash_seq_num, tid);
}

unsigned
Decode::squash(ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] Squashing.\n",tid);

    fixedbuffer[tid].clear();

    // Clear per-thread stallBuffer for the squashed thread
    auto delIt = stallBuffer[tid].begin();
    for (auto it0 = eachstallSize[tid].begin(); it0 != eachstallSize[tid].end();) {
        int size = *it0;
        auto start_it = delIt;
        auto end_it = start_it + size;
        if ((*start_it)->threadNumber == tid) {
            delIt = stallBuffer[tid].erase(start_it, end_it);
            it0 = eachstallSize[tid].erase(it0);
        }
        else {
            delIt = end_it;
            it0++;
        }
    }

    return 0;
}

void
Decode::measureDecodeBubbles(unsigned insts_decoded, ThreadID tid)
{
    // Analogous to Fetch::measureFrontendBubbles
    // Count unutilized decode slots when backend is not stalled
    // For N-wide decode, if decode supplies 0 instructions:
    // - decodeBubbles += N (count total empty slots)
    // - decodeBubbles_max += 1 (count occurrence of all slots being empty)
    
    // Check if backend (rename/issue) is not stalled for this thread
    bool backend_not_stalled = !stallSig->blockDecode[tid] && 
                               !fromCommit->commitInfo[tid].robSquashing;
    
    if (backend_not_stalled) {
        // Backend not stalled, count bubbles
        int unused_slots = decodeWidth - insts_decoded;
        if (unused_slots > 0) {
            // Has empty slots
            stats.decodeBubbles += unused_slots;
            stats.smtDecodeBubbles[tid] += unused_slots;
            
            if (unused_slots == decodeWidth) {
                // All slots empty, insts_decoded == 0
                stats.decodeBubbles_max++;
                stats.smtDecodeBubbles_max[tid]++;
            }
        }
        
        // Sample distribution of decoded instructions
        assert(insts_decoded <= decodeWidth);
        // stats.decodedInstsDist.sample(insts_decoded);
    }
}

void
Decode::updateActivate()
{
    bool any_unblocking = false;

    list<ThreadID>::iterator threads = activeThreads->begin();
    list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!stallSig->blockDecode[tid]) {
            any_unblocking = true;
            break;
        }
    }

    // Decode will have activity if it's unblocking.
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::DecodeIdx);
        }
    } else {
        // If it's not unblocking, then decode will not have any internal
        // activity.  Switch it to inactive.
        if (_status == Active) {
            _status = Inactive;
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::DecodeIdx);
        }
    }
}

void
Decode::moveInstsToBuffer()
{
    // Helper lambda: try to move head group from a specific thread's stallBuffer
    auto tryMoveHeadGroupFromThread = [&](ThreadID tid) -> bool {
        if (stallBuffer[tid].empty()) {
            return false;
        }

        // stallbuffer moves to fixedbuffer in strict FIFO order.
        if (!fixedbuffer[tid].empty()) {
            return false;
        }

        int insts_from_stall = eachstallSize[tid].front();
        eachstallSize[tid].pop_front();
        for (int i = 0; i < insts_from_stall; ++i) {
            const DynInstPtr &inst = stallBuffer[tid].front();
            assert(tid == inst->threadNumber);
            if (localSquashVer[tid].largerThan(inst->getVersion())) {
                inst->setSquashed();
            }
            assert(!fixedbuffer[inst->threadNumber].full());
            fixedbuffer[inst->threadNumber].push_back(inst);
            stallBuffer[tid].pop_front();
        }

        return true;
    };

    // Model one stage advance before latching the next cycle's input so a
    // full stall buffer can still accept a new fetch bundle when its head
    // group moves forward in the same cycle.
    // In SMT mode, we check all threads independently rather than strict FIFO
    // to maximize decode utilization
    std::vector<bool> thread_moved(numThreads, false);
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        thread_moved[tid] = tryMoveHeadGroupFromThread(tid);
    }

    // do not support mixed thread instructions in one fetch group
    int insts_from_fetch = fromFetch->size;
    if (insts_from_fetch != 0) {
        ThreadID tid = fromFetch->insts[0]->threadNumber;

        // move to this thread's stallbuffer
        panic_if(eachstallSize[tid].full(), 
                 "Decode stallbuffer[%d] overflow, has %d stalls\n", 
                 tid, eachstallSize[tid].size() + 1);
        eachstallSize[tid].push_back(insts_from_fetch);
        for (int i = 0; i < insts_from_fetch; i++) {
            stallBuffer[tid].push_back(fromFetch->insts[i]);
        }
    }

    // Debug output - show per-thread stall buffer status
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        DPRINTF(Decode, "[tid:%d] stallBuffer=%zu elems, eachstallSize=%zu groups, fixedbuffer=%zu elems, moved=%d\n",
                tid, stallBuffer[tid].size(), eachstallSize[tid].size(), 
                fixedbuffer[tid].size(), thread_moved[tid]);
    }

    // Check if all threads' stallBuffers are empty
    bool all_empty = true;
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!stallBuffer[tid].empty()) {
            all_empty = false;
            break;
        }
    }
    
    if (all_empty) {
        return;
    }

    // Second attempt: if any thread didn't move before accepting new fetch,
    // try again for those threads that didn't move
    // This allows newly arrived instructions to potentially move directly to fixedbuffer
    // if their thread's fixedbuffer is empty
    // Note: We only retry threads that had instructions in stallBuffer but couldn't move
    // (i.e., thread_moved[tid] == false AND stallBuffer was non-empty at first check)
    // Newly arrived instructions will be handled in the next cycle
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        // Only retry if this thread had instructions but couldn't move them
        // Don't process newly arrived instructions here - they'll be handled next cycle
        if (!thread_moved[tid] && !stallBuffer[tid].empty()) {
            tryMoveHeadGroupFromThread(tid);
        }
    }
}

void
Decode::checkSquash()
{
    for (int i = 0;i < numThreads; i++) {
        if (fromCommit->commitInfo[i].squash) {
            DPRINTF(Decode, "[tid:%i] Squashing instructions due to squash "
                    "from commit.\n", i);
            squash(i);
            localSquashVer[i].update(
                fromCommit->commitInfo[i].squashVersion.getVersion());
            DPRINTF(Decode, "Updating squash version to %u\n",
                    localSquashVer[i].getVersion());
        }
    }
}

bool
Decode::isLsuBound(const DynInstPtr &inst) const
{
    return inst->isMemRef() || inst->isReadBarrier() ||
           inst->isWriteBarrier() || inst->isNonSpeculative();
}

unsigned
Decode::lsuBypassPrefixLength(ThreadID tid) const
{
    if (numThreads <= 1 || tid != 0 ||
        !stallSig->blockDecode[tid] ||
        !stallSig->ldstAdmissionBlocked[tid] ||
        stallSig->blockIEW[tid]) {
        return 0;
    }

    std::vector<DynInstPtr> prefix;
    for (const auto &inst : fixedbuffer[tid]) {
        if (isLsuBound(inst)) {
            break;
        }
        prefix.push_back(inst);
    }

    return prefix.empty() || !cpu->canAdvanceNonLsuPrefix(tid, prefix) ?
        0 : prefix.size();
}

void
Decode::tick()
{
    toRename->fetchStallReason = fromFetch->fetchStallReason;
    wroteToTimeBuffer = false;
    toRenameIndex = 0;
    blockReason = StallReason::NoStall;
    setAllStalls(StallReason::NoStall);

    moveInstsToBuffer();

    checkSquash();

    // check threads stall & status
    ThreadID blocked_tid = InvalidThreadID;
    SmtActiveThreadArbiter active_arbiter;
    auto freezeActiveThread = [this](ThreadID tid) {
        stallSig->blockFetch[tid] = true;
        stallSig->fetchBlockReason[tid] = StallReason::OtherFragStall;
        toFetch->decodeInfo[tid].blockReason =
            stallSig->fetchBlockReason[tid];
    };
    const auto fetchFeedbackReserve =
        numThreads > 1 ? fetchToDecodeDelay : decodeToFetchDelay + 1;
    const bool fifoBackpressured =
        !stallBuffer.empty() &&
        eachstallSize.size() + fetchFeedbackReserve >=
            eachstallSize.capacity();
    const ThreadID fifoHeadTid =
        !stallBuffer.empty() ? stallBuffer.front()->threadNumber : InvalidThreadID;
    const StallReason fifoBlockReason =
        (fifoBackpressured && fifoHeadTid != InvalidThreadID &&
         stallSig->blockDecode[fifoHeadTid]) ?
            stallSig->decodeBlockReason[fifoHeadTid] :
            (fifoBackpressured ? StallReason::OtherFragStall :
                                 StallReason::NoStall);
    unsigned lsu_bypass_prefix[MaxThreads] = {};
    std::vector<ThreadID> active_tids;
    for (int i = 0; i < numThreads; i++) {
        bool block = stallSig->blockDecode[i];
        lsu_bypass_prefix[i] = lsuBypassPrefixLength(i);
        if (lsu_bypass_prefix[i] != 0) {
            DPRINTF(Decode,
                    "[tid:%i] Bypassing LSU admission block for %u non-LSU "
                    "instructions.\n",
                    i, lsu_bypass_prefix[i]);
        }
        bool active = (!block || lsu_bypass_prefix[i] != 0) &&
                      !fixedbuffer[i].empty();

        if(block){
            ++stats.smtblockedCycles[i];
        }

        if(!active)
        {
            ++stats.smtnotactiveCycles[i];
        }

        // Apply per-thread FIFO backpressure
        bool this_thread_fifo_bp = thread_fifo_bp[i];
        
        stallSig->blockFetch[i] = block || this_thread_fifo_bp;
        stallSig->fetchBlockReason[i] =
            stallSig->blockFetch[i] ?
                (block ? stallSig->decodeBlockReason[i] : 
                 thread_fifo_block_reason[i]) :
                StallReason::NoStall;
        toFetch->decodeInfo[i].blockReason = stallSig->fetchBlockReason[i];
        if (active) {
            active_tids.push_back(i);
            const auto freeze = active_arbiter.observe(
                i, smtBorrowPriority(fromIEW->iewInfo[i]));
            if (freeze.previousActive != InvalidThreadID) {
                freezeActiveThread(freeze.previousActive);
            }
            if (freeze.freezeCurrent) {
                freezeActiveThread(i);
            }
        } else if (block && blocked_tid == InvalidThreadID) {
            blocked_tid = i;
        }
    }
    ThreadID tid = active_arbiter.selected();
    // A blocked LSU thread owns the ordered prefix in its decode buffer. Let
    // it consume that prefix first, then use any remaining channel slots for
    // another active SMT thread.
    for (const ThreadID candidate : active_tids) {
        if (lsu_bypass_prefix[candidate] != 0) {
            tid = candidate;
            break;
        }
    }
    if (tid == InvalidThreadID) {
        // all threads are stalled, no need to process
        // Measure decode bubbles for all blocked threads (0 instructions decoded)
        for (int i = 0; i < numThreads; i++) {
            measureDecodeBubbles(0, i);
        }
        
        if (blocked_tid != InvalidThreadID) {
            setAllStalls(stallSig->fetchBlockReason[blocked_tid]);
            blockReason = stallSig->fetchBlockReason[blocked_tid];
        }
        toRename->decodeStallReason = decodeStalls;
        updateActivate();
        return;
    }
    DPRINTF(Decode,"Processing [tid:%i]\n",tid);

    std::vector<ThreadID> decoded_tids;
    auto decode_thread = [&](ThreadID decode_tid) {
        if (toRenameIndex >= decodeWidth) {
            return;
        }
        const unsigned remaining_width = decodeWidth - toRenameIndex;
        const unsigned thread_limit = lsu_bypass_prefix[decode_tid] == 0 ?
            decodeWidth : lsu_bypass_prefix[decode_tid];
        decodeInsts(decode_tid, std::min(remaining_width, thread_limit));
        decoded_tids.push_back(decode_tid);
    };

    decode_thread(tid);
    for (const ThreadID candidate : active_tids) {
        if (candidate != tid && toRenameIndex < decodeWidth) {
            decode_thread(candidate);
        }
    }

    // Each thread may leave a different tail in the fixedbuffer. Preserve
    // per-thread fetch feedback when both threads contributed this cycle.
    for (const ThreadID decoded_tid : decoded_tids) {
        if (!fixedbuffer[decoded_tid].empty()) {
            stallSig->blockFetch[decoded_tid] = true;
            if (stallSig->fetchBlockReason[decoded_tid] == StallReason::NoStall) {
                stallSig->fetchBlockReason[decoded_tid] =
                    stallSig->blockDecode[decoded_tid] ?
                        stallSig->decodeBlockReason[decoded_tid] :
                        StallReason::OtherFragStall;
            }
            toFetch->decodeInfo[decoded_tid].blockReason =
                stallSig->fetchBlockReason[decoded_tid];
        }
    }
    ++stats.runCycles;
    
    // Measure decode bubbles before updating stall signals
    measureDecodeBubbles(toRenameIndex, tid);
    
    if (stallSig->blockDecode[tid]) {
        setAllStalls(stallSig->decodeBlockReason[tid]);
    } else if (toRenameIndex > 0 && decodeStalls[0] == StallReason::NoStall) {
        for (int i = 0; i < decodeStalls.size(); i++) {
            if (i < toRenameIndex) {
                decodeStalls.at(i) = StallReason::NoStall;
            } else {
                decodeStalls.at(i) = fromFetch->fetchStallReason.at(i);
            }
        }
    }
    if (stallSig->blockFetch[tid] &&
        stallSig->fetchBlockReason[tid] == StallReason::NoStall) {
        stallSig->fetchBlockReason[tid] = blockReason;
    }
    toFetch->decodeInfo[tid].blockReason = stallSig->fetchBlockReason[tid];
    updateActivate();

    // if (stalls[tid].rename) {
    //     // stall from rename, pass rename stall
    //     setAllStalls(fromRename->renameInfo[tid].blockReason);
    // } else if (toRenameIndex == 0) {
    //     if (decodeStalls[0] != StallReason::NoStall) {
    //         setAllStalls(decodeStalls[0]);
    //     } else {
    //         // warn("decode have other Stall Reason!");
    //     }
    // } else {
    //     // no stall from decode, pass fetch stall(no stall/FetchFragStall/fetch all stall)
    //     for (int i = 0; i < decodeStalls.size(); i++) {
    //         if (i < toRenameIndex) {    // decode success, no stall
    //             decodeStalls.at(i) = StallReason::NoStall;
    //         } else {    // no insts to decode, pass fetch frag stall
    //             decodeStalls.at(i) = fromFetch->fetchStallReason.at(i);
    //         }
    //     }
    // }

    toRename->decodeStallReason = decodeStalls;

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");

        cpu->activityThisCycle();
    }
}

void
Decode::decodeInsts(ThreadID tid, unsigned max_insts)
{
    // Instructions can come either from the skid buffer or the list of
    // instructions coming from fetch, depending on decode's status.
    int insts_available = fixedbuffer[tid].size();

    std::queue<StallReason> decode_stalls;

    StallReason breakDecode = StallReason::NoStall;

    if (insts_available == 0) {
        DPRINTF(Decode, "[tid:%i] Nothing to do, breaking out"
                " early.\n",tid);
        // Should I change the status to idle?
        ++stats.idleCycles;
        ++stats.smtidleCycles[tid];

        StallReason stall = StallReason::NoStall;
        for (auto iter : fromFetch->fetchStallReason) {
            if (iter != StallReason::NoStall) {
                stall = iter;
                break;
            }
        }
        setAllStalls(stall);
        return;
    }

    auto& insts_to_decode = fixedbuffer[tid];

    DPRINTF(Decode, "[tid:%i] Sending instruction to rename.\n",tid);


    bool vec_decode_limit = false;

    if (!insts_to_decode.front()->isVector()) {
        vec_decode_limit = true;
    }

    std::vector<DynInstPtr> fusionInst;
    unsigned processed_insts = 0;
    while (insts_available > 0 && toRenameIndex < decodeWidth &&
           processed_insts < max_insts) {
        assert(!insts_to_decode.empty());
        if (vec_decode_limit && insts_to_decode.front()->isVector()) {
            break;
        }

        DynInstPtr inst = std::move(insts_to_decode.front());

        insts_to_decode.pop_front();
        ++processed_insts;

        DPRINTF(Decode, "[tid:%i] Processing instruction [sn:%lli] with "
                "PC %s\n", tid, inst->seqNum, inst->pcState());

        if (inst->isSquashed()) {
            DPRINTF(Decode, "[tid:%i] Instruction %i with PC %s is "
                    "squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;

            --insts_available;

            decode_stalls.push(StallReason::InstSquashed);

            continue;
        }

        // Also check if instructions have no source registers.  Mark
        // them as ready to issue at any time.  Not sure if this check
        // should exist here or at a later stage; however it doesn't matter
        // too much for function correctness.
        if (inst->numSrcRegs() == 0) {
            inst->setCanIssue();
        }

        // This current instruction is valid, so add it into the decode
        // queue.  The next instruction may not be valid, so check to
        // see if branches were predicted correctly.
        checkAndFuseInsts(fusionInst, inst);
        fusionInst.push_back(inst);

        ++toRenameIndex;
        ++stats.decodedInsts;
        --insts_available;
        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDecode);
#if TRACING_ON
        if (debug::O3PipeView) {
            inst->decodeTick = curTick() - inst->fetchTick;
            // DPRINTF(O3PipeView, "Record decode for inst sn:%lu\n",
            //         inst->seqNum);
        }
#endif

        if (inst->staticInst->isVectorConfig()) {
            inst->setSerializeBefore();
            inst->setSerializeAfter();
            decode_stalls.push(StallReason::SerializeStall);
            breakDecode = StallReason::SerializeStall;
            DPRINTF(Decode,
                    "[tid:%i] [sn:%llu] Vector config decoded, set serialize barrier and stop decoding younger "
                    "instructions.\n",
                    tid, inst->seqNum);
            break;
        }

        // Ensure that if it was predicted as a branch, it really is a
        // branch.
        if (inst->readPredTaken() && !inst->isControl()) {
            // panic("Instruction predicted as a branch!");

            ++stats.controlMispred;

            // Might want to set some sort of boolean and just do
            // a check at the end
            selfSquash(inst, inst->threadNumber);

            decode_stalls.push(StallReason::InstMisPred);
            breakDecode = StallReason::InstMisPred;

            break;
        }

        // Go ahead and compute any PC-relative branches.
        // This includes direct unconditional control and
        // direct conditional control that is predicted taken.
        //
        // 在 trace 模式下，如果 trace 已标记该指令会触发 trap/异常等控制流改变
        //（hasTraceCtrlFlowChange），则交由 trap/wrong-path 逻辑处理，不在 decode
        // 再做一次基于静态分支目标的校验，避免把 cond->trap 误统计为普通分支
        // mispredict，或在这里产生“错误”的 redirect。
        if (!(cpu->isTraceMode() && inst->hasTraceCtrlFlowChange()) &&
            inst->isDirectCtrl() &&
            (inst->isUncondCtrl() || inst->readPredTaken()))
        {
            ++stats.branchResolved;

            std::unique_ptr<PCStateBase> target = inst->branchTarget();
            // In trace mode, prefer ground-truth next PC from trace to avoid
            // relying on possibly out-of-range immediates (e.g., JAL 20-bit).
            if (cpu->isTraceMode() && inst->hasTraceBranchInfo()) {
                auto &t_override = target->as<RiscvISA::PCState>();
                Addr trace_next = inst->traceBranchNextPC();
                if (trace_next != t_override.pc()) {
                    DPRINTF(DecoupleBP,
                            "[tid:%i] [sn:%llu] Branch pc %s, Override target by trace: %s -> npc=%#lx\n",
                            tid, inst->seqNum, inst->pcState(), *target, trace_next);
                    t_override.pc(trace_next);
                    // assuming 4-byte instruction for now since we don't have this trace inst
                    t_override.npc(trace_next + 4);
                    DPRINTF(DecoupleBP,
                            "[tid:%i] [sn:%llu] After override target: %s, inst->branchTarget: %s\n",
                            tid, inst->seqNum, *target, *inst->branchTarget());
                }
            }
            auto &t = target->as<RiscvISA::PCState>();
            auto &pred = inst->readPredTarg().as<RiscvISA::PCState>();
            if (t.start_equals(pred) && !t.equals(pred)) {
                DPRINTF(
                    DecoupleBP,
                    "Override useless npc, from %#lx->%#lx to %#lx->%#lx\n",
                    pred.pc(), pred.npc(), t.pc(), t.npc());
                inst->setPredTarg(t);
            }
            if (*target != inst->readPredTarg()) {
                ++stats.branchMispred;

                RiscvISA::PCState cpTarget = target->clone()->as<RiscvISA::PCState>();
                RiscvISA::PCState cpPredTarget = inst->readPredTarg().clone()->as<RiscvISA::PCState>();

                if (cpTarget.instAddr() != cpPredTarget.instAddr() && cpTarget.npc() == cpPredTarget.npc()) {
                    ++stats.mispredictedByPC;
                } else if (cpTarget.instAddr() == cpPredTarget.instAddr() && cpTarget.npc() != cpPredTarget.npc()) {
                    ++stats.mispredictedByNPC;
                }

                // Might want to set some sort of boolean and just do
                // a check at the end
                selfSquash(inst, inst->threadNumber);

                decode_stalls.push(StallReason::InstMisPred);
                breakDecode = StallReason::InstMisPred;

                DPRINTF(Decode,
                        "[tid:%i] [sn:%llu] Updating predictions:"
                        " Wrong predicted target: %s PredPC: %s\n",
                        tid, inst->seqNum, inst->readPredTarg(), *target);
                //The micro pc after an instruction level branch should be 0
                inst->setPredTarg(*target);
                break;
            }
        }
        // unpredicted return can make use of ras results to get earlier resteer
        if (inst->isReturn() && !inst->isNonSpeculative() && !inst->readPredTaken()) {
            ++stats.branchMispred;
            decode_stalls.push(StallReason::InstMisPred);
            breakDecode = StallReason::InstMisPred;
            // return target cannot be computed in decode stage since it is an indirect branch
            // need to inquire bpu to get the target
            auto return_addr = fetch_ptr->getPreservedReturnAddr(inst);
            auto target = std::make_unique<RiscvISA::PCState>(return_addr);
            DPRINTF(Decode, "[tid:%i] [sn:%llu] Updating predictions:"
                    " Return not identified by bp: predTaken %d, PredPC: %s Now PC %s\n",
                    tid, inst->seqNum, inst->readPredTaken(), inst->readPredTarg(), *target);
            inst->setPredTaken(true);
            inst->setPredTarg(*target);
            // must squash after setting inst real target because it cannot be computed from static inst
            selfSquash(inst, inst->threadNumber);
            break;
        }
        if (inst->isNonSpeculative() && inst->readPredTaken()) {
            // TODO: redirect to fall thru
            std::unique_ptr<PCStateBase> npc(inst->pcState().clone());
            npc->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
            inst->setPredTaken(false);
            inst->setPredTarg(*npc);
        }
    }
    for (auto &fused_inst : fusionInst) {
        toRename->insts[toRename->size++] = fused_inst;
    }

    if (insts_available) {
        // current cycle insts was not all processed, need to block fetch in next cycle
        stallSig->blockFetch[tid] = true;
        if (breakDecode == StallReason::NoStall) {
            breakDecode = StallReason::OtherFragStall;
        }
    }

    // this stage is totally stalled, set all decode stalls
    if (!decode_stalls.empty()) {
        setAllStalls(decode_stalls.front());
        decode_stalls.pop();
    } else if (breakDecode != StallReason::NoStall) {
        setAllStalls(breakDecode);
    }

    // If we didn't process all instructions, then we will need to block
    // and put all those instructions into the skid buffer.
    if (!insts_to_decode.empty()) {
        blockReason = breakDecode;
    }

    // Record that decode has written to the time buffer for activity
    // tracking.
    if (toRenameIndex) {
        wroteToTimeBuffer = true;
    }
}

void
Decode::checkAndFuseInsts(std::vector<DynInstPtr> &vec, DynInstPtr& cur)
{
    if (vec.empty()) {
        return;
    }
    if (vec.back()->faulted() || cur->faulted()) {
        return;
    }
    if (!enableLoadFusion && (vec.back()->isLoad() || cur->isLoad())) {
        return;
    }
    if (vec.back()->getPC() >= ignoreFusionPC && vec.back()->getPC() < ignoreFusionPC + 8) {
        // ignore fusion for this pc range
        if (cpu->ticksToCycles(curTick() - lastSetIgnoreTick) > keepIgnoreFusionCycles) {
            ignoreFusionPC = 0;
        }
        return;
    }

    // first search
    auto first = (StaticInst*)vec.back()->staticInst.get();
    std::type_index first_type = typeid(0);
    auto it = RiscvISA::deCompressMap.find(typeid(*first));
    if (it != RiscvISA::deCompressMap.end()) {
        first_type = it->second;
    } else {
        first_type = typeid(*first);
    }
    auto finder = RiscvISA::fusionMap.find(RiscvISA::FusionKey(first_type, first->getImm()));
    if (finder == RiscvISA::fusionMap.end()) return ; // no fusion

    // second search
    assert(finder->second.index() == 1);

    auto second = cur->staticInst.get();
    std::type_index typeid_second = typeid(0);
    auto it_second = RiscvISA::deCompressMap.find(typeid(*second));
    if (it_second != RiscvISA::deCompressMap.end()) {
        typeid_second = it_second->second;
    } else {
        typeid_second = typeid(*second);
    }
    auto map = std::get<1>(finder->second);
    finder = map->find(RiscvISA::FusionKey(typeid_second, second->getImm()));
    if (finder == map->end()) return; // no fusion

    assert(finder->second.index() == 0);
    auto creator = std::get<0>(finder->second);

    const std::vector<DynInstPtr> inst_pair = {vec.back(), cur};
    auto fused_inst = creator(inst_pair);
    if (!fused_inst) return;
    vec.pop_back();

    DynInst::Arrays arrays;
    arrays.numSrcs = fused_inst->numSrcRegs();
    arrays.numDests = fused_inst->numDestRegs();

    // ugly but works for now
    RiscvISA::PCState thispc, predPC;
    thispc.set(inst_pair[0]->getPC());
    thispc.setNPC(inst_pair[1]->getNPC());
    predPC.update(thispc);
    predPC.advance();

    // Create a new DynInst from the instruction fetched.
    DynInstPtr instruction = new (arrays) DynInst(
            arrays, fused_inst, fused_inst, thispc, predPC, inst_pair[0]->seqNum, cpu);


    instruction->setVersion(inst_pair[1]->getVersion());
    instruction->setTid(inst_pair[1]->threadNumber);
    instruction->thread = inst_pair[1]->thread;
    instruction->setFtqId(inst_pair[1]->ftqId);

    instruction->instListIt = cpu->instList.insert(inst_pair[0]->instListIt, instruction);
    cpu->instList.erase(inst_pair[0]->instListIt);
    cpu->instList.erase(inst_pair[1]->instListIt);

    dynamic_cast<RiscvISA::FusionInst*>(fused_inst.get())->setFusedInst(instruction);

    cur = instruction;
    stats.numFusedInsts++;

    if (fusionType.find(fused_inst->getMnemonic()) == fusionType.end()) {
        fusionType[fused_inst->getMnemonic()] = 1;
    } else {
        fusionType[fused_inst->getMnemonic()]++;
    }
}

void
Decode::setAllStalls(StallReason decodeStall)
{
    for (int i = 0;i < decodeStalls.size();i++) {
        decodeStalls.at(i) = decodeStall;
    }
}

} // namespace o3
} // namespace gem5
