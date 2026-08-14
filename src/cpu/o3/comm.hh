/*
 * Copyright (c) 2011, 2016-2017 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
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

#ifndef __CPU_O3_COMM_HH__
#define __CPU_O3_COMM_HH__

#include <vector>

#include "arch/generic/pcstate.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "sim/faults.hh"

namespace gem5
{

namespace o3
{

/** stall reasons in each stages*/
enum StallReason {
    NoStall,  // Base
    IcacheStall,  // F
    ITlbStall,  // F
    DTlbStall,  // B
    BpStall,  // BS, bad speculation: Frontend is squashed
    IntStall,  // F
    TrapStall,  // F
    FTQBubble,  // F
    FetchFragStall,  // F
    OtherFetchStall,  // F
    OtherFragStall,
    SquashStall,  // BS
    FetchBufferInvalid,  // Never used
    InstMisPred,  // BS
    InstSquashed,  // BS
    SerializeStall,  // F
    ScalarLongExecute,  // B
    VectorLongExecute,  // B
    InstNotReady,  // B

    LoadL1Bound,
    LoadL2Bound,
    LoadL3Bound,
    LoadMemBound,
    StoreL1Bound,
    StoreL2Bound,
    StoreL3Bound,
    StoreMemBound,
    MemSquashed,  // maybe never used
    MemNotReady,
    MemCommitRateLimit,
    Atomic,
    OtherMemStall,  // B

    MemDQBandwidth,
    IntDQBandwidth,
    FVDQBandwidth,
    VectorReadyButNotIssued,  // B
    ScalarReadyButNotIssued,  // B
    ResumeUnblock,  // B
    CommitSquash,  // BS, cause not attributable
    ControlRecovery,  // BS
    MemVioRecovery,  // BS
    VPRecovery,  // BS
    TrapRecovery,  // BS
    ROBFull,  // B
    RegFull,  // B
    OtherStall,  // B
    NumStallReasons
};

/**
 * Why commit squashed. IEW cannot derive this itself: `squashAll()` clears
 * `mispredictInst`, and nothing cause-bearing is live during the
 * `robSquashing` tail.
 *
 * `None` must stay zero; TimeBuffer::advance() memsets new slots.
 */
enum class SquashCause
{
    None = 0,
    BranchMispredict,
    MemOrderViolation,
    ValuePrediction,
    Trap,
    ThreadContext,
    SquashAfter,
};

inline StallReason
squashCauseToStallReason(SquashCause cause)
{
    switch (cause) {
      case SquashCause::BranchMispredict:
        return StallReason::ControlRecovery;
      case SquashCause::MemOrderViolation:
        return StallReason::MemVioRecovery;
      case SquashCause::ValuePrediction:
        return StallReason::VPRecovery;
      case SquashCause::Trap:
        return StallReason::TrapRecovery;
      // TC writes / squash-after are simulator-side clears, not
      // microarchitectural events worth their own bucket.
      default:
        return StallReason::CommitSquash;
    }
}

/** Struct that defines the information passed from fetch to decode. */
struct FetchStruct
{
    int size;

    DynInstPtr insts[MaxWidth];
    Fault fetchFault;
    InstSeqNum fetchFaultSN;
    bool clearFetchFault;
    std::vector<StallReason> fetchStallReason;
};

/** Struct that defines the information passed from decode to rename. */
struct DecodeStruct
{
    int size;

    DynInstPtr insts[MaxWidth];
    std::vector<StallReason> fetchStallReason;
    std::vector<StallReason> decodeStallReason;
};

/** Struct that defines the information passed from rename to IEW. */
struct RenameStruct
{
    int size;

    DynInstPtr insts[MaxWidth];
    std::vector<StallReason> fetchStallReason;
    std::vector<StallReason> decodeStallReason;
    std::vector<StallReason> renameStallReason;
};

/** Struct that defines the information passed from IEW to commit. */
struct IEWStruct
{
    int size;

    DynInstPtr insts[MaxWidth];
    DynInstPtr mispredictInst[MaxThreads];
    Addr mispredPC[MaxThreads];
    InstSeqNum squashedSeqNum[MaxThreads];
    uint64_t squashedTargetId[MaxThreads];
    uint64_t squashedLoopIter[MaxThreads];
    std::unique_ptr<PCStateBase> pc[MaxThreads];

    bool squash[MaxThreads];
    bool branchMispredict[MaxThreads];
    bool branchTaken[MaxThreads];
    bool includeSquashInst[MaxThreads];

    bool valuePredictionError[MaxThreads];
};

struct IssueStruct
{
    int size;

    DynInstPtr insts[MaxWidth];
};

struct SquashInfo
{
    InstSeqNum squashSn;
    ThreadID   squashTid;
};

struct SquashVersion
{
    uint8_t version;
    const static uint8_t versionLimit = 16;
    const static uint8_t maxVersion = versionLimit - 1;
    const static uint8_t maxInflightSquash = 7;
    uint8_t getVersion() const {
        return version;
    }
    uint8_t nextVersion() const {
        return (version + 1) % versionLimit;
    }
    bool largerThan(uint8_t other) const {
        const uint8_t distance = (version + versionLimit - other) % versionLimit;
        if (distance == 0) {
            return false;
        }

        if (distance <= maxInflightSquash) {
            return true;
        }

        if (versionLimit - distance <= maxInflightSquash) {
            return false;
        }

        if (version != other) {
            panic("SquashVersion: %d, other: %d\n", version, other);
        }
        return false;
    }
    void update(uint8_t v) {
        version = v;
    }
    SquashVersion(uint8_t v) : version(v) {}
    SquashVersion() : version(0) {}
};

struct ResolveQueueEntry
{
    ThreadID resolvedTid;
    uint64_t resolvedFTQId;
    std::vector<uint64_t> resolvedInstPC;
};

/** Struct that defines all backwards communication. */
struct TimeStruct
{
    struct DecodeComm
    {
        std::unique_ptr<PCStateBase> nextPC;
        DynInstPtr mispredictInst;
        DynInstPtr squashInst;
        InstSeqNum doneSeqNum;
        Addr mispredPC;
        uint64_t branchAddr;
        unsigned branchCount;
        bool squash;
        bool predIncorrect;
        bool branchMispredict;
        bool branchTaken;

        StallReason blockReason;
    };

    DecodeComm decodeInfo[MaxThreads]; // decode to fetch

    struct RenameComm
    {
        StallReason blockReason;
    };

    RenameComm renameInfo[MaxThreads]; // rename to decode

    struct IewComm
    {
        StallReason robHeadStallReason;
        StallReason blockReason;
        StallReason lqHeadStallReason;
        StallReason sqHeadStallReason;

        struct ResolvedCFIEntry
        {
            uint64_t ftqId;
            uint64_t pc;
        };
        /** Resolved control-flow PCs produced this cycle (fetch buffers/merges). */
        std::vector<ResolvedCFIEntry> resolvedCFIs;  // *F

        /** IEW detected a redirect before the delayed formal squash reaches Fetch. */
        bool redirectPending = false;  // *F

        unsigned iqCount;
        unsigned ldstqCount;
        unsigned robCount;
    };

    IewComm iewInfo[MaxThreads]; // iew to rename, fetch

    struct CommitComm
    {
        /////////////////////////////////////////////////////////////////////
        // This code has been re-structured for better packing of variables
        // instead of by stage which is the more logical way to arrange the
        // data.
        // F = Fetch
        // D = Decode
        // I = IEW
        // R = Rename
        // As such each member is annotated with who consumes it
        // e.g. bool variable name // *F,R for Fetch and Rename
        /////////////////////////////////////////////////////////////////////

        /// The pc of the next instruction to execute. This is the next
        /// instruction for a branch mispredict, but the same instruction for
        /// order violation and the like
        std::unique_ptr<PCStateBase> pc; // *F
        Addr committedPC; // *F for trap squash

        /// Provide fetch the instruction that mispredicted, if this
        /// pointer is not-null a misprediction occured
        DynInstPtr mispredictInst;  // *F

        /// Instruction that caused the a non-mispredict squash
        DynInstPtr squashInst; // *F

        /// Hack for now to send back a strictly ordered access to the
        /// IEW stage.
        DynInstPtr strictlyOrderedLoad; // *I

        /// Communication specifically to the IQ to tell the IQ that it can
        /// schedule a non-speculative instruction.
        InstSeqNum nonSpecSeqNum; // *I

        /// Represents the instruction that has either been retired or
        /// squashed.  Similar to having a single bus that broadcasts the
        /// retired or squashed sequence number.
        InstSeqNum doneSeqNum; // *F, I

        InstSeqNum doneMemSeqNum;

        InstSeqNum robheadSeqNum;

        uint64_t doneFtqId; // F
        uint64_t squashedTargetId; // F
        unsigned squashedLoopIter; // F

        bool isTrapSquash;
        bool squash; // *F, D, R, I
        bool robSquashing; // *F, D, R, I

        /// Re-published on every `robSquashing` cycle, so IEW needs no copy.
        SquashCause squashCause; // *I

        SquashVersion squashVersion; // *F, D, R, I

        /// Rename should re-read number of free rob entries
        bool usedROB; // *R

        /// Notify Rename that the ROB is empty
        bool emptyROB; // *R

        /// Was the branch taken or not
        bool branchTaken; // *F
        /// If an interrupt is pending and fetch should stall
        bool interruptPending; // *F
        /// If the interrupt ended up being cleared before being handled
        bool clearInterrupt; // *F

        /// Hack for now to send back an strictly ordered access to
        /// the IEW stage.
        bool strictlyOrdered; // *I

        // Trace ctrl-flow faults: notify fetch how far to rollback trace reader.
        InstSeqNum traceTrapSeqNum; // *F
        bool traceTrapSkipInst;     // *F

    };

    CommitComm commitInfo[MaxThreads];// commit to iew, rename, fetch
};

inline bool
smtCanDonateRobHeadroom(StallReason reason)
{
    switch (reason) {
      case NoStall:
      case ROBFull:
      case RegFull:
      case MemDQBandwidth:
      case IntDQBandwidth:
      case FVDQBandwidth:
      case VectorReadyButNotIssued:
      case ScalarReadyButNotIssued:
      case CommitSquash:
      case ControlRecovery:
      case MemVioRecovery:
      case VPRecovery:
      case TrapRecovery:
        return false;
      default:
        return true;
    }
}

inline bool
smtIsMemoryPressureReason(StallReason reason)
{
    switch (reason) {
      case DTlbStall:
      case LoadL2Bound:
      case LoadL3Bound:
      case LoadMemBound:
      case StoreL2Bound:
      case StoreL3Bound:
      case StoreMemBound:
      case MemSquashed:
      case MemNotReady:
      case MemCommitRateLimit:
      case Atomic:
      case OtherMemStall:
        return true;
      default:
        return false;
    }
}

inline bool
smtHasBorrowThrottleStall(const TimeStruct::IewComm &info)
{
    return smtCanDonateRobHeadroom(info.robHeadStallReason) ||
           smtCanDonateRobHeadroom(info.lqHeadStallReason) ||
           smtCanDonateRobHeadroom(info.sqHeadStallReason);
}

inline bool
smtHasMemoryPressure(const TimeStruct::IewComm &info,
                     unsigned ldstqHighWater = 0)
{
    if (ldstqHighWater != 0 && info.ldstqCount >= ldstqHighWater) {
        return true;
    }

    return smtIsMemoryPressureReason(info.robHeadStallReason) ||
           smtIsMemoryPressureReason(info.lqHeadStallReason) ||
           smtIsMemoryPressureReason(info.sqHeadStallReason);
}

inline uint64_t
smtBorrowPriority(const TimeStruct::IewComm &info)
{
    constexpr uint64_t backend_stall_penalty = 1ULL << 48;
    constexpr uint64_t memory_pressure_penalty = 1ULL << 49;

    uint64_t score = static_cast<uint64_t>(info.robCount) +
                     static_cast<uint64_t>(info.iqCount) * 2 +
                     static_cast<uint64_t>(info.ldstqCount) * 4;

    if (smtHasBorrowThrottleStall(info)) {
        score += backend_stall_penalty;
    }
    if (smtHasMemoryPressure(info)) {
        score += memory_pressure_penalty;
    }

    return score;
}

struct SmtActiveThreadFreeze
{
    ThreadID previousActive = InvalidThreadID;
    bool freezeCurrent = false;
};

class SmtActiveThreadArbiter
{
  public:
    static constexpr uint64_t InvalidScore = static_cast<uint64_t>(-1);

    SmtActiveThreadFreeze observe(ThreadID tid, uint64_t score)
    {
        if (score < bestScore) {
            selectedTid = tid;
            bestScore = score;
        }

        if (freezeActive) {
            SmtActiveThreadFreeze freeze;
            freeze.freezeCurrent = true;
            return freeze;
        }

        if (firstActiveTid == InvalidThreadID) {
            firstActiveTid = tid;
            return {};
        }

        freezeActive = true;
        SmtActiveThreadFreeze freeze;
        freeze.previousActive = firstActiveTid;
        freeze.freezeCurrent = true;
        return freeze;
    }

    ThreadID selected() const { return selectedTid; }

  private:
    ThreadID selectedTid = InvalidThreadID;
    ThreadID firstActiveTid = InvalidThreadID;
    bool freezeActive = false;
    uint64_t bestScore = InvalidScore;
};


struct StallSignals
{
    StallSignals()
    {
        for (int i = 0; i < MaxThreads; ++i) {
            blockFetch[i] = false;
            blockDecode[i] = false;
            blockRename[i] = false;
            blockIEW[i] = false;
            fetchBlockReason[i] = StallReason::NoStall;
            decodeBlockReason[i] = StallReason::NoStall;
            renameBlockReason[i] = StallReason::NoStall;
            iewBlockReason[i] = StallReason::NoStall;
        }
    }

    bool blockFetch[MaxThreads];// decode to fetch
    bool blockDecode[MaxThreads];// rename to decode
    bool blockRename[MaxThreads];// iew to rename (if iew is stalling, rename all threads would be stalled)
    bool blockIEW[MaxThreads];// commit to iew
    StallReason fetchBlockReason[MaxThreads];// decode to fetch root cause
    StallReason decodeBlockReason[MaxThreads];// rename to decode root cause
    StallReason renameBlockReason[MaxThreads];// iew to rename root cause
    StallReason iewBlockReason[MaxThreads];// commit to iew root cause
};


} // namespace o3
} // namespace gem5

#endif //__CPU_O3_COMM_HH__
