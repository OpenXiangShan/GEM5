/*
 * Copyright (c) 2004-2005 The Regents of The University of Michigan
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


#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/pred/ftb/ftb.hh"
#include "debug/Fetch.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

DefaultFTB::DefaultFTB(const Params &p)
    : TimedBaseFTBPredictor(p)
{
    numEntries = p.numEntries;
    tagBits = p.tagBits;
    instShiftAmt = p.instShiftAmt;
    log2NumThreads = floorLog2(p.numThreads);
    DPRINTF(Fetch, "FTB: Creating FTB object.\n");

    if (!isPowerOf2(numEntries)) {
        fatal("FTB entries is not a power of 2!");
    }

    ftb.resize(numEntries);

    for (unsigned i = 0; i < numEntries; ++i) {
        ftb[i].valid = false;
    }

    idxMask = numEntries - 1;

    tagMask = (1 << tagBits) - 1;

    tagShiftAmt = instShiftAmt + floorLog2(numEntries);
}

void
DefaultFTB::tickStart()
{
    // nothing to do
}

void
DefaultFTB::tick() {}

void
DefaultFTB::putPCHistory(Addr startAddr,
                         const boost::dynamic_bitset<> &history,
                         std::array<FullFTBPrediction, 3> &stagePreds)
{
    // TODO: getting startAddr from pred is ugly
    FTBEntry find_entry = lookup(startAddr, 0);
    bool hit = find_entry.valid;
    // assign prediction for s2 and later stages
    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        stagePreds[s].valid = hit;
        stagePreds[s].ftbEntry = find_entry;
    }
}

void
DefaultFTB::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) {}

void
DefaultFTB::reset()
{
    for (unsigned i = 0; i < numEntries; ++i) {
        ftb[i].valid = false;
    }
}

inline
unsigned
DefaultFTB::getIndex(Addr instPC, ThreadID tid)
{
    // Need to shift PC over by the word offset.
    return ((instPC >> instShiftAmt)
            ^ (tid << (tagShiftAmt - instShiftAmt - log2NumThreads)))
            & idxMask;
}

inline
Addr
DefaultFTB::getTag(Addr instPC)
{
    return (instPC >> tagShiftAmt) & tagMask;
}

bool
DefaultFTB::valid(Addr instPC, ThreadID tid)
{
    unsigned ftb_idx = getIndex(instPC, tid);

    Addr inst_tag = getTag(instPC);

    assert(ftb_idx < numEntries);

    if (ftb[ftb_idx].valid
        && inst_tag == ftb[ftb_idx].tag
        && ftb[ftb_idx].tid == tid) {
        return true;
    } else {
        return false;
    }
}

// @todo Create some sort of return struct that has both whether or not the
// address is valid, and also the address.  For now will just use addr = 0 to
// represent invalid entry.
FTBEntry
DefaultFTB::lookup(Addr inst_pc, ThreadID tid)
{
    unsigned ftb_idx = getIndex(inst_pc, tid);

    Addr inst_tag = getTag(inst_pc);

    assert(ftb_idx < numEntries);

    if (ftb[ftb_idx].valid
        && inst_tag == ftb[ftb_idx].tag
        && ftb[ftb_idx].tid == tid) {
        return ftb[ftb_idx];
    } else {
        return FTBEntry();
    }
}

// TODO:: generate/update FTBentry with given info
void
DefaultFTB::update(Addr inst_pc, const PCStateBase &target, ThreadID tid)
{
    unsigned ftb_idx = getIndex(inst_pc, tid);

    assert(ftb_idx < numEntries);

    // ftb[ftb_idx].tid = tid;
    // ftb[ftb_idx].valid = true;
    // set(ftb[ftb_idx].target, target);
    // ftb[ftb_idx].tag = getTag(inst_pc);
}

} // namespace ftb_pred
} // namespace branch_prediction
} // namespace gem5
