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


#include "cpu/pred/btb/btb_ubtb.hh"

#include <algorithm>
#include <iterator>
#include <limits>
#include <string>

#include "base/intmath.hh"
#include "common.hh"

#ifdef UNIT_TEST
    #include "cpu/pred/btb/test/test_dprintf.hh"
#else
    #include "base/trace.hh"
    #include "cpu/o3/dyn_inst.hh"
    #include "debug/Fetch.hh"
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

namespace
{

constexpr int NumOverrideReasonBuckets = 3;
constexpr int NumBoolBuckets = 2;

constexpr const char *OverrideReasonLabels[NumOverrideReasonBuckets] = {
    "fall_thru",
    "control_addr",
    "target"
};

constexpr const char *AbtbHitLabels[NumBoolBuckets] = {
    "abtb_miss",
    "abtb_hit"
};

constexpr const char *AfterSquashLabels[NumBoolBuckets] = {
    "normal",
    "after_squash"
};

int
overrideReasonBucket(OverrideReason reason)
{
    switch (reason) {
      case OverrideReason::FALL_THRU:
        return 0;
      case OverrideReason::CONTROL_ADDR:
        return 1;
      case OverrideReason::TARGET:
        return 2;
      case OverrideReason::NO_OVERRIDE:
        break;
    }

    return -1;
}

int
boolBucket(bool value)
{
    return value ? 1 : 0;
}

} // namespace

#ifdef UNIT_TEST
UBTB::UBTB(unsigned num_sets, unsigned num_ways, unsigned tag_bits,
           bool using_s3_pred, bool smt_tid_partitioned, unsigned num_slots)
    : TimedBaseBTBPredictor(),
      threadMeta(),
      ubtb(),
      numSets(num_sets),
      numWays(num_ways),
      numSlots(num_slots),
      totalEntries(0),
      idxMask(0),
      idxShiftAmt(0),
      tagBits(tag_bits),
      tagMask(mask(tag_bits)),
      usingS3Pred(using_s3_pred),
      ubtbStats()
{
    setSmtTidPartitioned(smt_tid_partitioned);
#else
UBTB::UBTB(const Params &p)
    : TimedBaseBTBPredictor(p),
      threadMeta(),
      ubtb(),
      numSets(p.numSets),
      numWays(p.numWays),
      numSlots(p.numSlots),
      totalEntries(0),
      idxMask(0),
      idxShiftAmt(0),
      tagBits(p.tagBits),
      tagMask(mask(p.tagBits)),
      usingS3Pred(p.usingS3Pred),
      ubtbStats(this)
{
#endif
    if (numSets == 0 || !isPowerOf2(numSets)) {
        fatal("uBTB sets must be non-zero and a power of 2");
    }
    if (numSlots == 0) {
        fatal("uBTB branch slots must be non-zero");
    }
    if (numWays == 0) {
        fatal("uBTB ways must be non-zero");
    }
    if (usesTidPartitionedStorage() &&
        (numWays < 2 || numWays % 2 != 0)) {
        fatal("tid-partitioned uBTB requires an even number of ways");
    }

    const uint64_t capacity = static_cast<uint64_t>(numSets) * numWays;
    if (capacity > std::numeric_limits<unsigned>::max()) {
        fatal("uBTB total capacity is too large");
    }
    totalEntries = static_cast<unsigned>(capacity);
    idxMask = numSets - 1;
    if (!isPowerOf2(predictWidth) || predictWidth < 2) {
        fatal("uBTB prediction width must be a power of 2 and at least 2");
    }
    idxShiftAmt = floorLog2(predictWidth) - 1;

    // Entries belonging to a set are consecutive in the flat storage.
    ubtb.resize(totalEntries);
    for (auto &entry : ubtb) {
        entry.valid = false;
    }

    const unsigned accessibleWays = usesTidPartitionedStorage() ?
        numWays / 2 : numWays;
    ubtbStats.init(numSets, accessibleWays, numSlots);

#ifndef UNIT_TEST
    hasDB = true;
    dbName = "ubtb";
#endif

    threadMeta.resize(o3::MaxThreads);

    DPRINTF(UBTB, "uBTB: entries=%u sets=%u ways=%u indexShift=%u\n",
            totalEntries, numSets, numWays, idxShiftAmt);
}

#ifndef UNIT_TEST
void
UBTB::setTrace()
{
    if (enableDB) {
        std::vector<std::pair<std::string, DataType>> fields_vec = {
            std::make_pair("pc", UINT64),  std::make_pair("brType", UINT64), std::make_pair("target", UINT64),
            std::make_pair("idx", UINT64), std::make_pair("mode", UINT64),   std::make_pair("hit", UINT64)};
        ubtbTrace = _db->addAndGetTrace("uBTBTrace", fields_vec);
        ubtbTrace->init_table();
    }
}
#endif

unsigned
UBTB::getSet(Addr startAddr, uint8_t asidHash, ThreadID tid) const
{
    (void)tid;
    const Addr blockNumber = startAddr >> idxShiftAmt;
    const unsigned indexBits = floorLog2(numSets);
    Addr folded = blockNumber;
    if (indexBits != 0) {
        folded ^= blockNumber >> indexBits;
    }
    return xorAsidHashIntoIndex(
        folded & idxMask, indexBits, asidHash);
}

std::pair<UBTB::UBTBIter, UBTB::UBTBIter>
UBTB::setRange(unsigned set, ThreadID tid)
{
    assert(set < numSets);
    unsigned firstWay = 0;
    unsigned ways = numWays;
    if (usesTidPartitionedStorage()) {
        assert(tid < 2);
        ways /= 2;
        firstWay = tid * ways;
    }
    auto begin = ubtb.begin() + set * numWays + firstWay;
    return {begin, begin + ways};
}

std::pair<UBTB::ConstUBTBIter, UBTB::ConstUBTBIter>
UBTB::setRange(unsigned set, ThreadID tid) const
{
    assert(set < numSets);
    unsigned firstWay = 0;
    unsigned ways = numWays;
    if (usesTidPartitionedStorage()) {
        assert(tid < 2);
        ways /= 2;
        firstWay = tid * ways;
    }
    auto begin = ubtb.begin() + set * numWays + firstWay;
    return {begin, begin + ways};
}

BTBEntry
UBTB::BlockEntry::getTakenEntry() const
{
    if (usable()) {
        for (const auto &slot : slots) {
            if (slot.isUncond() || slot.alwaysTaken || slot.ctr >= 0) {
                return slot;
            }
        }
    }
    return BTBEntry();
}

#ifdef UNIT_TEST
unsigned
UBTB::testValidEntriesInSet(unsigned set, ThreadID tid) const
{
    auto [begin, end] = setRange(set, tid);
    return std::count_if(begin, end,
                         [](const auto &entry) { return entry.valid; });
}
#endif

void
UBTB::PredStatistics(const TickedUBTBEntry &entry, Addr startAddr)
{
    if (entry.usable()) {
        assert(entry.startPC == startAddr);
        DPRINTF(UBTB, "UBTB: lookup hit: \n");
        ubtbStats.predHit += 1;
        printTickedUBTBEntry(entry);
    } else {
        ubtbStats.predMiss++;
        DPRINTF(UBTB, "uBTB: lookup miss\n");
    }
    return;
}

void
UBTB::fillStagePredictions(const TickedUBTBEntry &entry,
                          std::vector<FullBTBPrediction> &stagePreds)
{
    FillStageLoop(s) {
        auto &pred = stagePreds[s];
        pred.btbEntries.clear();
        pred.condTakens.clear();
        pred.indirectTargets.clear();
        pred.returnTarget = 0;
        pred.predTick = curTick();
        if (!entry.usable()) {
            continue;
        }
        pred.btbEntries = entry.slots;
        bool hasReturn = false;
        for (const auto &slot : entry.slots) {
            if (slot.isCond) {
                pred.condTakens.push_back(
                    {slot.pc, slot.alwaysTaken || slot.ctr >= 0});
            }
            if (slot.isIndirect) {
                if (slot.isReturn) {
                    if (!hasReturn) {
                        pred.returnTarget = slot.target;
                        hasReturn = true;
                    }
                } else {
                    pred.indirectTargets.push_back({slot.pc, slot.target});
                }
            }
        }
    }
}

void
UBTB::putPCHistory(Addr startAddr, const boost::dynamic_bitset<> &history, std::vector<FullBTBPrediction> &stagePreds)
{
    const ThreadID tid = stagePreds.empty() ? 0 : stagePreds.front().tid;
    assert(tid < threadMeta.size());
    threadMeta[tid] = std::make_shared<UBTBMeta>();
    const uint8_t asidHash = stagePreds.empty() ? 0 : stagePreds.front().asidHash;
    auto it = lookup(startAddr, tid, asidHash);
    auto& entry = threadMeta[tid]->hit_entry;
    entry = (it != ubtb.end()) ? *it : TickedUBTBEntry();

    PredStatistics(entry, startAddr);

    // Fill predictions for each pipeline stage
    fillStagePredictions(entry, stagePreds);

}

void
UBTB::refreshPredictionMeta(Addr startAddr,
                            const boost::dynamic_bitset<> &history,
                            FullBTBPrediction &pred)
{
    (void)history;
    assert(pred.tid < threadMeta.size());
    threadMeta[pred.tid] = std::make_shared<UBTBMeta>();
    auto &meta = threadMeta[pred.tid];
    meta->hit_entry = lookupNoSideEffect(
        startAddr, pred.tid, pred.asidHash);
}

UBTB::UBTBIter
UBTB::lookup(Addr startAddr, ThreadID tid, uint8_t asidHash,
             LookupPort port)
{
    if (startAddr & 0x1) {
        return ubtb.end();  // ignore false hit when lowest bit is 1
    }

    const unsigned set = getSet(startAddr, asidHash, tid);
    Addr current_tag = getTag(startAddr, asidHash);

    DPRINTF(UBTB, "uBTB: compare tag %#lx in set %u\n",
            current_tag, set);

    auto [rangeBegin, rangeEnd] = setRange(set, tid);
    UBTBIter hit = rangeEnd;
    unsigned occupancy = 0;
    for (auto it = rangeBegin; it != rangeEnd; ++it) {
        if (!it->valid) {
            continue;
        }
        occupancy++;
        const bool matches = it->tag == current_tag &&
            it->startPC == startAddr;
        if (!matches) {
            continue;
        }
        if (hit == rangeEnd) {
            hit = it;
        } else {
            DPRINTF(UBTB,
                    "uBTB: duplicate hit for tag %#lx in set %u\n",
                    current_tag, set);
            it->valid = false;
        }
    }

    if (port == LookupPort::Prediction) {
        ubtbStats.setLookups[set]++;
        ubtbStats.setOccupancy.sample(occupancy);
    } else {
        ubtbStats.checkerLookups++;
        ubtbStats.checkerSetOccupancy.sample(occupancy);
    }
    if (hit == rangeEnd) {
        if (occupancy == std::distance(rangeBegin, rangeEnd)) {
            if (port == LookupPort::Prediction) {
                ubtbStats.setFullMisses[set]++;
            } else {
                ubtbStats.checkerFullMisses++;
            }
        }
        if (port == LookupPort::Checker) {
            ubtbStats.checkerMisses++;
        }
        return ubtb.end();
    }

    if (hit->overflow) {
        if (port == LookupPort::Prediction) {
            ubtbStats.predOverflowMisses++;
        } else {
            ubtbStats.checkerMisses++;
            ubtbStats.checkerOverflowMisses++;
        }
    } else if (port == LookupPort::Prediction) {
        ubtbStats.setHits[set]++;
    } else {
        ubtbStats.checkerHits++;
    }
    hit->tick = curTick();
    return hit;
}

UBTB::BlockEntry
UBTB::lookupForChecker(Addr startAddr, ThreadID tid, uint8_t asidHash)
{
    auto entry = lookup(startAddr, tid, asidHash, LookupPort::Checker);
    return entry != ubtb.end() ? BlockEntry(*entry) : BlockEntry();
}

void
UBTB::recordCheckerResult(bool matches)
{
    if (matches) {
        ubtbStats.checkerHitAgreements++;
    } else {
        ubtbStats.checkerHitDisagreements++;
    }
}

UBTB::TickedUBTBEntry
UBTB::lookupNoSideEffect(Addr startAddr, ThreadID tid,
                         uint8_t asidHash) const
{
    if (startAddr & 0x1) {
        return TickedUBTBEntry();
    }

    const unsigned set = getSet(startAddr, asidHash, tid);
    Addr current_tag = getTag(startAddr, asidHash);
    auto [range_begin, range_end] = setRange(set, tid);
    auto it = std::find_if(range_begin, range_end,
                           [current_tag, startAddr]
                           (const TickedUBTBEntry &way) {
                               return way.valid && way.tag == current_tag &&
                                      way.startPC == startAddr;
                           });

    return it != range_end ? *it : TickedUBTBEntry();
}


void
UBTB::fillLayout(Addr startAddr, ThreadID tid, uint8_t asidHash,
                 const std::vector<BTBEntry> &entries)
{
    if (startAddr & 1) {
        return;
    }

    TickedUBTBEntry layout;
    layout.valid = true;
    layout.startPC = startAddr;
    layout.tag = getTag(startAddr, asidHash);
    layout.tick = curTick();
    const Addr end = (startAddr + predictWidth) &
        ~mask(floorLog2(predictWidth) - 1);

    // The input is bounded by the supplying BTB's capacity. Normalize the
    // whole window before truncation, retaining branches after the exit.
    for (auto slot : entries) {
        if (slot.valid && slot.pc >= startAddr && slot.pc < end) {
            slot.source = getComponentIdx();
            layout.slots.push_back(slot);
        }
    }
    std::stable_sort(layout.slots.begin(), layout.slots.end(),
        [](const BTBEntry &a, const BTBEntry &b) { return a.pc < b.pc; });
    layout.slots.erase(std::unique(layout.slots.begin(), layout.slots.end(),
        [](const BTBEntry &a, const BTBEntry &b) { return a.pc == b.pc; }),
        layout.slots.end());
    layout.overflow = layout.slots.size() > numSlots;
    if (layout.overflow) {
        layout.slots.resize(numSlots);
        ubtbStats.layoutOverflowFills++;
    }
    ubtbStats.layoutFills++;
    ubtbStats.layoutSlots.sample(layout.slots.size());

    const unsigned set = getSet(startAddr, asidHash, tid);
    auto [begin, endWay] = setRange(set, tid);
    auto dest = std::find_if(begin, endWay,
        [&layout](const TickedUBTBEntry &way) {
            return way.valid && way.tag == layout.tag &&
                way.startPC == layout.startPC;
        });
    if (dest == endWay) {
        ubtbStats.setAllocations[set]++;
        dest = std::find_if(begin, endWay,
            [](const TickedUBTBEntry &way) { return !way.valid; });
        if (dest == endWay) {
            ubtbStats.setEvictions[set]++;
            dest = std::min_element(begin, endWay,
                [](const TickedUBTBEntry &a, const TickedUBTBEntry &b) {
                    return a.tick < b.tick;
                });
        }
    }
    *dest = std::move(layout);
}

void
UBTB::updateUsingS3Pred(FullBTBPrediction &s3Pred)
{
    if (!usingS3Pred) {
        return;
    }
    const bool taken = s3Pred.isTaken();
    const auto &meta = threadMeta[s3Pred.tid];
    const bool hit = meta && meta->hit_entry.usable();
    if (taken) {
        ubtbStats.s3UpdateHits++;
        if (hit) {
            ubtbStats.s1Hits3Taken++;
        } else {
            ubtbStats.s1Misses3Taken++;
        }
    } else {
        ubtbStats.s3UpdateMisses++;
        if (hit) {
            ubtbStats.s1Hits3FallThrough++;
        } else {
            ubtbStats.s1Misses3FallThrough++;
        }
    }
    // Copy BTB layout and base counters, not the final predicted exit or
    // conditional directions produced by TAGE/SC.
    fillLayout(s3Pred.bbStart, s3Pred.tid, s3Pred.asidHash,
               s3Pred.btbEntries);
}

void
UBTB::update(const FetchTarget &stream)
{
    const auto meta = std::static_pointer_cast<UBTBMeta>(
        stream.predMetas[getComponentIdx()]);
    const auto predictedExit = meta->hit_entry.getTakenEntry();
    if (stream.exeTaken) {
        if (!predictedExit.valid || predictedExit != stream.exeBranchInfo) {
            ubtbStats.updateMiss++;
        } else {
            ubtbStats.updateHit++;
        }
    }

    if (!usingS3Pred) {
        // Preserve the legacy backend training option with a whole-window
        // snapshot. Only executed slots train their base direction counters.
        auto entries = stream.predBTBEntries;
        if (stream.exeTaken) {
            auto it = std::find_if(entries.begin(), entries.end(),
                [&stream](const BTBEntry &entry) {
                    return entry.pc == stream.exeBranchInfo.pc;
                });
            if (it == entries.end()) {
                entries.emplace_back(stream.exeBranchInfo);
            } else {
                static_cast<BranchInfo &>(*it) = stream.exeBranchInfo;
                it->valid = true;
            }
        }
        for (auto &slot : entries) {
            if (slot.valid && slot.isCond &&
                slot.pc >= stream.getRealStartPC() &&
                slot.pc <= stream.updateEndInstPC) {
                const bool taken = stream.exeTaken &&
                    slot.pc == stream.exeBranchInfo.pc;
                slot.alwaysTaken = slot.alwaysTaken && taken;
                slot.ctr = taken ? std::min(slot.ctr + 1, 1) :
                                   std::max(slot.ctr - 1, -2);
            }
        }
        fillLayout(stream.getRealStartPC(), stream.tid, stream.asidHash,
                   entries);
    }
}

#ifndef UNIT_TEST
void
UBTB::commitBranch(const FetchTarget &stream, const DynInstPtr &inst)
{
    auto meta = std::static_pointer_cast<UBTBMeta>(stream.predMetas[getComponentIdx()]);
    auto &hit_entry = meta->hit_entry;
    auto pc = inst->getPC();
    auto npc = inst->getNPC();
    const auto slot = std::find_if(
        hit_entry.slots.begin(), hit_entry.slots.end(),
        [pc](const BTBEntry &entry) { return entry.pc == pc; });
    const bool this_branch_hit = hit_entry.usable() &&
        slot != hit_entry.slots.end();

    bool this_branch_taken = stream.exeTaken && stream.getControlPC() == pc;  // all uncond should be taken
    Addr this_branch_target = npc;
    if (this_branch_hit) {
        ubtbStats.allBranchHits++;
        if (this_branch_taken) {
            ubtbStats.allBranchHitTakens++;
        } else {
            ubtbStats.allBranchHitNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ubtbStats.condHits++;
            if (this_branch_taken) {
                ubtbStats.condHitTakens++;
            } else {
                ubtbStats.condHitNotTakens++;
            }
            const bool predictedTaken = slot->alwaysTaken || slot->ctr >= 0;
            if (predictedTaken == this_branch_taken) {
                ubtbStats.condPredCorrect++;
            } else {
                ubtbStats.condPredWrong++;
            }
        }
        if (inst->isUncondCtrl()) {
            ubtbStats.uncondHits++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ubtbStats.indirectHits++;
                Addr pred_target = slot->target;
                if (pred_target == this_branch_target) {
                    ubtbStats.indirectPredCorrect++;
                } else {
                    ubtbStats.indirectPredWrong++;
                }
            }
            if (inst->isCall()) {
                ubtbStats.callHits++;
            }
            if (inst->isReturn()) {
                ubtbStats.returnHits++;
            }
        }
    } else {
        ubtbStats.allBranchMisses++;
        if (this_branch_taken) {
            ubtbStats.allBranchMissTakens++;
        } else {
            ubtbStats.allBranchMissNotTakens++;
        }
        if (inst->isCondCtrl()) {
            ubtbStats.condMisses++;
            if (this_branch_taken) {
                ubtbStats.condMissTakens++;
                ubtbStats.condPredWrong++;
            } else {
                ubtbStats.condMissNotTakens++;
                ubtbStats.condPredCorrect++;
            }
        }
        if (inst->isUncondCtrl()) {
            ubtbStats.uncondMisses++;
        }
        // ignore non-speculative branches (e.g. syscall)
        if (!inst->isNonSpeculative()) {
            if (inst->isIndirectCtrl()) {
                ubtbStats.indirectMisses++;
                ubtbStats.indirectPredWrong++;
            }
            if (inst->isCall()) {
                ubtbStats.callMisses++;
            }
            if (inst->isReturn()) {
                ubtbStats.returnMisses++;
            }
        }
    }
}
#endif

void
UBTB::recordS1OverrideDetail(OverrideReason reason,
                             bool abtbHit,
                             bool afterSquash)
{
    const int reasonBucket = overrideReasonBucket(reason);
    if (reasonBucket < 0) {
        return;
    }

    ubtbStats.s1OverrideByReason[reasonBucket]++;
    ubtbStats.s1OverrideByReasonAndAbtbHit
        [reasonBucket][boolBucket(abtbHit)]++;
    ubtbStats.s1OverrideByReasonAndAfterSquash
        [reasonBucket][boolBucket(afterSquash)]++;
}

#ifndef UNIT_TEST
// Initialize uBTB statistics
UBTB::UBTBStats::UBTBStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(predMiss, statistics::units::Count::get(), "misses encountered on prediction"),
      ADD_STAT(predHit, statistics::units::Count::get(), "hits encountered on prediction"),
      ADD_STAT(updateMiss, statistics::units::Count::get(), "misses encountered on update"),
      ADD_STAT(updateHit, statistics::units::Count::get(), "hits encountered on update"),
      ADD_STAT(s3UpdateHits, statistics::units::Count::get(), "hits encountered on S3 update"),
      ADD_STAT(s3UpdateMisses, statistics::units::Count::get(), "misses encountered on S3 update"),
      ADD_STAT(setLookups, statistics::units::Count::get(),
               "uBTB prediction lookups by set"),
      ADD_STAT(setHits, statistics::units::Count::get(),
               "uBTB prediction hits by set"),
      ADD_STAT(setAllocations, statistics::units::Count::get(),
               "uBTB allocations by set"),
      ADD_STAT(setEvictions, statistics::units::Count::get(),
               "uBTB valid-entry evictions by set"),
      ADD_STAT(setFullMisses, statistics::units::Count::get(),
               "uBTB prediction misses whose selected set has no free way"),
      ADD_STAT(setOccupancy, statistics::units::Count::get(),
               "Valid ways in the selected uBTB set at prediction time"),
      ADD_STAT(checkerLookups, statistics::units::Count::get(),
               "PairTAGE second-block reads issued on the uBTB checker port"),
      ADD_STAT(checkerHits, statistics::units::Count::get(),
               "uBTB checker-port reads that found a complete block layout"),
      ADD_STAT(checkerMisses, statistics::units::Count::get(),
               "uBTB checker-port misses, including overflow layouts"),
      ADD_STAT(checkerFullMisses, statistics::units::Count::get(),
               "uBTB checker-port misses whose selected set had no free way"),
      ADD_STAT(checkerSetOccupancy, statistics::units::Count::get(),
               "Valid ways in the checker-selected uBTB set"),
      ADD_STAT(checkerHitAgreements, statistics::units::Count::get(),
               "PairTAGE second blocks agreeing with a uBTB hit prediction"),
      ADD_STAT(checkerHitDisagreements, statistics::units::Count::get(),
               "PairTAGE second blocks disagreeing with a uBTB hit prediction"),
      ADD_STAT(predOverflowMisses, statistics::units::Count::get(),
               "uBTB prediction misses caused by layout overflow"),
      ADD_STAT(checkerOverflowMisses, statistics::units::Count::get(),
               "uBTB checker misses caused by layout overflow"),
      ADD_STAT(layoutFills, statistics::units::Count::get(),
               "uBTB block layout fills, including empty and overflow layouts"),
      ADD_STAT(layoutOverflowFills, statistics::units::Count::get(),
               "uBTB fills with more branches than the slot capacity"),
      ADD_STAT(layoutSlots, statistics::units::Count::get(),
               "Stored slots per uBTB layout fill, capped at numSlots"),

      ADD_STAT(allBranchHits, statistics::units::Count::get(),
               "all types of branches committed that was predicted hit"),
      ADD_STAT(allBranchHitTakens, statistics::units::Count::get(),
               "all types of taken branches committed was that predicted hit"),
      ADD_STAT(allBranchHitNotTakens, statistics::units::Count::get(),
               "all types of not taken branches committed was that predicted hit"),
      ADD_STAT(allBranchMisses, statistics::units::Count::get(),
               "all types of branches committed that was predicted miss"),
      ADD_STAT(allBranchMissTakens, statistics::units::Count::get(),
               "all types of taken branches committed was that predicted miss"),
      ADD_STAT(allBranchMissNotTakens, statistics::units::Count::get(),
               "all types of not taken branches committed was that predicted miss"),
      ADD_STAT(condHits, statistics::units::Count::get(), "conditional branches committed that was predicted hit"),
      ADD_STAT(condHitTakens, statistics::units::Count::get(),
               "taken conditional branches committed was that predicted hit"),
      ADD_STAT(condHitNotTakens, statistics::units::Count::get(),
               "not taken conditional branches committed was that predicted hit"),
      ADD_STAT(condMisses, statistics::units::Count::get(), "conditional branches committed that was predicted miss"),
      ADD_STAT(condMissTakens, statistics::units::Count::get(),
               "taken conditional branches committed was that predicted miss"),
      ADD_STAT(condMissNotTakens, statistics::units::Count::get(),
               "not taken conditional branches committed was that predicted miss"),
      ADD_STAT(condPredCorrect, statistics::units::Count::get(),
               "conditional branches committed was that correctly predicted by btb"),
      ADD_STAT(condPredWrong, statistics::units::Count::get(),
               "conditional branches committed was that mispredicted by btb"),
      ADD_STAT(uncondHits, statistics::units::Count::get(), "unconditional branches committed that was predicted hit"),
      ADD_STAT(uncondMisses, statistics::units::Count::get(),
               "unconditional branches committed that was predicted miss"),
      ADD_STAT(indirectHits, statistics::units::Count::get(), "indirect branches committed that was predicted hit"),
      ADD_STAT(indirectMisses, statistics::units::Count::get(), "indirect branches committed that was predicted miss"),
      ADD_STAT(indirectPredCorrect, statistics::units::Count::get(),
               "indirect branches committed whose target was correctly predicted by btb"),
      ADD_STAT(indirectPredWrong, statistics::units::Count::get(),
               "indirect branches committed whose target was mispredicted by btb"),
      ADD_STAT(callHits, statistics::units::Count::get(), "calls committed that was predicted hit"),
      ADD_STAT(callMisses, statistics::units::Count::get(), "calls committed that was predicted miss"),
      ADD_STAT(returnHits, statistics::units::Count::get(), "returns committed that was predicted hit"),
      ADD_STAT(returnMisses, statistics::units::Count::get(), "returns committed that was predicted miss"),
      ADD_STAT(s1Hits3FallThrough, statistics::units::Count::get(), "s1 hits s3 predicted fall through"),
      ADD_STAT(s1Misses3Taken, statistics::units::Count::get(), "s1 misses s3 predicted taken"),
      ADD_STAT(s1Hits3Taken, statistics::units::Count::get(), "s1 hits s3 predicted taken"),
      ADD_STAT(s1Misses3FallThrough, statistics::units::Count::get(), "s1 misses s3 predicted fall through"),
      ADD_STAT(s1OverrideByReason, statistics::units::Count::get(),
               "uBTB-sourced S1 override events bucketed by override reason"),
      ADD_STAT(s1OverrideByReasonAndAbtbHit, statistics::units::Count::get(),
               "uBTB-sourced S1 override events bucketed by override reason and native aBTB hit"),
      ADD_STAT(s1OverrideByReasonAndAfterSquash, statistics::units::Count::get(),
               "uBTB-sourced S1 override events bucketed by override reason "
               "and whether the prediction is the first one after squash "
               "recovery")
{
    s1OverrideByReason.init(NumOverrideReasonBuckets);
    s1OverrideByReasonAndAbtbHit.init(NumOverrideReasonBuckets, NumBoolBuckets);
    s1OverrideByReasonAndAfterSquash.init(NumOverrideReasonBuckets, NumBoolBuckets);

    for (int i = 0; i < NumOverrideReasonBuckets; ++i) {
        s1OverrideByReason.subname(i, OverrideReasonLabels[i]);
        s1OverrideByReasonAndAbtbHit.subname(i, OverrideReasonLabels[i]);
        s1OverrideByReasonAndAfterSquash.subname(i, OverrideReasonLabels[i]);
    }
    for (int i = 0; i < NumBoolBuckets; ++i) {
        s1OverrideByReasonAndAbtbHit.ysubname(i, AbtbHitLabels[i]);
        s1OverrideByReasonAndAfterSquash.ysubname(i, AfterSquashLabels[i]);
    }
}
#endif

void
UBTB::UBTBStats::init(unsigned num_sets, unsigned accessible_ways,
                       unsigned num_slots)
{
    setLookups.init(num_sets);
    setHits.init(num_sets);
    setAllocations.init(num_sets);
    setEvictions.init(num_sets);
    setFullMisses.init(num_sets);
    setOccupancy.init(0, accessible_ways, 1);
    checkerSetOccupancy.init(0, accessible_ways, 1);
    layoutSlots.init(0, num_slots, 1);

#ifndef UNIT_TEST
    for (unsigned set = 0; set < num_sets; ++set) {
        const auto name = std::to_string(set);
        setLookups.subname(set, name);
        setHits.subname(set, name);
        setAllocations.subname(set, name);
        setEvictions.subname(set, name);
        setFullMisses.subname(set, name);
    }
#endif

#ifdef UNIT_TEST
    s1OverrideByReason.init(NumOverrideReasonBuckets);
    s1OverrideByReasonAndAbtbHit.init(
        NumOverrideReasonBuckets, NumBoolBuckets);
    s1OverrideByReasonAndAfterSquash.init(
        NumOverrideReasonBuckets, NumBoolBuckets);
#endif
}

#ifdef UNIT_TEST
} // namespace test
#endif

}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5
