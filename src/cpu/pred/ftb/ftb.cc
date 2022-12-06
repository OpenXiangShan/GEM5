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
    numBr = p.numBr;
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
    if (hit) {
        DPRINTF(FTB, "FTB: lookup hit, dumping hit entry\n");
        printFTBEntry(find_entry);
    } else {
        DPRINTF(FTB, "FTB: lookup miss\n");
    }
    // assign prediction for s2 and later stages
    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        DPRINTF(FTB, "FTB: assigning prediction for stage %d\n", s);
        stagePreds[s].valid = hit;
        stagePreds[s].ftbEntry = find_entry;
        DPRINTF(FTB, "FTB: numBranches %d\n", numBr);
        stagePreds[s].condTakens.clear(); // TODO: do this in generateFinalPredAndCreateOverrideBubbles
        for (int i = 0; i < numBr; ++i) {
            stagePreds[s].condTakens.push_back(false);
        }
        // TODO: this is a hack to get a valid target for indirect branches
        if (!find_entry.slots.empty()) {
            auto tail_slot = find_entry.slots.back();
            if (tail_slot.uncondValid()) {
                stagePreds[s].indirectTarget = tail_slot.target;
                if (tail_slot.isReturn) {
                    stagePreds[s].returnTarget = tail_slot.target;
                }
            }
        }
    }
    meta = FTBMeta(hit);
}

std::shared_ptr<void>
DefaultFTB::getPredictionMeta()
{
    std::shared_ptr<void> meta_void_ptr = std::make_shared<FTBMeta>(meta.hit);
    return meta_void_ptr;
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
DefaultFTB::update(FetchStream &stream, ThreadID tid)
{
    DPRINTF(FTB, "FTB: Updating FTB entry\n");
    // generate ftb entry
    Addr startPC = stream.startPC;
    unsigned ftb_idx = getIndex(startPC, tid);
    Addr inst_tag = getTag(startPC);

    bool pred_hit = stream.isHit;
    bool pred_hit_from_meta = std::static_pointer_cast<FTBMeta>(stream.predMetas[0])->hit; //TODO: get component idx
    assert(pred_hit == pred_hit_from_meta);

    bool stream_taken = stream.exeTaken;
    FTBEntry entry_to_write;
    if (pred_hit || stream_taken) {
        BranchInfo branch_info = stream.exeBranchInfo;
        bool is_uncond = branch_info.isUncond();
        // if pred not hit, establish a new entry
        if (!pred_hit) {
            DPRINTF(FTB, "pred miss, creating new FTB entry\n");
            FTBEntry new_entry;
            new_entry.valid = true;
            new_entry.tag = inst_tag;
            new_entry.tid = tid;
            std::vector<FTBSlot> &slots = new_entry.slots;
            FTBSlot new_slot = FTBSlot(branch_info);
            slots.push_back(new_slot);
            // uncond branch should set fallThruAddr to end of that inst
            if (is_uncond) {
                new_entry.fallThruAddr = branch_info.getEnd();
            } else {
                new_entry.fallThruAddr = startPC + 32;
            }
            entry_to_write = new_entry;
        } else {
            DPRINTF(FTB, "pred hit, updating FTB entry if necessary\n");
            DPRINTF(FTB, "printing old entry:\n");
            FTBEntry old_entry = stream.predFTBEntry;
            printFTBEntry(old_entry);
            assert(old_entry.tag == inst_tag && old_entry.tid == tid && old_entry.valid);
            std::vector<FTBSlot> &slots = old_entry.slots;
            bool new_branch = !branchIsInEntry(old_entry, branch_info.pc);
            if (new_branch && stream_taken) {
                DPRINTF(FTB, "new taken branch detected, inserting into FTB entry\n");
                // keep pc ascending order
                auto it = slots.begin();
                while (it != slots.end()) {
                    if (*it > branch_info) {
                        break;
                    }
                    ++it;
                }
                slots.insert(it, FTBSlot(branch_info));
                // remove the last slot if there are more than numBr slots
                if (slots.size() > numBr) {
                    DPRINTF(FTB, "removing last slot because there are more than %d slots", numBr);
                    Addr last_slot_pc = slots.rbegin()->pc;
                    slots.pop_back();
                    old_entry.fallThruAddr = last_slot_pc;
                }
            }
            entry_to_write = old_entry;
        }
        DPRINTF(FTB, "printing new entry:\n");
        printFTBEntry(entry_to_write);
        ftb[ftb_idx] = entry_to_write;
    }

    assert(ftb_idx < numEntries);

    // ftb[ftb_idx].tid = tid;
    // ftb[ftb_idx].valid = true;
    // set(ftb[ftb_idx].target, target);
    // ftb[ftb_idx].tag = getTag(inst_pc);
}

} // namespace ftb_pred
} // namespace branch_prediction
} // namespace gem5
