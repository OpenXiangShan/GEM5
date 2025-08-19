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
 * ABTB (Ahead-Pipelined BTB) Implementation
 * 
 * The ABTB is a BTB that supports ahead pipelining:
 * - Uses ahead pipelined stages (aheadPipelinedStages = 1)
 * - Manages aheadReadBtbEntries queue for pipelining
 * - Uses special tag calculation (starts from second bit)
 * - Tracks S0 prediction statistics
 * - No half-aligned entries (blockSize doesn't matter)
 */

#ifndef __CPU_PRED_BTB_ABTB_HH__
#define __CPU_PRED_BTB_ABTB_HH__

#include <queue>

#include "base/types.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/base_btb.hh"

// Conditional includes based on build mode
#ifdef UNIT_TEST
    #include <gmock/gmock.h>
    #include <gtest/gtest.h>
    #include "cpu/pred/btb/test/test_dprintf.hh"
    #include "cpu/pred/btb/test/timed_base_pred.hh"
#else
    #include "arch/generic/pcstate.hh"
    #include "base/logging.hh"
    #include "config/the_isa.hh"
    #include "debug/BTB.hh"
    #include "debug/BTBStats.hh"
    #include "debug/AheadPipeline.hh"
    #include "params/ABTB.hh"
    #include "cpu/pred/btb/timed_base_pred.hh"
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

class ABTB : public BaseBTB
{
  public:

#ifdef UNIT_TEST
    // Test constructor
    ABTB(unsigned numEntries, unsigned tagBits, unsigned numWays, unsigned numDelay,
         unsigned aheadPipelinedStages = 1);
#else
    // Production constructor
    typedef ABTBParams Params;
    ABTB(const Params& p);
#endif

    /*
     * Main prediction function
     * @param startAddr: start address of the fetch block
     * @param history: branch history register
     * @param stagePreds: predictions for each pipeline stage
     */
    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    /** Override recoverHist to handle ahead pipeline queue */
    void recoverHist(const boost::dynamic_bitset<> &history,
        const FetchStream &entry, int shamt, bool cond_taken) override;

  protected:
    /** Ahead-pipelined lookup function */
    std::vector<TickedBTBEntry> lookup(Addr block_pc) override;

    /** Override tag calculation for ahead pipelining (starts from second bit) */
    inline Addr getTag(Addr instPC) override {
        return (instPC >> tagShiftAmt) & tagMask;
    }

    /** Fill predictions with S0 statistics tracking */
    void fillStagePredictions(const std::vector<TickedBTBEntry>& entries,
                             std::vector<FullBTBPrediction>& stagePreds) override;

    /** Lookup entries in a single block with ahead pipelining */
    std::vector<TickedBTBEntry> lookupSingleBlock(Addr block_pc);

    /** Get the previous PC from the fetch stream for ahead pipelining */
    Addr getPreviousPC(const FetchStream &stream);

    /** Override update to handle ahead pipelining address calculation */
    void update(const FetchStream &stream) override;

  private:
    unsigned aheadPipelinedStages;  // Number of ahead pipelined stages
    std::queue<std::tuple<Addr, Addr, BTBSet>> aheadReadBtbEntries;  // Queue for ahead pipelining

#ifdef UNIT_TEST
    typedef uint64_t Scalar;
#else
    typedef statistics::Scalar Scalar;
#endif

    // Additional S0 prediction statistics for ABTB
#ifdef UNIT_TEST
    struct BTBStats {
#else
    struct ABTBStats : public statistics::Group {
#endif
        Scalar S0Predmiss;
        Scalar S0PredUseUBTB;
        Scalar S0PredUseABTB;

#ifndef UNIT_TEST
        ABTBStats(statistics::Group* parent);
#endif
    } abtbStats;
};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_ABTB_HH__