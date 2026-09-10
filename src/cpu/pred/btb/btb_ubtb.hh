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
 * - Each entry stores a bounded block layout with branch types, targets and
 *   base direction counters. Overflow layouts cannot supply predictions.
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
         bool using_s3_pred = true, bool smt_tid_partitioned = false,
         unsigned num_slots = 4);
#else
    typedef UBTBParams Params;

    UBTB(const Params& p);
#endif

    /** A snapshot of the whole prediction window, including not-taken slots.
     * An empty usable layout is a known branchless block, distinct from miss.
     * Slots are sorted by PC and bounded by numSlots, even on overflow.
     */
    struct BlockEntry
    {
        bool valid{false};
        bool overflow{false};
        Addr startPC{0};
        Addr tag{0};
        std::vector<BTBEntry> slots;

        bool usable() const { return valid && !overflow; }
        BTBEntry getTakenEntry() const;
    };

    struct TickedUBTBEntry : public BlockEntry
    {
        uint64_t tick{0};
    };

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

    /** Fill the complete BTB layout carried by S3, preserving base counters.
     * Final TAGE/SC directions and RAS/ITTAGE targets are not stored here.
     */
    void updateUsingS3Pred(FullBTBPrediction &s3Pred);

    /** Read a layout without overwriting the primary prediction metadata.
     * Only usable() layouts may be used as a second-block teacher.
     */
    BlockEntry lookupForChecker(Addr startAddr, ThreadID tid,
                               uint8_t asidHash);

    /** Attribute whether a produced PairTAGE second block agreed with the
     * checker prediction returned by lookupForChecker().
     */
    void recordCheckerResult(bool matches);

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
        DPRINTF(UBTB,
                "uBTB layout: valid %d, startPC %#lx, tag %#lx, "
                "slots %zu, overflow %d, tick %lu\n",
                e.valid, e.startPC, e.tag, e.slots.size(), e.overflow, e.tick);
    }

  private:

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

    /** Returns the tag bits of a given address.
     *  The tag is calculated as: (pc >> 1) & tagMask
     *  @param startPC The start address of the fetch block
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr startPC, uint8_t asidHash) const {
        Addr baseTag = (startPC >> 1) & tagMask;
        return injectAsidHashIntoTag(baseTag, tagBits, asidHash);
    }

    /** helper method called by putPCHistory: Searches for a entry in the uBTB.
     * @param startAddr The FB start address to look up
     * @return Iterator to the matching entry if found, or ubtb.end() if not found
     */
    enum class LookupPort
    {
        Prediction,
        Checker
    };

    UBTBIter lookup(Addr startAddr, ThreadID tid, uint8_t asidHash,
                    LookupPort port = LookupPort::Prediction);
    TickedUBTBEntry lookupNoSideEffect(Addr startAddr, ThreadID tid,
                                       uint8_t asidHash) const;

    /** helper method called by putPCHistory: Check uBTB entry pc range and update statistics
     * @param entry The uBTB entry to check
     * @param startAddr The start address of the fetch block
     */
    void PredStatistics(const TickedUBTBEntry &entry, Addr startAddr);

    /** helper method called by putPCHistory: Fill predictions for each pipeline stage based on uBTB entries
     *  @param entry The BTB entry containing branch info
     *  @param stagePreds Predictions for each pipeline stage
     */
    void fillStagePredictions(const TickedUBTBEntry& entry,
                              std::vector<FullBTBPrediction>& stagePreds);

    void fillLayout(Addr startAddr, ThreadID tid, uint8_t asidHash,
                    const std::vector<BTBEntry> &entries);

    /** The uBTB structure:
     *  - Stored flat as numSets consecutive groups of numWays entries
     *  - Each entry stores at most numSlots branches in one prediction window
     *  - Total size = numSets * numWays
     */
    std::vector<TickedUBTBEntry> ubtb;

    /** uBTB configuration parameters */
    unsigned numSets;       // Number of sets
    unsigned numWays;       // Number of ways per set
    unsigned numSlots;      // Maximum branches per block layout
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

        Scalar checkerLookups;
        Scalar checkerHits;
        Scalar checkerMisses;
        Scalar checkerFullMisses;
        Distribution checkerSetOccupancy;
        Scalar checkerHitAgreements;
        Scalar checkerHitDisagreements;
        Scalar predOverflowMisses;
        Scalar checkerOverflowMisses;
        Scalar layoutFills;
        Scalar layoutOverflowFills;
        Distribution layoutSlots;

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
        Vector s1OverrideByReason;
        Vector2d s1OverrideByReasonAndAbtbHit;
        Vector2d s1OverrideByReasonAndAfterSquash;

#ifdef UNIT_TEST
        UBTBStats() = default;
#else
        UBTBStats(statistics::Group* parent);
#endif
        void init(unsigned num_sets, unsigned accessible_ways,
                  unsigned num_slots);
    } ubtbStats;


};

#ifdef UNIT_TEST
} // namespace test
#endif

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_UBTB_HH__
