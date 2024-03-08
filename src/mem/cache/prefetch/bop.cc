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

BOP::BOP(const BOPPrefetcherParams &p)
    : Queued(p),
      scoreMax(p.score_max), roundMax(p.round_max),
      badScore(p.bad_score), rrEntries(p.rr_size),
      tagMask((1 << p.tag_bits) - 1),
      delayQueueEnabled(p.delay_queue_enable),
      delayQueueSize(p.delay_queue_size),
      delayTicks(cyclesToTicks(p.delay_queue_cycles)),
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
    while (!delayQueue.empty() &&
            delayQueue.front().processTick <= curTick())
    {
        insertIntoRR(delayQueue.front().rrEntry, RRWay::Left);
        delayQueue.pop_front();
    }

    // Schedule an event for the next element if there is one
    if (!delayQueue.empty()) {
        schedule(delayQueueEvent, delayQueue.front().processTick);
    }
}

unsigned int
BOP::hash(Addr addr, unsigned int way) const
{
    Addr hash1 = addr >> way;
    Addr hash2 = hash1 >> floorLog2(rrEntries);
    return (hash1 ^ hash2) & (Addr)(rrEntries - 1);
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
            rrLeft[hash(rr_entry.hashAddr, RRWay::Left)] = rr_entry;
            break;
        case RRWay::Right:
            rrRight[hash(rr_entry.hashAddr, RRWay::Right)] = rr_entry;
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
    return (addr >> lBlkSize) & tagMask;
}

std::pair<bool, BOP::RREntryDebug>
BOP::testRR(Addr tag) const
{
    if (rrLeft[hash(tag, RRWay::Left)].hashAddr == tag) {
        return std::make_pair(true, rrLeft[hash(tag, RRWay::Left)]);
    }
    if (rrRight[hash(tag, RRWay::Right)].hashAddr == tag) {
        return std::make_pair(true, rrRight[hash(tag, RRWay::Right)]);
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
    Addr lookup_addr = x - offset;
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
                        (uint8_t)offsetsListIterator->late, offsetsListIterator->offset, offsetsListIterator->depth);
                offsetsListIterator->late.reset();
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

    DPRINTF(BOPPrefetcher,
            "Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

    if (delayQueueEnabled) {
        insertIntoDelayQueue(addr, tag_x);
    } else {
        insertIntoRR(addr, tag_x, RRWay::Left);
    }

    // Go through the nth offset and update the score, the best score and the
    // current best offset if a better one is found
    bestOffsetLearning(tag_x, late, pfi);

    // This prefetcher is a degree 1 prefetch, so it will only generate one
    // prefetch at most per access
    if (issuePrefetchRequests) {
        Addr prefetch_addr = addr + (bestOffset << lBlkSize);
        stats.issuedOffsetDist.sample(bestOffset);
        sendPFWithFilter(pfi, prefetch_addr, addresses, 32, PrefetchSourceType::HWP_BOP);
        DPRINTF(BOPPrefetcher,
                "Generated prefetch %#lx offset: %d\n",
                prefetch_addr, bestOffset);
    } else {
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

BOP::BopStats::BopStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(issuedOffsetDist, statistics::units::Count::get(), "Distribution of issued offsets"),
      ADD_STAT(learnOffsetCount, statistics::units::Count::get(), "Number of learning offsets"),
      ADD_STAT(throttledCount, statistics::units::Count::get(), "Number of throttled prefetches")
{
    issuedOffsetDist.init(-64, 256, 1).prereq(issuedOffsetDist);
}

} // namespace prefetch
} // namespace gem5
