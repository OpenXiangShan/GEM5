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
splitmix64(uint64_t x)
{
    x = (x + 0x9E3779B97F4A7C15ULL) & 0xFFFFFFFFFFFFFFFFULL;
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL) & 0xFFFFFFFFFFFFFFFFULL;
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EBULL) & 0xFFFFFFFFFFFFFFFFULL;
    return x ^ (x >> 31);
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
      enableAdaptOffset(p.enable_adaptoffset),
      victimListSize(p.victimOffsetsListSize),
      restoreCycle(p.restoreCycle),
      delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()),
      issuePrefetchRequests(false),
      replayTracePrefix(p.replay_trace_prefix),
      replayTrainTable(replayTracePrefix.empty() ? "" : replayTracePrefix + "TrainTraceTable"),
      replayPrefetchTable(replayTracePrefix.empty() ? "" : replayTracePrefix + "PrefetchTraceTable"),
      bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0),
      enableStudentCover(p.enable_student_cover),
      studentPoolSize(p.student_pool_size),
      studentConfAlpha(p.student_conf_alpha),
      studentCovThreshold(p.student_cov_threshold),
      studentLargeOffsetPriorityEnable(
          p.student_large_offset_priority_enable),
      studentLargeOffsetPriorityCoeff(
          p.student_large_offset_priority_coeff),
      studentTeacherTopN(p.student_teacher_top_n),
      studentFilterEntries(p.student_filter_entries),
      studentHashCount(p.student_hash_count),
      studentHashMode(p.student_hash_mode),
      studentDelayQueueEnabled(p.student_delay_queue_enable),
      studentDelayQueueSize(p.student_delay_queue_size),
      studentDelayTicks(cyclesToTicks(p.student_delay_queue_cycles)),
      studentSelectedOffset(1),
      studentSelectedValid(false),
      studentSelectedEnable(false),
      studentPhaseTrainCount(0),
      stats(this, p.student_pool_size, p.student_delay_queue_size)
{
    const bool student_oracle_mode = studentUseOracleMode();

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
        "%s: student_pool_size=%u exceeds 64-bit filter mask capacity",
        name(), studentPoolSize);
    fatal_if(enableStudentCover && !student_oracle_mode &&
            studentFilterEntries == 0,
        "%s: student_filter_entries must be non-zero when student coverage is enabled",
        name());
    fatal_if(enableStudentCover && !student_oracle_mode &&
            !isPowerOf2(studentFilterEntries),
        "%s: student_filter_entries must be a power of 2", name());
    fatal_if(enableStudentCover && studentHashCount == 0,
        "%s: student_hash_count must be non-zero when student coverage is enabled",
        name());
    fatal_if(enableStudentCover && studentDelayQueueEnabled &&
            studentDelayQueueSize == 0,
        "%s: student_delay_queue_size must be non-zero when delayed coverage is enabled",
        name());
    fatal_if(enableStudentCover &&
            ((studentConfAlpha < 0.0) || (studentConfAlpha > 1.0)),
        "%s: student_conf_alpha must be in [0, 1]", name());
    fatal_if(enableStudentCover &&
            ((studentCovThreshold < 0.0) || (studentCovThreshold > 1.0)),
        "%s: student_cov_threshold must be in [0, 1]", name());
    fatal_if(enableStudentCover &&
            (studentHashMode != "lowbits") &&
            (studentHashMode != "bop_rr") &&
            (studentHashMode != "splitmix") &&
            (studentHashMode != "oracle") &&
            (studentHashMode != "exact"),
        "%s: unsupported student_hash_mode '%s'", name(), studentHashMode);
    if (enableStudentCover && studentTeacherTopN > 1) {
        warn("%s: student_teacher_top_n=%u currently reuses only the teacher best offset",
             name(), studentTeacherTopN);
    }
    if (enableStudentCover && studentDelayQueueEnabled && student_oracle_mode) {
        warn("%s: student delayed coverage only applies to DM filter modes; "
             "oracle/exact still use immediate visibility", name());
    }

    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);
    if (enableStudentCover) {
        studentPool.reserve(studentPoolSize);
        if (!student_oracle_mode) {
            studentFilterBits.assign(studentFilterEntries, 0);
        }
    }

    int offset_count = p.offsets.size();
    maxOffsetCount = p.negative_offsets_enable ? 2*p.offsets.size() : p.offsets.size();
    if (p.autoLearning) {
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
        return;
    }

    // Add the address to the delay queue and schedule an event to process
    // it after the specified delay cycles
    Tick process_tick = curTick() + delayTicks;

    delayQueue.push_back(DelayQueueEntry({full_addr, tag}, process_tick));

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

bool
BOP::studentUseOracleMode() const
{
    return (studentHashMode == "oracle") || (studentHashMode == "exact");
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
        const unsigned lgm = floorLog2(studentFilterEntries);
        const uint64_t base =
            ((line_addr & mask) ^ ((line_addr >> lgm) & mask)) & mask;
        base1 = base;
        base2 = ((((line_addr >> (2 * lgm)) & mask) ^ line_addr ^
                0xC2B2ULL) | 1ULL);
    } else {
        base1 = splitmix64(line_addr);
        base2 = splitmix64(line_addr ^ 0x9E3779B97F4A7C15ULL) | 1ULL;
    }

    for (unsigned i = 0; i < studentHashCount; ++i) {
        indexes.push_back(static_cast<unsigned int>(
            ((base1 + i * base2) & 0xFFFFFFFFFFFFFFFFULL) & mask));
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
            return positive ? (entry.offset > 0) : (entry.offset < 0);
        });
}

bool
BOP::studentShouldPreferLargeOffset(size_t best_idx, size_t worst_idx,
        uint32_t best_cov, uint32_t worst_cov) const
{
    if (studentPool.size() < 2 || studentPhaseTrainCount == 0) {
        return false;
    }

    if (!studentPoolAllSameSign()) {
        return false;
    }

    const auto abs_less = [](const StudentOffsetEntry &lhs,
            const StudentOffsetEntry &rhs) {
        const auto lhs_abs = std::llabs(lhs.offset);
        const auto rhs_abs = std::llabs(rhs.offset);
        if (lhs_abs != rhs_abs) {
            return lhs_abs < rhs_abs;
        }
        return lhs.offset < rhs.offset;
    };
    const auto minmax_abs = std::minmax_element(
        studentPool.begin(), studentPool.end(), abs_less);
    const auto best_abs = std::llabs(studentPool[best_idx].offset);
    const auto worst_abs = std::llabs(studentPool[worst_idx].offset);
    const auto min_abs = std::llabs(minmax_abs.first->offset);
    const auto max_abs = std::llabs(minmax_abs.second->offset);

    if (best_abs != min_abs || worst_abs != max_abs) {
        return false;
    }

    // Keep the detector inequality identical to the feature note.
    const double lhs = studentLargeOffsetPriorityCoeff *
        (static_cast<double>(worst_cov) - static_cast<double>(best_cov));
    const double rhs =
        static_cast<double>(worst_abs - best_abs) / studentPhaseTrainCount;
    return lhs <= rhs;
}

void
BOP::studentInsertFilterMask(const std::vector<unsigned int> &indexes,
        uint64_t mask)
{
    if (indexes.empty() || mask == 0) {
        return;
    }

    for (const auto idx : indexes) {
        studentFilterBits[idx] |= mask;
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
        studentInsertFilterMask(entry.filterIndexes, entry.mask);
        stats.studentDelayQueueDrainCount++;
        studentDelayQueue.pop_front();
    }

    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
}

void
BOP::studentEnqueuePrediction(Addr train_addr, size_t bit_idx, int64_t offset)
{
    const int64_t predicted =
        static_cast<int64_t>(train_addr) +
        offset * static_cast<int64_t>(blkSize);
    if (predicted < 0) {
        return;
    }

    const Addr predicted_addr = static_cast<Addr>(predicted);
    if (!crossPage && !samePage(train_addr, predicted_addr)) {
        return;
    }

    const uint64_t mask = 1ULL << bit_idx;
    const auto indexes = studentHashIndexes(predicted_addr >> lBlkSize);
    if (indexes.empty()) {
        return;
    }

    if (!studentDelayQueueEnabled) {
        studentInsertFilterMask(indexes, mask);
        return;
    }

    if (studentDelayQueue.size() >= studentDelayQueueSize) {
        stats.studentDelayQueueFullDropCount++;
        DPRINTF(BOPPrefetcher,
            "student delay queue full, drop predicted addr %#lx offset %ld\n",
            predicted_addr, offset);
        return;
    }

    studentDelayQueue.emplace_back(curTick() + studentDelayTicks, indexes, mask);
    stats.studentDelayQueueInsertCount++;
    stats.studentDelayQueueOccupancyDist.sample(studentDelayQueue.size());
}

size_t
BOP::studentPickBestIndex() const
{
    assert(!studentPool.empty());
    size_t best_idx = 0;
    auto best_key = std::make_tuple(
        studentPool[0].curPhaseCov,
        studentPool[0].conf,
        -std::llabs(studentPool[0].offset),
        -studentPool[0].offset);

    for (size_t i = 1; i < studentPool.size(); ++i) {
        const auto key = std::make_tuple(
            studentPool[i].curPhaseCov,
            studentPool[i].conf,
            -std::llabs(studentPool[i].offset),
            -studentPool[i].offset);
        if (key > best_key) {
            best_key = key;
            best_idx = i;
        }
    }
    return best_idx;
}

size_t
BOP::studentPickWorstIndex() const
{
    assert(!studentPool.empty());
    size_t worst_idx = 0;
    auto worst_key = std::make_tuple(
        studentPool[0].curPhaseCov,
        studentPool[0].conf,
        std::llabs(studentPool[0].offset),
        studentPool[0].offset);

    for (size_t i = 1; i < studentPool.size(); ++i) {
        const auto key = std::make_tuple(
            studentPool[i].curPhaseCov,
            studentPool[i].conf,
            std::llabs(studentPool[i].offset),
            studentPool[i].offset);
        if (key < worst_key) {
            worst_key = key;
            worst_idx = i;
        }
    }
    return worst_idx;
}

size_t
BOP::studentPickEvictIndex() const
{
    assert(!studentPool.empty());
    size_t victim_idx = 0;
    auto victim_key = std::make_tuple(
        studentPool[0].conf,
        studentPool[0].lastPhaseCov,
        -std::llabs(studentPool[0].offset),
        studentPool[0].offset);

    for (size_t i = 1; i < studentPool.size(); ++i) {
        const auto key = std::make_tuple(
            studentPool[i].conf,
            studentPool[i].lastPhaseCov,
            -std::llabs(studentPool[i].offset),
            studentPool[i].offset);
        if (key < victim_key) {
            victim_key = key;
            victim_idx = i;
        }
    }
    return victim_idx;
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

    if (studentUseOracleMode()) {
        for (auto &entry : studentPool) {
            const int64_t prev =
                static_cast<int64_t>(addr) -
                entry.offset * static_cast<int64_t>(blkSize);
            if (prev < 0) {
                continue;
            }

            const Addr prev_addr = static_cast<Addr>(prev);
            if (studentExactSeen.find(prev_addr) != studentExactSeen.end() &&
                (crossPage || samePage(prev_addr, addr))) {
                entry.curPhaseCov++;
            }
        }
        studentExactSeen.insert(addr);
        return;
    }

    if (studentDelayQueueEnabled) {
        studentDrainDelayQueue(curTick());
    }

    const Addr line_addr = addr >> lBlkSize;
    const auto query_indexes = studentHashIndexes(line_addr);
    uint64_t hit_mask = ~0ULL;
    for (const auto idx : query_indexes) {
        hit_mask &= studentFilterBits[idx];
    }

    for (size_t bit_idx = 0; bit_idx < studentPool.size(); ++bit_idx) {
        const uint64_t mask = 1ULL << bit_idx;
        if (hit_mask & mask) {
            studentPool[bit_idx].curPhaseCov++;
        }
    }

    for (size_t bit_idx = 0; bit_idx < studentPool.size(); ++bit_idx) {
        studentEnqueuePrediction(addr, bit_idx, studentPool[bit_idx].offset);
    }
}

bool
BOP::studentInsertTeacherBest(int64_t offset)
{
    if (!enableStudentCover || studentPoolSize == 0 || offset == 0 ||
        studentTeacherTopN == 0) {
        return false;
    }

    const auto it = std::find_if(studentPool.begin(), studentPool.end(),
        [offset](const StudentOffsetEntry &entry) {
            return entry.offset == offset;
        });
    if (it != studentPool.end()) {
        return false;
    }

    if (studentPool.size() >= studentPoolSize) {
        const size_t victim_idx = studentPickEvictIndex();
        DPRINTF(BOPPrefetcher,
            "student evict offset %ld conf %.3f last_cov %u for teacher best %ld\n",
            studentPool[victim_idx].offset, studentPool[victim_idx].conf,
            studentPool[victim_idx].lastPhaseCov, offset);
        studentPool.erase(studentPool.begin() + victim_idx);
    }

    studentPool.emplace_back(offset);
    stats.teacherInjectedCount++;
    stats.teacherInjectedOffsetDist.sample(offset);
    DPRINTF(BOPPrefetcher, "student insert teacher best offset %ld, pool size %zu\n",
            offset, studentPool.size());
    return true;
}

void
BOP::studentClearPhaseState()
{
    if (!enableStudentCover) {
        return;
    }

    std::fill(studentFilterBits.begin(), studentFilterBits.end(), 0);
    studentExactSeen.clear();
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
    return enableStudentCover && studentSelectedValid && studentSelectedEnable;
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

    if (!studentPool.empty()) {
        const size_t best_idx = studentPickBestIndex();
        const size_t worst_idx = studentPickWorstIndex();
        const uint32_t best_cov = studentPool[best_idx].curPhaseCov;
        const uint32_t worst_cov = studentPool[worst_idx].curPhaseCov;
        const bool prefer_large = studentLargeOffsetPriorityEnable &&
            studentShouldPreferLargeOffset(
                best_idx, worst_idx, best_cov, worst_cov);
        size_t selected_idx = best_idx;
        uint32_t selected_cov = best_cov;
        uint32_t sampled_worst_cov = worst_cov;
        const size_t reward_idx = prefer_large ? worst_idx : best_idx;
        const size_t punish_idx = prefer_large ? best_idx : worst_idx;

        if (prefer_large) {
            selected_idx = worst_idx;
            selected_cov = worst_cov;
            sampled_worst_cov = best_cov;
            stats.studentLargeOffsetPriorityCount++;
        }

        const double ratio = studentPhaseTrainCount > 0 ?
            static_cast<double>(selected_cov) / studentPhaseTrainCount : 0.0;
        const double worst_ratio = studentPhaseTrainCount > 0 ?
            static_cast<double>(sampled_worst_cov) /
                studentPhaseTrainCount : 0.0;

        studentSelectedOffset = studentPool[selected_idx].offset;
        studentSelectedValid = true;
        studentSelectedEnable = ratio >= studentCovThreshold;
        stats.studentCovRatioPctDist.sample(
            static_cast<uint64_t>(std::round(ratio * 100.0)));
        stats.studentWorstCovRatioPctDist.sample(
            static_cast<uint64_t>(std::round(worst_ratio * 100.0)));
        if (studentSelectedEnable) {
            stats.studentIssueCount++;
            stats.studentSelectedOffsetDist.sample(studentSelectedOffset);
        } else {
            stats.studentFallbackCount++;
        }

        for (size_t i = 0; i < studentPool.size(); ++i) {
            double update = 0.0;
            if (i == reward_idx) {
                update = 1.0;
            }
            if (i == punish_idx) {
                update = -1.0;
            }
            studentPool[i].conf =
                studentPool[i].conf * studentConfAlpha +
                update * (1.0 - studentConfAlpha);
            studentPool[i].lastPhaseCov = studentPool[i].curPhaseCov;
        }

        DPRINTF(BOPPrefetcher,
            "student phase end: cov_best %ld cov %u, cov_worst %ld cov %u,"
            "selected %ld ratio %.4f, worst_ratio %.4f, phase_train %u enable %d prefer_large %d\n",
            studentPool[best_idx].offset, best_cov,
            studentPool[worst_idx].offset, worst_cov, studentSelectedOffset,
            ratio, worst_ratio, studentPhaseTrainCount, studentSelectedEnable,
            prefer_large);
    } else {
        studentSelectedValid = false;
        studentSelectedEnable = false;
        stats.studentFallbackCount++;
        DPRINTF(BOPPrefetcher, "student phase end: empty pool, fallback to teacher\n");
    }

    if ((teacher_best_offset != 0) && (studentTeacherTopN != 0)) {
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
        } else if ((round >= roundMax/2) && (bestOffset != phaseBestOffset) && (bestScore <= badScore)) {
            DPRINTF(BOPPrefetcher, "last round offset has not enough confidence, early stop\n");
            DPRINTF(BOPPrefetcher, "score %u <  badScore %u\n", bestScore, badScore);
            issuePrefetchRequests = false;
        }
    }
    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    return false;
}

void
BOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late)
{
    Addr addr = blockAddress(pfi.getAddr());
    Addr tag_x = tag(addr);

    if (archDBer && !replayTrainTable.empty()) {
        archDBer->bopReplayTrainTraceWrite(replayTrainTable.c_str(), curTick(), addr);
    }

    DPRINTF(BOPPrefetcher,
            "Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

    if (delayQueueEnabled) {
        insertIntoDelayQueue(addr, tag_x);
    } else {
        insertIntoRR(addr, tag_x, RRWay::Left);
    }

    // Go through the nth offset and update the score, the best score and the
    // current best offset if a better one is found
    const bool teacher_phase_end = bestOffsetLearning(addr, late, pfi);
    studentObserveTrainAddr(addr);
    if (teacher_phase_end) {
        studentOnTeacherPhaseEnd(bestOffset);
    }

    // This prefetcher is a degree 1 prefetch, so it will only generate one
    // prefetch at most per access
    bool force_issue = forceBestOffsetValid && forceIssuePrefetch;
    bool student_issue = studentShouldIssue();
    int64_t issue_offset = forceBestOffsetValid ?
        forcedBestOffset : studentSelectIssueOffset(bestOffset);
    bool do_issue = issuePrefetchRequests || student_issue || force_issue;
    Addr prefetch_addr = addr + (issue_offset * (1ULL << lBlkSize));
    bool prefetch_disable = !do_issue || (!samePage(pfi.getAddr(), prefetch_addr) && !crossPage);

    if (archDBer && !replayPrefetchTable.empty()) {
        archDBer->bopReplayPrefetchTraceWrite(
            replayPrefetchTable.c_str(), curTick(), addr, prefetch_addr,
            issue_offset, prefetch_disable);
    }

    if (do_issue) {
        stats.issuedOffsetDist.sample(issue_offset);
        sendPFWithFilter(pfi, prefetch_addr, addresses, 32, PrefetchSourceType::HWP_BOP);
        DPRINTF(BOPPrefetcher,
                "Generated prefetch %#lx offset: %ld force %d student %d teacher_issue %d\n",
                prefetch_addr, issue_offset, forceBestOffsetValid,
                student_issue, issuePrefetchRequests);
    } else {
        stats.throttledCount++;
        DPRINTF(BOPPrefetcher, "Issue prefetch is false, can't issue\n");
    }

    if (forceBestOffsetValid) {
        forceBestOffsetValid = false;
        forceIssuePrefetch = false;
    }

    if (!victimRestoreScheduled && victimOffsetsList.size() > 0) {
        victimRestoreScheduled = true;
        DPRINTF(BOPPrefetcher, "start victimOffset restore\n");
        schedule(restore_event, cyclesToTicks(curCycle() + Cycles(restoreCycle)));
    }

    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
}

void
BOP::forceBestOffset(int64_t offset, bool force_issue)
{
    assert(offset != 0);
    forcedBestOffset = offset;
    forceBestOffsetValid = true;
    forceIssuePrefetch = force_issue;
}

bool
BOP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                      PrefetchSourceType src)
{
    if (!samePage(pfi.getAddr(), addr) && !crossPage) {
        return false;
    }
    if (archDBer && cache->level() == 1) {
        archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
    }
    if (filter->contains(addr)) {
        DPRINTF(BOPPrefetcher, "Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        DPRINTF(BOPPrefetcher, "Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}

void
BOP::notifyFill(const PacketPtr& pkt)
{

}

BOP::BopStats::BopStats(statistics::Group *parent, unsigned student_pool_size,
        unsigned student_delay_queue_size)
    : statistics::Group(parent),
      ADD_STAT(issuedOffsetDist, statistics::units::Count::get(), "Distribution of issued offsets"),
      ADD_STAT(teacherInjectedOffsetDist, statistics::units::Count::get(),
          "Distribution of teacher offsets injected into the student pool"),
      ADD_STAT(studentSelectedOffsetDist, statistics::units::Count::get(),
          "Distribution of student-selected offsets that passed gating"),
      ADD_STAT(studentPoolOccupancyDist, statistics::units::Count::get(),
          "Student pool occupancy observed at phase end"),
      ADD_STAT(studentCovRatioPctDist, statistics::units::Ratio::get(),
          "Coverage ratio of the student-selected offset at phase end, in percent"),
      ADD_STAT(studentWorstCovRatioPctDist, statistics::units::Ratio::get(),
          "Coverage ratio of the non-selected extreme offset at phase end, in percent"),
      ADD_STAT(studentDelayQueueOccupancyDist, statistics::units::Count::get(),
          "Student delayed-coverage queue occupancy"),
      ADD_STAT(learnOffsetCount, statistics::units::Count::get(), "Number of learning offsets"),
      ADD_STAT(teacherInjectedCount, statistics::units::Count::get(),
          "Number of teacher best offsets injected into the student pool"),
      ADD_STAT(studentPhaseCount, statistics::units::Count::get(),
          "Number of teacher-aligned student phases"),
      ADD_STAT(studentIssueCount, statistics::units::Count::get(),
          "Number of student phases that passed output gating"),
      ADD_STAT(studentFallbackCount, statistics::units::Count::get(),
          "Number of student phases that fell back to teacher output"),
      ADD_STAT(studentLargeOffsetPriorityCount,
          statistics::units::Count::get(),
          "Number of student phases that entered large-offset priority mode"),
      ADD_STAT(studentDelayQueueInsertCount, statistics::units::Count::get(),
          "Number of student delayed-coverage entries enqueued"),
      ADD_STAT(studentDelayQueueDrainCount, statistics::units::Count::get(),
          "Number of student delayed-coverage entries drained into the filter"),
      ADD_STAT(studentDelayQueueFullDropCount, statistics::units::Count::get(),
          "Number of student delayed-coverage entries dropped because the queue was full"),
      ADD_STAT(throttledCount, statistics::units::Count::get(), "Number of throttled prefetches")
{
    issuedOffsetDist.init(-256, 257, 1).prereq(issuedOffsetDist);
    teacherInjectedOffsetDist.init(-256, 257, 1).prereq(teacherInjectedOffsetDist);
    studentSelectedOffsetDist.init(-256, 257, 1).prereq(studentSelectedOffsetDist);
    studentPoolOccupancyDist.init(0, student_pool_size + 1, 1)
        .prereq(studentPoolOccupancyDist);
    studentCovRatioPctDist.init(0, 101, 1).prereq(studentCovRatioPctDist);
    studentWorstCovRatioPctDist.init(0, 101, 1)
        .prereq(studentWorstCovRatioPctDist);
    studentDelayQueueOccupancyDist.init(
        0, std::max(1u, student_delay_queue_size) + 1, 1)
        .prereq(studentDelayQueueOccupancyDist);
}

} // namespace prefetch
} // namespace gem5
