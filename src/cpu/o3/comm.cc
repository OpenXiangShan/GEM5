/*
 * Copyright (c) 2026 The Regents of The University of Michigan
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

#include "cpu/o3/comm.hh"

#include "cpu/o3/dyn_inst.hh"

namespace gem5
{

namespace o3
{

IEWStruct::IEWStruct(const IEWStruct &other)
{
    *this = other;
}

IEWStruct &
IEWStruct::operator=(const IEWStruct &other)
{
    if (this == &other)
        return *this;

    size = other.size;
    for (int i = 0; i < MaxWidth; ++i)
        insts[i] = other.insts[i];
    for (int i = 0; i < MaxThreads; ++i) {
        mispredictInst[i] = other.mispredictInst[i];
        mispredPC[i] = other.mispredPC[i];
        squashedSeqNum[i] = other.squashedSeqNum[i];
        squashedTargetId[i] = other.squashedTargetId[i];
        squashedLoopIter[i] = other.squashedLoopIter[i];
        pc[i] = clonePCState(other.pc[i]);
        squash[i] = other.squash[i];
        branchMispredict[i] = other.branchMispredict[i];
        branchTaken[i] = other.branchTaken[i];
        includeSquashInst[i] = other.includeSquashInst[i];
        valuePredictionError[i] = other.valuePredictionError[i];
    }

    return *this;
}

TimeStruct::TimeStruct(const TimeStruct &other)
{
    *this = other;
}

TimeStruct &
TimeStruct::operator=(const TimeStruct &other)
{
    if (this == &other)
        return *this;

    for (int i = 0; i < MaxThreads; ++i) {
        decodeInfo[i] = other.decodeInfo[i];
        renameInfo[i] = other.renameInfo[i];
        iewInfo[i] = other.iewInfo[i];
        commitInfo[i] = other.commitInfo[i];
    }

    return *this;
}

TimeStruct::DecodeComm::DecodeComm(const DecodeComm &other)
{
    *this = other;
}

TimeStruct::DecodeComm &
TimeStruct::DecodeComm::operator=(const DecodeComm &other)
{
    if (this == &other)
        return *this;

    nextPC = clonePCState(other.nextPC);
    mispredictInst = other.mispredictInst;
    squashInst = other.squashInst;
    doneSeqNum = other.doneSeqNum;
    mispredPC = other.mispredPC;
    branchAddr = other.branchAddr;
    branchCount = other.branchCount;
    squash = other.squash;
    predIncorrect = other.predIncorrect;
    branchMispredict = other.branchMispredict;
    branchTaken = other.branchTaken;
    blockReason = other.blockReason;
    return *this;
}

TimeStruct::CommitComm::CommitComm(const CommitComm &other)
{
    *this = other;
}

TimeStruct::CommitComm &
TimeStruct::CommitComm::operator=(const CommitComm &other)
{
    if (this == &other)
        return *this;

    pc = clonePCState(other.pc);
    committedPC = other.committedPC;
    mispredictInst = other.mispredictInst;
    squashInst = other.squashInst;
    strictlyOrderedLoad = other.strictlyOrderedLoad;
    nonSpecSeqNum = other.nonSpecSeqNum;
    doneSeqNum = other.doneSeqNum;
    doneMemSeqNum = other.doneMemSeqNum;
    robheadSeqNum = other.robheadSeqNum;
    doneFtqId = other.doneFtqId;
    squashedTargetId = other.squashedTargetId;
    squashedLoopIter = other.squashedLoopIter;
    isTrapSquash = other.isTrapSquash;
    squash = other.squash;
    robSquashing = other.robSquashing;
    squashVersion = other.squashVersion;
    usedROB = other.usedROB;
    emptyROB = other.emptyROB;
    branchTaken = other.branchTaken;
    interruptPending = other.interruptPending;
    clearInterrupt = other.clearInterrupt;
    strictlyOrdered = other.strictlyOrdered;
    traceTrapSeqNum = other.traceTrapSeqNum;
    traceTrapSkipInst = other.traceTrapSkipInst;
    return *this;
}

} // namespace o3
} // namespace gem5
