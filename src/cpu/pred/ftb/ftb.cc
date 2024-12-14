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
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/ftb/ftb.hh"
#include "debug/Fetch.hh"

namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

DefaultFTB::DefaultFTB(const Params &p)
    : TimedBaseFTBPredictor(p),
    numEntries(p.numEntries),
    tagBits(p.tagBits),
    instShiftAmt(p.instShiftAmt),
    log2NumThreads(floorLog2(p.numThreads)),
    predictWidth(p.predictWidth),
    numBr(p.numBr),
    numWays(p.numWays),
    numSets(numEntries / numWays),
    ftbStats(this)
{
    assert(numEntries % numWays == 0);

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
                         std::vector<FullFTBPrediction> &stagePreds)
{
    TickedFTBEntry find_entry = lookup(startAddr);
    bool hit = find_entry.valid;
    if (hit) {
        DPRINTF(FTB, "FTB: lookup hit, dumping hit entry\n");
        ftbStats.predHit++;
        printTickedFTBEntry(find_entry);
    } else {
        ftbStats.predMiss++;

        DPRINTF(FTB, "FTB: lookup miss\n");
    }
    assert(getDelay() < stagePreds.size());
    // assign prediction for s2 and later stages
    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        if (!isL0() && !hit && stagePreds[s].valid) {
            DPRINTF(FTB, "FTB: uftb hit and ftb miss, use uftb result");
            incNonL0Stat(ftbStats.predUseL0OnL1Miss);
            break;
        }
        DPRINTF(FTB, "FTB: assigning prediction for stage %d\n", s);
        stagePreds[s].valid = hit;
        stagePreds[s].ftbEntry = find_entry;
        DPRINTF(FTB, "FTB: numBranches %d\n", numBr);

        if (isL0()) {
            // use saturating counter of L0 FTB
            for (int i = 0; i < numBr; ++i) {
                if (find_entry.slots.size() > i) {
                    stagePreds[s].condTakens[i] = find_entry.slots[i].ctr >= 0 && hit;
                } else {
                    stagePreds[s].condTakens[i] = false;
                }
            }
        }
        // assign ftb prediction for indirect targets
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
    meta.entry = FTBEntry(find_entry);
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
    // return (instPC >> instShiftAmt) & idxMask;
    // use xor to mix tag and index bits
    Addr idx = ((instPC >> instShiftAmt) ^ (instPC >> (tagShiftAmt)));
    return idx & idxMask;
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
    if (inst_pc & 0x1) {
        return TickedFTBEntry(); // ignore false hit when lowest bit is 1
    }
    Addr ftb_idx = getIndex(inst_pc);

    Addr ftb_tag = getTag(inst_pc);
    DPRINTF(FTB, "FTB: Looking up FTB entry index %#lx tag %#lx\n", ftb_idx, ftb_tag);

    assert(ftb_idx < numSets);
    // ignore false hit when lowest bit is 1
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
    Addr startPC = stream.getRealStartPC();
    Addr inst_tag = getTag(startPC);


    bool pred_hit = stream.isHit;

    bool stream_taken = stream.exeTaken;
    FTBEntry entry_to_write;
    bool is_old_entry = pred_hit;
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
                incNonL0Stat(ftbStats.newEntryWithUncond);
            } else {
                new_entry.fallThruAddr = startPC + predictWidth;
                incNonL0Stat(ftbStats.newEntryWithCond);
            }
            entry_to_write = new_entry;
            incNonL0Stat(ftbStats.newEntry);
        } else {
            DPRINTF(FTB, "pred hit, updating FTB entry if necessary\n");
            DPRINTF(FTB, "printing old entry:\n");
            FTBEntry old_entry = stream.predFTBEntry;
            printFTBEntry(old_entry);
            // assert(old_entry.tag == inst_tag && old_entry.valid);
            std::vector<FTBSlot> &slots = old_entry.slots;
            bool new_branch = !branchIsInEntry(old_entry, branch_info.pc);
            if (new_branch && stream_taken) {
                is_old_entry = false;
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
                // ensure uncond slot is the tail slot
                // Note: if an unconditional jump has a target equal to fallThruPC,
                //       predicting it to be not taken will not be considered a mispredict
                //       thus an ftq entry would possibly has two taken branches inside,
                //       among which the first being an unconditional jump to its fallThruPC
                if (branch_info.isUncond() && branchIsInEntry(old_entry, branch_info.pc)) {
                    // check if there is other branches behind an indirect jump
                    // remove slots behind an unconditional jump
                    FTBSlot back = slots.back();
                    while (slots.back() > branch_info) {
                        DPRINTF(FTB, "erasing slot behind uncond slot:\n");
                        DPRINTF(FTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d, always_taken:%d\n",
                            back.pc, back.size, back.target, back.isCond, back.isIndirect, back.isCall, back.isReturn, back.alwaysTaken);
                        slots.pop_back();
                        back = slots.back();
                    }
                    assert(back == branch_info);
                    DPRINTF(FTB, "setting fallThruAddr to the next inst of uncond: %#lx\n", old_entry.fallThruAddr);
                    old_entry.fallThruAddr = branch_info.pc + branch_info.size;
                }
                if (branch_info.isCond) {
                    incNonL0Stat(ftbStats.oldEntryWithNewCond);
                } else {
                    incNonL0Stat(ftbStats.oldEntryWithNewUncond);
                }
            }
            if (!new_branch && branch_info.isIndirect && stream_taken) {
                auto &tailSlot = slots.back();
                assert(tailSlot.isIndirect);
                if (tailSlot.target != branch_info.target) {
                    tailSlot.target = branch_info.target;
                    is_old_entry = false;
                    incNonL0Stat(ftbStats.oldEntryIndirectTargetModified);
                }
            }
            // modify always taken logic
            auto it = slots.begin();
            while (it != slots.end()) {
                // set branches before current branch to alwaysTaken: false
                if (*it < branch_info) {
                    if (it->alwaysTaken) {
                        it->alwaysTaken = false;
                        is_old_entry = false;
                    }
                }
                // current always taken branch not taken: alwaysTaken false
                else if (*it == branch_info && it->alwaysTaken && !stream_taken) {
                    is_old_entry = false;
                    it->alwaysTaken = false;
                }
                it++;
            }
            entry_to_write = old_entry;
            incNonL0Stat(ftbStats.oldEntry);
        }
        DPRINTF(FTB, "printing new entry:\n");
        printFTBEntry(entry_to_write);
        checkFTBEntry(entry_to_write);
    }
    stream.updateFTBEntry = entry_to_write;
    stream.updateIsOldEntry = is_old_entry;
}

void
DefaultFTB::update(const FetchStream &stream)
{
    auto meta = std::static_pointer_cast<FTBMeta>(stream.predMetas[getComponentIdx()]);
    if (meta->hit) {
        ftbStats.updateHit++;
    } else {
        ftbStats.updateMiss++;
    }
    if (!isL0()) {
        bool l0_hit_l1_miss = meta->l0_hit && !meta->hit;
        if (l0_hit_l1_miss) {
            DPRINTF(FTB, "FTB: skipping entry write because of l0 hit\n");
            incNonL0Stat(ftbStats.updateUseL0OnL1Miss);
            return;
        }
    }
    Addr startPC = stream.getRealStartPC();
    Addr ftb_idx = getIndex(startPC);
    Addr ftb_tag = getTag(startPC);

    DPRINTF(FTB, "FTB: Updating FTB entry index %#lx tag %#lx\n", ftb_idx, ftb_tag);

    auto it = ftb[ftb_idx].find(ftb_tag);
    // if the tag is not found and the table is full
    bool not_found = it == ftb[ftb_idx].end();

    if (not_found) {
        std::pop_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
        const auto& old_entry = mruList[ftb_idx].back();
        DPRINTF(FTB, "FTB: Replacing entry with tag %#lx in set %#lx\n", old_entry->first, ftb_idx);
        ftb[ftb_idx].erase(old_entry->first);
    }

    auto updatedEntry = stream.updateFTBEntry;
    bool updatedIsOldEntry = stream.updateIsOldEntry;
    auto entryInFtbNow = ftb[ftb_idx][ftb_tag];
    // if this entry is old entry, use entry now in ftb to avoid overwriting entry with more branche info
    auto entry_to_write = (updatedIsOldEntry && !not_found) ? FTBEntry(entryInFtbNow) : updatedEntry;
    // train L0 FTB ctrs
    if (isL0()) {
        std::vector<bool> need_to_update;
        need_to_update.resize(numBr, false);
        auto &ftb_entry = entry_to_write;
        // get number of conditional branches to update
        int cond_num = 0;
        if (stream.exeTaken) {
            cond_num = ftb_entry.getNumCondInEntryBefore(stream.exeBranchInfo.pc);
            // for case of ftb entry is not full
            if (cond_num < numBr) {
                cond_num += !stream.exeBranchInfo.isUncond() ? 1 : 0;
            }
            // if ftb entry is full, and this branch is conditional,
            // we cannot update the last branch, as it will be removed
            // from current ftb entry
        } else {
            // corresponding to RTL, but in fact we should consider
            // whether the branches are flushed
            // TODO: fix it and check whether it can bring performance improvement
            cond_num = ftb_entry.getTotalNumConds();
        }
        assert(cond_num <= numBr);

        // assert(cond_num <= ftb_entry.slots.size());
        cond_num = std::min(cond_num, (int)ftb_entry.slots.size());
        for (int i = 0; i < cond_num; i++) {
            auto &slot = ftb_entry.slots[i];
            // only update branches with both taken/not taken behaviors observed
            need_to_update[i] = !slot.alwaysTaken;
        }
        for (int b = 0; b < numBr; b++) {
            if (!need_to_update[b]) {
                continue;
            }
            bool this_cond_actually_taken = stream.exeTaken && stream.exeBranchInfo == ftb_entry.slots[b];
            int ctr_to_be_updated;
            // read newest ctr if hit
            if (!not_found && it->second.slots.size() > b) {
                ctr_to_be_updated = entryInFtbNow.slots[b].ctr;
            } else {
                ctr_to_be_updated = updatedEntry.slots[b].ctr;
            }
            updateCtr(ctr_to_be_updated, this_cond_actually_taken);
            entry_to_write.slots[b].ctr = ctr_to_be_updated;
        }
    }

    ftb[ftb_idx][ftb_tag] = TickedFTBEntry(entry_to_write, curTick());
    ftb[ftb_idx][ftb_tag].tag = ftb_tag; // in case different ftb has different tags


    if (not_found) {
        auto it = ftb[ftb_idx].find(ftb_tag);
        assert(it != ftb[ftb_idx].end());
        mruList[ftb_idx].back() = it;
        std::push_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
    } else {
        std::make_heap(mruList[ftb_idx].begin(), mruList[ftb_idx].end(), older());
    }
    assert(ftb_idx < numSets);
    assert(ftb[ftb_idx].size() <= numWays);
    assert(mruList[ftb_idx].size() <= numWays);

    // ftb[ftb_idx].valid = true;
    // set(ftb[ftb_idx].target, target);
    // ftb[ftb_idx].tag = getTag(inst_pc);
}

void
DefaultFTB::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
    auto meta = std::static_pointer_cast<FTBMeta>(stream.predMetas[getComponentIdx()]);
    auto &entry = meta->entry;
    auto pc = inst->getPC();
    auto npc = inst->getNPC();
    // auto &static_inst = inst->staticInst();
    bool this_branch_hit = meta->hit && branchIsInEntry(entry, pc);
    // bool this_branch_miss = !this_branch_hit;
    bool cond_not_taken = inst->isCondCtrl() && !inst->branching();
    bool this_branch_taken = !cond_not_taken; // all uncond should be taken
    Addr this_branch_target = npc;
    const auto &slot = entry.getSlot(pc);
    if (this_branch_hit) {
        ftbStats.allBranchHits++;
        if (this_branch_taken) {
            ftbStats.allBranchHitTakens++;
        } else {
            ftbStats.allBranchHitNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ftbStats.condHits++;
            if (this_branch_taken) {
                ftbStats.condHitTakens++;
            } else {
                ftbStats.condHitNotTakens++;
            }
            if (isL0()) {
                bool pred_taken = slot.ctr >= 0;
                if (pred_taken == this_branch_taken) {
                    ftbStats.condPredCorrect++;
                } else {
                    ftbStats.condPredWrong++;
                }
            }
        }
        if (inst->isUncondCtrl()) {
            ftbStats.uncondHits++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ftbStats.indirectHits++;
                Addr pred_target = slot.target;
                if (pred_target == this_branch_target) {
                    ftbStats.indirectPredCorrect++;
                } else {
                    ftbStats.indirectPredWrong++;
                }
            }
            if (inst->isCall()) {
                ftbStats.callHits++;
            }
            if (inst->isReturn()) {
                ftbStats.returnHits++;
            }
        }
    } else {
        ftbStats.allBranchMisses++;
        if (this_branch_taken) {
            ftbStats.allBranchMissTakens++;
        } else {
            ftbStats.allBranchMissNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ftbStats.condMisses++;
            if (this_branch_taken) {
                ftbStats.condMissTakens++;
                if (isL0()) {
                    // only L0 FTB has saturating counters to predict conditional branches
                    // taken branches that is missed in ftb must have been mispredicted
                    ftbStats.condPredWrong++;
                }
            } else {
                ftbStats.condMissNotTakens++;
                if (isL0()) {
                    // only L0 FTB has saturating counters to predict conditional branches
                    // taken branches that is missed in ftb must have been mispredicted
                    ftbStats.condPredCorrect++;
                }
            }
        }
        if (inst->isUncondCtrl()) {
            ftbStats.uncondMisses++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ftbStats.indirectMisses++;
                ftbStats.indirectPredWrong++;
            }
            if (inst->isCall()) {
                ftbStats.callMisses++;
            }
            if (inst->isReturn()) {
                ftbStats.returnMisses++;
            }
        }
    }
}

DefaultFTB::FTBStats::FTBStats(statistics::Group* parent) :
    statistics::Group(parent),
    ADD_STAT(newEntry, statistics::units::Count::get(), "number of new ftb entries generated"),
    ADD_STAT(newEntryWithCond, statistics::units::Count::get(), "number of new ftb entries generated with conditional branch"),
    ADD_STAT(newEntryWithUncond, statistics::units::Count::get(), "number of new ftb entries generated with unconditional branch"),
    ADD_STAT(oldEntry, statistics::units::Count::get(), "number of old ftb entries updated"),
    ADD_STAT(oldEntryIndirectTargetModified, statistics::units::Count::get(), "number of old ftb entries with indirect target modified"),
    ADD_STAT(oldEntryWithNewCond, statistics::units::Count::get(), "number of old ftb entries with new conditional branches"),
    ADD_STAT(oldEntryWithNewUncond, statistics::units::Count::get(), "number of old ftb entries with new unconditional branches"),
    ADD_STAT(predMiss, statistics::units::Count::get(), "misses encountered on prediction"),
    ADD_STAT(predHit, statistics::units::Count::get(), "hits encountered on prediction"),
    ADD_STAT(updateMiss, statistics::units::Count::get(), "misses encountered on update"),
    ADD_STAT(updateHit, statistics::units::Count::get(), "hits encountered on update"),
    ADD_STAT(eraseSlotBehindUncond, statistics::units::Count::get(), "erase slots behind unconditional slot"),
    ADD_STAT(predUseL0OnL1Miss, statistics::units::Count::get(), "use l0 result on l1 miss when pred"),
    ADD_STAT(updateUseL0OnL1Miss, statistics::units::Count::get(), "use l0 result on l1 miss when update"),

    ADD_STAT(allBranchHits, statistics::units::Count::get(), "all types of branches committed that was predicted hit"),
    ADD_STAT(allBranchHitTakens, statistics::units::Count::get(), "all types of taken branches committed was that predicted hit"),
    ADD_STAT(allBranchHitNotTakens, statistics::units::Count::get(), "all types of not taken branches committed was that predicted hit"),
    ADD_STAT(allBranchMisses, statistics::units::Count::get(), "all types of branches committed that was predicted miss"),
    ADD_STAT(allBranchMissTakens, statistics::units::Count::get(), "all types of taken branches committed was that predicted miss"),
    ADD_STAT(allBranchMissNotTakens, statistics::units::Count::get(), "all types of not taken branches committed was that predicted miss"),
    ADD_STAT(condHits, statistics::units::Count::get(), "conditional branches committed that was predicted hit"),
    ADD_STAT(condHitTakens, statistics::units::Count::get(), "taken conditional branches committed was that predicted hit"),
    ADD_STAT(condHitNotTakens, statistics::units::Count::get(), "not taken conditional branches committed was that predicted hit"),
    ADD_STAT(condMisses, statistics::units::Count::get(), "conditional branches committed that was predicted miss"),
    ADD_STAT(condMissTakens, statistics::units::Count::get(), "taken conditional branches committed was that predicted miss"),
    ADD_STAT(condMissNotTakens, statistics::units::Count::get(), "not taken conditional branches committed was that predicted miss"),
    ADD_STAT(condPredCorrect, statistics::units::Count::get(), "conditional branches committed was that correctly predicted by ftb"),
    ADD_STAT(condPredWrong, statistics::units::Count::get(), "conditional branches committed was that mispredicted by ftb"),
    ADD_STAT(uncondHits, statistics::units::Count::get(), "unconditional branches committed that was predicted hit"),
    ADD_STAT(uncondMisses, statistics::units::Count::get(), "unconditional branches committed that was predicted miss"),
    ADD_STAT(indirectHits, statistics::units::Count::get(), "indirect branches committed that was predicted hit"),
    ADD_STAT(indirectMisses, statistics::units::Count::get(), "indirect branches committed that was predicted miss"),
    ADD_STAT(indirectPredCorrect, statistics::units::Count::get(), "indirect branches committed whose target was correctly predicted by ftb"),
    ADD_STAT(indirectPredWrong, statistics::units::Count::get(), "indirect branches committed whose target was mispredicted by ftb"),
    ADD_STAT(callHits, statistics::units::Count::get(), "calls committed that was predicted hit"),
    ADD_STAT(callMisses, statistics::units::Count::get(), "calls committed that was predicted miss"),
    ADD_STAT(returnHits, statistics::units::Count::get(), "returns committed that was predicted hit"),
    ADD_STAT(returnMisses, statistics::units::Count::get(), "returns committed that was predicted miss")

{
    auto ftb = dynamic_cast<branch_prediction::ftb_pred::DefaultFTB*>(parent);
    // do not need counter below in L0 ftb
    if (ftb->isL0()) {
        predUseL0OnL1Miss.prereq(predUseL0OnL1Miss);
        updateUseL0OnL1Miss.prereq(updateUseL0OnL1Miss);
        newEntry.prereq(newEntry);
        newEntryWithCond.prereq(newEntryWithCond);
        newEntryWithUncond.prereq(newEntryWithUncond);
        oldEntry.prereq(oldEntry);
        oldEntryIndirectTargetModified.prereq(oldEntryIndirectTargetModified);
        oldEntryWithNewCond.prereq(oldEntryWithNewCond);
        oldEntryWithNewUncond.prereq(oldEntryWithNewUncond);
        eraseSlotBehindUncond.prereq(eraseSlotBehindUncond);
    }
}

} // namespace ftb_pred
} // namespace branch_prediction
} // namespace gem5
