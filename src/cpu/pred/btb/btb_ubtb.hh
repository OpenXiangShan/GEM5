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
 * Micro Branch Target Buffer (uBTB) Implementation
 *
 * The uBTB is a cache-like structure that provides fast branch prediction:
 * - Fully associative organization
 * - MRU (Most Recently Used) replacement policy
 *
 * Key Features:
 * - Fast lookup using tags from branch addresses
 * - Each entry contains:
 *   - Branch type (conditional, unconditional, indirect, call, return)
 *   - Branch target address
 *   - 2-bit saturation counters for replacement policy
 *   - Timestamp for MRU tracking
 */

#ifndef __CPU_PRED_BTB_UBTB_HH__
#define __CPU_PRED_BTB_UBTB_HH__

#include <queue>

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/pred/btb/btb.hh"
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

    /** Creates a uBTB with the given number of entries, number of bits per
     *  tag, and instruction offset amount.
     *  @param numEntries Number of entries for the uBTB.
     *  @param tagBits Number of bits for each tag in the uBTB.
     */
    UBTB(const Params& p);

    /*
     * Micro-BTB Entry with timestamp for MRU replacement
     *
     * This structure extends BTBEntry to implement a uBTB entry with:
     * - valid: validity bit for this entry
     * - uctr: 2-bit saturation counter used in replacement policy
     * - tag: tag bits from branch address [23:1]
     * - tick: timestamp used for MRU (Most Recently Used) replacement policy
     * - numNTConds: number of not-taken conditional branches before the taken branch
     * - valid_2nd: existence of the second branch (for 2-taken support)
     * - branch_info_2nd: branch attributes for the second branch (for 2-taken support)
     */
    typedef struct TickedUBTBEntry : public BTBEntry
    {
        unsigned uctr; //2-bit saturation counter used in replacement policy
        uint64_t tick;  // timestamp for MRU replacement
        int  numNTConds; // number of conditional branches before the taken branch
        bool valid_2nd; // existence of the second branch
        BranchInfo branch_info_2nd; // branch attributes for the second branch

        TickedUBTBEntry() : BTBEntry(), uctr(0), tick(0), numNTConds(0), valid_2nd(false), branch_info_2nd() {}
        TickedUBTBEntry(const BTBEntry &be, uint64_t tick) : BTBEntry(be), uctr(0),
                        tick(tick), numNTConds(0), valid_2nd(false), branch_info_2nd() {}
    }TickedUBTBEntry;

    using UBTBIter = typename std::vector<TickedUBTBEntry>::iterator;
    using UBTBHeap = std::vector<UBTBIter>; // for MRU tracking

    void tickStart() override{};
    void tick() override{};

    /*
     * Entry point for uBTB Prediction, called at S1
     * @param startAddr: start address of the fetch block
     * @param history: branch history register (not used)
     * @param stagePreds: predictions for each pipeline stage
     *
     * This function:
     * 1. Looks up BTB entries for the fetch block
     * 2. Updates prediction statistics
     * 3. Fills predictions for each pipeline stage
     */
    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                      std::vector<FullBTBPrediction> &stagePreds) override;

    /** New unified prediction function for 2-taken support.
     * Performs uBTB lookup and fills both primary and secondary predictions if available.
     * @param startAddr The FB start address to look up
     * @param history Branch history register (not used)
     * @param stagePreds Predictions for each pipeline stage (filled with primary prediction)
     * @param secondPrediction Reference to store secondary prediction if available
     * @return Pair containing (hit_index, has_second_prediction)
     */
    std::pair<int, bool> getTwoTakenPrediction(Addr startAddr,
                                              const boost::dynamic_bitset<> &history,
                                              std::vector<FullBTBPrediction> &stagePreds,
                                              FullBTBPrediction &secondPrediction);

    /** Updates the uBTB predictions based on S3 prediction results.
     * This function is called from decoupled_bpred during S3 prediction
     * specifically, it reconciles differences between S1 (uBTB) and S3 predictions,
     * adjusting the uBTB's confidence in its predictions and updating entries
     * when necessary to improve future predictions.
     *
     * @param s3Pred The S3 prediction containing branch information and target
     */
    void updateUsingS3Pred(FullBTBPrediction &s3Pred);

    /**
     * Updates the uBTB using S3 prediction with 2-taken support (training/learning phase)
     *
     * @param dff_pred The first FB (from DFF buffer, represents previous
     * S3 pred), factually const but not declared as const
     * @param s3_pred The second FB (current S3 prediction)
     * @param hit_index The hit index from getTwoTakenPrediction (-1 if miss)
     */
    void updateUsingS3Pred(FullBTBPrediction &dff_pred,
                          FullBTBPrediction &s3_pred,
                          int hit_index);

    /** for statistics only
     * @param stream The fetch stream containing execution results and prediction metadata
     */
    void update(const FetchStream &stream) override;

    /** for statistics only
     * @param stream The fetch stream containing execution results
     * @param inst The dynamic instruction being committed
     */
    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    /** Get prediction BTBMeta
     *  @return Returns the prediction meta
     */
    std::shared_ptr<void> getPredictionMeta() override
    {
        return meta;
    }

    /** Retrieve stored MBTB meta for second prediction
     *  @return Returns the stored MBTB meta or nullptr if none available
     */
    std::shared_ptr<void> getMBTBSecondPredictionMeta() const {
        return mbtbSecondPredMeta;
    }

    // the following methods are not used
    void specUpdateHist(const boost::dynamic_bitset<> &history, FullBTBPrediction &pred) override {}
    void recoverHist(const boost::dynamic_bitset<> &history,
        const FetchStream &entry, int shamt, bool cond_taken) override;
    void reset();
    void setTrace() override;
    TraceManager *ubtbTrace;

    // for debugging purpose
    void printTickedUBTBEntry(const TickedUBTBEntry &e) {
        DPRINTF(UBTB, "uBTB entry: valid %d, pc:%#lx, tag: %#lx, size:%d, target:%#lx, \
            cond:%d, indirect:%d, call:%d, return:%d, tick:%lu, valid_2nd:%d",
            e.valid, e.pc, e.tag, e.size, e.target, e.isCond, e.isIndirect, e.isCall, e.isReturn, e.tick, e.valid_2nd);
        if (e.valid_2nd) {
            DPRINTF(UBTB, ", 2nd_pc:%#lx, 2nd_target:%#lx, 2nd_cond:%d, 2nd_indirect:%d, 2nd_call:%d, 2nd_return:%d",
                e.branch_info_2nd.pc, e.branch_info_2nd.target, e.branch_info_2nd.isCond,
                e.branch_info_2nd.isIndirect, e.branch_info_2nd.isCall, e.branch_info_2nd.isReturn);
        }
        DPRINTF(UBTB, "\n");
    }

    void dumpMruList() {
        DPRINTF(UBTB, "MRU list:\n");
        for (const auto &it: mruList) {
            printTickedUBTBEntry(*it);
        }
    }

  private:

    /** this struct holds the lastest prediction made by uBTB,
     * it's set in putPCHistory, and used in updateUsingS3Pred
     */
    struct LastPred
    {
        int hit_index; // -1 for miss, array index for hit

        LastPred() : hit_index(-1) {
            // Default constructor - will be assigned proper value later
        }
    };
    LastPred lastPred;

    /** this struct holds the metadata for uBTB,
     * note that unlike other predictors, the ubtb meta serves only statistical purpose
     * and has no functional significance,
     * it's set in putPCHistory, and passed to a fetch stream, to be later used in update.
     */
    struct UBTBMeta
    {
        TickedUBTBEntry hit_entry;
        UBTBMeta() {
            hit_entry = TickedUBTBEntry();
        }
    };
    std::shared_ptr<UBTBMeta> meta;

    // Storage for MBTB meta created during getTwoTakenPrediction
    std::shared_ptr<DefaultBTB::BTBMeta> mbtbSecondPredMeta{nullptr};

    // helper methods
    /*
     * Comparator for MRU heap
     * Returns true if a's timestamp is larger than b's
     * This creates a min-heap where the oldest entry is at the top
     */
    struct older
    {
        bool operator()(const UBTBIter &a, const UBTBIter &b) const
        {
            return a->tick > b->tick;
        }
    };

    /** Returns the tag bits of a given address.
     *  The tag is calculated as: (pc >> 1) & tagMask
     *  @param startPC The start address of the fetch block
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr startPC) {
        return (startPC >> 1) & tagMask;
    }

    void updateUCtr(unsigned &ctr, bool inc) {
        if (inc && ctr < 3) {ctr++;}
        if (!inc && ctr > 0) {ctr--;}
    }

    /** helper method called by putPCHistory: Searches for a entry in the uBTB.
     * @param startAddr The FB start address to look up
     * @return Index of the matching entry if found, or -1 if not found
     */
    int lookup(Addr startAddr);

    /** helper method called by putPCHistory: Check uBTB entry pc range and update statistics
     * @param entry The uBTB entry to check
     * @param startAddr The start address of the fetch block
     */
    void PredStatistics(const TickedUBTBEntry entry, Addr startAddr);

    /** helper method called by putPCHistory: Fill predictions for each pipeline stage based on uBTB entries
     *  @param entry The BTB entry containing branch info
     *  @param stagePreds Predictions for each pipeline stage
     */
    void fillStagePredictions(const TickedUBTBEntry& entry,
                              std::vector<FullBTBPrediction>& stagePreds);

    /** helper method for 2-taken: Construct a FullBTBPrediction from BranchInfo
     *  @param branchInfo The branch information for the second prediction
     *  @param bbStart The basic block start address for the prediction
     *  @param prediction The prediction object to fill
     */
    void fillSecondPrediction(const BranchInfo& branchInfo, Addr bbStart, FullBTBPrediction& prediction);

    /** helper method for 2-taken: Check if two predictions can form a valid 2-taken sequence
     *  @param dff The first prediction (from DFF buffer)
     *  @param s3Pred The second prediction (current S3 prediction)
     *  @return true if the predictions can form a valid 2-taken sequence
     */
    bool check2TakenConditions(FullBTBPrediction& dff, const FullBTBPrediction& s3Pred);

    /** Common helper function for training logic - handles entry update based on hit/miss scenarios
     *  @param entry_index Index of the entry that was hit during prediction (-1 for miss)
     *  @param pred The S3 prediction to train with
     *  @param secondPred Second prediction for 2-taken training (can be nullptr for 1-taken)
     */
    void updateEntryAtIndex(int entry_index, FullBTBPrediction& pred, FullBTBPrediction* secondPred);

    /** helper method called in updateUsingS3Pred: This function replaces an existing uBTB entry with new prediction
     *
     * @param entryIndex Index of the entry to replace
     * @param newPrediction The new prediction to store
     */
    void replaceOldEntry(int entryIndex, FullBTBPrediction & newPrediction);

    /** helper method for 2-taken: Add second prediction to an existing uBTB entry
     *
     * @param entryIndex Index of the entry to update
     * @param secondPred The second prediction to add (must not be nullptr)
     */
    void addSecondPredictionToEntry(int entryIndex, FullBTBPrediction* secondPred);

    /** Helper to create MBTB meta for second prediction
     *  @param branch_info_2nd The branch information for the second prediction
     */
    void createMBTBMetaForSecondPrediction(const BranchInfo& branch_info_2nd);

    /** Helper function to calculate numNTConds (number of not-taken conditional branches)
     *  @param prediction The prediction containing history information
     *  @return Number of conditional branches before the taken branch
     */
    int calculateNumNTConds(FullBTBPrediction& prediction);

    /** The uBTB structure:
     *  - Implemented as a fully associative table
     *  - Each entry can store one branch
     *  - Total size = numEntries
     */
    std::vector<TickedUBTBEntry> ubtb;


    /** MRU tracking:
     *  - Uses a heap to track entry timestamps
     *  - Oldest entry is at top of heap for fast replacement
     */
    UBTBHeap mruList;

    /** uBTB configuration parameters */
    unsigned numEntries;    // Total number of entries

    /** Address calculation masks and shifts */
    unsigned tagBits;      // Number of tag bits
    Addr tagMask;          // Mask for extracting tag bits


    struct UBTBStats : public statistics::Group
    {

        statistics::Scalar predMiss;
        statistics::Scalar predHit;
        statistics::Scalar updateMiss;

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

        // 2-taken condition check statistics
        statistics::Scalar twoTakenConditionChecks;      ///< Total number of 2-taken condition checks
        statistics::Scalar twoTakenConditionPassed;      ///< Number of times all conditions passed
        statistics::Scalar twoTakenFailEmptyPreds;       ///< Rejected due to empty predictions
        statistics::Scalar twoTakenFailFirstNotTaken;    ///< Rejected due to first branch not taken
        statistics::Scalar twoTakenFailFirstIndirect;    ///< Rejected due to first branch being indirect
        statistics::Scalar twoTakenFailSecondIndirect;   ///< Rejected due to second branch being indirect
        statistics::Scalar twoTakenFailSecondCond;       ///< Rejected due to second branch being conditional
        statistics::Scalar twoTakenAcceptAlwaysTaken;   ///< Accepted alwaysTaken conditional branch as 2nd prediction
        statistics::Scalar twoTakenFailRetRet;           ///< Rejected due to ret->ret sequence
        statistics::Scalar twoTakenFailCallCall;         ///< Rejected due to call->call sequence

        UBTBStats(statistics::Group* parent);
    } ubtbStats;


};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_UBTB_HH__
