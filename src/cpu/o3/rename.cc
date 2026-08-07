/*
 * Copyright (c) 2010-2012, 2014-2019 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
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

#include "cpu/o3/rename.hh"

#include <list>

#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/reg_class.hh"
#include "debug/Activity.hh"
#include "debug/O3PipeView.hh"
#include "debug/Rename.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

Rename::Rename(CPU *_cpu, const BaseO3CPUParams &params)
    : ratSnapshotActive(params.robWalkPolicy == ROBWalkPolicy::NaiveCpt),
      numMaxRatSnapshot(params.numMaxRatSnapshot),
      ratSnapshotDistance(params.ratSnapshotDistance),
      cpu(_cpu),
      iewToRenameDelay(params.iewToRenameDelay),
      decodeToRenameDelay(params.decodeToRenameDelay),
      commitToRenameDelay(params.commitToRenameDelay),
      renameWidth(params.renameWidth),
      releaseWidth(params.phyregReleaseWidth),
      numThreads(params.numThreads),
      pregBackendBackpressureDonorEnabled(
          params.smtPregBackendBackpressureDonor),
      pregBackendBackpressureDonorHoldCycles(
          params.smtPregBackendBackpressureDonorHoldCycles),
      stats(_cpu, this),
      valuePred(params.valuePred),
      enableSelectiveVPFlush(params.enableSelectiveVPFlush)
{
    if (renameWidth > MaxWidth)
        fatal("renameWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             renameWidth, static_cast<int>(MaxWidth));

    for (uint32_t tid = 0; tid < MaxThreads; tid++) {
        fixedbuffer[tid] = boost::circular_buffer<DynInstPtr>(renameWidth);
        renameMap[tid] = nullptr;
        stalls[tid] = {false, false};
        finalCommitSeq[tid] = 0;
        releaseSeq[tid] = 0;
    }

    assert(decodeToRenameDelay == 1);

    renameStalls.resize(renameWidth, StallReason::NoStall);
}

std::string
Rename::name() const
{
    return cpu->name() + ".rename";
}

Rename::RenameStats::RenameStats(CPU *cpu, Rename *rename)
    : statistics::Group(cpu, "rename"),
      ADD_STAT(squashCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is squashing"),
      ADD_STAT(idleCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is idle"),
      ADD_STAT(blockCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is blocking"),
      ADD_STAT(serializeStallCycles, statistics::units::Cycle::get(),
               "count of cycles rename stalled for serializing inst"),
      ADD_STAT(runCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is running"),
      ADD_STAT(unblockCycles, statistics::units::Cycle::get(),
               "Number of cycles rename is unblocking"),
      ADD_STAT(renamedInsts, statistics::units::Count::get(),
               "Number of instructions processed by rename per thread"),
      ADD_STAT(squashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions processed by rename"),
      ADD_STAT(ROBFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to ROB full"),
      ADD_STAT(IQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to IQ full"),
      ADD_STAT(LQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to LQ full" ),
      ADD_STAT(SQFullEvents, statistics::units::Count::get(),
               "Number of times rename has blocked due to SQ full"),
      ADD_STAT(fullRegistersEvents, statistics::units::Count::get(),
               "Number of times there has been no free registers"),
      ADD_STAT(perThreadPregFullEvents, statistics::units::Count::get(),
               "Number of times a thread hit per-thread Preg quota while global free list has regs"),
      ADD_STAT(renamedOperands, statistics::units::Count::get(),
               "Number of destination operands rename has renamed"),
      ADD_STAT(lookups, statistics::units::Count::get(),
               "Number of register rename lookups that rename has made"),
      ADD_STAT(intLookups, statistics::units::Count::get(),
               "Number of integer rename lookups"),
      ADD_STAT(fpLookups, statistics::units::Count::get(),
               "Number of floating rename lookups"),
      ADD_STAT(vecLookups, statistics::units::Count::get(),
               "Number of vector rename lookups"),
      ADD_STAT(vecPredLookups, statistics::units::Count::get(),
               "Number of vector predicate rename lookups"),
      ADD_STAT(committedMaps, statistics::units::Count::get(),
               "Number of HB maps that are committed"),
      ADD_STAT(undoneMaps, statistics::units::Count::get(),
               "Number of HB maps that are undone due to squashing"),
      ADD_STAT(serializing, statistics::units::Count::get(),
               "count of serializing insts renamed"),
      ADD_STAT(tempSerializing, statistics::units::Count::get(),
               "count of temporary serializing insts renamed"),
      ADD_STAT(skidInsts, statistics::units::Count::get(),
               "count of insts added to the skid buffer"),
      ADD_STAT(moveEliminated, statistics::units::Count::get(),
               "count of insts eliminated by move elimination"),
      ADD_STAT(constantFolded, statistics::units::Count::get(),
               "count of insts eliminated by constant folding"),
      ADD_STAT(stallEvents, statistics::units::Count::get(),
               "count of stall events"),
      ADD_STAT(smtStallEvents, statistics::units::Count::get(),
               "Number of events the Rename has stalled per thread"),
      ADD_STAT(pregDonorCycles, statistics::units::Cycle::get(),
               "Per-thread cycles as a Preg borrowing donor"),
      ADD_STAT(pregBackendDonorCycles, statistics::units::Cycle::get(),
               "Per-thread cycles as a Preg donor due to ROB-full backpressure"),
      ADD_STAT(assignedRatSnapshot, statistics::units::Count::get(),
               "Number of RAT checkpoints taken"),
      ADD_STAT(committedRatSnapshot, statistics::units::Count::get(),
               "Number of RAT checkpoints released at commit"),
      ADD_STAT(squashedRatSnapshot, statistics::units::Count::get(),
               "Number of RAT checkpoints discarded on squash"),
      ADD_STAT(distanceRatSnapshot, statistics::units::Count::get(),
               "Instruction distance between successive RAT checkpoints")
{
    squashCycles.prereq(squashCycles);
    idleCycles.prereq(idleCycles);
    blockCycles.prereq(blockCycles);
    serializeStallCycles.flags(statistics::total);
    runCycles.prereq(idleCycles);
    unblockCycles.prereq(unblockCycles);

    squashedInsts.prereq(squashedInsts);

    ROBFullEvents.prereq(ROBFullEvents);
    IQFullEvents.prereq(IQFullEvents);
    LQFullEvents.prereq(LQFullEvents);
    SQFullEvents.prereq(SQFullEvents);

    renamedOperands.prereq(renamedOperands);
    lookups.prereq(lookups);
    intLookups.prereq(intLookups);
    fpLookups.prereq(fpLookups);
    vecLookups.prereq(vecLookups);
    vecPredLookups.prereq(vecPredLookups);

    committedMaps.prereq(committedMaps);
    undoneMaps.prereq(undoneMaps);
    serializing.flags(statistics::total);
    tempSerializing.flags(statistics::total);
    skidInsts.flags(statistics::total);
    moveEliminated.flags(statistics::total);
    constantFolded.flags(statistics::total);

    renamedInsts.init(cpu->numThreads).flags(statistics::total);
    fullRegistersEvents.init(cpu->numThreads).flags(statistics::total);
    perThreadPregFullEvents.init(cpu->numThreads).flags(statistics::total);
    pregDonorCycles.init(cpu->numThreads).flags(statistics::total);
    pregBackendDonorCycles.init(cpu->numThreads).flags(statistics::total);

    stallEvents.init(StallEventCount).flags(statistics::total);
    smtStallEvents
        .init(StallEventCount,0,cpu->numThreads-1,1)
        .flags(statistics::total);
    std::map < StallEvent, const char* > stall_event_str = {
        { ROBWalk, "ROBWalk"},
        { IEWStall, "IEWStall"},
        { ROBFull, "ROBFull"},
        { IQFull, "IQFull"},
        { LSQFull, "LSQFull"},
        { RegFull, "RegFull"},
        { SerializeInst, "SerializeInst"},
        { BWFull, "BWFull"},
    };

    for (int i = 0; i < StallEventCount; i++) {
        stallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
        smtStallEvents.subname(i, stall_event_str[static_cast<StallEvent>(i)]);
    }

    distanceRatSnapshot.init(10, 100, 5).flags(statistics::pdf);
}

void
Rename::regProbePoints()
{
    ppRename = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Rename");
    ppSquashInRename = new ProbePointArg<SeqNumRegPair>(cpu->getProbeManager(),
                                                        "SquashInRename");
}

void
Rename::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to read information from time buffer, from IEW stage.
    fromIEW = timeBuffer->getWire(-iewToRenameDelay);

    // Setup wire to read infromation from time buffer, from commit stage.
    fromCommit = timeBuffer->getWire(-commitToRenameDelay);

    // Setup wire to write information to previous stages.
    toDecode = timeBuffer->getWire(0);
}

void
Rename::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to write information to future stages.
    toIEW = renameQueue->getWire(0);
}

void
Rename::setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)
{
    decodeQueue = dq_ptr;

    // Setup wire to get information from decode.
    fromDecode = decodeQueue->getWire(-decodeToRenameDelay);
}

void
Rename::startupStage()
{
    resetStage();
}

void
Rename::clearStates(ThreadID tid)
{
    stalls[tid].iew = false;
}

void
Rename::resetStage()
{
    _status = Inactive;
    resumeUnblocking = false;

    // Grab the number of free entries directly from the stages.
    for (ThreadID tid = 0; tid < numThreads; tid++) {

        stalls[tid].iew = false;
        finalCommitSeq[tid] = 0;
        releaseSeq[tid] = 0;
        ratSnapshotBuffer[tid].clear();
    }

    numRatSnapshotInUse = 0;
    lastRatSnapshotDistance = 0;
}

void
Rename::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}


void
Rename::setRenameMap(UnifiedRenameMap rm_ptr[MaxThreads])
{
    for (ThreadID tid = 0; tid < numThreads; tid++)
        renameMap[tid] = &rm_ptr[tid];
}

void
Rename::setFreeList(UnifiedFreeList *fl_ptr)
{
    freeList = fl_ptr;
}

void
Rename::setScoreboard(Scoreboard *_scoreboard)
{
    scoreboard = _scoreboard;
}

bool
Rename::isDrained() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!historyBuffer[tid].empty() ||
            !fixedbuffer[tid].empty() ||
            !ratSnapshotBuffer[tid].empty())
            return false;
    }
    return true;
}

void
Rename::takeOverFrom()
{
    resetStage();
}

void
Rename::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        assert(historyBuffer[tid].empty());
        assert(fixedbuffer[tid].empty());
        assert(ratSnapshotBuffer[tid].empty());
    }
}

void
Rename::squash(const InstSeqNum &squash_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] [squash sn:%llu] Squashing instructions.\n",
        tid,squash_seq_num);

    fixedbuffer[tid].clear();

    doSquash(squash_seq_num, tid);
}

void
Rename::tick()
{
    toIEW->fetchStallReason = fromDecode->fetchStallReason;
    toIEW->decodeStallReason = fromDecode->decodeStallReason;

    wroteToTimeBuffer = false;
    blockReason = StallReason::NoStall;
    setAllStalls(StallReason::NoStall);

    moveInstsToBuffer();

    checkSquash();

    releasePhysRegs();

    // Establish this cycle's Preg SMT-DynamicBorrowing donor eligibility for
    // every thread up front, before any thread's canRename() consumes it via
    // UnifiedFreeList::borrowingLimit() -- avoids the result depending on
    // which thread happens to be processed first in the loop below.
    for (int i = 0; i < numThreads; i++) {
        bool backendQuotaFull = pregBackendBackpressureDonorEnabled &&
                                 hasBackendQuotaFullStall(i);
        if (backendQuotaFull) {
            pregBackendDonorCycles[i] = pregBackendBackpressureDonorHoldCycles;
        } else if (pregBackendDonorCycles[i] > 0) {
            --pregBackendDonorCycles[i];
        }
        bool backendBackpressureDonor = pregBackendDonorCycles[i] > 0;
        bool isDonor = !hasPregDemand(i) || backendBackpressureDonor;
        freeList->setBorrowingDonor(i, isDonor);
        if (isDonor)
            ++stats.pregDonorCycles[i];
        if (backendBackpressureDonor)
            ++stats.pregBackendDonorCycles[i];
    }

    // check threads stall & status
    ThreadID blocked_tid = InvalidThreadID;
    SmtActiveThreadArbiter active_arbiter;
    auto freezeActiveThread = [this](ThreadID tid) {
        stallSig->blockDecode[tid] = true;
        stallSig->decodeBlockReason[tid] = StallReason::OtherFragStall;
        toDecode->renameInfo[tid].blockReason =
            stallSig->decodeBlockReason[tid];
    };
    regFullThisCycle = false;
    for (int i = 0; i < numThreads; i++) {
        bool can_rename = canRename(i);
        bool block = stallSig->blockRename[i] || !can_rename;
        bool active = !block && !fixedbuffer[i].empty();
        StallReason block_reason = StallReason::NoStall;
        if (stallSig->blockRename[i]) {
            block_reason = stallSig->renameBlockReason[i];
        } else if (!can_rename) {
            block_reason = checkRenameStallFromIEW(i);
            if (block_reason == StallReason::NoStall) {
                block_reason = StallReason::RegFull;
                ++stats.fullRegistersEvents[i];
                regFullThisCycle = true;
                // Check if this is per-thread quota exhaustion
                // (global free list has regs but thread hit its limit)
                for (int rc = 0; rc <= RMiscRegClass; rc++) {
                    if (freeList->isPerThreadExhausted(
                            (RegClassType)rc, i, renameWidth)) {
                        ++stats.perThreadPregFullEvents[i];
                        break;
                    }
                }
            }
        }

        if (block_reason == StallReason::ROBFull) {
            stats.smtStallEvents[ROBFull].sample(i);
        } else if (block_reason == StallReason::RegFull) {
            stats.smtStallEvents[RegFull].sample(i);
        } else if (block_reason == StallReason::SerializeStall) {
            stats.smtStallEvents[SerializeInst].sample(i);
        } else if ( block_reason == StallReason::MemDQBandwidth ||
                    block_reason == StallReason::IntDQBandwidth ||
                    block_reason == StallReason::FVDQBandwidth) {
            stats.smtStallEvents[BWFull].sample(i);
        }

        DPRINTF(Rename, "[tid:%i] blockRename: %i, canRename: %i, block: %i, active: %i\n",
                i, stallSig->blockRename[i], can_rename, block, active);

        // if rename has no insts, no need to block decode, even if rename is blocked for other reasons
        stallSig->blockDecode[i] = block && !fixedbuffer[i].empty();
        stallSig->decodeBlockReason[i] =
            stallSig->blockDecode[i] ? block_reason : StallReason::NoStall;
        toDecode->renameInfo[i].blockReason = stallSig->decodeBlockReason[i];
        if (active) {
            const auto freeze = active_arbiter.observe(
                i, smtBorrowPriority(fromIEW->iewInfo[i]));
            if (freeze.previousActive != InvalidThreadID) {
                freezeActiveThread(freeze.previousActive);
            }
            if (freeze.freezeCurrent) {
                freezeActiveThread(i);
            }
        } else if (stallSig->blockDecode[i] && blocked_tid == InvalidThreadID) {
            blocked_tid = i;
        }
    }
    if (regFullThisCycle)
        stats.stallEvents[RegFull]++;
    const ThreadID tid = active_arbiter.selected();

    if (tid == InvalidThreadID) {
        // all threads are stalled, no need to process
        if (blocked_tid != InvalidThreadID) {
            setAllStalls(stallSig->decodeBlockReason[blocked_tid]);
            blockReason = stallSig->decodeBlockReason[blocked_tid];
        }
        toIEW->renameStallReason = renameStalls;
        updateActivate();
        return;
    }
    DPRINTF(Rename, "Processing [tid:%i]\n", tid);

    renameInsts(tid);
    if (stallSig->blockRename[tid]) {
        setAllStalls(stallSig->renameBlockReason[tid]);
        stats.smtStallEvents[stallSig->renameBlockReason[tid]].sample(tid);
    } else if (toIEW->size > 0 && renameStalls[0] == StallReason::NoStall) {
        for (int i = 0; i < renameStalls.size(); i++) {
            if (i < toIEW->size) {
                renameStalls.at(i) = StallReason::NoStall;
            } else {
                renameStalls.at(i) = fromDecode->decodeStallReason.at(i);
            }
        }
    }

    stallSig->decodeBlockReason[tid] =
        stallSig->blockDecode[tid] ? blockReason : StallReason::NoStall;
    toDecode->renameInfo[tid].blockReason = stallSig->decodeBlockReason[tid];

    toIEW->renameStallReason = renameStalls;

    updateActivate();

    bool release_pending = false;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        if (releaseSeq[tid] < finalCommitSeq[tid]) {
            release_pending = true;
            break;
        }
    }

    if (wroteToTimeBuffer || release_pending) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
Rename::releasePhysRegs()
{
    // Release physical registers up to releaseWidth
    auto threads = activeThreads->begin();
    while (threads != activeThreads->end()) {
        ThreadID tid = *threads++;

        if (releaseSeq[tid] + releaseWidth < finalCommitSeq[tid]) {
            releaseSeq[tid] += releaseWidth;
        } else {
            releaseSeq[tid] = finalCommitSeq[tid];
        }

        removeFromHistory(releaseSeq[tid], tid);
        if (ratSnapshotActive)
            commitSnapshot(finalCommitSeq[tid], tid);
        // doneSeqNum is also reused as a squash-progress marker while the
        // ROB is walking younger entries. Only real commit progress should
        // release physical registers.
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {

            finalCommitSeq[tid] = fromCommit->commitInfo[tid].doneSeqNum;
            releaseSeq[tid] =
                historyBuffer[tid].empty() ? 0 : historyBuffer[tid].back().instSeqNum;
        }
    }
}

bool
Rename::canRename(ThreadID tid)
{
    std::vector<int> demand_phy_regs(RMiscRegClass + 1, 0);
    auto& insts_to_rename = fixedbuffer[tid];
    int num_insts = insts_to_rename.size();

    // calculate physical registers needed by these `num_insts` instructions
    for (int i = 0; i < num_insts; i++) {
        DynInstPtr inst = insts_to_rename.at(i);
        if (inst->isSquashed()) {
            continue;
        }

        for (int j = 0; j < RMiscRegClass + 1 ; j++) {
            demand_phy_regs[j] += inst->numDestRegs((RegClassType)j);
        }
    }

    // if total demand registers are less than renameWidth,
    // then set it to renameWidth.
    // In actual hardware, due to timing constraints,
    // we can only evaluate whether rename can be down in the worst-case scenario.
    for (int i = 0; i < RMiscRegClass + 1; i++) {
        switch (i) {
            case IntRegClass:
            case FloatRegClass:
            case VecRegClass:
            case RMiscRegClass:
                demand_phy_regs[i] = std::max(demand_phy_regs[i], (int)renameWidth);
                break;
            default:
                break;
        }
    }

    // check if the demand registers can be satisfied or not
    for (int i = 0; i < RMiscRegClass + 1; i++) {
        if (demand_phy_regs[i] > renameMap[tid]->numFreeEntries((RegClassType)i)) {
            DPRINTF(Rename,
                    "[tid:%i] Cannot rename because demand for %s physical "
                    "registers is %i, but only %i are free.\n",
                    tid, RegId((RegClassType)i, 0).className(), demand_phy_regs[i],
                    renameMap[tid]->numFreeEntries((RegClassType)i));
            return false;
        }
    }
    return true;
}

bool
Rename::hasPregDemand(ThreadID tid) const
{
    for (const auto &inst : fixedbuffer[tid]) {
        if (inst->isSquashed()) {
            continue;
        }
        for (int j = 0; j < RMiscRegClass + 1; j++) {
            if (inst->numDestRegs((RegClassType)j) > 0) {
                return true;
            }
        }
    }
    return false;
}

bool
Rename::hasBackendQuotaFullStall(ThreadID tid)
{
    switch (checkRenameStallFromIEW(tid)) {
      case StallReason::ROBFull:
        return true;
      default:
        return false;
    }
}

void
Rename::renameInsts(ThreadID tid)
{
    // Instructions can be either in the skid buffer or the queue of
    // instructions coming from decode, depending on the status.
    auto& insts_to_rename = fixedbuffer[tid];
    int insts_available = insts_to_rename.size();

    int renamed_insts = 0;
    int toIEWIndex = 0;

    std::queue<StallReason> rename_stalls;

    StallReason breakRename = StallReason::NoStall;
    while (insts_available > 0) {

        assert(!insts_to_rename.empty());

        DynInstPtr inst = insts_to_rename.front();

        insts_to_rename.pop_front();

        if (inst->isSquashed()) {
            DPRINTF(Rename, "[sn:%llu] instruction  with PC %s is squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;

            // Decrement how many instructions are available.
            --insts_available;

            rename_stalls.push(StallReason::InstSquashed);

            continue;
        }

        assert(renameMap[tid]->canRename(inst));

        DPRINTF(Rename, "[tid:%i] [sn:%llu] Renaming instruction with PC %s.\n",
            tid, inst->seqNum, inst->pcState());

        renameSrcRegs(inst, inst->threadNumber);

        renameDestRegs(inst, inst->threadNumber);

        if (ratSnapshotActive) {
            if (ratSnapshotAvailable() && suitableForRatSnapshot(inst)) {
                takeSnapshot(inst, tid);
            } else {
                ++lastRatSnapshotDistance;
            }
        }

        cpu->perfCCT->updateInstPos(inst->seqNum, PerfRecord::AtRename);

        ++renamed_insts;
        // Notify potential listeners that source and destination registers for
        // this instruction have been renamed.
        ppRename->notify(inst);

        // Put instruction in rename queue.
        toIEW->insts[toIEWIndex] = inst;
        ++(toIEW->size);

        // Increment which instruction we're on.
        ++toIEWIndex;

        // Decrement how many instructions are available.
        --insts_available;
    }

    // Check if there's any instructions left that haven't yet been renamed.
    // If so then block.
    if (!fixedbuffer[tid].empty()) {
        stallSig->blockDecode[tid] = true;
        if (breakRename == StallReason::NoStall) {
            breakRename = checkRenameStallFromIEW(tid);
            if (breakRename == StallReason::NoStall) {
                breakRename = StallReason::RegFull;
                ++stats.fullRegistersEvents[tid];
                if (!regFullThisCycle) {
                    stats.stallEvents[RegFull]++;
                    regFullThisCycle = true;
                }
                for (int rc = 0; rc <= RMiscRegClass; rc++) {
                    if (freeList->isPerThreadExhausted(
                            (RegClassType)rc, tid, renameWidth)) {
                        ++stats.perThreadPregFullEvents[tid];
                        break;
                    }
                }
            }
        }
        blockReason = breakRename;
        DPRINTF(Rename, "[tid:%i] Stalling because there are still instructions to "
                "rename.\n", tid);
    }

    if (!rename_stalls.empty()) {
        setAllStalls(rename_stalls.front());
        rename_stalls.pop();
    } else if (breakRename != StallReason::NoStall) {
        setAllStalls(breakRename);
    }

    stats.renamedInsts[tid] += renamed_insts;

    if (breakRename == StallReason::ROBFull) {
        stats.smtStallEvents[ROBFull].sample(tid);
    } else if (breakRename == StallReason::RegFull) {
        stats.smtStallEvents[RegFull].sample(tid);
    } else if (breakRename == StallReason::SerializeStall) {
        stats.smtStallEvents[SerializeInst].sample(tid);
    } else if ( breakRename == StallReason::MemDQBandwidth ||
                breakRename == StallReason::IntDQBandwidth ||
                breakRename == StallReason::FVDQBandwidth) {
        stats.smtStallEvents[BWFull].sample(tid);
    }

    // If we wrote to the time buffer, record this.
    if (toIEWIndex) {
        wroteToTimeBuffer = true;
    }
}

void
Rename::moveInstsToBuffer()
{
    int insts_from_decode = fromDecode->size;
    if (insts_from_decode == 0) {
        return;
    }
    ThreadID tid = fromDecode->insts[0]->threadNumber;
    for (int i = 0; i < insts_from_decode; ++i) {
        const DynInstPtr &inst = fromDecode->insts[i];
        assert(inst->threadNumber == tid);
        if (localSquashVer[tid].largerThan(inst->getVersion())) {
            inst->setSquashed();
        } else {
            assert(!fixedbuffer[tid].full());
            fixedbuffer[tid].push_back(inst);
        }

#if TRACING_ON
        if (debug::O3PipeView) {
            inst->renameTick = curTick() - inst->fetchTick;
        }
#endif
    }
}

void
Rename::checkSquash()
{
    for (int i = 0; i < numThreads; i++) {
        if (fromCommit->commitInfo[i].squash) {
            DPRINTF(Rename, "[tid:%i] Squashing instructions due to squash from "
                    "commit.\n", i);

            squash(fromCommit->commitInfo[i].doneSeqNum, i);

            localSquashVer[i].update(
                fromCommit->commitInfo[i].squashVersion.getVersion());
            DPRINTF(Rename, "Updating squash version to %u\n",
                    localSquashVer[i].getVersion());
        }
    }
}

void
Rename::updateActivate()
{
    bool any_unblocking = true;

    // Rename will have activity if it's unblocking.
    if (any_unblocking) {
        if (_status == Inactive) {
            _status = Active;

            DPRINTF(Activity, "Activating stage.\n");

            cpu->activateStage(CPU::RenameIdx);
        }
    } else {
        // If it's not unblocking, then rename will not have any internal
        // activity.  Switch it to inactive.
        if (_status == Active) {
            _status = Inactive;
            DPRINTF(Activity, "Deactivating stage.\n");

            cpu->deactivateStage(CPU::RenameIdx);
        }
    }
}

void
Rename::tryFreePReg(PhysRegIdPtr preg, ThreadID tid)
{
    const auto preg_idx = preg->flatIndex();
    if (preg->getRef() == 0 || preg->classValue() == InvalidRegClass) {
        return;
    }

    preg->decRef();
    if (preg->getRef() == 0) {
        // Put the renamed physical register back on the free list.
        DPRINTF(Rename, "Really free up p%i on squash with ref=%i\n", preg_idx,
                preg->getRef());
        freeList->addReg(preg, tid);
    } else {
        DPRINTF(Rename, "Not to free up p%i on squash for ref=%i\n",
                preg->flatIndex(), preg->getRef());
    }
}

void
Rename::doSquash(const InstSeqNum &squashed_seq_num, ThreadID tid)
{
    if (ratSnapshotActive)
        squashSnapshot(squashed_seq_num, tid);

    auto hb_it = historyBuffer[tid].begin();

    // After a syscall squashes everything, the history buffer may be empty
    // but the ROB may still be squashing instructions.
    // Go through the most recent instructions, undoing the mappings
    // they did and freeing up the registers.
    while (!historyBuffer[tid].empty() &&
           hb_it->instSeqNum > squashed_seq_num) {
        assert(hb_it != historyBuffer[tid].end());

        DPRINTF(Rename,
                "[tid:%i] Removing history entry with sequence "
                "number %i (archReg: %d, newPhysReg: %s, prevPhysReg: %s).\n",
                tid, hb_it->instSeqNum, hb_it->archReg.index(),
                hb_it->newPhysReg.toString(),
                hb_it->prevPhysReg.toString());

        // Undo the rename mapping only if it was really a change.
        // Special regs that are not really renamed (like misc regs
        // and the zero reg) can be recognized because the new mapping
        // is the same as the old one.  While it would be merely a
        // waste of time to update the rename table, we definitely
        // don't want to put these on the free list.
        if (hb_it->newPhysReg != hb_it->prevPhysReg) {
            // Tell the rename map to set the architected register to the
            // previous physical register that it was renamed to.
            renameMap[tid]->setEntry(hb_it->archReg, hb_it->prevPhysReg);
            if (hb_it->newPhysReg.PhyReg() != hb_it->prevPhysReg.PhyReg()) {
                tryFreePReg(hb_it->newPhysReg.PhyReg(), tid);
            }
        }

        // Notify potential listeners that the register mapping needs to be
        // removed because the instruction it was mapped to got squashed. Note
        // that this is done before hb_it is incremented.
        ppSquashInRename->notify(std::make_pair(hb_it->instSeqNum,
                                                hb_it->newPhysReg.PhyReg()));

        historyBuffer[tid].erase(hb_it++);

        ++stats.undoneMaps;
    }
}

int
Rename::countRatSnapshots()
{
    int total = 0;
    for (ThreadID tid = 0; tid < numThreads; tid++)
        total += countRatSnapshots(tid);
    return total;
}

size_t
Rename::countRatSnapshots(ThreadID tid)
{
    return ratSnapshotBuffer[tid].size();
}

void
Rename::takeSnapshot(const DynInstPtr &inst, ThreadID tid)
{
    inst->setRatSnapshotted();
    numRatSnapshotInUse++;
    ratSnapshotBuffer[tid].push_front(inst->seqNum);
    assert(numRatSnapshotInUse <= numMaxRatSnapshot);

    stats.assignedRatSnapshot++;
    stats.distanceRatSnapshot.sample(lastRatSnapshotDistance);
    lastRatSnapshotDistance = 0;

    DPRINTF(Rename, "[tid:%i] Took RAT checkpoint at [sn:%llu], inUse %d\n",
            tid, inst->seqNum, numRatSnapshotInUse);
}

void
Rename::commitSnapshot(InstSeqNum commit_seq_num, ThreadID tid)
{
    // Oldest checkpoints sit at the back; release those the commit head passed.
    while (!ratSnapshotBuffer[tid].empty() &&
           ratSnapshotBuffer[tid].back() <= commit_seq_num) {
        ratSnapshotBuffer[tid].pop_back();
        numRatSnapshotInUse--;
        stats.committedRatSnapshot++;
    }
    assert(numRatSnapshotInUse >= 0);
}

void
Rename::squashSnapshot(InstSeqNum squash_seq_num, ThreadID tid)
{
    // Newest checkpoints sit at the front; release those on the discarded
    // (younger) path. The checkpoint at the redirect itself survives and is
    // released later at commit.
    while (!ratSnapshotBuffer[tid].empty() &&
           ratSnapshotBuffer[tid].front() > squash_seq_num) {
        ratSnapshotBuffer[tid].pop_front();
        numRatSnapshotInUse--;
        stats.squashedRatSnapshot++;
    }
    assert(numRatSnapshotInUse >= 0);
}

bool
Rename::suitableForRatSnapshot(const DynInstPtr &inst)
{
    return inst->isControl() && lastRatSnapshotDistance >= ratSnapshotDistance;
}

void
Rename::removeFromHistory(InstSeqNum inst_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Removing a committed instruction from the "
            "history buffer %u (size=%i), until [sn:%llu].\n",
            tid, tid, historyBuffer[tid].size(), inst_seq_num);

    auto hb_it = historyBuffer[tid].end();

    --hb_it;

    if (historyBuffer[tid].empty()) {
        DPRINTF(Rename, "[tid:%i] History buffer is empty.\n", tid);
        return;
    } else if (hb_it->instSeqNum > inst_seq_num) {
        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Old sequence number encountered. "
                "Ensure that a syscall happened recently.\n",
                tid,inst_seq_num);
        return;
    }

    // Commit all the renames up until (and including) the committed sequence
    // number. Some or even all of the committed instructions may not have
    // rename histories if they did not have destination registers that were
    // renamed.
    while (!historyBuffer[tid].empty() &&
           hb_it != historyBuffer[tid].end() &&
           hb_it->instSeqNum <= inst_seq_num) {

        DPRINTF(Rename,
                "[tid:%i] try to free up older rename of reg %s (%s), "
                "[sn:%llu].\n",
                tid, hb_it->prevPhysReg.toString(),
                hb_it->prevPhysReg.PhyReg()->className(),
                hb_it->instSeqNum);


        // Don't free special phys regs like misc and zero regs, which
        // can be recognized because the new mapping is the same as
        // the old one.
        if (hb_it->newPhysReg.PhyReg() != hb_it->prevPhysReg.PhyReg()) {
            tryFreePReg(hb_it->prevPhysReg.PhyReg(), tid);
        }

        ++stats.committedMaps;

        historyBuffer[tid].erase(hb_it--);
    }
}

void
Rename::renameSrcRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_src_regs = inst->numSrcRegs();

    // Get the architectual register numbers from the source and
    // operands, and redirect them to the right physical register.
    for (int src_idx = 0; src_idx < num_src_regs; src_idx++) {
        const RegId& src_reg = inst->srcRegIdx(src_idx);
        VirtRegId renamed_reg;

        renamed_reg = map->lookup(tc->flattenRegId(src_reg));
        switch (src_reg.classValue()) {
          case InvalidRegClass:
            break;
          case IntRegClass:
            stats.intLookups++;
            break;
          case FloatRegClass:
            stats.fpLookups++;
            break;
          case VecRegClass:
          case VecElemClass:
            stats.vecLookups++;
            break;
          case VecPredRegClass:
            stats.vecPredLookups++;
            break;
          case CCRegClass:
          case RMiscRegClass:
          case MiscRegClass:
            break;

          default:
            panic("Invalid register class: %d.", src_reg.classValue());
        }

        DPRINTF(Rename,
                "[tid:%i] "
                "Looking up %s arch reg x%i, got %s\n",
                tid, src_reg.className(), src_reg.index(),
                renamed_reg.toString());

        inst->renameSrcReg(src_idx, renamed_reg);

        // See if the register is ready or not.
        if (scoreboard->getReg(renamed_reg.PhyReg())) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is ready.\n",
                    tid, renamed_reg.PhyReg()->index(), renamed_reg.PhyReg()->flatIndex(),
                    renamed_reg.PhyReg()->className());

            inst->markSrcRegReady(src_idx);
        } else {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is not ready.\n",
                    tid, renamed_reg.PhyReg()->index(), renamed_reg.PhyReg()->flatIndex(),
                    renamed_reg.PhyReg()->className());
        }

        ++stats.lookups;
    }
}

void
Rename::renameDestRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];
    unsigned num_dest_regs = inst->numDestRegs();

    // Rename the destination registers.
    for (int dest_idx = 0; dest_idx < num_dest_regs; dest_idx++) {
        const RegId& dest_reg = inst->destRegIdx(dest_idx);
        UnifiedRenameMap::RenameInfo rename_result;

        RegId flat_dest_regid = tc->flattenRegId(dest_reg);
        flat_dest_regid.setNumPinnedWrites(dest_reg.getNumPinnedWrites());

        VirtRegId bypass_reg;
        bool inc_ref_of_last_dest_phy_reg = false;
        if (cpu->enableMoveElimination && inst->isMov()) {
            // Move elimination
            bypass_reg =
                map->lookup(tc->flattenRegId(inst->srcRegIdx(0)));
            DPRINTF(Rename, "Find the last reg p%i renamed for mv x%i, x%i\n",
                    bypass_reg.PhyReg()->flatIndex(), dest_reg.index(),
                    inst->srcRegIdx(0).index());
            inc_ref_of_last_dest_phy_reg = true;
            inst->setEmptyMov();
            DPRINTF(Rename, "[sn:%llu] Inst is nop: %i, is move: %i\n", inst->seqNum, inst->isNop(),
                    inst->isMov());
            stats.moveEliminated++;
        } else if (inst->isAddImm() &&
            (cpu->enableConstantFolding || (cpu->enableMovImmElimination && inst->srcRegIdx(0).isZeroReg()))) {
            // Constant folding
            bypass_reg =
                map->lookup(tc->flattenRegId(inst->srcRegIdx(0)));

            if (!bypass_reg.IEOper() || bypass_reg.IEOper()->type == IEOperand::Type::ADD) {
                if (bypass_reg.IEOper()) {
                    IEOperPtr ie_op = new IEOperand(IEOperand::Type::ADD,
                        bypass_reg.IEOper()->imm + inst->staticInst->getImm());
                    bypass_reg.setIEOper(ie_op);
                } else {
                    IEOperPtr ie_op = new IEOperand(IEOperand::Type::ADD, inst->staticInst->getImm());
                    bypass_reg.setIEOper(ie_op);
                }
                inc_ref_of_last_dest_phy_reg = true;

                inst->setConstantFolded();
                DPRINTF(Rename, "[sn:%llu] Inst constant folded, virtRegId: %s\n", inst->seqNum, bypass_reg.toString());
                stats.constantFolded++;
            }
        }

        rename_result = map->rename(flat_dest_regid, bypass_reg);

        inst->flattenedDestIdx(dest_idx, flat_dest_regid);

        if (!inc_ref_of_last_dest_phy_reg) {
            scoreboard->unsetReg(rename_result.first.PhyReg());
        }

        DPRINTF(Rename, "[tid:%i] %s arch reg x%i (%s) from %s to %s.\n", tid,
                inc_ref_of_last_dest_phy_reg ? "Mov" : "Rename",
                dest_reg.index(), dest_reg.className(),
                rename_result.second.toString(),
                rename_result.first.toString());

        // Record the rename information so that a history can be kept.
        RenameHistory hb_entry(inst->seqNum, flat_dest_regid,
                               rename_result.first,
                               rename_result.second);

        historyBuffer[tid].push_front(hb_entry);

        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Adding instruction to history buffer (size=%i).\n",
                tid,(*historyBuffer[tid].begin()).instSeqNum,
                historyBuffer[tid].size());

        // Tell the instruction to rename the appropriate destination
        // register (dest_idx) to the new physical register
        // (rename_result.first), and record the previous physical
        // register that the same logical register was renamed to
        // (rename_result.second).
        inst->renameDestReg(dest_idx,
                            rename_result.first,
                            rename_result.second);

        ++stats.renamedOperands;

        if (valuePred) {
            if (num_dest_regs != 1 || !inst->canLVP()) {
                inst->vpSupported = false;
                inst->vpResult.speculative = false;
                inst->vpResult.value = 0xdeadbeefULL;
                inst->vpApplied = false;
                continue;
            }

            inst->vpSupported = true;
            if (inst->vpResult.speculative) {
                inst->vpApplied = true;
                if (!enableSelectiveVPFlush) {
                    // old behavior: let back-to-back rename consumers see ready
                    scoreboard->setReg(rename_result.first.PhyReg());
                }
                inst->setRegOperand(inst->staticInst.get(), 0, inst->vpResult.value);
                // must pop result here
                inst->popResult();
                DPRINTF(Rename,
                        "Rename-Stage instruction[%s] generate "
                        "prediction value."
                        "seq num: %lu pc: %lX "
                        "prediction value: %lu \n",
                        inst->staticInst->disassemble(inst->getPC()),
                        inst->seqNum, inst->getPC(), inst->vpResult.value);
            } else {
                inst->vpApplied = false;
            }
        }
    }
}

void
Rename::incrFullStat(const FullSource &source)
{
    switch (source) {
      case ROB:
        ++stats.ROBFullEvents;
        stats.stallEvents[ROBFull]++;
        break;
      case IQ:
        ++stats.IQFullEvents;
        stats.stallEvents[IQFull]++;
        break;
      case LQ:
        ++stats.LQFullEvents;
        stats.stallEvents[LSQFull]++;
        break;
      case SQ:
        ++stats.SQFullEvents;
        stats.stallEvents[LSQFull]++;
        break;
      default:
        panic("Rename full stall stat should be incremented for a reason!");
        break;
    }
}

void
Rename::dumpHistory()
{
    std::list<RenameHistory>::iterator buf_it;

    for (ThreadID tid = 0; tid < numThreads; tid++) {

        buf_it = historyBuffer[tid].begin();

        while (buf_it != historyBuffer[tid].end()) {
            cprintf("Seq num: %i\nArch reg[%s]: %i New phys reg:"
                    " %i[%s] Old phys reg: %i[%s]\n",
                    (*buf_it).instSeqNum,
                    (*buf_it).archReg.className(),
                    (*buf_it).archReg.index(),
                    (*buf_it).newPhysReg.PhyReg()->index(),
                    (*buf_it).newPhysReg.PhyReg()->className(),
                    (*buf_it).prevPhysReg.PhyReg()->index(),
                    (*buf_it).prevPhysReg.PhyReg()->className());

            buf_it++;
        }
    }
}

void
Rename::setAllStalls(StallReason renameStall)
{
    for (int i = 0;i < renameStalls.size();i++) {
        renameStalls.at(i) = renameStall;
    }
}

StallReason
Rename::checkRenameStallFromIEW(ThreadID tid)
{
    StallReason robHeadStallReason = fromIEW->iewInfo[tid].robHeadStallReason;
    if (robHeadStallReason != StallReason::NoStall) {
        return robHeadStallReason;
    }

    StallReason lqHeadStallReason = fromIEW->iewInfo[tid].lqHeadStallReason;
    if (lqHeadStallReason != StallReason::NoStall) {
        return lqHeadStallReason;
    }

    StallReason sqHeadStallReason = fromIEW->iewInfo[tid].sqHeadStallReason;
    if (sqHeadStallReason != StallReason::NoStall) {
        return sqHeadStallReason;
    }

    return StallReason::NoStall;
}

} // namespace o3
} // namespace gem5
