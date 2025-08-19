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

/*
 * MBTB (Half-Aligned BTB) Implementation
 * 
 * The MBTB is a BTB that uses half-aligned entries:
 * - Each entry covers a 32-byte aligned block
 * - Lookups search two consecutive 32-byte blocks
 * - Uses blockSize-aligned addressing (blockSize = 32)
 * - No ahead pipelining stages
 */

#ifndef __CPU_PRED_BTB_MBTB_HH__
#define __CPU_PRED_BTB_MBTB_HH__

#include "cpu/pred/btb/base_btb.hh"

// Conditional includes based on build mode
#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "params/MBTB.hh"
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

class MBTB : public BaseBTB
{
  public:

#ifdef UNIT_TEST
    // Test constructor
    MBTB(unsigned numEntries, unsigned tagBits, unsigned numWays, unsigned numDelay,
         uint64_t blockSize = 32);
#else
    // Production constructor
    typedef MBTBParams Params;
    MBTB(const Params& p);
#endif

    /*
     * Main prediction function
     * @param startAddr: start address of the fetch block
     * @param history: branch history register
     * @param stagePreds: predictions for each pipeline stage
     */
    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

  protected:
    /** Half-aligned lookup function that searches two consecutive 32B blocks */
    std::vector<TickedBTBEntry> lookup(Addr block_pc) override;

    /** Override index calculation for half-aligned addressing */
    inline Addr getIndex(Addr instPC) override {
        return (instPC >> idxShiftAmt) & idxMask;
    }

    /** Override tag calculation for half-aligned addressing */
    inline Addr getTag(Addr instPC) override {
        return (instPC >> tagShiftAmt) & tagMask;
    }

    /** Lookup entries in a single 32B block */
    std::vector<TickedBTBEntry> lookupSingleBlock(Addr block_pc);

  private:
    uint64_t blockSize;  // Size of each block (32 bytes for MBTB)
};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_MBTB_HH__