/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

#include "mem/cache/prefetch/bop.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <tuple>

#include "base/stats/group.hh"
#include "debug/BOPOffsets.hh"
#include "debug/BOPPrefetcher.hh"
#include "mem/cache/base.hh"
#include "params/BOPPrefetcher.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

namespace
{

uint64_t
splitmix64(uint64_t value)
{
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

} // anonymous namespace

BOP::BOP(const BOPPrefetcherParams &p)
    : Queued(p),
      scoreMax(p.score_max), roundMax(p.round_max),
      badScore(p.bad_score), rrEntries(p.rr_size),
      tagMask((1 << p.tag_bits) - 1),
      delayQueueEnabled(p.delay_queue_enable),
      delayQueueSize(p.delay_queue_size),
      delayTicks(cyclesToTicks(p.delay_queue_cycles)),
      crossPage(p.crossPage),
      standaloneL1(p.standalone_l1),
      standaloneFilter(p.standalone_l1 ?
          std::make_unique<boost::compute::detail::lru_cache<Addr, Addr>>(
              256) : nullptr),
      enableAdaptOffset(p.enable_adaptoffset),
      negativeOffsetsEnabled(p.negative_offsets_enable),
      autoLearning(p.autoLearning),
      victimListSize(p.victimOffsetsListSize),
      restoreCycle(p.restoreCycle),
      enableDirectQualityGate(p.enable_direct_quality_gate),
      directQualityKind(p.direct_quality_kind),
      directQualityReplacementPolicy(p.direct_quality_replacement_policy),
      directQualityGate(),
      enableStudentCover(p.enable_student_cover),
      studentPoolSize(p.student_pool_size),
      studentIssueTopN(p.student_issue_top_n),
      studentConfAlpha(p.student_conf_alpha),
      studentCovThreshold(p.student_cov_threshold),
      studentTeacherTopN(p.student_teacher_top_n),
      studentFilterEntries(p.student_filter_entries),
      studentHashCount(p.student_hash_count),
      studentHashMode(p.student_hash_mode),
      studentLargeOffsetPriorityEnable(
          p.student_large_offset_priority_enable),
      studentLargeOffsetPriorityCoeff(
          p.student_large_offset_priority_coeff),
      studentDelayQueueEnabled(p.student_delay_queue_enable),
      studentDelayQueueSize(p.student_delay_queue_size),
      studentDelayTicks(cyclesToTicks(p.student_delay_queue_cycles)),
      delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()),
      issuePrefetchRequests(false), bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0),
      stats(this, p.student_pool_size, p.student_delay_queue_size)
{
    if (standaloneL1) {
        fatal_if(!usePFBuffer, "%s: standalone L1 BOP requires PFRequestBuffer",
                 name());
        filter = standaloneFilter.get();
        setSharedFilterContextQualified(true);
    }
    if (enableDirectQualityGate) {
        DirectQualityGate::Config config;
        config.qualityEntries = p.direct_quality_entries;
        config.qualityWays = p.direct_quality_ways;
        config.qualityTagBits = p.direct_quality_tag_bits;
        config.feedbackEntries = p.direct_quality_feedback_entries;
        config.feedbackWays = p.direct_quality_feedback_ways;
        config.feedbackTagBits = p.direct_quality_feedback_tag_bits;
        config.horizon = p.direct_quality_horizon;
        config.minSamples = p.direct_quality_min_samples;
        config.observeSamplePeriod = p.direct_quality_observe_sample_period;
        config.openSamplePeriod = p.direct_quality_open_sample_period;
        config.blockProbePeriod = p.direct_quality_block_probe_period;
        config.borderlineBlockProbePeriod =
            p.direct_quality_borderline_block_probe_period;
        config.unusedPerUseful = p.direct_quality_unused_per_useful;
        config.blockGuard = p.direct_quality_block_guard;
        config.strictUnusedPerUseful =
            p.direct_quality_strict_unused_per_useful;
        config.strictBlockGuard = p.direct_quality_strict_block_guard;
        config.reopenUnusedPerUseful =
            p.direct_quality_reopen_unused_per_useful;
        config.reopenGuard = p.direct_quality_reopen_guard;
        config.decayPeriod = p.direct_quality_decay_period;
        config.epochBits = p.direct_quality_epoch_bits;
        config.epochShift = p.direct_quality_epoch_shift;
        config.epochTimeout = p.direct_quality_epoch_timeout;
        directQualityGate = std::make_shared<DirectQualityGate>(
            config, directQualityReplacementPolicy);
    }
    if (!isPowerOf2(rrEntries)) {
        fatal("%s: number of RR entries is not power of 2\n", name());
    }
    if (!isPowerOf2(blkSize)) {
        fatal("%s: cache line size is not power of 2\n", name());
    }
    fatal_if(enableStudentCover && studentPoolSize == 0,
             "%s: student_pool_size must be non-zero when student coverage is enabled",
             name());
    fatal_if(enableStudentCover && studentPoolSize > 64,
             "%s: student_pool_size=%u exceeds the 64-bit coverage mask",
             name(), studentPoolSize);
    fatal_if(studentIssueTopN < 1 || studentIssueTopN > 2,
             "%s: student_issue_top_n must be 1 or 2", name());
    fatal_if(enableStudentCover && !isPowerOf2(studentFilterEntries),
             "%s: student_filter_entries must be a power of two", name());
    fatal_if(enableStudentCover && studentHashCount == 0,
             "%s: student_hash_count must be non-zero when student coverage is enabled",
             name());
    fatal_if(enableStudentCover &&
                 studentHashMode != "lowbits" &&
                 studentHashMode != "bop_rr" &&
                 studentHashMode != "splitmix",
             "%s: unsupported student_hash_mode '%s'", name(),
             studentHashMode);
    fatal_if(enableStudentCover &&
                 (studentConfAlpha < 0.0 || studentConfAlpha > 1.0),
             "%s: student_conf_alpha must be in [0, 1]", name());
    fatal_if(enableStudentCover &&
                 (studentCovThreshold < 0.0 || studentCovThreshold > 1.0),
             "%s: student_cov_threshold must be in [0, 1]", name());
    fatal_if(enableStudentCover && studentDelayQueueEnabled &&
                 studentDelayQueueSize == 0,
             "%s: student_delay_queue_size must be non-zero when delay is enabled",
             name());

    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);
    if (enableStudentCover) {
        studentPool.reserve(studentPoolSize);
        studentFilterBits.assign(studentFilterEntries, 0);
    }

    int offset_count = p.offsets.size();
    maxOffsetCount = p.negative_offsets_enable ? 2*p.offsets.size() : p.offsets.size();
    if (autoLearning) {
        maxOffsetCount = 32;
    }


    for (int i = 0; i < offset_count; i++) {
        offsetsList.emplace_back(p.offsets[i], (uint8_t) 0);
        originOffsets.push_back(p.offsets[i]);
        DPRINTF(BOPPrefetcher, "add %d to offset list\n", p.offsets[i]);
        if (p.negative_offsets_enable) {
            offsetsList.emplace_back(-p.offsets[i], (uint8_t) 0);
            originOffsets.push_back(-p.offsets[i]);
            DPRINTF(BOPPrefetcher, "add %d to offset list\n", -p.offsets[i]);
        }
    }

    bestOffset = offsetsList.back().calcOffset();

    offsetsListIterator = offsetsList.begin();
    bestoffsetsListIterator = offsetsListIterator;

    restore_event = new EventFunctionWrapper([this](){
        assert(victimOffsetsList.size() > 0);
        int offset = victimOffsetsList.front();
        victimOffsetsList.pop_front();
        DPRINTF(BOPPrefetcher, "restore offset %d to offsetsList\n", offset);
        tryAddOffset(offset);
        if (victimOffsetsList.size() > 0) {
            DPRINTF(BOPPrefetcher, "start victimOffset restore\n");
            schedule(restore_event, cyclesToTicks(curCycle() + Cycles(restoreCycle)));
        }
        else {
            victimRestoreScheduled = false;
        }
    },name(),false);
}

void
BOP::delayQueueEventWrapper()
{
    if (!delayQueue.empty() &&
            delayQueue.front().processTick <= curTick())
    {
        if (archDBer && archDBer->dumpBopReplayTrace && archDBer->dumpGlobal) {
            archDBer->bopReplayDelayActionTraceWrite(
                name().c_str(), "teacher_dequeue", curTick(),
                delayQueue.front().rrEntry.fullAddr,
                delayQueue.front().processTick, delayQueue.size() - 1);
        }
        insertIntoRR(delayQueue.front().rrEntry, RRWay::Left);
        delayQueue.pop_front();
    }

    // Schedule an event for the next element if there is one
    if (!delayQueue.empty() && (delayQueue.front().processTick <= curTick())) {
        schedule(delayQueueEvent, nextCycle());
    } else if (!delayQueue.empty()) {
        schedule(delayQueueEvent, delayQueue.front().processTick);
    }
}

unsigned int
BOP::hash(Addr addr, unsigned int way) const
{
    // NOTE: This unit-test BOP is used to replay XiangShan-generated traces.
    // Align RR indexing with XiangShan Chisel (BestOffsetPrefetch.scala):
    //   lineAddr = addr >> offsetBits
    //   hash1 = lineAddr[rrIdxBits-1:0]
    //   hash2 = lineAddr[2*rrIdxBits-1:rrIdxBits]
    //   idx   = hash1 ^ hash2
    //
    // The original gem5 BOP implementation used two banks (Left/Right) with
    // different hashing. XiangShan uses a single direct-mapped RR, so 'way'
    // is ignored here.
    //
    // Original gem5 BOP (indexed using the *tag* value, not full addr):
    //   Addr hash1 = tag >> way;
    //   Addr hash2 = hash1 >> floorLog2(rrEntries);
    //   idx = (hash1 ^ hash2) & (rrEntries - 1);
    (void)way;

    const unsigned rrIdxBits = floorLog2(rrEntries);
    const unsigned offsetBits = floorLog2(blkSize);
    const Addr line_addr = addr >> offsetBits;
    const Addr mask = static_cast<Addr>(rrEntries - 1);
    const Addr hash1 = line_addr & mask;
    const Addr hash2 = (line_addr >> rrIdxBits) & mask;
    return static_cast<unsigned int>((hash1 ^ hash2) & mask);
}

void
BOP::insertIntoRR(Addr full_addr, Addr tag, unsigned int way)
{
    insertIntoRR(RREntryDebug(full_addr, tag), way);
}

void
BOP::insertIntoRR(RREntryDebug rr_entry, unsigned int way)
{
    switch (way) {
        case RRWay::Left:
            rrLeft[hash(rr_entry.fullAddr, RRWay::Left)] = rr_entry;
            break;
        case RRWay::Right:
            rrRight[hash(rr_entry.fullAddr, RRWay::Right)] = rr_entry;
            break;
    }
}

void
BOP::insertIntoDelayQueue(Addr full_addr, Addr tag)
{
    if (delayQueue.size() == delayQueueSize) {
        if (archDBer && archDBer->dumpBopReplayTrace && archDBer->dumpGlobal) {
            archDBer->bopReplayDelayActionTraceWrite(
                name().c_str(), "teacher_drop_full", curTick(), full_addr,
                curTick() + delayTicks, delayQueue.size());
        }
        return;
    }

    // Add the address to the delay queue and schedule an event to process
    // it after the specified delay cycles
    Tick process_tick = curTick() + delayTicks;

    delayQueue.push_back(DelayQueueEntry({full_addr, tag}, process_tick));
    if (archDBer && archDBer->dumpBopReplayTrace && archDBer->dumpGlobal) {
        archDBer->bopReplayDelayActionTraceWrite(
            name().c_str(), "teacher_enqueue", curTick(), full_addr,
            process_tick, delayQueue.size());
    }

    if (!delayQueueEvent.scheduled()) {
        schedule(delayQueueEvent, process_tick);
    }
}

void
BOP::resetScores()
{
    for (auto& it : offsetsList) {
        it.score = 0;
    }
}

inline Addr
BOP::tag(Addr addr) const
{
    // Align tag extraction with XiangShan Chisel (BestOffsetPrefetch.scala):
    //   tag = lineAddr[rrIdxBits+rrTagBits-1:rrIdxBits]
    // where lineAddr = addr >> offsetBits.
    //
    // Original gem5 BOP (commented) used:
    //   (addr >> offsetBits) & tagMask
    // which kept the lowest tagBits of the line address.
    const unsigned rrIdxBits = floorLog2(rrEntries);
    const unsigned offsetBits = floorLog2(blkSize);
    const Addr line_addr = addr >> offsetBits;
    return (line_addr >> rrIdxBits) & tagMask;
}

std::pair<bool, BOP::RREntryDebug>
BOP::testRR(Addr addr) const
{
    const Addr t = tag(addr);
    const unsigned idx_l = hash(addr, RRWay::Left);
    if (rrLeft[idx_l].hashAddr == t) {
        return std::make_pair(true, rrLeft[idx_l]);
    }
    const unsigned idx_r = hash(addr, RRWay::Right);
    if (rrRight[idx_r].hashAddr == t) {
        return std::make_pair(true, rrRight[idx_r]);
    }

    return std::make_pair(false, RREntryDebug());
}

bool
BOP::tryAddOffset(int64_t offset, bool late)
{
    assert(offset != 0);
    bool find_it = std::find(offsetsList.begin(), offsetsList.end(), offset) != offsetsList.end();
    if (find_it) {
        return false;
    }
    if (victimOffsetsList.size() >= victimListSize) {
        DPRINTF(BOPPrefetcher, "victimOffsetsList is full, can't add offset\n");
        return false;
    }

    DPRINTF(BOPPrefetcher, "Reach %s entry, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    // dump offsets:
    DPRINTF(BOPPrefetcher, "offset list:\n");
    for (const auto& it : offsetsList) {
        DPRINTF(BOPPrefetcher, "%d*%d\n", it.offset, it.depth);
    }
    DPRINTF(BOPPrefetcher, "victim offset list:\n");
    for (const auto& it : victimOffsetsList) {
        DPRINTF(BOPPrefetcher, "%d\n", it);
    }

    if (offsetsList.size() >= maxOffsetCount) {
        int evict_offset = 0;
        auto it = offsetsList.begin();
        while (it != offsetsList.end()) {
            if (it->score <= badScore) {
                break;
            }
            it++;
        }
        if (it == offsetsList.end()) {
            // all offsets are good, erase the one before the iterator
            if (offsetsListIterator == offsetsList.begin()) {
                // the iterator is the first element, erase the last one
                DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "erase offset %d from offset list\n",
                        offsetsList.rbegin()->offset);
                auto end_offset = --offsetsList.end();
                evict_offset = end_offset->offset;
                offsetsList.erase(end_offset);
            } else {
                auto temp = --offsetsListIterator;
                DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "erase offset %d from offset list\n",
                        temp->offset);
                evict_offset = temp->offset;
                offsetsListIterator = offsetsList.erase(temp);
            }
        } else {
            // erase it from set and list
            DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "erase unused offset %d from offset list\n",
                     it->offset);
            evict_offset = it->offset;
            if (it == offsetsListIterator) {
                offsetsListIterator = offsetsList.erase(it);  // update iterator
                if (offsetsListIterator == offsetsList.end()) {
                    offsetsListIterator = offsetsList.begin();
                }
            } else {
                offsetsList.erase(it);
            }
            DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "%s after erase: iter offset: %d\n", __FUNCTION__,
                     offsetsListIterator->calcOffset());
        }
        assert(evict_offset != 0);
        if (std::find(originOffsets.begin(), originOffsets.end(), evict_offset) != originOffsets.end()) {
            DPRINTF(BOPPrefetcher, "add offset %d to victimOffsetsList\n", evict_offset);
            victimOffsetsList.push_back(evict_offset);
        }
    }

    auto best_it = getBestOffsetIter();

    auto offset_it = std::find(offsetsList.begin(), offsetsList.end(), offset);
    if (offset_it == offsetsList.end()) {
        bool found = false;
        for (auto it = offsetsList.begin(); it != offsetsList.end(); it++) {
            if (it == offsetsListIterator) {
                found = true;
            }
        }
        DPRINTF(BOPPrefetcher, "%s mid: iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
        assert(found);
        // insert it next to the offsetsListIterator
        auto next_it = std::next(offsetsListIterator);
        offsetsList.emplace(next_it, (int32_t) offset, (uint8_t) 0);
        stats.learnOffsetCount++;
        DPRINTFV(debug::BOPPrefetcher || debug::BOPOffsets, "add %d to offset list\n", offset);

    } else {
        bool found = false;
        for (auto it = offsetsList.begin(); it != offsetsList.end(); it++) {
            if (it->offset == offset) {
                found = true;
                break;
            } else {
                DPRINTF(BOPPrefetcher || debug::BOPOffsets, "offset %d != %ld\n", offset, it->offset);
            }
        }
        assert(found);
    }
    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    return true;
}

std::list<BOP::OffsetListEntry>::iterator
BOP::getBestOffsetIter()
{
    return std::find(offsetsList.begin(), offsetsList.end(), bestOffset);
}

std::vector<unsigned int>
BOP::studentHashIndexes(Addr line_addr) const
{
    std::vector<unsigned int> indexes;
    if (!enableStudentCover || studentFilterEntries == 0) {
        return indexes;
    }

    indexes.reserve(studentHashCount);
    const uint64_t mask = static_cast<uint64_t>(studentFilterEntries - 1);
    uint64_t base1 = 0;
    uint64_t base2 = 1;
    if (studentHashMode == "lowbits") {
        base1 = line_addr;
        base2 = ((line_addr >> 6) ^ (line_addr >> 12) ^ 0x9E37ULL) | 1ULL;
    } else if (studentHashMode == "bop_rr") {
        const unsigned int lgm = floorLog2(studentFilterEntries);
        base1 = ((line_addr & mask) ^ ((line_addr >> lgm) & mask)) & mask;
        base2 = ((((line_addr >> (2 * lgm)) & mask) ^ line_addr ^
                  0xC2B2ULL) | 1ULL);
    } else {
        base1 = splitmix64(line_addr);
        base2 = splitmix64(line_addr ^ 0x9E3779B97F4A7C15ULL) | 1ULL;
    }

    for (unsigned int i = 0; i < studentHashCount; ++i) {
        indexes.push_back(static_cast<unsigned int>((base1 + i * base2) &
                                                    mask));
    }
    return indexes;
}

bool
BOP::studentPoolAllSameSign() const
{
    if (studentPool.empty()) {
        return false;
    }
    const bool positive = studentPool.front().offset > 0;
    return std::all_of(studentPool.begin(), studentPool.end(),
        [positive](const StudentOffsetEntry &entry) {
            return positive ? entry.offset > 0 : entry.offset < 0;
        });
}

bool
BOP::studentIntermediateOffsetsMatchSlope(size_t best_idx, size_t worst_idx,
                                          uint32_t best_cov,
                                          uint32_t worst_cov) const
{
    if (studentPool.size() <= 2) {
        return true;
    }

    const auto best_abs = std::llabs(studentPool[best_idx].offset);
    const auto worst_abs = std::llabs(studentPool[worst_idx].offset);
    const double ref_dist = static_cast<double>(worst_abs - best_abs);
    if (ref_dist <= 0.0) {
        return false;
    }
    const double ref_cov_gap = static_cast<double>(best_cov) -
        static_cast<double>(worst_cov);
    for (size_t index = 0; index < studentPool.size(); ++index) {
        if (index == best_idx || index == worst_idx) {
            continue;
        }
        const auto current_abs = std::llabs(studentPool[index].offset);
        if (current_abs <= best_abs || current_abs >= worst_abs) {
            return false;
        }
        const uint32_t current_cov = studentPool[index].curPhaseCov;
        if (current_cov > best_cov || current_cov < worst_cov) {
            return false;
        }
        const double current_dist = static_cast<double>(current_abs - best_abs);
        const double current_cov_gap = static_cast<double>(best_cov) -
            static_cast<double>(current_cov);
        if (ref_cov_gap == 0.0) {
            if (current_cov_gap != 0.0) {
                return false;
            }
            continue;
        }
        const double lhs = current_cov_gap * ref_dist;
        const double rhs = ref_cov_gap * current_dist;
        if (lhs < studentLargeOffsetPriorityCoeff * rhs ||
            studentLargeOffsetPriorityCoeff * lhs > rhs) {
            return false;
        }
    }
    return true;
}

bool
BOP::studentShouldPreferLargeOffset(size_t best_idx, size_t worst_idx,
                                    uint32_t best_cov,
                                    uint32_t worst_cov) const
{
    if (studentPool.size() < 2 || studentPhaseTrainCount == 0 ||
        !studentPoolAllSameSign()) {
        return false;
    }
    const auto abs_less = [](const StudentOffsetEntry &lhs,
                             const StudentOffsetEntry &rhs) {
        const auto lhs_abs = std::llabs(lhs.offset);
        const auto rhs_abs = std::llabs(rhs.offset);
        return lhs_abs == rhs_abs ? lhs.offset < rhs.offset :
                                    lhs_abs < rhs_abs;
    };
    const auto minmax = std::minmax_element(studentPool.begin(),
                                            studentPool.end(), abs_less);
    const auto best_abs = std::llabs(studentPool[best_idx].offset);
    const auto worst_abs = std::llabs(studentPool[worst_idx].offset);
    if (best_abs != std::llabs(minmax.first->offset) ||
        worst_abs != std::llabs(minmax.second->offset)) {
        return false;
    }
    const double lhs = studentLargeOffsetPriorityCoeff *
        (static_cast<double>(worst_cov) - static_cast<double>(best_cov));
    const double rhs = static_cast<double>(worst_abs - best_abs) /
        studentPhaseTrainCount;
    return lhs <= rhs && studentIntermediateOffsetsMatchSlope(
        best_idx, worst_idx, best_cov, worst_cov);
}

void
BOP::studentInsertFilterMask(const std::vector<unsigned int> &indexes,
                             uint64_t mask)
{
    for (const auto index : indexes) {
        studentFilterBits[index] |= mask;
    }
}

void
BOP::studentDrainDelayQueue(Tick now)
{
    if (!studentDelayQueueEnabled) {
        return;
    }
    while (!studentDelayQueue.empty() &&
           studentDelayQueue.front().readyTick <= now) {
        const auto &entry = studentDelayQueue.front();
        if (archDBer && archDBer->dumpBopReplayTrace && archDBer->dumpGlobal) {
            archDBer->bopReplayDelayActionTraceWrite(
                name().c_str(), "student_dequeue", now, entry.predictedAddr,
                entry.readyTick, studentDelayQueue.size() - 1);
        }
        studentInsertFilterMask(entry.filterIndexes, entry.mask);
        stats.studentDelayQueueDrainCount++;
        studentDelayQueue.pop_front();
    }
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
}

void
BOP::studentEnqueuePrediction(Addr train_addr, size_t bit_idx,
                              int64_t offset)
{
    const int64_t predicted = static_cast<int64_t>(train_addr) +
        offset * static_cast<int64_t>(blkSize);
    if (predicted < 0) {
        return;
    }
    const Addr predicted_addr = static_cast<Addr>(predicted);
    if (!crossPage && !samePage(train_addr, predicted_addr)) {
        return;
    }
    const auto indexes = studentHashIndexes(predicted_addr >> lBlkSize);
    if (indexes.empty()) {
        return;
    }
    const uint64_t mask = 1ULL << bit_idx;
    if (!studentDelayQueueEnabled) {
        studentInsertFilterMask(indexes, mask);
        return;
    }
    if (studentDelayQueue.size() >= studentDelayQueueSize) {
        stats.studentDelayQueueFullDropCount++;
        if (archDBer && archDBer->dumpBopReplayTrace && archDBer->dumpGlobal) {
            archDBer->bopReplayDelayActionTraceWrite(
                name().c_str(), "student_drop_full", curTick(),
                predicted_addr, curTick() + studentDelayTicks,
                studentDelayQueue.size());
        }
        return;
    }
    studentDelayQueue.emplace_back(curTick() + studentDelayTicks,
                                   predicted_addr, indexes, mask);
    if (archDBer && archDBer->dumpBopReplayTrace && archDBer->dumpGlobal) {
        archDBer->bopReplayDelayActionTraceWrite(
            name().c_str(), "student_enqueue", curTick(), predicted_addr,
            curTick() + studentDelayTicks, studentDelayQueue.size());
    }
    stats.studentDelayQueueInsertCount++;
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
}

size_t
BOP::studentPickBestIndex() const
{
    assert(!studentPool.empty());
    size_t best = 0;
    auto best_key = std::make_tuple(studentPool[0].curPhaseCov,
                                    studentPool[0].conf,
                                    -std::llabs(studentPool[0].offset),
                                    -studentPool[0].offset);
    for (size_t index = 1; index < studentPool.size(); ++index) {
        const auto key = std::make_tuple(studentPool[index].curPhaseCov,
                                         studentPool[index].conf,
                                         -std::llabs(studentPool[index].offset),
                                         -studentPool[index].offset);
        if (key > best_key) {
            best = index;
            best_key = key;
        }
    }
    return best;
}

size_t
BOP::studentPickSecondIndex(size_t best_idx) const
{
    assert(studentPool.size() >= 2);
    size_t second = best_idx == 0 ? 1 : 0;
    auto second_key = std::make_tuple(studentPool[second].curPhaseCov,
                                      studentPool[second].conf,
                                      -std::llabs(studentPool[second].offset),
                                      -studentPool[second].offset);
    for (size_t index = 0; index < studentPool.size(); ++index) {
        if (index == best_idx) {
            continue;
        }
        const auto key = std::make_tuple(studentPool[index].curPhaseCov,
                                         studentPool[index].conf,
                                         -std::llabs(studentPool[index].offset),
                                         -studentPool[index].offset);
        if (key > second_key) {
            second = index;
            second_key = key;
        }
    }
    return second;
}

size_t
BOP::studentPickWorstIndex() const
{
    assert(!studentPool.empty());
    size_t worst = 0;
    auto worst_key = std::make_tuple(studentPool[0].curPhaseCov,
                                     studentPool[0].conf,
                                     std::llabs(studentPool[0].offset),
                                     studentPool[0].offset);
    for (size_t index = 1; index < studentPool.size(); ++index) {
        const auto key = std::make_tuple(studentPool[index].curPhaseCov,
                                         studentPool[index].conf,
                                         std::llabs(studentPool[index].offset),
                                         studentPool[index].offset);
        if (key < worst_key) {
            worst = index;
            worst_key = key;
        }
    }
    return worst;
}

size_t
BOP::studentPickEvictIndex() const
{
    assert(!studentPool.empty());
    size_t victim = 0;
    auto victim_key = std::make_tuple(studentPool[0].conf,
                                      studentPool[0].lastPhaseCov,
                                      -std::llabs(studentPool[0].offset),
                                      studentPool[0].offset);
    for (size_t index = 1; index < studentPool.size(); ++index) {
        const auto key = std::make_tuple(studentPool[index].conf,
                                         studentPool[index].lastPhaseCov,
                                         -std::llabs(studentPool[index].offset),
                                         studentPool[index].offset);
        if (key < victim_key) {
            victim = index;
            victim_key = key;
        }
    }
    return victim;
}

void
BOP::studentObserveTrainAddr(Addr addr)
{
    if (!enableStudentCover) {
        return;
    }
    studentPhaseTrainCount++;
    if (studentPool.empty()) {
        return;
    }
    studentDrainDelayQueue(curTick());
    uint64_t hit_mask = ~0ULL;
    for (const auto index : studentHashIndexes(addr >> lBlkSize)) {
        hit_mask &= studentFilterBits[index];
    }
    for (size_t index = 0; index < studentPool.size(); ++index) {
        if (hit_mask & (1ULL << index)) {
            studentPool[index].curPhaseCov++;
        }
        studentEnqueuePrediction(addr, index, studentPool[index].offset);
    }
}

bool
BOP::studentInsertTeacherBest(int64_t offset)
{
    if (!enableStudentCover || offset == 0 || studentTeacherTopN == 0 ||
        std::any_of(studentPool.begin(), studentPool.end(),
            [offset](const StudentOffsetEntry &entry) {
                return entry.offset == offset;
            })) {
        return false;
    }
    if (studentPool.size() >= studentPoolSize) {
        studentPool.erase(studentPool.begin() + studentPickEvictIndex());
    }
    studentPool.emplace_back(offset);
    stats.teacherInjectedCount++;
    stats.teacherInjectedOffsetDist.sample(offset);
    return true;
}

void
BOP::studentClearPhaseState()
{
    if (!enableStudentCover) {
        return;
    }
    std::fill(studentFilterBits.begin(), studentFilterBits.end(), 0);
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
    studentDelayQueue.clear();
    studentPhaseTrainCount = 0;
    for (auto &entry : studentPool) {
        entry.curPhaseCov = 0;
    }
}

bool
BOP::studentShouldIssue() const
{
    return enableStudentCover && studentSelectedValid &&
           studentSelectedEnable;
}

int64_t
BOP::studentSelectIssueOffset(int64_t teacher_best_offset) const
{
    return studentShouldIssue() ? studentSelectedOffset : teacher_best_offset;
}

void
BOP::studentOnTeacherPhaseEnd(int64_t teacher_best_offset)
{
    if (!enableStudentCover) {
        return;
    }
    stats.studentPhaseCount++;
    stats.studentPoolOccupancyDist.sample(studentPool.size());
    studentSecondValid = false;
    studentSecondEnable = false;
    if (!studentPool.empty()) {
        const size_t best = studentPickBestIndex();
        const size_t worst = studentPickWorstIndex();
        const uint32_t best_cov = studentPool[best].curPhaseCov;
        const uint32_t worst_cov = studentPool[worst].curPhaseCov;
        const bool prefer_large = studentLargeOffsetPriorityEnable &&
            studentShouldPreferLargeOffset(best, worst, best_cov, worst_cov);
        const size_t selected = prefer_large ? worst : best;
        const uint32_t selected_cov = prefer_large ? worst_cov : best_cov;
        const size_t reward = prefer_large ? worst : best;
        const size_t punish = prefer_large ? best : worst;
        const double coverage = studentPhaseTrainCount == 0 ? 0.0 :
            static_cast<double>(selected_cov) / studentPhaseTrainCount;
        const double other_coverage = studentPhaseTrainCount == 0 ? 0.0 :
            static_cast<double>(prefer_large ? best_cov : worst_cov) /
            studentPhaseTrainCount;
        studentSelectedOffset = studentPool[selected].offset;
        studentSelectedValid = true;
        studentSelectedEnable = coverage >= studentCovThreshold;
        if (studentIssueTopN == 2 && studentPool.size() >= 2 &&
            !prefer_large) {
            const size_t second = studentPickSecondIndex(best);
            const double second_coverage = studentPhaseTrainCount == 0 ? 0.0 :
                static_cast<double>(studentPool[second].curPhaseCov) /
                studentPhaseTrainCount;
            studentSecondOffset = studentPool[second].offset;
            studentSecondValid = true;
            studentSecondEnable = studentSelectedEnable &&
                                  second_coverage >= studentCovThreshold;
            stats.studentSecondCovRatioPctDist.sample(
                static_cast<uint64_t>(std::round(second_coverage * 100.0)));
            if (studentSecondEnable) {
                stats.studentSecondIssueCount++;
                stats.studentSecondOffsetDist.sample(studentSecondOffset);
            }
        }
        stats.studentCovRatioPctDist.sample(
            static_cast<uint64_t>(std::round(coverage * 100.0)));
        stats.studentWorstCovRatioPctDist.sample(
            static_cast<uint64_t>(std::round(other_coverage * 100.0)));
        if (studentSelectedEnable) {
            stats.studentIssueCount++;
            stats.studentSelectedOffsetDist.sample(studentSelectedOffset);
        } else {
            stats.studentFallbackCount++;
        }
        if (prefer_large) {
            stats.studentLargeOffsetPriorityCount++;
        }
        for (size_t index = 0; index < studentPool.size(); ++index) {
            double update = 0.0;
            if (index == reward) {
                update = 1.0;
            } else if (index == punish) {
                update = -1.0;
            }
            studentPool[index].conf = studentPool[index].conf *
                studentConfAlpha + update * (1.0 - studentConfAlpha);
            studentPool[index].lastPhaseCov = studentPool[index].curPhaseCov;
        }
    } else {
        studentSelectedValid = false;
        studentSelectedEnable = false;
        stats.studentFallbackCount++;
    }
    if (teacher_best_offset != 0 && studentTeacherTopN != 0) {
        studentInsertTeacherBest(teacher_best_offset);
    }
    studentClearPhaseState();
}

bool
BOP::bestOffsetLearning(Addr x, bool late, const PrefetchInfo &pfi)
{
    DPRINTF(BOPPrefetcher, "Reach %s entry, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    Addr offset = offsetsListIterator->calcOffset();
    Addr lookup_addr = x - (offset << lBlkSize);
    DPRINTF(BOPPrefetcher, "%s: offset: %d lookup addr: %#lx\n", __FUNCTION__, offset, lookup_addr);
    // There was a hit in the RR table, increment the score for this offset
    auto [exist, rr_entry] = testRR(lookup_addr);
    if (exist) {
        if (archDBer) {
            archDBer->bopTrainTraceWrite(curTick(), rr_entry.fullAddr, pfi.getAddr(), offset,
                                        offsetsListIterator->score + 1, pfi.isCacheMiss());
        }

        DPRINTF(BOPPrefetcher, "Address %#lx found in the RR table\n", x);
        offsetsListIterator->score++;
        if (enableAdaptOffset) {
            if (offsetsListIterator->score >= round / 2) {
                if (late) {
                    offsetsListIterator->late += 2;
                } else {
                    offsetsListIterator->late--;
                }

                auto best_it = getBestOffsetIter();
                bool update_depth = false;
                if (offsetsListIterator->late > (uint8_t)42) {
                    offsetsListIterator->depth++;
                    update_depth = true;
                }
                if (offsetsListIterator->late < (uint8_t)4) {
                    offsetsListIterator->depth = std::max(1, offsetsListIterator->depth - 1);
                    update_depth = true;
                }

                if (update_depth) {
                    if (best_it == offsetsListIterator) {
                        bestOffset = best_it->calcOffset();
                    }
                    DPRINTF(BOPPrefetcher, "Late saturates %u, offset updated to %d * %d\n",
                            (uint8_t)offsetsListIterator->late, offsetsListIterator->offset,
                            offsetsListIterator->depth);
                    offsetsListIterator->late.reset();
                }
            }
        }

        DPRINTF(BOPPrefetcher, "Offset %d score: %i, late: %i, depth: %i, late sat: %u\n", offsetsListIterator->offset,
                offsetsListIterator->score, late, offsetsListIterator->depth, (uint8_t)offsetsListIterator->late);
        if (offsetsListIterator->score > bestScore) {
            bestoffsetsListIterator = offsetsListIterator;
            bestScore = (*offsetsListIterator).score;
            phaseBestOffset = offsetsListIterator->calcOffset();
            DPRINTF(BOPPrefetcher, "New best score is %lu, phase best offset is %lu\n", bestScore, phaseBestOffset);
        }
    }

    offsetsListIterator++;

    // All the offsets in the list were visited meaning that a learning
    // phase finished. Check if
    if (offsetsListIterator == offsetsList.end()) {
        offsetsListIterator = offsetsList.begin();
        round++;

        // Check if the best offset must be updated if:
        // (1) One of the scores equals SCORE_MAX
        // (2) The number of rounds equals ROUND_MAX
        if ((bestScore >= scoreMax) || (round == roundMax)) {
            DPRINTF(BOPPrefetcher, "update new score: %d round: %d phase best offset: %d\n",
                    bestScore, round, phaseBestOffset);

            if (bestScore > badScore) {
                issuePrefetchRequests = true;
                DPRINTF(BOPPrefetcher, "Enable prefetch\n");
            } else {
                issuePrefetchRequests = false;
                DPRINTF(BOPPrefetcher, "Disable prefetch\n");
            }

            bestOffset = phaseBestOffset;
            round = 0;
            bestScore = 0;
            phaseBestOffset = 0;
            resetScores();
            //issuePrefetchRequests = true;
            return true;
         } // here temporarily disable early stop, to align with RTL
        // else if ((round >= roundMax/2) && (bestOffset != phaseBestOffset) && (bestScore <= badScore)) {
        //     DPRINTF(BOPPrefetcher, "last round offset has not enough confidence, early stop\n");
        //     DPRINTF(BOPPrefetcher, "score %u <  badScore %u\n", bestScore, badScore);
        //     issuePrefetchRequests = false;
        // }
    }
    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    return false;
}

void
BOP::writeBOPReplayMeta()
{
    if (replayMetaWritten || !archDBer || !archDBer->dumpBopReplayTrace ||
        !archDBer->dumpGlobal) {
        return;
    }

    std::ostringstream offsets;
    for (size_t index = 0; index < originOffsets.size(); ++index) {
        if (index != 0)
            offsets << ',';
        offsets << originOffsets[index];
    }
    archDBer->bopReplayMetaTraceWrite(
        name().c_str(), blkSize, scoreMax, roundMax, badScore, rrEntries,
        floorLog2(tagMask + 1), delayQueueEnabled, delayQueueSize, delayTicks,
        crossPage, enableAdaptOffset, negativeOffsetsEnabled, autoLearning,
        victimListSize, restoreCycle, clockPeriod(), enableStudentCover,
        studentPoolSize, studentIssueTopN, studentConfAlpha, studentCovThreshold,
        studentTeacherTopN, studentFilterEntries, studentHashMode,
        studentHashCount, studentLargeOffsetPriorityEnable,
        studentLargeOffsetPriorityCoeff, studentDelayQueueEnabled,
        studentDelayQueueSize, studentDelayTicks, offsets.str());
    replayMetaWritten = true;
}

void
BOP::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    if (standaloneL1 && pkt->isDemand() && pkt->isRead() && !pkt->isWrite()) {
        notifyDirectQualityDemand(pfi.getAddr());
    }
    Queued::notify(pkt, pfi);
}

void
BOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late,
        PrefetchSourceType source, bool miss_repeat)
{
    calculatePrefetch(pfi, addresses,
                      late && source == PrefetchSourceType::HWP_BOP);
}

void
BOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late,
        uint64_t replay_access_seq)
{
    Addr addr = blockAddress(pfi.getAddr());
    Addr tag_x = tag(addr);
    const bool replay_tracing = replay_access_seq != 0 && archDBer &&
        archDBer->dumpBopReplayTrace && archDBer->dumpGlobal;
    if (replay_tracing)
        writeBOPReplayMeta();

    DPRINTF(BOPPrefetcher,
            "Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

    if (delayQueueEnabled) {
        insertIntoDelayQueue(addr, tag_x);
    } else {
        insertIntoRR(addr, tag_x, RRWay::Left);
    }

    // The student observes the same demand stream and advances at completed
    // teacher phases. It may issue independently once its selected offset
    // passes the coverage threshold.
    const bool teacher_phase_end = bestOffsetLearning(addr, late, pfi);
    studentObserveTrainAddr(addr);
    if (teacher_phase_end) {
        studentOnTeacherPhaseEnd(bestOffset);
    }

    const bool teacher_issue = issuePrefetchRequests;
    const bool student_issue = studentShouldIssue();
    const int64_t selected_offset = studentSelectIssueOffset(bestOffset);
    const bool algorithm_issue = (teacher_issue || student_issue) &&
        selected_offset != 0;

    const bool second_issue = studentSecondValid && studentSecondEnable &&
                              studentSecondOffset != 0;
    const auto issue_candidate = [&](int64_t offset, unsigned rank,
                                     bool enabled, bool candidate_student_issue,
                                     bool candidate_teacher_issue) {
        bool cqf_allowed = true;
        bool page_allowed = true;
        if (enabled) {
            const Addr prefetch_addr = addr + (offset * (1ULL << lBlkSize));
            page_allowed = crossPage || samePage(pfi.getAddr(), prefetch_addr);
            // A candidate filtered by the page rule has no CQF outcome.
            if (page_allowed && enableDirectQualityGate && directQualityGate &&
                pfi.hasPC()) {
                const auto decision = directQualityGate->admit(
                    pfi.getPC(), static_cast<uint8_t>(directQualityKind),
                    addr, blockAddress(prefetch_addr));
                cqf_allowed = decision.allowed;
                updateDirectQualityStats();
            }
            if (cqf_allowed) {
                stats.issuedOffsetDist.sample(offset);
                const bool generated = sendPFWithFilter(
                    pfi, prefetch_addr, addresses, 32,
                    PrefetchSourceType::HWP_BOP);
                if (rank == 2 && page_allowed &&
                    (!standaloneL1 || generated)) {
                    stats.studentSecondEnqueued++;
                }
                DPRINTF(BOPPrefetcher,
                        "Generated prefetch %#lx offset: %lld rank %u\n",
                        prefetch_addr, static_cast<long long>(offset), rank);
            }
        }
        if (replay_tracing) {
            const std::string bop_name = name();
            const char *bop_kind = bop_name.find("large") != std::string::npos
                ? "large" : "small";
            archDBer->bopReplayEventTraceWrite(
                replay_access_seq, curTick(), bop_name.c_str(), bop_kind,
                addr, pfi.hasPC() ? pfi.getPC() : 0, pfi.hasPC(), late,
                bestOffset, candidate_teacher_issue, candidate_student_issue,
                rank == 1 ? studentSelectedValid : studentSecondValid,
                rank == 1 ? studentSelectedEnable : studentSecondEnable,
                rank == 1 ? studentSelectedOffset : studentSecondOffset,
                offset, enabled, cqf_allowed, rank, page_allowed);
        }
    };
    issue_candidate(selected_offset, 1, algorithm_issue, student_issue,
                    teacher_issue);
    if (studentIssueTopN == 2 && studentSecondValid) {
        issue_candidate(studentSecondOffset, 2, second_issue,
                        second_issue, false);
    }
    if (!algorithm_issue) {
        stats.throttledCount++;
        DPRINTF(BOPPrefetcher, "Issue prefetch is false, can't issue\n");
    }

    if (!victimRestoreScheduled && victimOffsetsList.size() > 0) {
        victimRestoreScheduled = true;
        DPRINTF(BOPPrefetcher, "start victimOffset restore\n");
        schedule(restore_event, cyclesToTicks(curCycle() + Cycles(restoreCycle)));
    }

    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
}

bool
BOP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                      PrefetchSourceType src)
{
    // Count generated prefetch
    prefetchStats.pfGenerated++;

    if (!samePage(pfi.getAddr(), addr) && !crossPage) {
        // Count filtered prefetch (cross-page)
        prefetchStats.pfFiltered++;
        return false;
    }
    if (standaloneL1) {
        const Addr filter_key = sharedFilterKey(pfi, addr);
        if (filter->contains(filter_key)) {
            DPRINTF(BOPPrefetcher, "Skip recently prefetched: %lx\n", addr);
            prefetchStats.pfFiltered++;
            return false;
        }
        filter->insert(filter_key, 0);
        if (archDBer && cache->level() == 1) {
            archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(),
                                     addr, src);
        }
        // The standalone L1 path has one producer and one PFRequestBuffer.
        // Do not also enqueue the same candidate via the direct queue path.
        if (PFRequestBuffer.size() >= max_pf_buffer_size) {
            stats.bufferOverwriteCount++;
        }
        InsertPFRequestToBuffer(
            AddrPriority(addr, prio, src, pfi.trigger_info));
        return true;
    }
    if (archDBer && cache->level() == 1) {
        archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
    }
    if (PFRequestBuffer.size() >= max_pf_buffer_size) {
        stats.bufferOverwriteCount++;
    }
    InsertPFRequestToBuffer(AddrPriority(addr, prio, src, pfi.trigger_info));
    Addr filter_key = sharedFilterKey(pfi, addr);
    if (filter->contains(filter_key)) {
        DPRINTF(BOPPrefetcher, "Skip recently prefetched: %lx\n", addr);
        // Count filtered prefetch
        prefetchStats.pfFiltered++;
        return false;
    } else {
        DPRINTF(BOPPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(filter_key, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}

void
BOP::notifyFill(const PacketPtr& pkt)
{

}

BOP::BopStats::BopStats(statistics::Group *parent,
                        unsigned int student_pool_size,
                        unsigned int student_delay_queue_size)
    : statistics::Group(parent),
      ADD_STAT(issuedOffsetDist, statistics::units::Count::get(), "Distribution of issued offsets"),
      ADD_STAT(teacherInjectedOffsetDist, statistics::units::Count::get(),
               "Teacher offsets admitted to the student pool"),
      ADD_STAT(studentSelectedOffsetDist, statistics::units::Count::get(),
               "Student-selected offsets that pass the coverage threshold"),
      ADD_STAT(studentSecondOffsetDist, statistics::units::Count::get(),
               "Second-ranked student offsets passing coverage threshold"),
      ADD_STAT(studentSecondCovRatioPctDist, statistics::units::Ratio::get(),
               "Second-ranked student coverage percentage at phase end"),
      ADD_STAT(studentPoolOccupancyDist, statistics::units::Count::get(),
               "Student pool occupancy at teacher phase end"),
      ADD_STAT(studentCovRatioPctDist, statistics::units::Ratio::get(),
               "Student selected coverage percentage at phase end"),
      ADD_STAT(studentWorstCovRatioPctDist, statistics::units::Ratio::get(),
               "Student non-selected coverage percentage at phase end"),
      ADD_STAT(studentDelayQueueOccupancyDist, statistics::units::Count::get(),
               "Student coverage delay queue occupancy"),
      ADD_STAT(learnOffsetCount, statistics::units::Count::get(), "Number of learning offsets"),
      ADD_STAT(teacherInjectedCount, statistics::units::Count::get(),
               "Teacher offsets inserted into the student pool"),
      ADD_STAT(studentPhaseCount, statistics::units::Count::get(),
               "Teacher-aligned student coverage phases"),
      ADD_STAT(studentIssueCount, statistics::units::Count::get(),
               "Student phases passing the independent issue threshold"),
      ADD_STAT(studentSecondIssueCount, statistics::units::Count::get(),
               "Student phases with a second offset passing coverage threshold"),
      ADD_STAT(studentSecondEnqueued, statistics::units::Count::get(),
               "Second-ranked candidates added to the BOP request buffer"),
      ADD_STAT(bufferOverwriteCount, statistics::units::Count::get(),
               "BOP request buffer entries overwritten when full"),
      ADD_STAT(studentFallbackCount, statistics::units::Count::get(),
               "Student phases falling back to teacher output"),
      ADD_STAT(studentLargeOffsetPriorityCount, statistics::units::Count::get(),
               "Student phases selecting the large-offset priority path"),
      ADD_STAT(studentDelayQueueInsertCount, statistics::units::Count::get(),
               "Student coverage predictions inserted into the delay queue"),
      ADD_STAT(studentDelayQueueDrainCount, statistics::units::Count::get(),
               "Student coverage predictions made visible after delay"),
      ADD_STAT(studentDelayQueueFullDropCount, statistics::units::Count::get(),
               "Student coverage predictions dropped because the delay queue was full"),
      ADD_STAT(throttledCount, statistics::units::Count::get(), "Number of throttled prefetches"),
      ADD_STAT(directQualityCandidates, statistics::units::Count::get(),
               "Raw BOP candidates evaluated by CQF"),
      ADD_STAT(directQualityAllowed, statistics::units::Count::get(),
               "Raw BOP candidates allowed by CQF"),
      ADD_STAT(directQualitySuppressed, statistics::units::Count::get(),
               "Raw BOP candidates suppressed by CQF"),
      ADD_STAT(directQualitySampled, statistics::units::Count::get(),
               "Raw BOP candidates sampled by CQF"),
      ADD_STAT(directQualityUseful, statistics::units::Count::get(),
               "CQF useful outcomes"),
      ADD_STAT(directQualityUnused, statistics::units::Count::get(),
               "CQF unused outcomes"),
      ADD_STAT(directQualityFeedbackConflicts, statistics::units::Count::get(),
               "CQF feedback conflicts"),
      ADD_STAT(directQualityFeedbackReplacements, statistics::units::Count::get(),
               "CQF feedback replacements"),
      ADD_STAT(directQualityFeedbackCoalesced, statistics::units::Count::get(),
               "CQF coalesced feedback candidates"),
      ADD_STAT(directQualityFeedbackFoldedCandidates,
               statistics::units::Count::get(),
               "CQF candidates whose feedback addresses were folded"),
      ADD_STAT(directQualityFeedbackFoldedDemands,
               statistics::units::Count::get(),
               "CQF demands whose feedback addresses were folded"),
      ADD_STAT(directQualityFeedbackExpiries, statistics::units::Count::get(),
               "CQF feedback expiries"),
      ADD_STAT(directQualityUnknownDrops, statistics::units::Count::get(),
               "CQF censored feedback drops"),
      ADD_STAT(directQualityOrphanOutcomes, statistics::units::Count::get(),
               "CQF orphan outcomes"),
      ADD_STAT(directQualityStateTransitions, statistics::units::Count::get(),
               "CQF state transitions"),
      ADD_STAT(directQualityPeakOutstanding, statistics::units::Count::get(),
               "Peak CQF feedback entries")
{
    issuedOffsetDist.init(-64, 256, 1).prereq(issuedOffsetDist);
    teacherInjectedOffsetDist.init(-256, 257, 1).prereq(
        teacherInjectedOffsetDist);
    studentSelectedOffsetDist.init(-256, 257, 1).prereq(
        studentSelectedOffsetDist);
    studentSecondOffsetDist.init(-256, 257, 1).prereq(
        studentSecondOffsetDist);
    studentSecondCovRatioPctDist.init(0, 101, 1).prereq(
        studentSecondCovRatioPctDist);
    studentPoolOccupancyDist.init(0, student_pool_size + 1, 1).prereq(
        studentPoolOccupancyDist);
    studentCovRatioPctDist.init(0, 101, 1).prereq(studentCovRatioPctDist);
    studentWorstCovRatioPctDist.init(0, 101, 1).prereq(
        studentWorstCovRatioPctDist);
    studentDelayQueueOccupancyDist.init(
        0, std::max(1u, student_delay_queue_size) + 1, 1).prereq(
            studentDelayQueueOccupancyDist);
}

void
BOP::updateDirectQualityStats()
{
    if (!directQualityGate)
        return;
    stats.directQualityCandidates = directQualityGate->candidates();
    stats.directQualityAllowed = directQualityGate->allowed();
    stats.directQualitySuppressed = directQualityGate->suppressed();
    stats.directQualitySampled = directQualityGate->sampled();
    stats.directQualityUseful = directQualityGate->useful();
    stats.directQualityUnused = directQualityGate->unused();
    stats.directQualityFeedbackConflicts = directQualityGate->feedbackConflicts();
    stats.directQualityFeedbackReplacements = directQualityGate->feedbackReplacements();
    stats.directQualityFeedbackCoalesced = directQualityGate->feedbackCoalesced();
    stats.directQualityFeedbackFoldedCandidates =
        directQualityGate->feedbackFoldedCandidates();
    stats.directQualityFeedbackFoldedDemands =
        directQualityGate->feedbackFoldedDemands();
    stats.directQualityFeedbackExpiries = directQualityGate->feedbackExpiries();
    stats.directQualityUnknownDrops = directQualityGate->unknownDrops();
    stats.directQualityOrphanOutcomes = directQualityGate->orphanOutcomes();
    stats.directQualityStateTransitions = directQualityGate->stateTransitions();
    stats.directQualityPeakOutstanding = directQualityGate->peakOutstanding();
}

void
BOP::shareDirectQualityGateWith(BOP &other)
{
    if (enableDirectQualityGate != other.enableDirectQualityGate)
        fatal("Large and Small BOP must agree on CQF enablement\n");
    if (!enableDirectQualityGate)
        return;
    if (!directQualityGate->configMatches(*other.directQualityGate))
        fatal("Large and Small BOP must use matching CQF parameters\n");
    if (directQualityKind == other.directQualityKind)
        fatal("Large and Small BOP must use different CQF kinds\n");
    other.directQualityGate = directQualityGate;
}

void
BOP::notifyDirectQualityDemand(Addr paddr)
{
    if (!enableDirectQualityGate || !directQualityGate)
        return;
    directQualityGate->observeDemand(blockAddress(paddr));
    updateDirectQualityStats();
}

} // namespace prefetch
} // namespace gem5
