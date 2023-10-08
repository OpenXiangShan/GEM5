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

#include "debug/BOPPrefetcher.hh"
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
      delayQueueEvent([this]{ delayQueueEventWrapper(); }, name()),
      issuePrefetchRequests(false), bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0)
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

    for (int i = 0; i < offset_count; i++) {
        offsetsList.emplace_back(p.offsets[i], (uint8_t) 0);
        DPRINTF(BOPPrefetcher, "add %d to offset list\n", p.offsets[i]);
        if (p.negative_offsets_enable) {
            offsetsList.emplace_back(-p.offsets[i], (uint8_t) 0);
            DPRINTF(BOPPrefetcher, "add %d to offset list\n", -p.offsets[i]);
        }
    }

    bestOffset = offsetsList.back().calcOffset();

    offsetsListIterator = offsetsList.begin();
    bestoffsetsListIterator = offsetsListIterator;
}

void
BOP::delayQueueEventWrapper()
{
    while (!delayQueue.empty() &&
            delayQueue.front().processTick <= curTick())
    {
        Addr addr_x = delayQueue.front().baseAddr;
        insertIntoRR(addr_x, RRWay::Left);
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
BOP::insertIntoRR(Addr addr, unsigned int way)
{
    switch (way) {
        case RRWay::Left:
            rrLeft[hash(addr, RRWay::Left)] = addr;
            break;
        case RRWay::Right:
            rrRight[hash(addr, RRWay::Right)] = addr;
            break;
    }
}

void
BOP::insertIntoDelayQueue(Addr x)
{
    if (delayQueue.size() == delayQueueSize) {
        return;
    }

    // Add the address to the delay queue and schedule an event to process
    // it after the specified delay cycles
    Tick process_tick = curTick() + delayTicks;

    delayQueue.push_back(DelayQueueEntry(x, process_tick));

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

bool
BOP::testRR(Addr addr) const
{
    if (rrLeft[hash(addr, RRWay::Left)] == addr) {
        return true;
    }
    if (rrRight[hash(addr, RRWay::Right)] == addr) {
        return true;
    }

    return false;
}

void
BOP::tryAddOffset(int64_t offset, bool late)
{
    assert(offset != 0);
    DPRINTF(BOPPrefetcher, "Reach %s entry, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    // dump offsets:
    DPRINTF(BOPPrefetcher, "offsets: ");
    for (const auto& it : offsetsList) {
        DPRINTFR(BOPPrefetcher, "%d*%d ", it.offset, it.depth);
    }
    DPRINTFR(BOPPrefetcher, "\n");

    if (offsets.size() >= maxOffsetCount) {
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
                DPRINTF(BOPPrefetcher, "erase offset %d from offset list\n", offsetsList.rbegin()->offset);
                offsets.erase(offsetsList.rbegin()->offset);
                offsetsList.erase(--offsetsList.end());

            } else {
                offsets.erase((--offsetsListIterator)->offset);
                DPRINTF(BOPPrefetcher, "erase offset %d from offset list\n", offsetsListIterator->offset);
                offsetsListIterator = offsetsList.erase(offsetsListIterator);
            }
        } else {
            // erase it from set and list
            DPRINTF(BOPPrefetcher, "erase unused offset %d from offset list\n", it->offset);
            offsets.erase(it->offset);
            if (it == offsetsListIterator) {
                offsetsListIterator = offsetsList.erase(it);  // update iterator
                if (offsetsListIterator == offsetsList.end()) {
                    offsetsListIterator = offsetsList.begin();
                }
            } else {
                offsetsList.erase(it);
            }
            DPRINTF(BOPPrefetcher, "%s after erase: iter offset: %d\n", __FUNCTION__,
                    offsetsListIterator->calcOffset());
        }
    }

    auto best_it = getBestOffsetIter();

    if (best_it != offsetsList.end() && (offset % best_it->offset == 0 && best_it->offset != 1)) {
        DPRINTF(BOPPrefetcher, "offset %d is a multiple of best offset %d, skip\n", offset, best_it->offset);
        return;
    }

    auto offset_it = offsets.find(offset);
    if (offset_it == offsets.end()) {
        offsets.insert(offset);
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
        DPRINTF(BOPPrefetcher, "add %d to offset list\n", offset);

    } else {
        bool found = false;
        for (auto it = offsetsList.begin(); it != offsetsList.end(); it++) {
            if (it->offset == offset) {
                found = true;
                break;
            } else {
                DPRINTF(BOPPrefetcher, "offset %d != %ld\n", offset, it->offset);
            }
        }
        assert(found);
    }
    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
}

std::list<BOP::OffsetListEntry>::iterator
BOP::getBestOffsetIter()
{
    return bestoffsetsListIterator;
}

bool
BOP::bestOffsetLearning(Addr x, bool late)
{
    DPRINTF(BOPPrefetcher, "Reach %s entry, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
    Addr offset_addr = offsetsListIterator->calcOffset();
    Addr lookup_addr = x - offset_addr;
    DPRINTF(BOPPrefetcher, "%s: offset: %d lookup addr: %#lx\n", __FUNCTION__, offset_addr, lookup_addr);
    // There was a hit in the RR table, increment the score for this offset
    if (testRR(lookup_addr)) {
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
            }
            else {
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
        insertIntoDelayQueue(tag_x);
    } else {
        insertIntoRR(tag_x, RRWay::Left);
    }

    // Go through the nth offset and update the score, the best score and the
    // current best offset if a better one is found
    //if (!bestOffsetLearning(tag_x, late)) {
    bestOffsetLearning(tag_x, late);
    //}

    // This prefetcher is a degree 1 prefetch, so it will only generate one
    // prefetch at most per access
    if (issuePrefetchRequests) {
        Addr prefetch_addr = addr + (bestOffset << lBlkSize);
        addresses.push_back(AddrPriority(prefetch_addr, 32, PrefetchSourceType::HWP_BOP));
        DPRINTF(BOPPrefetcher,
                "Generated prefetch %#lx offset: %d\n",
                prefetch_addr, bestOffset);
    } else {
        DPRINTF(BOPPrefetcher, "Issue prefetch is false, can't issue\n");
    }

    DPRINTF(BOPPrefetcher, "Reach %s end, iter offset: %d\n", __FUNCTION__, offsetsListIterator->calcOffset());
}

void
BOP::notifyFill(const PacketPtr& pkt)
{

}

} // namespace prefetch
} // namespace gem5
