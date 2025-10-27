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

#include <list>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/limits.hh"
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

ROB::ROB(CPU *_cpu, const BaseO3CPUParams &params)
    : robPolicy(params.smtROBPolicy),
      robWalkPolicy(params.robWalkPolicy),
      cpu(_cpu),
      numEntries(params.numROBEntries),
      instsPerGroup(params.CROB_instPerGroup),
      rollbackWidth(params.squashWidth),
      replayWidth(params.replayWidth),
      constSquashCycle(params.ConstSquashCycle),
      numInstsInROB(0),
      numThreads(params.numThreads),
      stats(_cpu)
{
    //Figure out rob policy
    if (robPolicy == SMTQueuePolicy::Dynamic) {
        //Set Max Entries to Total ROB Capacity
        for (ThreadID tid = 0; tid < numThreads; tid++) {
            maxEntries[tid] = numEntries;
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
        //    || robWalkPolicy == ROBWalkPolicy::NaiveCpt
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
        squashIt[tid] = instList[tid].end();
        squashedSeqNum[tid] = 0;
        doneSquashing[tid] = true;
    }
    numInstsInROB = 0;

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
    } else {
        return 0;
    }
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
ROB::countInstsOfGroups(int groups)
{
    int sum = 0;
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        auto it = threadGroups[tid].begin();
        for (int i = 0; i < groups && it != threadGroups[tid].end(); i++, it++) {
            sum += *it;
        }
    }
    return sum;
}

void
ROB::commitGroup(const DynInstPtr inst, ThreadID tid)
{
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

    stats.writes++;

    DPRINTF(ROB, "Adding inst PC %s to the ROB.\n", inst->pcState());

    assert(numInstsInROB <= numEntries * instsPerGroup);

    ThreadID tid = inst->threadNumber;

    // allocate group
    bool alloc = (this->*allocateNewGroup)(inst, tid);
    lastInsertCycle = cpu->curCycle();
    if (alloc) {
        if (!threadGroups[tid].empty()) [[likely]] {
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
            threadGroups[tid].size());
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

    head_inst->clearInROB();
    head_inst->setCommitted();

    //Update "Global" Head of ROB
    updateHead();

    // @todo: A special case is needed if the instruction being
    // retired is the only instruction in the ROB; otherwise the tail
    // iterator will become invalidated.
    cpu->removeFrontInst(head_inst);
}

bool
ROB::isHeadGroupReady(ThreadID tid)
{
    stats.reads++;

    if (!threadGroups[tid].empty() && threadGroups[tid].front() != 0) {
        auto it = instList[tid].begin();

        for (int i = 0; i < threadGroups[tid].front(); i++, it++) {
            auto& inst = *it;
            // first inst must be readyToCommit
            if (!inst->readyToCommit()) {
                return false;
            }

            // if one group has barrier or non-speculative or fault
            // this group must be committed
            if (inst->readyToCommit() && (!inst->isExecuted() || inst->faulted())) {
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
    if (!threadGroups[tid].empty() && threadGroups[tid].front() != 0) {
        auto it = instList[tid].begin();
        InstSeqNum seqnum = 0;
        for (int i = 0; i < threadGroups[tid].front(); i++, it++) {
            auto& inst = *it;
            if (!inst->readyToCommit() || !inst->isExecuted() || inst->faulted()) {
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
    return maxEntries[tid] - threadGroups[tid].size();
}

void
ROB::doSquash(ThreadID tid)
{
    stats.writes++;
    DPRINTF(ROB, "[tid:%i] Squashing instructions until [sn:%llu].\n",
            tid, squashedSeqNum[tid]);

    assert(squashIt[tid] != instList[tid].end());

    if ((*squashIt[tid])->seqNum < squashedSeqNum[tid]) {
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
         (*squashIt[tid])->seqNum > squashedSeqNum[tid];
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

        auto prevIt = std::prev(squashIt[tid]);
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

            squashIt[tid] = instList[tid].end();

            doneSquashing[tid] = true;

            return;
        }

        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        if ((*squashIt[tid]) == (*tail_thread))
            robTailUpdate = true;

        instList[tid].erase(squashIt[tid]);

        squashIt[tid] = prevIt;
    }


    // Check if ROB is done squashing.
    if ((*squashIt[tid])->seqNum <= squashedSeqNum[tid]) {
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


void
ROB::squash(InstSeqNum squash_num, ThreadID tid)
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

    // TODO: find the number of instructions to squash and
    // the number of uncommited instructions
    unsigned total_inst_to_squash = 0;
    for (auto it = instList[tid].begin(); it != instList[tid].end(); ++it) {
        if ((*it)->seqNum > squash_num) {
            total_inst_to_squash++;
        }
    }
    unsigned num_uncommited_inst = instList[tid].size() - total_inst_to_squash;

    dynSquashWidth = computeDynSquashWidth(num_uncommited_inst, total_inst_to_squash);

    if (!instList[tid].empty()) {
        InstIt tail_thread = instList[tid].end();
        tail_thread--;

        squashIt[tid] = tail_thread;

        doSquash(tid);
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

const DynInstPtr&
ROB::readHeadInst(ThreadID tid)
{
    if (!threadGroups[tid].empty() && threadGroups[tid].front() != 0) {
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

ROB::ROBStats::ROBStats(statistics::Group *parent)
  : statistics::Group(parent, "rob"),
    ADD_STAT(reads, statistics::units::Count::get(),
        "The number of ROB reads"),
    ADD_STAT(writes, statistics::units::Count::get(),
        "The number of ROB writes"),
    ADD_STAT(instPergroup, statistics::units::Count::get())
{
    instPergroup.init(0, 8, 1).flags(statistics::nozero);
}

DynInstPtr
ROB::findInst(ThreadID tid, InstSeqNum squash_inst)
{
    for (InstIt it = instList[tid].begin(); it != instList[tid].end(); it++) {
        if ((*it)->seqNum == squash_inst) {
            return *it;
        }
    }
    return NULL;
}

} // namespace o3
} // namespace gem5
