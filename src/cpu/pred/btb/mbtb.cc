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


#include "cpu/pred/btb/mbtb.hh"

#include "base/intmath.hh"

// Additional conditional includes based on build mode
#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "base/trace.hh"
    #include "cpu/o3/dyn_inst.hh"
    #include "debug/AheadPipeline.hh"
    #include "debug/Fetch.hh"
#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

// Conditional namespace wrapper for testing
#ifdef UNIT_TEST
namespace test {
#endif

/*
 * MBTB Constructor
 * Initializes:
 * - MBTB structure (sets and ways)
 * - MRU tracking for each set
 * - Address calculation parameters (index/tag masks and shifts)
 * - Always uses half-aligned mode (64-byte block coverage)
 */
#ifdef UNIT_TEST
// Test constructor for unit testing mode
MBTB::MBTB(unsigned numEntries, unsigned tagBits, unsigned numWays, unsigned numDelay)
    : TimedBaseBTBPredictor(),
      victimCacheSize(8),
      victimCachePtr(0),
      numEntries(numEntries),
      numWays(numWays),
      tagBits(tagBits)
{
    setNumDelay(numDelay);
#else
// Production constructor
MBTB::MBTB(const Params &p)
    : TimedBaseBTBPredictor(p),
    victimCacheSize(16), // Default size, will be configurable later
    victimCachePtr(0),
    numEntries(p.numEntries),
    numWays(p.numWays),
    tagBits(p.tagBits),
    btbStats(this)
{
    // MBTB doesn't support ahead-pipelined stages
#endif
    // Calculate shift amounts for index calculation
    // MBTB is always half-aligned: | tag | idx | block offset | instShiftAmt
    idxShiftAmt = floorLog2(blockSize);

    // For dual SRAM: each SRAM has numWays/2 ways, total still numWays*2
    assert(numEntries % (numWays * 2) == 0);
    numSets = numEntries / (numWays * 2); // Each SRAM has numSets sets

    if (!isPowerOf2(numEntries)) {
        fatal("BTB entries is not a power of 2!");
    }

    // Initialize dual SRAM BTB structure and MRU tracking
    sram0.resize(numSets);
    sram1.resize(numSets);
    mru0.resize(numSets);
    mru1.resize(numSets);
    
    // Initialize SRAM0
    for (unsigned i = 0; i < numSets; ++i) {
        auto &set = sram0[i];
        set.resize(numWays);
        auto it = set.begin();
        for (; it != set.end(); it++) {
            it->valid = false;
            mru0[i].push_back(it);
        }
        std::make_heap(mru0[i].begin(), mru0[i].end(), older());
    }
    
    // Initialize SRAM1
    for (unsigned i = 0; i < numSets; ++i) {
        auto &set = sram1[i];
        set.resize(numWays);
        auto it = set.begin();
        for (; it != set.end(); it++) {
            it->valid = false;
            mru1[i].push_back(it);
        }
        std::make_heap(mru1[i].begin(), mru1[i].end(), older());
    }

    // | tag | idx | block offset | instShiftAmt
    idxMask = numSets - 1;

    tagMask = (1UL << tagBits) - 1;
    // MBTB uses standard tag shift calculation
    tagShiftAmt = idxShiftAmt + floorLog2(numSets);

    // Initialize victim cache
    victimCache.resize(victimCacheSize);
    for (auto& entry : victimCache) {
        entry.valid = false;
        entry.tick = 0;
    }

    DPRINTF(BTB, "numEntries %d, numSets %d, numWays %d, tagBits %d, tagShiftAmt %d, "
        "idxMask %#lx, tagMask %#lx, victimCacheSize %d\n",
        numEntries, numSets, numWays, tagBits, tagShiftAmt, idxMask, tagMask, victimCacheSize);

#ifndef UNIT_TEST
    hasDB = true;
    dbName = std::string("MainBTB");
#endif
}

#ifndef UNIT_TEST
void
MBTB::tickStart()
{
    // nothing to do
}

void
MBTB::tick() {}

void
MBTB::setTrace()
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
        btbTrace = _db->addAndGetTrace("MainBTBTrace", fields_vec);
        btbTrace->init_table();
    }
}
#endif

/**
 * Process BTB entries:
 * 1. Sort entries by PC order
 * 2. Remove entries before the start PC
 */
std::vector<MBTB::TickedBTBEntry>
MBTB::processEntries(const std::vector<TickedBTBEntry>& entries, Addr startAddr)
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
MBTB::fillStagePredictions(const std::vector<TickedBTBEntry>& entries,
                                    std::vector<FullBTBPrediction>& stagePreds)
{

    FillStageLoop(s) {
        DPRINTF(BTB, "BTB: assigning prediction for stage %d\n", s);
        // Copy BTB entries to stage prediction
        stagePreds[s].btbEntries.clear();
        for (auto e : entries) {
            stagePreds[s].btbEntries.push_back(BTBEntry(e));
        }
        checkAscending(stagePreds[s].btbEntries);
        if (s == getDelay()) dumpBTBEntries(stagePreds[s].btbEntries);

        stagePreds[s].predTick = curTick();

        stagePreds[s].condTakens.clear();
        stagePreds[s].indirectTargets.clear();
    }

    // Set predictions for each branch
    for (auto &e : entries) {
        assert(e.valid);
        if (e.isCond) {
            // TODO: a performance bug here, mbtb should not update condTakens!
            // if (isL0()) {  // only L0 BTB has saturating counter
            // use saturating counter of L0 BTB

            FillStageLoop(s) stagePreds[s].condTakens.push_back({e.pc, e.alwaysTaken || (e.ctr >= 0)});

            // } else {  // L1 BTB condTakens depends on the TAGE predictor
            // }
        } else if (e.isIndirect) {
            // Set predicted target for indirect branches
            DPRINTF(BTB, "setting indirect target for pc %#lx to %#lx\n", e.pc, e.target);

            FillStageLoop(s) stagePreds[s].indirectTargets.push_back({e.pc, e.target});

            if (e.isReturn) {
                FillStageLoop(s) stagePreds[s].returnTarget = e.target;
            }
            break;
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
MBTB::updatePredictionMeta(const std::vector<TickedBTBEntry>& entries,
                                   std::vector<FullBTBPrediction>& stagePreds)
{

    // Save L0 BTB entries for L1 BTB's reference
    if (getDelay() >= 1) {
        // L0 should be zero-bubble
        meta->l0_hit_entries = stagePreds[0].btbEntries;
    }

    // Save current BTB entries
    for (auto e: entries) {
        meta->hit_entries.push_back(BTBEntry(e));
    }
}

void
MBTB::putPCHistory(Addr startAddr,
                         const boost::dynamic_bitset<> &history,
                         std::vector<FullBTBPrediction> &stagePreds)
{
    meta = std::make_shared<BTBMeta>();
    // Lookup all matching entries in BTB
    auto find_entries = lookup(startAddr, meta);

    // Process BTB entries
    auto processed_entries = processEntries(find_entries, startAddr);
    
    // Fill predictions for each pipeline stage
    fillStagePredictions(processed_entries, stagePreds);
    
    // Update metadata for later stages
    updatePredictionMeta(processed_entries, stagePreds);
}

std::shared_ptr<void>
MBTB::getPredictionMeta()
{
    return meta;
}

void
MBTB::specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) {}

void
MBTB::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    // MBTB doesn't support ahead-pipelined stages, nothing to recover
}


/**
 * Helper function to lookup entries in a single block
 * @param block_pc The aligned PC to lookup
 * @return Vector of matching BTB entries
 */
std::vector<MBTB::TickedBTBEntry>
MBTB::lookupSingleBlock(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }
    // Select SRAM based on 32B-aligned address
    int sram_id = getSRAMId(block_pc);
    auto& target_sram = (sram_id == 0) ? sram0 : sram1;
    auto& target_mru = (sram_id == 0) ? mru0 : mru1;
    
    Addr btb_idx = getIndex(block_pc);
    auto& btb_set = target_sram[btb_idx];
    assert(btb_idx < numSets);

    Addr current_tag = getTag(block_pc);
    DPRINTF(BTB, "BTB: Doing tag comparison for SRAM%d index 0x%lx tag %#lx\n",
        sram_id, btb_idx, current_tag);
        
    for (auto &way : btb_set) {
        if (way.valid && way.tag == current_tag) {
            res.push_back(way);
            way.tick = curTick();  // Update timestamp for MRU
            std::make_heap(target_mru[btb_idx].begin(), target_mru[btb_idx].end(), older());
        }
    }
    return res;
}

std::vector<MBTB::TickedBTBEntry>
MBTB::lookup(Addr block_pc, std::shared_ptr<BTBMeta> meta)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }

    // MBTB always uses half-aligned lookup
    // Calculate 32B aligned address
    Addr alignedPC = block_pc & ~(blockSize - 1);
    // Lookup first 32B block
    res = lookupSingleBlock(alignedPC);
    // Lookup next 32B block
    auto nextBlockRes = lookupSingleBlock(alignedPC + blockSize);
    // Merge results
    res.insert(res.end(), nextBlockRes.begin(), nextBlockRes.end());

    // lookup victim cache if victim cache is enabled
    if (victimCacheSize > 0) {
        auto victimResults = lookupVictimCache(block_pc);
        if (!victimResults.empty()) {
            DPRINTF(BTB, "Victim cache hit for lookup at %#lx\n", block_pc);
            incNonL0Stat(btbStats.victimCacheHit);
            res.insert(res.end(), victimResults.begin(), victimResults.end()); // merge victim results
            meta->victim_cache_hit_entries.insert(
                meta->victim_cache_hit_entries.end(), victimResults.begin(), victimResults.end());
            meta->had_victim_cache_hit = true;  // save prediction meta for later use in update()
        }
    }

    // Sort entries by PC order
    std::sort(res.begin(), res.end(),
             [](const TickedBTBEntry &a, const TickedBTBEntry &b) {
                 return a.pc < b.pc;
             });

    DPRINTF(BTB, "MBTB: Half-aligned lookup results:\n");
    // dumpTickedBTBEntries(res);
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
MBTB::getAndSetNewBTBEntry(FetchStream &stream)
{
    DPRINTF(BTB, "getAndSetNewBTBEntry called for pc %#lx\n", stream.startPC);
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
        DPRINTF(BTB, "Creating new BTB entry for pc %#lx\n", stream.exeBranchInfo.pc);
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
        DPRINTF(BTB, "Not creating new entry: pred_branch_hit=%d, stream.exeTaken=%d\n",
                pred_branch_hit, stream.exeTaken);
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
MBTB::processOldEntries(const FetchStream &stream)
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
MBTB::checkPredictionHit(const FetchStream &stream, const BTBMeta* meta)
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
    bool l0_hit_l1_miss = pred_l0_branch_hit && !pred_branch_hit;
    if (l0_hit_l1_miss) {
        DPRINTF(BTB, "BTB: skipping entry write because of l0 hit\n");
        incNonL0Stat(btbStats.updateUseL0OnL1Miss);
    }
}


/**
 * Collect all entries that need to be updated
 * 1. Process old entries
 * 2. Add new entry if necessary
 */
std::vector<BTBEntry>
MBTB::collectEntriesToUpdate(const std::vector<BTBEntry>& old_entries,
                                     const FetchStream &stream)
{
    auto all_entries = old_entries;

    if (!stream.updateIsOldEntry) {
        all_entries.push_back(stream.updateNewBTBEntry);
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
MBTB::updateBTBEntry(const BTBEntry& entry, const FetchStream &stream)
{
    // Select SRAM based on entry PC's 32B-aligned address
    Addr alignedPC = entry.pc & ~(blockSize - 1);
    int sram_id = getSRAMId(alignedPC);
    auto& target_sram = (sram_id == 0) ? sram0 : sram1;
    auto& target_mru = (sram_id == 0) ? mru0 : mru1;
    
    // Calculate index and tag for this entry
    Addr btb_idx = getIndex(entry.pc);
    Addr btb_tag = getTag(entry.pc);

    // Look for matching entry in the target SRAM
    bool found = false;
    auto it = target_sram[btb_idx].begin();
    for (; it != target_sram[btb_idx].end(); it++) {
        if (*it == entry) {
            found = true;
            break;
        }
    }
    // Look for matching entry in victim cache
    bool found_in_vc = false;
    int vc_idx = -1;
    for (int i = 0; i < (int)victimCache.size(); i++) {
        auto &vc_entry = victimCache[i];
        if (vc_entry.valid && vc_entry.pc == entry.pc) {    // pc is tag compared
            found_in_vc = true;
            vc_idx = i;
            break;
        }
    }
    // if cond entry in btb now, use the one in btb, since we need the up-to-date counter
    // else use the recorded entry
    // Prefer freshest source: if MBTB already has it, use MBTB entry;
    // else if VC has it, use VC entry; else use recorded entry
    BTBEntry entry_to_write = entry;
    if (entry.isCond && found) {
        entry_to_write = BTBEntry(*it);
    } else if (!found && found_in_vc) {
        entry_to_write = BTBEntry(victimCache[vc_idx]);
    }
    entry_to_write.tag = btb_tag;   // update tag after found it!
    // update saturating counter if necessary
    if (entry_to_write.isCond) {
        bool this_cond_taken = stream.exeTaken && stream.getControlPC() == entry_to_write.pc;
        if (!this_cond_taken) {
            entry_to_write.alwaysTaken = false;
        }
        if(!entry_to_write.alwaysTaken) {
            updateCtr(entry_to_write.ctr, this_cond_taken);
        }
    }
    // update indirect target if necessary
    if (entry_to_write.isIndirect && stream.exeTaken && stream.getControlPC() == entry_to_write.pc) {
        entry_to_write.target = stream.exeBranchInfo.target;
    }
    auto ticked_entry = TickedBTBEntry(entry_to_write, curTick());
    if (found) {
        // Update existing entry
        *it = ticked_entry;
#ifndef UNIT_TEST
        if (enableDB) {
            BTBTrace rec;
            rec.set(ticked_entry.pc, ticked_entry.getType(),
                ticked_entry.target, btb_idx, Mode::WRITE, 1);
            btbTrace->write_record(rec);
        }
#endif
        btbStats.updateExisting++;
        // If also present in VC, remove duplicate
        if (found_in_vc) {
            eraseFromVictimCacheByPC(entry.pc);
        }
        std::make_heap(target_mru[btb_idx].begin(), target_mru[btb_idx].end(), older());
        return;
    } else if (found_in_vc) {
        // Centralized promotion policy: do NOT promote here.
        // Only update the VC entry in place and return.
        victimCache[vc_idx] = ticked_entry;
        return;
    } else {
        // Replace oldest entry in the set
        DPRINTF(BTB, "trying to replace entry in SRAM%d set %#lx\n", sram_id, btb_idx);
        // put the oldest entry in this set to the back of heap
        std::pop_heap(target_mru[btb_idx].begin(), target_mru[btb_idx].end(), older());
        const auto& entry_in_btb_now = target_mru[btb_idx].back();
#ifndef UNIT_TEST
        if (enableDB) {
            BTBTrace rec;
            rec.set(entry_in_btb_now->pc, entry_in_btb_now->getType(),
                    entry_in_btb_now->target, btb_idx, Mode::EVICT, 0);
                btbTrace->write_record(rec);
        }
#endif
        if (entry_in_btb_now->valid) {
            // if all ways are really occupied, we need to replace valid entry
            // means 32B block is more than 4ways/ 4 branches
            btbStats.updateReplaceValidOne++;

            // Insert evicted entry into victim cache
            insertVictimCache(*entry_in_btb_now);
        }
        btbStats.updateReplace++;
        DPRINTF(BTB, "BTB: Replacing entry with tag %#lx, pc %#lx in set %#lx\n",
                entry_in_btb_now->tag, entry_in_btb_now->pc, btb_idx);
        *entry_in_btb_now = ticked_entry;
#ifndef UNIT_TEST
        if (enableDB) {
            BTBTrace rec;
            rec.set(entry_in_btb_now->pc, entry_in_btb_now->getType(),
                entry_in_btb_now->target, btb_idx, Mode::WRITE, 0);
            btbTrace->write_record(rec);
        }
#endif
    }
    std::make_heap(target_mru[btb_idx].begin(), target_mru[btb_idx].end(), older());
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
MBTB::update(const FetchStream &stream)
{
    // 0. Promote victim-cache hit entries observed during prediction back to MBTB
    if (victimCacheSize > 0) {
        auto meta = std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]);
        promoteVictimHits(meta->victim_cache_hit_entries);
    }

    // 1. Process old entries
    auto old_entries = processOldEntries(stream);
    
    // 2. Check prediction hit status, for stats recording
    checkPredictionHit(stream,
        std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]).get());

    // 3. Collect entries to update
    auto entries_to_update = collectEntriesToUpdate(old_entries, stream);
    
    // 4. Update BTB entries - each entry uses its own PC to select SRAM and calculate index/tag
    for (auto &entry : entries_to_update) {
        updateBTBEntry(entry, stream);
    }
}

/**
 * Victim cache operations implementation
 */
std::vector<MBTB::TickedBTBEntry>
MBTB::lookupVictimCache(Addr block_pc)
{
    std::vector<TickedBTBEntry> results;
    Addr alignedPC = block_pc & ~(blockSize - 1);

    // Check both 32B blocks for half-aligned lookup
    for (auto &entry : victimCache) {
        if (!entry.valid) continue;

        Addr entryAlignedPC = entry.pc & ~(blockSize - 1);
        // Check if this entry is in either of the two 32B blocks we're looking for
        if (entryAlignedPC == alignedPC || entryAlignedPC == (alignedPC + blockSize)) {
            Addr current_tag = getTag(entry.pc);
            if (entry.tag == current_tag) {
                results.push_back(entry);
                DPRINTF(BTB, "Victim cache hit for pc %#lx\n", entry.pc);
                // refresh LRU timestamp on hit
                entry.tick = curTick();
            }
        }
    }

    return results;
}

void
MBTB::promoteVictimHits(const std::vector<BTBEntry>& victim_hits)
{
    if (victimCacheSize == 0) return;

    for (const auto &vc_hit_entry : victim_hits) {
        Addr alignedPC = vc_hit_entry.pc & ~(blockSize - 1);
        int sram_id = getSRAMId(alignedPC);
        auto &target_sram = (sram_id == 0) ? sram0 : sram1;
        auto &target_mru = (sram_id == 0) ? mru0 : mru1;

        Addr btb_idx = getIndex(vc_hit_entry.pc);
        Addr btb_tag = getTag(vc_hit_entry.pc);

        // Check if already present in MBTB
        bool found_in_btb = false;
        auto it = target_sram[btb_idx].begin();
        for (; it != target_sram[btb_idx].end(); it++) {
            if (it->valid && it->pc == vc_hit_entry.pc) {
                found_in_btb = true;
                break;
            }
        }

        TickedBTBEntry ticked_entry(vc_hit_entry, curTick());
        ticked_entry.tag = btb_tag;

        if (found_in_btb) {
            *it = ticked_entry;
#ifndef UNIT_TEST
            if (enableDB) {
                BTBTrace rec;
                rec.set(it->pc, it->getType(), it->target, btb_idx, Mode::WRITE, 1);
                btbTrace->write_record(rec);
            }
#endif
            btbStats.victimCachePromote++;
            std::make_heap(target_mru[btb_idx].begin(), target_mru[btb_idx].end(), older());
        } else {
            // Replace oldest entry in the set
            std::pop_heap(target_mru[btb_idx].begin(), target_mru[btb_idx].end(), older());
            const auto &entry_in_btb_now = target_mru[btb_idx].back();
#ifndef UNIT_TEST
            if (enableDB) {
                BTBTrace rec;
                rec.set(entry_in_btb_now->pc, entry_in_btb_now->getType(),
                        entry_in_btb_now->target, btb_idx, Mode::EVICT, 0);
                btbTrace->write_record(rec);
            }
#endif
            if (entry_in_btb_now->valid) {
                insertVictimCache(*entry_in_btb_now);
            }
            *entry_in_btb_now = ticked_entry;
#ifndef UNIT_TEST
            if (enableDB) {
                BTBTrace rec;
                rec.set(entry_in_btb_now->pc, entry_in_btb_now->getType(),
                        entry_in_btb_now->target, btb_idx, Mode::WRITE, 0);
                btbTrace->write_record(rec);
            }
#endif
            btbStats.victimCachePromote++;
            std::make_heap(target_mru[btb_idx].begin(), target_mru[btb_idx].end(), older());
        }

        // Remove from VC
        eraseFromVictimCacheByPC(vc_hit_entry.pc);
    }
}


bool
MBTB::eraseFromVictimCacheByPC(Addr pc)
{
    bool erased = false;
    for (auto &entry : victimCache) {
        if (entry.valid && entry.pc == pc) {
            entry.valid = false;
            erased = true;
            break;
        }
    }
    return erased;
}

void
MBTB::insertVictimCache(const TickedBTBEntry& evicted_entry)
{
    if (victimCacheSize == 0) return;

    // 1) If same PC exists, update and refresh tick
    for (auto &entry : victimCache) {
        if (entry.valid && entry.pc == evicted_entry.pc) {
            entry = evicted_entry;
            entry.tick = curTick();
            return;
        }
    }

    // 2) Try to find an invalid slot first
    for (auto &entry : victimCache) {
        if (!entry.valid) {
            entry = evicted_entry;
            entry.tick = curTick();
            entry.valid = true;
            return;
        }
    }

    // 3) No invalid slot: find LRU (smallest tick)
    auto lru_it = victimCache.begin();
    for (auto it = victimCache.begin(); it != victimCache.end(); ++it) {
        if (it->tick < lru_it->tick) {
            lru_it = it;
        }
    }
    DPRINTF(BTB, "LRU replace in victim cache, evict pc %#lx with pc %#lx\n",
            lru_it->pc, evicted_entry.pc);
    *lru_it = evicted_entry;
    lru_it->tick = curTick();
}

#ifndef UNIT_TEST
void
MBTB::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
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
#endif

#ifndef UNIT_TEST
MBTB::BTBStats::BTBStats(statistics::Group* parent) :
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
    ADD_STAT(returnMisses, statistics::units::Count::get(), "returns committed that was predicted miss"),

    ADD_STAT(victimCacheHit, statistics::units::Count::get(), "victim cache hits"),
    ADD_STAT(victimCachePromote, statistics::units::Count::get(), "entries promoted from victim cache")

{
}
#endif

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
