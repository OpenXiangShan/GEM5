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
 * Branch Target Buffer (BTB) Implementation
 *
 * The BTB is a cache-like structure that stores information about branches:
 * - Branch type (conditional, unconditional, indirect, call, return)
 * - Branch target address
 * - Branch prediction information (counter for conditional branches)
 *
 * Key Features:
 * - N-way set associative organization
 * - MRU (Most Recently Used) replacement policy
 * - Support for multiple branch types
 * - Support for multiple prediction stages (L0/L1 BTB)
 */

#ifndef __CPU_PRED_BTB_UBTB_HH__
#define __CPU_PRED_BTB_UBTB_HH__

#include <queue>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "debug/UBTB.hh"
#include "params/UBTB.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class UBTB : public TimedBaseBTBPredictor
{
  private:

  public:

    typedef UBTBParams Params;

    UBTB(const Params& p);

    /*
     * Micro-BTB Entry with timestamp for MRU replacement
     *
     * This structure extends BranchInfo to implement a uBTB entry with:
     * - valid: validity bit for this entry
     * - tctr: 2-bit saturation counter for branch direction prediction
     * - uctr: 2-bit saturation counter used by the replacement policy
     * - tag: tag bits from fetch block address [23:1]p
     * - tick: timestamp used for MRU (Most Recently Used) replacement policy
     */
    typedef struct TickedUBTBEntry : public BTBEntry
    {
        unsigned uctr; //2-bit saturation counter used in replacement policy
        uint64_t tick;  // timestamp for MRU replacement
        int  numNTConds; // number of conditional branches before the taken branch
        TickedUBTBEntry() : BTBEntry(), uctr(0), tick(0), numNTConds(0) {}
        TickedUBTBEntry(const BTBEntry &be, uint64_t tick) : BTBEntry(be), uctr(0), tick(tick), numNTConds(0) {}
    }TickedUBTBEntry;

    // A BTB set is a vector of entries (ways)
    using UBTBSet = std::vector<TickedUBTBEntry>;
    using UBTBSetIter = typename UBTBSet::iterator;
    // MRU heap for each set
    using UBTBHeap = std::vector<UBTBSetIter>;

    /** Creates a BTB with the given number of entries, number of bits per
     *  tag, and instruction offset amount.
     *  @param numEntries Number of entries for the BTB.
     *  @param tagBits Number of bits for each tag in the BTB.
     *  @param instShiftAmt Offset amount for instructions to ignore alignment.
     */
    UBTB(unsigned numEntries, unsigned tagBits,
               unsigned instShiftAmt, unsigned numThreads);

    void tickStart() override{};

    void tick() override{};

    /*
     * Main prediction function
     * @param startAddr: start address of the fetch block
     * @param history: branch history register
     * @param stagePreds: predictions for each pipeline stage
     *
     * This function:
     * 1. Looks up BTB entries for the fetch block
     * 2. Updates prediction statistics
     * 3. Fills predictions for each pipeline stage
     */
    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    void updateUsingS3Pred(FullBTBPrediction &s3Pred);

    /** Get prediction BTBMeta
     *  @return Returns the prediction meta
     */
    std::shared_ptr<void> getPredictionMeta() override
    {
        std::shared_ptr<void> meta_void_ptr = std::make_shared<UBTBMeta>(meta);
        return meta_void_ptr;
    }
    // not used
    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override {}

    void recoverHist(const boost::dynamic_bitset<> &history,
        const FetchStream &entry, int shamt, bool cond_taken) override{};


    void reset();

    /** Updates the BTB with the branch info of a block and execution result.
     *  This function:
     *  1. Updates existing entries with new information
     *  2. Adds new entries if necessary
     *  3. Updates MRU information
     */
    void update(const FetchStream &stream) override;

    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    void setTrace() override;

    TraceManager *ubtbTrace;

    void printTickedUBTBEntry(const TickedUBTBEntry &e) {
        DPRINTF(UBTB, "uBTB entry: valid %d, pc:%#lx, tag: %#lx, size:%d, target:%#lx, \
            cond:%d, indirect:%d, call:%d, return:%d, tick:%lu\n",
            e.valid, e.pc, e.tag, e.size, e.target, e.isCond, e.isIndirect, e.isCall, e.isReturn, e.tick);
    }

    void dumpMruList() {
        DPRINTF(UBTB, "MRU list:\n");
        for (const auto &it: mruList) {
            printTickedUBTBEntry(*it);
        }
    }



  private:

    typedef struct LastPred
    {
        UBTBSetIter hit_entry; // this might point to ubtb.end()

        LastPred() {
            // Default constructor - will be assigned proper value later
        }
    }LastPred;

    typedef struct UBTBMeta
    {
        TickedUBTBEntry hit_entry;
        UBTBMeta() {
            hit_entry = TickedUBTBEntry();
        }
    }UBTBMeta;

    // helper methods
    /*
     * Comparator for MRU heap
     * Returns true if a's timestamp is larger than b's
     * This creates a min-heap where the oldest entry is at the top
     */
    struct older
    {
        bool operator()(const UBTBSetIter &a, const UBTBSetIter &b) const
        {
            return a->tick > b->tick;
        }
    };

    /** Returns the tag bits of a given address.
     *  The tag is calculated as: (pc >> tagShiftAmt) & tagMask
     *  where tagShiftAmt = idxShiftAmt + log2(numSets)
     *  @param inst_PC The branch's address.
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr startPC) {
        return (startPC >> 1) & tagMask;
    }

    /** Update the 2-bit saturating counter for conditional branches
     *  Counter range: [-2, 1]
     *  - Increment on taken (max 1)
     *  - Decrement on not taken (min -2)
     */
    void updateTCtr(int &ctr, bool taken) {
        if (taken && ctr < 1) {ctr++;}
        if (!taken && ctr > -2) {ctr--;}
    }

    void updateUCtr(unsigned &ctr, bool inc) {
        if (inc && ctr < 3) {ctr++;}
        if (!inc && ctr > 0) {ctr--;}
    }

    UBTBSetIter lookup(Addr block_pc);

    void PredStatistics(const TickedUBTBEntry entry, Addr startAddr);

    /** Fill predictions for each pipeline stage based on BTB entries
     *  @param entry The BTB entry containing branch info
     *  @param stagePreds Predictions for each pipeline stage
     */
    void fillStagePredictions(const TickedUBTBEntry& entry,
                              std::vector<FullBTBPrediction>& stagePreds);

    void replaceOldEntry(UBTBSetIter oldEntry,FullBTBPrediction & newPrediction);







    /** The BTB structure:
     *  - Organized as numSets sets
     *  - Each set has numWays ways
     *  - Total size = numSets * numWays = numEntries
     */
    std::vector<TickedUBTBEntry> ubtb;


    LastPred lastPred; // last prediction, set in putPCHistory, used in updateUsingS3Pred
    UBTBMeta meta; // metadata for uBTB, set in putPCHistory, used in update, for the sole purpose of statistics

    /** MRU tracking:
     *  - One heap per set
     *  - Each heap tracks the MRU order of entries in that set
     *  - Oldest entry is at the top of heap
     */
    UBTBHeap mruList;

    /** BTB configuration parameters */
    unsigned numEntries;    // Total number of entries

    /** Address calculation masks and shifts */
    unsigned tagBits;      // Number of tag bits
    Addr tagMask;          // Mask for extracting tag bits

    enum Mode
    {
        READ, WRITE, EVICT
    };

    struct UBTBStats : public statistics::Group
    {

        statistics::Scalar predMiss;
        statistics::Scalar predHit;
        statistics::Scalar updateMiss;
        statistics::Scalar updateHit;
        statistics::Scalar updateExisting;
        statistics::Scalar updateReplace;
        statistics::Scalar updateReplaceValidOne;


        statistics::Scalar S0Predmiss;
        statistics::Scalar S0PredUseUBTB;
        statistics::Scalar S0PredUseABTB;

        // per branch statistics
        statistics::Scalar allBranchHits;
        statistics::Scalar allBranchHitTakens;
        statistics::Scalar allBranchHitNotTakens;
        statistics::Scalar allBranchMisses;
        statistics::Scalar allBranchMissTakens;
        statistics::Scalar allBranchMissNotTakens;

        statistics::Scalar condHits;
        statistics::Scalar condHitTakens;
        statistics::Scalar condHitNotTakens;
        statistics::Scalar condMisses;
        statistics::Scalar condMissTakens;
        statistics::Scalar condMissNotTakens;
        statistics::Scalar condPredCorrect;
        statistics::Scalar condPredWrong;

        statistics::Scalar uncondHits;
        statistics::Scalar uncondMisses;

        statistics::Scalar indirectHits;
        statistics::Scalar indirectMisses;
        statistics::Scalar indirectPredCorrect;
        statistics::Scalar indirectPredWrong;

        statistics::Scalar callHits;
        statistics::Scalar callMisses;

        statistics::Scalar returnHits;
        statistics::Scalar returnMisses;

        UBTBStats(statistics::Group* parent);
    } ubtbStats;


};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_UBTB_HH__
