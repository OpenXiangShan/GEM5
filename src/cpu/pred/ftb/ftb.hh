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
#include "params/DefaultFTB.hh"


namespace gem5
{

namespace branch_prediction
{

namespace ftb_pred
{

// TODO: multiway-associative FTB
class DefaultFTB : public TimedBaseFTBPredictor
{
  private:

  public:

    typedef DefaultFTBParams Params;

    DefaultFTB(const Params& p);

    struct TickedFTBEntry : public FTBEntry
    {
        uint64_t tick;
        TickedFTBEntry(const FTBEntry &entry, uint64_t tick)
            : FTBEntry(entry), tick(tick) {}
        TickedFTBEntry() : tick(0) {}
    };

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
                      std::array<FullFTBPrediction, 3> &stagePreds) override;

    std::shared_ptr<void> getPredictionMeta() override;

    void specUpdateHist(const boost::dynamic_bitset<> &history, FullFTBPrediction &pred) override;

    unsigned getDelay() override {return numDelay;}

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
    FTBEntry lookup(Addr instPC);

    /** Checks if an block starting with the given PC is in the FTB.
     *  @param inst_PC The address of the block to look up.
     *  @return Whether or not the entry of the block exists in the FTB.
     */
    bool valid(Addr instPC);

    // TODO: add decoded info and exe result of a block
    /** Updates the FTB with the branch info of a block and execution result.
     */
    void update(const FetchStream &stream) override;

    /**
     * @brief derive new ftb entry from old ones and set updateFTBEntry field in stream
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

    void printFTBEntry(FTBEntry e) {
        DPRINTF(FTB, "FTB entry: valid %d, tag %#lx, fallThruAddr:%#lx, slots:\n",
            e.valid, e.tag, e.fallThruAddr);
        for (auto &slot : e.slots) {
            DPRINTF(FTB, "    pc:%#lx, size:%d, target:%#lx, cond:%d, indirect:%d, call:%d, return:%d\n",
                slot.pc, slot.size, slot.target, slot.isCond, slot.isIndirect, slot.isCall, slot.isReturn);
        }
    }

  private:
    /** Returns the index into the FTB, based on the branch's PC.
     *  @param inst_PC The branch to look up.
     *  @return Returns the index into the FTB.
     */
    inline unsigned getIndex(Addr instPC);

    /** Returns the tag bits of a given address.
     *  @param inst_PC The branch's address.
     *  @return Returns the tag bits.
     */
    inline Addr getTag(Addr instPC);

    /** The actual FTB. */
    // TODO: make each set to be a map structure

    std::vector<FTBMap> ftb;

    std::vector<FTBHeap> mruList;


    /** The number of entries in the FTB. */
    unsigned numEntries;

    /** The index mask. */
    unsigned idxMask;

    /** The number of tag bits per entry. */
    unsigned tagBits;

    /** The tag mask. */
    unsigned tagMask;

    /** Number of bits to shift PC when calculating index. */
    unsigned instShiftAmt;

    /** Number of bits to shift PC when calculating tag. */
    unsigned tagShiftAmt;

    /** Log2 NumThreads used for hashing threadid */
    unsigned log2NumThreads;

    unsigned numBr;

    unsigned numWays;

    unsigned numSets;

    unsigned numDelay;

    typedef struct FTBMeta {
        bool hit;
        FTBMeta() : hit(false) {}
        FTBMeta(bool h) : hit(h) {}
        FTBMeta(const FTBMeta &other) : hit(other.hit) {}
    }FTBMeta;

    FTBMeta meta;
};

} // namespace ftb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_FTB_FTB_HH__
