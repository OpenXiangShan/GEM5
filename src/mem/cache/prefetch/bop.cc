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

BOP::PCValidationConfidenceTable::PCValidationConfidenceTable(
    unsigned int entries, unsigned int tag_bits, unsigned int counter_bits,
    unsigned int initial_confidence, unsigned int medium_threshold,
    unsigned int high_threshold, unsigned int hit_increment,
    unsigned int medium_sample_period, unsigned int miss_decay_period,
    unsigned int epoch_bits)
    : entries(entries),
      indexBits(entries > 0 ? floorLog2(entries) : 0),
      tagBits(tag_bits),
      tagMask(tag_bits > 0 && tag_bits < sizeof(Addr) * 8
                  ? (static_cast<Addr>(1) << tag_bits) - 1 : 0),
      counterMax(counter_bits > 0 && counter_bits <= 8
                     ? (1U << counter_bits) - 1 : 0),
      initialConfidence(initial_confidence),
      mediumThreshold(medium_threshold),
      highThreshold(high_threshold),
      hitIncrement(hit_increment),
      mediumSamplePeriod(medium_sample_period),
      missDecayPeriod(miss_decay_period),
      epochMask(epoch_bits > 0 && epoch_bits <= 8
                    ? (1U << epoch_bits) - 1 : 0),
      table(entries)
{
    if (!isPowerOf2(entries)) {
        fatal("BOP PC validation entries must be a power of two\n");
    }
    if (tagBits == 0 || tagBits >= sizeof(Addr) * 8) {
        fatal("BOP PC validation tag bits must be in [1, %zu)\n",
              sizeof(Addr) * 8);
    }
    if (counter_bits == 0 || counter_bits > 8) {
        fatal("BOP PC validation counter bits must be in [1, 8]\n");
    }
    if (epoch_bits == 0 || epoch_bits > 8) {
        fatal("BOP PC validation epoch bits must be in [1, 8]\n");
    }
    if (initialConfidence > counterMax ||
        mediumThreshold > highThreshold || highThreshold > counterMax) {
        fatal("Invalid BOP PC validation confidence thresholds\n");
    }
    if (!isPowerOf2(mediumSamplePeriod) ||
        !isPowerOf2(missDecayPeriod)) {
        fatal("BOP PC validation sample periods must be powers of two\n");
    }
}

Addr
BOP::PCValidationConfidenceTable::foldedPC(Addr pc) const
{
    // RISC-V instructions are at least 2-byte aligned. Fold non-adjacent PC
    // bits before splitting the compact signature into index and partial tag.
    Addr signature = pc >> 1;
    signature ^= signature >> 7;
    signature ^= signature >> 13;
    signature ^= signature >> 27;
    return signature;
}

bool
BOP::PCValidationConfidenceTable::sample(
    Addr pc, Addr line, unsigned int period, Addr salt) const
{
    assert(isPowerOf2(period));

    Addr signature = foldedPC(pc) ^ line ^ salt ^ currentEpoch;
    signature ^= signature >> 9;
    signature ^= signature >> 17;
    signature ^= signature >> 29;
    return (signature & (period - 1)) == 0;
}

BOP::PCValidationConfidenceTable::LookupResult
BOP::PCValidationConfidenceTable::lookup(Addr pc)
{
    const Addr signature = foldedPC(pc);
    const unsigned int index = signature & (entries - 1);
    const Addr tag = (signature >> indexBits) & tagMask;
    Entry &entry = table[index];

    LookupResult result;
    result.index = index;
    result.tag = tag;
    result.entryHit = entry.valid && entry.tag == tag;
    result.replaced = entry.valid && !result.entryHit;

    if (!result.entryHit) {
        entry.valid = true;
        entry.tag = tag;
        entry.confidence = initialConfidence;
        entry.epoch = currentEpoch;
    } else if (entry.epoch != currentEpoch) {
        entry.confidence = initialConfidence;
        entry.epoch = currentEpoch;
        result.epochReset = true;
    }

    result.confidence = entry.confidence;
    result.epoch = entry.epoch;
    if (entry.confidence >= highThreshold) {
        result.state = PCConfidenceState::High;
    } else if (entry.confidence >= mediumThreshold) {
        result.state = PCConfidenceState::Medium;
    } else {
        result.state = PCConfidenceState::Low;
    }
    return result;
}

bool
BOP::PCValidationConfidenceTable::sampleMediumIssue(
    Addr pc, Addr line) const
{
    return sample(pc, line, mediumSamplePeriod, 0x9e37);
}

void
BOP::PCValidationConfidenceTable::submitValidation(
    const LookupResult &lookup, Addr pc, Addr trigger_line,
    bool validation_hit)
{
    if (!pending.valid) {
        pending.valid = true;
        pending.pc = pc;
        pending.triggerLine = trigger_line;
        pending.index = lookup.index;
        pending.tag = lookup.tag;
    } else if (pending.index != lookup.index || pending.tag != lookup.tag ||
               pending.pc != pc) {
        panic("BOP PC validation shared table was not committed per demand\n");
    }

    pending.validationHit = pending.validationHit || validation_hit;
    pending.participants++;
}

void
BOP::PCValidationConfidenceTable::noteOffsetChange()
{
    pending.offsetChanged = true;
}

BOP::PCValidationConfidenceTable::CommitResult
BOP::PCValidationConfidenceTable::commit()
{
    CommitResult result;
    if (!pending.valid && !pending.offsetChanged) {
        return result;
    }

    result.hadPending = true;
    result.hadValidation = pending.participants != 0;
    result.offsetChanged = pending.offsetChanged;
    result.validationHit = pending.validationHit;
    result.pc = pending.pc;
    result.triggerLine = pending.triggerLine;
    result.index = pending.index;
    result.tag = pending.tag;
    result.participants = pending.participants;
    if (result.hadValidation) {
        Entry &entry = table[pending.index];
        assert(entry.valid && entry.tag == pending.tag);
        result.confidenceBefore = entry.confidence;

        // A best-offset change starts a new validation regime. Preserve the
        // current-demand decision, then lazily reset entries before the next
        // demand instead of applying evidence tied to the old regime.
        if (!pending.offsetChanged) {
            if (pending.validationHit) {
                entry.confidence = std::min(
                    counterMax, static_cast<unsigned int>(entry.confidence) +
                                    hitIncrement);
            } else if (sample(pending.pc, pending.triggerLine,
                              missDecayPeriod, 0x7f4a)) {
                entry.confidence = entry.confidence == 0
                    ? 0 : entry.confidence - 1;
                result.decayed = true;
            }
            result.confidenceAfter = entry.confidence;
        } else {
            result.confidenceAfter = entry.confidence;
        }
    }

    if (pending.offsetChanged) {
        currentEpoch = (currentEpoch + 1) & epochMask;
    }
    result.epochAfter = currentEpoch;
    pending = PendingUpdate();
    return result;
}

bool
BOP::PCValidationConfidenceTable::configMatches(
    const PCValidationConfidenceTable &other) const
{
    return entries == other.entries && tagBits == other.tagBits &&
           counterMax == other.counterMax &&
           initialConfidence == other.initialConfidence &&
           mediumThreshold == other.mediumThreshold &&
           highThreshold == other.highThreshold &&
           hitIncrement == other.hitIncrement &&
           mediumSamplePeriod == other.mediumSamplePeriod &&
           missDecayPeriod == other.missDecayPeriod &&
           epochMask == other.epochMask;
}

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
      enableIssueValidation(p.enable_issue_validation),
      enablePCValidationConfidence(p.enable_pc_validation_confidence),
      pcValidationEntries(p.pc_validation_entries),
      pcValidationTagBits(p.pc_validation_tag_bits),
      pcValidationCounterBits(p.pc_validation_counter_bits),
      pcValidationInitial(p.pc_validation_initial),
      pcValidationMediumThreshold(p.pc_validation_medium_threshold),
      pcValidationHighThreshold(p.pc_validation_high_threshold),
      pcValidationHitIncrement(p.pc_validation_hit_increment),
      pcValidationMediumSamplePeriod(p.pc_validation_medium_sample_period),
      pcValidationMissDecayPeriod(p.pc_validation_miss_decay_period),
      pcValidationEpochBits(p.pc_validation_epoch_bits),
      victimListSize(p.victimOffsetsListSize),
      restoreCycle(p.restoreCycle),
      delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()),
      issuePrefetchRequests(false), bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0), stats(this)
{
    if (!isPowerOf2(rrEntries)) {
        fatal("%s: number of RR entries is not power of 2\n", name());
    }
    if (!isPowerOf2(blkSize)) {
        fatal("%s: cache line size is not power of 2\n", name());
    }
    if (enableIssueValidation && enablePCValidationConfidence) {
        fatal("%s: strict and PC-confidence BOP validation are mutually exclusive\n",
              name());
    }
    if (enablePCValidationConfidence) {
        pcValidationTable = std::make_shared<PCValidationConfidenceTable>(
            pcValidationEntries, pcValidationTagBits, pcValidationCounterBits,
            pcValidationInitial, pcValidationMediumThreshold,
            pcValidationHighThreshold, pcValidationHitIncrement,
            pcValidationMediumSamplePeriod, pcValidationMissDecayPeriod,
            pcValidationEpochBits);
    }

    rrLeft.resize(rrEntries);
    rrRight.resize(rrEntries);

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
BOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late)
{
    Addr addr = blockAddress(pfi.getAddr());
    Addr tag_x = tag(addr);
    const Addr trigger_pc = pfi.hasPC() ? pfi.getPC() : 0;
    const bool trigger_is_demand =
        pfi.trigger_info.pkt && pfi.trigger_info.pkt->isDemand();
    const int trigger_pf_source =
        static_cast<int>(pfi.getXsMetadata().prefetchSource);

    DPRINTF(BOPPrefetcher,
            "Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

    if (delayQueueEnabled) {
        insertIntoDelayQueue(addr, tag_x);
    } else {
        insertIntoRR(addr, tag_x, RRWay::Left);
    }

    // Go through the nth offset and update the score, the best score and the
    // current best offset if a better one is found.
    const int64_t previous_best_offset = bestOffset;
    bestOffsetLearning(addr, late, pfi);
    const bool best_offset_changed = bestOffset != previous_best_offset;
    if (enablePCValidationConfidence && best_offset_changed) {
        pcValidationTable->noteOffsetChange();
    }

    const Addr validation_addr = bestOffset != 0
        ? addr - (static_cast<Addr>(bestOffset) << lBlkSize) : 0;
    const Addr prefetch_addr = bestOffset != 0
        ? addr + (bestOffset * (1ULL << lBlkSize)) : 0;
    bool issue_prefetch = issuePrefetchRequests;
    int validation_hit = -1;
    int pc_entry_hit = -1;
    int pc_confidence = -1;
    int pc_state = static_cast<int>(PCConfidenceState::None);
    int pc_sampled = 0;
    int pc_epoch = -1;
    int pc_index = -1;
    Addr pc_tag = 0;
    const bool validation_enabled =
        enableIssueValidation || enablePCValidationConfidence;

    if (issue_prefetch && enableIssueValidation) {
        assert(bestOffset != 0);
        validation_hit = testRR(validation_addr).first;

        stats.issueValidationChecks++;
        if (validation_hit) {
            stats.issueValidationHits++;
        } else {
            stats.issueValidationSuppressed++;
            issue_prefetch = false;
        }
        DPRINTF(BOPPrefetcher, "Issue validation addr %#lx best offset %lld: %s\n", validation_addr,
                static_cast<long long>(bestOffset), validation_hit ? "hit" : "miss");
    } else if (issue_prefetch && enablePCValidationConfidence) {
        assert(bestOffset != 0);
        validation_hit = testRR(validation_addr).first;
        stats.issueValidationChecks++;
        if (validation_hit) {
            stats.issueValidationHits++;
        }

        if (!pfi.hasPC()) {
            // Do not merge all missing-PC accesses into the same synthetic PC
            // entry. The conservative fallback remains strict validation.
            if (!validation_hit) {
                issue_prefetch = false;
                stats.issueValidationSuppressed++;
                stats.pcValidationNoPCSuppressions++;
            }
        } else {
            const auto pc_lookup = pcValidationTable->lookup(trigger_pc);
            pc_entry_hit = pc_lookup.entryHit;
            pc_confidence = pc_lookup.confidence;
            pc_state = static_cast<int>(pc_lookup.state);
            pc_epoch = pc_lookup.epoch;
            pc_index = pc_lookup.index;
            pc_tag = pc_lookup.tag;

            stats.pcValidationTableLookups++;
            if (pc_lookup.entryHit) {
                stats.pcValidationTableHits++;
            } else {
                stats.pcValidationTableMisses++;
            }
            if (pc_lookup.replaced) {
                stats.pcValidationTableReplacements++;
            }
            if (pc_lookup.epochReset) {
                stats.pcValidationEpochResets++;
            }
            stats.pcValidationConfidenceDist.sample(pc_lookup.confidence);

            if (!validation_hit) {
                switch (pc_lookup.state) {
                  case PCConfidenceState::High:
                    stats.pcValidationHighMissIssued++;
                    break;
                  case PCConfidenceState::Medium:
                    pc_sampled = pcValidationTable->sampleMediumIssue(
                        trigger_pc, addr >> lBlkSize);
                    if (pc_sampled) {
                        stats.pcValidationMediumMissIssued++;
                    } else {
                        issue_prefetch = false;
                        stats.issueValidationSuppressed++;
                        stats.pcValidationMediumMissSuppressed++;
                    }
                    break;
                  case PCConfidenceState::Low:
                    issue_prefetch = false;
                    stats.issueValidationSuppressed++;
                    stats.pcValidationLowMissSuppressed++;
                    break;
                  case PCConfidenceState::None:
                    panic("Missing PC validation confidence state\n");
                }
            }
            pcValidationTable->submitValidation(
                pc_lookup, trigger_pc, addr >> lBlkSize, validation_hit);
        }

        DPRINTF(BOPPrefetcher,
                "PC validation addr %#lx offset %lld: RR %s, PC state %d, "
                "confidence %d, issue %d\n",
                validation_addr, static_cast<long long>(bestOffset),
                validation_hit ? "hit" : "miss", pc_state, pc_confidence,
                issue_prefetch);
    }

    // This prefetcher is a degree 1 prefetch, so it will only generate one
    // prefetch at most per access.
    bool generated = false;
    bool buffered = false;
    bool filtered = false;
    bool filter_passed = false;
    if (issue_prefetch) {
        generated = true;
        buffered = samePage(pfi.getAddr(), prefetch_addr) || crossPage;
        stats.issuedOffsetDist.sample(bestOffset);
        filter_passed = sendPFWithFilter(
            pfi, prefetch_addr, addresses, 32, PrefetchSourceType::HWP_BOP);
        filtered = !filter_passed;
        DPRINTF(BOPPrefetcher,
                "Generated prefetch %#lx offset: %d\n",
                prefetch_addr, bestOffset);
    } else if (!issuePrefetchRequests) {
        stats.throttledCount++;
        DPRINTF(BOPPrefetcher, "Issue prefetch is false, can't issue\n");
    }

    if (archDBer) {
        archDBer->bopValidationTraceWrite(
            curTick(), "candidate", name().c_str(), trigger_pc, addr,
            validation_addr, prefetch_addr, bestOffset, bestScore, round, late,
            trigger_is_demand, pfi.isCacheMiss(), trigger_pf_source,
            pfi.isPfFirstHit(), pfi.isPfHit(), issuePrefetchRequests,
            validation_enabled, validation_hit,
            issuePrefetchRequests && validation_enabled && !issue_prefetch,
            generated, buffered, filtered, filter_passed,
            enablePCValidationConfidence, pc_index, pc_tag, pc_entry_hit,
            pc_confidence, pc_state, pc_sampled, pc_epoch);
    }

    // A BOP outside a large/small composite still has well-defined behavior:
    // commit its one participant immediately. Shared pairs commit explicitly
    // after both engines have submitted their validation result.
    if (enablePCValidationConfidence && !pcValidationTableShared) {
        commitPCValidationConfidence();
    }

    if (!victimRestoreScheduled && victimOffsetsList.size() > 0) {
        victimRestoreScheduled = true;
        DPRINTF(BOPPrefetcher, "start victimOffset restore\n");
        schedule(restore_event, cyclesToTicks(curCycle() + Cycles(restoreCycle)));
    }

    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
}

void
BOP::sharePCValidationConfidenceWith(BOP &other)
{
    if (enablePCValidationConfidence != other.enablePCValidationConfidence) {
        fatal("%s and %s must agree on PC validation confidence enablement\n",
              name(), other.name());
    }
    if (!enablePCValidationConfidence) {
        return;
    }
    if (!pcValidationTable->configMatches(*other.pcValidationTable)) {
        fatal("%s and %s must use matching PC validation confidence parameters\n",
              name(), other.name());
    }
    other.pcValidationTable = pcValidationTable;
    pcValidationTableShared = true;
    other.pcValidationTableShared = true;
}

void
BOP::tracePCValidationUpdate(
    const PCValidationConfidenceTable::CommitResult &result)
{
    if (!archDBer || !result.hadPending) {
        return;
    }

    archDBer->bopValidationConfidenceUpdateTraceWrite(
        curTick(), name().c_str(), result.pc, result.index, result.tag,
        result.validationHit, result.participants, result.confidenceBefore,
        result.confidenceAfter, result.decayed, result.offsetChanged,
        result.epochAfter);
}

void
BOP::commitPCValidationConfidence()
{
    if (!enablePCValidationConfidence) {
        return;
    }

    const auto result = pcValidationTable->commit();
    if (!result.hadPending) {
        return;
    }
    if (result.offsetChanged) {
        stats.pcValidationOffsetEpochChanges++;
    }
    if (result.hadValidation && !result.offsetChanged) {
        if (result.validationHit) {
            stats.pcValidationHitUpdates++;
        } else if (result.decayed) {
            stats.pcValidationMissDecays++;
        } else {
            stats.pcValidationMissNoDecays++;
        }
    }
    tracePCValidationUpdate(result);
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
    if (archDBer && cache->level() == 1) {
        archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
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

BOP::BopStats::BopStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(issuedOffsetDist, statistics::units::Count::get(), "Distribution of issued offsets"),
      ADD_STAT(learnOffsetCount, statistics::units::Count::get(), "Number of learning offsets"),
      ADD_STAT(throttledCount, statistics::units::Count::get(), "Number of globally throttled prefetches"),
      ADD_STAT(issueValidationChecks, statistics::units::Count::get(),
               "Number of current-best-offset issue validation checks"),
      ADD_STAT(issueValidationHits, statistics::units::Count::get(),
               "Number of current-best-offset issue validation RR hits"),
      ADD_STAT(issueValidationSuppressed, statistics::units::Count::get(),
               "Number of BOP prefetches suppressed by issue validation"),
      ADD_STAT(pcValidationTableLookups, statistics::units::Count::get(),
               "Number of PC validation-confidence table lookups"),
      ADD_STAT(pcValidationTableHits, statistics::units::Count::get(),
               "Number of PC validation-confidence partial-tag hits"),
      ADD_STAT(pcValidationTableMisses, statistics::units::Count::get(),
               "Number of PC validation-confidence misses"),
      ADD_STAT(pcValidationTableReplacements, statistics::units::Count::get(),
               "Number of valid PC validation-confidence entries replaced"),
      ADD_STAT(pcValidationEpochResets, statistics::units::Count::get(),
               "Number of lazy PC validation-confidence epoch resets"),
      ADD_STAT(pcValidationNoPCSuppressions, statistics::units::Count::get(),
               "Validation misses suppressed because the trigger has no PC"),
      ADD_STAT(pcValidationHighMissIssued, statistics::units::Count::get(),
               "Validation misses issued at high PC confidence"),
      ADD_STAT(pcValidationMediumMissIssued, statistics::units::Count::get(),
               "Sampled validation misses issued at medium PC confidence"),
      ADD_STAT(pcValidationMediumMissSuppressed, statistics::units::Count::get(),
               "Unsampled validation misses suppressed at medium PC confidence"),
      ADD_STAT(pcValidationLowMissSuppressed, statistics::units::Count::get(),
               "Validation misses suppressed at low PC confidence"),
      ADD_STAT(pcValidationHitUpdates, statistics::units::Count::get(),
               "Merged shared-PC validation-hit confidence updates"),
      ADD_STAT(pcValidationMissDecays, statistics::units::Count::get(),
               "Merged sampled all-miss confidence decays"),
      ADD_STAT(pcValidationMissNoDecays, statistics::units::Count::get(),
               "Merged all-miss updates without sampled decay"),
      ADD_STAT(pcValidationOffsetEpochChanges, statistics::units::Count::get(),
               "Shared PC validation-confidence epoch changes"),
      ADD_STAT(pcValidationConfidenceDist, statistics::units::Count::get(),
               "PC validation confidence observed at candidate issue")
{
    issuedOffsetDist.init(-64, 256, 1).prereq(issuedOffsetDist);
    pcValidationConfidenceDist.init(0, 256, 1).prereq(
        pcValidationConfidenceDist);
}

} // namespace prefetch
} // namespace gem5
