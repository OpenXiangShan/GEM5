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


#include "cpu/pred/btb/btb.hh"

#include "base/intmath.hh"
#include "base/trace.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/AheadPipeline.hh"
#include "debug/Fetch.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

/*
 * BTB Constructor
 * Initializes:
 * - BTB structure (sets and ways)
 * - MRU tracking for each set
 * - Address calculation parameters (index/tag masks and shifts)
 */
DefaultBTB::DefaultBTB(const Params &p)
    : TimedBaseBTBPredictor(p),
    numEntries(p.numEntries),
    numWays(p.numWays),
    entryHalfAligned(p.entryHalfAligned),
    tagBits(p.tagBits),
    log2NumThreads(floorLog2(p.numThreads)),
    btbStats(this)
{
    // Calculate shift amounts for index calculation
    if (entryHalfAligned) { // if half aligned, | tag | idx | block offset | instShiftAmt
        idxShiftAmt = floorLog2(blockSize);
    } else { // if not aligned to blockSize, | tag | idx | instShiftAmt
        idxShiftAmt = 1;
    }

    assert(numEntries % numWays == 0);
    numSets = numEntries / numWays;
    this->aheadPipelinedStages = p.aheadPipelinedStages;
    // half-aligned should not be used with ahead-pipelined stages
    assert(aheadPipelinedStages == 0 || !entryHalfAligned);

    if (!isPowerOf2(numEntries)) {
        fatal("BTB entries is not a power of 2!");
    }

    // Initialize BTB structure and MRU tracking
    btb.resize(numSets);
    mruList.resize(numSets);
    for (unsigned i = 0; i < numSets; ++i) {
        auto &set = btb[i];
        set.resize(numWays);
        auto it = set.begin();
        for (; it != set.end(); it++) {
            it->valid = false;
            mruList[i].push_back(it);
        }
        std::make_heap(mruList[i].begin(), mruList[i].end(), older());
    }

    // | tag | idx | block offset | instShiftAmt

    idxMask = numSets - 1;

    tagMask = (1UL << tagBits) - 1;
    // if ahead-pipelined stages are enabled, tag starts from the second bit (PC[tagBits+1 :2])
    tagShiftAmt = (aheadPipelinedStages > 0) ? idxShiftAmt : idxShiftAmt + floorLog2(numSets);

    DPRINTF(BTB, "numEntries %d, numSets %d, numWays %d, tagBits %d, tagShiftAmt %d, idxMask %#lx, tagMask %#lx\n",
        numEntries, numSets, numWays, tagBits, tagShiftAmt, idxMask, tagMask);

    hasDB = true;
    switch (getDelay()) {
        case 0: dbName = std::string("btb_0"); break;
        case 1: dbName = std::string("btb_1"); break;
        default: dbName = std::string("btb"); break;
    }
}

void
DefaultBTB::tickStart()
{
    // nothing to do
}

void
DefaultBTB::tick() {}

void
DefaultBTB::setTrace()
{
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("pc", UINT64),
            std::make_pair("brType", UINT64),
            std::make_pair("target", UINT64),
            std::make_pair("idx", UINT64),
            std::make_pair("mode", UINT64),
            std::make_pair("hit", UINT64)
        };
        switch (getDelay()) {
            case 0: btbTrace = _db->addAndGetTrace("BTBTrace_0", fields_vec); break;
            case 1: btbTrace = _db->addAndGetTrace("BTBTrace_1", fields_vec); break;
            default: btbTrace = _db->addAndGetTrace("BTBTrace", fields_vec); break;
        }
        btbTrace->init_table();
    }
}

/**
 * Process BTB entries:
 * 1. Sort entries by PC order
 * 2. Remove entries before the start PC
 */
std::vector<DefaultBTB::TickedBTBEntry>
DefaultBTB::processEntries(const std::vector<TickedBTBEntry>& entries, Addr startAddr)
{
    int hitNum = entries.size();
    bool hit = hitNum > 0;
    
    // Update prediction statistics
    if (hit) {
        DPRINTF(BTB, "BTB: lookup hit, dumping hit entry\n");
        btbStats.predHit += hitNum;
        for (auto &entry: entries) {
            printTickedBTBEntry(entry);
        }
    } else {
        btbStats.predMiss++;
        DPRINTF(BTB, "BTB: lookup miss\n");
    }

    auto processed_entries = entries;
    
    // Sort by instruction order
    std::sort(processed_entries.begin(), processed_entries.end(), 
             [](const BTBEntry &a, const BTBEntry &b) {
                 return a.pc < b.pc;
             });
    
    // Remove entries before the start PC
    auto it = std::remove_if(processed_entries.begin(), processed_entries.end(),
                           [startAddr](const BTBEntry &e) {
                               return e.pc < startAddr;
                           });
    processed_entries.erase(it, processed_entries.end());

    // remove entries after the range of mBTB
    Addr mbtb_end = (startAddr + predictWidth) & ~mask(floorLog2(predictWidth) - 1);
    it = std::remove_if(processed_entries.begin(), processed_entries.end(),
                        [mbtb_end](const BTBEntry &e) {
                            return e.pc >= mbtb_end;
                        });
    processed_entries.erase(it, processed_entries.end());
    return processed_entries;
}

/**
 * Fill predictions for each pipeline stage:
 * 1. Copy BTB entries
 * 2. Set conditional branch predictions
 * 3. Set indirect branch targets
 */
void
DefaultBTB::fillStagePredictions(const std::vector<TickedBTBEntry>& entries,
                                    std::vector<FullBTBPrediction>& stagePreds)
{
    // S0 prediction source statistic is tracked by ABTB
    if (aheadPipelinedStages > 0) {
        if (stagePreds[0].btbEntries.size() > 0) {
            DPRINTF(BTB, "BTB: predsOfEachStage are already filled by uBTB, skipping ABTB prediction\n");
            btbStats.S0PredUseUBTB++;
            return;
        }
    }

    for (int s = getDelay(); s < stagePreds.size(); ++s) {
        // if (!isL0() && !hit && stagePreds[s].valid) {
        //     DPRINTF(BTB, "BTB: ubtb hit and btb miss, use ubtb result");
        //     incNonL0Stat(btbStats.predUseL0OnL1Miss);
        //     break;
        // }
        DPRINTF(BTB, "BTB: assigning prediction for stage %d\n", s);
        
        // Copy BTB entries to stage prediction
        stagePreds[s].btbEntries.clear();
        for (auto e: entries) {
            stagePreds[s].btbEntries.push_back(BTBEntry(e));
        }
        checkAscending(stagePreds[s].btbEntries);
        dumpBTBEntries(stagePreds[s].btbEntries);

        // Set predictions for each branch
        for (auto &e : entries) {
            assert(e.valid);
            if (e.isCond) {
                // TODO: a performance bug here, mbtb should not update condTakens!
                // if (isL0()) {  // only L0 BTB has saturating counter
                    // use saturating counter of L0 BTB
                    stagePreds[s].condTakens[e.pc] = e.alwaysTaken || (e.ctr >= 0);
                // } else {  // L1 BTB condTakens depends on the TAGE predictor
                // }
            } else if (e.isIndirect) {
                // Set predicted target for indirect branches
                DPRINTF(BTB, "setting indirect target for pc %#lx to %#lx\n", e.pc, e.target);
                stagePreds[s].indirectTargets[e.pc] = e.target;
                if (e.isReturn) {
                    stagePreds[s].returnTarget = e.target;
                }
                break;
            }
        }

        stagePreds[s].predTick = curTick();
    }

    // Update S0 prediction source statistics, if the control flow reached this point, we know that uBTB miss
    if (aheadPipelinedStages > 0) {
        if (entries.size() > 0) {
            btbStats.S0PredUseABTB++;
        } else {
            btbStats.S0Predmiss++;
        }
    }
}

/**
 * Update metadata for later stages:
 * 1. Clear old metadata
 * 2. Save L0 BTB entries for L1 BTB's reference
 * 3. Save current BTB entries
 */
void
DefaultBTB::updatePredictionMeta(const std::vector<TickedBTBEntry>& entries,
                                   std::vector<FullBTBPrediction>& stagePreds)
{
    meta.l0_hit_entries.clear();
    meta.hit_entries.clear();
    
    // Save L0 BTB entries for L1 BTB's reference
    if (getDelay() >= 1) {
        // L0 should be zero-bubble
        meta.l0_hit_entries = stagePreds[0].btbEntries;
    }

    // Save current BTB entries
    for (auto e: entries) {
        meta.hit_entries.push_back(BTBEntry(e));
    }
}

void
DefaultBTB::putPCHistory(Addr startAddr,
                         const boost::dynamic_bitset<> &history,
                         std::vector<FullBTBPrediction> &stagePreds)
{
    // Lookup all matching entries in BTB
    auto find_entries = lookup(startAddr);
    
    // Process BTB entries
    auto processed_entries = processEntries(find_entries, startAddr);
    
    // Fill predictions for each pipeline stage
    fillStagePredictions(processed_entries, stagePreds);
    
    // Update metadata for later stages
    updatePredictionMeta(processed_entries, stagePreds);
}

std::shared_ptr<void>
DefaultBTB::getPredictionMeta()
{
    std::shared_ptr<void> meta_void_ptr = std::make_shared<BTBMeta>(meta);
    return meta_void_ptr;
}

void
DefaultBTB::specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) {}

void
DefaultBTB::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    // clear ahead pipeline first
    while (!aheadReadBtbEntries.empty()) {
        aheadReadBtbEntries.pop();
    }
    /*
    // do ahead reads to fill the ahead-pipeline
    // with the squashed stream we could only do one read
    // if want to support more ahead stages, interface needs to be modified
    auto startAddr = entry.startPC;
    // access ftb memory, get a set
    Addr ftb_read_idx = getIndex(startAddr);
    auto ftb_read_set = ftb[ftb_read_idx];
    assert(ftb_read_idx < numSets);
    // in ahead-pipelined implementations, we do memory access first with
    // address of the previous block, and do tag compare with current address
    // thus we need to store the entry read from memory for later use
    if (aheadPipelinedStages > 0) {
        DPRINTF(AheadPipeline, "FTB: in recoverHist\n");
        DPRINTF(AheadPipeline, "FTB: pushing set for ahead-pipelined stages %d,
            idx %d\n", aheadPipelinedStages, ftb_read_idx);
        DPRINTF(AheadPipeline, "FTB: dumping ftb set\n");
        for (auto &entry : ftb_read_set) {
            printTickedFTBEntry(entry.second);
        }
        aheadReadFtbEntries.push(std::make_tuple(startAddr, ftb_read_idx, ftb_read_set));
    }*/
}


/**
 * Helper function to lookup entries in a single block
 * @param block_pc The aligned PC to lookup
 * @return Vector of matching BTB entries
 */
std::vector<DefaultBTB::TickedBTBEntry>
DefaultBTB::lookupSingleBlock(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }
    Addr btb_idx = getIndex(block_pc);
    auto btb_set = btb[btb_idx];
    assert(btb_idx < numSets);
    // in ahead-pipelined implementations, we do memory access first with
    // address of the previous block, and do tag compare with current address
    // thus we need to store the entry read from memory for later use
    if (aheadPipelinedStages > 0) {
        assert(!entryHalfAligned);
        DPRINTF(AheadPipeline, "FTB: pushing set for ahead-pipelined stages %d, idx %d\n",
             aheadPipelinedStages, btb_idx);
        // DPRINTF(AheadPipeline, "FTB: dumping ftb set\n");
        // for (auto &entry : btb_set) {
        //     printTickedBTBEntry(entry);
        // }
        aheadReadBtbEntries.push(std::make_tuple(block_pc, btb_idx, btb_set));
    }

    Addr current_tag = getTag(block_pc);
    Addr current_pc = 0;
    Addr current_idx = 0;
    BTBSet current_set;
    if (aheadPipelinedStages == 0) {
        current_pc = block_pc;
        current_idx = btb_idx;
        current_set = btb_set;
    } else {
        // only if the ahead-pipeline is filled can we use the entry
        if (aheadReadBtbEntries.size() >= aheadPipelinedStages+1) {
            // +1 because we pushed a new set in this cycle before
            // in case there are push without corresponding pop
            assert(aheadReadBtbEntries.size() == aheadPipelinedStages+1);
            std::tie(current_pc, current_idx, current_set) = aheadReadBtbEntries.front();
            DPRINTF(AheadPipeline, "BTB: ahead-pipeline filled, using set %d from pc %#lx\n",
                current_idx, current_pc);
            DPRINTF(AheadPipeline, "BTB: dumping btb set\n");
            for (auto &entry : current_set) {
                printTickedBTBEntry(entry);
            }
            aheadReadBtbEntries.pop();
        } else {
            DPRINTF(AheadPipeline, "BTB: ahead-pipeline not filled, only have %d sets read,"
                " skipping tag compare, assigning miss\n", aheadReadBtbEntries.size());
        }
    }
    DPRINTF(BTB, "BTB: Doing tag comparison for index %d tag %#lx\n",
        current_idx, current_tag);
    for (auto &way : current_set) {
        if (way.valid && way.tag == current_tag) {
            res.push_back(way);
            way.tick = curTick();  // Update timestamp for MRU
            std::make_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
        }
    }
    return res;
}

std::vector<DefaultBTB::TickedBTBEntry>
DefaultBTB::lookup(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }

    if (entryHalfAligned) {
        // Calculate 32B aligned address
        Addr alignedPC = block_pc & ~(blockSize - 1);
        // Lookup first 32B block
        res = lookupSingleBlock(alignedPC);
        // Lookup next 32B block
        auto nextBlockRes = lookupSingleBlock(alignedPC + blockSize);
        // Merge results
        res.insert(res.end(), nextBlockRes.begin(), nextBlockRes.end());

        // Sort entries by PC order
        std::sort(res.begin(), res.end(),
                 [](const TickedBTBEntry &a, const TickedBTBEntry &b) {
                     return a.pc < b.pc;
                 });

        DPRINTF(BTB, "BTB: Half-aligned lookup results:\n");
        dumpTickedBTBEntries(res);
    } else {
        res = lookupSingleBlock(block_pc);
    }
    return res;
}

/*
 * Generate a new BTB entry or update an existing one based on execution results
 * 
 * This function is called during BTB update to:
 * 1. Check if the executed branch was predicted (hit in BTB)
 * 2. If hit, prepare to update the existing entry
 * 3. If miss and branch was taken:
 *    - Create a new entry
 *    - For conditional branches, initialize as always taken with counter = 1
 * 4. Set the tag and update stream metadata for later use in update()
 * 
 * Note: This is only called in L1 BTB during update
 */
void
DefaultBTB::getAndSetNewBTBEntry(FetchStream &stream)
{
    DPRINTF(BTB, "generating new btb entry\n");
    // Get prediction metadata from previous stages
    auto meta = std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]);
    auto &predBTBEntries = meta->hit_entries;
    
    // Check if this branch was predicted (exists in BTB)
    bool pred_branch_hit = false;
    BTBEntry entry_to_write = BTBEntry();
    for (auto &e: predBTBEntries) {
        if (stream.exeBranchInfo == e) {
            pred_branch_hit = true;
            entry_to_write = e;
            break;
        }
    }
    bool is_old_entry = pred_branch_hit;

    // If branch was not predicted but was actually taken in execution, create new entry
    if (!pred_branch_hit && stream.exeTaken) {
        BTBEntry new_entry = BTBEntry(stream.exeBranchInfo);
        new_entry.valid = true;
        // For conditional branches, initialize as always taken
        if (new_entry.isCond) {
            new_entry.alwaysTaken = true;
            new_entry.ctr = 0;  // Start with positive prediction
            incNonL0Stat(btbStats.newEntryWithCond);
        } else {
            incNonL0Stat(btbStats.newEntryWithUncond);
        }
        incNonL0Stat(btbStats.newEntry);
        entry_to_write = new_entry;
        is_old_entry = false;
    } else {
        // Existing entries will be updated in update()
    }

    // Set tag and update stream metadata for use in update()
    entry_to_write.tag = getTag(entry_to_write.pc);
    stream.updateNewBTBEntry = entry_to_write;
    stream.updateIsOldEntry = is_old_entry;
}

/**
 * Process old BTB entries from prediction metadata
 * 1. Get prediction metadata
 * 2. Remove entries that were not executed
 */
std::vector<BTBEntry>
DefaultBTB::processOldEntries(const FetchStream &stream)
{
    auto meta = std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]);
    // hit entries whose corresponding insts are acutally executed
    Addr end_inst_pc = stream.updateEndInstPC;
    DPRINTF(BTB, "end_inst_pc: %#lx\n", end_inst_pc);
    // remove not executed btb entries, pc > end_inst_pc
    auto old_entries = meta->hit_entries;
    DPRINTF(BTB, "old_entries.size(): %lu\n", old_entries.size());
    dumpBTBEntries(old_entries);
    auto remove_it = std::remove_if(old_entries.begin(), old_entries.end(),
        [end_inst_pc](const BTBEntry &e) { return e.pc > end_inst_pc; });
    old_entries.erase(remove_it, old_entries.end());
    DPRINTF(BTB, "after removing not executed insts, old_entries.size(): %lu\n", old_entries.size());
    dumpBTBEntries(old_entries);

    btbStats.updateHit += old_entries.size();
    
    return old_entries;
}

/**
 * Check if the branch was predicted correctly
 * Also check L0 BTB prediction status
 */
void
DefaultBTB::checkPredictionHit(const FetchStream &stream, const BTBMeta* meta)
{
    bool pred_branch_hit = false;
    for (auto &e : meta->hit_entries) {
        if (stream.exeBranchInfo == e) {
            pred_branch_hit = true;
            break;
        }
    }
    if (!pred_branch_hit && stream.exeTaken) {
        DPRINTF(BTB, "update miss detected, pc %#lx, predTick %lu\n", stream.exeBranchInfo.pc, stream.predTick);
        btbStats.updateMiss++;
    }

    // Check if L0 BTB had a hit but L1 BTB missed
    bool pred_l0_branch_hit = false;
    for (auto &e : meta->l0_hit_entries) {
        if (stream.exeBranchInfo == e) {
            pred_l0_branch_hit = true;
            break;
        }
    }
    if (!isL0()) {
        bool l0_hit_l1_miss = pred_l0_branch_hit && !pred_branch_hit;
        if (l0_hit_l1_miss) {
            DPRINTF(BTB, "BTB: skipping entry write because of l0 hit\n");
            incNonL0Stat(btbStats.updateUseL0OnL1Miss);
            // return;
        }
    }
}


/**
 * Collect all entries that need to be updated
 * 1. Process old entries
 * 2. Add new entry if necessary
 */
std::vector<BTBEntry>
DefaultBTB::collectEntriesToUpdate(const std::vector<BTBEntry>& old_entries,
                                     const FetchStream &stream)
{
    auto all_entries = old_entries;

    if (isL0()){
        // since we don't want duplications in uBTB's entriesToUpdate,
        // which causes its counter to update twice unintentionally
        // we need to check if the new entry already exists in uBTB
        bool pred_branch_hit = false;
        for (auto &e: all_entries) {
            if (stream.updateNewBTBEntry == e) {
                pred_branch_hit = true;
                break;
            }
        }
        if (!pred_branch_hit) {
            all_entries.push_back(stream.updateNewBTBEntry);
        }

    }else{
        if (!stream.updateIsOldEntry) { // L0 BTB always updates
            all_entries.push_back(stream.updateNewBTBEntry);
        }
    }
    DPRINTF(BTB, "all_entries_to_update.size(): %lu\n", all_entries.size());
    dumpBTBEntries(all_entries);
    return all_entries;
}

/**
 * Update or replace BTB entry
 * 1. Look for matching entry
 * 2. for cond entry, if found, use the one in btb, since we need the up-to-date counter
 * 3. for indirect entry, update target if necessary
 * 4. Update existing entry or replace oldest entry
 * 5. Update MRU information
 */
void
DefaultBTB::updateBTBEntry(Addr btb_idx, Addr btb_tag, const BTBEntry& entry, const FetchStream &stream)
{

    // Look for matching entry
    bool found = false;
    auto it = btb[btb_idx].begin();
    for (; it != btb[btb_idx].end(); it++) {
        if (*it == entry) {
            found = true;
            break;
        }
    }
    // if cond entry in btb now, use the one in btb, since we need the up-to-date counter
    // else use the recorded entry
    auto entry_to_write = entry.isCond && found ? BTBEntry(*it) : entry;
    entry_to_write.tag = btb_tag;   // update tag after found it!
    // update saturating counter if necessary
    if (entry_to_write.isCond) {
        bool this_cond_taken = stream.exeTaken && stream.getControlPC() == entry_to_write.pc;
        if (!this_cond_taken) {
            entry_to_write.alwaysTaken = false;
        }
        // if (isL0()) {  // only L0 BTB has saturating counter
        if(!entry_to_write.alwaysTaken) {
            updateCtr(entry_to_write.ctr, this_cond_taken);
        }
        // }
    }
    // update indirect target if necessary
    if (entry_to_write.isIndirect && stream.exeTaken && stream.getControlPC() == entry_to_write.pc) {
        entry_to_write.target = stream.exeBranchInfo.target;
    }
    auto ticked_entry = TickedBTBEntry(entry_to_write, curTick());
    if (found) {
        // Update existing entry
        *it = ticked_entry;
        if (enableDB) {
            BTBTrace rec;
            rec.set(ticked_entry.pc, ticked_entry.getType(),
                ticked_entry.target, btb_idx, Mode::WRITE, 1);
            btbTrace->write_record(rec);
        }
        btbStats.updateExisting++;
    } else {
        // Replace oldest entry in the set
        DPRINTF(BTB, "trying to replace entry in set %#lx\n", btb_idx);
        dumpMruList(mruList[btb_idx]);
        // put the oldest entry in this set to the back of heap
        std::pop_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
        const auto& entry_in_btb_now = mruList[btb_idx].back();
        if (enableDB) {
            BTBTrace rec;
            rec.set(entry_in_btb_now->pc, entry_in_btb_now->getType(),
                    entry_in_btb_now->target, btb_idx, Mode::EVICT, 0);
                btbTrace->write_record(rec);
        }
        if (entry_in_btb_now->valid) {
            // if all ways are really occupied, we need to replace valid entry
            // means 32B block is more than 4ways/ 4 branches
            btbStats.updateReplaceValidOne++;
        }
        btbStats.updateReplace++;
        DPRINTF(BTB, "BTB: Replacing entry with tag %#lx, pc %#lx in set %#lx\n",
                entry_in_btb_now->tag, entry_in_btb_now->pc, btb_idx);
        *entry_in_btb_now = ticked_entry;
        if (enableDB) {
            BTBTrace rec;
            rec.set(entry_in_btb_now->pc, entry_in_btb_now->getType(),
                entry_in_btb_now->target, btb_idx, Mode::WRITE, 0);
            btbTrace->write_record(rec);
        }
        dumpMruList(mruList[btb_idx]);
    }
    std::make_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
}

/*
 * Update BTB with execution results
 * Steps:
 * 1. Get old entries that were hit during prediction
 * 2. Remove entries that were not actually executed
 * 3. Update statistics
 * 4. Update existing entries or create new ones
 * 5. Update MRU information
 */
void
DefaultBTB::update(const FetchStream &stream)
{
    // 1. Process old entries
    auto old_entries = processOldEntries(stream);
    
    // 2. Check prediction hit status, for stats recording
    checkPredictionHit(stream,
        std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]).get());

    // 3. Collect entries to update
    auto entries_to_update = collectEntriesToUpdate(old_entries, stream);
    
    // 4. Update BTB entries - each entry uses its own PC to calculate index and tag
    for (auto &entry : entries_to_update) {
        // Calculate 32-byte aligned address for this entry
        Addr entryPC = entry.pc;
        Addr btb_idx;
        Addr btb_tag;

        if (entryHalfAligned) {
            btb_idx = getIndex(entryPC);
            btb_tag = getTag(entryPC);
        } else{
            Addr startPC = stream.getRealStartPC();
            btb_idx = getIndex(startPC);
            btb_tag = getTag(startPC);
        }

        if (aheadPipelinedStages > 0) {
            Addr previousPC = getPreviousPC(stream);
            if (previousPC == 0) {
                DPRINTF(BTB, "ahead-pipeline: no previous PC, skipping update\n");
                return;
            }
            btb_idx = getIndex(previousPC);
        }
        updateBTBEntry(btb_idx, btb_tag, entry, stream);

    }
    
    // Verify BTB state
    for (unsigned i = 0; i < numSets; i++) {
        assert(btb[i].size() <= numWays);
        assert(mruList[i].size() <= numWays);
    }
}

/**
 * Get the previous PC from the fetch stream, useful in the update of ahead-pipelined BTB
 * @param stream Fetch stream containing prediction info
 * @return Previous PC, 0 if the stream is not filled
 */
Addr
DefaultBTB::getPreviousPC(const FetchStream &stream)
{
    // get pc from the nth previous block, the value of n is aheadPipelinedStages
    auto previous_pcs = stream.previousPCs;
    if (previous_pcs.size() < aheadPipelinedStages) {
        // if the stream is not filled, we cannot update ftb
        DPRINTF(AheadPipeline, "FTB: ahead-pipeline not filled, only have %d pcs read,"
            " skipping ftb update\n", previous_pcs.size());
        return 0;
    } else {
        DPRINTF(AheadPipeline, "FTB: ahead-pipeline filled, using pc %d blocks before,"
            " prevoiusPC.size() %d\n", aheadPipelinedStages, previous_pcs.size());
        while (previous_pcs.size() > aheadPipelinedStages) {
            previous_pcs.pop();
        }
        return previous_pcs.front();
    }
}

void
DefaultBTB::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
{
    auto meta = std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]);
    auto &hit_entries = meta->hit_entries;
    auto pc = inst->getPC();
    auto npc = inst->getNPC();
    // auto &static_inst = inst->staticInst();
    bool this_branch_hit = false;
    auto entry = BTBEntry();
    for (auto e : hit_entries) {
        if (e.pc == pc) {
            this_branch_hit = true;
            entry = e;
            break;
        }
    }
    // bool this_branch_miss = !this_branch_hit;
    bool cond_not_taken = inst->isCondCtrl() && !inst->branching();
    bool this_branch_taken = stream.exeTaken && stream.getControlPC() == pc; // all uncond should be taken
    Addr this_branch_target = npc;
    if (this_branch_hit) {
        btbStats.allBranchHits++;
        if (this_branch_taken) {
            btbStats.allBranchHitTakens++;
        } else {
            btbStats.allBranchHitNotTakens++;
        }
        if (inst->isCondCtrl()) {
            btbStats.condHits++;
            if (this_branch_taken) {
                btbStats.condHitTakens++;
            } else {
                btbStats.condHitNotTakens++;
            }
            // if (isL0()) {
                bool pred_taken = entry.ctr >= 0;
                if (pred_taken == this_branch_taken) {
                    btbStats.condPredCorrect++;
                } else {
                    btbStats.condPredWrong++;
                }
            // }
        }
        if (inst->isUncondCtrl()) {
            btbStats.uncondHits++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                btbStats.indirectHits++;
                Addr pred_target = entry.target;
                if (pred_target == this_branch_target) {
                    btbStats.indirectPredCorrect++;
                } else {
                    btbStats.indirectPredWrong++;
                }
            }
            if (inst->isCall()) {
                btbStats.callHits++;
            }
            if (inst->isReturn()) {
                btbStats.returnHits++;
            }
        }
    } else {
        btbStats.allBranchMisses++;
        if (this_branch_taken) {
            btbStats.allBranchMissTakens++;
        } else {
            btbStats.allBranchMissNotTakens++;
        }
        if (inst->isCondCtrl()) {
            btbStats.condMisses++;
            if (this_branch_taken) {
                btbStats.condMissTakens++;
                // if (isL0()) {
                    // only L0 BTB has saturating counters to predict conditional branches
                    // taken branches that is missed in btb must have been mispredicted
                    btbStats.condPredWrong++;
                // }
            } else {
                btbStats.condMissNotTakens++;
                // if (isL0()) {
                    // only L0 BTB has saturating counters to predict conditional branches
                    // taken branches that is missed in btb must have been mispredicted
                    btbStats.condPredCorrect++;
                // }
            }
        }
        if (inst->isUncondCtrl()) {
            btbStats.uncondMisses++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                btbStats.indirectMisses++;
                btbStats.indirectPredWrong++;
            }
            if (inst->isCall()) {
                btbStats.callMisses++;
            }
            if (inst->isReturn()) {
                btbStats.returnMisses++;
            }
        }
    }
}

DefaultBTB::BTBStats::BTBStats(statistics::Group* parent) :
    statistics::Group(parent),
    ADD_STAT(newEntry, statistics::units::Count::get(), "number of new btb entries generated"),
    ADD_STAT(newEntryWithCond, statistics::units::Count::get(), "number of new btb entries generated with conditional branch"),
    ADD_STAT(newEntryWithUncond, statistics::units::Count::get(), "number of new btb entries generated with unconditional branch"),
    ADD_STAT(oldEntry, statistics::units::Count::get(), "number of old btb entries updated"),
    ADD_STAT(oldEntryIndirectTargetModified, statistics::units::Count::get(), "number of old btb entries with indirect target modified"),
    ADD_STAT(oldEntryWithNewCond, statistics::units::Count::get(), "number of old btb entries with new conditional branches"),
    ADD_STAT(oldEntryWithNewUncond, statistics::units::Count::get(), "number of old btb entries with new unconditional branches"),
    ADD_STAT(predMiss, statistics::units::Count::get(), "misses encountered on prediction"),
    ADD_STAT(predHit, statistics::units::Count::get(), "hits encountered on prediction"),
    ADD_STAT(updateMiss, statistics::units::Count::get(), "misses encountered on update"),
    ADD_STAT(updateHit, statistics::units::Count::get(), "hits encountered on update"),
    ADD_STAT(updateExisting, statistics::units::Count::get(), "existing entries updated"),
    ADD_STAT(updateReplace, statistics::units::Count::get(), "entries replaced"),
    ADD_STAT(updateReplaceValidOne, statistics::units::Count::get(), "entries replaced with valid entry"),
    ADD_STAT(eraseSlotBehindUncond, statistics::units::Count::get(), "erase slots behind unconditional slot"),
    ADD_STAT(predUseL0OnL1Miss, statistics::units::Count::get(), "use l0 result on l1 miss when pred"),
    ADD_STAT(updateUseL0OnL1Miss, statistics::units::Count::get(), "use l0 result on l1 miss when update"),
    ADD_STAT(S0Predmiss, statistics::units::Count::get(), "misses encountered on S0 prediction, i.e. uBTB and ABTB miss"),
    ADD_STAT(S0PredUseUBTB, statistics::units::Count::get(), "uBTB prediction used, i.e. uBTB hit"),
    ADD_STAT(S0PredUseABTB, statistics::units::Count::get(), "aBTB prediction used, i.e. uBTB miss and ABTB hit"),

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
    ADD_STAT(condPredCorrect, statistics::units::Count::get(), "conditional branches committed was that correctly predicted by btb"),
    ADD_STAT(condPredWrong, statistics::units::Count::get(), "conditional branches committed was that mispredicted by btb"),
    ADD_STAT(uncondHits, statistics::units::Count::get(), "unconditional branches committed that was predicted hit"),
    ADD_STAT(uncondMisses, statistics::units::Count::get(), "unconditional branches committed that was predicted miss"),
    ADD_STAT(indirectHits, statistics::units::Count::get(), "indirect branches committed that was predicted hit"),
    ADD_STAT(indirectMisses, statistics::units::Count::get(), "indirect branches committed that was predicted miss"),
    ADD_STAT(indirectPredCorrect, statistics::units::Count::get(), "indirect branches committed whose target was correctly predicted by btb"),
    ADD_STAT(indirectPredWrong, statistics::units::Count::get(), "indirect branches committed whose target was mispredicted by btb"),
    ADD_STAT(callHits, statistics::units::Count::get(), "calls committed that was predicted hit"),
    ADD_STAT(callMisses, statistics::units::Count::get(), "calls committed that was predicted miss"),
    ADD_STAT(returnHits, statistics::units::Count::get(), "returns committed that was predicted hit"),
    ADD_STAT(returnMisses, statistics::units::Count::get(), "returns committed that was predicted miss")

{
    auto btb = dynamic_cast<branch_prediction::btb_pred::DefaultBTB*>(parent);
    // do not need counter below in L0 btb
    if (btb->isL0()) {
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
    if (btb->aheadPipelinedStages == 0){
        S0Predmiss.prereq(S0Predmiss);
        S0PredUseUBTB.prereq(S0PredUseUBTB);
        S0PredUseABTB.prereq(S0PredUseABTB);
    }
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
