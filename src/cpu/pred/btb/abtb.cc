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

// Additional conditional includes based on build mode
#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "base/trace.hh"
    #include "debug/Fetch.hh"
    #include "debug/AheadPipeline.hh"
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
 * ABTB Constructor
 * Initializes ahead-pipelined BTB
 */
#ifdef UNIT_TEST
// Test constructor
ABTB::ABTB(unsigned numEntries, unsigned tagBits, unsigned numWays, unsigned numDelay,
           unsigned aheadPipelinedStages)
    : BaseBTB(numEntries, tagBits, numWays, numDelay),
      aheadPipelinedStages(aheadPipelinedStages)
{
    // Set shift amounts for ahead pipelining
    idxShiftAmt = 1;  // Not aligned to blockSize
    tagShiftAmt = idxShiftAmt;  // Tag starts from second bit
    
    // Initialize BTB structure
    initializeBTB();
}
#else
// Production constructor
ABTB::ABTB(const Params &p)
    : BaseBTB(p),
      aheadPipelinedStages(p.aheadPipelinedStages),
      abtbStats(this)
{
    // Set ABTB specific parameters
    numEntries = p.numEntries;
    numWays = p.numWays;
    tagBits = p.tagBits;
    
    // Set shift amounts for ahead pipelining
    idxShiftAmt = 1;  // Not aligned to blockSize
    tagShiftAmt = idxShiftAmt;  // Tag starts from second bit
    
    // Initialize BTB structure
    initializeBTB();
}
#endif

void
ABTB::putPCHistory(Addr startAddr,
                   const boost::dynamic_bitset<> &history,
                   std::vector<FullBTBPrediction> &stagePreds)
{
    meta = std::make_shared<BTBMeta>();
    
    // Lookup all matching entries in BTB
    auto find_entries = lookup(startAddr);
    
    // Process BTB entries
    auto processed_entries = processEntries(find_entries, startAddr);
    
    // Fill predictions for each pipeline stage with S0 statistics
    fillStagePredictions(processed_entries, stagePreds);
    
    // Update metadata for later stages
    updatePredictionMeta(processed_entries, stagePreds);
}

/**
 * Fill predictions with S0 statistics tracking
 */
void
ABTB::fillStagePredictions(const std::vector<TickedBTBEntry>& entries,
                          std::vector<FullBTBPrediction>& stagePreds)
{
    // S0 prediction source statistic is tracked by ABTB
    if (aheadPipelinedStages > 0) {
        if (stagePreds[0].btbEntries.size() > 0) {
            DPRINTF(BTB, "BTB: predsOfEachStage are already filled by uBTB, skipping ABTB prediction\n");
            abtbStats.S0PredUseUBTB++;
            return;
        }
    }

    // Call base class implementation for basic prediction filling
    BaseBTB::fillStagePredictions(entries, stagePreds);

    // Update S0 prediction source statistics
    if (aheadPipelinedStages > 0) {
        if (entries.size() > 0) {
            abtbStats.S0PredUseABTB++;
        } else {
            abtbStats.S0Predmiss++;
        }
    }
}

void
ABTB::recoverHist(const boost::dynamic_bitset<> &history, const FetchStream &entry, int shamt, bool cond_taken)
{
    // Clear ahead pipeline first
    while (!aheadReadBtbEntries.empty()) {
        aheadReadBtbEntries.pop();
    }
    
    // Call base class implementation
    BaseBTB::recoverHist(history, entry, shamt, cond_taken);
}

/**
 * Ahead-pipelined lookup function
 */
std::vector<ABTB::TickedBTBEntry>
ABTB::lookup(Addr block_pc)
{
    return lookupSingleBlock(block_pc);
}

/**
 * Lookup entries in a single block with ahead pipelining
 */
std::vector<ABTB::TickedBTBEntry>
ABTB::lookupSingleBlock(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }
    
    Addr btb_idx = getIndex(block_pc);
    auto btb_set = btb[btb_idx];
    assert(btb_idx < numSets);
    
    // In ahead-pipelined implementations, we do memory access first with
    // address of the previous block, and do tag compare with current address
    // thus we need to store the entry read from memory for later use
    if (aheadPipelinedStages > 0) {
        DPRINTF(AheadPipeline, "BTB: pushing set for ahead-pipelined stages %d, idx %ld\n",
             aheadPipelinedStages, btb_idx);
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
        // Only if the ahead-pipeline is filled can we use the entry
        if (aheadReadBtbEntries.size() >= aheadPipelinedStages + 1) {
            // +1 because we pushed a new set in this cycle before
            assert(aheadReadBtbEntries.size() == aheadPipelinedStages + 1);
            std::tie(current_pc, current_idx, current_set) = aheadReadBtbEntries.front();
            DPRINTF(AheadPipeline, "BTB: ahead-pipeline filled, using set %ld from pc %#lx\n",
                current_idx, current_pc);
            aheadReadBtbEntries.pop();
        } else {
            DPRINTF(AheadPipeline, "BTB: ahead-pipeline not filled, only have %ld sets read,"
                " skipping tag compare, assigning miss\n", aheadReadBtbEntries.size());
            return res;
        }
    }
    
    DPRINTF(BTB, "BTB: Doing tag comparison for index 0x%lx tag %#lx\n",
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

/**
 * Get the previous PC from the fetch stream for ahead pipelining
 */
Addr
ABTB::getPreviousPC(const FetchStream &stream)
{
    // Get pc from the nth previous block, the value of n is aheadPipelinedStages
    auto previous_pcs = stream.previousPCs;
    if (previous_pcs.size() < aheadPipelinedStages) {
        // If the stream is not filled, we cannot update btb
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

/**
 * Override update to handle ahead pipelining address calculation
 */
void
ABTB::update(const FetchStream &stream)
{
    // 1. Process old entries
    auto old_entries = processOldEntries(stream);
    
    // 2. Check prediction hit status, for stats recording
    checkPredictionHit(stream,
        std::static_pointer_cast<BTBMeta>(stream.predMetas[getComponentIdx()]).get());

    // 3. Collect entries to update
    auto entries_to_update = collectEntriesToUpdate(old_entries, stream);
    
    // 4. Update BTB entries with ahead pipelining consideration
    for (auto &entry : entries_to_update) {
        Addr entryPC = entry.pc;
        Addr btb_idx;
        Addr btb_tag;

        if (aheadPipelinedStages > 0) {
            Addr previousPC = getPreviousPC(stream);
            if (previousPC == 0) {
                DPRINTF(BTB, "ahead-pipeline: no previous PC, skipping update\n");
                return;
            }
            btb_idx = getIndex(previousPC);
            btb_tag = getTag(entryPC);
        } else {
            btb_idx = getIndex(entryPC);
            btb_tag = getTag(entryPC);
        }

        updateBTBEntry(btb_idx, btb_tag, entry, stream);
    }
    
    // Verify BTB state
    for (unsigned i = 0; i < numSets; i++) {
        assert(btb[i].size() <= numWays);
        assert(mruList[i].size() <= numWays);
    }
}

#ifndef UNIT_TEST
ABTB::ABTBStats::ABTBStats(statistics::Group* parent) :
    statistics::Group(parent),
    ADD_STAT(S0Predmiss, statistics::units::Count::get(), "misses encountered on S0 prediction, i.e. uBTB and ABTB miss"),
    ADD_STAT(S0PredUseUBTB, statistics::units::Count::get(), "uBTB prediction used, i.e. uBTB hit"),
    ADD_STAT(S0PredUseABTB, statistics::units::Count::get(), "aBTB prediction used, i.e. uBTB miss and ABTB hit")
{
    auto abtb = dynamic_cast<branch_prediction::btb_pred::ABTB*>(parent);
    if (abtb->aheadPipelinedStages == 0) {
        S0Predmiss.prereq(S0Predmiss);
        S0PredUseUBTB.prereq(S0PredUseUBTB);
        S0PredUseABTB.prereq(S0PredUseABTB);
    }
}
#endif

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5