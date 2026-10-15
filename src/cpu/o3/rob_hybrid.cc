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

#include "cpu/o3/rob_hybrid.hh"

#include <bitset>
#include <initializer_list>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/rob.hh"
#include "cpu/static_inst.hh"
#include "debug/ROB.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

namespace
{

struct HybridClassRule
{
    using FlagSet = std::bitset<StaticInst::Num_Flags>;

    ROB::HybridInstClass instClass;
    FlagSet flags;
    std::bitset<enums::Num_OpClass> opClasses;

    HybridClassRule(ROB::HybridInstClass inst_class,
                    std::initializer_list<StaticInst::Flags> inst_flags,
                    std::initializer_list<OpClass> op_classes)
        : instClass(inst_class)
    {
        for (const auto flag : inst_flags) {
            flags.set(flag);
        }
        for (const auto op_class : op_classes) {
            opClasses.set(op_class);
        }
    }

    bool
    matches(const FlagSet &inst_flags, OpClass op_class) const
    {
        return (inst_flags & flags).any() || opClasses[op_class];
    }
};

using InstClass = ROB::HybridInstClass;
using InstFlag = StaticInst::Flags;

// Rules match any listed flag or OpClass; the first match wins (N > C > S).
const HybridClassRule hybridClassRules[] = {
    {InstClass::NoCompress,
     {InstFlag::IsSerializing, InstFlag::IsSerializeBefore,
      InstFlag::IsSerializeAfter, InstFlag::IsNonSpeculative,
      InstFlag::IsSquashAfter, InstFlag::IsReadBarrier,
      InstFlag::IsWriteBarrier, InstFlag::IsAtomic,
      InstFlag::IsLoadReserved, InstFlag::IsStoreConditional,
      InstFlag::IsVector, InstFlag::IsMicroop, InstFlag::IsMacroop},
     {VectorConfigOp}},
    {InstClass::Complex,
     {InstFlag::IsLoad, InstFlag::IsStore, InstFlag::IsControl},
     {IntJpOp}},
    {InstClass::Simple,
     {},
     {IntAluOp, IntMultOp, IntDivOp, Int2FpOp, FloatAddOp, FloatMultOp,
      FloatMultAccOp, FloatDivOp, FloatSqrtOp, FloatCmpOp, FloatCvtOp,
      FloatMvOp, FloatMiscOp}},
};

} // anonymous namespace

ROB::HybridInstClass
ROB::classifyHybridInst(const DynInstPtr &inst) const
{
    if (inst->faulted()) {
        return HybridInstClass::NoCompress;
    }

    // Include dynamic serialization and trace control-flow metadata at enqueue.
    auto flags = inst->staticInst->getFlags();
    flags.set(StaticInst::IsSerializeBefore, inst->isSerializeBefore());
    flags.set(StaticInst::IsSerializeAfter, inst->isSerializeAfter());
    flags.set(StaticInst::IsControl, inst->isControl());
    const auto op_class = inst->opClass();

    for (const auto &rule : hybridClassRules) {
        if (rule.matches(flags, op_class)) {
            return rule.instClass;
        }
    }

    DPRINTF(ROB, "Hybrid N [sn:%llu]: uncovered decoded OpClass %s\n",
            inst->seqNum, enums::OpClassStrings[op_class]);
    return HybridInstClass::NoCompress;
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

void
ROB::validateHybridConfig(const BaseO3CPUParams &params) const
{
    fatal_if(params.numThreads != 1,
             "Hybrid CROB requires numThreads = 1");
    fatal_if(params.enable_loadFusion,
             "Hybrid CROB requires enable_loadFusion = False");
    fatal_if(params.enableConstantFolding ||
             params.enableMovImmElimination,
             "Hybrid CROB requires enableConstantFolding = False and "
             "enableMovImmElimination = False");
    fatal_if(instsPerGroup == 0,
             "Hybrid CROB requires CROB_instPerGroup > 0");
}

void
ROB::insertHybridMember(const DynInstPtr &inst, ThreadID tid)
{
    auto &entry = hybridEntries[tid].back();
    assert(entry.id == inst->hybridEntryId);
    auto &remaining = inst->hybridSlotIsFormer ?
        entry.formerRemaining : entry.latterRemaining;
    ++remaining;
    assert(entry.memberCount() <= instsPerGroup);
}

void
ROB::commitHybridMember(const DynInstPtr &inst, ThreadID tid)
{
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
}

void
ROB::squashHybridMember(const DynInstPtr &inst, ThreadID tid)
{
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

} // namespace o3
} // namespace gem5
