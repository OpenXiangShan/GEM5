/*
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#include "cpu/o3/store_set.hh"

#include <algorithm>
#include <limits>
#include <string>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/StoreSet.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/replacement_policies/tree_plru_rp.hh"
#include "mem/cache/tags/indexing_policies/set_associative.hh"
#include "params/SetAssociative.hh"
#include "params/TreePLRURP.hh"

namespace gem5
{

namespace o3
{

uint64_t StoreSet::ssitInstanceCounter = 0;

namespace
{

class StoreSetMDPSetAssociative : public SetAssociative
{
  private:
    // splitmix64 finalizer: small, fast, good avalanche for hashed indexing.
    static uint64_t
    mix64(uint64_t x)
    {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

  protected:
    uint32_t extractSet(const Addr addr) const override
    {
        return static_cast<uint32_t>(mix64(static_cast<uint64_t>(addr)) &
                                     setMask);
    }

    Addr extractTag(const Addr addr) const override { return addr; }

  public:
    StoreSetMDPSetAssociative(const SetAssociativeParams &p)
      : SetAssociative(p)
    {
    }
    ~StoreSetMDPSetAssociative() = default;
};

} // namespace

StoreSet::StoreSet() = default;

StoreSet::StoreSet(uint64_t clear_period, int _SSIT_size, int _LFST_size,
                   int _store_set_clear_thres, int _LFSTEntrySize)
{
    init(clear_period, _store_set_clear_thres, _SSIT_size, _LFST_size,
         _LFSTEntrySize);
}

StoreSet::~StoreSet()
{
}

void
StoreSet::regStats(statistics::Group *parent)
{
    if (!parent || stats) {
        return;
    }
    stats = std::make_unique<StoreSetStats>(parent);
}

StoreSet::StoreSetStats::StoreSetStats(statistics::Group *parent)
    : statistics::Group(parent, "storeSet"),
      ADD_STAT(ssitLookups, statistics::units::Count::get(),
               "Number of SSIT lookups."),
      ADD_STAT(ssitHits, statistics::units::Count::get(),
               "Number of SSIT lookup hits."),
      ADD_STAT(ssitMisses, statistics::units::Count::get(),
               "Number of SSIT lookup misses."),
      ADD_STAT(ssitInserts, statistics::units::Count::get(),
               "Number of SSIT insertions (including overwrites)."),
      ADD_STAT(ssitVictimizations, statistics::units::Count::get(),
               "Number of SSIT victim selections for insertions."),
      ADD_STAT(ssitInvalidations, statistics::units::Count::get(),
               "Number of SSIT entry invalidations (evict/clear)."),
      ADD_STAT(violationCreateNew, statistics::units::Count::get(),
               "Violations that create new store sets."),
      ADD_STAT(violationAttachStore, statistics::units::Count::get(),
               "Violations that attach a store to an existing store set."),
      ADD_STAT(violationAttachLoad, statistics::units::Count::get(),
               "Violations that attach a load to an existing store set."),
      ADD_STAT(violationSameSSIDStrict, statistics::units::Count::get(),
               "Violations within the same SSID that set strict for the load."),
      ADD_STAT(violationMerge, statistics::units::Count::get(),
               "Violations that merge two different SSIDs."),
      ADD_STAT(ssidAllocations, statistics::units::Count::get(),
               "Number of SSID allocations requested."),
      ADD_STAT(ssidAllocFromFreeList, statistics::units::Count::get(),
               "Number of SSID allocations served from the free list."),
      ADD_STAT(ssidAllocFromReclaim, statistics::units::Count::get(),
               "Number of SSID allocations served by reclaiming an in-use SSID "
               "(free list exhausted)."),
      ADD_STAT(ssidAllocExhaustRate, statistics::units::Ratio::get(),
               "Rate of SSID allocations that hit free list exhaustion.",
               ssidAllocFromReclaim / ssidAllocations),
      ADD_STAT(ssidReclaims, statistics::units::Count::get(),
               "Number of SSID reclaims performed (evict SSIT mappings)."),
      ADD_STAT(ssidMerges, statistics::units::Count::get(),
               "Number of SSID merges performed."),
      ADD_STAT(ssidReleases, statistics::units::Count::get(),
               "Number of SSIDs released back to the free list.")
{
}

void
StoreSet::SSITEntry::invalidate()
{
    if (owner && isValid()) {
        if (owner->stats) {
            owner->stats->ssitInvalidations++;
        }
        owner->ssitEntryInvalidated(ssid);
    }
    TaggedEntry::invalidate();
    ssid = 0;
    strict = false;
}

StoreSet::SSITEntry*
StoreSet::findSSITEntry(Addr pc)
{
    if (stats) {
        stats->ssitLookups++;
    }
    if (!ssit) {
        if (stats) {
            stats->ssitMisses++;
        }
        return nullptr;
    }

    Addr key = ssitKey(pc);
    SSITEntry* entry = ssit->findEntry(key, false);
    if (entry) {
        if (stats) {
            stats->ssitHits++;
        }
        ssit->accessEntry(entry);
    } else {
        if (stats) {
            stats->ssitMisses++;
        }
    }
    return entry;
}

void
StoreSet::ssitEntryInvalidated(SSID ssid)
{
    assert(ssid < (SSID)LFSTSize);
    if (!ssidInUse[ssid]) {
        return;
    }
    assert(ssidRefCount[ssid] > 0);
    ssidRefCount[ssid]--;
    maybeReleaseSSID(ssid);
}

void
StoreSet::touchSSID(SSID ssid)
{
    if (ssid < ssidLastUse.size() && ssidInUse[ssid]) {
        ssidLastUse[ssid] = ++ssidUseCounter;
    }
}

void
StoreSet::clearLFSTEntry(SSID ssid)
{
    assert(ssid < (SSID)LFSTSize);
    VictimEntryID[ssid] = 0;
    for (int j = 0; j < LFSTEntrySize; ++j) {
        validLFSTLarge[ssid][j] = false;
        LFSTLarge[ssid][j] = 0;
        LFSTLargePC[ssid][j] = 0;
    }
}

void
StoreSet::maybeReleaseSSID(SSID ssid)
{
    if (!ssidInUse[ssid] || ssidRefCount[ssid] != 0) {
        return;
    }

    ssidInUse[ssid] = false;
    ssidLastUse[ssid] = 0;
    ssidFreeList.push_back(ssid);
    clearLFSTEntry(ssid);
    if (stats) {
        stats->ssidReleases++;
    }
}

void
StoreSet::reclaimSSID(SSID ssid)
{
    if (stats) {
        stats->ssidReclaims++;
    }
    if (ssit) {
        for (auto& entry : *ssit) {
            if (entry.isValid() && entry.ssid == ssid) {
                // inorder not to release the ssid, set owner to nullptr
                entry.owner = nullptr;
                ssit->invalidate(&entry);
                entry.owner = this;
            }
        }
    }
    ssidRefCount[ssid] = 0;
    clearLFSTEntry(ssid);
}

StoreSet::SSID
StoreSet::allocSSID()
{
    if (stats) {
        stats->ssidAllocations++;
    }
    if (!ssidFreeList.empty()) {
        SSID ssid = ssidFreeList.back();
        ssidFreeList.pop_back();
        ssidInUse[ssid] = true;
        ssidRefCount[ssid] = 0;
        touchSSID(ssid);
        clearLFSTEntry(ssid);
        if (stats) {
            stats->ssidAllocFromFreeList++;
        }
        return ssid;
    }

    // SSID pool exhausted: reclaim the least-recently used SSID.
    if (stats) {
        stats->ssidAllocFromReclaim++;
    }
    SSID victim = 0;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    bool found = false;
    for (SSID ssid = 0; ssid < (SSID)LFSTSize; ++ssid) {
        if (!ssidInUse[ssid]) {
            continue;
        }
        found = true;
        if (ssidLastUse[ssid] < oldest) {
            oldest = ssidLastUse[ssid];
            victim = ssid;
        }
    }

    if (!found) {
        fatal("StoreSet: SSID allocator is inconsistent (free list empty "
              "and no SSID marked in-use)!\n");
    }

    DPRINTF(StoreSet,
            "StoreSet: SSID pool exhausted, reclaiming SSID=%u (lastUse=%lu)\n",
            victim, oldest);

    reclaimSSID(victim);
    ssidInUse[victim] = true;
    ssidRefCount[victim] = 0;
    touchSSID(victim);
    return victim;
}

void
StoreSet::mergeSSIDs(SSID winner, SSID loser)
{
    if (winner == loser) {
        return;
    }
    if (stats) {
        stats->ssidMerges++;
    }

    // Merge the SSIT mappings.
    if (ssit) {
        for (auto& entry : *ssit) {
            if (!entry.isValid() || entry.ssid != loser) {
                continue;
            }
            assert(ssidRefCount[loser] > 0);
            ssidRefCount[loser]--;
            ssidRefCount[winner]++;
            entry.ssid = winner;
        }
    }

    // Merge LFST state: try to keep as many producers as possible.
    for (int j = 0; j < LFSTEntrySize; ++j) {
        if (!validLFSTLarge[loser][j]) {
            continue;
        }

        InstSeqNum seq = LFSTLarge[loser][j];
        bool already_present = false;
        for (int k = 0; k < LFSTEntrySize; ++k) {
            if (validLFSTLarge[winner][k] && LFSTLarge[winner][k] == seq) {
                already_present = true;
                break;
            }
        }
        if (already_present) {
            continue;
        }

        int victim = findVictimInLFSTEntry(winner);
        LFSTLarge[winner][victim] = seq;
        LFSTLargePC[winner][victim] = LFSTLargePC[loser][j];
        validLFSTLarge[winner][victim] = true;
    }

    maybeReleaseSSID(loser);
    touchSSID(winner);
}

void
StoreSet::updateSSITEntry(Addr pc, SSID ssid, bool strict)
{
    assert(ssid < (SSID)LFSTSize);
    Addr key = ssitKey(pc);

    SSITEntry* entry = ssit->findEntry(key, false);
    if (entry) {
        ssit->accessEntry(entry);

        if (entry->ssid != ssid) {
            ssitEntryInvalidated(entry->ssid);
            ssidRefCount[ssid]++;
            entry->ssid = ssid;
        }
        if (strict) {
            entry->strict = true;
        }
    } else {
        if (stats) {
            stats->ssitVictimizations++;
        }
        entry = ssit->findVictim(key);
        entry->ssid = ssid;
        entry->strict = strict;
        ssit->insertEntry(key, false, entry);
        ssidRefCount[ssid]++;
        if (stats) {
            stats->ssitInserts++;
        }
    }

    touchSSID(ssid);
}

void
StoreSet::init(uint64_t clear_period, int clear_period_thres, int _SSIT_size,
               int _LFST_size, int _LFST_entry_size)
{
    clearPeriod = clear_period;
    clearPeriodThreshold = clear_period_thres;

    // This class is initialized twice in the current O3 pipeline: once in the
    // MemDepUnit constructor and again in MemDepUnit::init(). We must avoid
    // recreating SimObject-based helper policies (and leaving dangling pointers
    // in the global SimObject list).
    if (ssit) {
        fatal_if(_SSIT_size != SSITSize,
                 "StoreSet: re-init with a different SSIT size is unsupported "
                 "(old=%d new=%d)\n",
                 SSITSize, _SSIT_size);
        fatal_if(_LFST_size != LFSTSize,
                 "StoreSet: re-init with a different LFST size is unsupported "
                 "(old=%d new=%d)\n",
                 LFSTSize, _LFST_size);
        fatal_if(_LFST_entry_size != LFSTEntrySize,
                 "StoreSet: re-init with a different LFST entry size is "
                 "unsupported (old=%d new=%d)\n",
                 LFSTEntrySize, _LFST_entry_size);

        clear();
        memOpsPred = 0;
        lastClearPeriodCycle = 0;
        return;
    }

    SSITSize = _SSIT_size;
    LFSTSize = _LFST_size;
    LFSTEntrySize = _LFST_entry_size;

    DPRINTF(StoreSet, "StoreSet: Creating store set object.\n");
    DPRINTF(StoreSet, "StoreSet: SSIT size: %i, LFST size: %i.\n",
            SSITSize, LFSTSize);

    if (SSITSize % SSITAssoc != 0) {
        fatal("Invalid SSIT size (must be multiple of %d)!\n", SSITAssoc);
    }
    if (!isPowerOf2(SSITSize / SSITAssoc)) {
        fatal("Invalid SSIT size (numSets must be power of 2)!\n");
    }

    if (!isPowerOf2(LFSTSize)) {
        fatal("Invalid LFST size!\n");
    }

    ssidInUse.assign(LFSTSize, false);
    ssidRefCount.assign(LFSTSize, 0);
    ssidLastUse.assign(LFSTSize, 0);
    ssidFreeList.clear();
    ssidFreeList.reserve(LFSTSize);
    for (SSID ssid = 0; ssid < (SSID)LFSTSize; ++ssid) {
        ssidFreeList.push_back(ssid);
    }
    ssidUseCounter = 0;

    LFSTLarge.resize(LFSTSize);
    LFSTLargePC.resize(LFSTSize);
    validLFSTLarge.resize(LFSTSize);
    VictimEntryID.resize(LFSTSize);

    for (int i = 0; i < LFSTSize; ++i) {
        LFSTLarge[i].resize(LFSTEntrySize);
        LFSTLargePC[i].resize(LFSTEntrySize);
        validLFSTLarge[i].resize(LFSTEntrySize);
        VictimEntryID[i] = 0;
        for (int j = 0; j < LFSTEntrySize; ++j) {
            validLFSTLarge[i][j] = false;
            LFSTLarge[i][j] = 0;
            LFSTLargePC[i][j] = 0;
        }
    }

    const uint64_t ssit_id = ++ssitInstanceCounter;

    ssitIndexingParams = std::make_unique<SetAssociativeParams>();
    ssitIndexingParams->name =
        "store_set_ssit_indexing_" + std::to_string(ssit_id);
    ssitIndexingParams->eventq_index = 0;
    ssitIndexingParams->assoc = SSITAssoc;
    ssitIndexingParams->entry_size = 1;
    ssitIndexingParams->size = SSITSize;
    ssitIndexingParams->num_slices = 1;
    ssitIndexingParams->slice_idx = 0;

    ssitIndexingPolicy =
        std::make_unique<StoreSetMDPSetAssociative>(*ssitIndexingParams);

    ssitReplacementParams = std::make_unique<TreePLRURPParams>();
    ssitReplacementParams->name =
        "store_set_ssit_repl_" + std::to_string(ssit_id);
    ssitReplacementParams->eventq_index = 0;
    ssitReplacementParams->num_leaves = SSITAssoc;

    ssitReplacementPolicy = std::make_unique<replacement_policy::TreePLRU>(
        *ssitReplacementParams);

    ssit = std::make_unique<SSITTable>(
        SSITAssoc, SSITSize, ssitIndexingPolicy.get(),
        ssitReplacementPolicy.get(), SSITEntry(this));

    memOpsPred = 0;
    lastClearPeriodCycle = 0;
}

void
StoreSet::violation(Addr store_PC, Addr load_PC)
{
    auto* load_entry = findSSITEntry(load_PC);
    auto* store_entry = findSSITEntry(store_PC);

    const bool valid_load_SSID = load_entry != nullptr;
    const bool valid_store_SSID = store_entry != nullptr;

    if (!valid_load_SSID && !valid_store_SSID) {
        if (stats) {
            stats->violationCreateNew++;
        }
        SSID new_set_load = allocSSID();
        SSID new_set_store = allocSSID();
        updateSSITEntry(load_PC, new_set_load, false);
        updateSSITEntry(store_PC, new_set_store, false);

        DPRINTF(StoreSet,
                "StoreSet: New store set SSID_ld=%i SSID_st=%i for load %#x, store %#x\n",
                new_set_load, new_set_store, load_PC, store_PC);
    } else if (valid_load_SSID && !valid_store_SSID) {
        if (stats) {
            stats->violationAttachStore++;
        }
        SSID new_set_store = allocSSID();
        updateSSITEntry(store_PC, new_set_store, false);
        DPRINTF(StoreSet,
                "StoreSet: Adding store %#x to existing SSID=%i (load %#x)\n",
                store_PC, new_set_store, load_PC);
    } else if (!valid_load_SSID && valid_store_SSID) {
        if (stats) {
            stats->violationAttachLoad++;
        }
        SSID new_set_load = allocSSID();
        updateSSITEntry(load_PC, new_set_load, false);
        DPRINTF(StoreSet,
                "StoreSet: Adding load %#x to existing SSID=%i (store %#x)\n",
                load_PC, new_set_load, store_PC);
    } else {
        SSID load_SSID = load_entry->ssid;
        SSID store_SSID = store_entry->ssid;

        assert(load_SSID < LFSTSize && store_SSID < LFSTSize);

        if (load_SSID == store_SSID) {
            if (stats) {
                stats->violationSameSSIDStrict++;
            }
            updateSSITEntry(load_PC, load_SSID, true);
            DPRINTF(StoreSet,
                    "StoreSet: Same SSID=%i, setting load %#x strict\n",
                    load_SSID, load_PC);
            return;
        }

        if (stats) {
            stats->violationMerge++;
        }
        SSID winner = std::min(load_SSID, store_SSID);
        SSID loser = (winner == load_SSID) ? store_SSID : load_SSID;

        DPRINTF(StoreSet,
                "StoreSet: Merging SSIDs winner=%i loser=%i for load %#x, "
                "store %#x\n",
                winner, loser, load_PC, store_PC);

        mergeSSIDs(winner, loser);
    }
}

void
StoreSet::checkClear(Cycles curCycle)
{
    uint64_t delta_cycle = (uint64_t)curCycle - lastClearPeriodCycle;
    memOpsPred++;
    if (delta_cycle > clearPeriodThreshold) {
        memOpsPred = 0;
        clear();
        lastClearPeriodCycle = (uint64_t)curCycle;
    }
}

void
StoreSet::insertLoad(Addr load_PC, InstSeqNum load_seq_num, Cycles curCycle)
{
    checkClear(curCycle);
    return;
}

void
StoreSet::insertStore(Addr store_PC, InstSeqNum store_seq_num, ThreadID tid,
                      Cycles curCycle)
{
    checkClear(curCycle);
    auto* entry = findSSITEntry(store_PC);
    if (!entry) {
        // Do nothing if there's no valid entry.
        return;
    }

    SSID store_SSID = entry->ssid;
    assert(store_SSID < LFSTSize);
    touchSSID(store_SSID);

    int victim_inst = findVictimInLFSTEntry(store_SSID);
    LFSTLarge[store_SSID][victim_inst] = store_seq_num;
    LFSTLargePC[store_SSID][victim_inst] = store_PC;
    validLFSTLarge[store_SSID][victim_inst] = true;

    DPRINTF(StoreSet, "Store %#x sn:%lu updated the LFST[SSID=%i][%i]\n",
            store_PC, store_seq_num, store_SSID, victim_inst);
    dump();
}

bool
StoreSet::checkInstStrict(Addr pc)
{
    auto* entry = findSSITEntry(pc);
    if (!entry) {
        return false;
    }
    return entry->strict;
}

std::vector<InstSeqNum>
StoreSet::checkInst(Addr PC)
{
    std::vector<InstSeqNum> vec = {};
    auto* entry = findSSITEntry(PC);
    if (!entry) {
        DPRINTF(StoreSet, "Inst %#x had no SSID\n", PC);
        return vec;
    }

    SSID inst_SSID = entry->ssid;
    assert(inst_SSID < LFSTSize);
    touchSSID(inst_SSID);

    for (int j = 0; j < LFSTEntrySize; ++j) {
        if (validLFSTLarge[inst_SSID][j]) {
            vec.push_back(LFSTLarge[inst_SSID][j]);
        }
    }
    DPRINTF(StoreSet, "Inst %#x with ssid=%i, had %lu valid producer\n",
            PC, inst_SSID, vec.size());
    return vec;
}

void
StoreSet::issued(Addr issued_PC, InstSeqNum issued_seq_num, bool is_store)
{
    // This only is updated upon a store being issued.
    if (!is_store) {
        return;
    }

    auto* entry = findSSITEntry(issued_PC);
    if (!entry) {
        return;
    }

    SSID store_SSID = entry->ssid;
    assert(store_SSID < LFSTSize);
    touchSSID(store_SSID);

    for (int j = 0; j < LFSTEntrySize; ++j) {
        if (validLFSTLarge[store_SSID][j] &&
            LFSTLarge[store_SSID][j] == issued_seq_num) {
            validLFSTLarge[store_SSID][j] = false;
            LFSTLarge[store_SSID][j] = 0;
            LFSTLargePC[store_SSID][j] = 0;
        }
    }
}

void
StoreSet::squash(InstSeqNum squashed_num, ThreadID tid)
{
    for (int i = 0; i < LFSTSize; ++i) {
        for (int j = 0; j < LFSTEntrySize; ++j) {
            if (validLFSTLarge[i][j] && LFSTLarge[i][j] > squashed_num) {
                LFSTLarge[i][j] = 0;
                LFSTLargePC[i][j] = 0;
                validLFSTLarge[i][j] = false;
            } else if (!validLFSTLarge[i][j]) {
                LFSTLarge[i][j] = 0;
                LFSTLargePC[i][j] = 0;
            }
        }
    }
}

void
StoreSet::clear()
{
    if (ssit) {
        for (auto& entry : *ssit) {
            entry.owner = nullptr;
        }
        for (auto& entry : *ssit) {
            ssit->invalidate(&entry);
        }
        for (auto& entry : *ssit) {
            entry.owner = this;
        }
    }

    ssidInUse.assign(LFSTSize, false);
    ssidRefCount.assign(LFSTSize, 0);
    ssidLastUse.assign(LFSTSize, 0);
    ssidFreeList.clear();
    ssidFreeList.reserve(LFSTSize);
    for (SSID ssid = 0; ssid < (SSID)LFSTSize; ++ssid) {
        ssidFreeList.push_back(ssid);
    }
    ssidUseCounter = 0;

    for (int i = 0; i < LFSTSize; ++i) {
        clearLFSTEntry(i);
    }
}

void
StoreSet::dump()
{
}

int
StoreSet::findVictimInLFSTEntry(int store_SSID)
{
    for (int j = 0; j < LFSTEntrySize; ++j) {
        if (!validLFSTLarge[store_SSID][j]) {
            return j;
        }
    }
    VictimEntryID[store_SSID]++;
    if (VictimEntryID[store_SSID] >= LFSTEntrySize) {
        VictimEntryID[store_SSID] %= LFSTEntrySize;
    }
    return VictimEntryID[store_SSID];
}

} // namespace o3
} // namespace gem5
