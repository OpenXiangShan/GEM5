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

#ifndef __CPU_PRED_BTB_BTB_HH__
#define __CPU_PRED_BTB_BTB_HH__

#include <queue>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/pred/btb/stream_struct.hh"
#include "cpu/pred/btb/timed_base_pred.hh"
#include "debug/BTB.hh"
#include "debug/BTBStats.hh"
#include "params/DefaultBTB.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

class DefaultBTB : public TimedBaseBTBPredictor
{
  private:

  public:

    typedef DefaultBTBParams Params;

    DefaultBTB(const Params& p);

    /*
     * BTB Entry with timestamp for MRU replacement
     * Inherits from BTBEntry which contains:
     * - valid: whether this entry is valid
     * - pc: branch instruction address
     * - target: branch target address
     * - size: branch instruction size
     * - isCond/isIndirect/isCall/isReturn: branch type flags
     * - alwaysTaken: whether this conditional branch is always taken
     * - ctr: 2-bit counter for conditional branch prediction
     */
    typedef struct TickedBTBEntry : public BTBEntry
    {
        uint64_t tick;  // timestamp for MRU replacement
        TickedBTBEntry(const BTBEntry &entry, uint64_t tick)
            : BTBEntry(entry), tick(tick) {}
        TickedBTBEntry() : tick(0) {}
    }TickedBTBEntry;

    // A BTB set is a vector of entries (ways)
    using BTBSet = std::vector<TickedBTBEntry>;
    using BTBSetIter = typename BTBSet::iterator;
    // MRU heap for each set
    using BTBHeap = std::vector<BTBSetIter>;
    void tickStart() override;
    
    void tick() override;

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

    /** Get prediction BTBMeta
     *  @return Returns the prediction meta
     */
    std::shared_ptr<void> getPredictionMeta() override;

    // not used
    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override;

    void recoverHist(const boost::dynamic_bitset<> &history,
        const FetchStream &entry, int shamt, bool cond_taken) override;

    /** Creates a BTB with the given number of entries, number of bits per
     *  tag, and instruction offset amount.
     *  @param numEntries Number of entries for the BTB.
     *  @param tagBits Number of bits for each tag in the BTB.
     *  @param instShiftAmt Offset amount for instructions to ignore alignment.
     */
    DefaultBTB(unsigned numEntries, unsigned tagBits,
               unsigned instShiftAmt, unsigned numThreads);

    void reset();
    
    /**
     * @brief derive new btb entry from old ones and set updateNewBTBEntry field in stream
     *        only in L1BTB will this function be called before update
     * 
     * @param stream 
     */
    void getAndSetNewBTBEntry(FetchStream &stream);

    /** Updates the BTB with the branch info of a block and execution result.
     *  This function:
     *  1. Updates existing entries with new information
     *  2. Adds new entries if necessary
     *  3. Updates MRU information
     */
    void update(const FetchStream &stream) override;

    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    void setTrace() override;

    TraceManager *btbTrace;


    void printBTBEntry(const BTBEntry &e, uint64_t tick = 0) {
        DPRINTF(BTB, "BTB entry: valid %d, pc:%#lx, tag: %#lx, size:%d, target:%#lx, \
            cond:%d, indirect:%d, call:%d, return:%d, always_taken:%d, tick:%lu\n",
            e.valid, e.pc, e.tag, e.size, e.target, e.isCond, e.isIndirect, e.isCall, e.isReturn, e.alwaysTaken, tick);
    }

    void printTickedBTBEntry(const TickedBTBEntry &e) {
        printBTBEntry(e, e.tick);
    }

    void dumpBTBEntries(const std::vector<BTBEntry> &es) {
        DPRINTF(BTB, "BTB entries:\n");
        for (const auto &entry : es) {
            printBTBEntry(entry);
        }
    }

    void dumpTickedBTBEntries(const std::vector<TickedBTBEntry> &es) {
        DPRINTF(BTB, "BTB entries:\n");
        for (const auto &entry : es) {
            printTickedBTBEntry(entry);
        }
    }

    void dumpMruList(const BTBHeap &list) {
        DPRINTF(BTB, "MRU list:\n");
        for (const auto &it: list) {
            printTickedBTBEntry(*it);
        }
    }



  private:
    /** Returns the index into the BTB, based on the branch's PC.
     *  The index is calculated as: (pc >> idxShiftAmt) & idxMask
     *  where idxShiftAmt is:
     *  - log2(blockSize) if aligned to blockSize
     *  - 1 if not aligned to blockSize
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the BTB.
     */
    inline Addr getIndex(Addr instPC) {
        return (instPC >> idxShiftAmt) & idxMask;
    }

    /** Returns the tag bits of a given address.
     *  The tag is calculated as: (pc >> tagShiftAmt) & tagMask
     *  where tagShiftAmt = idxShiftAmt + log2(numSets)
     *  @param inst_PC The branch's address.
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr instPC) {
        return (instPC >> tagShiftAmt) & tagMask;
    }

    /** Helper function to check if this is L0 BTB
     *  L0 BTB has zero delay (getDelay() == 0)
     */
    bool isL0() { return getDelay() == 0; }

    /** Update the 2-bit saturating counter for conditional branches
     *  Counter range: [-2, 1]
     *  - Increment on taken (max 1)
     *  - Decrement on not taken (min -2)
     */
    void updateCtr(int &ctr, bool taken) {
        if (taken && ctr < 1) {ctr++;}
        if (!taken && ctr > -2) {ctr--;}
    }

    typedef struct BTBMeta {
        std::vector<BTBEntry> hit_entries;  // hit entries in L1 BTB
        std::vector<BTBEntry> l0_hit_entries; // hit entries in L0 BTB
        BTBMeta() {
            std::vector<BTBEntry> es;
            hit_entries = es;
            l0_hit_entries = es;
        }
    }BTBMeta;

    std::shared_ptr<BTBMeta> meta; // metadata for BTB, set in putPCHistory, used in update

    /** Process BTB entries for prediction
     *  @param entries Vector of BTB entries to process
     *  @param startAddr Start address of the fetch block
     *  @return Vector of processed entries in program order
     */
    std::vector<TickedBTBEntry> processEntries(const std::vector<TickedBTBEntry>& entries, 
                                              Addr startAddr);

    /** Fill predictions for pipeline stages
     *  @param entries Processed BTB entries
     *  @param stagePreds Vector of predictions for each stage
     */
    void fillStagePredictions(const std::vector<TickedBTBEntry>& entries,
                             std::vector<FullBTBPrediction>& stagePreds);

    /** Update prediction metadata
     *  @param entries Processed BTB entries
     */
    void updatePredictionMeta(const std::vector<TickedBTBEntry>& entries,
                               std::vector<FullBTBPrediction>& stagePreds);

    /** Process prediction metadata and old entries
     *  @param stream Fetch stream containing prediction info
     *  @return Processed old BTB entries
     */
    std::vector<BTBEntry> processOldEntries(const FetchStream &stream);

    /** Get the previous PC from the fetch stream
     *  @param stream Fetch stream containing prediction info
     *  @return Previous PC
     */
    Addr getPreviousPC(const FetchStream &stream);

    /** Check branch prediction hit status
     *  @param stream Fetch stream containing execution results
     *  @param meta BTB metadata from prediction
     */
    void checkPredictionHit(const FetchStream &stream,
                           const BTBMeta* meta);

    /** Collect entries that need to be updated
     *  @param old_entries Processed old entries
     *  @param stream Fetch stream with update info
     *  @return Vector of entries to update
     */
    std::vector<BTBEntry> collectEntriesToUpdate(
        const std::vector<BTBEntry>& old_entries,
        const FetchStream &stream);

    /** Update or replace BTB entry
     *  @param btb_idx Index of the BTB entry
     *  @param btb_tag Tag of the BTB entry
     *  @param entry Entry to update/replace
     *  @param stream Fetch stream with update info
     */
    void updateBTBEntry(Addr btb_idx, Addr btb_tag, const BTBEntry& entry, const FetchStream &stream);

    /*
     * Comparator for MRU heap
     * Returns true if a's timestamp is larger than b's
     * This creates a min-heap where the oldest entry is at the top
     */
    struct older
    {
        bool operator()(const BTBSetIter &a, const BTBSetIter &b) const
        {
            return a->tick > b->tick;
        }
    };

    /**
     * @brief check if the entries in the vector are in ascending order, means the pc is in ascending order
     * 
     * @param es 
     */
    void checkAscending(std::vector<BTBEntry> &es) {
        Addr last = 0;
        bool misorder = false;
        for (auto &entry : es) {
            if (entry.pc <= last) {
                misorder = true;
                break;
            }
            last = entry.pc;
        }
        if (misorder) {
            panic("BTB entries are not in ascending order");
        }
    }

    /** Looks up an address for all possible entries in the BTB. Address are aligned in this function
     *  @param inst_PC The address of the block to look up.
     *  @return Returns all hit BTB entries.
     */
    std::vector<TickedBTBEntry> lookup(Addr block_pc);

    /** Helper function to lookup entries in a single block
     * @param block_pc The aligned PC to lookup
     * @return Vector of matching BTB entries
     */
    std::vector<TickedBTBEntry> lookupSingleBlock(Addr block_pc);

    /** The BTB structure:
     *  - Organized as numSets sets
     *  - Each set has numWays ways
     *  - Total size = numSets * numWays = numEntries
     */
    std::vector<BTBSet> btb;

    /** MRU tracking:
     *  - One heap per set
     *  - Each heap tracks the MRU order of entries in that set
     *  - Oldest entry is at the top of heap
     */
    std::vector<BTBHeap> mruList;

    std::queue<std::tuple<Addr, Addr, BTBSet>> aheadReadBtbEntries;

    /** BTB configuration parameters */
    unsigned numEntries;    // Total number of entries
    unsigned numWays;       // Number of ways per set
    unsigned numSets;       // Number of sets (numEntries/numWays)
    // Whether the entries are half-aligned, in other words, whether it is mBTB. Note that the global
    // variable halfAligned in stream_common.hh is used to define the alignment of the fallthrough address
    // of the a fetch block, and it is orthogonal to whether the entries are stored in 32B aligned blocks
    bool entryHalfAligned;



    /** Address calculation masks and shifts */
    Addr idxMask;          // Mask for extracting index bits
    unsigned tagBits;      // Number of tag bits
    Addr tagMask;          // Mask for extracting tag bits
    unsigned idxShiftAmt;  // Amount to shift PC for index
    unsigned tagShiftAmt;  // Amount to shift PC for tag

    /** Thread handling */
    unsigned log2NumThreads;  // Log2 of number of threads for hashing

    /** Branch counter */
    unsigned numBr;  // Number of branches seen

    enum Mode {
        READ, WRITE, EVICT
    };

    struct BTBStats : public statistics::Group {
        statistics::Scalar newEntry;
        statistics::Scalar newEntryWithCond;
        statistics::Scalar newEntryWithUncond;
        statistics::Scalar oldEntry;
        statistics::Scalar oldEntryIndirectTargetModified;
        statistics::Scalar oldEntryWithNewCond;
        statistics::Scalar oldEntryWithNewUncond;

        statistics::Scalar predMiss;
        statistics::Scalar predHit;
        statistics::Scalar updateMiss;
        statistics::Scalar updateHit;
        statistics::Scalar updateExisting;
        statistics::Scalar updateReplace;
        statistics::Scalar updateReplaceValidOne;

        statistics::Scalar eraseSlotBehindUncond;

        statistics::Scalar predUseL0OnL1Miss;
        statistics::Scalar updateUseL0OnL1Miss;

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

        BTBStats(statistics::Group* parent);
    } btbStats;

    void incNonL0Stat(statistics::Scalar &stat) {
        if (!isL0()) {
            stat++;
        }
    }
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_BTB_HH__
