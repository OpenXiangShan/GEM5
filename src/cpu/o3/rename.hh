/*
 * Copyright (c) 2012, 2017 ARM Limited
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

#ifndef __CPU_O3_RENAME_HH__
#define __CPU_O3_RENAME_HH__

#include <list>
#include <utility>

#include "base/statistics.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/commit.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/free_list.hh"
#include "cpu/o3/iew.hh"
#include "cpu/o3/limits.hh"
#include "cpu/timebuf.hh"
#include "cpu/valuepred/valuepred_unit.hh"
#include "sim/probe/probe.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

/**
 * Rename handles both single threaded and SMT rename. Its
 * width is specified by the parameters; each cycle it tries to rename
 * that many instructions. It holds onto the rename history of all
 * instructions with destination registers, storing the
 * arch. register, the new physical register, and the old physical
 * register, to allow for undoing of mappings if squashing happens, or
 * freeing up registers upon commit. Rename handles blocking if the
 * ROB, IQ, or LSQ is going to be full. Rename also handles barriers,
 * and does so by stalling on the instruction until the ROB is empty
 * and there are no instructions in flight to the ROB.
 */
class Rename
{
  public:
    // A deque is used to queue the instructions. Barrier insts must
    // be added to the front of the queue, which is the only reason for
    // using a deque instead of a queue. (Most other stages use a
    // queue)
    typedef std::deque<DynInstPtr> InstQueue;

  public:
    /** Overall rename status. Used to determine if the CPU can
     * deschedule itself due to a lack of activity.
     */
    enum RenameStatus
    {
        Active,
        Inactive
    };

    /** Individual thread status. */
    enum ThreadStatus
    {
        Running,
        Idle,
        StartSquash,
        Squashing,
        Blocked,
        Unblocking,
        SerializeStall
    };

    enum StallEvent
    {
        ROBWalk=0,
        IEWStall,
        ROBFull,
        IQFull,
        LSQFull,
        RegFull,
        SerializeInst,
        BWFull,
        StallEventCount
    };

  private:
    /** Rename status. */
    RenameStatus _status;

    /** Probe points. */
    typedef std::pair<InstSeqNum, PhysRegIdPtr> SeqNumRegPair;
    /** To probe when register renaming for an instruction is complete */
    ProbePointArg<DynInstPtr> *ppRename;
    /**
     * To probe when an instruction is squashed and the register mapping
     * for it needs to be undone
     */
    ProbePointArg<SeqNumRegPair> *ppSquashInRename;

  public:
    /** Rename constructor. */
    Rename(CPU *_cpu, const BaseO3CPUParams &params);

    /** Returns the name of rename. */
    std::string name() const;

    /** Registers probes. */
    void regProbePoints();

    /** Sets the main backwards communication time buffer pointer. */
    void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr);

    /** Sets pointer to time buffer used to communicate to the next stage. */
    void setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr);

    /** Sets pointer to time buffer coming from decode. */
    void setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr);

    /** Sets pointer to IEW stage. Used only for initialization. */
    void setIEWStage(IEW *iew_stage) { iew_ptr = iew_stage; }

    /** Sets pointer to commit stage. Used only for initialization. */
    void
    setCommitStage(Commit *commit_stage)
    {
        commit_ptr = commit_stage;
    }

  private:
    /** Pointer to IEW stage. Used only for initialization. */
    IEW *iew_ptr;

    /** Pointer to commit stage. Used only for initialization. */
    Commit *commit_ptr;

  public:
    /** Initializes variables for the stage. */
    void startupStage();

    /** Clear all thread-specific states */
    void clearStates(ThreadID tid);

    /** Sets pointer to list of active threads. */
    void setActiveThreads(std::list<ThreadID> *at_ptr);

    /** Sets pointer to rename maps (per-thread structures). */
    void setRenameMap(UnifiedRenameMap rm_ptr[MaxThreads]);

    /** Sets pointer to the free list. */
    void setFreeList(UnifiedFreeList *fl_ptr);

    /** Sets pointer to the scoreboard. */
    void setScoreboard(Scoreboard *_scoreboard);

    /** Perform sanity checks after a drain. */
    void drainSanityCheck() const;

    /** Has the stage drained? */
    bool isDrained() const;

    /** Takes over from another CPU's thread. */
    void takeOverFrom();

    /** Squashes all instructions in a thread. */
    void squash(const InstSeqNum &squash_seq_num, ThreadID tid);

    /** Ticks rename, which processes all input signals and attempts to rename
     * as many instructions as possible.
     */
    void tick();

    /** Debugging function used to dump history buffer of renamings. */
    void dumpHistory();

    void setStallSignals(StallSignals* stall_signals) { stallSig = stall_signals; }
    void setStallSignalBank(StallSignalBank* bank) { stallSignalBank = bank; }

    struct RenamePrepareResult
    {
        Cycles cycle = Cycles(0);
        ThreadID selectedTid = InvalidThreadID;
        ThreadID blockedTid = InvalidThreadID;
        unsigned activeThreads = 0;
        unsigned blockedThreads = 0;
        unsigned regFullEvents = 0;
        bool multipleActive = false;
        bool canRename[MaxThreads] = {};
        bool iewBlock[MaxThreads] = {};
        bool block[MaxThreads] = {};
        bool active[MaxThreads] = {};
        bool decodeBlock[MaxThreads] = {};
        StallReason renameBlockReason[MaxThreads] = {};
        StallReason decodeBlockReason[MaxThreads] = {};
    };

    static constexpr unsigned NumRenameRegClasses =
        static_cast<unsigned>(RMiscRegClass) + 1;

    struct RenamePrepareInput
    {
        Cycles cycle = Cycles(0);
        ThreadID numThreads = 0;
        bool fixedbufferEmpty[MaxThreads] = {};
        unsigned fixedbufferSize[MaxThreads] = {};
        int demandPhyRegs[MaxThreads][NumRenameRegClasses] = {};
        int freePhyRegs[MaxThreads][NumRenameRegClasses] = {};
        StallSignalLatch iewToRename;
        StallReason robHeadStallReason[MaxThreads] = {};
        StallReason lqHeadStallReason[MaxThreads] = {};
        StallReason sqHeadStallReason[MaxThreads] = {};
    };

    struct FutureCandidatePrepareProfile
    {
        bool valid = false;
        unsigned blockReason = 0;
        unsigned schedulerReason = 0;
        unsigned fixedBufferPops = 0;
        unsigned dispatchedBeforeBlock = 0;
    };

    bool previewFutureDecodeLatch(Cycles cycle,
                                  const StallSignalLatch &iew_to_rename,
                                  const DecodeStruct *snapshot_decode,
                                  const TimeStruct *snapshot_iew,
                                  const TimeStruct *snapshot_commit,
                                  StallSignalLatch &rename_to_decode,
                                  RenamePrepareResult *prepare_result =
                                      nullptr);
    bool buildFutureDecodeLatchInput(
            Cycles cycle, const StallSignalLatch &iew_to_rename,
            const DecodeStruct *snapshot_decode,
            const TimeStruct *snapshot_iew,
            const TimeStruct *snapshot_commit,
            RenamePrepareInput &input,
            bool count_stats = true);
    bool previewFutureDecodeLatch(
            const RenamePrepareInput &input,
            StallSignalLatch &rename_to_decode,
            RenamePrepareResult *prepare_result = nullptr) const;
    RenamePrepareResult previewFuturePrepare(
            const RenamePrepareInput &input) const;
    void recordFuturePrepareProbe();
    void recordFuturePrepareSkipped();
    void recordFuturePreviewSkipped(const RenamePrepareResult &result);
    void setPendingFuturePrepare(const RenamePrepareResult &result);
    void setPendingFutureCandidatePrepare(
            const RenamePrepareResult &result,
            const FutureCandidatePrepareProfile &profile,
            const RenamePrepareInput &input);

  private:
    struct RenameThreadPrepareResult
    {
        Cycles cycle = Cycles(0);
        ThreadID tid = InvalidThreadID;
        bool canRename = true;
        bool iewBlock = false;
        bool block = false;
        bool active = false;
        bool blocked = false;
        bool decodeBlock = false;
        unsigned regFullEvents = 0;
        StallReason renameBlockReason = StallReason::NoStall;
        StallReason decodeBlockReason = StallReason::NoStall;
    };

    struct RenameThreadPrepareResults
    {
        RenameThreadPrepareResult byThread[MaxThreads];
    };

    void setDecodeStall(ThreadID tid, bool block, StallReason reason);
    void setDecodeBlock(ThreadID tid, bool block);
    RenamePrepareInput buildRenamePrepareInput(
            Cycles cycle,
            const StallSignalLatch *iew_to_rename_override = nullptr,
            const DecodeStruct *snapshot_decode = nullptr,
            const TimeStruct *snapshot_iew = nullptr) const;
    RenameThreadPrepareResult prepareRenameThreadControl(
            const RenamePrepareInput &input, ThreadID tid) const;
    RenamePrepareResult combineRenameThreadPrepareResults(
            const RenamePrepareInput &input,
            const RenameThreadPrepareResults &thread_results) const;
    RenamePrepareResult prepareRenameControl(
            const RenamePrepareInput &input) const;
    unsigned applyFutureReleaseDeltas(RenamePrepareInput &input) const;
    RenamePrepareResult runRenamePrepare(Cycles cycle);
    void mergeRenamePrepareResult(const RenamePrepareResult &result,
                                  bool countPrepareStats);
    bool samePrepareResult(const RenamePrepareResult &lhs,
                           const RenamePrepareResult &rhs) const;
    unsigned futurePrepareMismatchReason(
            const RenamePrepareResult &lhs,
            const RenamePrepareResult &rhs) const;
    bool sameFutureCandidateInput(
            const RenamePrepareInput &lhs,
            const RenamePrepareInput &rhs) const;
    void recordFutureCandidateInputDifferenceFields(
            const RenamePrepareInput &expected,
            const RenamePrepareInput &actual,
            statistics::Vector &fields) const;
    void recordFutureCandidateIEWBlockDiffDirections(
            const RenamePrepareInput &expected,
            const RenamePrepareInput &actual,
            bool prepare_match);

    /** Reset this pipeline stage */
    void resetStage();

    /** Renames instructions for the given thread. Also handles serializing
     * instructions.
     */
    void renameInsts(ThreadID tid, const TimeStruct *iew_input);

    /** Checks if the rename map can rename all the given number of instructions this cycle. */
    bool canRename(ThreadID tid);

    void releasePhysRegs(const TimeStruct *commit_input);

    /** Separates instructions from decode into individual lists of instructions
     * sorted by thread.
     */
    const TimeStruct *iewInput(Cycles cycle) const;
    const TimeStruct *commitInput(Cycles cycle) const;
    const DecodeStruct *decodeInput(Cycles cycle) const;
    void moveInstsToBuffer(const DecodeStruct *decode_input);

    void checkSquash(const TimeStruct *commit_input);

    /** Updates overall rename status based on all of the threads' statuses. */
    void updateActivate();

    /** Executes actual squash, removing squashed instructions. */
    void doSquash(const InstSeqNum &squash_seq_num, ThreadID tid);

    /** Removes a committed instruction's rename history. */
    void removeFromHistory(InstSeqNum inst_seq_num, ThreadID tid);

    /** Renames the source registers of an instruction. */
    void renameSrcRegs(const DynInstPtr &inst, ThreadID tid);

    /** Renames the destination registers of an instruction. */
    void renameDestRegs(const DynInstPtr &inst, ThreadID tid);

    /** Holds the information for each destination register rename. It holds
     * the instruction's sequence number, the arch register, the old physical
     * register for that arch. register, and the new physical register.
     */
    struct RenameHistory
    {
        RenameHistory(InstSeqNum _instSeqNum, const RegId& _archReg,
                      VirtRegId _newPhysReg,
                      VirtRegId _prevPhysReg)
            : instSeqNum(_instSeqNum), archReg(_archReg),
              newPhysReg(_newPhysReg), prevPhysReg(_prevPhysReg)
        {
        }

        /** The sequence number of the instruction that renamed. */
        InstSeqNum instSeqNum;
        /** The architectural register index that was renamed. */
        RegId archReg;
        /** The new physical register that the arch. register is renamed to. */
        VirtRegId newPhysReg;
        /** The old physical register that the arch. register was renamed to.
         */
        VirtRegId prevPhysReg;
    };

    /** A per-thread list of all destination register renames, used to either
     * undo rename mappings or free old physical registers.
     */
    std::list<RenameHistory> historyBuffer[MaxThreads];

    InstSeqNum finalCommitSeq = 0;

    InstSeqNum releaseSeq = 0;

    void tryFreePReg(PhysRegIdPtr phys_reg);

    /** Pointer to CPU. */
    CPU *cpu;

    /** Pointer to main time buffer used for backwards communication. */
    TimeBuffer<TimeStruct> *timeBuffer;

    /** Wire to get IEW's output from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromIEW;

    /** Wire to get commit's output from backwards time buffer. */
    TimeBuffer<TimeStruct>::wire fromCommit;

    /** Wire to write infromation heading to previous stages. */
    TimeBuffer<TimeStruct>::wire toDecode;

    /** Rename instruction queue. */
    TimeBuffer<RenameStruct> *renameQueue;

    /** Wire to write any information heading to IEW. */
    TimeBuffer<RenameStruct>::wire toIEW;

    /** Decode instruction queue interface. */
    TimeBuffer<DecodeStruct> *decodeQueue;

    /** Wire to get decode's output from decode queue. */
    TimeBuffer<DecodeStruct>::wire fromDecode;

    /** Queue of all instructions coming from decode this cycle. */
    boost::circular_buffer<DynInstPtr> fixedbuffer[MaxThreads];

    struct PendingFuturePrepare
    {
        bool valid = false;
        RenamePrepareResult result;
    };

    struct PendingFutureCandidatePrepare
    {
        bool valid = false;
        RenamePrepareResult result;
        FutureCandidatePrepareProfile profile;
        RenamePrepareInput input;
    };

    PendingFuturePrepare pendingFuturePrepare;
    PendingFutureCandidatePrepare pendingFutureCandidatePrepare;

    /** Rename map interface. */
    UnifiedRenameMap *renameMap[MaxThreads];

    /** Free list interface. */
    UnifiedFreeList *freeList;

    /** Pointer to the list of active threads. */
    std::list<ThreadID> *activeThreads;

    /** Pointer to the scoreboard. */
    Scoreboard *scoreboard;

    /** Variable that tracks if decode has written to the time buffer this
     * cycle. Used to tell CPU if there is activity this cycle.
     */
    bool wroteToTimeBuffer;

    /** Source of possible stalls. */
    struct Stalls
    {
        bool iew;
        bool commit;
    };

    /** Tracks which stages are telling decode to stall. */
    Stalls stalls[MaxThreads];

    StallSignals* stallSig;
    StallSignalBank* stallSignalBank = nullptr;

    /** Delay between iew and rename, in ticks. */
    int iewToRenameDelay;

    /** Delay between decode and rename, in ticks. */
    int decodeToRenameDelay;

    /** Delay between commit and rename, in ticks. */
    unsigned commitToRenameDelay;

    /** Rename width, in instructions. */
    unsigned renameWidth;

    unsigned releaseWidth;

    /** Whether or not rename needs to resume clearing out the skidbuffer
     * after squashing. */
    bool resumeUnblocking;

    /** The number of threads active in rename. */
    ThreadID numThreads;

    /** Enum to record the source of a structure full stall.  Can come from
     * either ROB, IQ, LSQ, and it is priortized in that order.
     */
    enum FullSource
    {
        ROB,
        IQ,
        LQ,
        SQ,
        NONE
    };

    /** Function used to increment the stat that corresponds to the source of
     * the stall.
     */
    void incrFullStat(const FullSource &source);

    struct RenameStats : public statistics::Group
    {
        RenameStats(statistics::Group *parent);

        /** Stat for total number of cycles spent squashing. */
        statistics::Scalar squashCycles;
        /** Stat for total number of cycles spent idle. */
        statistics::Scalar idleCycles;
        /** Stat for total number of cycles spent blocking. */
        statistics::Scalar blockCycles;
        /** Stat for total number of cycles spent stalling for a serializing
         *  inst. */
        statistics::Scalar serializeStallCycles;
        /** Stat for total number of cycles spent running normally. */
        statistics::Scalar runCycles;
        /** Stat for total number of cycles spent unblocking. */
        statistics::Scalar unblockCycles;
        /** Stat for total number of renamed instructions. */
        statistics::Scalar renamedInsts;
        /** Stat for total number of squashed instructions that rename
         * discards. */
        statistics::Scalar squashedInsts;
        /** Stat for number of rename prepare tasks submitted. */
        statistics::Scalar prepareTasks;
        /** Stat for number of rename prepare results merged. */
        statistics::Scalar prepareMerges;
        /** Accumulated active thread count seen by rename prepare. */
        statistics::Scalar prepareActiveThreads;
        /** Accumulated blocked thread count seen by rename prepare. */
        statistics::Scalar prepareBlockedThreads;
        /** Accumulated inactive thread count seen by rename prepare. */
        statistics::Scalar prepareInactiveThreads;
        /** Number of times rename prepare saw multiple active threads. */
        statistics::Scalar prepareMultipleActive;
        /** Number of future rename prepare probes submitted. */
        statistics::Scalar futurePrepareProbes;
        /** Number of future rename prepare probes skipped. */
        statistics::Scalar futurePrepareSkipped;
        /** Breakdown of why future rename input construction was skipped. */
        statistics::Vector futureInputSkipReasons;
        /** Breakdown of commit controls blocking future rename input. */
        statistics::Vector futureInputCommitControlReasons;
        /** Breakdown of why future Rename-to-Decode latch preview failed. */
        statistics::Vector futurePreviewSkipReasons;
        /** Future rename inputs that projected pending phys-reg releases. */
        statistics::Scalar futureInputVirtualReleaseSteps;
        /** Phys regs virtually added by future rename release projection. */
        statistics::Scalar futureInputVirtualReleaseRegs;
        /** Number of future rename prepare results made pending. */
        statistics::Scalar futurePrepareMerges;
        /** Number of current rename prepares reused from future work. */
        statistics::Scalar futurePrepareReuses;
        /** Number of future rename prepare validation checks. */
        statistics::Scalar futurePrepareChecks;
        /** Number of future rename prepare validation matches. */
        statistics::Scalar futurePrepareMatches;
        /** Number of future rename prepare validation mismatches. */
        statistics::Scalar futurePrepareMismatches;
        /** Breakdown of future rename prepare validation mismatches. */
        statistics::Vector futurePrepareMismatchReasons;
        /** Number of stale future rename prepare results discarded. */
        statistics::Scalar futurePrepareStale;
        /** Diagnostic candidate future rename prepares checked. */
        statistics::Scalar futureCandidatePrepareChecks;
        /** Diagnostic candidate future rename prepares matching. */
        statistics::Scalar futureCandidatePrepareMatches;
        /** Diagnostic candidate future rename prepares mismatching. */
        statistics::Scalar futureCandidatePrepareMismatches;
        /** Candidate future rename prepare mismatch reasons. */
        statistics::Vector futureCandidatePrepareMismatchReasons;
        /** Diagnostic candidate future rename prepares discarded stale. */
        statistics::Scalar futureCandidatePrepareStale;
        /** Candidate future rename prepare matches by scheduler reason. */
        statistics::Vector futureCandidatePrepareMatchesBySchedulerReason;
        /** Candidate future rename prepare mismatches by scheduler reason. */
        statistics::Vector futureCandidatePrepareMismatchesBySchedulerReason;
        /** Candidate future rename prepare matches by expected pops. */
        statistics::Vector futureCandidatePrepareMatchesByExpectedPops;
        /** Candidate future rename prepare mismatches by expected pops. */
        statistics::Vector futureCandidatePrepareMismatchesByExpectedPops;
        /** Candidate prepare result stability vs candidate input stability. */
        statistics::Vector futureCandidatePrepareInputStability;
        /** Direction of candidate/current IEW block input differences. */
        statistics::Vector futureCandidateIEWBlockDiffDirections;
        /** Candidate future rename prepare inputs checked. */
        statistics::Scalar futureCandidateInputChecks;
        /** Candidate future rename prepare inputs matching current input. */
        statistics::Scalar futureCandidateInputMatches;
        /** Candidate future rename prepare inputs differing from current. */
        statistics::Scalar futureCandidateInputDifferences;
        /** Fields differing in candidate future rename prepare inputs. */
        statistics::Vector futureCandidateInputDifferenceFields;
        /** Input fields differing when candidate prepare still matched. */
        statistics::Vector futureCandidateInputMatchDifferenceFields;
        /** Input fields differing when candidate prepare mismatched. */
        statistics::Vector futureCandidateInputMismatchDifferenceFields;
        /** Stat for total number of times that the ROB starts a stall in
         * rename. */
        statistics::Scalar ROBFullEvents;
        /** Stat for total number of times that the IQ starts a stall in
         *  rename. */
        statistics::Scalar IQFullEvents;
        /** Stat for total number of times that the LQ starts a stall in
         *  rename. */
        statistics::Scalar LQFullEvents;
        /** Stat for total number of times that the SQ starts a stall in
         *  rename. */
        statistics::Scalar SQFullEvents;
        /** Stat for total number of times that rename runs out of free
         *  registers to use to rename. */
        statistics::Scalar fullRegistersEvents;
        /** Stat for total number of renamed destination registers. */
        statistics::Scalar renamedOperands;
        /** Stat for total number of source register rename lookups. */
        statistics::Scalar lookups;
        statistics::Scalar intLookups;
        statistics::Scalar fpLookups;
        statistics::Scalar vecLookups;
        statistics::Scalar vecPredLookups;
        /** Stat for total number of committed renaming mappings. */
        statistics::Scalar committedMaps;
        /** Stat for total number of mappings that were undone due to a
         *  squash. */
        statistics::Scalar undoneMaps;
        /** Number of serialize instructions handled. */
        statistics::Scalar serializing;
        /** Number of instructions marked as temporarily serializing. */
        statistics::Scalar tempSerializing;
        /** Number of instructions inserted into skid buffers. */
        statistics::Scalar skidInsts;

        statistics::Scalar moveEliminated;
        statistics::Scalar constantFolded;

        statistics::Vector stallEvents;
    } stats;

    std::vector<StallReason> renameStalls;

    StallReason blockReason{NoStall};

    RenamePrepareResult lastPrepareResult;

    void setAllStalls(StallReason renameStall);

    StallReason checkRenameStallFromIEW(
            ThreadID tid, const TimeStruct *iew_input);

    SquashVersion localSquashVer;

    /** Value predictor */
    valuepred::VPUnit *valuePred;

    /** Enable selective VP flush path */
    bool enableSelectiveVPFlush;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RENAME_HH__
