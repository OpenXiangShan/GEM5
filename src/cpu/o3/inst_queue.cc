/*
 * Copyright (c) 2011-2014, 2017-2020 ARM Limited
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

#include "cpu/o3/inst_queue.hh"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/issue_queue.hh"
#include "cpu/o3/limits.hh"
#include "debug/IQ.hh"
#include "debug/Counters.hh"
#include "debug/Schedule.hh"
#include "enums/OpClass.hh"
#include "params/BaseO3CPU.hh"
#include "sim/core.hh"
#include "sim/cur_tick.hh"

// clang complains about std::set being overloaded with Packet::set if
// we open up the entire namespace std
using std::list;

namespace gem5
{

namespace o3
{

InstructionQueue::FUCompletion::FUCompletion(const DynInstPtr &_inst,
    int fu_idx, InstructionQueue *iq_ptr)
    : Event(Stat_Event_Pri, AutoDelete),
      inst(_inst), fuIdx(fu_idx), iqPtr(iq_ptr), freeFU(false)
{
}

void
InstructionQueue::FUCompletion::process()
{
    iqPtr->processFUCompletion(inst, -1);
    inst = NULL;
}


const char *
InstructionQueue::FUCompletion::description() const
{
    return "Functional unit completion";
}

InstructionQueue::InstructionQueue(CPU *cpu_ptr, IEW *iew_ptr,
        const BaseO3CPUParams &params)
    : cpu(cpu_ptr),
      iewStage(iew_ptr),
      scheduler(params.scheduler),
      numThreads(params.numThreads),
      totalWidth(8),
      commitToIEWDelay(params.commitToIEWDelay),
      iqStats(cpu, totalWidth),
      iqIOStats(cpu)
{
    const auto &reg_classes = params.isa[0]->regClasses();
    // Set the number of total physical registers
    // As the vector registers have two addressing modes, they are added twice
    numPhysRegs = params.numPhysIntRegs + params.numPhysFloatRegs +
                    params.numPhysVecRegs +
                    params.numPhysVecRegs * (
                            reg_classes.at(VecElemClass).numRegs() /
                            reg_classes.at(VecRegClass).numRegs()) +
                    params.numPhysVecPredRegs +
                    params.numPhysCCRegs +
                    params.numPhysRMiscRegs;

    //Initialize Mem Dependence Units
    for (ThreadID tid = 0; tid < MaxThreads; tid++) {
        memDepUnit[tid].init(params, tid, cpu_ptr);
        memDepUnit[tid].setIQ(this);
    }

    scheduler->setCPU(cpu_ptr);
    scheduler->resetDepGraph(numPhysRegs);
    scheduler->setMemDepUnit(memDepUnit);

    resetState();
}

InstructionQueue::~InstructionQueue()
{
}

std::string
InstructionQueue::name() const
{
    return cpu->name() + ".iq";
}

InstructionQueue::IQStats::IQStats(CPU *cpu, const unsigned &total_width)
    : statistics::Group(cpu),
    ADD_STAT(instsAdded, statistics::units::Count::get(),
             "Number of instructions added to the IQ (excludes non-spec)"),
    ADD_STAT(nonSpecInstsAdded, statistics::units::Count::get(),
             "Number of non-speculative instructions added to the IQ"),
    ADD_STAT(instsIssued, statistics::units::Count::get(),
             "Number of instructions issued"),
    ADD_STAT(intInstsIssued, statistics::units::Count::get(),
             "Number of integer instructions issued"),
    ADD_STAT(floatInstsIssued, statistics::units::Count::get(),
             "Number of float instructions issued"),
    ADD_STAT(branchInstsIssued, statistics::units::Count::get(),
             "Number of branch instructions issued"),
    ADD_STAT(memInstsIssued, statistics::units::Count::get(),
             "Number of memory instructions issued"),
    ADD_STAT(miscInstsIssued, statistics::units::Count::get(),
             "Number of miscellaneous instructions issued"),
    ADD_STAT(squashedInstsIssued, statistics::units::Count::get(),
             "Number of squashed instructions issued"),
    ADD_STAT(squashedInstsExamined, statistics::units::Count::get(),
             "Number of squashed instructions iterated over during squash; "
             "mainly for profiling"),
    ADD_STAT(squashedOperandsExamined, statistics::units::Count::get(),
             "Number of squashed operands that are examined and possibly "
             "removed from graph"),
    ADD_STAT(squashedNonSpecRemoved, statistics::units::Count::get(),
             "Number of squashed non-spec instructions that were removed"),
    ADD_STAT(numIssuedDist, statistics::units::Count::get(),
             "Number of insts issued each cycle"),
    ADD_STAT(statIssuedInstType, statistics::units::Count::get(),
             "Number of instructions issued per FU type, per thread"),
    ADD_STAT(issueRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Cycle>::get(),
             "Inst issue rate", instsIssued / cpu->baseStats.numCycles),
    ADD_STAT(fuBusy, statistics::units::Count::get(), "FU busy when requested"),
    ADD_STAT(fuBusyRate, statistics::units::Rate<
                statistics::units::Count, statistics::units::Count>::get(),
             "FU busy rate (busy events/executed inst)")
{
    instsAdded
        .prereq(instsAdded);

    nonSpecInstsAdded
        .prereq(nonSpecInstsAdded);

    instsIssued
        .prereq(instsIssued);

    intInstsIssued
        .prereq(intInstsIssued);

    floatInstsIssued
        .prereq(floatInstsIssued);

    branchInstsIssued
        .prereq(branchInstsIssued);

    memInstsIssued
        .prereq(memInstsIssued);

    miscInstsIssued
        .prereq(miscInstsIssued);

    squashedInstsIssued
        .prereq(squashedInstsIssued);

    squashedInstsExamined
        .prereq(squashedInstsExamined);

    squashedOperandsExamined
        .prereq(squashedOperandsExamined);

    squashedNonSpecRemoved
        .prereq(squashedNonSpecRemoved);
/*
    queueResDist
        .init(Num_OpClasses, 0, 99, 2)
        .name(name() + ".IQ:residence:")
        .desc("cycles from dispatch to issue")
        .flags(total | pdf | cdf )
        ;
    for (int i = 0; i < Num_OpClasses; ++i) {
        queueResDist.subname(i, opClassStrings[i]);
    }
*/
    numIssuedDist
        .init(0,total_width,1)
        .flags(statistics::pdf)
        ;
/*
    dist_unissued
        .init(Num_OpClasses+2)
        .name(name() + ".unissued_cause")
        .desc("Reason ready instruction not issued")
        .flags(pdf | dist)
        ;
    for (int i=0; i < (Num_OpClasses + 2); ++i) {
        dist_unissued.subname(i, unissued_names[i]);
    }
*/
    statIssuedInstType
        .init(cpu->numThreads,enums::Num_OpClass)
        .flags(statistics::total | statistics::pdf | statistics::dist)
        ;
    statIssuedInstType.ysubnames(enums::OpClassStrings);

    //
    //  How long did instructions for a particular FU type wait prior to issue
    //
/*
    issueDelayDist
        .init(Num_OpClasses,0,99,2)
        .name(name() + ".")
        .desc("cycles from operands ready to issue")
        .flags(pdf | cdf)
        ;
    for (int i=0; i<Num_OpClasses; ++i) {
        std::stringstream subname;
        subname << opClassStrings[i] << "_delay";
        issueDelayDist.subname(i, subname.str());
    }
*/
    issueRate
        .flags(statistics::total)
        ;

    fuBusy
        .init(cpu->numThreads)
        .flags(statistics::total)
        ;

    fuBusyRate
        .flags(statistics::total)
        ;
    fuBusyRate = fuBusy / instsIssued;
}

InstructionQueue::IQIOStats::IQIOStats(statistics::Group *parent)
    : statistics::Group(parent),
    ADD_STAT(intInstQueueReads, statistics::units::Count::get(),
             "Number of integer instruction queue reads"),
    ADD_STAT(intInstQueueWrites, statistics::units::Count::get(),
             "Number of integer instruction queue writes"),
    ADD_STAT(intInstQueueWakeupAccesses, statistics::units::Count::get(),
             "Number of integer instruction queue wakeup accesses"),
    ADD_STAT(fpInstQueueReads, statistics::units::Count::get(),
             "Number of floating instruction queue reads"),
    ADD_STAT(fpInstQueueWrites, statistics::units::Count::get(),
             "Number of floating instruction queue writes"),
    ADD_STAT(fpInstQueueWakeupAccesses, statistics::units::Count::get(),
             "Number of floating instruction queue wakeup accesses"),
    ADD_STAT(vecInstQueueReads, statistics::units::Count::get(),
             "Number of vector instruction queue reads"),
    ADD_STAT(vecInstQueueWrites, statistics::units::Count::get(),
             "Number of vector instruction queue writes"),
    ADD_STAT(vecInstQueueWakeupAccesses, statistics::units::Count::get(),
             "Number of vector instruction queue wakeup accesses"),
    ADD_STAT(intAluAccesses, statistics::units::Count::get(),
             "Number of integer alu accesses"),
    ADD_STAT(fpAluAccesses, statistics::units::Count::get(),
             "Number of floating point alu accesses"),
    ADD_STAT(vecAluAccesses, statistics::units::Count::get(),
             "Number of vector alu accesses")
{
    using namespace statistics;
    intInstQueueReads
        .flags(total);

    intInstQueueWrites
        .flags(total);

    intInstQueueWakeupAccesses
        .flags(total);

    fpInstQueueReads
        .flags(total);

    fpInstQueueWrites
        .flags(total);

    fpInstQueueWakeupAccesses
        .flags(total);

    vecInstQueueReads
        .flags(total);

    vecInstQueueWrites
        .flags(total);

    vecInstQueueWakeupAccesses
        .flags(total);

    intAluAccesses
        .flags(total);

    fpAluAccesses
        .flags(total);

    vecAluAccesses
        .flags(total);
}

void
InstructionQueue::resetState()
{
    for (ThreadID tid = 0; tid < MaxThreads; ++tid) {
        squashedSeqNum[tid] = 0;
    }

    nonSpecInsts.clear();
    deferredMemInsts.clear();
    blockedMemInsts.clear();
    retryMemInsts.clear();
    wbOutstanding = 0;
}

void
InstructionQueue::setActiveThreads(list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;
}

void
InstructionQueue::setIssueToExecuteQueue(TimeBuffer<IssueStruct> *i2e_ptr)
{
      issueToExecuteQueue = i2e_ptr;
}

void
InstructionQueue::setScheduler(Scheduler* scheduler)
{
    this->scheduler = scheduler;
}

void
InstructionQueue::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    fromCommit = timeBuffer->getWire(-commitToIEWDelay);
}

bool
InstructionQueue::isDrained() const
{
    bool drained = scheduler->isDrained() &&
                   instsToExecute.empty() &&
                   wbOutstanding == 0;
    for (ThreadID tid = 0; tid < numThreads; ++tid)
        drained = drained && memDepUnit[tid].isDrained();

    return drained;
}

void
InstructionQueue::drainSanityCheck() const
{
    assert(instsToExecute.empty());
    for (ThreadID tid = 0; tid < numThreads; ++tid)
        memDepUnit[tid].drainSanityCheck();
}

void
InstructionQueue::takeOverFrom()
{
    resetState();
}

bool
InstructionQueue::isReady(const DynInstPtr& inst)
{
    return scheduler->ready(inst);
}

bool
InstructionQueue::isFull(const DynInstPtr& inst)
{
    return scheduler->full(inst);
}

bool
InstructionQueue::hasReadyInsts()
{
    return scheduler->hasReadyInsts();
}

void
InstructionQueue::insert(const DynInstPtr &new_inst)
{
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;
    } else {
        iqIOStats.intInstQueueWrites++;
    }
    // Make sure the instruction is valid
    assert(new_inst);

    DPRINTF(IQ, "Adding instruction [sn:%llu] PC %s to the IQ.\n",
            new_inst->seqNum, new_inst->pcState());

    scheduler->insert(new_inst);

    ++iqStats.instsAdded;
}

void
InstructionQueue::insertNonSpec(const DynInstPtr &new_inst)
{
    // @todo: Clean up this code; can do it by setting inst as unable
    // to issue, then calling normal insert on the inst.
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;
    } else {
        iqIOStats.intInstQueueWrites++;
    }

    assert(new_inst);

    scheduler->insertNonSpec(new_inst);
    nonSpecInsts[new_inst->seqNum] = new_inst;

    DPRINTF(Schedule, "nonSpecInsts size: %lu\n", nonSpecInsts.size());

    DPRINTF(IQ, "Adding non-speculative instruction [sn:%llu] PC %s "
            "to the IQ.\n",
            new_inst->seqNum, new_inst->pcState());

    // instList[new_inst->threadNumber].push_back(new_inst);

    ++iqStats.nonSpecInstsAdded;
}

void
InstructionQueue::insertBarrier(const DynInstPtr &barr_inst)
{
    memDepUnit[barr_inst->threadNumber].insertBarrier(barr_inst);

    insertNonSpec(barr_inst);
}

DynInstPtr
InstructionQueue::getInstToExecute()
{
    assert(!instsToExecute.empty());
    DynInstPtr inst = std::move(instsToExecute.front());
    instsToExecute.pop_front();
    if (inst->isFloating()) {
        iqIOStats.fpInstQueueReads++;
    } else if (inst->isVector()) {
        iqIOStats.vecInstQueueReads++;
    } else {
        iqIOStats.intInstQueueReads++;
    }
    return inst;
}

void
InstructionQueue::processFUCompletion(const DynInstPtr &inst, int fu_idx)
{
    DPRINTF(IQ, "Processing FU completion [sn:%llu]\n", inst->seqNum);
    assert(!cpu->switchedOut());
    // The CPU could have been sleeping until this op completed (*extremely*
    // long latency op).  Wake it if it was.  This may be overkill.
   --wbOutstanding;
    iewStage->wakeCPU();

    // @todo: Ensure that these FU Completions happen at the beginning
    // of a cycle, otherwise they could add too many instructions to
    // the queue.
    issueToExecuteQueue->access(0)->size++;
    instsToExecute.push_back(inst);
}

bool
InstructionQueue::execLatencyCheck(const DynInstPtr& inst, uint32_t& op_latency)
{
    // Leading zero count
    auto clz = [](RegVal val) {
#if defined(__GNUC__) || defined(__clang__)
        return val == 0 ? 64 : __builtin_clzll(val);
#else
        for (int i = 0; i < 64; i++) {
            if (val & (0x1lu << 63)) {
                return i;
            }
            val <<= 1;
        }
        return 64;
#endif
    };


    RegVal rs1;
    RegVal rs2;
    int delay_;
    switch (inst->opClass()) {
        case OpClass::IntDiv:
            rs1 = cpu->readArchIntReg(inst->srcRegIdx(0).index(),
                                      inst->threadNumber);
            rs2 = cpu->readArchIntReg(inst->srcRegIdx(1).index(),
                                      inst->threadNumber);
            // rs1 / rs2 : 0x80/0x8 ,delay_ = 4
            // get the leading zero difference between rs1 and rs2 (rs1 > rs2)
            delay_ = std::max(clz(rs2) - clz(rs1), 0);
            if (rs2 == 1) {
                // rs1 / 1 = rs1
                op_latency = 5;
            } else if (rs1 == rs2) {
                // rs1 / rs2 = 1 rem 0
                op_latency = 7;
            } else if (clz(rs2) - clz(rs1) < 0) {
                // if rs2 > rs1 then rs1/rs2 = 0 rem rs1
                op_latency = 5;
            } else {
                // base_latency + dynamic_latency
                // dynamic_latency determined by delay_
                op_latency = 7 + delay_ / 4;
            }
            return true;
        case OpClass::FloatSqrt:
            rs1 = cpu->readArchFloatReg(inst->srcRegIdx(0).index(),
                                        inst->threadNumber);
            rs2 = cpu->readArchFloatReg(inst->srcRegIdx(1).index(),
                                        inst->threadNumber);
            // for special values, fsqrt/fdiv early finish
            // such as nan, inf or rs1 == rs2
            switch (inst->staticInst->operWid()) {
                case 32:
                    if (__isnanf(*((float*)(&rs1))) ||
                        __isnanf(*((float*)(&rs2))) ||
                        __isinff(*((float*)(&rs1))) ||
                        __isinff(*((float*)(&rs2))) ||
                        (*((float*)(&rs2)) - 1.0f < 1e-6f)) {
                        op_latency = 2;
                        break;
                    }
                    op_latency = 10;
                    break;
                case 64:
                    if (__isnan(*((double*)(&rs1))) ||
                        __isnan(*((double*)(&rs2))) ||
                        __isinf(*((double*)(&rs1))) ||
                        __isinf(*((double*)(&rs2))) ||
                        (*((double*)(&rs2)) - 1.0 < 1e-15)) {
                        op_latency = 2;
                        break;
                    }
                    op_latency = 15;
                    break;
                default:
                    panic("Unsupported float width\n");
                    return false;
            }
            return true;
        case OpClass::FloatDiv:
            rs1 = cpu->readArchFloatReg(inst->srcRegIdx(0).index(),
                                        inst->threadNumber);
            rs2 = cpu->readArchFloatReg(inst->srcRegIdx(1).index(),
                                        inst->threadNumber);
            switch (inst->staticInst->operWid()) {
                case 32:
                    if (__isnanf(*((float*)(&rs1))) ||
                        __isnanf(*((float*)(&rs2))) ||
                        __isinff(*((float*)(&rs1))) ||
                        __isinff(*((float*)(&rs2))) ||
                        (*((float*)(&rs2)) - 1.0f < 1e-6f)) {
                        op_latency = 2;
                        break;
                    }
                    op_latency = 7;
                    break;
                case 64:
                    if (__isnan(*((double*)(&rs1))) ||
                        __isnan(*((double*)(&rs2))) ||
                        __isinf(*((double*)(&rs1))) ||
                        __isinf(*((double*)(&rs2))) ||
                        (*((double*)(&rs2)) - 1.0 < 1e-15)) {
                        op_latency = 2;
                        break;
                    }
                    op_latency = 12;
                    break;
                default:
                    panic("Unsupported float width\n");
                    return false;
            }
            return true;
        default:
            return false;
    }
    return false;
}

// @todo: Figure out a better way to remove the squashed items from the
// lists.  Checking the top item of each list to see if it's squashed
// wastes time and forces jumps.
void
InstructionQueue::scheduleReadyInsts()
{
    DPRINTF(IQ, "Attempting to schedule ready instructions from "
            "the IQ.\n");

    IssueStruct *i2e_info = issueToExecuteQueue->access(0);

    DynInstPtr mem_inst;
    while ((mem_inst = getDeferredMemInstToExecute())) {
        mem_inst->issueQue->retryMem(mem_inst);
    }

    // See if any cache blocked instructions are able to be executed
    while ((mem_inst = getBlockedMemInstToExecute())) {
        mem_inst->issueQue->retryMem(mem_inst);
    }

    // Have iterator to head of the list
    // While I haven't exceeded bandwidth or reached the end of the list,
    // Try to get a FU that can do what this op needs.
    // If successful, change the oldestInst to the new top of the list, put
    // the queue in the proper place in the list.
    // Increment the iterator.
    // This will avoid trying to schedule a certain op class if there are no
    // FUs that handle it.
    int total_issued = 0;
    DynInstPtr issued_inst;
    while ((issued_inst = scheduler->getInstToFU())) {

        if (issued_inst->isFloating()) {
            iqIOStats.fpInstQueueReads++;
        } else if (issued_inst->isVector()) {
            iqIOStats.vecInstQueueReads++;
        } else {
            iqIOStats.intInstQueueReads++;
        }

        if (issued_inst->isSquashed()) {
            ++iqStats.squashedInstsIssued;
            continue;
        }

        if (issued_inst->isFloating()) {
            iqIOStats.fpAluAccesses++;
        } else if (issued_inst->isVector()) {
            iqIOStats.vecAluAccesses++;
        } else {
            iqIOStats.intAluAccesses++;
        }

        uint32_t op_latency = scheduler->getOpLatency(issued_inst);
        execLatencyCheck(issued_inst, op_latency);
        assert(op_latency < 64);
        DPRINTF(Schedule, "[sn:%llu] start execute %u cycles\n", issued_inst->seqNum, op_latency);
        cpu->perfCCT->updateInstPos(issued_inst->seqNum, PerfRecord::AtFU);
        if (op_latency <= 1 || issued_inst->isLoad() || issued_inst->isStore()) {
            i2e_info->size++;
            instsToExecute.push_back(issued_inst);
        }
        else {
            ++wbOutstanding;
            FUCompletion *execution = new FUCompletion(issued_inst, 0, this);
            cpu->schedule(execution, cpu->clockEdge(Cycles(op_latency - 1))-1);
        }
        ++total_issued;
        if (issued_inst->firstIssue == -1) {
            issued_inst->firstIssue = curTick();
        }

        iqStats.statIssuedInstType[issued_inst->threadNumber][issued_inst->opClass()]++;
    }

    iqStats.numIssuedDist.sample(total_issued);
    iqStats.instsIssued+= total_issued;

    // If we issued any instructions, tell the CPU we had activity.
    // @todo If the way deferred memory instructions are handeled due to
    // translation changes then the deferredMemInsts condition should be
    // removed from the code below.
    if (total_issued || !retryMemInsts.empty() || !deferredMemInsts.empty()) {
        cpu->activityThisCycle();
    } else {
        DPRINTF(IQ, "Not able to schedule any instructions.\n");
    }
}

void
InstructionQueue::notifyExecuted(const DynInstPtr &inst)
{
    memDepUnit[inst->threadNumber].issue(inst);
}

void
InstructionQueue::scheduleNonSpec(const InstSeqNum &inst)
{
    DPRINTF(IQ, "Marking nonspeculative instruction [sn:%llu] as ready "
            "to execute.\n", inst);

    NonSpecMapIt inst_it = nonSpecInsts.find(inst);

    assert(inst_it != nonSpecInsts.end());

    ThreadID tid = (*inst_it).second->threadNumber;

    (*inst_it).second->setAtCommit();
    (*inst_it).second->setCanIssue();

    scheduler->addToFU((*inst_it).second);

    (*inst_it).second = NULL;

    nonSpecInsts.erase(inst_it);
}

void
InstructionQueue::commit(const InstSeqNum &inst, ThreadID tid)
{
    DPRINTF(IQ, "[tid:%i] Committing instructions older than [sn:%llu]\n",
            tid,inst);
    scheduler->doCommit(inst);
}

int
InstructionQueue::wakeDependents(const DynInstPtr &completed_inst)
{
    int dependents = 0;

    // The instruction queue here takes care of both floating and int ops
    if (completed_inst->isFloating()) {
        iqIOStats.fpInstQueueWakeupAccesses++;
    } else if (completed_inst->isVector()) {
        iqIOStats.vecInstQueueWakeupAccesses++;
    } else {
        iqIOStats.intInstQueueWakeupAccesses++;
    }

    completed_inst->lastWakeDependents = curTick();

    DPRINTF(IQ, "Waking dependents of completed instruction.\n");

    assert(!completed_inst->isSquashed());

    // Tell the memory dependence unit to wake any dependents on this
    // instruction if it is a memory instruction.  Also complete the memory
    // instruction at this point since we know it executed without issues.
    ThreadID tid = completed_inst->threadNumber;
    if (completed_inst->isMemRef()) {
        memDepUnit[tid].completeInst(completed_inst);

        DPRINTF(IQ, "Completing mem instruction PC: %s [sn:%llu]\n",
            completed_inst->pcState(), completed_inst->seqNum);

        completed_inst->memOpDone(true);
    } else if (completed_inst->isReadBarrier() ||
               completed_inst->isWriteBarrier()) {
        // Completes a non mem ref barrier
        memDepUnit[tid].completeInst(completed_inst);
    }

    for (int dest_reg_idx = 0;
         dest_reg_idx < completed_inst->numDestRegs();
         dest_reg_idx++)
    {
        PhysRegIdPtr dest_reg =
            completed_inst->renamedDestIdx(dest_reg_idx);

        // Special case of uniq or control registers.  They are not
        // handled by the IQ and thus have no dependency graph entry.
        if (dest_reg->isFixedMapping()) {
            DPRINTF(IQ, "Reg %d [%s] is part of a fix mapping, skipping\n",
                    dest_reg->index(), dest_reg->className());
            continue;
        }

        // Avoid waking up dependents if the register is pinned
        dest_reg->decrNumPinnedWritesToComplete();
        if (dest_reg->isPinned())
            completed_inst->setPinnedRegsWritten();

        if (dest_reg->getNumPinnedWritesToComplete() != 0) {
            DPRINTF(IQ, "Reg %d [%s] is pinned, skipping\n",
                    dest_reg->index(), dest_reg->className());
            continue;
        }

        DPRINTF(IQ, "Waking any dependents on register %i (%s).\n",
                dest_reg->index(),
                dest_reg->className());
    }

    return dependents;
}

void
InstructionQueue::rescheduleMemInst(const DynInstPtr &resched_inst)
{
    DPRINTF(IQ, "Rescheduling mem inst [sn:%llu]\n", resched_inst->seqNum);

    // Reset DTB translation state
    resched_inst->translationStarted(false);
    resched_inst->translationCompleted(false);

    resched_inst->clearCanIssue();
    memDepUnit[resched_inst->threadNumber].reschedule(resched_inst);
}

void
InstructionQueue::replayMemInst(const DynInstPtr &replay_inst)
{
    memDepUnit[replay_inst->threadNumber].replay();
}

void
InstructionQueue::deferMemInst(const DynInstPtr &deferred_inst)
{
    deferredMemInsts.push_back(deferred_inst);
}

void
InstructionQueue::blockMemInst(const DynInstPtr &blocked_inst)
{
    blocked_inst->clearCanIssue();
    blockedMemInsts.push_back(blocked_inst);
    DPRINTF(IQ, "Memory inst [sn:%llu] PC %s is blocked, will be "
            "reissued later\n", blocked_inst->seqNum,
            blocked_inst->pcState());
}

void
InstructionQueue::cacheUnblocked()
{
    DPRINTF(IQ, "Cache is unblocked, rescheduling blocked memory "
            "instructions\n");
    retryMemInsts.splice(retryMemInsts.end(), blockedMemInsts);
    // Get the CPU ticking again
    cpu->wakeCPU();
}

DynInstPtr
InstructionQueue::getDeferredMemInstToExecute()
{
    for (ListIt it = deferredMemInsts.begin(); it != deferredMemInsts.end();
         ++it) {
        if ((*it)->translationCompleted() || (*it)->isSquashed()) {
            DPRINTF(IQ, "Deferred mem inst [sn:%llu] PC %s is ready to "
                    "execute\n", (*it)->seqNum, (*it)->pcState());
            DynInstPtr mem_inst = std::move(*it);
            deferredMemInsts.erase(it);
            return mem_inst;
        }
        if (!(*it)->translationCompleted()) {
            DPRINTF(
                IQ,
                "Deferred mem inst [sn:%llu] PC %s has not been translated\n",
                (*it)->seqNum, (*it)->pcState());
        }
    }
    return nullptr;
}

DynInstPtr
InstructionQueue::getBlockedMemInstToExecute()
{
    if (retryMemInsts.empty()) {
        return nullptr;
    } else {
        DynInstPtr mem_inst = std::move(retryMemInsts.front());
        retryMemInsts.pop_front();
        return mem_inst;
    }
}

void
InstructionQueue::violation(const DynInstPtr &store,
        const DynInstPtr &faulting_load)
{
    iqIOStats.intInstQueueWrites++;
    memDepUnit[store->threadNumber].violation(store, faulting_load);
}

void
InstructionQueue::squash(ThreadID tid)
{
    DPRINTF(IQ, "[tid:%i] Starting to squash instructions in "
            "the IQ.\n", tid);

    // Read instruction sequence number of last instruction out of the
    // time buffer.
    squashedSeqNum[tid] = fromCommit->commitInfo[tid].doneSeqNum;

    doSquash(tid);

    // Also tell the memory dependence unit to squash.
    memDepUnit[tid].squash(squashedSeqNum[tid], tid);
}

void
InstructionQueue::doSquash(ThreadID tid)
{
    // // Start at the tail.
    // ListIt squash_it = instList[tid].end();
    // --squash_it;

    DPRINTF(IQ, "[tid:%i] Squashing until sequence number %i!\n",
            tid, squashedSeqNum[tid]);
    scheduler->doSquash(squashedSeqNum[tid]);

    for (auto it = nonSpecInsts.begin(); it != nonSpecInsts.end();) {
        if (it->first > squashedSeqNum[tid]) {
            auto& squashed_inst = it->second;
            if (!squashed_inst->isIssued() ||
                (squashed_inst->isMemRef() &&
                !squashed_inst->memOpDone())) {

                bool is_acq_rel = squashed_inst->isFullMemBarrier() &&
                            (squashed_inst->isLoad() ||
                            (squashed_inst->isStore() &&
                                !squashed_inst->isStoreConditional()));

                // Remove the instruction from the dependency list.
                if (is_acq_rel ||
                    (!squashed_inst->isNonSpeculative() &&
                    !squashed_inst->isStoreConditional() &&
                    !squashed_inst->isAtomic() &&
                    !squashed_inst->isReadBarrier() &&
                    !squashed_inst->isWriteBarrier())) {
                    it++;
                } else if (!squashed_inst->isStoreConditional() ||
                        !squashed_inst->isCompleted()) {

                    (*it).second = NULL;

                    it = nonSpecInsts.erase(it);

                    ++iqStats.squashedNonSpecRemoved;
                }
                else {
                    it++;
                }
            }
            else {
                it++;
            }
        }
        else {
            it++;
        }
    }
}

} // namespace o3
} // namespace gem5
