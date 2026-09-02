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
 * Main BTB (MBTB) Implementation
 * 
 * The MBTB is a half-aligned BTB that covers 64-byte blocks:
 * - Queries two consecutive 32-byte aligned blocks
 * - Fixed half-aligned behavior (no entryHalfAligned parameter)
 * - 8-way set associative organization  
 * - MRU (Most Recently Used) replacement policy
 * - Support for multiple branch types
 */

#ifndef __CPU_PRED_BTB_MBTB_HH__
#define __CPU_PRED_BTB_MBTB_HH__

#include <queue>
#include <vector>

#include "base/types.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/test_stats.hh"
#include "cpu/pred/btb/timed_base_pred.hh"

// Conditional includes based on build mode
#ifdef UNIT_TEST
    #include <gmock/gmock.h>
    #include <gtest/gtest.h>
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "arch/generic/pcstate.hh"
    #include "base/logging.hh"
    #include "config/the_isa.hh"
    #include "debug/BTB.hh"
    #include "debug/BTBStats.hh"
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

class MBTB : public TimedBaseBTBPredictor
{
  private:

  public:

#ifdef UNIT_TEST
    // Test constructor  
    MBTB(unsigned numEntries, unsigned tagBits, unsigned numWays, unsigned numDelay);
#else
    // Production constructor
    typedef MBTBParams Params;

    MBTB(const Params& p);
#endif

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

#ifdef UNIT_TEST
    unsigned tick{0};
    unsigned getComponentIdx() { return 0; }
    uint64_t curTick() { return tick++; }
#else
    // Production methods
    void tickStart() override;

    void tick() override;

    void commitBranch(const FetchTarget &stream, const DynInstPtr &inst) override;
    void setTrace() override;
    TraceManager *btbTrace;
#endif

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

    std::vector<BTBEntry> getPredictedEntriesNoSideEffect(
        Addr startAddr, ThreadID tid, uint8_t asidHash) const;

    /** Get prediction BTBMeta
     *  @return Returns the prediction meta
     */
    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override;
    void refreshPredictionMeta(Addr startAddr,
                               const boost::dynamic_bitset<> &history,
                               FullBTBPrediction &pred) override;

    /** Derive the old/new BTB entry selected for this update attempt. */
    void prepareUpdate(const FetchTarget &stream, PreparedUpdate &update);

    /** Updates the BTB with the branch info of a block and execution result.
     *  This function:
     *  1. Updates existing entries with new information
     *  2. Adds new entries if necessary
     *  3. Updates MRU information
     */
    void update(const FetchTarget &stream,
                const PreparedUpdate &update) override;

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
    inline Addr getIndex(Addr instPC, uint8_t asidHash,
                         ThreadID tid) const {
        Addr baseIndex = (instPC >> idxShiftAmt) & idxMask;
        Addr index = xorAsidHashIntoIndex(
            baseIndex, floorLog2(numSets), asidHash);
        return partitionIndex(index, numSets, tid);
    }

    /** Returns the tag bits of a given address.
     *  The tag is calculated as: (pc >> tagShiftAmt) & tagMask
     *  where tagShiftAmt = idxShiftAmt + log2(numSets)
     *  @param inst_PC The branch's address.
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr instPC, uint8_t asidHash) const {
        const unsigned shift = tagShiftAmt -
            (usesTidPartitionedStorage() ? 1 : 0);
        Addr baseTag = (instPC >> shift) & tagMask;
        return injectAsidHashIntoTag(baseTag, tagBits, asidHash);
    }

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
        std::vector<BTBEntry> hit_entries;
        BTBMeta() {
            std::vector<BTBEntry> es;
            hit_entries = es;
        }
    }BTBMeta;

    // Prediction metadata lives until the top level copies it into an FTQ
    // entry, so concurrent SMT lookups must not overwrite another thread.
    std::vector<std::shared_ptr<BTBMeta>> threadMeta;

    /** Process BTB entries for prediction
     *  @param entries Vector of BTB entries to process
     *  @param startAddr Start address of the fetch block
     *  @return Vector of processed entries in program order
     */
    std::vector<TickedBTBEntry> processEntries(const std::vector<TickedBTBEntry>& entries, 
                                              Addr startAddr);
    std::vector<TickedBTBEntry> processEntriesNoSideEffect(
        const std::vector<TickedBTBEntry>& entries, Addr startAddr) const;

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

    /** Check branch prediction hit status
     *  @param stream Fetch stream containing execution results
     *  @param meta BTB metadata from prediction
     */
    void checkPredictionHit(const FetchTarget &stream,
                            const BTBMeta* meta,
                            const PreparedUpdate &update);

    /** Update or replace BTB entry
     *  @param entry Entry to update/replace (PC used to select SRAM and calculate index/tag)
     *  @param stream Fetch stream with update info
     */
    void updateBTBEntry(const BranchUpdate &branch, const FetchTarget &stream);

    // Helper: build updated entry (ctr/alwaysTaken/indirect target/tag)
    BTBEntry buildUpdatedEntry(const BranchUpdate &branch,
                               const BTBEntry* existing_entry,
                               const FetchTarget &stream);

    // Helper: update an existing entry in SRAM set
    void updateExistingInSRAMSet(Addr btb_idx,
                                 BTBHeap &heap,
                                 BTBSetIter it_found,
                                 const TickedBTBEntry &ticked_entry);

    // Helper: replace the oldest entry in SRAM set
    void replaceOldestInSRAMSet(int sram_id,
                                Addr btb_idx,
                                BTBHeap &heap,
                                const TickedBTBEntry &ticked_entry);

    // Helper: commit/update an entry in victim cache at given index
    void commitToVictimCache(int vc_idx, const TickedBTBEntry &ticked_entry);

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
    std::vector<TickedBTBEntry> lookup(Addr block_pc, ThreadID tid,
                                       uint8_t asidHash,
                                       std::shared_ptr<BTBMeta> meta);
    std::vector<TickedBTBEntry> lookupNoSideEffect(
        Addr block_pc, ThreadID tid, uint8_t asidHash) const;

    /** Helper function to lookup entries in a single block
     * @param block_pc The aligned PC to lookup
     * @return Vector of matching BTB entries
     */
    std::vector<TickedBTBEntry> lookupSingleBlock(Addr block_pc, ThreadID tid,
                                                  uint8_t asidHash);

    /** Victim cache operations */
    std::vector<TickedBTBEntry> lookupVictimCache(Addr block_pc, uint8_t asidHash);
    std::vector<TickedBTBEntry> lookupSingleBlockNoSideEffect(
        Addr block_pc, ThreadID tid, uint8_t asidHash) const;
    std::vector<TickedBTBEntry> lookupVictimCacheNoSideEffect(
        Addr block_pc, uint8_t asidHash) const;
    void insertVictimCache(const TickedBTBEntry& evicted_entry);
    bool eraseFromVictimCacheByPC(Addr pc);

    /** Dual SRAM BTB structure:
     *  - Two independent 4-way SRAMs (sram0 and sram1)
     *  - Each SRAM organized as numSets sets with 4 ways
     *  - Total size = numSets * 4 * 2 = numEntries (same as before)
     *  - SRAM selection based on 32B-aligned PC[5]
     */
    std::vector<BTBSet> sram0, sram1;

    /** Independent MRU tracking for each SRAM:
     *  - mru0 for sram0, mru1 for sram1
     *  - Each maintains MRU order within its own 4-way sets
     *  - Oldest entry is at the top of each heap
     */
    std::vector<BTBHeap> mru0, mru1;

    /** Victim cache for evicted entries:
     *  - Small fully-associative cache for recently evicted entries
     *  - Reduces conflict misses from SRAM capacity limitations
     *  - Uses LRU replacement policy (based on tick)
     */
    std::vector<TickedBTBEntry> victimCache;
    unsigned victimCacheSize;

    /** BTB configuration parameters */
    unsigned numEntries;    // Total number of entries
    unsigned numWays;       // Number of ways per SRAM (4 for dual-SRAM)
    unsigned numSets;       // Number of sets per SRAM (numEntries/numWays/2)
    
    /** SRAM selection helper function */
    inline int getSRAMId(Addr pc) const {
        // Use the bit after block offset to select SRAM
        // For 32B blocks: bit 5 selects SRAM (blockSize=32, log2(32)=5)
        return ((pc >> floorLog2(blockSize)) & 1);
    }

#ifdef UNIT_TEST
    uint64_t blockSize{32};  // max size in byte of a Fetch Block
#endif

    /** Address calculation masks and shifts */
    Addr idxMask;          // Mask for extracting index bits
    unsigned tagBits;      // Number of tag bits
    Addr tagMask;          // Mask for extracting tag bits
    unsigned idxShiftAmt;  // Amount to shift PC for index
    unsigned tagShiftAmt;  // Amount to shift PC for tag

    /** Branch counter */
    unsigned numBr;  // Number of branches seen

    enum Mode {
        READ, WRITE, EVICT
    };

    using Scalar = test_stats::Scalar;
    using Distribution = test_stats::Distribution;

#ifdef UNIT_TEST
public:
    struct BTBStats {
#else
    struct BTBStats : public statistics::Group {
#endif
        Scalar newEntry;
        Scalar newEntryWithCond;
        Scalar newEntryWithUncond;

        Scalar predMiss;
        Scalar predHit;
        Scalar predHitNum;
        Scalar updateMiss;
        Scalar updateHit;
        Scalar updateExisting;
        Scalar updateReplace;
        Scalar updateReplaceValidOne;
        Scalar updateInVC;
        Scalar updateTotal;
        Scalar updateFixTarget;

        // per branch statistics
        Scalar allBranchHits;
        Scalar allBranchHitTakens;
        Scalar allBranchHitNotTakens;
        Scalar allBranchMisses;
        Scalar allBranchMissTakens;
        Scalar allBranchMissNotTakens;

        Scalar condHits;
        Scalar condHitTakens;
        Scalar condHitNotTakens;
        Scalar condMisses;
        Scalar condMissTakens;
        Scalar condMissNotTakens;
        Scalar condPredCorrect;
        Scalar condPredWrong;

        Scalar uncondHits;
        Scalar uncondMisses;

        Scalar indirectHits;
        Scalar indirectMisses;
        Scalar indirectPredCorrect;
        Scalar indirectPredWrong;

        Scalar callHits;
        Scalar callMisses;

        Scalar returnHits;
        Scalar returnMisses;

        // Victim cache statistics
        Scalar victimCacheHit;

        Distribution predHitCount;
#ifndef UNIT_TEST
        BTBStats(statistics::Group* parent, int numWays);
#endif
        void init(int numWays);
    } btbStats;

};

// Close conditional namespace wrapper for testing
#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_MBTB_HH__
