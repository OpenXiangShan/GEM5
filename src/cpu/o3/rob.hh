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

#ifndef __CPU_O3_ROB_HH__
#define __CPU_O3_ROB_HH__

#include <queue>
#include <string>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/reg_class.hh"
#include "enums/ROBCompressPolicy.hh"
#include "enums/ROBWalkPolicy.hh"
#include "enums/SMTQueuePolicy.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;

struct DerivO3CPUParams;

/**
 * ROB class.  The ROB is largely what drives squashing.
 */
class ROB
{
  public:
    typedef std::pair<RegIndex, RegIndex> UnmapInfo;
    typedef typename std::list<DynInstPtr>::iterator InstIt;

    /** Possible ROB statuses. */
    enum Status
    {
        Running,
        Idle,
        ROBSquashing
    };

  private:
    /** Per-thread ROB status. */
    Status robStatus[MaxThreads];

    /** ROB resource sharing policy for SMT mode. */
    SMTQueuePolicy robPolicy;

    ROBWalkPolicy robWalkPolicy;

    bool allocateGroup_none(const DynInstPtr inst, ThreadID tid);
    bool allocateGroup_kmhv2(const DynInstPtr inst, ThreadID tid);
    bool allocateGroup_MohBoE(const DynInstPtr inst, ThreadID tid);
    bool allocateGroup_kmhv3(const DynInstPtr inst, ThreadID tid);

  public:
    /** ROB constructor.
     *  @param _cpu   The cpu object pointer.
     *  @param params The cpu params including several ROB-specific parameters.
     */
    ROB(CPU *_cpu, const BaseO3CPUParams &params);

    std::string name() const;

    /** Sets pointer to the list of active threads.
     *  @param at_ptr Pointer to the list of active threads.
     */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Takes over another CPU's thread. */
    void takeOverFrom();

    /** Function to insert an instruction into the ROB. Note that whatever
     *  calls this function must ensure that there is enough space within the
     *  ROB for the new instruction.
     *  @param inst The instruction being inserted into the ROB.
     */
    void insertInst(const DynInstPtr &inst);

    /** Returns pointer to the head instruction within the ROB.  There is
     *  no guarantee as to the return value if the ROB is empty.
     *  @retval Pointer to the DynInst that is at the head of the ROB.
     */
//    DynInstPtr readHeadInst();

    /** Returns a pointer to the head instruction of a specific thread within
     *  the ROB.
     *  @return Pointer to the DynInst that is at the head of the ROB.
     */
    const DynInstPtr &readHeadInst(ThreadID tid);

    /** Returns a pointer to the instruction with the given sequence if it is
     *  in the ROB.
     */
    DynInstPtr findInst(ThreadID tid, InstSeqNum squash_inst);

    /** Returns pointer to the tail instruction within the ROB.  There is
     *  no guarantee as to the return value if the ROB is empty.
     *  @retval Pointer to the DynInst that is at the tail of the ROB.
     */
//    DynInstPtr readTailInst();

    /** Returns a pointer to the tail instruction of a specific thread within
     *  the ROB.
     *  @return Pointer to the DynInst that is at the tail of the ROB.
     */
    DynInstPtr readTailInst(ThreadID tid);

    /** Retires the head instruction, removing it from the ROB. */
//    void retireHead();

    /** Retires the head instruction of a specific thread, removing it from the
     *  ROB.
     */
    void retireHead(ThreadID tid);

    /** Is the oldest instruction across all threads ready. */
//    bool isHeadReady();

    /** Is the oldest group of instructions across a particular thread ready. */
    bool isHeadGroupReady(ThreadID tid);

    InstSeqNum getHeadGroupLastDoneSeq(ThreadID tid);

    /** Re-adjust ROB partitioning. */
    void resetEntries();

    /** Number of entries needed For 'num_threads' amount of threads. */
    int entryAmount(ThreadID num_threads);

    /** Returns the number of free entries in a specific ROB paritition. */
    unsigned numFreeEntries(ThreadID tid);

    /** Returns the maximum number of entries for a specific thread. */
    unsigned getMaxEntries(ThreadID tid)
    { return maxEntries[tid]; }

    /** Returns the number of entries being used by a specific thread. */
    unsigned getThreadEntries(ThreadID tid)
    { return threadGroups[tid].size(); }

    /** Returns if the ROB is full. */
    bool isFull()
    {
      for (int i =0;i<MaxThreads;i++) {
        if (isFull(i)) return true;
      }
      return false;
    }

    /** Returns if a specific thread's partition is full. */
    bool isFull(ThreadID tid)
    { return threadGroups[tid].size() == numEntries; }

    /** Returns if the ROB is empty. */
    bool isEmpty() const
    { return numInstsInROB == 0; }

    /** Returns if a specific thread's partition is empty. */
    bool isEmpty(ThreadID tid) const
    { return threadGroups[tid].size() == 0; }

    /** Executes the squash, marking squashed instructions. */
    void doSquash(ThreadID tid);

    /** Squashes all instructions younger than the given sequence number for
     *  the specific thread.
     */
    void squash(InstSeqNum squash_num, ThreadID tid);

    /** Updates the head instruction with the new oldest instruction. */
    void updateHead();

    /** Updates the tail instruction with the new youngest instruction. */
    void updateTail();

    /** Checks if the ROB is still in the process of squashing instructions.
     *  @retval Whether or not the ROB is done squashing.
     */
    bool isDoneSquashing(ThreadID tid) const
    { return doneSquashing[tid]; }

    /** Checks if the ROB is still in the process of squashing instructions for
     *  any thread.
     */
    bool isDoneSquashing();

    /** This is more of a debugging function than anything.  Use
     *  numInstsInROB to get the instructions in the ROB unless you are
     *  double checking that variable.
     */
    int countInsts();

    /** This is more of a debugging function than anything.  Use
     *  threadEntries to get the instructions in the ROB unless you are
     *  double checking that variable.
     */
    size_t countInsts(ThreadID tid);

    uint32_t countGroupAllInst(ThreadID tid) {
        int sum = 0;
        for (auto it : threadGroups[tid]) {
          assert(it);
          sum += it;
        }
        return sum;
    }

    uint32_t countInstsOfGroups(int groups);

    bool (ROB::*allocateNewGroup)(const DynInstPtr inst, ThreadID tid);

    void commitGroup(const DynInstPtr inst, ThreadID tid);

    void squashGroup(const DynInstPtr inst, ThreadID tid);

    auto& getInstList(ThreadID tid) { return instList[tid]; }

  private:
    /** Reset the ROB state */
    void resetState();

    /** Pointer to the CPU. */
    CPU *cpu;

    /** Active Threads in CPU */
    std::list<ThreadID> *activeThreads;

    /** Number of instructions in the ROB. */
    unsigned numEntries;

    unsigned instsPerGroup;

    std::deque<unsigned> threadGroups[MaxThreads];

    uint64_t lastInsertCycle = 0;

    /** Max Insts a Thread Can Have in the ROB */
    unsigned maxEntries[MaxThreads];

    /** ROB List of Instructions */
    std::list<DynInstPtr> instList[MaxThreads];

    /** Number of instructions that can be squashed in a single cycle. */
    unsigned rollbackWidth;

    unsigned replayWidth{6};

    unsigned dynSquashWidth{6};

    unsigned constSquashCycle{1};

    unsigned computeDynSquashWidth(unsigned uncommitted_insts, unsigned to_squash);

  public:
    /** Iterator pointing to the instruction which is the last instruction
     *  in the ROB.  This may at times be invalid (ie when the ROB is empty),
     *  however it should never be incorrect.
     */
    InstIt tail;

    /** Iterator pointing to the instruction which is the first instruction in
     *  in the ROB*/
    InstIt head;

  private:
    /** Iterator used for walking through the list of instructions when
     *  squashing.  Used so that there is persistent state between cycles;
     *  when squashing, the instructions are marked as squashed but not
     *  immediately removed, meaning the tail iterator remains the same before
     *  and after a squash.
     *  This will always be set to cpu->instList.end() if it is invalid.
     */
    InstIt squashIt[MaxThreads];

  public:
    /** Number of instructions in the ROB. */
    int numInstsInROB;

    /** Dummy instruction returned if there are no insts left. */
    DynInstPtr dummyInst;

  private:
    /** The sequence number of the squashed instruction. */
    InstSeqNum squashedSeqNum[MaxThreads];

    /** Is the ROB done squashing. */
    bool doneSquashing[MaxThreads];

    /** Number of active threads. */
    ThreadID numThreads;


    struct ROBStats : public statistics::Group
    {
        ROBStats(statistics::Group *parent);

        // The number of rob_reads
        statistics::Scalar reads;
        // The number of rob_writes
        statistics::Scalar writes;

        statistics::Distribution instPergroup;
    } stats;
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_ROB_HH__
