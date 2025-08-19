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
 * Initializes half-aligned BTB with 32-byte blocks
 */
#ifdef UNIT_TEST
// Test constructor
MBTB::MBTB(unsigned numEntries, unsigned tagBits, unsigned numWays, unsigned numDelay,
           uint64_t blockSize)
    : BaseBTB(numEntries, tagBits, numWays, numDelay),
      blockSize(blockSize)
{
    // Set shift amounts for half-aligned addressing
    idxShiftAmt = floorLog2(blockSize);
    tagShiftAmt = idxShiftAmt + floorLog2(numEntries / numWays);

    // Initialize BTB structure
    initializeBTB();
}
#else
// Production constructor
MBTB::MBTB(const Params &p)
    : BaseBTB(p),
      blockSize(p.blockSize)
{
    // Set MBTB specific parameters
    numEntries = p.numEntries;
    numWays = p.numWays;
    tagBits = p.tagBits;
    
    // Set shift amounts for half-aligned addressing
    idxShiftAmt = floorLog2(blockSize);
    tagShiftAmt = idxShiftAmt + floorLog2(numEntries / numWays);
    
    // Initialize BTB structure
    initializeBTB();
}
#endif

void
MBTB::putPCHistory(Addr startAddr,
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

/**
 * Half-aligned lookup that searches two consecutive 32B blocks
 */
std::vector<MBTB::TickedBTBEntry>
MBTB::lookup(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }

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

    DPRINTF(BTB, "MBTB: Half-aligned lookup results:\n");
    dumpTickedBTBEntries(res);
    
    return res;
}

/**
 * Lookup entries in a single 32B block
 */
std::vector<MBTB::TickedBTBEntry>
MBTB::lookupSingleBlock(Addr block_pc)
{
    std::vector<TickedBTBEntry> res;
    if (block_pc & 0x1) {
        return res; // ignore false hit when lowest bit is 1
    }
    
    Addr btb_idx = getIndex(block_pc);
    auto btb_set = btb[btb_idx];
    assert(btb_idx < numSets);
    
    Addr current_tag = getTag(block_pc);
    
    DPRINTF(BTB, "MBTB: Doing tag comparison for index 0x%lx tag %#lx\n",
        btb_idx, current_tag);
        
    for (auto &way : btb_set) {
        if (way.valid && way.tag == current_tag) {
            res.push_back(way);
            way.tick = curTick();  // Update timestamp for MRU
            std::make_heap(mruList[btb_idx].begin(), mruList[btb_idx].end(), older());
        }
    }
    
    return res;
}

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
