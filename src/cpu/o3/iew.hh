/*
 * Copyright (c) 2010-2012, 2014, 2019 ARM Limited
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

#ifndef __CPU_O3_IEW_HH__
#define __CPU_O3_IEW_HH__

#include <cstdint>
#include <deque>
#include <map>
#include <queue>
#include <set>
#include <vector>

#include <boost/circular_buffer.hpp>

#include "base/statistics.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/inst_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/lsq.hh"
#include "cpu/o3/rob.hh"
#include "cpu/o3/scoreboard.hh"
#include "cpu/timebuf.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "debug/IEW.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class FUPool;
class Scheduler;

/**
 * IEW handles both single threaded and SMT IEW
 * (issue/execute/writeback).  It handles the dispatching of
 * instructions to the LSQ/IQ as part of the issue stage, and has the
 * IQ try to issue instructions each cycle. The execute latency is
 * actually tied into the issue latency to allow the IQ to be able to
 * do back-to-back scheduling without having to speculatively schedule
 * instructions. This happens by having the IQ have access to the
 * functional units, and the IQ gets the execution latencies from the
 * FUs when it issues instructions. Instructions reach the execute
 * stage on the last cycle of their execution, which is when the IQ
 * knows to wake up any dependent instructions, allowing back to back
 * scheduling. The execute portion of IEW separates memory
 * instructions from non-memory instructions, either telling the LSQ
 * to execute the instruction, or executing the instruction directly.
 * The writeback portion of IEW completes the instructions by waking
 * up any dependents, and marking the register ready on the
 * scoreboard.
 */
class IEW
{
  public:
    /** Overall IEW stage status. Used to determine if the CPU can
     * deschedule itself due to a lack of activity.
     */
    enum Status
    {
        Active,
        Inactive
    };

    /** Status for Issue, Execute, and Writeback stages. */
    enum StageStatus
    {
        Running,
        Blocked,
        Idle,
        StartSquash,
        Squashing,
        Unblocking
    };

    enum StallEvent
    {
        CacheMiss=0,
        Translation,
        ROBWalk,
        IQFull,
        LSQFull,
        DispBWFull,
        StallEventCount
    };

    enum DQType
    {
        IntDQ,
        FVDQ,
        MemDQ,
        NumDQ
    };

  private:

    /** The dispatch queue capacity */
    std::vector<uint32_t> dqSize;

    /** Overall stage status. */
    Status _status;
    /** Execute status. */
    StageStatus exeStatus;
    /** Writeback status. */
    StageStatus wbStatus;

    bool serializeOnNextInst[MaxThreads];

    /** Probe points. */
    ProbePointArg<DynInstPtr> *ppMispredict;
    ProbePointArg<DynInstPtr> *ppDispatch;
    /** To probe when instruction execution begins. */
    ProbePointArg<DynInstPtr> *ppExecute;
    /** To probe when instruction execution is complete. */
    ProbePointArg<DynInstPtr> *ppToCommit;

    StallSignals* stallSig;
    StallSignalBank* stallSignalBank = nullptr;

  public:
    /** Constructs a IEW with the given parameters. */
    IEW(CPU *_cpu, const BaseO3CPUParams &params);

    /** Returns the name of the IEW stage. */
    std::string name() const;

    /** Registers probes. */
    void regProbePoints();

    /** Initializes stage; sends back the number of free IQ and LSQ entries. */
    void startupStage();

    /** Clear all thread-specific states */
    void clearStates(ThreadID tid);

    /** Sets main time buffer used for backwards communication. */
    void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr);

    /** Sets time buffer for getting instructions coming from rename. */
    void setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr);

    /** Sets time buffer to pass on instructions to commit. */
    void setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr);

    /** Sets pointer to list of active threads. */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Sets pointer to the scoreboard. */
    void setScoreboard(Scoreboard *sb_ptr);

    /** Wakeup depandents of value predicted load inst. */
    void lvpWakeDependents(const DynInstPtr &inst);

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Has the stage drained? */
    bool isDrained() const;

    /** Takes over from another CPU's thread. */
    void takeOverFrom();

    /** Squashes instructions in IEW for a specific thread. */
    void squash(ThreadID tid, const TimeStruct *commit_input);

    /** Wakes all dependents of a completed instruction. */
    void wakeDependents(const DynInstPtr &inst);

    /** Tells memory dependence unit that a memory instruction needs to be
     * rescheduled. It will re-execute once replayMemInst() is called.
     */
    void rescheduleMemInst(const DynInstPtr &inst);

    /** Re-executes all rescheduled memory instructions. */
    void replayMemInst(const DynInstPtr &inst);

    /** Moves memory instruction onto the list of cache blocked instructions */
    void blockMemInst(const DynInstPtr &inst);

    /** Moves load instruction onto the Set of cache missed instructions */
    void cacheMissLdReplay(const DynInstPtr &inst);

    /** Notifies that the cache has become unblocked */
    void cacheUnblocked();

    /** Inst is ready to finish (The last cycle in FU) */
    void readyToFinish(const DynInstPtr &inst);

    /** Updates overall IEW status based on all of the stages' statuses. */
    void updateActivate();

    /** Resets entries of the IQ and the LSQ. */
    void resetEntries();

    /** Tells the CPU to wakeup if it has descheduled itself due to no
     * activity. Used mainly by the LdWritebackEvent.
     */
    void wakeCPU();

    /** Reports to the CPU that there is activity this cycle. */
    void activityThisCycle();

    /** Tells CPU that the IEW stage is active and running. */
    void activateStage();

    /** Tells CPU that the IEW stage is inactive and idle. */
    void deactivateStage();

    /** Returns if the LSQ has any stores to writeback. */
    bool hasStoresToWB() { return ldstQueue.hasStoresToWB(); }

    /** Returns if the LSQ has any stores to writeback. */
    bool hasStoresToWB(ThreadID tid) { return ldstQueue.hasStoresToWB(tid); }

    /** Just set the relevant flag in lsq and at the appropriate
     * time, lsq will attempt to write the data in the store buffer
     * back to the cache. returns true if there is no data in either
     * the store queue or the store buffer to write back to.
     */
    bool flushStores(ThreadID tid) { return ldstQueue.flushStores(tid); }

    /** Check if we need to squash after a load/store/branch is executed. */
    void SquashCheckAfterExe(DynInstPtr inst);

    /** notify the mem_dep_unit */
    void notifyExecuted(const DynInstPtr &inst) { instQueue.notifyExecuted(inst); }

    /**
     * Defers a memory instruction when its DTB translation incurs a hw
     * page table walk.
     */
    void deferMemInst(const DynInstPtr &deferred_inst) { instQueue.deferMemInst(deferred_inst); }

    /** Check misprediction  */
    void checkMisprediction(const DynInstPtr &inst);

    // hardware transactional memory
    // For debugging purposes, it is useful to keep track of the most recent
    // htmUid that has been committed (architecturally, not transactionally)
    // to ensure that the core and the memory subsystem are observing
    // correct ordering constraints.
    void setLastRetiredHtmUid(ThreadID tid, uint64_t htmUid)
    {
        ldstQueue.setLastRetiredHtmUid(tid, htmUid);
    }

    // if load tlb miss or cache miss
    void loadCancel(const DynInstPtr &inst);

    void stlfFailLdReplay(const DynInstPtr &inst, const InstSeqNum &store_seq_num);

    void mdpAddrReplayRegister(const DynInstPtr &inst,
                               const std::vector<InstSeqNum> &store_seq_nums);
    void mdpAddrReplayRegisterStrict(const DynInstPtr &inst,
                                     size_t required_store_completed_idx);
    void mdpAddrReplayPipeDone(const DynInstPtr &inst);
    void mdpAddrReplayUpdateStoreCompletedIdx(ThreadID tid,
                                              size_t store_completed_idx);

    uint32_t getIQInsts();

    void setStallSignals(StallSignals* stall_signals) { stallSig = stall_signals; }
    void setStallSignalBank(StallSignalBank* bank) { stallSignalBank = bank; }

    enum class FuturePreviewSkipReason : uint8_t
    {
        ActiveDispatch,
        MultipleActive,
        NumReasons,
    };

    enum class FutureActiveDispatchSource : uint8_t
    {
        ExistingFixedBuffer,
        RenameInput,
        Mixed,
        Unknown,
        NumSources,
    };

    enum class FutureActiveDispatchMode : uint8_t
    {
        DirectIssue,
        DispatchQueue,
        NumModes,
    };

    enum class FutureActiveDispatchPreviewOutcome : uint8_t
    {
        Skipped,
        DrainedNoResource,
        DrainedWithResources,
        BlockedWithResources,
        NumOutcomes,
    };

    enum class FutureActiveDispatchPreviewBlockReason : uint8_t
    {
        BuildInputFailed,
        InvalidPreview,
        UnsupportedTokens,
        SerializeBlocked,
        LQFull,
        SQFull,
        SchedulerNotReady,
        NumReasons,
    };

    enum class FutureDispatchSchedulerBlockReason : uint8_t
    {
        NoBlock,
        InvalidState,
        InvalidOp,
        InvalidDispSeq,
        InvalidSelector,
        ReplayBlocked,
        IQFull,
        InportFull,
        NumReasons,
    };

    struct IEWPrepareResult
    {
        Cycles cycle = Cycles(0);
        ThreadID selectedTid = InvalidThreadID;
        unsigned activeThreads = 0;
        unsigned blockedThreads = 0;
        bool multipleActive = false;
        bool commitBlock[MaxThreads] = {};
        bool ldstBlock[MaxThreads] = {};
        bool block[MaxThreads] = {};
        bool active[MaxThreads] = {};
        bool renameBlock[MaxThreads] = {};
        StallReason renameBlockReason[MaxThreads] = {};
    };

    struct IEWPrepareInput
    {
        Cycles cycle = Cycles(0);
        ThreadID numThreads = 0;
        bool fixedbufferEmpty[MaxThreads] = {};
        unsigned fixedbufferSize[MaxThreads] = {};
        unsigned fixedbufferSquashedInsts[MaxThreads] = {};
        unsigned renameInputInsts[MaxThreads] = {};
        bool ldstCanInsert[MaxThreads] = {};
        StallReason ldstBlockReason[MaxThreads] = {};
        bool dispatchStageEnabled = false;
        StallSignalLatch commitToIEW;
    };

    static constexpr unsigned MaxFutureDispatchPreviewEntries = MaxWidth * 2;

    struct FutureDispatchPreviewEntry
    {
        bool valid = false;
        bool squashed = false;
        bool splitStoreAddr = false;
        bool atomic = false;
        bool load = false;
        bool store = false;
        bool storeConditional = false;
        bool readBarrier = false;
        bool writeBarrier = false;
        bool nop = false;
        bool eliminated = false;
        bool nonSpeculative = false;
        bool serializeBefore = false;
        bool serializeAfter = false;
        OpClass opClass = No_OpClass;
        InstSeqNum seqNum = 0;
    };

    struct FutureDispatchPreviewInput
    {
        bool valid = false;
        Cycles cycle = Cycles(0);
        ThreadID tid = InvalidThreadID;
        unsigned entries = 0;
        unsigned freeLQEntries = 0;
        unsigned freeSQEntries = 0;
        bool serializeNext = false;
        InstSeqNum robHeadSeqNum = 0;
        FutureDispatchPreviewEntry insts[
            MaxFutureDispatchPreviewEntries];
    };

    struct FutureDispatchCandidateProfile
    {
        bool valid = false;
        bool drained = false;
        unsigned fixedBufferPops = 0;
        unsigned dispatchedBeforeBlock = 0;
        FutureActiveDispatchPreviewBlockReason blockReason =
            FutureActiveDispatchPreviewBlockReason::NumReasons;
        FutureDispatchSchedulerBlockReason schedulerBlockReason =
            FutureDispatchSchedulerBlockReason::NumReasons;
    };

    bool previewFutureRenameLatch(Cycles cycle,
                                  const StallSignalLatch &commit_to_iew,
                                  const RenameStruct *snapshot_rename,
                                  const TimeStruct *snapshot_commit,
                                  StallSignalLatch &iew_to_rename,
                                  IEWPrepareResult *prepare_result = nullptr);
    bool buildFutureRenameLatchInput(
            Cycles cycle, const StallSignalLatch &commit_to_iew,
            const RenameStruct *snapshot_rename,
            const TimeStruct *snapshot_commit,
            IEWPrepareInput &input);
    bool previewFutureRenameLatch(
            const IEWPrepareInput &input,
            StallSignalLatch &iew_to_rename,
            IEWPrepareResult *prepare_result = nullptr) const;
    bool previewFutureRenameLatch(
            const IEWPrepareInput &input,
            const RenameStruct *snapshot_rename,
            const TimeStruct *snapshot_commit,
            StallSignalLatch &iew_to_rename,
            IEWPrepareResult *prepare_result = nullptr,
            FutureActiveDispatchPreviewOutcome *dispatch_outcome = nullptr,
            FutureActiveDispatchPreviewBlockReason *dispatch_block_reason =
                nullptr,
            FutureDispatchCandidateProfile *dispatch_profile = nullptr);
    IEWPrepareResult previewFuturePrepare(
            const IEWPrepareInput &input) const;
    static FuturePreviewSkipReason futurePreviewSkipReason(
            const IEWPrepareResult &result);
    static FutureActiveDispatchSource futureActiveDispatchSource(
            const IEWPrepareInput &input, const IEWPrepareResult &result);
    static FutureActiveDispatchMode futureActiveDispatchMode(
            const IEWPrepareInput &input);
    static bool futureActiveDispatchDrainsWithoutResources(
            const IEWPrepareInput &input, const IEWPrepareResult &result);
    bool buildFutureDispatchPreviewInput(
            const IEWPrepareInput &input,
            const IEWPrepareResult &result,
            const RenameStruct *snapshot_rename,
            const TimeStruct *snapshot_commit,
            FutureDispatchPreviewInput &preview_input);
    void recordFuturePrepareProbe();
    void recordFuturePrepareSkipped();
    void recordFuturePreviewSkipped(FuturePreviewSkipReason reason);
    void recordFutureActiveDispatchPreviewSkipped(
            const IEWPrepareInput &input, const IEWPrepareResult &result,
            FutureActiveDispatchPreviewBlockReason block_reason =
                FutureActiveDispatchPreviewBlockReason::NumReasons);
    void recordFutureActiveDispatchPreviewAccepted(
            const IEWPrepareInput &input, const IEWPrepareResult &result,
            FutureActiveDispatchPreviewOutcome outcome);
    void setPendingFuturePrepare(const IEWPrepareResult &result);

  private:
    struct IEWThreadPrepareResult
    {
        Cycles cycle = Cycles(0);
        ThreadID tid = InvalidThreadID;
        bool commitBlock = false;
        bool ldstBlock = false;
        bool block = false;
        bool active = false;
        bool blocked = false;
        bool renameBlock = false;
        StallReason renameBlockReason = StallReason::NoStall;
    };

    struct IEWThreadPrepareResults
    {
        IEWThreadPrepareResult byThread[MaxThreads];
    };

    struct DispatchHeadSnapshot
    {
        bool valid = false;
        bool squashed = false;
        bool committed = false;
        bool atomic = false;
        bool storeConditional = false;
        bool load = false;
        bool store = false;
        bool vector = false;
        bool nonSpeculative = false;
        bool readyToIssue = false;
        bool issued = false;
        bool translationStarted = false;
        bool translationCompleted = false;
        bool hasPendingCacheReq = false;
        bool readyTickUnset = false;
        bool firstIssueSet = false;
        int pendingCacheDepth = -1;
    };

    struct DispatchStatusPrepareInput
    {
        Cycles cycle = Cycles(0);
        ThreadID tid = InvalidThreadID;
        bool lqEmpty = true;
        bool sqEmpty = true;
        bool lqFull = false;
        bool sqFull = false;
        DispatchHeadSnapshot robHead;
        DispatchHeadSnapshot lqHead;
        DispatchHeadSnapshot sqHead;
    };

    struct DispatchStatusPrepareResult
    {
        Cycles cycle = Cycles(0);
        ThreadID tid = InvalidThreadID;
        StallReason robHeadStallReason = StallReason::NoStall;
        StallReason lqHeadStallReason = StallReason::NoStall;
        StallReason sqHeadStallReason = StallReason::NoStall;
    };

    struct DispatchDrainPreviewResult
    {
        struct OutputSnapshot
        {
            unsigned fixedBufferPops = 0;
            unsigned squashedPops = 0;
            unsigned iqInserts = 0;
            unsigned lqInserts = 0;
            unsigned sqInserts = 0;
            unsigned nonSpecInserts = 0;
            unsigned barrierInserts = 0;
            unsigned producerAdds = 0;
        };

        struct BlockTokenSnapshot
        {
            bool valid = false;
            FutureDispatchSchedulerBlockReason reason =
                FutureDispatchSchedulerBlockReason::NumReasons;
            int iqIndex = -1;
            int selector = -1;
            OpClass opClass = No_OpClass;
            int dispSeq = -1;
            int freeEntries = 0;
            int freeInports = 0;
            bool replayBlocked = false;
        };

        bool valid = false;
        Cycles cycle = Cycles(0);
        ThreadID tid = InvalidThreadID;
        unsigned visibleInsts = 0;
        unsigned dispatchedBeforeBlock = 0;
        OutputSnapshot output;
        bool drained = false;
        StallReason stallReason = StallReason::NoStall;
        FutureActiveDispatchPreviewBlockReason blockReason =
            FutureActiveDispatchPreviewBlockReason::NumReasons;
        FutureDispatchSchedulerBlockReason schedulerBlockReason =
            FutureDispatchSchedulerBlockReason::NumReasons;
        BlockTokenSnapshot schedulerBlockToken;
    };

    struct PendingFutureDispatchPreview
    {
        bool valid = false;
        DispatchDrainPreviewResult result;
    };

    struct PendingFutureRenameLatchPreview
    {
        bool valid = false;
        Cycles cycle = Cycles(0);
        StallSignalLatch latch;
    };

    struct WritebackPrepareInput
    {
        struct Entry
        {
            bool valid = false;
            ThreadID tid = InvalidThreadID;
            bool loadWithSavedRequest = false;
            bool wakeEligible = false;
        };

        Cycles cycle = Cycles(0);
        unsigned width = 0;
        unsigned validInsts = 0;
        Entry entries[MaxWidth];
    };

    struct WritebackPrepareResult
    {
        Cycles cycle = Cycles(0);
        unsigned validInsts = 0;
        ThreadID tid[MaxWidth] = {};
        bool loadWithSavedRequest[MaxWidth] = {};
        bool wakeEligible[MaxWidth] = {};
        uint64_t instsToCommit[MaxThreads] = {};
    };

    void setRenameStall(ThreadID tid, bool block, StallReason reason);
    void setRenameBlock(ThreadID tid, bool block);
    IEWPrepareInput buildIEWPrepareInput(
            Cycles cycle,
            const StallSignalLatch *commit_to_iew_override = nullptr,
            const RenameStruct *snapshot_rename = nullptr,
            bool reset_lsq_pop_entries = true);
    IEWThreadPrepareResult prepareIEWThreadControl(
            const IEWPrepareInput &input, ThreadID tid) const;
    IEWPrepareResult combineIEWThreadPrepareResults(
            const IEWPrepareInput &input,
            const IEWThreadPrepareResults &thread_results) const;
    IEWPrepareResult prepareIEWControl(const IEWPrepareInput &input) const;
    IEWPrepareResult runIEWPrepare(Cycles cycle);
    void mergeIEWPrepareResult(const IEWPrepareResult &result,
                               bool countPrepareStats);
    bool samePrepareResult(const IEWPrepareResult &lhs,
                           const IEWPrepareResult &rhs) const;
    DispatchHeadSnapshot snapshotDispatchHead(
            const DynInstPtr &inst) const;
    DispatchStatusPrepareInput buildDispatchStatusPrepareInput(
            Cycles cycle, ThreadID tid);
    StallReason checkLoadStoreSnapshot(
            const DispatchHeadSnapshot &inst) const;
    StallReason checkDispatchHeadSnapshot(
            const DispatchStatusPrepareInput &input) const;
    DispatchStatusPrepareResult prepareDispatchStatusControl(
            const DispatchStatusPrepareInput &input) const;
    DispatchStatusPrepareResult runDispatchStatusPrepare(
            Cycles cycle, ThreadID tid);
    void verifyDispatchStatusPrepareResult(
            ThreadID tid, const DispatchStatusPrepareResult &result);
    DispatchDrainPreviewResult previewDirectDispatchDrain(
            Cycles cycle, ThreadID tid, const TimeStruct *commit_input);
    DispatchDrainPreviewResult previewFutureDirectDispatchDrain(
            const FutureDispatchPreviewInput &input) const;
    void verifyDirectDispatchDrainPreview(
            const DispatchDrainPreviewResult &result, ThreadID tid);
    void verifyDirectDispatchOutputSnapshot(
            const DispatchDrainPreviewResult &result,
            const DispatchDrainPreviewResult::OutputSnapshot &actual,
            ThreadID tid);
    void setPendingFutureDispatchPreview(
            const DispatchDrainPreviewResult &result);
    void setPendingFutureRenameLatchPreview(
            Cycles cycle, const StallSignalLatch &latch);
    void verifyPendingFutureRenameLatchPreview(
            unsigned dispatch_publishability_reason);
    bool sameRenameLatchPreview(const StallSignalLatch &lhs,
                                const StallSignalLatch &rhs) const;
    void recordRenameLatchPreviewDifferences(
            const StallSignalLatch &expected,
            const StallSignalLatch &actual);
    static bool sameDispatchOutputSnapshot(
            const DispatchDrainPreviewResult::OutputSnapshot &lhs,
            const DispatchDrainPreviewResult::OutputSnapshot &rhs);
    void recordDispatchOutputSnapshotFieldDifferences(
            const DispatchDrainPreviewResult::OutputSnapshot &expected,
            const DispatchDrainPreviewResult::OutputSnapshot &actual,
            statistics::Vector &fields);
    unsigned futureDispatchOutputPublishabilityReason(
            const DispatchDrainPreviewResult &expected,
            const DispatchDrainPreviewResult *actual) const;
    void recordFutureDispatchOutputPublishability(
            const DispatchDrainPreviewResult &expected,
            const DispatchDrainPreviewResult *actual);
    void recordFutureDispatchBlockTokenCheck(
            const DispatchDrainPreviewResult &expected,
            const DispatchDrainPreviewResult *actual,
            unsigned dispatch_publishability_reason);
    static bool sameDispatchBlockTokenSnapshot(
            const DispatchDrainPreviewResult::BlockTokenSnapshot &lhs,
            const DispatchDrainPreviewResult::BlockTokenSnapshot &rhs);
    void recordDispatchBlockTokenDifferenceFields(
            const DispatchDrainPreviewResult::BlockTokenSnapshot &expected,
            const DispatchDrainPreviewResult::BlockTokenSnapshot &actual);
    void verifyPendingFutureDispatchPreview(
            const DispatchDrainPreviewResult *actual);
    bool sameDispatchDrainPreview(
            const DispatchDrainPreviewResult &lhs,
            const DispatchDrainPreviewResult &rhs) const;
    unsigned futureDispatchPreviewDifferenceReason(
            const DispatchDrainPreviewResult &expected,
            const DispatchDrainPreviewResult *actual) const;
    WritebackPrepareInput buildWritebackPrepareInput(Cycles cycle) const;
    WritebackPrepareResult prepareWritebackControl(
            const WritebackPrepareInput &input) const;
    WritebackPrepareResult runWritebackPrepare(Cycles cycle);
    void verifyWritebackPrepareResult(
            const WritebackPrepareResult &result);
    const RenameStruct *renameInput(Cycles cycle) const;
    const TimeStruct *commitInput(Cycles cycle) const;

    struct PendingFuturePrepare
    {
        bool valid = false;
        IEWPrepareResult result;
    };

    PendingFuturePrepare pendingFuturePrepare;
    PendingFutureDispatchPreview pendingFutureDispatchPreview;
    PendingFutureRenameLatchPreview pendingFutureRenameLatchPreview;

    /** Sends commit proper information for a squash due to a branch
     * mispredict.
     */
    void squashDueToBranch(const DynInstPtr &inst, ThreadID tid);

    /** Sends commit proper information for a squash due to a memory order
     * violation.
     */
    void squashDueToMemOrder(const DynInstPtr &inst, ThreadID tid);

    /** Sends commit proper information for a squash due to a value
     * mispredict.
     */
    void squashDueToValuePrediction(const DynInstPtr &inst, ThreadID tid);

    bool canInsertLDSTQue(ThreadID tid,
                          bool reset_lsq_pop_entries = true);

    /** Dispatches instructions to IQ and LSQ. */
    void dispatchInsts(const RenameStruct *rename_input,
                       const TimeStruct *commit_input);

    DispatchDrainPreviewResult dispatchInstFromRename(
            ThreadID tid,
            const RenameStruct *rename_input,
            const TimeStruct *commit_input);

    /** dispatchQueue is the buffer between rename and iq
     *  first, dispatch the inst from DispatchQueue to IQ
     *  second, receive new inst from rename, store it to DQ
     */
    void dispatchInstFromDispQue();
    void classifyInstToDispQue(ThreadID tid,
                               const RenameStruct *rename_input,
                               const TimeStruct *commit_input);

    /** Executes instructions. In the case of memory operations, it informs the
     * LSQ to execute the instructions. Also handles any redirects that occur
     * due to the executed instructions.
     */
    void executeInsts();

    /** Writebacks instructions. In our model, the instruction's execute()
     * function atomically reads registers, executes, and writes registers.
     * Thus this writeback only wakes up dependent instructions, and informs
     * the scoreboard of registers becoming ready.
     */
    void writebackInsts();

    bool checkSerialize(const DynInstPtr& inst,
                        const TimeStruct *commit_input);

    /** Processes inputs and changes state accordingly. */
    void checkSquash(const TimeStruct *commit_input);

    /** Sorts instructions coming from rename into lists separated by thread. */
    void moveInstsToBuffer(const RenameStruct *rename_input);

  public:
    /** Ticks IEW stage, causing Dispatch, the IQ, the LSQ, Execute, and
     * Writeback to run for one cycle.
     */
    void tick();

  private:
    /** Updates execution stats based on the instruction. */
    void updateExeInstStats(const DynInstPtr &inst);

    /** Pointer to main time buffer used for backwards communication. */
    TimeBuffer<TimeStruct> *timeBuffer;

    /** Wire to write information heading to previous stages. */
    TimeBuffer<TimeStruct>::wire toFetch;

    /** Wire to get commit's output from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromCommit;

    /** Wire to write information heading to previous stages. */
    TimeBuffer<TimeStruct>::wire toRename;

    /** Rename instruction queue interface. */
    TimeBuffer<RenameStruct> *renameQueue;

    /** Wire to get rename's output from rename queue. */
    TimeBuffer<RenameStruct>::wire fromRename;

    /** Issue stage queue. */
    TimeBuffer<IssueStruct> issueToExecQueue;

    /** Wire to read information from the issue stage time queue. */
    TimeBuffer<IssueStruct>::wire fromIssue;

    /**
     * IEW stage time buffer.  Holds ROB indices of instructions that
     * can be marked as completed.
     */
    TimeBuffer<IEWStruct> *iewQueue;

    /** Wire to write infromation heading to commit. */
    TimeBuffer<IEWStruct>::wire toCommit;

    /** Queue of all instructions coming from rename this cycle. */
    std::deque<DynInstPtr> fixedbuffer[MaxThreads];

    std::deque<DynInstPtr> dispQue[3];

    /** Scoreboard pointer. */
    Scoreboard* scoreboard;

    SquashVersion localSquashVer{0};

    /** Value predictor */
    valuepred::VPUnit *valuePred;

    /** Enable selective VP flush path */
    bool enableSelectiveVPFlush;

  private:
    /** CPU pointer. */
    CPU *cpu;

    Scheduler* scheduler;

    /** Records if IEW has written to the time buffer this cycle, so that the
     * CPU can deschedule itself if there is no activity.
     */
    bool wroteToTimeBuffer;

    /** Debug function to print instructions that are issued this cycle. */
    void printAvailableInsts();

  public:

    Scheduler* getScheduler() { return scheduler; }
    /** Instruction queue. */
    InstructionQueue instQueue;

    /** Load / store queue. */
    LSQ ldstQueue;

    std::vector<uint32_t> dispWidth;

    /** Records if the LSQ needs to be updated on the next cycle, so that
     * IEW knows if there will be activity on the next cycle.
     */
    bool updateLSQNextCycle;

  private:
    /** Records if there is a fetch redirect on this cycle for each thread. */
    bool fetchRedirect[MaxThreads];

    /** Records if the queues have been changed (inserted or issued insts),
     * so that IEW knows to broadcast the updated amount of free entries.
     */
    bool updatedQueues;

    /** Commit to IEW delay. */
    Cycles commitToIEWDelay;

    /** Rename to IEW delay. */
    Cycles renameToIEWDelay;

    bool enableDispatchStage;

    unsigned renameWidth;

    /** Index into queue of instructions being written back. */
    unsigned wbNumInst;

    /** Cycle number within the queue of instructions being written back.
     * Used in case there are too many instructions writing back at the current
     * cycle and writesbacks need to be scheduled for the future. See comments
     * in instToCommit().
     */
    unsigned wbCycle;

    /** IEW to Commit delay. */
    const Cycles iewToCommitDelay;

    /** Writeback width. */
    unsigned wbWidth;

    bool enableStoreSetTrain;

    /** Number of active threads. */
    ThreadID numThreads;

    /** Pointer to list of active threads. */
    std::list<ThreadID> *activeThreads;

    /** Maximum size of the skid buffer. */
    unsigned skidBufferMax;

    struct IEWStats : public statistics::Group
    {
        IEWStats(CPU *cpu);

        /** Stat for total number of idle cycles. */
        statistics::Scalar idleCycles;
        /** Stat for total number of squashing cycles. */
        statistics::Scalar squashCycles;
        /** Stat for total number of blocking cycles. */
        statistics::Scalar blockCycles;
        /** Stat for total number of unblocking cycles. */
        statistics::Scalar unblockCycles;
        /** Stat for total number of instructions dispatched. */
        statistics::Scalar dispatchedInsts;
        /** Stat for total number of squashed instructions dispatch skips. */
        statistics::Scalar dispSquashedInsts;
        /** Stat for number of IEW prepare tasks submitted. */
        statistics::Scalar prepareTasks;
        /** Stat for number of IEW prepare results merged. */
        statistics::Scalar prepareMerges;
        /** Accumulated active thread count seen by IEW prepare. */
        statistics::Scalar prepareActiveThreads;
        /** Accumulated blocked thread count seen by IEW prepare. */
        statistics::Scalar prepareBlockedThreads;
        /** Accumulated empty-thread count evaluated inline by IEW prepare. */
        statistics::Scalar prepareInlineEmptyThreads;
        /** Number of times IEW prepare saw multiple active threads. */
        statistics::Scalar prepareMultipleActive;
        /** Stat for number of future IEW prepare probes submitted. */
        statistics::Scalar futurePrepareProbes;
        /** Stat for number of future IEW prepare probes skipped. */
        statistics::Scalar futurePrepareSkipped;
        /** Breakdown of future IEW input construction skip reasons. */
        statistics::Vector futureInputSkipReasons;
        /** Breakdown of commit control fields blocking future IEW input. */
        statistics::Vector futureInputCommitControlReasons;
        /** Commit progress fields accepted by future IEW input. */
        statistics::Vector futureInputAllowedCommitProgress;
        /** Breakdown of why future IEW preview could not predict a latch. */
        statistics::Vector futurePreviewSkipReasons;
        /** Source of active dispatches that blocked future IEW preview. */
        statistics::Vector futureActiveDispatchSources;
        /** Dispatch mode for active dispatches blocking future IEW preview. */
        statistics::Vector futureActiveDispatchModes;
        /** Preview outcome for active dispatches seen by future IEW preview. */
        statistics::Vector futureActiveDispatchPreviewOutcomes;
        /** Block reason for skipped active future dispatch previews. */
        statistics::Vector futureActiveDispatchPreviewBlockReasons;
        /** Scheduler token reason for future SchedulerNotReady previews. */
        statistics::Vector futureActiveDispatchSchedulerBlockReasons;
        /** Visible instruction count for active future dispatch previews. */
        statistics::Distribution futureActiveDispatchInsts;
        /** Stat for number of future IEW prepare results merged. */
        statistics::Scalar futurePrepareMerges;
        /** Stat for number of future IEW prepare results reused. */
        statistics::Scalar futurePrepareReuses;
        /** Stat for number of future IEW prepare results checked. */
        statistics::Scalar futurePrepareChecks;
        /** Stat for number of future IEW prepare checks that matched. */
        statistics::Scalar futurePrepareMatches;
        /** Stat for number of future IEW prepare checks that mismatched. */
        statistics::Scalar futurePrepareMismatches;
        /** Stat for number of stale future IEW prepare results. */
        statistics::Scalar futurePrepareStale;
        /** Stat for number of dispatch status prepare tasks submitted. */
        statistics::Scalar dispatchStatusPrepareTasks;
        /** Stat for number of dispatch status prepare results merged. */
        statistics::Scalar dispatchStatusPrepareMerges;
        /** Stat for dispatch status prepare validation mismatches. */
        statistics::Scalar dispatchStatusPrepareMismatches;
        /** Stat for direct-dispatch drain previews submitted. */
        statistics::Scalar dispatchDrainPreviewProbes;
        /** Stat for direct-dispatch drain previews skipped. */
        statistics::Scalar dispatchDrainPreviewSkipped;
        /** Breakdown of direct-dispatch drain preview skip reasons. */
        statistics::Vector dispatchDrainPreviewSkipReasons;
        /** Stat for direct-dispatch drain preview validation matches. */
        statistics::Scalar dispatchDrainPreviewMatches;
        /** Stat for direct-dispatch drain preview validation mismatches. */
        statistics::Scalar dispatchDrainPreviewMismatches;
        /** Direct-dispatch blocked stall reason validation matches. */
        statistics::Scalar dispatchDrainPreviewStallReasonMatches;
        /** Direct-dispatch blocked stall reason validation mismatches. */
        statistics::Scalar dispatchDrainPreviewStallReasonMismatches;
        /** Direct-dispatch stall reason checks skipped after side effects. */
        statistics::Scalar dispatchDrainPreviewStallReasonSideEffectSkips;
        /** Current-cycle direct-dispatch output snapshots checked. */
        statistics::Scalar dispatchOutputSnapshotChecks;
        /** Current-cycle direct-dispatch output snapshots matching. */
        statistics::Scalar dispatchOutputSnapshotMatches;
        /** Current-cycle direct-dispatch output snapshots mismatching. */
        statistics::Scalar dispatchOutputSnapshotMismatches;
        /** Fields mismatching in current-cycle dispatch output snapshots. */
        statistics::Vector dispatchOutputSnapshotMismatchFields;
        /** Future direct-dispatch previews checked next cycle. */
        statistics::Scalar futureDispatchPreviewChecks;
        /** Future direct-dispatch previews matching current-cycle preview. */
        statistics::Scalar futureDispatchPreviewMatches;
        /** Future direct-dispatch previews differing from current-cycle preview. */
        statistics::Scalar futureDispatchPreviewDifferences;
        /** Breakdown of future direct-dispatch preview differences. */
        statistics::Vector futureDispatchPreviewDifferenceReasons;
        /** Direction of dispatched-before-block count differences. */
        statistics::Vector
            futureDispatchPreviewDispatchedBeforeBlockDiffDirections;
        /** Direction of drained/block state differences. */
        statistics::Vector futureDispatchPreviewDrainedDiffDirections;
        /** Absolute size of dispatched-before-block count differences. */
        statistics::Distribution
            futureDispatchPreviewDispatchedBeforeBlockDelta;
        /** Future dispatch output snapshots checked next cycle. */
        statistics::Scalar futureDispatchOutputSnapshotChecks;
        /** Future dispatch output snapshots matching current-cycle preview. */
        statistics::Scalar futureDispatchOutputSnapshotMatches;
        /** Future dispatch output snapshots differing from current preview. */
        statistics::Scalar futureDispatchOutputSnapshotDifferences;
        /** Fields differing in future dispatch output snapshots. */
        statistics::Vector futureDispatchOutputSnapshotDifferenceFields;
        /** Publishability classification for future dispatch outputs. */
        statistics::Vector futureDispatchOutputPublishability;
        /** Block reason for stable future blocked dispatch outputs. */
        statistics::Vector futureDispatchOutputStableBlockedReasons;
        /** Scheduler reason for stable SchedulerNotReady outputs. */
        statistics::Vector futureDispatchOutputStableBlockedSchedulerReasons;
        /** Fixedbuffer pops in stable blocked future dispatch outputs. */
        statistics::Distribution futureDispatchOutputStableBlockedPops;
        /** Block reason for future previews that were not stable. */
        statistics::Vector futureDispatchOutputPreviewDifferentReasons;
        /** Scheduler reason for unstable SchedulerNotReady previews. */
        statistics::Vector
            futureDispatchOutputPreviewDifferentSchedulerReasons;
        /** Expected fixedbuffer pops in unstable future previews. */
        statistics::Distribution futureDispatchOutputPreviewDifferentPops;
        /** Future scheduler block token snapshots checked next cycle. */
        statistics::Scalar futureDispatchBlockTokenChecks;
        /** Future scheduler block token snapshots matching actual. */
        statistics::Scalar futureDispatchBlockTokenMatches;
        /** Future scheduler block token snapshots differing from actual. */
        statistics::Scalar futureDispatchBlockTokenDifferences;
        /** Fields differing in future scheduler block token snapshots. */
        statistics::Vector futureDispatchBlockTokenDifferenceFields;
        /** Block token snapshot matches by output publishability class. */
        statistics::Vector futureDispatchBlockTokenMatchesByPublishability;
        /** Block token snapshot diffs by output publishability class. */
        statistics::Vector futureDispatchBlockTokenDifferencesByPublishability;
        /** Future IEW-to-Rename latch previews checked next cycle. */
        statistics::Scalar futureRenameLatchPreviewChecks;
        /** Future IEW-to-Rename latch previews matching actual latch. */
        statistics::Scalar futureRenameLatchPreviewMatches;
        /** Future IEW-to-Rename latch previews differing from actual latch. */
        statistics::Scalar futureRenameLatchPreviewDifferences;
        /** Breakdown of future IEW-to-Rename latch preview differences. */
        statistics::Vector futureRenameLatchPreviewDifferenceReasons;
        /** Future IEW-to-Rename latch preview matches by output class. */
        statistics::Vector futureRenameLatchPreviewMatchesByPublishability;
        /** Future IEW-to-Rename latch preview diffs by output class. */
        statistics::Vector
            futureRenameLatchPreviewDifferencesByPublishability;
        /** Future IEW-to-Rename latch previews discarded before checking. */
        statistics::Scalar futureRenameLatchPreviewStale;
        /** Future direct-dispatch previews discarded before checking. */
        statistics::Scalar futureDispatchPreviewStale;
        /** Stat for number of writeback prepare tasks submitted. */
        statistics::Scalar writebackPrepareTasks;
        /** Stat for number of writeback prepare results merged. */
        statistics::Scalar writebackPrepareMerges;
        /** Stat for number of writeback prepare cycles with no entries. */
        statistics::Scalar writebackPrepareNoWork;
        /** Stat for writeback prepare validation mismatches. */
        statistics::Scalar writebackPrepareMismatches;
        /** Stat for total number of dispatched load instructions. */
        statistics::Scalar dispLoadInsts;
        /** Stat for total number of dispatched store instructions. */
        statistics::Scalar dispStoreInsts;
        /** Stat for total number of dispatched non speculative insts. */
        statistics::Scalar dispNonSpecInsts;
        /** Stat for number of times the IQ becomes full. */
        statistics::Scalar iqFullEvents;
        /** Stat for number of times the LSQ becomes full. */
        statistics::Scalar lsqFullEvents;
        /** Stat for total number of memory ordering violation events. */
        statistics::Scalar memOrderViolationEvents;
        /** Stat for total number of incorrect predicted taken branches. */
        statistics::Scalar predictedTakenIncorrect;
        /** Stat for total number of incorrect predicted not taken branches. */
        statistics::Scalar predictedNotTakenIncorrect;
        /** Stat for total number of mispredicted branches detected at
         *  execute. */
        statistics::Formula branchMispredicts;

        statistics::Distribution dispDist;

        struct ExecutedInstStats : public statistics::Group
        {
            ExecutedInstStats(CPU *cpu);

            /** Stat for total number of executed instructions. */
            statistics::Scalar numInsts;
            /** Stat for total number of executed load instructions. */
            statistics::Vector numLoadInsts;
            /** Stat for total number of squashed instructions skipped at
             *  execute. */
            statistics::Scalar numSquashedInsts;
            /** Number of executed software prefetches. */
            statistics::Vector numSwp;
            /** Number of executed nops. */
            statistics::Vector numNop;
            /** Number of executed meomory references. */
            statistics::Vector numRefs;
            /** Number of executed branches. */
            statistics::Vector numBranches;
            /** Number of executed store instructions. */
            statistics::Formula numStoreInsts;
            /** Number of instructions executed per cycle. */
            statistics::Formula numRate;
        } executedInstStats;

        /** Number of instructions sent to commit. */
        statistics::Vector instsToCommit;
        /** Number of instructions that writeback. */
        statistics::Vector writebackCount;
        /** Number of instructions that wake consumers. */
        statistics::Vector producerInst;
        /** Number of instructions that wake up from producers. */
        statistics::Vector consumerInst;
        /** Number of instructions per cycle written back. */
        statistics::Formula wbRate;
        /** Average number of woken instructions per writeback. */
        statistics::Formula wbFanout;

        statistics::Vector stallEvents;

        /** Distribution of number of fetch stall reasons each tick. */
        statistics::Vector fetchStallReason;
        /** Distribution of number of decode stall reasons each tick. */
        statistics::Vector decodeStallReason;
        /** Distribution of number of fetrenamech stall reasons each tick. */
        statistics::Vector renameStallReason;
        /** Distribution of number of dispatch stall reasons each tick. */

        statistics::Vector dispatchStallReason;
    } iewStats;

    /** The width that can be dispatched to the scheduler per cycle. */
    std::vector<StallReason> dispatchStalls;

    StallReason blockReason{NoStall};

    IEWPrepareResult lastPrepareResult;

    ROB* rob;

    void setAllStalls(StallReason dispatchStall);

    StallReason checkLoadStoreInst(DynInstPtr inst);

    StallReason dqTypeToReason(DQType dq_type);

    DQType getInstDQType(const DynInstPtr &inst);

    StallReason checkDispatchStall(ThreadID tid, int dq_stall, const DynInstPtr &dispatch_inst, int disp_seq);

    StallReason checkLSQStall(ThreadID tid, bool isLoad);

  public:

    const IEWStats& getIEWStats() const { return iewStats; }

    void setRob(ROB *rob);

};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_IEW_HH__
