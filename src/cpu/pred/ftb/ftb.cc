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
    numWays = p.numWays;
    assert(numEntries % numWays == 0);
    numSets = numEntries / numWays;
    numDelay = p.numDelay;

    if (!isPowerOf2(numEntries)) {
        fatal("FTB entries is not a power of 2!");
    }

    ftb.resize(numSets);
    mruList.resize(numSets);
    for (unsigned i = 0; i < numSets; ++i) {
        for (unsigned j = 0; j < numWays; ++j) {
            ftb[i][0xfffffff-j]; // dummy initialization
        }
        auto &set = ftb[i];
        for (auto it = set.begin(); it != set.end(); it++) {
            it->second.valid = false;
            mruList[i].push_back(it);
        }
        std::make_heap(mruList[i].begin(), mruList[i].end(), older());
    }


    idxMask = numSets - 1;

    tagMask = (1UL << tagBits) - 1;

    tagShiftAmt = instShiftAmt + floorLog2(numSets);
    DPRINTF(FTB, "numEntries %d, numSets %d, numWays %d, tagBits %d, tagShiftAmt %d, idxMask %#lx, tagMask %#lx\n",
        numEntries, numSets, numWays, tagBits, tagShiftAmt, idxMask, tagMask);
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
    TickedFTBEntry find_entry = lookup(startAddr);
    bool hit = find_entry.valid;
    if (hit) {
        DPRINTF(FTB, "FTB: lookup hit, dumping hit entry\n");
        printFTBEntry(find_entry);
    } else {
        DPRINTF(FTB, "FTB: lookup miss\n");
    }
    // assign prediction for s2 and later stages
    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        if (!isL0() && !hit && stagePreds[s].valid) {
            DPRINTF(FTB, "FTB: uftb hit and ftb miss, use uftb result");
            break;
        }
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
        stagePreds[s].predTick = curTick();
    }
    if (getDelay() >= 1) {
        meta.l0_hit = stagePreds[getDelay() - 1].valid;
    }
    meta.hit = hit;
}

std::shared_ptr<void>
DefaultFTB::getPredictionMeta()
{
    std::shared_ptr<void> meta_void_ptr = std::make_shared<FTBMeta>(meta);
    return meta_void_ptr;
}

void
DefaultFTB::specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) {}

void
DefaultFTB::reset()
{
    for (unsigned i = 0; i < numSets; ++i) {
        for (unsigned j = 0; j < numWays; ++j) {
            ftb[i][j].valid = false;
        }
    }
}

inline
Addr
DefaultFTB::getIndex(Addr instPC)
{
    // Need to shift PC over by the word offset.
    return (instPC >> instShiftAmt) & idxMask;
}

inline
Addr
DefaultFTB::getTag(Addr instPC)
{
    return (instPC >> tagShiftAmt) & tagMask;
}

bool
DefaultFTB::valid(Addr instPC)
{
    Addr ftb_idx = getIndex(instPC);

    Addr inst_tag = getTag(instPC);

    assert(ftb_idx < numEntries);

    for (int w = 0; w < numWays; w++) {
        if (ftb[ftb_idx][w].valid
            && inst_tag == ftb[ftb_idx][w].tag) {
            return true;
        }
    }
    return false;
}

// @todo Create some sort of return struct that has both whether or not the
// address is valid, and also the address.  For now will just use addr = 0 to
// represent invalid entry.
DefaultFTB::TickedFTBEntry
DefaultFTB::lookup(Addr inst_pc)
{
    Addr ftb_idx = getIndex(inst_pc);

    Addr ftb_tag = getTag(inst_pc);
    DPRINTF(FTB, "FTB: Looking up FTB entry index %#lx tag %#lx\n", ftb_idx, ftb_tag);

    assert(ftb_idx < numSets);

    const auto &it = ftb[ftb_idx].find(ftb_tag);
    if (it != ftb[ftb_idx].end()) {
        if (it->second.valid) {
            it->second.tick = curTick();
            std::make_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
            return it->second;
        }
    }
    return TickedFTBEntry();
}

void
DefaultFTB::getAndSetNewFTBEntry(FetchStream &stream)
{
    DPRINTF(FTB, "generating new ftb entry\n");
    // generate ftb entry
    Addr startPC = stream.startPC;
    Addr inst_tag = getTag(startPC);


    bool pred_hit = stream.isHit;

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
            // printFTBEntry(old_entry);
            assert(old_entry.tag == inst_tag && old_entry.valid);
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
        // printFTBEntry(entry_to_write);
    }
    stream.updateFTBEntry = entry_to_write;
}

void
DefaultFTB::update(const FetchStream &stream)
{
    if (!isL0()) {
        // TODO: get component idx
        auto meta = std::static_pointer_cast<FTBMeta>(stream.predMetas[1]);
        bool l0_hit_l1_miss = meta->l0_hit && !meta->hit;
        if (l0_hit_l1_miss) {
            DPRINTF(FTB, "FTB: skipping entry write because of l0 hit\n");
            return;
        }
    }
    Addr startPC = stream.startPC;
    Addr ftb_idx = getIndex(startPC);
    Addr ftb_tag = getTag(startPC);

    DPRINTF(FTB, "FTB: Updating FTB entry index %#lx tag %#lx\n", ftb_idx, ftb_tag);

    auto it = ftb[ftb_idx].find(ftb_tag);
    // if the tag is not found and the table is full
    bool new_entry = it == ftb[ftb_idx].end();

    if (new_entry) {
        std::pop_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
        const auto& old_entry = mruList[ftb_idx].back();
        DPRINTF(FTB, "FTB: Replacing entry with tag %#lx in set %#lx\n", old_entry->first, ftb_idx);
        ftb[ftb_idx].erase(old_entry->first);
    }

    ftb[ftb_idx][stream.updateFTBEntry.tag] = TickedFTBEntry(stream.updateFTBEntry, curTick());

    if (new_entry) {
        auto it = ftb[ftb_idx].find(stream.updateFTBEntry.tag);
        mruList[ftb_idx].back() = it;
        std::push_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
    } else {
        std::make_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
    }
    assert(ftb_idx < numSets);

    // ftb[ftb_idx].valid = true;
    // set(ftb[ftb_idx].target, target);
    // ftb[ftb_idx].tag = getTag(inst_pc);
}

} // namespace ftb_pred
} // namespace branch_prediction
} // namespace gem5
