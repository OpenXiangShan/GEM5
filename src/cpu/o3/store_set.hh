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

#ifndef __CPU_O3_STORE_SET_HH__
#define __CPU_O3_STORE_SET_HH__

#include <list>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "mem/cache/tags/tagged_entry.hh"

namespace gem5
{

class BaseIndexingPolicy;
template<class Entry>
class AssociativeSet;
struct SetAssociativeParams;
struct TreePLRURPParams;

namespace replacement_policy
{
class Base;
} // namespace replacement_policy

namespace o3
{

struct ltseqnum
{
    bool
    operator()(const InstSeqNum &lhs, const InstSeqNum &rhs) const
    {
        return lhs > rhs;
    }
};

/**
 * Implements a store set predictor for determining if memory
 * instructions are dependent upon each other.  See paper "Memory
 * Dependence Prediction using Store Sets" by Chrysos and Emer.  SSID
 * stands for Store Set ID, SSIT stands for Store Set ID Table, and
 * LFST is Last Fetched Store Table.
 */
class StoreSet
{
  public:
    typedef unsigned SSID;

  public:
    /** Default constructor.  init() must be called prior to use. */
    StoreSet();

    /** Creates store set predictor with given table sizes. */
    StoreSet(uint64_t clear_period, int SSIT_size, int LFST_size,int _store_set_clear_thres, int _LFSTEntrySize);

    /** Default destructor. */
    ~StoreSet();

    /** Initializes the store set predictor with the given table sizes. */
    void init(uint64_t clear_period, int clear_period_thres, int _SSIT_size, int _LFST_size, int _LFST_entry_size);

    /** Records a memory ordering violation between the younger load
     * and the older store. */
    void violation(Addr store_PC, Addr load_PC);

    /** Clears the store set predictor every so often so that all the
     * entries aren't used and stores are constantly predicted as
     * conflicting.
     */
    void checkClear(Cycles curCycle);

    /** Inserts a load into the store set predictor.  This does nothing but
     * is included in case other predictors require a similar function.
     */
    void insertLoad(Addr load_PC, InstSeqNum load_seq_num,Cycles curCycle);

    /** Inserts a store into the store set predictor.  Updates the
     * LFST if the store has a valid SSID. */
    void insertStore(Addr store_PC, InstSeqNum store_seq_num, ThreadID tid, Cycles curCycle);

    /** Checks if the instruction with the given PC is dependent upon
     * any store.  @return Returns the sequence number of the store
     * instruction this PC is dependent upon.  Returns 0 if none.
     */
    std::vector<InstSeqNum> checkInst(Addr PC);

    /** Records this PC/sequence number as issued. */
    void issued(Addr issued_PC, InstSeqNum issued_seq_num, bool is_store);

    /** Squashes for a specific thread until the given sequence number. */
    void squash(InstSeqNum squashed_num, ThreadID tid);

    /** Resets all tables. */
    void clear();

    /** Debug function to dump the contents of the store list. */
    void dump();
    bool checkInstStrict(Addr pc);
  private:

    uint64_t lastClearPeriodCycle=0;
    int findVictimInLFSTEntry(int store_SSID);

    static constexpr int SSITAssoc = 4;
    // RISC-V can have 16-bit compressed instructions, so PC is at least 2B
    // aligned. Only drop bit0 to avoid aliasing between PC and PC+2.
    static constexpr int SSITPcShift = 1;

    static uint64_t ssitInstanceCounter;

    struct SSITEntry : public TaggedEntry
    {
        StoreSet* owner = nullptr;
        SSID ssid = 0;
        bool strict = false;

        SSITEntry(StoreSet* owner = nullptr) : TaggedEntry(), owner(owner)
        {
        }

        void invalidate() override;
    };

    using SSITTable = AssociativeSet<SSITEntry>;

    Addr ssitKey(Addr pc) const { return pc >> SSITPcShift; }
    SSITEntry* findSSITEntry(Addr pc);
    void updateSSITEntry(Addr pc, SSID ssid, bool strict);
    void ssitEntryInvalidated(SSID ssid);

    std::unique_ptr<SetAssociativeParams> ssitIndexingParams;
    std::unique_ptr<TreePLRURPParams> ssitReplacementParams;
    std::unique_ptr<BaseIndexingPolicy> ssitIndexingPolicy;
    std::unique_ptr<replacement_policy::Base> ssitReplacementPolicy;
    std::unique_ptr<SSITTable> ssit;

    SSID allocSSID();
    void touchSSID(SSID ssid);
    void maybeReleaseSSID(SSID ssid);
    void reclaimSSID(SSID ssid);
    void clearLFSTEntry(SSID ssid);
    void mergeSSIDs(SSID winner, SSID loser);

    std::vector<bool> ssidInUse;
    std::vector<int> ssidRefCount;
    std::vector<uint64_t> ssidLastUse;
    std::vector<SSID> ssidFreeList;
    uint64_t ssidUseCounter = 0;

    /** Last Fetched Store Table. */
    std::vector<std::vector<InstSeqNum>> LFSTLarge,LFSTLargePC;
    std::vector<InstSeqNum> VictimEntryID;

    /** Bit vector to tell if the LFST has a valid entry. */
    std::vector<std::vector<bool>> validLFSTLarge;

    /** Map of stores that have been inserted into the store set, but
     * not yet issued or squashed.
     */
    // std::map<InstSeqNum, int, ltseqnum> storeList;

    typedef std::map<InstSeqNum, int, ltseqnum>::iterator SeqNumMapIt;

    /** Number of loads/stores to process before wiping predictor so all
     * entries don't get saturated
     */
    uint64_t clearPeriod;

    /** Store Set ID Table size, in entries. */
    int SSITSize;

    /** Last Fetched Store Table size, in entries. */
    int LFSTSize;

    int LFSTEntrySize;
    uint64_t clearPeriodThreshold;

    /** Number of memory operations predicted since last clear of predictor */
    int memOpsPred;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_STORE_SET_HH__
