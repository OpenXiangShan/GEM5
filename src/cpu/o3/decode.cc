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
    : compactionEnabled(params.enableDecodeFusionCompaction &&
                        params.numThreads == 1 && !params.enableTraceMode),
      compactionScanWidth(params.decodeFusionScanWidth),
      compactionFetchReserve(params.fetchToDecodeDelay * params.decodeWidth),
      cpu(_cpu),
      renameToDecodeDelay(params.renameToDecodeDelay),
      iewToDecodeDelay(params.iewToDecodeDelay),
      commitToDecodeDelay(params.commitToDecodeDelay),
      fetchToDecodeDelay(params.fetchToDecodeDelay),
      decodeToFetchDelay(params.decodeToFetchDelay),
      decodeWidth(params.decodeWidth),
      numPreDispatchThreads(params.smtNumPreDispatchThreads),
      aggregateDecodeWidth(decodeWidth * numPreDispatchThreads),
      numThreads(params.numThreads),
      enableLoadFusion(params.enable_loadFusion),
      stats(_cpu, params)
{
    panic_if(numPreDispatchThreads == 0 ||
             numPreDispatchThreads > numThreads ||
             numPreDispatchThreads > 2,
             "smtNumPreDispatchThreads (%u) must be in [1, min(2, "
             "numThreads (%u))]",
             numPreDispatchThreads, numThreads);
    panic_if(aggregateDecodeWidth > MaxWidth,
             "aggregate SMT decode width (%u * %u) exceeds MaxWidth (%u)",
             decodeWidth, numPreDispatchThreads, MaxWidth);

    if (params.enableDecodeFusionCompaction && !compactionEnabled) {
        warn("Decode fusion compaction is inactive for SMT or Trace mode; "
             "using the legacy Decode path");
    }
    if (compactionEnabled) {
        fatal_if(params.enable_loadFusion || params.enableConstantFolding ||
                 params.enableMovImmElimination,
                 "Decode fusion compaction requires enable_loadFusion, "
                 "enableConstantFolding and enableMovImmElimination=False");
        fatal_if(decodeWidth == 0 || decodeWidth != params.renameWidth,
                 "Decode fusion compaction requires nonzero, equal "
                 "decodeWidth and renameWidth");
        fatal_if(fetchToDecodeDelay < Cycles(1) ||
                 fetchToDecodeDelay > Cycles(params.backComSize),
                 "Decode fusion compaction requires fetchToDecodeDelay in "
                 "[1, backComSize]");
        fatal_if(params.enablePredecode && fetchToDecodeDelay < Cycles(3),
                 "Decode fusion compaction with enablePredecode requires "
                 "fetchToDecodeDelay >= 3");
        fatal_if(compactionScanWidth == 0 ||
                 params.decodeFusionBufferSize < compactionScanWidth ||
                 params.decodeFusionBufferSize < compactionFetchReserve,
                 "Invalid decodeFusionBufferSize/decodeFusionScanWidth: "
                 "the FIFO must hold the scan window and in-flight fetch "
                 "reserve, and the scan window must be nonzero");
        compactionBuffer.set_capacity(params.decodeFusionBufferSize);
        inform("Decode fusion compaction enabled: buffer=%u scan=%u output=%u",
               params.decodeFusionBufferSize, compactionScanWidth, decodeWidth);
    }

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
    decodedBranchHistory[tid].clear();
    if (compactionEnabled) {
        assert(tid == 0);
        compactionBuffer.clear();
        nextCompactionBundle = 0;
    }
}

void
Decode::resetStage()
{
    _status = Inactive;
    if (compactionEnabled) {
        compactionBuffer.clear();
        nextCompactionBundle = 0;
    }
}

std::string
Decode::name() const
{
    return cpu->name() + ".decode";
}

Decode::DecodeStats::DecodeStats(CPU *cpu, const BaseO3CPUParams &params)
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
      ADD_STAT(threadsDecodedPerCycle, statistics::units::Count::get(),
               "Distinct SMT threads decoded in one cycle"),
      ADD_STAT(instsDecodedPerCycle, statistics::units::Count::get(),
               "Instructions decoded across all SMT threads in one cycle"),
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
               "Decode efficiency: actual decoded insts vs ideal width"),
      ADD_STAT(compactionInputInsts, statistics::units::Count::get(),
               "Raw entries received by compacting Decode"),
      ADD_STAT(compactionRawInsts, statistics::units::Count::get(),
               "Valid raw instructions consumed by compacting Decode"),
      ADD_STAT(compactionDiscardedInsts, statistics::units::Count::get(),
               "Invalid entries discarded inside the Decode scan window"),
      ADD_STAT(compactionFlushedInsts, statistics::units::Count::get(),
               "Queued entries removed by Decode squash"),
      ADD_STAT(compactionFusedPairs, statistics::units::Count::get(),
               "Pairs fused by compacting Decode"),
      ADD_STAT(compactionOutputInsts, statistics::units::Count::get(),
               "Actual objects sent to Rename by compacting Decode"),
      ADD_STAT(compactionCrossBundleFusions, statistics::units::Count::get(),
               "Fused pairs spanning two Fetch delivery bundles"),
      ADD_STAT(compactionFetchBlockedCycles, statistics::units::Cycle::get(),
               "Cycles compacting Decode blocks Fetch delivery"),
      ADD_STAT(compactionRawPerCycle, statistics::units::Count::get(),
               "Valid raw instructions consumed per compacting cycle"),
      ADD_STAT(compactionOutputPerCycle, statistics::units::Count::get(),
               "Rename objects produced per compacting cycle"),
      ADD_STAT(compactionOccupancy, statistics::units::Count::get(),
               "Compacting FIFO occupancy after consumption"),
      ADD_STAT(compactionStopReasons, statistics::units::Count::get(),
               "Reason compacting Decode stopped in each cycle")
{
    const bool compact = params.enableDecodeFusionCompaction &&
                         params.numThreads == 1 && !params.enableTraceMode;
    compactionRawPerCycle.init(0, compact ? params.decodeFusionScanWidth : 1, 1);
    compactionOutputPerCycle.init(0, params.decodeWidth, 1);
    compactionOccupancy.init(0, compact ? params.decodeFusionBufferSize : 1, 1);
    compactionStopReasons.init(static_cast<unsigned>(CompactionStop::NumReasons));
    const char *stop_names[] = {"inputEmpty", "outputFull", "scanLimit",
        "vectorBoundary", "serialize", "redirect", "backendBlocked", "squash"};
    for (unsigned i = 0; i < static_cast<unsigned>(CompactionStop::NumReasons); ++i)
        compactionStopReasons.subname(i, stop_names[i]);

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
    threadsDecodedPerCycle.init(0, MaxThreads, 1).flags(statistics::pdf);
    instsDecodedPerCycle.init(0, MaxWidth, 1).flags(statistics::pdf);
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
    if (compactionEnabled) {
        assert(compactionBuffer.empty());
    }
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        assert(fixedbuffer[tid].empty());
    }
}

bool
Decode::isDrained() const
{
    if (compactionEnabled && !compactionBuffer.empty()) {
        return false;
    }
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
Decode::squashBranchHistory(ThreadID tid, InstSeqNum squash_seq_num,
                            bool include_squash_inst)
{
    auto &branch_history = decodedBranchHistory[tid];
    while (!branch_history.empty() &&
           (include_squash_inst ?
                branch_history.front().seqNum >= squash_seq_num :
                branch_history.front().seqNum > squash_seq_num)) {
        branch_history.pop_front();
    }
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

    if (compactionEnabled) {
        stats.compactionFlushedInsts += compactionBuffer.size();
        compactionBuffer.clear();
    }
    fixedbuffer[tid].clear();
    squashBranchHistory(tid, squash_seq_num, false);

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

    if (compactionEnabled) {
        const auto &info = fromCommit->commitInfo[tid];
        const size_t queued = compactionBuffer.size();
        for (size_t i = 0; i < queued; ++i) {
            auto entry = compactionBuffer.front();
            compactionBuffer.pop_front();
            if (entry.inst->isSquashed() || entry.inst->seqNum > info.doneSeqNum) {
                ++stats.compactionFlushedInsts;
            } else {
                // Preserve the baseline's selective squash boundary. Older
                // survivors must also remain valid when admitted by Rename.
                entry.inst->setVersion(info.squashVersion);
                compactionBuffer.push_back(entry);
            }
        }
        squashBranchHistory(tid, info.doneSeqNum, false);
        return 0;
    }

    // Selectively remove only instructions younger than squash boundary
    {
        InstSeqNum squash_seq = fromCommit->commitInfo[tid].doneSeqNum;
        for (auto it = fixedbuffer[tid].begin(); it != fixedbuffer[tid].end(); ) {
            if ((*it)->seqNum > squash_seq) {
                it = fixedbuffer[tid].erase(it);
            } else {
                ++it;
            }
        }
    }
    squashBranchHistory(tid, fromCommit->commitInfo[tid].doneSeqNum, false);

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

    int insts_from_fetch = fromFetch->size;
    if (insts_from_fetch != 0) {
        std::array<unsigned, MaxThreads> thread_sizes{};
        for (int i = 0; i < insts_from_fetch; i++) {
            const ThreadID tid = fromFetch->insts[i]->threadNumber;
            assert(tid < numThreads);
            ++thread_sizes[tid];
        }
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            if (thread_sizes[tid] == 0) {
                continue;
            }
            panic_if(eachstallSize[tid].full(),
                     "Decode stallbuffer[%d] overflow, has %d stalls\n",
                     tid, eachstallSize[tid].size() + 1);
            panic_if(stallBuffer[tid].capacity() - stallBuffer[tid].size() <
                         thread_sizes[tid],
                     "Decode stallbuffer[%d] lacks room for %u instructions\n",
                     tid, thread_sizes[tid]);
            assert(thread_sizes[tid] <= decodeWidth);
            eachstallSize[tid].push_back(thread_sizes[tid]);
        }
        for (int i = 0; i < insts_from_fetch; ++i) {
            const ThreadID tid = fromFetch->insts[i]->threadNumber;
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

void
Decode::tick()
{
    if (compactionEnabled) {
        tickCompaction();
        return;
    }
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
    std::vector<ThreadID> active_tids;
    auto freezeActiveThread = [this](ThreadID tid) {
        stallSig->blockFetch[tid] = true;
        stallSig->fetchBlockReason[tid] = StallReason::OtherFragStall;
        toFetch->decodeInfo[tid].blockReason =
            stallSig->fetchBlockReason[tid];
    };
    const auto fetchFeedbackReserve =
        numThreads > 1 ? decodeToFetchDelay + 1 : decodeToFetchDelay + 1;
    
    // Per-thread FIFO backpressure judgment
    // Each thread's stallBuffer is independent, so we check per-thread
    std::vector<bool> thread_fifo_bp(numThreads, false);
    std::vector<StallReason> thread_fifo_block_reason(numThreads, StallReason::NoStall);
    
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!stallBuffer[tid].empty()) {
            thread_fifo_bp[tid] = 
                eachstallSize[tid].size() + fetchFeedbackReserve >=
                eachstallSize[tid].capacity();
            
            if (thread_fifo_bp[tid] && stallSig->blockDecode[tid]) {
                thread_fifo_block_reason[tid] = stallSig->decodeBlockReason[tid];
            } else if (thread_fifo_bp[tid]) {
                thread_fifo_block_reason[tid] = StallReason::OtherFragStall;
            }
        }
    }
    
    // Per-thread backpressure application
    for (int i = 0; i < numThreads; i++) {
        bool block = stallSig->blockDecode[i];
        bool active = !block && !fixedbuffer[i].empty();

        if(block){
            ++stats.smtblockedCycles[i];
        }

        if(!active)
        {
            ++stats.smtnotactiveCycles[i];
        }

        // Apply per-thread FIFO backpressure
        bool this_thread_fifo_bp = thread_fifo_bp[i];
        
        //stallSig->blockFetch[i] = block || this_thread_fifo_bp;
        stallSig->blockFetch[i] = this_thread_fifo_bp;
        stallSig->fetchBlockReason[i] =
            stallSig->blockFetch[i] ?
                (block ? stallSig->decodeBlockReason[i] : 
                 thread_fifo_block_reason[i]) :
                StallReason::NoStall;
        toFetch->decodeInfo[i].blockReason = stallSig->fetchBlockReason[i];
        if (active) {
            active_tids.push_back(i);
            active_arbiter.observe(i, smtBorrowPriority(fromIEW->iewInfo[i]));
        } else if (block && blocked_tid == InvalidThreadID) {
            blocked_tid = i;
        }
    }
    const ThreadID primary_tid = active_arbiter.selected();
    if (primary_tid == InvalidThreadID) {
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
        stats.threadsDecodedPerCycle.sample(0);
        stats.instsDecodedPerCycle.sample(0);
        updateActivate();
        return;
    }

    std::vector<ThreadID> selected_tids{primary_tid};
    for (const ThreadID tid : active_tids) {
        if (tid != primary_tid &&
            selected_tids.size() < numPreDispatchThreads) {
            selected_tids.push_back(tid);
        }
    }
    for (const ThreadID tid : active_tids) {
        if (std::find(selected_tids.begin(), selected_tids.end(), tid) ==
            selected_tids.end()) {
            freezeActiveThread(tid);
        }
    }

    unsigned decoded_this_cycle = 0;
    unsigned decoded_threads_this_cycle = 0;
    for (const ThreadID tid : selected_tids) {
        DPRINTF(Decode, "Processing [tid:%i]\n", tid);
        const unsigned before = toRenameIndex;
        decodeInsts(tid, decodeWidth);
        const unsigned decoded = toRenameIndex - before;
        decoded_this_cycle += decoded;
        decoded_threads_this_cycle += decoded != 0;
        measureDecodeBubbles(decoded, tid);

        if (!fixedbuffer[tid].empty()) {
            stallSig->blockFetch[tid] = true;
            if (stallSig->fetchBlockReason[tid] == StallReason::NoStall) {
                stallSig->fetchBlockReason[tid] =
                    stallSig->blockDecode[tid] ?
                        stallSig->decodeBlockReason[tid] :
                        StallReason::OtherFragStall;
            }
        }
        toFetch->decodeInfo[tid].blockReason =
            stallSig->fetchBlockReason[tid];
    }
    ++stats.runCycles;

    stats.threadsDecodedPerCycle.sample(decoded_threads_this_cycle);
    stats.instsDecodedPerCycle.sample(decoded_this_cycle);

    if (stallSig->blockDecode[primary_tid]) {
        setAllStalls(stallSig->decodeBlockReason[primary_tid]);
    } else if (toRenameIndex > 0 && decodeStalls[0] == StallReason::NoStall) {
        for (int i = 0; i < decodeStalls.size(); i++) {
            if (i < toRenameIndex) {
                decodeStalls.at(i) = StallReason::NoStall;
            } else {
                decodeStalls.at(i) = fromFetch->fetchStallReason.at(i);
            }
        }
    }
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

bool
Decode::compactionInstInvalid(const DynInstPtr &inst) const
{
    return inst->isSquashed() ||
           localSquashVer[inst->threadNumber].largerThan(inst->getVersion());
}

bool
Decode::canFusePair(const DynInstPtr &first, const DynInstPtr &second) const
{
    const auto eligible = [this](const DynInstPtr &inst) {
        return !compactionInstInvalid(inst) && !inst->faulted() &&
               !inst->isFusion() && !inst->isMicroop() &&
               !inst->isControl() && !inst->isVector() &&
               !inst->staticInst->isVectorConfig() &&
               !inst->isSerializeBefore() && !inst->isSerializeAfter() &&
               !inst->isNonSpeculative() && !inst->readPredTaken();
    };
    return eligible(first) && eligible(second) &&
           first->threadNumber == second->threadNumber &&
           first->getVersion() == second->getVersion() &&
           first->getFtqId() == second->getFtqId() &&
           first->getLoopIteration() == second->getLoopIteration() &&
           first->seqNum < second->seqNum &&
           first->pcState().getFallThruPC() == second->getPC();
}

void
Decode::receiveCompactionInsts()
{
    const int incoming = fromFetch->size;
    panic_if(incoming < 0 || incoming > decodeWidth,
             "Invalid compacting Decode input size: %d", incoming);
    panic_if(compactionBuffer.size() + incoming > compactionBuffer.capacity(),
             "Compacting Decode FIFO overflow: queued=%u incoming=%d "
             "capacity=%u", compactionBuffer.size(), incoming,
             compactionBuffer.capacity());
    if (!incoming) {
        return;
    }

    const uint64_t bundle = nextCompactionBundle++;
    for (int i = 0; i < incoming; ++i) {
        const auto &inst = fromFetch->insts[i];
        assert(inst && inst->threadNumber == 0);
        const auto &squash = fromCommit->commitInfo[0];
        if (squash.squash && inst->seqNum <= squash.doneSeqNum &&
            !inst->isSquashed()) {
            inst->setVersion(squash.squashVersion);
        }
        if (compactionInstInvalid(inst)) {
            inst->setSquashed();
        }
        // Keep invalid entries in place until scanned. Removing them here
        // would hide an adjacency boundary and grant free scan bandwidth.
        compactionBuffer.push_back({inst, bundle});
    }
    stats.compactionInputInsts += incoming;
}

void
Decode::recordCompactionDecode(const DynInstPtr &inst)
{
    ++stats.decodedInsts;
    cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtDecode);
#if TRACING_ON
    if (debug::O3PipeView && inst->fetchTick != Tick(-1)) {
        inst->decodeTick = curTick() - inst->fetchTick;
    }
#endif
}

void
Decode::decodeCompactedInsts(unsigned &raw, unsigned &discarded,
                            unsigned &fused, CompactionStop &stop)
{
    const unsigned window = std::min<size_t>(compactionScanWidth,
                                             compactionBuffer.size());
    unsigned scanned = 0;
    bool first_valid = true;
    bool scalar_head = false;
    while (scanned < window && toRenameIndex < decodeWidth &&
           !compactionBuffer.empty()) {
        const CompactionEntry first = compactionBuffer.front();
        if (compactionInstInvalid(first.inst)) {
            first.inst->setSquashed();
            compactionBuffer.pop_front();
            ++scanned;
            ++discarded;
            ++stats.squashedInsts;
            continue;
        }
        if (first_valid) {
            scalar_head = !first.inst->isVector();
            first_valid = false;
        }
        if (scalar_head && first.inst->isVector()) {
            stop = CompactionStop::VectorBoundary;
            blockReason = StallReason::OtherFragStall;
            return;
        }

        StaticInstPtr prepared;
        CompactionEntry second;
        if (scanned + 1 < window && compactionBuffer.size() > 1) {
            second = compactionBuffer[1];
            if (canFusePair(first.inst, second.inst)) {
                // Preparation may expire ignoreFusionPC, but must not
                // consume input, redirect, or replace the global instList.
                prepared = prepareFusion(first.inst, second.inst);
            }
        }

        DynInstPtr output;
        if (prepared) {
            compactionBuffer.pop_front();
            compactionBuffer.pop_front();
            scanned += 2;
            raw += 2;
            ++fused;
            recordCompactionDecode(first.inst);
            recordCompactionDecode(second.inst);
            output = applyFusion(first.inst, second.inst, prepared);
            if (first.bundle != second.bundle) {
                ++stats.compactionCrossBundleFusions;
            }
            // Preserve original O3PipeView records. Emitting another fetch
            // record for the fused object would duplicate the first seqNum.
            DPRINTF(Decode, "Compaction fusion: first=%llu second=%llu "
                    "output=%llu bundles=%llu/%llu\n", first.inst->seqNum,
                    second.inst->seqNum, output->seqNum, first.bundle,
                    second.bundle);
        } else {
            compactionBuffer.pop_front();
            ++scanned;
            ++raw;
            output = first.inst;
            recordCompactionDecode(output);
        }

        if (output->numSrcRegs() == 0) {
            output->setCanIssue();
        }
        assert(toRename->size < decodeWidth);
        toRename->insts[toRename->size++] = output;
        ++toRenameIndex;
        wroteToTimeBuffer = true;

        // Only a consumed single instruction may perform control effects.
        // Fused pairs contain neither control nor serializing instructions.
        if (!prepared) {
            const StallReason control = processInstControl(output, 0);
            if (control != StallReason::NoStall) {
                blockReason = control;
                stop = control == StallReason::SerializeStall ?
                    CompactionStop::Serialize : CompactionStop::Redirect;
                return;
            }
        }
    }

    if (toRenameIndex == decodeWidth) {
        stop = CompactionStop::OutputFull;
    } else if (compactionBuffer.empty()) {
        stop = CompactionStop::InputEmpty;
        blockReason = StallReason::FetchFragStall;
    } else {
        stop = CompactionStop::ScanLimit;
        blockReason = discarded ? StallReason::InstSquashed :
                                  StallReason::OtherFragStall;
    }
}

void
Decode::tickCompaction()
{
    assert(numThreads == 1);
    assert(toRename->size == 0);
    toRename->fetchStallReason = fromFetch->fetchStallReason;
    wroteToTimeBuffer = false;
    toRenameIndex = 0;
    blockReason = StallReason::NoStall;
    setAllStalls(StallReason::NoStall);

    checkSquash();
    receiveCompactionInsts();

    unsigned raw = 0;
    unsigned discarded = 0;
    unsigned fused = 0;
    CompactionStop stop = CompactionStop::InputEmpty;
    const bool active = std::find(activeThreads->begin(), activeThreads->end(),
                                  ThreadID(0)) != activeThreads->end();
    if (fromCommit->commitInfo[0].squash) {
        stop = CompactionStop::Squash;
        blockReason = StallReason::CommitSquash;
        ++stats.squashCycles;
    } else if (stallSig->blockDecode[0] || !active) {
        stop = CompactionStop::BackendBlocked;
        blockReason = stallSig->decodeBlockReason[0];
        ++stats.smtblockedCycles[0];
    } else if (compactionBuffer.empty()) {
        ++stats.idleCycles;
        ++stats.smtidleCycles[0];
        ++stats.smtnotactiveCycles[0];
        blockReason = StallReason::OtherFetchStall;
        for (const auto reason : fromFetch->fetchStallReason) {
            if (reason != StallReason::NoStall) {
                blockReason = reason;
                break;
            }
        }
    } else {
        decodeCompactedInsts(raw, discarded, fused, stop);
        ++stats.runCycles;
    }

    assert(raw >= fused && toRenameIndex == raw - fused);
    assert(raw + discarded <= compactionScanWidth);
    assert(toRenameIndex == toRename->size && toRenameIndex <= decodeWidth);
    stats.compactionRawInsts += raw;
    stats.compactionDiscardedInsts += discarded;
    stats.compactionFusedPairs += fused;
    stats.compactionOutputInsts += toRenameIndex;
    stats.compactionRawPerCycle.sample(raw);
    stats.compactionOutputPerCycle.sample(toRenameIndex);
    stats.compactionOccupancy.sample(compactionBuffer.size());
    ++stats.compactionStopReasons[static_cast<unsigned>(stop)];
    stats.threadsDecodedPerCycle.sample(raw != 0);
    stats.instsDecodedPerCycle.sample(raw);
    measureDecodeBubbles(toRenameIndex, 0);

    for (unsigned i = toRenameIndex; i < decodeWidth; ++i) {
        decodeStalls[i] = blockReason;
    }
    toRename->decodeStallReason = decodeStalls;

    // Decode precedes Fetch in CPU::tick. Reserve D-1 existing in-flight
    // bundles plus the bundle Fetch may send this cycle, even if consumption
    // stops completely next cycle. StallSignals persist across ticks.
    const bool fifo_blocked = compactionBuffer.size() + compactionFetchReserve >
                              compactionBuffer.capacity();
    const bool redirect = toFetch->decodeInfo[0].squash;
    stallSig->blockFetch[0] = redirect || fifo_blocked;
    stallSig->fetchBlockReason[0] = redirect ? StallReason::InstMisPred :
        fifo_blocked ? (stallSig->blockDecode[0] ?
            stallSig->decodeBlockReason[0] : StallReason::OtherFragStall) :
        StallReason::NoStall;
    toFetch->decodeInfo[0].blockReason = stallSig->fetchBlockReason[0];
    stats.compactionFetchBlockedCycles += stallSig->blockFetch[0];

    DPRINTF(Decode, "Compaction: raw=%u discarded=%u fused=%u output=%u "
            "queued=%u stop=%u\n", raw, discarded, fused, toRenameIndex,
            compactionBuffer.size(), static_cast<unsigned>(stop));
    updateActivate();
    if (wroteToTimeBuffer || discarded) {
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
    while (insts_available > 0 && toRenameIndex < aggregateDecodeWidth &&
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

        const StallReason control_stall = processInstControl(inst, tid);
        if (control_stall != StallReason::NoStall) {
            decode_stalls.push(control_stall);
            breakDecode = control_stall;
            break;
        }
    }
    for (auto &fused_inst : fusionInst) {
        assert(toRename->size < MaxWidth);
        toRename->insts[toRename->size++] = fused_inst;
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

StallReason
Decode::processInstControl(const DynInstPtr &inst, ThreadID tid)
{
    if (inst->staticInst->isVectorConfig()) {
        inst->setSerializeBefore();
        inst->setSerializeAfter();
        DPRINTF(Decode,
                "[tid:%i] [sn:%llu] Vector config decoded, set serialize barrier and stop decoding younger "
                "instructions.\n",
                tid, inst->seqNum);
        return StallReason::SerializeStall;
    }

    // Ensure that if it was predicted as a branch, it really is a
    // branch.
    if (inst->readPredTaken() && !inst->isControl() &&
        !inst->isPredecodeChecked()) {
        // panic("Instruction predicted as a branch!");

        ++stats.controlMispred;

        // Might want to set some sort of boolean and just do
        // a check at the end
        selfSquash(inst, inst->threadNumber);

        return StallReason::InstMisPred;
    }

    // Go ahead and compute any PC-relative branches.
    // This includes direct unconditional control and
    // direct conditional control that is predicted taken.
    //
    // 在 trace 模式下，如果 trace 已标记该指令会触发 trap/异常等控制流改变
    //（hasTraceCtrlFlowChange），则交由 trap/wrong-path 逻辑处理，不在 decode
    // 再做一次基于静态分支目标的校验，避免把 cond->trap 误统计为普通分支
    // mispredict，或在这里产生“错误”的 redirect。
    if (!inst->isPredecodeChecked() &&
        !(cpu->isTraceMode() && inst->hasTraceCtrlFlowChange()) &&
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

            DPRINTF(Decode,
                    "[tid:%i] [sn:%llu] Updating predictions:"
                    " Wrong predicted target: %s PredPC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg(), *target);
            //The micro pc after an instruction level branch should be 0
            inst->setPredTarg(*target);
            return StallReason::InstMisPred;
        }
    }
    // unpredicted return can make use of ras results to get earlier resteer
    if (!inst->isPredecodeChecked() &&
        inst->isReturn() && !inst->isNonSpeculative() &&
        !inst->readPredTaken()) {
        ++stats.branchMispred;
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
        return StallReason::InstMisPred;
    }
    if (inst->isNonSpeculative() && inst->readPredTaken()) {
        // TODO: redirect to fall thru
        std::unique_ptr<PCStateBase> npc(inst->pcState().clone());
        npc->as<RiscvISA::PCState>().set(inst->pcState().getFallThruPC());
        inst->setPredTaken(false);
        inst->setPredTarg(*npc);
    }

    if (inst->isControl() &&
        !(inst->isDirectCtrl() && inst->isUncondCtrl())) {
        branchInfo branch_info = {
            inst->isIndirectCtrl(),
            inst->readPredTaken(),
            inst->readPredTarg().instAddr(),
            inst->seqNum,
            inst->pcState().instAddr(),
        };
        decodedBranchHistory[tid].push_front(branch_info);
        if (decodedBranchHistory[tid].size() > MAX_BRANCH_HISTORY) {
            decodedBranchHistory[tid].pop_back();
        }
    }
    return StallReason::NoStall;
}

void
Decode::checkAndFuseInsts(std::vector<DynInstPtr> &vec, DynInstPtr& cur)
{
    if (vec.empty()) {
        return;
    }
    auto fused_inst = prepareFusion(vec.back(), cur);
    if (!fused_inst) {
        return;
    }
    const DynInstPtr first = vec.back();
    vec.pop_back();
    cur = applyFusion(first, cur, fused_inst);
}

StaticInstPtr
Decode::prepareFusion(const DynInstPtr &first_inst,
                      const DynInstPtr &second_inst)
{
    if (first_inst->faulted() || second_inst->faulted()) {
        return nullptr;
    }
    if (!enableLoadFusion && (first_inst->isLoad() || second_inst->isLoad())) {
        return nullptr;
    }
    if (first_inst->getPC() >= ignoreFusionPC && first_inst->getPC() < ignoreFusionPC + 8) {
        // ignore fusion for this pc range
        if (cpu->ticksToCycles(curTick() - lastSetIgnoreTick) > keepIgnoreFusionCycles) {
            ignoreFusionPC = 0;
        }
        return nullptr;
    }

    // first search
    auto first = (StaticInst*)first_inst->staticInst.get();
    std::type_index first_type = typeid(0);
    auto it = RiscvISA::deCompressMap.find(typeid(*first));
    if (it != RiscvISA::deCompressMap.end()) {
        first_type = it->second;
    } else {
        first_type = typeid(*first);
    }
    auto finder = RiscvISA::fusionMap.find(RiscvISA::FusionKey(first_type, first->getImm()));
    if (finder == RiscvISA::fusionMap.end()) return nullptr; // no fusion

    // second search
    assert(finder->second.index() == 1);

    auto second = second_inst->staticInst.get();
    std::type_index typeid_second = typeid(0);
    auto it_second = RiscvISA::deCompressMap.find(typeid(*second));
    if (it_second != RiscvISA::deCompressMap.end()) {
        typeid_second = it_second->second;
    } else {
        typeid_second = typeid(*second);
    }
    auto map = std::get<1>(finder->second);
    finder = map->find(RiscvISA::FusionKey(typeid_second, second->getImm()));
    if (finder == map->end()) return nullptr; // no fusion

    assert(finder->second.index() == 0);
    auto creator = std::get<0>(finder->second);

    const std::vector<DynInstPtr> inst_pair = {first_inst, second_inst};
    auto fused_inst = creator(inst_pair);
    if (!fused_inst) return nullptr;
    return fused_inst;

}

DynInstPtr
Decode::applyFusion(const DynInstPtr &first_inst,
                    const DynInstPtr &second_inst,
                    const StaticInstPtr &fused_inst)
{
    const std::vector<DynInstPtr> inst_pair = {first_inst, second_inst};
    DynInst::Arrays arrays;
    arrays.numSrcs = fused_inst->numSrcRegs();
    arrays.numDests = fused_inst->numDestRegs();

    // ugly but works for now
    RiscvISA::PCState thispc, predPC;
    if (compactionEnabled) {
        thispc.update(inst_pair[0]->pcState());
    } else {
        thispc.set(inst_pair[0]->getPC());
    }
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
    if (compactionEnabled) {
        instruction->setLoopIteration(inst_pair[1]->getLoopIteration());
        instruction->fallThruPC = inst_pair[1]->pcState().getFallThruPC();
        if (inst_pair[0]->isPredecodeChecked() &&
            inst_pair[1]->isPredecodeChecked()) {
            instruction->setPredecodeChecked();
        }
    }

    instruction->instListIt = cpu->instList.insert(inst_pair[0]->instListIt, instruction);
    cpu->instList.erase(inst_pair[0]->instListIt);
    cpu->instList.erase(inst_pair[1]->instListIt);

    dynamic_cast<RiscvISA::FusionInst*>(fused_inst.get())->setFusedInst(instruction);

    stats.numFusedInsts++;

    if (fusionType.find(fused_inst->getMnemonic()) == fusionType.end()) {
        fusionType[fused_inst->getMnemonic()] = 1;
    } else {
        fusionType[fused_inst->getMnemonic()]++;
    }
    return instruction;
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
