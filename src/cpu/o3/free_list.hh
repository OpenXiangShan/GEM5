/*
 * Copyright (c) 2016-2018 ARM Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder. You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#ifndef __CPU_O3_FREE_LIST_HH__
#define __CPU_O3_FREE_LIST_HH__

#include <algorithm>
#include <array>
#include <iostream>
#include <list>
#include <queue>

#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/regfile.hh"
#include "debug/FreeList.hh"
#include "enums/SMTQueuePolicy.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

/**
 * Free list for a single class of registers (e.g., integer
 * or floating point).  Because the register class is implicitly
 * determined by the rename map instance being accessed, all
 * architectural register index parameters and values in this class
 * are relative (e.g., %fp2 is just index 2).
 */
class SimpleFreeList
{
  private:

    /** The actual free list */
    std::queue<PhysRegIdPtr> freeRegs;

  public:

    SimpleFreeList() {};

    /** Add a physical register to the free list */
    void addReg(PhysRegIdPtr reg) { freeRegs.push(reg); }

    /** Add physical registers to the free list */
    template<class InputIt>
    void
    addRegs(InputIt first, InputIt last) {
        std::for_each(first, last, [this](typename InputIt::value_type& reg) {
            freeRegs.push(&reg);
        });
    }

    /** Get the next available register from the free list */
    PhysRegIdPtr getReg()
    {
        assert(!freeRegs.empty());
        PhysRegIdPtr free_reg = freeRegs.front();
        freeRegs.pop();
        DPRINTF(FreeList, "Allocate p%i (%#lx), next free: p%i (%#lx)\n",
                free_reg->flatIndex(), free_reg,
                freeRegs.empty() ? -1 : freeRegs.front()->flatIndex(),
                freeRegs.empty() ? nullptr : freeRegs.front());
        free_reg->incRef();
        return free_reg;
    }

    /** Return the number of free registers on the list. */
    unsigned numFreeRegs() const { return freeRegs.size(); }

    /** True iff there are free registers on the list. */
    bool hasFreeRegs() const { return !freeRegs.empty(); }
};


/**
 * FreeList class that simply holds the list of free integer and floating
 * point registers.  Can request for a free register of either type, and
 * also send back free registers of either type.  This is a very simple
 * class, but it should be sufficient for most implementations.  Like all
 * other classes, it assumes that the indices for the floating point
 * registers starts after the integer registers end.  Hence the variable
 * numPhysicalIntRegs is logically equivalent to the baseFP dependency.
 * Note that while this most likely should be called FreeList, the name
 * "FreeList" is used in a typedef within the CPU Policy, and therefore no
 * class can be named simply "FreeList".
 * @todo: Give a better name to the base FP dependency.
 */
class UnifiedFreeList
{
  private:

    /** The object name, for DPRINTF.  We have to declare this
     *  explicitly because Scoreboard is not a SimObject. */
    const std::string _name;

    std::array<SimpleFreeList, RMiscRegClass + 1> freeLists;

    /**
     * The register file object is used only to distinguish integer
     * from floating-point physical register indices.
     */
    PhysRegFile *regFile;

    /** SMT resource sharing policy for the Preg free lists. */
    SMTQueuePolicy pregPolicy;

    /** Percentage (0-100) of per-thread fair share that a donor thread
     *  reserves.  donorQuota(type) = numPhysRegs[type] / activeThreadCount
     *  * donorReservePercent / 100.  Automatically scales with thread count. */
    const unsigned donorReservePercent;

    /** Fixed per-thread base quota override (0 = use numPhysRegs/activeThreads). */
    const unsigned fixedBase;

    ThreadID numThreads;

    /** Pointer to the CPU's active-threads list, for DynamicBorrowing's
     *  active-thread-count-based base quota and for Partitioned's
     *  resetEntries(). */
    std::list<ThreadID> *activeThreads = nullptr;

    /** Total physical registers of each class, captured at construction
     *  time (before any allocation happens). */
    unsigned numPhysRegs[RMiscRegClass + 1] = {};

    /** Per-thread, per-class static cap used by the Partitioned policy. */
    unsigned maxEntries[MaxThreads][RMiscRegClass + 1] = {};

    /** Per-thread, per-class occupancy accounting. Needed because
     *  freeLists[] is a single shared queue per class with no notion of
     *  which thread holds which register. */
    unsigned threadUsed[MaxThreads][RMiscRegClass + 1] = {};

    /** Whether a thread may donate unused headroom this cycle.  Set by
     *  Rename::tick() every cycle via setBorrowingDonor() before any
     *  thread's canRename() is evaluated. */
    bool donor[MaxThreads] = {};

    unsigned activeThreadCount() const;

    /** Per-thread base quota for register class type. */
    unsigned base(RegClassType type) const;

    /** Reduced reserve quota used for a donor thread. */
    unsigned donorQuota(RegClassType type) const;

    /** Self-contained DynamicBorrowing limit for the given thread:
     *  numPhysRegs[type] - sum_active_other(max(used[other], reserve[other])).
     *  Only counts active threads to avoid deadlocking single-thread runs. */
    unsigned borrowingLimit(RegClassType type, ThreadID tid) const;

  public:
    /** Constructs a free list.
     *  @param _my_name Name of the free list, for DPRINTF.
     *  @param _regFile The register file, used to populate the free list.
     *  @param params CPU params, providing smtPregPolicy and friends.
     */
    UnifiedFreeList(const std::string &_my_name, PhysRegFile *_regFile,
                     const BaseO3CPUParams &params);

    /** Gives the name of the freelist. */
    std::string name() const { return _name; };

    /** Sets pointer to the list of active threads. */
    void
    setActiveThreads(std::list<ThreadID> *at_ptr)
    {
        activeThreads = at_ptr;
    }

    /** Marks/unmarks a thread as a borrowing donor.  Called by
     *  Rename::tick() every cycle for all threads before rename begins. */
    void
    setBorrowingDonor(ThreadID tid, bool val)
    {
        donor[tid] = val;
    }

    /** Recomputes the Partitioned policy's per-thread maxEntries when the
     *  active thread count changes. */
    void resetEntries();

    /** Gets a free register of type type for thread tid, and records the
     *  allocation against that thread's occupancy. */
    PhysRegIdPtr
    getReg(RegClassType type, ThreadID tid)
    {
        PhysRegIdPtr reg = freeLists[type].getReg();
        threadUsed[tid][type]++;
        return reg;
    }

    /** Adds a register back to the free list. Used only for the initial,
     *  thread-agnostic population of the free lists at construction time;
     *  does not touch threadUsed accounting. */
    template<class InputIt>
    void
    addRegs(InputIt first, InputIt last)
    {
        std::for_each(first, last, [this](auto &reg) { addReg(&reg); });
    }

    /** Adds a register back to the free list, without per-thread
     *  accounting. Only meant for the initial bulk population via
     *  addRegs() above.
     *  NOTE: freed_reg's refCnt must is 0
     */
    void
    addReg(PhysRegIdPtr freed_reg)
    {
        freeLists[freed_reg->classValue()].addReg(freed_reg);
    }

    /** Adds a register back to the free list on behalf of thread tid,
     *  releasing that thread's occupancy.
     *  NOTE: freed_reg's refCnt must is 0
     */
    void
    addReg(PhysRegIdPtr freed_reg, ThreadID tid)
    {
        assert(tid < MaxThreads &&
               threadUsed[tid][freed_reg->classValue()] > 0);
        threadUsed[tid][freed_reg->classValue()]--;
        freeLists[freed_reg->classValue()].addReg(freed_reg);
    }

    /** Resets per-thread occupancy accounting on thread removal.
     *  Called from CPU::removeThread() after the thread's mappings have
     *  been released. */
    void
    resetThreadUsed(ThreadID tid)
    {
        for (int i = 0; i <= RMiscRegClass; i++)
            threadUsed[tid][i] = 0;
        donor[tid] = false;
    }

    /** Checks if there are any free registers of type type. */
    bool
    hasFreeRegs(RegClassType type) const
    {
        return freeLists[type].hasFreeRegs();
    }

    /** Returns the number of free registers of type type. */
    unsigned
    numFreeRegs(RegClassType type) const
    {
        return freeLists[type].numFreeRegs();
    }

    /** Whether thread tid may allocate n more registers of type type
     *  under the configured smtPregPolicy. */
    bool canAllocate(RegClassType type, ThreadID tid, unsigned n = 1) const;

    /** Number of registers of type type thread tid may allocate right
     *  now under the configured smtPregPolicy (bounded by the number of
     *  physically free registers). Equivalent to the pre-SMT-partition
     *  numFreeRegs() when smtPregPolicy is Dynamic. */
    unsigned numAllocatable(RegClassType type, ThreadID tid) const;

    /** Returns true when thread tid cannot allocate n registers of type
     *  due to per-thread quota exhaustion (Partitioned or DynamicBorrowing),
     *  while the global free list still has sufficient free registers.
     *  Always returns false under the Dynamic policy. */
    bool isPerThreadExhausted(RegClassType type, ThreadID tid, unsigned n) const;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_FREE_LIST_HH__
