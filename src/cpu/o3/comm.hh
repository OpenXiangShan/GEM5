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
    CommitSquash,  // BS
    OtherStall,  // B
    NumStallReasons
};

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
    uint64_t squashedStreamId[MaxThreads];
    uint64_t squashedTargetId[MaxThreads];
    uint64_t squashedLoopIter[MaxThreads];
    std::unique_ptr<PCStateBase> pc[MaxThreads];

    bool squash[MaxThreads];
    bool branchMispredict[MaxThreads];
    bool branchTaken[MaxThreads];
    bool includeSquashInst[MaxThreads];
};

struct IssueStruct
{
    int size;

    DynInstPtr insts[MaxWidth];
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
        bool larger = version > other && version - other <= maxInflightSquash;
        bool wrapped_larger =
            version + versionLimit > other &&
            version + versionLimit - other <= maxInflightSquash;
        if (!(larger || wrapped_larger || (version == other))) {
            panic("SquashVersion: %d, other: %d\n", version, other);
        }
        return larger || wrapped_larger;
    }
    void update(uint8_t v) {
        version = v;
    }
    SquashVersion(uint8_t v) : version(v) {}
    SquashVersion() : version(0) {}
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

    DecodeComm decodeInfo[MaxThreads];

    struct RenameComm
    {
        StallReason blockReason;
    };

    RenameComm renameInfo[MaxThreads];

    struct IewComm
    {
        // Also eventually include skid buffer space.
        unsigned freeLQEntries;
        unsigned freeSQEntries;
        unsigned dispatchedToLQ;
        unsigned dispatchedToSQ;

        unsigned ldstqCount;

        unsigned dispatched;
        bool usedIQ;
        bool usedLSQ;

        StallReason robHeadStallReason;
        StallReason blockReason;
        StallReason lqHeadStallReason;
        StallReason sqHeadStallReason;
    };

    IewComm iewInfo[MaxThreads];

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

        uint64_t doneFsqId; // F
        uint64_t squashedStreamId; // F
        uint64_t squashedTargetId; // F
        unsigned squashedLoopIter; // F

        /// Tell Rename how many free entries it has in the ROB
        unsigned freeROBEntries; // *R

        bool isTrapSquash;
        bool squash; // *F, D, R, I
        bool robSquashing; // *F, D, R, I

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

    };

    CommitComm commitInfo[MaxThreads];

    bool decodeBlock[MaxThreads];
    bool decodeUnblock[MaxThreads];
    bool renameBlock[MaxThreads];
    bool renameUnblock[MaxThreads];
    bool iewBlock[MaxThreads];
    bool iewUnblock[MaxThreads];
};

} // namespace o3
} // namespace gem5

#endif //__CPU_O3_COMM_HH__
