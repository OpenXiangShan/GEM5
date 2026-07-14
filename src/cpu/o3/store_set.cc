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

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/MDPFeedback.hh"
#include "debug/StoreSet.hh"

namespace gem5
{

namespace o3
{

namespace
{

const char *
mdpFeedbackSourceName(StoreSet::MDPFeedbackSource source)
{
    switch (source) {
      case StoreSet::MDPFeedbackSource::NoForward:
        return "none";
      case StoreSet::MDPFeedbackSource::StoreQueue:
        return "sq";
      case StoreSet::MDPFeedbackSource::StoreBuffer:
        return "sbuffer";
    }
    return "unknown";
}

} // anonymous namespace

StoreSet::StoreSet(uint64_t clear_period, int _SSIT_size, int _LFST_size,
                   int _store_set_clear_thres, int _LFSTEntrySize,
                   bool enable_feedback_counter,
                   bool enable_sbuffer_forward_feedback,
                   unsigned depend_threshold, unsigned initial_counter,
                   unsigned ssit_tag_bits)
    : clearPeriod(clear_period), SSITSize(_SSIT_size), LFSTSize(_LFST_size),LFSTEntrySize(_LFSTEntrySize),
      clearPeriodThreshold(_store_set_clear_thres),
      enableFeedbackCounter(enable_feedback_counter),
      enableSBufferForwardFeedback(enable_sbuffer_forward_feedback),
      dependThreshold(std::min<unsigned>(depend_threshold, 3)),
      initialCounter(std::min<unsigned>(initial_counter, 3)),
      ssitTagBits(ssit_tag_bits)
{
    DPRINTF(StoreSet, "StoreSet: Creating store set object.\n");
    DPRINTF(StoreSet, "StoreSet: SSIT size: %i, LFST size: %i.\n",
            SSITSize, LFSTSize);

    if (!isPowerOf2(SSITSize)) {
        fatal("Invalid SSIT size!\n");
    }

    SSIT.resize(SSITSize);

    validSSIT.resize(SSITSize);
    SSITStrict.resize(SSITSize);
    SSITTag.resize(SSITSize);
    SSITCounter.resize(SSITSize);

    for (int i = 0; i < SSITSize; ++i) {
        validSSIT[i] = false;
        SSITStrict[i] = false;
        SSITTag[i] = 0;
        SSITCounter[i] = 0;
    }

    if (!isPowerOf2(LFSTSize)) {
        fatal("Invalid LFST size!\n");
    }

    //LFST.resize(LFSTSize);
    LFSTLarge.resize(LFSTSize);
    LFSTLargePC.resize(LFSTSize);
    validLFSTLarge.resize(LFSTSize);
    //validLFST.resize(LFSTSize);
    VictimEntryID.resize(LFSTSize);

    for (int i = 0; i < LFSTSize; ++i) {
        // validLFST[i] = false;
        // LFST[i] = 0;
        LFSTLarge[i].resize(LFSTEntrySize);
        LFSTLargePC[i].resize(LFSTEntrySize);
        validLFSTLarge[i].resize(LFSTEntrySize);
        VictimEntryID[i]=0;
        for (int j=0;j<LFSTEntrySize;++j) {
            validLFSTLarge[i][j] = 0;
            LFSTLarge[i][j] = 0;
            LFSTLargePC[i][j] = 0;
        }
    }

    indexMask = SSITSize - 1;

    offsetBits = 2;

    memOpsPred = 0;
}

StoreSet::~StoreSet()
{
}

void
StoreSet::init(uint64_t clear_period, int clear_period_thres,
               int _SSIT_size, int _LFST_size, int _LFST_entry_size,
               bool enable_feedback_counter,
               bool enable_sbuffer_forward_feedback,
               unsigned depend_threshold, unsigned initial_counter,
               unsigned ssit_tag_bits)
{
    SSITSize = _SSIT_size;
    LFSTSize = _LFST_size;
    clearPeriod = clear_period;
    clearPeriodThreshold = clear_period_thres;
    LFSTEntrySize = _LFST_entry_size;
    enableFeedbackCounter = enable_feedback_counter;
    enableSBufferForwardFeedback = enable_sbuffer_forward_feedback;
    dependThreshold = std::min<unsigned>(depend_threshold, 3);
    initialCounter = std::min<unsigned>(initial_counter, 3);
    ssitTagBits = ssit_tag_bits;

    DPRINTF(StoreSet, "StoreSet: Creating store set object.\n");
    DPRINTF(StoreSet, "StoreSet: SSIT size: %i, LFST size: %i.\n",
            SSITSize, LFSTSize);

    SSIT.resize(SSITSize);

    validSSIT.resize(SSITSize);

    SSITStrict.resize(SSITSize);
    SSITTag.resize(SSITSize);
    SSITCounter.resize(SSITSize);
    for (int i = 0; i < SSITSize; ++i) {
        validSSIT[i] = false;
        SSITStrict[i] = false;
        SSITTag[i] = 0;
        SSITCounter[i] = 0;
    }
    LFSTLarge.resize(LFSTSize);
    LFSTLargePC.resize(LFSTSize);
    validLFSTLarge.resize(LFSTSize);
    VictimEntryID.resize(LFSTSize);


    // LFST.resize(LFSTSize);

    // validLFST.resize(LFSTSize);


    for (int i = 0; i < LFSTSize; ++i) {
        // validLFST[i] = false;
        // LFST[i] = 0;
        LFSTLarge[i].resize(LFSTEntrySize);
        LFSTLargePC[i].resize(LFSTEntrySize);
        validLFSTLarge[i].resize(LFSTEntrySize);
        VictimEntryID[i]=0;
        for (int j=0;j<LFSTEntrySize;++j) {
            validLFSTLarge[i][j] = false;
            LFSTLarge[i][j] = 0;
            LFSTLargePC[i][j] = 0;
        }
    }

    indexMask = SSITSize - 1;

    offsetBits = 2;

    memOpsPred = 0;

    lastClearPeriodCycle = 0;
}

uint16_t
StoreSet::calcTag(Addr pc) const
{
    const unsigned bits = std::min<unsigned>(ssitTagBits, 16);
    if (bits == 0) {
        return 0;
    }

    Addr shifted = pc >> offsetBits;
    Addr mixed = shifted ^ (shifted >> (unsigned)std::log2(SSITSize)) ^
                 (shifted >> 17);
    const Addr mask = (Addr(1) << bits) - 1;
    return static_cast<uint16_t>(mixed & mask);
}

bool
StoreSet::ssitHit(Addr pc, int index) const
{
    assert(index < SSITSize);
    if (!validSSIT[index]) {
        return false;
    }
    return SSITTag[index] == calcTag(pc);
}

bool
StoreSet::predictsDependent(int index) const
{
    assert(index < SSITSize);
    return !enableFeedbackCounter || SSITCounter[index] >= dependThreshold;
}

uint8_t
StoreSet::saturatingInc(uint8_t counter) const
{
    return counter == 3 ? 3 : counter + 1;
}

uint8_t
StoreSet::saturatingDec(uint8_t counter) const
{
    return counter == 0 ? 0 : counter - 1;
}

void
StoreSet::setSSITEntry(Addr pc, SSID ssid, uint8_t initial_counter)
{
    int index = calcIndexSSIT(pc);
    assert(index < SSITSize);
    assert(ssid < LFSTSize);
    validSSIT[index] = true;
    SSIT[index] = ssid;
    SSITTag[index] = calcTag(pc);
    SSITCounter[index] = std::min<uint8_t>(initial_counter, 3);
}

void
StoreSet::invalidateSSITEntry(int index)
{
    assert(index < SSITSize);
    validSSIT[index] = false;
    SSIT[index] = 0;
    SSITStrict[index] = false;
    SSITTag[index] = 0;
    SSITCounter[index] = 0;
}


void
StoreSet::violation(Addr store_PC, Addr load_PC)
{
    int load_index = calcIndexSSIT(load_PC);
    int store_index = calcIndexSSIT(store_PC);

    assert(load_index < SSITSize && store_index < SSITSize);

    const bool load_hit = ssitHit(load_PC, load_index);
    const bool store_hit = ssitHit(store_PC, store_index);
    const uint8_t old_load_counter =
        load_hit ? SSITCounter[load_index] : 0;
    const uint8_t old_store_counter =
        store_hit ? SSITCounter[store_index] : 0;

    SSID load_ssid = load_hit ? SSIT[load_index] : calcSSID(load_PC);
    SSID store_ssid = store_hit ? SSIT[store_index] : calcSSID(store_PC);
    assert(load_ssid < LFSTSize && store_ssid < LFSTSize);

    const char *action = "allocate_both";
    SSID chosen_ssid = std::min(load_ssid, store_ssid);
    if (load_hit && store_hit) {
        action = load_ssid == store_ssid ? "same_ssid_inc" : "merge_ssid";
    } else if (load_hit) {
        chosen_ssid = load_ssid;
        action = "allocate_store";
    } else if (store_hit) {
        chosen_ssid = store_ssid;
        action = "allocate_load";
    }

    setSSITEntry(load_PC, chosen_ssid,
                 load_hit ? saturatingInc(old_load_counter) :
                            initialCounter);
    setSSITEntry(store_PC, chosen_ssid,
                 store_hit ? saturatingInc(old_store_counter) :
                             initialCounter);

    if (load_index == store_index && calcTag(load_PC) == calcTag(store_PC)) {
        SSITStrict[load_index] = true;
    }

    DPRINTF(StoreSet, "StoreSet violation load %#x store %#x -> ssid %u\n",
            load_PC, store_PC, chosen_ssid);
    DPRINTF(MDPFeedback,
            "MDP violation train store_pc=%#x load_pc=%#x store_index=%d "
            "load_index=%d store_tag_hit=%d load_tag_hit=%d "
            "old_store_ctr=%u old_load_ctr=%u new_store_ctr=%u "
            "new_load_ctr=%u store_ssid=%u load_ssid=%u action=%s\n",
            store_PC, load_PC, store_index, load_index, store_hit, load_hit,
            old_store_counter, old_load_counter, SSITCounter[store_index],
            SSITCounter[load_index], SSIT[store_index], SSIT[load_index],
            action);
}

void
StoreSet::checkClear(Cycles curCycle)
{
    uint64_t delta_cycle = (uint64_t)curCycle - lastClearPeriodCycle;
    memOpsPred++;
    // if (memOpsPred > clearPeriod) {
        // DPRINTF(StoreSet, "Wiping predictor state beacuse %d ld/st executed\n",
        //         clearPeriod);
    if (delta_cycle > clearPeriodThreshold) {
        memOpsPred = 0;
        clear();
        lastClearPeriodCycle = (uint64_t)curCycle;
    }
}

void
StoreSet::insertLoad(Addr load_PC, InstSeqNum load_seq_num,Cycles curCycle)
{
    checkClear(curCycle);
    // Does nothing.
    return;
}

void
StoreSet::insertStore(Addr store_PC, InstSeqNum store_seq_num, ThreadID tid, Cycles curCycle)
{
    int index = calcIndexSSIT(store_PC);

    int store_SSID;

    // checkClear();
    int victim_inst;
    checkClear(curCycle);
    assert(index < SSITSize);

    const bool tag_hit = ssitHit(store_PC, index);
    const bool strong = tag_hit && predictsDependent(index);
    const char *action = "insert_lfst";

    if (!validSSIT[index]) {
        action = "skip_no_entry";
        DPRINTF(MDPFeedback,
                "MDP insertStore store_pc=%#x sn=%llu index=%d valid=0 "
                "tag_hit=0 counter=0 threshold=%u action=%s ssid=0 "
                "lfst_slot=-1\n",
                store_PC, store_seq_num, index, dependThreshold, action);
        // Do nothing if there's no valid entry.
        return;
    } else if (!tag_hit) {
        action = "skip_tag_miss";
        DPRINTF(MDPFeedback,
                "MDP insertStore store_pc=%#x sn=%llu index=%d valid=1 "
                "tag_hit=0 counter=%u threshold=%u action=%s ssid=%u "
                "lfst_slot=-1\n",
                store_PC, store_seq_num, index, SSITCounter[index],
                dependThreshold, action, SSIT[index]);
        return;
    } else if (!strong) {
        action = "skip_counter_weak";
        DPRINTF(MDPFeedback,
                "MDP insertStore store_pc=%#x sn=%llu index=%d valid=1 "
                "tag_hit=1 counter=%u threshold=%u action=%s ssid=%u "
                "lfst_slot=-1\n",
                store_PC, store_seq_num, index, SSITCounter[index],
                dependThreshold, action, SSIT[index]);
        return;
    } else {
        store_SSID = SSIT[index];

        assert(store_SSID < LFSTSize);

        // Update the last store that was fetched with the current one.
        // LFST[store_SSID] = store_seq_num;
        const bool replacing =
            std::all_of(validLFSTLarge[store_SSID].begin(),
                        validLFSTLarge[store_SSID].end(),
                        [](bool valid) { return valid; });
        victim_inst = findVictimInLFSTEntry(store_SSID);
        action = replacing ? "replace_lfst_slot" : "insert_lfst";
        LFSTLarge[store_SSID][victim_inst] = store_seq_num;

        // validLFST[store_SSID] = 1;
        LFSTLargePC[store_SSID][victim_inst] = store_PC;

        // storeList[store_seq_num] = store_SSID;
        validLFSTLarge[store_SSID][victim_inst] = 1;

        DPRINTF(StoreSet, "Store %#x sn:%lu updated the LFST[SSID=%i][%i]\n",
                store_PC, store_seq_num, store_SSID, victim_inst);
        DPRINTF(MDPFeedback,
                "MDP insertStore store_pc=%#x sn=%llu index=%d valid=1 "
                "tag_hit=1 counter=%u threshold=%u action=%s ssid=%u "
                "lfst_slot=%d\n",
                store_PC, store_seq_num, index, SSITCounter[index],
                dependThreshold, action, store_SSID, victim_inst);
        dump();
    }
}

bool
StoreSet::checkInstStrict(Addr pc, PredictionInfo *pred_info)
{
    int index = calcIndexSSIT(pc);
    assert(index < SSITSize);

    const bool tag_hit = ssitHit(pc, index);
    const bool strong = tag_hit && predictsDependent(index);
    if (pred_info) {
        pred_info->valid = validSSIT[index];
        pred_info->tagHit = tag_hit;
        pred_info->counterStrong = strong;
        pred_info->ssitIndex = index;
        pred_info->tag = calcTag(pc);
        pred_info->storedTag = validSSIT[index] ? SSITTag[index] : 0;
        pred_info->ssid = tag_hit ? SSIT[index] : 0;
        pred_info->counter = validSSIT[index] ? SSITCounter[index] : 0;
    }

    if (!tag_hit || !strong) {
        DPRINTF(MDPFeedback,
                "MDP strict lookup load_pc=%#x index=%d tag_hit=%d "
                "counter=%u strict=0 result=0\n",
                pc, index, tag_hit,
                validSSIT[index] ? SSITCounter[index] : 0);
        return false;
    }
    DPRINTF(MDPFeedback,
            "MDP strict lookup load_pc=%#x index=%d tag_hit=1 "
            "counter=%u strict=%d result=%d\n",
            pc, index, SSITCounter[index], SSITStrict[index],
            SSITStrict[index]);
    return SSITStrict[index];
}

std::vector<InstSeqNum>
StoreSet::checkInst(Addr PC, PredictionInfo *pred_info)
{
    int index = calcIndexSSIT(PC);

    int inst_SSID;

    assert(index < SSITSize);

    std::vector<InstSeqNum> vec = {};
    const uint16_t tag = calcTag(PC);
    const bool tag_hit = ssitHit(PC, index);
    const bool strong = tag_hit && predictsDependent(index);

    if (pred_info) {
        pred_info->valid = validSSIT[index];
        pred_info->tagHit = tag_hit;
        pred_info->counterStrong = strong;
        pred_info->ssitIndex = index;
        pred_info->tag = tag;
        pred_info->storedTag = validSSIT[index] ? SSITTag[index] : 0;
        pred_info->ssid = tag_hit ? SSIT[index] : 0;
        pred_info->counter = validSSIT[index] ? SSITCounter[index] : 0;
    }

    const char *result = "predict_dependent";
    if (!validSSIT[index]) {
        result = "no_entry";
        DPRINTF(StoreSet, "Inst %#x with index %i had no SSID\n",
                PC, index);
        DPRINTF(MDPFeedback,
                "MDP lookup load_pc=%#x index=%d valid=0 tag_hit=0 "
                "stored_tag=0 calc_tag=%#x ssid=0 counter=0 threshold=%u "
                "result=%s producers=0\n",
                PC, index, tag, dependThreshold, result);

        // Return 0 if there's no valid entry.
        return vec;
    } else if (!tag_hit) {
        result = "tag_miss";
        DPRINTF(MDPFeedback,
                "MDP lookup load_pc=%#x index=%d valid=1 tag_hit=0 "
                "stored_tag=%#x calc_tag=%#x ssid=%u counter=%u "
                "threshold=%u result=%s producers=0\n",
                PC, index, SSITTag[index], tag, SSIT[index],
                SSITCounter[index], dependThreshold, result);
        return vec;
    } else if (!strong) {
        result = "counter_weak";
        DPRINTF(MDPFeedback,
                "MDP lookup load_pc=%#x index=%d valid=1 tag_hit=1 "
                "stored_tag=%#x calc_tag=%#x ssid=%u counter=%u "
                "threshold=%u result=%s producers=0\n",
                PC, index, SSITTag[index], tag, SSIT[index],
                SSITCounter[index], dependThreshold, result);
        return vec;
    } else {
        inst_SSID = SSIT[index];

        assert(inst_SSID < LFSTSize);

        // if (!validLFST[inst_SSID]) {

        //     DPRINTF(StoreSet, "Inst %#x with index %i and SSID %i had no "
        //             "dependency\n", PC, index, inst_SSID);

        //     return 0;
        // } else {
        //     DPRINTF(StoreSet, "Inst %#x with index %i and SSID %i had LFST "
        //             "inum of %i\n", PC, index, inst_SSID, LFST[inst_SSID]);

        //     return LFST[inst_SSID];
        // }
        for (int j = 0; j < LFSTEntrySize; ++j) {
            if (validLFSTLarge[inst_SSID][j]) {
                vec.push_back(LFSTLarge[inst_SSID][j]);
            }
        }
        if (vec.empty()) {
            result = "no_lfst_producer";
        }
        if (pred_info) {
            pred_info->producers = vec.size();
            pred_info->predictedDependent = !vec.empty();
        }
        DPRINTF(StoreSet, "Inst %#x with index=%i, ssid=%i, had %lu valid producer\n",
                PC, index, inst_SSID, vec.size());
        DPRINTF(MDPFeedback,
                "MDP lookup load_pc=%#x index=%d valid=1 tag_hit=1 "
                "stored_tag=%#x calc_tag=%#x ssid=%u counter=%u "
                "threshold=%u result=%s producers=%llu\n",
                PC, index, SSITTag[index], tag, inst_SSID,
                SSITCounter[index], dependThreshold, result,
                static_cast<unsigned long long>(vec.size()));
        return vec;
    }
}

void
StoreSet::issued(Addr issued_PC, InstSeqNum issued_seq_num, bool is_store)
{
    // This only is updated upon a store being issued.
    if (!is_store) {
        return;
    }

    int index = calcIndexSSIT(issued_PC);

    int store_SSID;

    assert(index < SSITSize);

    // SeqNumMapIt store_list_it = storeList.find(issued_seq_num);

    // if (store_list_it != storeList.end()) {
    //     storeList.erase(store_list_it);
    // }

    // Make sure the SSIT still has a valid entry for the issued store.
    if (!validSSIT[index]) {
        DPRINTF(MDPFeedback,
                "MDP issued store_pc=%#x sn=%llu index=%d tag_hit=0 "
                "ssid=0 cleared_slots=0\n",
                issued_PC, issued_seq_num, index);
        return;
    }
    const bool tag_hit = ssitHit(issued_PC, index);
    if (!tag_hit) {
        DPRINTF(MDPFeedback,
                "MDP issued store_pc=%#x sn=%llu index=%d tag_hit=0 "
                "ssid=%u cleared_slots=0\n",
                issued_PC, issued_seq_num, index, SSIT[index]);
        return;
    }

    store_SSID = SSIT[index];

    assert(store_SSID < LFSTSize);

    // If the last fetched store in the store set refers to the store that
    // was just issued, then invalidate the entry.
    // if (validLFST[store_SSID] && LFST[store_SSID] == issued_seq_num) {
    //     DPRINTF(StoreSet, "StoreSet: store invalidated itself in LFST.\n");
    //     validLFST[store_SSID] = false;
    // }

    unsigned cleared_slots = 0;
    for (int j=0;j<LFSTEntrySize;++j) {
        if (validLFSTLarge[store_SSID][j] && LFSTLarge[store_SSID][j] == issued_seq_num) {
            validLFSTLarge[store_SSID][j] = false;
            LFSTLarge[store_SSID][j] = 0;
            LFSTLargePC[store_SSID][j] = 0;
            cleared_slots++;
        }
    }
    DPRINTF(MDPFeedback,
            "MDP issued store_pc=%#x sn=%llu index=%d tag_hit=1 "
            "ssid=%u cleared_slots=%u\n",
            issued_PC, issued_seq_num, index, store_SSID, cleared_slots);
}

StoreSet::FeedbackResult
StoreSet::feedback(Addr load_pc, MDPFeedbackSource source, bool predicted)
{
    FeedbackResult result;
    int index = calcIndexSSIT(load_pc);
    result.ssitIndex = index;
    result.tag = calcTag(load_pc);
    result.valid = validSSIT[index];

    if (!predicted) {
        result.action = MDPFeedbackAction::SkipNotPredicted;
        DPRINTF(MDPFeedback,
                "MDP feedback load_pc=%#x index=%d valid=%d tag_hit=0 "
                "source=%s predicted=0 old_ctr=0 new_ctr=0 "
                "action=skip_not_predicted\n",
                load_pc, index, result.valid, mdpFeedbackSourceName(source));
        return result;
    }

    if (!validSSIT[index]) {
        result.action = MDPFeedbackAction::SkipNoEntry;
        DPRINTF(MDPFeedback,
                "MDP feedback load_pc=%#x index=%d valid=0 tag_hit=0 "
                "source=%s predicted=1 old_ctr=0 new_ctr=0 "
                "action=skip_no_entry\n",
                load_pc, index, mdpFeedbackSourceName(source));
        return result;
    }

    result.tagHit = ssitHit(load_pc, index);
    result.oldCounter = SSITCounter[index];
    result.newCounter = result.oldCounter;
    if (!result.tagHit) {
        result.action = MDPFeedbackAction::SkipTagMiss;
        DPRINTF(MDPFeedback,
                "MDP feedback load_pc=%#x index=%d valid=1 tag_hit=0 "
                "source=%s predicted=1 old_ctr=%u new_ctr=%u "
                "action=skip_tag_miss\n",
                load_pc, index, mdpFeedbackSourceName(source), result.oldCounter,
                result.newCounter);
        return result;
    }

    if (!enableFeedbackCounter) {
        result.action = MDPFeedbackAction::SatAt3;
        DPRINTF(MDPFeedback,
                "MDP feedback load_pc=%#x index=%d valid=1 tag_hit=1 "
                "source=%s predicted=1 old_ctr=%u new_ctr=%u "
                "action=counter_disabled\n",
                load_pc, index, mdpFeedbackSourceName(source),
                result.oldCounter, result.newCounter);
        return result;
    }

    const bool positive_feedback =
        source == MDPFeedbackSource::StoreQueue ||
        (enableSBufferForwardFeedback &&
         source == MDPFeedbackSource::StoreBuffer);
    if (positive_feedback) {
        result.newCounter = saturatingInc(result.oldCounter);
        SSITCounter[index] = result.newCounter;
        result.action = result.oldCounter == result.newCounter ?
            MDPFeedbackAction::SatAt3 : MDPFeedbackAction::Inc;
    } else {
        result.newCounter = saturatingDec(result.oldCounter);
        SSITCounter[index] = result.newCounter;
        if (result.newCounter == 0) {
            invalidateSSITEntry(index);
            result.action = MDPFeedbackAction::ClearCounterZero;
        } else {
            result.action = result.oldCounter == result.newCounter ?
                MDPFeedbackAction::SatAt0 : MDPFeedbackAction::Dec;
        }
    }

    const char *action = "inc";
    switch (result.action) {
      case MDPFeedbackAction::Inc: action = "inc"; break;
      case MDPFeedbackAction::Dec: action = "dec"; break;
      case MDPFeedbackAction::ClearCounterZero:
        action = "clear_counter_zero"; break;
      case MDPFeedbackAction::SatAt0: action = "sat_at_0"; break;
      case MDPFeedbackAction::SatAt3: action = "sat_at_3"; break;
      default: action = "skip_not_predicted"; break;
    }
    DPRINTF(MDPFeedback,
            "MDP feedback load_pc=%#x index=%d valid=1 tag_hit=1 "
            "source=%s predicted=1 old_ctr=%u new_ctr=%u action=%s\n",
            load_pc, index, mdpFeedbackSourceName(source), result.oldCounter,
            result.newCounter, action);
    return result;
}

void
StoreSet::squash(InstSeqNum squashed_num, ThreadID tid)
{
    for (int i=0;i<LFSTSize;++i) {
        for (int j=0; j<LFSTEntrySize; ++j) {
            if (validLFSTLarge[i][j] && LFSTLarge[i][j] > squashed_num) {
                LFSTLarge[i][j] =0;
                LFSTLargePC[i][j] = 0;
                validLFSTLarge[i][j] = false;
            }
            else if (!validLFSTLarge[i][j]) {
                LFSTLarge[i][j] = 0;
                LFSTLargePC[i][j] = 0;
            }
        }
    }
}

void
StoreSet::clear()
{
    for (int i = 0; i < SSITSize; ++i) {
        invalidateSSITEntry(i);
    }

    for (int i = 0; i < LFSTSize; ++i) {
        for (int j=0;j<LFSTEntrySize;++j) {
            validLFSTLarge[i][j] = false;
        }
    }

}

void
StoreSet::dump()
{
    // cprintf("storeList.size(): %i\n", storeList.size());
    // SeqNumMapIt store_list_it = storeList.begin();

    // int num = 0;

    // while (store_list_it != storeList.end()) {
    //     cprintf("%i: [sn:%lli] SSID:%i\n",
    //             num, (*store_list_it).first, (*store_list_it).second);
    //     num++;
    //     store_list_it++;
    // }
}

int
StoreSet::findVictimInLFSTEntry(int store_SSID)
{
    for (int j=0;j<LFSTEntrySize;++j) {
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

Addr
StoreSet::XORFold(Addr pc, uint64_t resetWidth)
{
    uint64_t pcWidth = sizeof(pc)*8;
    uint64_t fold_range = (pcWidth + resetWidth -1)/resetWidth;
    uint64_t xored = 0;
    uint64_t value_low;

    do {
        value_low = pc & ((1<<resetWidth)-1);
        xored ^= value_low;
        pc >>= resetWidth;
        fold_range--;
    }while (fold_range !=0);
    return xored;
}

} // namespace o3
} // namespace gem5
