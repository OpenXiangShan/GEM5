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


#include "cpu/pred/btb/abtb.hh"

#include "base/intmath.hh"
#include "stream_struct.hh"

// Additional conditional includes based on build mode
#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "base/trace.hh"
    #include "cpu/o3/dyn_inst.hh"
    #include "debug/AheadPipeline.hh"
    #include "debug/Fetch.hh"
    #include "debug/ABTB.hh"
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
 * BTB Constructor
 * Initializes:
 * - BTB structure (sets and ways)
 * - MRU tracking for each set
 * - Address calculation parameters (index/tag masks and shifts)
 */
#ifdef UNIT_TEST
// Test constructor for unit testing mode - fixed ahead-pipelined configuration
AheadBTB::AheadBTB(unsigned numEntries, unsigned tagBits, unsigned numWays, unsigned numDelay)
    : TimedBaseBTBPredictor(),
      numEntries(numEntries),
      numWays(numWays),
      tagBits(tagBits)
{
    setNumDelay(numDelay);
    this->aheadPipelinedStages = 1; // fixed ahead-pipelined stages = 1
#else
// Production constructor - fixed ahead-pipelined configuration
AheadBTB::AheadBTB(const Params &p)
    : TimedBaseBTBPredictor(p),
    numEntries(p.numEntries),
    numWays(p.numWays),
    tagBits(p.tagBits),
    usingS3Pred(p.usingS3Pred),
    btbStats(this)
{
    // AheadBTB supports configurable ahead-pipelined stages, but must be > 0
    this->aheadPipelinedStages = p.aheadPipelinedStages;
    assert(this->aheadPipelinedStages > 0);
#endif
    // AheadBTB always uses single instruction alignment: | tag | idx | instShiftAmt
    idxShiftAmt = 1;

    assert(numEntries % numWays == 0);
    numSets = numEntries / numWays;
    // AheadBTB always uses ahead-pipelined stages = 1

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
    // AheadBTB always uses ahead-pipelined tag calculation: tag starts from the second bit
    tagShiftAmt = idxShiftAmt;

    DPRINTF(ABTB, "numEntries %d, numSets %d, numWays %d, tagBits %d, tagShiftAmt %d, idxMask %#lx, tagMask %#lx\n",
        numEntries, numSets, numWays, tagBits, tagShiftAmt, idxMask, tagMask);

#ifndef UNIT_TEST
    hasDB = true;
    dbName = std::string("AheadBTB");
#endif
}

#ifndef UNIT_TEST
void
AheadBTB::tickStart()
{
    // nothing to do
}

void
AheadBTB::tick() {}

void
AheadBTB::setTrace()
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
        btbTrace = _db->addAndGetTrace("ABTBTrace", fields_vec);
        btbTrace->init_table();
    }
}
#endif

/**
 * Process BTB entries:
 * 1. Sort entries by PC order
 * 2. Remove entries before the start PC
 */
std::vector<AheadBTB::TickedBTBEntry>
AheadBTB::processEntries(const std::vector<TickedBTBEntry>& entries, Addr startAddr)
{
    int hitNum = entries.size();
    bool hit = hitNum > 0;
    
    // Update prediction statistics
    if (hit) {
        DPRINTF(ABTB, "BTB: lookup hit, dumping hit entry\n");
        btbStats.predHit += hitNum;
        for (auto &entry: entries) {
            printTickedBTBEntry(entry);
        }
    } else {
        btbStats.predMiss++;
        DPRINTF(ABTB, "BTB: lookup miss\n");
    }

    auto processed_entries = entries;
    
    // Sort by instruction order
    std::sort(processed_entries.begin(), processed_entries.end(), 
             [](const BTBEntry &a, const BTBEntry &b) {
                 return a.pc < b.pc;
             });
    return processed_entries;
}

/**
 * Fill predictions for each pipeline stage:
 * 1. Copy BTB entries
 * 2. Set conditional branch predictions
 * 3. Set indirect branch targets
 */
void
AheadBTB::fillStagePredictions(const std::vector<TickedBTBEntry>& entries,
                                    std::vector<FullBTBPrediction>& stagePreds)
{    // S0 prediction source statistic is tracked by AheadBTB
    // AheadBTB always has aheadPipelinedStages > 0
    BTBEntry ubtb_pred_entry;
    std::vector<TickedBTBEntry> mixed_entries;

    // if ubtb has prediction, add ubtb entry to aBTB entries
    if (stagePreds[0].btbEntries.size() > 0) {
        DPRINTF(ABTB, "AheadBTB: predsOfEachStage are already filled by uBTB, skipping AheadBTB prediction\n");
        btbStats.S0PredUseUBTB++;
        //if ubtb has prediction, add ubtb entry to aBTB entries
        ubtb_pred_entry = stagePreds[0].btbEntries[0];
        assert(ubtb_pred_entry.valid);
        mixed_entries = entries;
        mixed_entries.push_back(TickedBTBEntry(ubtb_pred_entry, curTick()));
        // Deduplicate entries by pc (order can change)
        std::sort(mixed_entries.begin(), mixed_entries.end(),
              [](const TickedBTBEntry& a, const TickedBTBEntry& b) {
                  return a.pc < b.pc;
              });
        // Remove duplicates
        mixed_entries.erase(std::unique(mixed_entries.begin(), mixed_entries.end(),
              [](const TickedBTBEntry& a, const TickedBTBEntry& b) {
                  return a.pc == b.pc;
              }),
              mixed_entries.end());
        // return;
    } else {// no uBTB prediction, only use aBTB prediction
        mixed_entries = entries;
        if (entries.size() > 0) {
            btbStats.S0PredUseABTB++;
        } else {
            btbStats.S0Predmiss++;
        }
    }

    FillStageLoop(s) {
        DPRINTF(ABTB, "BTB: assigning prediction for stage %d\n", s);
        // Copy BTB entries to stage prediction
        stagePreds[s].btbEntries.clear();
        for (auto e : mixed_entries) {
            stagePreds[s].btbEntries.push_back(BTBEntry(e));
        }
        checkAscending(stagePreds[s].btbEntries);
        dumpBTBEntries(stagePreds[s].btbEntries);

        stagePreds[s].predTick = curTick();
        stagePreds[s].condTakens.clear();
        stagePreds[s].indirectTargets.clear();
    }

    // Set predictions for each branch
    for (auto &e : mixed_entries) {
        assert(e.valid);
        if (e.isCond) {
            FillStageLoop(s) stagePreds[s].condTakens.push_back({e.pc, e.alwaysTaken || (e.ctr >= 0)});
        } else if (e.isIndirect) {
            // Set predicted target for indirect branches
            DPRINTF(ABTB, "setting indirect target for pc %#lx to %#lx\n", e.pc, e.target);

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
 * 2. Save current BTB entries
 */
void
AheadBTB::updatePredictionMeta(const std::vector<TickedBTBEntry>& entries,
                                   std::vector<FullBTBPrediction>& stagePreds)
{
    // Save current BTB entries
    for (auto e: entries) {
        meta->hit_entries.push_back(BTBEntry(e));
    }

    lastPredEntries = meta->hit_entries;
}

void
AheadBTB::putPCHistory(Addr startAddr,
                         const boost::dynamic_bitset<> &history,
                         std::vector<FullBTBPrediction> &stagePreds)
{
    meta = std::make_shared<BTBMeta>();
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
AheadBTB::getPredictionMeta()
{
    // Lazy-initialize meta so callers never observe a null pointer
    // This avoids early-cycle crashes when prediction hasn't populated meta yet
    if (!meta) {
        meta = std::make_shared<BTBMeta>();
    }
    return meta;
}

void
AheadBTB::specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) {}

void
AheadBTB::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    // clear ahead pipeline first
    while (!aheadReadBtbEntries.empty()) {
        aheadReadBtbEntries.pop();
    }
}


/**
 * Helper function to lookup entries in a single block
 * @param block_pc The aligned PC to lookup
 * @return Vector of matching BTB entries
 */
std::vector<AheadBTB::TickedBTBEntry>
AheadBTB::lookupSingleBlock(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }
    Addr btb_idx = getIndex(block_pc);
    auto btb_set = btb[btb_idx];
    assert(btb_idx < numSets);
    // AheadBTB always uses ahead-pipelined implementation:
    // memory access with previous block PC, tag compare with current PC
    DPRINTF(AheadPipeline, "AheadBTB: pushing set for ahead-pipelined stages, idx %ld\n", btb_idx);
    aheadReadBtbEntries.push(std::make_tuple(block_pc, btb_idx, btb_set));

    Addr tag_curStartpc = getTag(block_pc);// abtb uses current FB pc to get tag
    Addr pc = 0;
    Addr idx_prvStartpc = 0;// abtb uses previous FB pc to get index
    BTBSet set;
    // AheadBTB always uses ahead-pipelined logic (aheadPipelinedStages > 0)
    // only if the ahead-pipeline is filled can we use the entry
    if (aheadReadBtbEntries.size() >= aheadPipelinedStages+1) {
        // +1 because we pushed a new set in this cycle before
        // in case there are push without corresponding pop
        assert(aheadReadBtbEntries.size() == aheadPipelinedStages+1);
        std::tie(pc, idx_prvStartpc, set) = aheadReadBtbEntries.front();
        DPRINTF(AheadPipeline, "AheadBTB: ahead-pipeline filled, using set %ld from pc %#lx\n",
            idx_prvStartpc, pc);
        DPRINTF(AheadPipeline, "AheadBTB: dumping btb set\n");
        for (auto &entry : set) {
            printTickedBTBEntry(entry);
        }
        aheadReadBtbEntries.pop();
    } else {
        DPRINTF(AheadPipeline, "AheadBTB: ahead-pipeline not filled, only have %ld sets read,"
            " skipping tag compare, assigning miss\n", aheadReadBtbEntries.size());
    }
    DPRINTF(ABTB, "BTB: Doing tag comparison for index 0x%lx tag %#lx\n",
        idx_prvStartpc, tag_curStartpc);
    for (auto &way : set) {
        if (way.valid && way.tag == tag_curStartpc) {
            res.push_back(way);
            way.tick = curTick();  // Update timestamp for MRU
            std::make_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
        }
    }
    return res;
}

std::vector<AheadBTB::TickedBTBEntry>
AheadBTB::lookup(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }

    // AheadBTB always uses single block lookup
    res = lookupSingleBlock(block_pc);
    return res;
}


/**
 * Process old BTB entries from prediction metadata
 * 1. Get prediction metadata
 * 2. Remove entries that were not executed
 */
std::vector<BTBEntry>
AheadBTB::processOldEntries(const std::vector<BTBEntry>& hit_entries,
                            Addr end_inst_pc)
{
    // auto meta = std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]);
    // // hit entries whose corresponding insts are acutally executed
    // Addr end_inst_pc = stream.updateEndInstPC;
    DPRINTF(ABTB, "end_inst_pc: %#lx\n", end_inst_pc);
    // remove not executed btb entries, pc > end_inst_pc
    auto old_entries = hit_entries;
    DPRINTF(ABTB, "old_entries.size(): %lu\n", old_entries.size());
    //dumpBTBEntries(old_entries);
    auto remove_it = std::remove_if(old_entries.begin(), old_entries.end(),
        [end_inst_pc](const BTBEntry &e) { return e.pc > end_inst_pc; });
    old_entries.erase(remove_it, old_entries.end());
    DPRINTF(ABTB, "after removing not executed insts, old_entries.size(): %lu\n", old_entries.size());
    //dumpBTBEntries(old_entries);

    btbStats.updateHit += old_entries.size();
    
    return old_entries;
}

/**
 * Check if the branch was predicted correctly
 */
void
AheadBTB::checkPredictionHit(const FetchStream &stream, const BTBMeta* meta)
{
    bool pred_branch_hit = false;
    for (auto &e : meta->hit_entries) {
        if (stream.exeBranchInfo == e) {
            pred_branch_hit = true;
            break;
        }
    }
    if (!pred_branch_hit && stream.exeTaken) {
        DPRINTF(ABTB, "update miss detected, pc %#lx, predTick %lu\n", stream.exeBranchInfo.pc, stream.predTick);
        btbStats.updateMiss++;
    }

}


/**
 * Collect all entries that need to be updated
 * 1. Process old entries
 * 2. Add new entry if necessary
 */
std::vector<BTBEntry>
AheadBTB::collectEntriesToUpdate(const std::vector<BTBEntry>& old_entries,
                                     const FetchStream &stream)
{
    auto all_entries = old_entries;

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

    DPRINTF(ABTB, "all_entries_to_update.size(): %lu\n", all_entries.size());
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
AheadBTB::updateBTBEntry(Addr btb_idx, Addr btb_tag, const BTBEntry& entry,
                                        const BranchInfo takenbranchinfo,const bool isTaken)
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
    entry_to_write.resolved = false; // reset resolved bit on update
    entry_to_write.tag = btb_tag;   // update tag after found it!
    // update saturating counter if necessary
    if (entry_to_write.isCond) {
        bool this_cond_taken = isTaken && takenbranchinfo.pc == entry_to_write.pc;
        if (!this_cond_taken) {
            entry_to_write.alwaysTaken = false;
        }
        if (!entry_to_write.alwaysTaken) {
            updateCtr(entry_to_write.ctr, this_cond_taken);
        }
    }
    // update indirect target if necessary
    if (entry_to_write.isIndirect && isTaken && takenbranchinfo.pc == entry_to_write.pc) {
        entry_to_write.target = takenbranchinfo.target;
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
    } else {
        // Replace oldest entry in the set
        DPRINTF(ABTB, "trying to replace entry in set %#lx\n", btb_idx);
        dumpMruList(mruList[btb_idx]);
        // put the oldest entry in this set to the back of heap
        std::pop_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
        const auto& entry_in_btb_now = mruList[btb_idx].back();
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
        }
        btbStats.updateReplace++;
        DPRINTF(ABTB, "BTB: Replacing entry with tag %#lx, pc %#lx in set %#lx\n",
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
        dumpMruList(mruList[btb_idx]);
    }
    std::make_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
}


void
AheadBTB::updateUsingS3Pred(FullBTBPrediction &s3Pred, const Addr previousPC)
{
    if (!usingS3Pred) {
        DPRINTF(ABTB, "AheadBTB: not using S3 prediction for update, skipping\n");
        return;
    }

    Addr end_inst_pc = s3Pred.isTaken() ? s3Pred.getTakenEntry().pc :
                            (s3Pred.bbStart + predictWidth) & ~mask(floorLog2(predictWidth)-1);

    // AheadBTB use S3 prediction for update
    auto old_entries= processOldEntries(lastPredEntries, end_inst_pc);

    auto entries_to_update = collectEntriesToUpdateFromS3Pred(old_entries,s3Pred);

    for (auto &entry : entries_to_update) {
        Addr startPC = s3Pred.bbStart;
        Addr btb_tag = getTag(startPC);  // use last pc to get tag
        if (previousPC == 0) {
            DPRINTF(ABTB, "AheadBTB: no previous PC, skipping update\n");
            return;
        }
        Addr btb_idx = getIndex(previousPC);  // use last pc to get idx
        BranchInfo takenbranchinfo;
        takenbranchinfo.pc = s3Pred.getTakenEntry().pc;
        takenbranchinfo.target = s3Pred.getTakenEntry().target;

        updateBTBEntry(btb_idx, btb_tag, entry, takenbranchinfo, s3Pred.isTaken());
    }
}
std::vector<BTBEntry>
AheadBTB::collectEntriesToUpdateFromS3Pred(const std::vector<BTBEntry>& old_entries,
                                     FullBTBPrediction &s3Pred)
{
    auto all_entries = old_entries;
    BTBEntry new_entry = BTBEntry();
    // which causes its counter to update twice unintentionally
    // we need to check if the new entry already exists in uBTB
    bool pred_branch_hit = false;
    for (auto &e: old_entries) {
        if (s3Pred.getTakenEntry() == e) {
            pred_branch_hit = true;
            break;
        }
    }
    if (!pred_branch_hit&& s3Pred.isTaken()) {
        new_entry = s3Pred.getTakenEntry();
        new_entry.valid = true;

        if (new_entry.isCond) {
            new_entry.alwaysTaken = true;
            new_entry.ctr = 0;
        }
        all_entries.push_back(new_entry);
    }

    DPRINTF(ABTB, "all_entries_to_update.size(): %lu\n", all_entries.size());
    return all_entries;
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
AheadBTB::update(const FetchStream &stream)
{
    if (usingS3Pred) {
        DPRINTF(ABTB, "AheadBTB: using S3 prediction for update, skipping AheadBTB update\n");
        return;
    }
    auto meta = std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]).get();
    Addr end_inst_pc = stream.updateEndInstPC;

    // 1. Process old entries
    auto old_entries = processOldEntries(meta->hit_entries, end_inst_pc);

    // 2. Check prediction hit status, for stats recording
    checkPredictionHit(stream,
        std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]).get());

    // 3. Collect entries to update
    auto entries_to_update = collectEntriesToUpdate(old_entries, stream);
    
    // 4. Update BTB entries - each entry uses its own PC to calculate index and tag
    for (auto &entry : entries_to_update) {
        Addr startPC = stream.getRealStartPC();
        Addr btb_tag = getTag(startPC);  // use current pc to get tag

        // AheadBTB always uses ahead-pipelined update logic
        Addr previousPC = getPreviousPC(stream);
        if (previousPC == 0) {
            DPRINTF(ABTB, "AheadBTB: no previous PC, skipping update\n");
            return;
        }
        Addr btb_idx = getIndex(previousPC);  // use last pc to get idx
        updateBTBEntry(btb_idx, btb_tag, entry, stream.exeBranchInfo, stream.exeTaken);
    }
}

/**
 * Get the previous PC from the fetch stream, useful in the update of ahead-pipelined BTB
 * @param stream Fetch stream containing prediction info
 * @return Previous PC, 0 if the stream is not filled
 */
Addr
AheadBTB::getPreviousPC(const FetchStream &stream)
{
    // get pc from the nth previous block, the value of n is aheadPipelinedStages
    auto previous_pcs = stream.previousPCs;
    if (previous_pcs.size() < aheadPipelinedStages) {
        // if the stream is not filled, we cannot update btb
        DPRINTF(AheadPipeline, "BTB: ahead-pipeline not filled, only have %ld pcs read,"
            " skipping btb update\n", previous_pcs.size());
        return 0;
    } else {
        DPRINTF(AheadPipeline, "BTB: ahead-pipeline filled, using pc %d blocks before,"
            " previousPC.size() %ld\n", aheadPipelinedStages, previous_pcs.size());
        while (previous_pcs.size() > aheadPipelinedStages) {
            previous_pcs.pop();
        }
        return previous_pcs.front();
    }
}

#ifndef UNIT_TEST
void
AheadBTB::commitBranch(const FetchStream &stream, const DynInstPtr &inst)
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
                bool pred_taken = entry.ctr >= 0;
                if (pred_taken == this_branch_taken) {
                    btbStats.condPredCorrect++;
                } else {
                    btbStats.condPredWrong++;
                }
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
                btbStats.condPredWrong++;
            } else {
                btbStats.condMissNotTakens++;
                btbStats.condPredCorrect++;
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
AheadBTB::BTBStats::BTBStats(statistics::Group* parent) :
    statistics::Group(parent),

    ADD_STAT(predMiss, statistics::units::Count::get(), "misses encountered on prediction"),
    ADD_STAT(predHit, statistics::units::Count::get(), "hits encountered on prediction"),
    ADD_STAT(updateMiss, statistics::units::Count::get(), "misses encountered on update"),
    ADD_STAT(updateHit, statistics::units::Count::get(), "hits encountered on update"),
    ADD_STAT(updateExisting, statistics::units::Count::get(), "existing entries updated"),
    ADD_STAT(updateReplace, statistics::units::Count::get(), "entries replaced"),
    ADD_STAT(updateReplaceValidOne, statistics::units::Count::get(), "entries replaced with valid entry"),

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
}
#endif

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
