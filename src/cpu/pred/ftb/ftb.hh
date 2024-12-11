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

#ifndef __CPU_PRED_FTB_FTB_HH__
#define __CPU_PRED_FTB_FTB_HH__

#include "arch/generic/pcstate.hh"
#include "base/logging.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/pred/ftb/stream_struct.hh"
#include "cpu/pred/ftb/timed_base_pred.hh"
#include "debug/FTB.hh"
#include "debug/FTBStats.hh"
#include "params/DefaultFTB.hh"


namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

class DefaultFTB : public TimedBaseFTBPredictor
{
  private:

  public:

    typedef DefaultFTBParams Params;

    DefaultFTB(const Params& p);

    typedef struct TickedFTBEntry : public FTBEntry
    {
        uint64_t tick;
        TickedFTBEntry(const FTBEntry &entry, uint64_t tick)
            : FTBEntry(entry), tick(tick) {}
        TickedFTBEntry() : tick(0) {}
    }TickedFTBEntry;

    using FTBMap = std::map<Addr, TickedFTBEntry>;
    using FTBMapIter = typename FTBMap::iterator;
    using FTBHeap = std::vector<FTBMapIter>;


    struct older
    {
        bool operator()(const FTBMapIter &a, const FTBMapIter &b) const
        {
            return a->second.tick > b->second.tick;
        }
    };

    void tickStart() override;
    
    void tick() override;

    void putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history,
                      std::vector<FullFTBPrediction> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;

    void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) override;

    /** Creates a FTB with the given number of entries, number of bits per
     *  tag, and instruction offset amount.
     *  @param numEntries Number of entries for the FTB.
     *  @param tagBits Number of bits for each tag in the FTB.
     *  @param instShiftAmt Offset amount for instructions to ignore alignment.
     */
    DefaultFTB(unsigned numEntries, unsigned tagBits,
               unsigned instShiftAmt, unsigned numThreads);

    void reset();

    /** Looks up an address in the FTB. Must call valid() first on the address.
     *  @param inst_PC The address of the branch to look up.
     *  @return Returns the FTB entry.
     */
    TickedFTBEntry lookup(Addr instPC);

    /** Checks if an block starting with the given PC is in the FTB.
     *  @param inst_PC The address of the block to look up.
     *  @return Whether or not the entry of the block exists in the FTB.
     */
    bool valid(Addr instPC);

    /** Updates the FTB with the branch info of a block and execution result.
     */
    void update(const FetchStream &stream) override;

    void commitBranch(const FetchStream &stream, const DynInstPtr &inst) override;

    /**
     * @brief derive new ftb entry from old ones and set updateFTBEntry field in stream
     *        only in L1FTB will this function be called when update
     * 
     * @param stream 
     */
    void getAndSetNewFTBEntry(FetchStream &stream);

    bool entryHasUncond(FTBEntry e) {
        if (!e.slots.empty()) {
            return e.slots.back().uncondValid();
        } else {
            return false;
        }
    }

    bool entryHasCond(FTBEntry e) {
      return numCondInEntry(e) > 0;
    }

    int numCondInEntry(FTBEntry e) {
        int numCond = 0;
        if (!e.slots.empty()) {
            for (auto &slot : e.slots) {
                if (slot.condValid()) {
                    numCond++;
                }
            }
        }
        return numCond;
    }

    bool entryIsFull(FTBEntry e) {
        // int validSlots = 0;
        // if (!e.slots.empty()) {
        //     for (auto &slot : e.slots) {
        //         if (slot.valid) {
        //             validSlots++;
        //         }
        //     }
        // }
        // return validSlots == numBr;
        return e.slots.size() == numBr;
    }

    bool branchIsInEntry(FTBEntry e, Addr instPC) {
        if (!e.slots.empty()) {
            for (auto &slot : e.slots) {
                if (slot.pc == instPC) {
                    return true;
                }
            }
        }
        return false;
    }

    void printFTBEntry(FTBEntry &e, uint64_t tick = 0) {
        DPRINTF(FTB, "FTB entry: valid %d, tag %#lx, fallThruAddr:%#lx, tick:%lu, slots:\n",
            e.valid, e.tag, e.fallThruAddr, tick);
        for (auto &slot : e.slots) {
            DPRINTF(FTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d, always_taken:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn, slot.alwaysTaken);
        }
    }

    void printTickedFTBEntry(TickedFTBEntry &e) {
        printFTBEntry(e, e.tick);
    }

    void checkFTBEntry(FTBEntry &e) {
        bool uncond_encountered = false;
        for (auto &slot : e.slots) {
            assert(!uncond_encountered);
            if (slot.isUncond()) {
                uncond_encountered = true;
            }
        }
    }

  private:
    /** Returns the index into the FTB, based on the branch's PC.
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the FTB.
     */
    inline Addr getIndex(Addr instPC);

    /** Returns the tag bits of a given address.
     *  @param inst_PC The branch's address.
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr instPC);

    bool isL0() { return getDelay() == 0; }

    void updateCtr(int &ctr, bool taken) {
        if (taken && ctr < 1) {ctr++;}
        if (!taken && ctr > -2) {ctr--;}
    }

    /** The actual FTB. */

    std::vector<FTBMap> ftb;

    std::vector<FTBHeap> mruList;


    /** The number of entries in the FTB. */
    unsigned numEntries;

    /** The index mask. */
    Addr idxMask;

    /** The number of tag bits per entry. */
    unsigned tagBits;

    /** The tag mask. */
    Addr tagMask;

    /** Number of bits to shift PC when calculating index. */
    unsigned instShiftAmt;

    /** Number of bits to shift PC when calculating tag. */
    unsigned tagShiftAmt;

    /** Log2 NumThreads used for hashing threadid */
    unsigned log2NumThreads;

    unsigned predictWidth;

    unsigned numBr;

    unsigned numWays;

    unsigned numSets;

    typedef struct FTBMeta
    {
        bool hit;
        bool l0_hit;
        FTBEntry entry;
        FTBMeta() : hit(false), l0_hit(false), entry(FTBEntry()) {}
        FTBMeta(bool h, bool h0, FTBEntry e) : hit(h), l0_hit(h0), entry(e) {}
        FTBMeta(const FTBMeta &other) : hit(other.hit), l0_hit(other.l0_hit), entry(other.entry) {}
    }FTBMeta;

    FTBMeta meta;

    struct FTBStats : public statistics::Group {
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

        statistics::Scalar eraseSlotBehindUncond;

        statistics::Scalar predUseL0OnL1Miss;
        statistics::Scalar updateUseL0OnL1Miss;

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

        FTBStats(statistics::Group* parent);
    } ftbStats;

    void incNonL0Stat(statistics::Scalar &stat) {
        if (!isL0()) {
            stat++;
        }
    }
};

} // namespace ftb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_FTB_FTB_HH__
