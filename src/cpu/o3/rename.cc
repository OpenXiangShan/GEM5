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
#include "debug/Counters.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

Rename::Rename(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      iewToRenameDelay(params.iewToRenameDelay),
      decodeToRenameDelay(params.decodeToRenameDelay),
      commitToRenameDelay(params.commitToRenameDelay),
      renameWidth(params.renameWidth),
      numThreads(params.numThreads),
      stats(_cpu)
{
    if (renameWidth > MaxWidth)
        fatal("renameWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             renameWidth, static_cast<int>(MaxWidth));

    // @todo: Make into a parameter.
    skidBufferMax = (decodeToRenameDelay + 1) * params.decodeWidth * 2;
    for (uint32_t tid = 0; tid < MaxThreads; tid++) {
        renameStatus[tid] = Idle;
        renameMap[tid] = nullptr;
        instsInProgress[tid] = 0;
        loadsInProgress[tid] = 0;
        storesInProgress[tid] = 0;
        freeEntries[tid] = {0, 0, 0};
        emptyROB[tid] = true;
        stalls[tid] = {false, false};
        serializeInst[tid] = nullptr;
        serializeOnNextInst[tid] = false;
    }

    renameStalls.resize(renameWidth, StallReason::NoStall);
}

std::string
Rename::name() const
{
    return cpu->name() + ".rename";
}

Rename::RenameStats::RenameStats(statistics::Group *parent)
    : statistics::Group(parent, "rename"),
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
               "Number of instructions processed by rename"),
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
      ADD_STAT(stallEvents, statistics::units::Count::get(),
               "count of stall events")
{
    squashCycles.prereq(squashCycles);
    idleCycles.prereq(idleCycles);
    blockCycles.prereq(blockCycles);
    serializeStallCycles.flags(statistics::total);
    runCycles.prereq(idleCycles);
    unblockCycles.prereq(unblockCycles);

    renamedInsts.prereq(renamedInsts);
    squashedInsts.prereq(squashedInsts);

    ROBFullEvents.prereq(ROBFullEvents);
    IQFullEvents.prereq(IQFullEvents);
    LQFullEvents.prereq(LQFullEvents);
    SQFullEvents.prereq(SQFullEvents);
    fullRegistersEvents.prereq(fullRegistersEvents);

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

    stallEvents.init(StallEventCount).flags(statistics::total);
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
    }
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
    renameStatus[tid] = Idle;

    freeEntries[tid].lqEntries = iew_ptr->ldstQueue.numFreeLoadEntries(tid);
    freeEntries[tid].sqEntries = iew_ptr->ldstQueue.numFreeStoreEntries(tid);
    freeEntries[tid].robEntries = commit_ptr->numROBFreeEntries(tid);
    emptyROB[tid] = true;

    stalls[tid].iew = false;
    serializeInst[tid] = NULL;

    instsInProgress[tid] = 0;
    loadsInProgress[tid] = 0;
    storesInProgress[tid] = 0;

    serializeOnNextInst[tid] = false;
}

void
Rename::resetStage()
{
    _status = Inactive;

    resumeSerialize = false;
    resumeUnblocking = false;

    // Grab the number of free entries directly from the stages.
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        renameStatus[tid] = Idle;
        freeEntries[tid].lqEntries =
            iew_ptr->ldstQueue.numFreeLoadEntries(tid);
        freeEntries[tid].sqEntries =
            iew_ptr->ldstQueue.numFreeStoreEntries(tid);
        freeEntries[tid].robEntries = commit_ptr->numROBFreeEntries(tid);
        emptyROB[tid] = true;

        stalls[tid].iew = false;
        serializeInst[tid] = NULL;

        instsInProgress[tid] = 0;
        loadsInProgress[tid] = 0;
        storesInProgress[tid] = 0;

        serializeOnNextInst[tid] = false;
    }
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
        if (instsInProgress[tid] != 0 ||
            !historyBuffer[tid].empty() ||
            !skidBuffer[tid].empty() ||
            !insts[tid].empty() ||
            (renameStatus[tid] != Idle && renameStatus[tid] != Running))
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
        assert(insts[tid].empty());
        assert(skidBuffer[tid].empty());
        assert(instsInProgress[tid] == 0);
    }
}

void
Rename::squash(const InstSeqNum &squash_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] [squash sn:%llu] Squashing instructions.\n",
        tid,squash_seq_num);

    // Clear the stall signal if rename was blocked or unblocking before.
    // If it still needs to block, the blocking should happen the next
    // cycle and there should be space to hold everything due to the squash.
    if (renameStatus[tid] == Blocked ||
        renameStatus[tid] == Unblocking) {
        toDecode->renameUnblock[tid] = 1;

        resumeSerialize = false;
        serializeInst[tid] = NULL;
    } else if (renameStatus[tid] == SerializeStall) {
        if (serializeInst[tid]->seqNum <= squash_seq_num) {
            DPRINTF(Rename, "[tid:%i] [squash sn:%llu] "
                "Rename will resume serializing after squash\n",
                tid,squash_seq_num);
            resumeSerialize = true;
            assert(serializeInst[tid]);
        } else {
            resumeSerialize = false;
            toDecode->renameUnblock[tid] = 1;

            serializeInst[tid] = NULL;
        }
    }

    // Set the status to Squashing.
    renameStatus[tid] = Squashing;

    // Squash any instructions from decode.
    for (int i=0; i<fromDecode->size; i++) {
        if (fromDecode->insts[i]->threadNumber == tid &&
            fromDecode->insts[i]->seqNum > squash_seq_num) {
            fromDecode->insts[i]->setSquashed();
            wroteToTimeBuffer = true;
        }

    }

    // Clear the instruction list and skid buffer in case they have any
    // insts in them.
    insts[tid].clear();

    // Clear the skid buffer in case it has any data in it.
    skidBuffer[tid].clear();

    doSquash(squash_seq_num, tid);
}

void
Rename::tick()
{
    toIEW->fetchStallReason = fromDecode->fetchStallReason;
    toIEW->decodeStallReason = fromDecode->decodeStallReason;   // 将decode stall原因直接传递给IEW

    wroteToTimeBuffer = false;

    blockThisCycle = false;

    bool status_change = false;

    toIEWIndex = 0;

    sortInsts();    // fromDecode->insts 排序放入InstQueue insts 中

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    // Check stall and squash signals.
    while (threads != end) {
        ThreadID tid = *threads++;

        DPRINTF(Rename, "Processing [tid:%i]\n", tid);

        status_change = checkSignalsAndUpdate(tid) || status_change;

        rename(status_change, tid);  // 每个线程重命名

        toDecode->renameInfo[tid].blockReason = blockReason;
    }

    toIEW->renameStallReason = renameStalls;

    if (status_change) {
        updateStatus();
    }

    if (wroteToTimeBuffer) {  // decode是否写入时间缓冲区, 监控CPU 活跃状态
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }

    threads = activeThreads->begin();

    while (threads != end) {
        ThreadID tid = *threads++;

        // If we committed this cycle then doneSeqNum will be > 0
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            renameStatus[tid] != Squashing) {

            removeFromHistory(fromCommit->commitInfo[tid].doneSeqNum,
                                  tid);  // 从history中删除已提交的inst
        }
    }

    // @todo: make into updateProgress function
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        instsInProgress[tid] -= fromIEW->iewInfo[tid].dispatched;  // 从IEW来的inst， 指令计数--
        loadsInProgress[tid] -= fromIEW->iewInfo[tid].dispatchedToLQ;
        storesInProgress[tid] -= fromIEW->iewInfo[tid].dispatchedToSQ;
        assert(loadsInProgress[tid] >= 0);
        assert(storesInProgress[tid] >= 0);
        assert(instsInProgress[tid] >=0);
    }

}

void
Rename::rename(bool &status_change, ThreadID tid)
{
    // If status is Running or idle,
    //     call renameInsts()
    // If status is Unblocking,
    //     buffer any instructions coming from decode
    //     continue trying to empty skid buffer
    //     check if stall conditions have passed

    if (renameStatus[tid] == Blocked) {
        ++stats.blockCycles;
        setAllStalls(blockReason);
    } else if (renameStatus[tid] == Squashing) {
        ++stats.squashCycles;
        setAllStalls(StallReason::SquashStall);
    } else if (renameStatus[tid] == SerializeStall) {
        ++stats.serializeStallCycles;
        setAllStalls(StallReason::SerializeStall);
        // If we are currently in SerializeStall and resumeSerialize
        // was set, then that means that we are resuming serializing
        // this cycle.  Tell the previous stages to block.
        if (resumeSerialize) {
            resumeSerialize = false;
            blockReason = StallReason::SerializeStall;
            block(tid);
            toDecode->renameUnblock[tid] = false;
        }
    } else if (renameStatus[tid] == Unblocking) {
        if (resumeUnblocking) {
            blockReason = StallReason::ResumeUnblock;
            block(tid);
            resumeUnblocking = false;
            toDecode->renameUnblock[tid] = false;
        }
    }
    // 如果状态是running or idle， 则调用renameInsts()
    if (renameStatus[tid] == Running ||
        renameStatus[tid] == Idle) {
        DPRINTF(Rename,
                "[tid:%i] "
                "Not blocked, so attempting to run stage.\n",
                tid);

        renameInsts(tid);
    } else if (renameStatus[tid] == Unblocking) {
        renameInsts(tid);

        if (validInsts()) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);  // 将insts[tid] 中的inst 放入skidBuffer[tid] 中，能放就放
        }

        // If we switched over to blocking, then there's a potential for
        // an overall status change.
        status_change = unblock(tid) || status_change || blockThisCycle;
    }
}

void
Rename::renameInsts(ThreadID tid)
{
    // Instructions can be either in the skid buffer or the queue of
    // instructions coming from decode, depending on the status.  指令可能在skid buffer 或 从decode 来的insts[tid] 中， 取决于状态
    int insts_available = renameStatus[tid] == Unblocking ?
        skidBuffer[tid].size() : insts[tid].size();  // 如果状态是unblocking， 则insts_available 为 skidBuffer[tid].size()， 否则为 insts[tid].size()

    int instsAvailable = insts_available;

    // Check the decode queue to see if instructions are available.
    // If there are no available instructions to rename, then do nothing.
    if (insts_available == 0) {
        DPRINTF(Rename, "[tid:%i] Nothing to do, breaking out early.\n",
                tid);
        // Should I change status to idle?
        ++stats.idleCycles;

        StallReason stall = StallReason::NoStall;
        for (auto iter : fromDecode->decodeStallReason) {
            if (iter != StallReason::NoStall) {
                stall = iter;
                break;
            }
        }
        setAllStalls(stall);    // 根据decode stall原因，设置rename stall原因

        return;
    } else if (renameStatus[tid] == Unblocking) {
        ++stats.unblockCycles;
    } else if (renameStatus[tid] == Running) {
        ++stats.runCycles;
    }

    // Will have to do a different calculation for the number of free
    // entries.
    int free_rob_entries = calcFreeROBEntries(tid);  // 计算ROB空闲数
    int free_iq_entries  = calcFreeIQEntries(tid);  // 计算IQ空闲数
    int min_free_entries = free_rob_entries;  // 最小空闲数

    DPRINTF(Rename, "free_rob_entries=%d, free_iq_entries=%d, insts_available=%d\n",
            free_rob_entries, free_iq_entries, insts_available);  // 打印空闲数

    FullSource source = ROB;

    if (free_iq_entries < min_free_entries) {
        min_free_entries = free_iq_entries;
        source = IQ;
    }

    // Check if there's any space left.
    if (min_free_entries <= 0) {
        DPRINTF(Rename,
                "[tid:%i] Blocking due to no free ROB/IQ/ entries.\n"
                "ROB has %i free entries.\n"
                "IQ has %i free entries.\n",
                tid, free_rob_entries, free_iq_entries);

        blockThisCycle = true;

        block(tid);

        incrFullStat(source);

        setAllStalls(checkRenameStallFromIEW(tid));

        return;
    } else if (min_free_entries < insts_available) {    // 如果最小空闲数小于insts_available， 则阻塞
        DPRINTF(Rename,
                "[tid:%i] "
                "Will have to block this cycle. "
                "%i insts available, "
                "but only %i insts can be renamed due to ROB/IQ/LSQ limits.\n",
                tid, insts_available, min_free_entries);

        insts_available = min_free_entries;

        blockThisCycle = true;

        blockReason = checkRenameStallFromIEW(tid);

        incrFullStat(source);
    }

    InstQueue &insts_to_rename = renameStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    DPRINTF(Rename,
            "[tid:%i] "
            "%i available instructions to send iew.\n",
            tid, insts_available);

    DPRINTF(Rename,
            "[tid:%i] "
            "%i insts pipelining from Rename | "
            "%i insts dispatched to IQ last cycle.\n",
            tid, instsInProgress[tid], fromIEW->iewInfo[tid].dispatched);

    // Handle serializing the next instruction if necessary.
    if (serializeOnNextInst[tid]) {
        if (emptyROB[tid] && instsInProgress[tid] == 0) {
            // ROB already empty; no need to serialize.
            serializeOnNextInst[tid] = false;
        } else if (!insts_to_rename.empty()) {
            insts_to_rename.front()->setSerializeBefore();
        }
    }

    int renamed_insts = 0;

    int rename_width = renameWidth;
    int count_ = 0;
    for (auto &it : insts_to_rename) {
        count_++;
        if (it->opClass() == FMAAccOp) {
            rename_width++;
        }
        if (count_ >= renameWidth ||
            rename_width >= renameWidth * 2) {
            break;
        }
    }

    std::queue<StallReason> rename_stalls;

    StallReason breakRename = StallReason::NoStall;

    while (insts_available > 0 &&  toIEWIndex < rename_width) { // 准备发送insts_to_rename 中的inst 到IEW, 遍历
        DPRINTF(Rename, "[tid:%i] Sending instructions to IEW.\n", tid);

        assert(!insts_to_rename.empty());

        DynInstPtr inst = insts_to_rename.front();

        //For all kind of instructions, check ROB and IQ first For load
        //instruction, check LQ size and take into account the inflight loads
        //For store instruction, check SQ size and take into account the
        //inflight stores
        // 对于所有类型的指令，首先检查 ROB 和 IQ
        // 对于加载指令，检查 LQ 大小并考虑正在进行中的加载
        // 对于存储指令，检查 SQ 大小并考虑正在进行中的存储

        if (inst->isLoad()) {
            if (calcFreeLQEntries(tid) <= 0) {
                DPRINTF(Rename, "[tid:%i] Cannot rename due to no free LQ\n",
                        tid);
                source = LQ;
                incrFullStat(source);
                rename_stalls.push(checkRenameStallFromIEW(tid));
                breakRename = checkRenameStallFromIEW(tid);
                break;
            }
        }

        if (inst->isStore() || inst->isAtomic()) {
            if (calcFreeSQEntries(tid) <= 0) {
                DPRINTF(Rename, "[tid:%i] Cannot rename due to no free SQ\n",
                        tid);
                source = SQ;
                incrFullStat(source);
                rename_stalls.push(checkRenameStallFromIEW(tid));
                breakRename = checkRenameStallFromIEW(tid);
                break;
            }
        }

        insts_to_rename.pop_front();

        if (renameStatus[tid] == Unblocking) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Removing [sn:%llu] PC:%s from rename skidBuffer\n",
                    tid, inst->seqNum, inst->pcState());
        }

        if (inst->isSquashed()) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "instruction %i with PC %s is squashed, skipping.\n",
                    tid, inst->seqNum, inst->pcState());

            ++stats.squashedInsts;

            // Decrement how many instructions are available.
            --insts_available;

            rename_stalls.push(StallReason::InstSquashed);

            continue;
        }

        DPRINTF(Rename,
                "[tid:%i] "
                "Processing instruction [sn:%llu] with PC %s.\n",
                tid, inst->seqNum, inst->pcState());    // 打印当前处理的inst

        // Check here to make sure there are enough destination registers
        // to rename to.  Otherwise block.
        if (!renameMap[tid]->canRename(inst)) {
            DPRINTF(Rename,
                    "Blocking due to "
                    " lack of free physical registers to rename to.\n");
            blockThisCycle = true;
            insts_to_rename.push_front(inst);
            ++stats.fullRegistersEvents;

            blockReason = checkRenameStallFromIEW(tid);

            rename_stalls.push(blockReason);

            breakRename = blockReason;

            break;
        }

        // Handle serializeAfter/serializeBefore instructions.
        // serializeAfter marks the next instruction as serializeBefore.
        // serializeBefore makes the instruction wait in rename until the ROB
        // is empty.

        // In this model, IPR accesses are serialize before
        // instructions, and store conditionals are serialize after
        // instructions.  This is mainly due to lack of support for
        // out-of-order operations of either of those classes of
        // instructions.
        if (inst->isSerializeBefore() && !inst->isSerializeHandled()) {
            DPRINTF(Rename, "Serialize before instruction encountered.\n");

            if (!inst->isTempSerializeBefore()) {
                stats.serializing++;
                inst->setSerializeHandled();
            } else {
                stats.tempSerializing++;
            }

            // Change status over to SerializeStall so that other stages know
            // what this is blocked on.
            renameStatus[tid] = SerializeStall;

            serializeInst[tid] = inst;

            blockThisCycle = true;

            blockReason = StallReason::SerializeStall;

            rename_stalls.push(StallReason::SerializeStall);

            breakRename = StallReason::SerializeStall;

            break;
        } else if ((inst->isStoreConditional() || inst->isSerializeAfter()) &&
                   !inst->isSerializeHandled()) {
            DPRINTF(Rename, "Serialize after instruction encountered.\n");

            stats.serializing++;

            inst->setSerializeHandled();

            serializeAfter(insts_to_rename, tid);
        }
        renameSrcRegs(inst, inst->threadNumber);  // 重命名源寄存器

        renameDestRegs(inst, inst->threadNumber);  // 重命名目的寄存器

        if (inst->isAtomic() || inst->isStore()) {
            storesInProgress[tid]++;  // 存储指令计数++
        } else if (inst->isLoad()) {
            loadsInProgress[tid]++;  // 加载指令计数++
        }

        ++renamed_insts;
        // Notify potential listeners that source and destination registers for
        // this instruction have been renamed.
        ppRename->notify(inst);  // 通知潜在的监听器，当前指令的源和目的寄存器已重命名

        // Put instruction in rename queue.
        toIEW->insts[toIEWIndex] = inst;
        ++(toIEW->size);  // 将当前inst 放入toIEW->insts[toIEWIndex]

        // Increment which instruction we're on.
        ++toIEWIndex;

        // Decrement how many instructions are available.
        --insts_available;
    }

    StallReason stall = StallReason::NoStall;
    for (auto iter : fromDecode->decodeStallReason) {
        if (iter != StallReason::NoStall) {
            stall = iter;
            break;
        }
    }

    for (int i = 0;i < renameWidth;i++) {
        if (i < renamed_insts) {
            renameStalls.at(i) = StallReason::NoStall;
        } else {
            if (!rename_stalls.empty()) {   // 如果当前inst未rename，则根据rename_stalls队列赋值
                renameStalls.at(i) = rename_stalls.front();
                rename_stalls.pop();
            } else if (breakRename != StallReason::NoStall) {
                renameStalls.at(i) = breakRename;
            } else if (instsAvailable < renameWidth && instsAvailable > 0) {
                renameStalls.at(i) = StallReason::OtherFragStall;
            } else if (instsAvailable == 0) {
                renameStalls.at(i) = stall;     // decode stall原因
            }else {
                renameStalls.at(i) = StallReason::OtherStall;
            }
        }
    }

    instsInProgress[tid] += renamed_insts;
    stats.renamedInsts += renamed_insts;

    // If we wrote to the time buffer, record this.
    if (toIEWIndex) {
        wroteToTimeBuffer = true;
    }

    // Check if there's any instructions left that haven't yet been renamed.
    // If so then block.
    if (insts_available) {
        blockThisCycle = true;
        blockReason = breakRename;
    }

    if (blockThisCycle) {
        block(tid);
        toDecode->renameUnblock[tid] = false;
    }
}

void
Rename::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop_front();

        assert(tid == inst->threadNumber);

        DPRINTF(Rename, "[tid:%i] Inserting [sn:%llu] PC: %s into Rename "
                "skidBuffer\n", tid, inst->seqNum, inst->pcState());

        ++stats.skidInsts;

        skidBuffer[tid].push_back(inst);
    }

    if (skidBuffer[tid].size() > skidBufferMax) {
        InstQueue::iterator it;
        warn("Skidbuffer contents:\n");
        for (it = skidBuffer[tid].begin(); it != skidBuffer[tid].end(); it++) {
            warn("[tid:%i] %s [sn:%llu].\n", tid,
                    (*it)->staticInst->disassemble(
                        inst->pcState().instAddr()),
                    (*it)->seqNum);
        }
        panic("Skidbuffer Exceeded Max Size");
    }
}

void
Rename::sortInsts()
{
    int insts_from_decode = fromDecode->size;
    for (int i = 0; i < insts_from_decode; ++i) {
        const DynInstPtr &inst = fromDecode->insts[i];
        if (localSquashVer.largerThan(inst->getVersion())) {
            inst->setSquashed();
        } else {
            insts[inst->threadNumber].push_back(inst);
        }
#if TRACING_ON
        if (debug::O3PipeView) {
            inst->renameTick = curTick() - inst->fetchTick;
        }
#endif
    }
}

bool
Rename::skidsEmpty()
{
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (!skidBuffer[tid].empty())
            return false;
    }

    return true;
}

void
Rename::updateStatus()
{
    bool any_unblocking = false;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (renameStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

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

bool
Rename::block(ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Blocking.\n", tid);

    // Add the current inputs onto the skid buffer, so they can be
    // reprocessed when this stage unblocks.
    skidInsert(tid);

    // Only signal backwards to block if the previous stages do not think
    // rename is already blocked.
    if (renameStatus[tid] != Blocked) {
        // If resumeUnblocking is set, we unblocked during the squash,
        // but now we're have unblocking status. We need to tell earlier
        // stages to block.
        if (resumeUnblocking || renameStatus[tid] != Unblocking) {
            toDecode->renameBlock[tid] = true;
            toDecode->renameUnblock[tid] = false;
            wroteToTimeBuffer = true;
        }

        // Rename can not go from SerializeStall to Blocked, otherwise
        // it would not know to complete the serialize stall.
        if (renameStatus[tid] != SerializeStall) {
            // Set status to Blocked.
            renameStatus[tid] = Blocked;
            return true;
        }
    }

    return false;
}

bool
Rename::unblock(ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Trying to unblock.\n", tid);

    // Rename is done unblocking if the skid buffer is empty.
    if (skidBuffer[tid].empty() && renameStatus[tid] != SerializeStall) {

        DPRINTF(Rename, "[tid:%i] Done unblocking.\n", tid);

        toDecode->renameUnblock[tid] = true;
        wroteToTimeBuffer = true;

        renameStatus[tid] = Running;
        return true;
    }

    return false;
}

void
Rename::tryFreePReg(PhysRegIdPtr preg)
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
        freeList->addReg(preg);
    } else {
        DPRINTF(Rename, "Not to free up p%i on squash for ref=%i\n",
                preg->flatIndex(), preg->getRef());
    }
}

void
Rename::doSquash(const InstSeqNum &squashed_seq_num, ThreadID tid)
{
    auto hb_it = historyBuffer[tid].begin();

    // After a syscall squashes everything, the history buffer may be empty
    // but the ROB may still be squashing instructions.
    // Go through the most recent instructions, undoing the mappings
    // they did and freeing up the registers.
    while (!historyBuffer[tid].empty() &&
           hb_it->instSeqNum > squashed_seq_num) {
        assert(hb_it != historyBuffer[tid].end());

        DPRINTF(Rename, "[tid:%i] Removing history entry with sequence "
                "number %i (archReg: %d, newPhysReg: %d, prevPhysReg: %d).\n",
                tid, hb_it->instSeqNum, hb_it->archReg.index(),
                hb_it->newPhysReg->index(), hb_it->prevPhysReg->index());

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
            tryFreePReg(hb_it->newPhysReg);
        }

        // Notify potential listeners that the register mapping needs to be
        // removed because the instruction it was mapped to got squashed. Note
        // that this is done before hb_it is incremented.
        ppSquashInRename->notify(std::make_pair(hb_it->instSeqNum,
                                                hb_it->newPhysReg));

        historyBuffer[tid].erase(hb_it++);

        ++stats.undoneMaps;
    }
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
                "[tid:%i] try to free up older rename of reg p%i (%s), "
                "[sn:%llu].\n",
                tid, hb_it->prevPhysReg->flatIndex(),
                hb_it->prevPhysReg->className(), hb_it->instSeqNum);


        // Don't free special phys regs like misc and zero regs, which
        // can be recognized because the new mapping is the same as
        // the old one.
        if (hb_it->newPhysReg != hb_it->prevPhysReg) {
            tryFreePReg(hb_it->prevPhysReg);
        }

        ++stats.committedMaps;

        historyBuffer[tid].erase(hb_it--);
    }
}

void
Rename::renameSrcRegs(const DynInstPtr &inst, ThreadID tid)
{
    gem5::ThreadContext *tc = inst->tcBase();
    UnifiedRenameMap *map = renameMap[tid];  // 获取重命名映射
    unsigned num_src_regs = inst->numSrcRegs();  // 获取源寄存器数量

    // 获取源寄存器和操作数的架构寄存器编号，
    // 并将它们重定向到正确的物理寄存器。
    for (int src_idx = 0; src_idx < num_src_regs; src_idx++) {  // 遍历所有源寄存器
        const RegId& src_reg = inst->srcRegIdx(src_idx);  // 获取当前源寄存器
        PhysRegIdPtr renamed_reg;  // 重命名后的物理寄存器

        renamed_reg = map->lookup(tc->flattenRegId(src_reg));  // 查找重命名后的物理寄存器
        switch (src_reg.classValue()) {  // 根据寄存器类型进行统计
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
                "Looking up %s arch reg x%i, got p%i\n",
                tid, src_reg.className(),
                src_reg.index(), renamed_reg->flatIndex());  // 打印重命名结果

        inst->renameSrcReg(src_idx, renamed_reg);  // 重命名源寄存器

        // 检查寄存器是否就绪
        if (scoreboard->getReg(renamed_reg)) {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is ready.\n",
                    tid, renamed_reg->index(), renamed_reg->flatIndex(),
                    renamed_reg->className());  // 打印寄存器就绪信息

            inst->markSrcRegReady(src_idx);  // 标记源寄存器就绪
        } else {
            DPRINTF(Rename,
                    "[tid:%i] "
                    "Register %d (flat: %d) (%s) is not ready.\n",
                    tid, renamed_reg->index(), renamed_reg->flatIndex(),
                    renamed_reg->className());
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

    // Rename the destination registers.  重命名目的寄存器
    for (int dest_idx = 0; dest_idx < num_dest_regs; dest_idx++) {
        const RegId& dest_reg = inst->destRegIdx(dest_idx);  // 获取当前目的寄存器
        UnifiedRenameMap::RenameInfo rename_result;  // 重命名结果

        RegId flat_dest_regid = tc->flattenRegId(dest_reg);  // 展平目的寄存器
        flat_dest_regid.setNumPinnedWrites(dest_reg.getNumPinnedWrites());  // 设置展平目的寄存器的写入次数

        PhysRegIdPtr last_dest_phy_reg = nullptr;  // 最后一个空闲目的物理寄存器
        bool mov_elim = false;  // mov 消除标志
        if (inst->staticInst->isMov()) {  // 检查是否是mov指令
            last_dest_phy_reg =
                map->lookup(tc->flattenRegId(inst->srcRegIdx(0))); 
            DPRINTF(Rename, "Find the last reg p%i renamed for mv x%i, x%i\n",
                    last_dest_phy_reg->flatIndex(), dest_reg.index(),
                    inst->srcRegIdx(0).index());  // 打印找到的寄存器 find the last reg p65535 renamed for mv x1, x0
            mov_elim = true;  // 设置mov消除标志
            inst->setEmptyMove(true);  // 设置空指令标志
            DPRINTF(Rename, "Inst sn:%lu is nop: %i, is move: %i\n",
                    inst->seqNum, inst->isNop(), inst->staticInst->isMov());  // mov 设置为nop 指令
        }

        rename_result = map->rename(flat_dest_regid, last_dest_phy_reg);  // 重命名目的寄存器

        inst->flattenedDestIdx(dest_idx, flat_dest_regid);  // 更新指令的展平目的寄存器索引

        if (!mov_elim) {
            scoreboard->unsetReg(rename_result.first);  // 如果不是移动消除,则在记分板中取消设置该寄存器
        }

        DPRINTF(Rename, "[tid:%i] %s map arch reg x%i (%s) to p%i.\n",
                tid, mov_elim ? "Mov" : "Rename",
                dest_reg.index(), dest_reg.className(),
                rename_result.first->flatIndex());  // 打印重命名结果

        // Record the rename information so that a history can be kept. 记录重命名信息以便保持历史记录
        RenameHistory hb_entry(inst->seqNum, flat_dest_regid,
                               rename_result.first,
                               rename_result.second);  // 记录重命名信息

        historyBuffer[tid].push_front(hb_entry);  // 将重命名信息添加到历史记录中

        DPRINTF(Rename, "[tid:%i] [sn:%llu] "
                "Adding instruction to history buffer (size=%i).\n",
                tid,(*historyBuffer[tid].begin()).instSeqNum,
                historyBuffer[tid].size());  // 打印历史记录大小

        // Tell the instruction to rename the appropriate destination
        // register (dest_idx) to the new physical register
        // (rename_result.first), and record the previous physical
        // register that the same logical register was renamed to
        // (rename_result.second).
        inst->renameDestReg(dest_idx,
                            rename_result.first,
                            rename_result.second);  // 重命名目的寄存器

        ++stats.renamedOperands;  // 增加重命名操作数统计
    }
}

int
Rename::calcFreeROBEntries(ThreadID tid)
{
    int num_free = freeEntries[tid].robEntries -
                  (instsInProgress[tid] - fromIEW->iewInfo[tid].dispatched);

    //DPRINTF(Rename,"[tid:%i] %i rob free\n",tid,num_free);

    return num_free;
}

int
Rename::calcFreeIQEntries(ThreadID tid)
{

    return 100;
}

int
Rename::calcFreeLQEntries(ThreadID tid)
{
        int num_free = freeEntries[tid].lqEntries -
            (loadsInProgress[tid] - fromIEW->iewInfo[tid].dispatchedToLQ);
        DPRINTF(Rename,
                "calcFreeLQEntries: free lqEntries: %d, loadsInProgress: %d, "
                "loads dispatchedToLQ: %d\n",
                freeEntries[tid].lqEntries, loadsInProgress[tid],
                fromIEW->iewInfo[tid].dispatchedToLQ);
        return num_free;
}

int
Rename::calcFreeSQEntries(ThreadID tid)
{
        int num_free = freeEntries[tid].sqEntries -
            (storesInProgress[tid] - fromIEW->iewInfo[tid].dispatchedToSQ);
        DPRINTF(Rename, "calcFreeSQEntries: free sqEntries: %d, "
                "storesInProgress: %d, stores dispatchedToSQ: %d\n",
                freeEntries[tid].sqEntries, storesInProgress[tid],
                fromIEW->iewInfo[tid].dispatchedToSQ);
        return num_free;
}

unsigned
Rename::validInsts()
{
    unsigned inst_count = 0;

    for (int i=0; i<fromDecode->size; i++) {
        if (!fromDecode->insts[i]->isSquashed())
            inst_count++;
    }

    return inst_count;
}

void
Rename::readStallSignals(ThreadID tid)
{
    if (fromIEW->iewBlock[tid]) {
        stalls[tid].iew = true;
    }

    if (fromIEW->iewUnblock[tid]) {
        assert(stalls[tid].iew);
        stalls[tid].iew = false;
    }
}

bool
Rename::checkStall(ThreadID tid)
{
    bool ret_val = false;

    if (stalls[tid].iew) {
        DPRINTF(Rename,"[tid:%i] Stall from IEW stage detected.\n", tid);

        blockReason = fromIEW->iewInfo[tid].blockReason;
        ret_val = true;
    } else if (calcFreeROBEntries(tid) <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: ROB has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    } else if (calcFreeIQEntries(tid) <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: IQ has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    } else if (calcFreeLQEntries(tid) <= 0 && calcFreeSQEntries(tid) <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: LSQ has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    } else if (renameMap[tid]->numFreeEntries() <= 0) {
        DPRINTF(Rename,"[tid:%i] Stall: RenameMap has 0 free entries.\n", tid);
        blockReason = checkRenameStallFromIEW(tid);
        ret_val = true;
    } else if (renameStatus[tid] == SerializeStall &&
               (!emptyROB[tid] || instsInProgress[tid])) {
        DPRINTF(Rename,"[tid:%i] Stall: Serialize stall and ROB is not "
                "empty.\n",
                tid);
        blockReason = StallReason::SerializeStall;
        ret_val = true;
    }

    return ret_val;
}

void
Rename::readFreeEntries(ThreadID tid)
{
    if (fromIEW->iewInfo[tid].usedLSQ) {
        freeEntries[tid].lqEntries = fromIEW->iewInfo[tid].freeLQEntries;
        freeEntries[tid].sqEntries = fromIEW->iewInfo[tid].freeSQEntries;
    }

    if (fromCommit->commitInfo[tid].usedROB) {
        freeEntries[tid].robEntries =
            fromCommit->commitInfo[tid].freeROBEntries;
        emptyROB[tid] = fromCommit->commitInfo[tid].emptyROB;
        if (emptyROB[tid])
            DPRINTF(Rename, "[tid:%i] ROB is empty now.\n", tid);
    }

    DPRINTF(Rename, "[tid:%i] Free ROB: %i, "
                    "Free LQ: %i, Free SQ: %i, FreeRM %i(%i %i %i %i %i %i)\n",
            tid,
            freeEntries[tid].robEntries,
            freeEntries[tid].lqEntries,
            freeEntries[tid].sqEntries,
            renameMap[tid]->numFreeEntries(),
            renameMap[tid]->numFreeEntries(IntRegClass),
            renameMap[tid]->numFreeEntries(FloatRegClass),
            renameMap[tid]->numFreeEntries(VecRegClass),
            renameMap[tid]->numFreeEntries(VecElemClass),
            renameMap[tid]->numFreeEntries(VecPredRegClass),
            renameMap[tid]->numFreeEntries(CCRegClass));

    DPRINTF(Rename, "[tid:%i] %i instructions not yet in ROB\n",
            tid, instsInProgress[tid]);
}

bool
Rename::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is
    // Check stall signals, block if necessary.
    // If status was blocked
    //     check if stall conditions have passed
    //         if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.
    // If status was serialize stall
    //     check if ROB is empty and no insts are in flight to the ROB

    readFreeEntries(tid);
    readStallSignals(tid);

    if (fromCommit->commitInfo[tid].squash) {
        DPRINTF(Rename, "[tid:%i] Squashing instructions due to squash from "
                "commit.\n", tid);

        squash(fromCommit->commitInfo[tid].doneSeqNum, tid);

        localSquashVer.update(fromCommit->commitInfo[tid].squashVersion.getVersion());
        DPRINTF(Rename, "Updating squash version to %u\n",
                localSquashVer.getVersion());

        return true;
    }

    if (checkStall(tid)) {
        return block(tid);
    }

    if (renameStatus[tid] == Blocked) {
        DPRINTF(Rename, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        renameStatus[tid] = Unblocking;

        unblock(tid);

        return true;
    }

    if (renameStatus[tid] == Squashing) {
        // Switch status to running if rename isn't being told to block or
        // squash this cycle.
        if (resumeSerialize) {
            DPRINTF(Rename,
                    "[tid:%i] Done squashing, switching to serialize.\n", tid);

            renameStatus[tid] = SerializeStall;
            return true;
        } else if (resumeUnblocking) {
            DPRINTF(Rename,
                    "[tid:%i] Done squashing, switching to unblocking.\n",
                    tid);
            renameStatus[tid] = Unblocking;
            return true;
        } else {
            DPRINTF(Rename, "[tid:%i] Done squashing, switching to running.\n",
                    tid);
            renameStatus[tid] = Running;
            return false;
        }
    }

    if (renameStatus[tid] == SerializeStall) {
        // Stall ends once the ROB is free.
        DPRINTF(Rename, "[tid:%i] Done with serialize stall, switching to "
                "unblocking.\n", tid);

        DynInstPtr serial_inst = serializeInst[tid];

        renameStatus[tid] = Unblocking;

        unblock(tid);

        DPRINTF(Rename, "[tid:%i] Processing instruction [%lli] with "
                "PC %s.\n", tid, serial_inst->seqNum, serial_inst->pcState());

        // Put instruction into queue here.
        serial_inst->clearSerializeBefore();

        if (!skidBuffer[tid].empty()) {
            skidBuffer[tid].push_front(serial_inst);
        } else {
            insts[tid].push_front(serial_inst);
        }

        DPRINTF(Rename, "[tid:%i] Instruction must be processed by rename."
                " Adding to front of list.\n", tid);

        serializeInst[tid] = NULL;

        return true;
    }

    // If we've reached this point, we have not gotten any signals that
    // cause rename to change its status.  Rename remains the same as before.
    return false;
}

void
Rename::serializeAfter(InstQueue &inst_list, ThreadID tid)
{
    if (inst_list.empty()) {
        // Mark a bit to say that I must serialize on the next instruction.
        serializeOnNextInst[tid] = true;
        return;
    }

    // Set the next instruction as serializing.
    inst_list.front()->setSerializeBefore();
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
                    (*buf_it).newPhysReg->index(),
                    (*buf_it).newPhysReg->className(),
                    (*buf_it).prevPhysReg->index(),
                    (*buf_it).prevPhysReg->className());

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

    if (robHeadStallReason == StallReason::NoStall) {
        if (calcFreeLQEntries(tid) <= 0) {
            return fromIEW->iewInfo[tid].lqHeadStallReason;
        } else if (calcFreeSQEntries(tid) <= 0) {
            return fromIEW->iewInfo[tid].sqHeadStallReason;
        } else {
            return robHeadStallReason;
        }
    } else {
        return robHeadStallReason;
    }
}

} // namespace o3
} // namespace gem5
