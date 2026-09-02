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
 * - Configurable set-associative organization
 * - LRU replacement within each set
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

#include <memory>
#include <utility>
#include <vector>

#include "base/types.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/timed_base_pred.hh"

#ifdef UNIT_TEST
    #include "cpu/pred/btb/test_stats.hh"
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "arch/generic/pcstate.hh"
    #include "base/statistics.hh"
    #include "base/logging.hh"
    #include "config/the_isa.hh"
    #include "debug/UBTB.hh"
    #include "params/UBTB.hh"
#endif

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

#ifdef UNIT_TEST
namespace test
{
#endif

class UBTB : public TimedBaseBTBPredictor
{
  private:

  public:

#ifdef UNIT_TEST
    UBTB(unsigned num_sets, unsigned num_ways, unsigned tag_bits,
         bool using_s3_pred = true, bool smt_tid_partitioned = false);
#else
    typedef UBTBParams Params;

    UBTB(const Params& p);
#endif

    /*
     * Micro-BTB Entry with timestamp for MRU replacement
     *
     * This structure extends BTBEntry to implement a uBTB entry with:
     * - valid: validity bit for this entry
     * - uctr: 2-bit saturation counter used in replacement policy
     * - tag: tag bits from branch address [23:1]
     * - tick: timestamp used for LRU (Least Recently Used) replacement policy
     */
    typedef struct TickedUBTBEntry : public BTBEntry
    {
        unsigned uctr; //2-bit saturation counter used in replacement policy
        uint64_t tick;  // timestamp for MRU replacement
        TickedUBTBEntry() : BTBEntry(), uctr(0), tick(0) {}
        TickedUBTBEntry(const BTBEntry &be, uint64_t tick) : BTBEntry(be), uctr(0), tick(tick) {}
    }TickedUBTBEntry;

    using UBTBIter = typename std::vector<TickedUBTBEntry>::iterator;
    using ConstUBTBIter =
        typename std::vector<TickedUBTBEntry>::const_iterator;

#ifdef UNIT_TEST
    uint64_t testTick{0};
    uint64_t curTick() { return testTick++; }

    unsigned testSetIndex(Addr start_addr, uint8_t asid_hash = 0,
                          ThreadID tid = 0) const
    {
        return getSet(start_addr, asid_hash, tid);
    }

    unsigned testValidEntriesInSet(unsigned set, ThreadID tid = 0) const;
#endif

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

    /** Updates the uBTB predictions based on S3 prediction results.
     * This function is called from decoupled_bpred during S3 prediction
     * specifically, it reconciles differences between S1 (uBTB) and S3 predictions,
     * adjusting the uBTB's confidence in its predictions and updating entries
     * when necessary to improve future predictions.
     *
     * @param s3Pred The S3 prediction containing branch information and target
     */
    void updateUsingS3Pred(FullBTBPrediction &s3Pred);

    /** for statistics only
     * @param stream The fetch stream containing execution results and prediction metadata
     */
    void update(const FetchTarget &stream) override;

    /** for statistics only
     * @param stream The fetch stream containing execution results
     * @param inst The dynamic instruction being committed
     */
#ifndef UNIT_TEST
    void commitBranch(const FetchTarget &stream,
                      const DynInstPtr &inst) override;
#endif

    /** Records fine-grained attribution for S1 override events whose source is
     *  uBTB. The counters are updated at override time rather than commit time.
     */
    void recordS1OverrideDetail(OverrideReason reason,
                                bool abtbHit,
                                bool afterSquash);

    /** Get prediction BTBMeta
     *  @return Returns the prediction meta
     */
    std::shared_ptr<void> getPredictionMeta(ThreadID tid = 0) override
    {
        if (tid >= threadMeta.size()) {
            return nullptr;
        }
        return threadMeta[tid];
    }
    void refreshPredictionMeta(Addr startAddr,
                               const boost::dynamic_bitset<> &history,
                               FullBTBPrediction &pred) override;

    void reset();
#ifndef UNIT_TEST
    void setTrace() override;
    TraceManager *ubtbTrace;
#endif

    // for debuggin purpose
    void printTickedUBTBEntry(const TickedUBTBEntry &e) {
        DPRINTF(UBTB, "uBTB entry: valid %d, pc:%#lx, tag: %#lx, size:%d, target:%#lx, \
            cond:%d, indirect:%d, call:%d, return:%d, tick:%lu\n",
            e.valid, e.pc, e.tag, e.size, e.target, e.isCond, e.isIndirect, e.isCall, e.isReturn, e.tick);
    }

  private:

    /** this struct holds the lastest prediction made by uBTB,
     * it's set in putPCHistory, and used in updateUsingS3Pred
     */
    struct LastPred
    {
        bool valid{false};
        unsigned set{0};
        unsigned way{0};
    };
    std::vector<LastPred> lastPred;

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
    std::vector<std::shared_ptr<UBTBMeta>> threadMeta;

    // helper methods

    unsigned getSet(Addr startAddr, uint8_t asidHash, ThreadID tid) const;
    std::pair<UBTBIter, UBTBIter> setRange(unsigned set, ThreadID tid);
    std::pair<ConstUBTBIter, ConstUBTBIter>
    setRange(unsigned set, ThreadID tid) const;
    void rememberLastPred(ThreadID tid, unsigned set, UBTBIter entry);
    UBTBIter getLastPredEntry(ThreadID tid);

    /** Returns the tag bits of a given address.
     *  The tag is calculated as: (pc >> 1) & tagMask
     *  @param startPC The start address of the fetch block
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr startPC, uint8_t asidHash) const {
        Addr baseTag = (startPC >> 1) & tagMask;
        return injectAsidHashIntoTag(baseTag, tagBits, asidHash);
    }

    void updateUCtr(unsigned &ctr, bool inc) {
        if (inc && ctr < 3) {ctr++;}
        if (!inc && ctr > 0) {ctr--;}
    }

    /** helper method called by putPCHistory: Searches for a entry in the uBTB.
     * @param startAddr The FB start address to look up
     * @return Iterator to the matching entry if found, or ubtb.end() if not found
     */
    UBTBIter lookup(Addr startAddr, ThreadID tid, uint8_t asidHash);
    TickedUBTBEntry lookupNoSideEffect(Addr startAddr, ThreadID tid,
                                       uint8_t asidHash) const;

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

    /** helper method called in updateUsingS3Pred: This function replaces an existing uBTB entry with new prediction
     *
     * @param oldEntry Iterator to the entry to replace
     * @param newPrediction The new prediction to store
     */
    void replaceOldEntry(UBTBIter oldEntryIter, const BTBEntry &newTakenEntry,
                         Addr startAddr, uint8_t asidHash);

    //using the FB final taken branch to update uBTB
    void updateNewEntry(UBTBIter oldEntryIter, const BTBEntry &takenEntry,
                        const Addr startAddr, ThreadID tid,
                        uint8_t asidHash);

    /** The uBTB structure:
     *  - Stored flat as numSets consecutive groups of numWays entries
     *  - Each entry can store one branch
     *  - Total size = numSets * numWays
     */
    std::vector<TickedUBTBEntry> ubtb;

    /** uBTB configuration parameters */
    unsigned numSets;       // Number of sets
    unsigned numWays;       // Number of ways per set
    unsigned totalEntries;  // Derived total number of entries

    /** Address calculation masks and shifts */
    Addr idxMask;          // Mask for extracting set index bits
    unsigned idxShiftAmt;  // Remove the fetch-block alignment bits
    unsigned tagBits;      // Number of tag bits
    Addr tagMask;          // Mask for extracting tag bits
    bool usingS3Pred;    // using S3 prediction to update uBTB

#ifdef UNIT_TEST
    using Scalar = test_stats::Scalar;
    using Vector = test_stats::Vector;
    using Distribution = test_stats::Distribution;

    // The production Vector2d is only needed for override attribution. Keep
    // its unit-test replacement local to uBTB rather than extending shared
    // test statistics infrastructure.
    class Vector2d
    {
      private:
        std::vector<std::vector<uint64_t>> values;

      public:
        void init(std::size_t x_size, std::size_t y_size)
        {
            values.assign(x_size, std::vector<uint64_t>(y_size, 0));
        }

        std::vector<uint64_t> &operator[](std::size_t idx)
        {
            assert(idx < values.size());
            return values[idx];
        }
    };
#else
    using Scalar = statistics::Scalar;
    using Vector = statistics::Vector;
    using Vector2d = statistics::Vector2d;
    using Distribution = statistics::Distribution;
#endif


#ifdef UNIT_TEST
    struct UBTBStats
#else
    struct UBTBStats : public statistics::Group
#endif
    {
        Scalar predMiss;
        Scalar predHit;
        Scalar updateMiss;
        Scalar updateHit;
        Scalar s3UpdateHits;
        Scalar s3UpdateMisses;

        Vector setLookups;
        Vector setHits;
        Vector setAllocations;
        Vector setEvictions;
        Vector setFullMisses;
        Distribution setOccupancy;

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

        Scalar s1Hits3FallThrough;
        Scalar s1Misses3Taken;
        Scalar s1Hits3Taken;
        Scalar s1Misses3FallThrough;
        Scalar s1InvalidatedEntries;
        Vector s1OverrideByReason;
        Vector2d s1OverrideByReasonAndAbtbHit;
        Vector2d s1OverrideByReasonAndAfterSquash;

#ifdef UNIT_TEST
        UBTBStats() = default;
#else
        UBTBStats(statistics::Group* parent);
#endif
        void init(unsigned num_sets, unsigned accessible_ways);
    } ubtbStats;


};

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_UBTB_HH__
