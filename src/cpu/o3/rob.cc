/*
 * Copyright (c) 2012 ARM Limited
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

#include "cpu/o3/rob.hh"

#include <algorithm>
#include <list>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/commit.hh"
#include "debug/Fetch.hh"
#include "debug/ROB.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

bool
ROB::allocateGroup_none(const DynInstPtr inst, ThreadID tid)
{
    return true; // No group allocation needed
}

bool
ROB::allocateGroup_kmhv2(const DynInstPtr inst, ThreadID tid)
{
    auto& groups = threadGroups[tid];
    auto& prev = instList[tid].back();

    // load/store/control exclusive one group
    bool alloc = false;
    if (groups.empty()) [[unlikely]] {
        alloc = true;
    } else if (inst->isMemRef() || inst->isControl() || inst->isNonSpeculative()) {
        alloc = true;
    } else if (prev->isMemRef() || prev->isControl() ||
               prev->isNonSpeculative()) {
        alloc = true;
    } else if (prev->ftqId != inst->ftqId) {
        alloc = true;
    } else if (lastInsertCycle != cpu->curCycle()) {
        // different cycle
        alloc = true;
    } else if (groups.back() >= instsPerGroup) {
        alloc = true;
    }
    return alloc;
}

bool
ROB::allocateGroup_MohBoE(const DynInstPtr inst, ThreadID tid)
{
    auto& groups = threadGroups[tid];
    auto& prev = instList[tid].back();

    // load/store on group head
    // control on group end
    bool alloc = false;
    if (groups.empty()) [[unlikely]] {
        alloc = true;
    } else if (inst->isMemRef() || inst->isNonSpeculative()) {
        alloc = true;
    } else if (prev->isControl()) {
        alloc = true;
    } else if (lastInsertCycle != cpu->curCycle()) {
        // different cycle
        alloc = true;
    } else if (groups.back() >= instsPerGroup) {
        alloc = true;
    }
    return alloc;
}

bool
ROB::allocateGroup_kmhv3(const DynInstPtr inst, ThreadID tid)
{
    auto& groups = threadGroups[tid];
    auto& prev = instList[tid].back();

    bool alloc = false;
    if (groups.empty()) [[unlikely]] {
        alloc = true;
    } else if (lastInsertCycle != cpu->curCycle()) {
        // different cycle
        alloc = true;
    } else if (groups.back() >= instsPerGroup) {
        alloc = true;
    }
    return alloc;
}

ROB::HybridInstClass
ROB::classifyHybridInst(const DynInstPtr &inst) const
{
    // DynInst's serialize-before/after queries include dynamic status bits.
    // Classification observes enqueue-time state, including fused DynInsts.
    if (inst->faulted() || inst->isSerializing() ||
        inst->isSerializeBefore() || inst->isSerializeAfter() ||
        inst->isNonSpeculative() || inst->isSquashAfter() ||
        inst->isReadBarrier() || inst->isWriteBarrier() ||
        inst->isAtomic() || inst->isLoadReserved() ||
        inst->isStoreConditional() || inst->isVector() ||
        inst->opClass() == VectorConfigOp ||
        inst->isMicroop() || inst->isMacroop()) {
        return HybridInstClass::NoCompress;
    }

    if (inst->isLoad() || inst->isStore() || inst->isControl() ||
        inst->opClass() == IntJpOp) {
        return HybridInstClass::Complex;
    }

    // Use decoded semantics, including RVC and hints decoded as IntAluOp.
    switch (inst->opClass()) {
      case IntAluOp:
      case IntMultOp:
      case IntDivOp:
      case Int2FpOp:
      case FloatAddOp:
      case FloatMultOp:
      case FloatMultAccOp:
      case FloatDivOp:
      case FloatSqrtOp:
      case FloatCmpOp:
      case FloatCvtOp:
      case FloatMvOp:
      case FloatMiscOp:
        return HybridInstClass::Simple;
      default:
        DPRINTF(ROB, "Hybrid N [sn:%llu]: uncovered decoded OpClass %s\n",
                inst->seqNum, enums::OpClassStrings[inst->opClass()]);
        return HybridInstClass::NoCompress;
    }
}

ROB::HybridPlan
ROB::planHybridBatch(const std::vector<DynInstPtr> &insts) const
{
    assert(isHybrid());
    HybridPlan plan;
    plan.reserve(insts.size());

    for (const auto &inst : insts) {
        assert(inst && !inst->isSquashed() && inst->threadNumber == 0);
        appendHybridClass(plan, classifyHybridInst(inst), instsPerGroup);
    }
    return plan;
}

void
ROB::insertHybridBatch(const std::vector<DynInstPtr> &insts,
                      const HybridPlan &plan)
{
    assert(isHybrid());
    assert(canAllocate(0, plan.size()));
#ifndef NDEBUG
    size_t planned_insts = 0;
    for (const auto &group : plan) {
        assert(group.memberCount() > 0 && group.memberCount() <= instsPerGroup);
        planned_insts += group.memberCount();
    }
    assert(planned_insts == insts.size());
#endif

    size_t next_inst = 0;
    for (const auto &group : plan) {
        HybridEntryType type = HybridEntryType::NORMAL;
        switch (group.type) {
          case HybridGroupType::CC: type = HybridEntryType::CC; break;
          case HybridGroupType::CS: type = HybridEntryType::CS; break;
          case HybridGroupType::SC: type = HybridEntryType::SC; break;
          default: break;
        }
        assert(canAllocate(0, 1));
        const uint64_t id = nextHybridEntryId++;
        assert(id != 0);
        hybridEntries[0].push_back({id, type, 0, 0});
        DPRINTF(ROB, "Hybrid allocate id=%llu type=%u former=%u latter=%u\n",
                id, static_cast<unsigned>(group.type),
                group.formerLength, group.latterLength);
        for (unsigned offset = 0; offset < group.memberCount(); ++offset) {
            const auto &inst = insts[next_inst++];
            assert(inst && !inst->isSquashed() && inst->threadNumber == 0);
            inst->hybridEntryId = id;
            inst->hybridSlotIsFormer = offset < group.formerLength;
            insertInstWithGroup(inst, offset == 0);
            DPRINTF(ROB, "Hybrid member id=%llu sn=%llu former=%u\n",
                    id, inst->seqNum, inst->hybridSlotIsFormer);
        }
        stats.hybridGroupType[static_cast<unsigned>(group.type)]++;
        stats.hybridGroupLength.sample(group.memberCount());
        stats.instPergroup.sample(group.memberCount());
    }
    stats.hybridAllocatedGroups += plan.size();
    stats.hybridAllocatedInsts += insts.size();
    assertHybridInvariants(0);
}

void
ROB::assertHybridInvariants(ThreadID tid) const
{
    if (!isHybrid()) {
        return;
    }
    assert(tid == 0);
    const auto &entries = hybridEntries[tid];
    assert(threadGroups[tid].empty());
    assert(entries.size() <= numEntries);
    assert(entries.empty() == instList[tid].empty());
    assert(numInstsInROB >= 0 &&
           static_cast<size_t>(numInstsInROB) == instList[tid].size());
    assert(entries.empty() ||
           (entries.front().memberCount() > 0 &&
            entries.front().memberCount() <= instsPerGroup &&
            entries.back().memberCount() > 0 &&
            entries.back().memberCount() <= instsPerGroup));
#ifndef NDEBUG
    // Full scans are explicit ROB-trace validation, never a normal hot path.
    if (debug::ROB) {
        size_t count = 0;
        auto inst = instList[tid].begin();
        for (const auto &entry : entries) {
            assert(entry.memberCount() > 0 &&
                   entry.memberCount() <= instsPerGroup);
            for (unsigned i = 0; i < entry.memberCount(); ++i, ++inst) {
                assert(inst != instList[tid].end());
                assert((*inst)->hybridEntryId == entry.id);
                assert((*inst)->hybridSlotIsFormer ==
                       (i < entry.formerRemaining));
            }
            count += entry.memberCount();
        }
        assert(count == instList[tid].size());
        DPRINTF(ROB, "Hybrid invariant groups=%u dynInsts=%u sum=%u\n",
                entries.size(), instList[tid].size(), count);
    }
#endif
}

ROB::ROB(CPU *_cpu, const BaseO3CPUParams &params)
    : robPolicy(params.smtROBPolicy),
      borrowingDonorReserveEntries(params.smtBorrowDonorReserveEntries),
      borrowingBaseReserveEntries(params.smtBorrowBaseReserveEntries),
      robWalkPolicy(params.robWalkPolicy),
      hybrid(params.RobCompressPolicy == ROBCompressPolicy::hybrid),
      cpu(_cpu),
      numEntries(params.numROBEntries),
      instsPerGroup(params.CROB_instPerGroup),
      rollbackWidth(params.squashWidth),
      replayWidth(params.squashWidth),
      constSquashCycle(params.ConstSquashCycle),
      robWalkByDestRegs(params.robWalkByDestRegs),
      numInstsInROB(0),
      numThreads(params.numThreads),
      stats(_cpu, params.CROB_instPerGroup)
{
    if (isHybrid()) {
        fatal_if(params.numThreads != 1,
                 "Hybrid CROB requires numThreads = 1");
        fatal_if(params.valuePred != nullptr,
                 "Hybrid CROB requires valuePred = NULL");
        fatal_if(params.enable_loadFusion,
                 "Hybrid CROB requires enable_loadFusion = False");
        fatal_if(params.enableConstantFolding ||
                 params.enableMovImmElimination,
                 "Hybrid CROB requires enableConstantFolding = False and "
                 "enableMovImmElimination = False");
        fatal_if(instsPerGroup == 0,
                 "Hybrid CROB requires CROB_instPerGroup > 0");
    }
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        borrowingDonor[tid] = false;
        borrowingStateHoldCycle[tid] = 0;
    }

    //Figure out rob policy
    if (robPolicy == SMTQueuePolicy::Dynamic) {
        //Set Max Entries to Total ROB Capacity
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = numEntries;
        }

    } else if (robPolicy == SMTQueuePolicy::DynamicBorrowing) {
        DPRINTF(Fetch, "ROB sharing policy set to DynamicBorrowing\n");

        int part_amt = numEntries / numThreads;
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = part_amt;
        }

    } else if (robPolicy == SMTQueuePolicy::Partitioned) {
        DPRINTF(Fetch, "ROB sharing policy set to Partitioned\n");

        //@todo:make work if part_amt doesnt divide evenly.
        int part_amt = numEntries / numThreads;

        //Divide ROB up evenly
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = part_amt;
        }

    } else if (robPolicy == SMTQueuePolicy::Threshold) {
        DPRINTF(Fetch, "ROB sharing policy set to Threshold\n");

        int threshold =  params.smtROBThreshold;;

        //Divide up by threshold amount
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = threshold;
        }
    }

    assert((robWalkPolicy == ROBWalkPolicy::Rollback && rollbackWidth > 0)
           || (robWalkPolicy == ROBWalkPolicy::Replay && replayWidth > 0)
           || (robWalkPolicy == ROBWalkPolicy::ConstCycle && constSquashCycle > 0)
           || (robWalkPolicy == ROBWalkPolicy::NaiveCpt && replayWidth > 0)
        //    || robWalkPolicy == ROBWalkPolicy::ConfidentCpt
           );

    for (ThreadID tid = numThreads; tid < MaxThreads; tid++) {
        maxEntries[tid] = 0;
    }

    switch(params.RobCompressPolicy) {
        case ROBCompressPolicy::none:
            allocateNewGroup = &ROB::allocateGroup_none;
            instsPerGroup = 1;
            break;
        case ROBCompressPolicy::kmhv2:
            allocateNewGroup = &ROB::allocateGroup_kmhv2;
            break;
        case ROBCompressPolicy::MohBoE:
            allocateNewGroup = &ROB::allocateGroup_MohBoE;
            break;
        case ROBCompressPolicy::kmhv3:
            allocateNewGroup = &ROB::allocateGroup_kmhv3;
            break;
        case ROBCompressPolicy::hybrid:
            // Hybrid insertion consumes explicit batch boundaries.
            allocateNewGroup = nullptr;
            break;
        default:
            panic("Unknown ROB compression policy");
            break;
    }

    resetState();
}

void
ROB::resetState()
{
    for (ThreadID tid = 0; tid  < MaxThreads; tid++) {
        threadGroups[tid].clear();
        hybridEntries[tid].clear();
        squashIt[tid] = instList[tid].end();
        squashedSeqNum[tid] = 0;
        doneSquashing[tid] = true;
        borrowingDonor[tid] = false;
    }
    numInstsInROB = 0;
    hybridSquashTarget.reset();
    nextHybridEntryId = 1;

    // Initialize the "universal" ROB head & tail point to invalid
    // pointers
    head = instList[0].end();
    tail = instList[0].end();
}

std::string
ROB::name() const
{
    return cpu->name() + ".rob";
}

void
ROB::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    DPRINTF(ROB, "Setting active threads list pointer.\n");
    activeThreads = at_ptr;
}

void
ROB::drainSanityCheck() const
{
    for (ThreadID tid = 0; tid  < numThreads; tid++)
        assert(instList[tid].empty());
    assert(isEmpty());
}

void
ROB::takeOverFrom()
{
    resetState();
}

void
ROB::resetEntries()
{
    if (robPolicy != SMTQueuePolicy::Dynamic || numThreads > 1) {
        auto active_threads = activeThreads->size();

        std::list<ThreadID>::iterator threads = activeThreads->begin();
        std::list<ThreadID>::iterator end = activeThreads->end();

        while (threads != end) {
            ThreadID tid = *threads++;

            if (robPolicy == SMTQueuePolicy::Partitioned) {
                maxEntries[tid] = numEntries / active_threads;
            } else if (robPolicy == SMTQueuePolicy::DynamicBorrowing) {
                maxEntries[tid] = numEntries / active_threads;
            } else if (robPolicy == SMTQueuePolicy::Threshold &&
                       active_threads == 1) {
                maxEntries[tid] = numEntries;
            }
        }
    }
}

int
ROB::entryAmount(ThreadID num_threads)
{
    if (robPolicy == SMTQueuePolicy::Partitioned) {
        return numEntries / num_threads;
    } else if (robPolicy == SMTQueuePolicy::DynamicBorrowing) {
        return numEntries / num_threads;
    } else {
        return 0;
    }
}

unsigned
ROB::activeThreadCount() const
{
    if (!activeThreads || activeThreads->empty()) {
        return numThreads == 0 ? 1 : numThreads;
    }
    return activeThreads->size();
}

unsigned
ROB::totalEntries() const
{
    unsigned total = 0;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        total += getThreadEntries(tid);
    }
    return total;
}

bool
ROB::canBorrow(ThreadID tid) const
{
    return robPolicy == SMTQueuePolicy::DynamicBorrowing &&
           tid < numThreads;
}

unsigned
ROB::borrowingLimit(ThreadID tid) const
{
    if (tid >= numThreads) {
        return 0;
    }

    if (!canBorrow(tid)) {
        return maxEntries[tid];
    }

    const unsigned active_threads = std::max(1U, activeThreadCount());
    const unsigned base = borrowingBaseReserveEntries;
    const unsigned donor_resume_quota =
        std::min(base, borrowingDonorReserveEntries);

    unsigned reserved = 0;
    for (ThreadID other = 0; other < numThreads; ++other) {
        if (other == tid) {
            continue;
        }

        const unsigned reserve =
            borrowingDonor[other] ? donor_resume_quota : base;
        const unsigned used = getThreadEntries(other);
        reserved += std::max(reserve, used);
    }

    if (reserved >= numEntries) {
        return 0;
    }

    return numEntries - reserved;
}

void 
ROB::setBorrowingDonor(ThreadID tid, bool donor, Commit* commit)
{ 
    if (borrowingDonor[tid] != donor) {
        bool old_donor = borrowingDonor[tid];
        unsigned state_hold_cycle = borrowingStateHoldCycle[tid];
        unsigned rob_entries_used = getThreadEntries(tid);
        unsigned rob_entries_free = numFreeEntries(tid);

        commit->recordROBBorrowingStateChangeStats(
            tid, old_donor, state_hold_cycle,
            rob_entries_used, rob_entries_free);

        borrowingDonor[tid] = donor; 
        borrowingStateHoldCycle[tid] = 0;
    }
}

void 
ROB::addBorrowingStateHoldCycle() {
    for (int i = 0; i < numThreads; ++i) {
        borrowingStateHoldCycle[i]++;
    }
}

bool
ROB::canAllocate(ThreadID tid, unsigned entries) const
{
    if (tid >= numThreads) {
        return false;
    }

    const unsigned used = getThreadEntries(tid);

    if (isHybrid() && used + entries > numEntries) {
        return false;
    }

    if (robPolicy == SMTQueuePolicy::DynamicBorrowing) {
        if (totalEntries() + entries > numEntries) {
            return false;
        }
        return used + entries <= borrowingLimit(tid);
    }

    return used + entries <= maxEntries[tid];
}

int
ROB::countInsts()
{
    int total = 0;

    for (ThreadID tid = 0; tid < numThreads; tid++)
        total += countInsts(tid);

    return total;
}

size_t
ROB::countInsts(ThreadID tid)
{
    return instList[tid].size();
}

uint32_t
ROB::countInstsOfGroups(ThreadID tid, int groups)
{
    int sum = 0;
    if (isHybrid()) {
        auto it = hybridEntries[tid].begin();
        for (int i = 0; i < groups && it != hybridEntries[tid].end();
             ++i, ++it) {
            sum += it->memberCount();
        }
        return sum;
    }
    auto it = threadGroups[tid].begin();
    for (int i = 0; i < groups && it != threadGroups[tid].end(); i++, it++) {
        sum += *it;
    }
    return sum;
}

uint32_t
ROB::countInstsOfGroups(int groups)
{
    int sum = 0;
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        sum += countInstsOfGroups(tid, groups);
    }
    return sum;
}

void
ROB::commitGroup(const DynInstPtr inst, ThreadID tid)
{
    if (isHybrid()) {
        auto &entry = hybridEntries[tid].front();
        assert(entry.id == inst->hybridEntryId);
        entry.removeMember(inst->hybridSlotIsFormer, false);
        DPRINTF(ROB, "Hybrid remove id=%llu sn=%llu former=%u "
                "reason=%s remaining=%u\n", entry.id, inst->seqNum,
                inst->hybridSlotIsFormer,
                inst->isSquashed() ? "drain" : "commit", entry.memberCount());
        if (entry.memberCount() == 0) {
            hybridEntries[tid].pop_front();
        }
        return;
    }
    assert(!threadGroups[tid].empty());

    if (threadGroups[tid].front() == 1) {
        threadGroups[tid].pop_front();
    } else {
        threadGroups[tid].front()--;
    }
}

void
ROB::squashGroup(const DynInstPtr inst, ThreadID tid)
{
    if (isHybrid()) {
        auto &entry = hybridEntries[tid].back();
        assert(entry.id == inst->hybridEntryId);
        const bool retain_former = hybridSquashTarget &&
            entry.id == hybridSquashTarget->entryId &&
            !hybridSquashTarget->removes(entry.id, true);
        const bool downgraded = entry.removeMember(
            inst->hybridSlotIsFormer, retain_former);
        DPRINTF(ROB, "Hybrid remove id=%llu sn=%llu former=%u "
                "reason=squash remaining=%u\n", entry.id, inst->seqNum,
                inst->hybridSlotIsFormer, entry.memberCount());
        if (downgraded) {
            stats.hybridDowngrades++;
            DPRINTF(ROB, "Hybrid downgrade id=%llu former=%u\n",
                    entry.id, entry.formerRemaining);
        }
        if (entry.memberCount() == 0) {
            hybridEntries[tid].pop_back();
        }
        return;
    }
    assert(!threadGroups[tid].empty());

    if (threadGroups[tid].back() == 1) {
        threadGroups[tid].pop_back();
    } else {
        threadGroups[tid].back()--;
    }
}

void
ROB::insertInst(const DynInstPtr &inst)
{
    assert(inst);
    assert(!isHybrid());
    assert(canAllocate(inst->threadNumber, 1));
    const bool alloc = (this->*allocateNewGroup)(inst, inst->threadNumber);
    insertInstWithGroup(inst, alloc);
}

void
ROB::insertInstWithGroup(const DynInstPtr &inst, bool new_group)
{
    assert(inst);

    stats.writes++;

    DPRINTF(ROB, "Adding inst PC %s to the ROB.\n", inst->pcState());

    assert(numInstsInROB <= numEntries * instsPerGroup);

    ThreadID tid = inst->threadNumber;

    // allocate group
    lastInsertCycle = cpu->curCycle();
    if (isHybrid()) {
        auto &entry = hybridEntries[tid].back();
        assert(entry.id == inst->hybridEntryId);
        auto &remaining = inst->hybridSlotIsFormer ?
            entry.formerRemaining : entry.latterRemaining;
        ++remaining;
        assert(entry.memberCount() <= instsPerGroup);
    } else if (new_group) {
        assert(canAllocate(tid, 1));
        if (!isHybrid() && !threadGroups[tid].empty()) [[likely]] {
            stats.instPergroup.sample(threadGroups[tid].back());
        }
        threadGroups[tid].push_back(1);
    } else {
        assert(threadGroups[tid].back() < instsPerGroup);
        threadGroups[tid].back()++;
    }

    instList[tid].push_back(inst);

    //Set Up head iterator if this is the 1st instruction in the ROB
    if (numInstsInROB == 0) {
        head = instList[tid].begin();
        assert((*head) == inst);
    }

    //Must Decrement for iterator to actually be valid  since __.end()
    //actually points to 1 after the last inst
    tail = instList[tid].end();
    tail--;

    inst->setInROB();

    ++numInstsInROB;

    assert((*tail) == inst);

    DPRINTF(ROB, "[tid:%i] Now has %d instructions.\n", tid,
            getThreadEntries(tid));
    assertHybridInvariants(tid);
}

void
ROB::retireHead(ThreadID tid)
{
    stats.writes++;

    assert(numInstsInROB > 0);

    // Get the head ROB instruction by copying it and remove it from the list
    InstIt head_it = instList[tid].begin();

    DynInstPtr head_inst = std::move(*head_it);
    instList[tid].erase(head_it);

    assert(head_inst->readyToCommit());
    assert(!head_inst->isSquashed());

    DPRINTF(ROB, "[tid:%i] Retiring head instruction, "
            "instruction PC %s, [sn:%llu]\n", tid, head_inst->pcState(),
            head_inst->seqNum);

    --numInstsInROB;

    //Update Group Size
    commitGroup(head_inst, tid);
    assertHybridInvariants(tid);

    head_inst->clearInROB();
    head_inst->setCommitted();

    //Update "Global" Head of ROB
    updateHead();

    // @todo: A special case is needed if the instruction being
    // retired is the only instruction in the ROB; otherwise the tail
    // iterator will become invalidated.
    cpu->removeFrontInst(head_inst);
}

void
ROB::drainSquashedHead(ThreadID tid)
{
    stats.writes++;

    assert(numInstsInROB > 0);

    InstIt head_it = instList[tid].begin();

    DynInstPtr head_inst = std::move(*head_it);
    instList[tid].erase(head_it);

    assert(head_inst->readyToCommit());
    assert(head_inst->isSquashed());

    DPRINTF(ROB, "[tid:%i] Draining squashed head instruction, "
            "instruction PC %s, [sn:%llu]\n", tid, head_inst->pcState(),
            head_inst->seqNum);

    --numInstsInROB;

    commitGroup(head_inst, tid);
    assertHybridInvariants(tid);

    head_inst->clearInROB();

    updateHead();

    cpu->removeFrontInst(head_inst);
}

bool
ROB::isHeadGroupReady(ThreadID tid)
{
    stats.reads++;

    if (headGroupSize(tid) != 0) {
        auto it = instList[tid].begin();
        for (int i = 0; i < headGroupSize(tid); i++, it++) {
            auto& inst = *it;
            // first inst must be readyToCommit
            if (!inst->readyToCommit()) {
                if (i > 0 && inst->isSerializeBefore()) {
                    return true;
                }
                return false;
            }

            // if one group has barrier or non-speculative or fault
            // this group can commit directly.
            if (inst->isNonSpeculative() || inst->isStoreConditional() || !inst->isExecuted() || inst->faulted()) {
                return true;
            }
        }
        return true;
    }

    return false;
}

InstSeqNum
ROB::getHeadGroupLastDoneSeq(ThreadID tid)
{
    if (headGroupSize(tid) != 0) {
        auto it = instList[tid].begin();
        InstSeqNum seqnum = 0;
        for (int i = 0; i < headGroupSize(tid); i++, it++) {
            auto& inst = *it;
            if (!inst->readyToCommit() || !inst->isExecuted() || inst->faulted() ||
                 // If this inst contains a raw violation that is only
                 // handled during a commit, do not bypass it.
                 inst->memDepInfo.violationPending ||
                 // An external snoop can still turn a possible violation into a
                 // ReExec fault. Keep younger stores out of the SBuffer until the
                 // load either commits or is squashed.
                 inst->possibleLoadViolation()) {
                break;
            }
            seqnum = inst->seqNum;
        }
        return seqnum;
    }
    return 0;
}

unsigned
ROB::numFreeEntries(ThreadID tid)
{
    if (robPolicy == SMTQueuePolicy::DynamicBorrowing) {
        const unsigned limit = borrowingLimit(tid);
        const unsigned used = getThreadEntries(tid);
        if (limit <= used) {
            return 0;
        }
        return limit - used;
    }

    const unsigned limit = isHybrid() ?
        std::min(maxEntries[tid], numEntries) : maxEntries[tid];
    return limit - getThreadEntries(tid);
}

void
ROB::doSquash(ThreadID tid)
{
    stats.writes++;
    DPRINTF(ROB, "[tid:%i] Squashing instructions until [sn:%llu].\n",
            tid, squashedSeqNum[tid]);

    assert(squashIt[tid] != instList[tid].end());

    if (!shouldSquash(*squashIt[tid], tid)) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

        doneSquashing[tid] = true;
        return;
    }

    bool robTailUpdate = false;

    assert(dynSquashWidth);
    unsigned int num_insts_to_squash = dynSquashWidth;

    // If the CPU is exiting, squash all of the instructions
    // it is told to, even if that exceeds the squashWidth.
    // Set the number to the number of entries (the max).
    if (cpu->isThreadExiting(tid))
    {
        num_insts_to_squash = numEntries * instsPerGroup;
    }

    for (int numSquashed = 0;
         numSquashed < num_insts_to_squash &&
         squashIt[tid] != instList[tid].end() &&
         shouldSquash(*squashIt[tid], tid);
         ++numSquashed)
    {
        DPRINTF(ROB, "[tid:%i] Squashing instruction PC %s, seq num %i.\n",
                (*squashIt[tid])->threadNumber,
                (*squashIt[tid])->pcState(),
                (*squashIt[tid])->seqNum);

        // Mark the instruction as squashed, and ready to commit so that
        // it can drain out of the pipeline.
        (*squashIt[tid])->setSquashed();

        (*squashIt[tid])->setCanCommit();

        // printf("[ROB] squash seqNum %ld\n", (*squashIt[tid])->seqNum);

        // A Hybrid flush may remove the last DynInst of its final group.
        // There is no predecessor when that instruction is list.begin().
        auto prevIt = isHybrid() && squashIt[tid] == instList[tid].begin() ?
            instList[tid].end() : std::prev(squashIt[tid]);
        --numInstsInROB;

        //Update Group Size
        squashGroup(*squashIt[tid], tid);

        (*squashIt[tid])->clearInROB();
        // head_inst->setCommitted();
        cpu->removeFrontInst(*squashIt[tid]);

        if (instList[tid].empty() || squashIt[tid] == instList[tid].begin()) {
            DPRINTF(ROB, "Reached head of instruction list while "
                    "squashing.\n");

            instList[tid].erase(squashIt[tid]);

            if (isHybrid()) {
                updateHead();
                updateTail();
                assertHybridInvariants(tid);
            }

            squashIt[tid] = instList[tid].end();

            doneSquashing[tid] = true;

            return;
        }

        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        if ((*squashIt[tid]) == (*tail_thread))
            robTailUpdate = true;

        instList[tid].erase(squashIt[tid]);
        assertHybridInvariants(tid);

        squashIt[tid] = prevIt;
    }


    // Check if ROB is done squashing.
    if (!shouldSquash(*squashIt[tid], tid)) {
        DPRINTF(ROB, "[tid:%i] Done squashing instructions.\n",
                tid);

        squashIt[tid] = instList[tid].end();

        doneSquashing[tid] = true;
    }

    if (robTailUpdate) {
        updateTail();
    }
}


void
ROB::updateHead()
{
    InstSeqNum lowest_num = 0;
    bool first_valid = true;

    // @todo: set ActiveThreads through ROB or CPU
    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (instList[tid].empty())
            continue;

        if (first_valid) {
            head = instList[tid].begin();
            lowest_num = (*head)->seqNum;
            first_valid = false;
            continue;
        }

        InstIt head_thread = instList[tid].begin();

        DynInstPtr head_inst = (*head_thread);

        assert(head_inst != 0);

        if (head_inst->seqNum < lowest_num) {
            head = head_thread;
            lowest_num = head_inst->seqNum;
        }
    }

    if (first_valid) {
        head = instList[0].end();
    }

}

void
ROB::updateTail()
{
    tail = instList[0].end();
    bool first_valid = true;

    std::list<ThreadID>::iterator threads = activeThreads->begin();
    std::list<ThreadID>::iterator end = activeThreads->end();

    while (threads != end) {
        ThreadID tid = *threads++;

        if (instList[tid].empty()) {
            continue;
        }

        // If this is the first valid then assign w/out
        // comparison
        if (first_valid) {
            tail = instList[tid].end();
            tail--;
            first_valid = false;
            continue;
        }

        // Assign new tail if this thread's tail is younger
        // than our current "tail high"
        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        if ((*tail_thread)->seqNum > (*tail)->seqNum) {
            tail = tail_thread;
        }
    }
}


bool
ROB::shouldSquash(const DynInstPtr &inst, ThreadID tid) const
{
    if (isHybrid() && hybridSquashTarget) {
        return hybridSquashTarget->removes(inst->hybridEntryId,
                                          inst->hybridSlotIsFormer);
    }
    return inst->seqNum > squashedSeqNum[tid];
}

InstSeqNum
ROB::squashHybrid(const DynInstPtr &redirect, bool flush_itself, ThreadID tid)
{
    assert(isHybrid() && tid == 0 && redirect && redirect->isInROB());
    hybridSquashTarget = HybridSquashTarget{
        redirect->hybridEntryId, redirect->hybridSlotIsFormer, flush_itself};
    // A slot-aware squash always removes a suffix. Publish its exact boundary
    // to the existing sequence-based Rename/IQ/LSQ recovery mechanisms.
    InstSeqNum boundary = instList[tid].front()->seqNum - 1;
    for (const auto &inst : instList[tid]) {
        if (shouldSquash(inst, tid)) {
            break;
        }
        boundary = inst->seqNum;
    }
    DPRINTF(ROB, "Hybrid squash id=%llu former=%u itself=%u boundary=%llu\n",
            redirect->hybridEntryId, redirect->hybridSlotIsFormer,
            flush_itself, boundary);
    startSquash(boundary, tid);
    return boundary;
}

void
ROB::squash(InstSeqNum squash_num, ThreadID tid)
{
    // Full flushes (trap/TC/already-retired squash-after) retain the existing
    // architectural boundary, which can lie inside a partially retired slot.
    hybridSquashTarget.reset();
    if (isHybrid()) {
        DPRINTF(ROB, "Hybrid full squash boundary=%llu\n", squash_num);
    }
    startSquash(squash_num, tid);
}

void
ROB::startSquash(InstSeqNum squash_num, ThreadID tid)
{
    if (isEmpty(tid)) {
        DPRINTF(ROB, "Does not need to squash due to being empty "
                "[sn:%llu]\n",
                squash_num);

        return;
    }

    DPRINTF(ROB, "Starting to squash within the ROB.\n");

    robStatus[tid] = ROBSquashing;

    doneSquashing[tid] = false;

    squashedSeqNum[tid] = squash_num;

    if (robWalkPolicy == ROBWalkPolicy::NaiveCpt) {
        dynSquashWidth = computeSnapshotSquashWidth(squash_num, tid);
    } else {
        unsigned total_inst_to_squash = 0;
        for (auto it = instList[tid].begin(); it != instList[tid].end(); ++it) {
            if (shouldSquash(*it, tid)) {
                total_inst_to_squash++;
            }
        }
        unsigned num_uncommitted_inst =
            instList[tid].size() - total_inst_to_squash;

        dynSquashWidth =
            computeDynSquashWidth(num_uncommitted_inst, total_inst_to_squash);
    }

    if (!instList[tid].empty()) {
        InstIt tail_thread = instList[tid].end();
        tail_thread--;
        squashIt[tid] = tail_thread;

        // dont squash on current cycle
        // doSquash(tid);
    }
}

unsigned
ROB::computeDynSquashWidth(unsigned uncommitted_insts, unsigned to_squash)
{
    unsigned dyn_squash_width = 0;
    double expected_cycles;
    switch (robWalkPolicy) {
        case ROBWalkPolicy::Rollback:
            dyn_squash_width = rollbackWidth;
            DPRINTF(ROB, "Recovery with rollback, walk ROB with width %u\n", dyn_squash_width);
            break;

        case ROBWalkPolicy::Replay:
            expected_cycles =
                std::max(2.0, ((double)uncommitted_insts / replayWidth));
            dyn_squash_width = ceil((double)to_squash / expected_cycles);
            dyn_squash_width = std::max(dyn_squash_width, 1u);
            DPRINTF(
                ROB,
                "Recovery with replay, walk ROB with width %u in %f cycles\n",
                dyn_squash_width, expected_cycles);
            break;

        case ROBWalkPolicy::ConstCycle:
            dyn_squash_width = ceil((double) to_squash / (double) constSquashCycle);
            dyn_squash_width = std::max(dyn_squash_width, rollbackWidth);
            DPRINTF(ROB, "Recovery with const cycle, walk ROB with width %u\n", dyn_squash_width);
            break;

        default:
            break;
    }
    return dyn_squash_width;
}

unsigned
ROB::computeSnapshotSquashWidth(InstSeqNum squash_num, ThreadID tid)
{
    unsigned younger_before_squash = 0;   // work to squash (younger)
    unsigned older_after_snapshot = 0;    // residual walk from nearest older cp
    bool hit = false;

    for (auto it = instList[tid].begin(); it != instList[tid].end(); ++it) {
        unsigned w = robWalkByDestRegs ? (*it)->numDestRegs() : 1;
        if ((*it)->seqNum > squash_num) {
            younger_before_squash += w;
        } else if ((*it)->seqNum < squash_num) {
            older_after_snapshot =
                (*it)->isRatSnapshotted() ? 0 : older_after_snapshot + w;
        } else if ((*it)->isRatSnapshotted()) {
            hit = true;
        }
    }

    if (hit) {
        older_after_snapshot = 0;
        stats.robRatSnapshotHits++;
    }

    double expected_cycles =
        std::max(2.0, ((double)older_after_snapshot / replayWidth));
    unsigned dyn_squash_width = std::max(1u,
        (unsigned)ceil((double)younger_before_squash / expected_cycles));

    stats.snapshotSquashWidth.sample(dyn_squash_width);
    DPRINTF(ROB, "NaiveCpt squash [sn:%llu] hit=%d residual=%u toSquash=%u "
            "width=%u\n", squash_num, hit, older_after_snapshot,
            younger_before_squash, dyn_squash_width);
    return dyn_squash_width;
}

const DynInstPtr&
ROB::readHeadInst(ThreadID tid)
{
    if (headGroupSize(tid) != 0) {
        assert(instList[tid].size() > 0);
        InstIt head_thread = instList[tid].begin();

        assert((*head_thread)->isInROB());

        return *head_thread;
    } else {
        return dummyInst;
    }
}

DynInstPtr
ROB::readTailInst(ThreadID tid)
{
    InstIt tail_thread = instList[tid].end();
    tail_thread--;

    return *tail_thread;
}

ROB::ROBStats::ROBStats(statistics::Group *parent, unsigned group_limit)
  : statistics::Group(parent, "rob"),
    ADD_STAT(reads, statistics::units::Count::get(),
        "The number of ROB reads"),
    ADD_STAT(writes, statistics::units::Count::get(),
        "The number of ROB writes"),
    ADD_STAT(instPergroup, statistics::units::Count::get()),
    ADD_STAT(hybridAllocatedGroups, statistics::units::Count::get(),
        "Physical ROB groups allocated by successful Hybrid batches"),
    ADD_STAT(hybridDowngrades, statistics::units::Count::get(),
        "Hybrid entries downgraded to NORMAL after latter squash"),
    ADD_STAT(hybridAllocatedInsts, statistics::units::Count::get(),
        "DynInsts inserted by successful Hybrid batches, including wrong path"),
    ADD_STAT(hybridGroupType, statistics::units::Count::get(),
        "Final group types at successful Hybrid batch allocation"),
    ADD_STAT(hybridGroupLength, statistics::units::Count::get(),
        "Final DynInst group lengths at successful Hybrid batch allocation"),
    ADD_STAT(hybridAllocationCompressionRatio, statistics::units::Rate<
        statistics::units::Count, statistics::units::Count>::get(),
        "Successfully allocated Hybrid DynInsts per physical ROB group"),
    ADD_STAT(robRatSnapshotHits, statistics::units::Count::get(),
        "Squashes that landed exactly on a RAT checkpoint (NaiveCpt)"),
    ADD_STAT(snapshotSquashWidth, statistics::units::Count::get(),
        "Distribution of NaiveCpt dynamic squash width")
{
    instPergroup.init(0, 8, 1).flags(statistics::nozero);
    hybridGroupType.init(static_cast<unsigned>(HybridGroupType::NumTypes))
        .flags(statistics::pdf);
    hybridGroupType.subname(static_cast<unsigned>(HybridGroupType::NormalS),
                            "NORMAL-S");
    hybridGroupType.subname(static_cast<unsigned>(HybridGroupType::NormalC),
                            "NORMAL-C");
    hybridGroupType.subname(static_cast<unsigned>(HybridGroupType::NormalN),
                            "NORMAL-N");
    hybridGroupType.subname(static_cast<unsigned>(HybridGroupType::CC), "CC");
    hybridGroupType.subname(static_cast<unsigned>(HybridGroupType::CS), "CS");
    hybridGroupType.subname(static_cast<unsigned>(HybridGroupType::SC), "SC");
    hybridGroupLength.init(1, std::max(1U, group_limit), 1)
        .flags(statistics::pdf);
    hybridAllocationCompressionRatio =
        hybridAllocatedInsts / hybridAllocatedGroups;
    snapshotSquashWidth.init(0, 256, 6).flags(statistics::pdf);
}

DynInstPtr
ROB::findInst(ThreadID tid, InstSeqNum seqnum)
{
    for (InstIt it = instList[tid].begin(); it != instList[tid].end(); it++) {
        if ((*it)->seqNum == seqnum) {
            return *it;
        }
    }
    return NULL;
}

} // namespace o3
} // namespace gem5
