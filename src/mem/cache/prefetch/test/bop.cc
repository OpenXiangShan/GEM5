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

#include "mem/cache/prefetch/test/bop.hh"

#include <cassert>
#include <cstdio>

#include "base/intmath.hh"

// #include "base/stats/group.hh"
// #include "debug/BOPOffsets.hh"
// #include "debug/BOPPrefetcher.hh"
// #include "mem/cache/base.hh"

namespace gem5
{

namespace prefetch
{

namespace test
{

BOP::BOP(const BOPPrefetcherParams &p, SimpleEventQueue *eq)
    : eventQueue(eq),
      scoreMax(p.score_max), roundMax(p.round_max),
      badScore(p.bad_score), blockSize(p.block_size),rrEntries(p.rr_size),
      tagMask((1 << p.tag_bits) - 1),
      delayQueueEnabled(p.delay_queue_enable),
      delayQueueSize(p.delay_queue_size),
      delayCycles(p.delay_queue_cycles),
      crossPage(p.crossPage),
      victimListSize(p.victimOffsetsListSize),
      restoreCycle(p.restoreCycle),
      issuePrefetchRequests(false), bestOffset(1), phaseBestOffset(0),
      bestScore(0), round(0)
{
    // These need mocks or to be passed in if used
    // lBlkSize = p.block_size;
    // isPowerOf2 = ...;

    // if (!isPowerOf2(rrEntries)) {
    //     assert(false && "number of RR entries is not power of 2");
    // }
    // if (!isPowerOf2(blkSize)) {
    //     assert(false && "cache line size is not power of 2");
    // }

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
        printf_wrapper("BOPPrefetcher: add %d to offset list\n", p.offsets[i]);
        if (p.negative_offsets_enable) {
            offsetsList.emplace_back(-p.offsets[i], (uint8_t) 0);
            originOffsets.push_back(-p.offsets[i]);
            printf_wrapper("BOPPrefetcher: add %d to offset list\n", -p.offsets[i]);
        }
    }

    bestOffset = offsetsList.back().calcOffset();

    offsetsListIterator = offsetsList.begin();
    bestoffsetsListIterator = offsetsListIterator;

    filter = new boost::compute::detail::lru_cache<Addr, Addr>(128);
}

void
BOP::delayQueueEventWrapper()
{
    while (!delayQueue.empty() &&
            delayQueue.front().processCycle <= eventQueue->curCycle())
    {
        insertIntoRR(delayQueue.front().rrEntry, RRWay::Left);
        delayQueue.pop_front();
    }

    // Schedule an event for the next element if there is one
    if (!delayQueue.empty()) {
        eventQueue->schedule([this]{ delayQueueEventWrapper(); }, delayQueue.front().processCycle);
    }
}

void
BOP::restoreEventWrapper()
{
    assert(victimOffsetsList.size() > 0);
    int offset = victimOffsetsList.front();
    victimOffsetsList.pop_front();
    printf_wrapper("BOPPrefetcher: restore offset %d to offsetsList\n", offset);
    tryAddOffset(offset);
    if (victimOffsetsList.size() > 0) {
        printf_wrapper("BOPPrefetcher: start victimOffset restore\n");
        eventQueue->schedule([this](){restoreEventWrapper();}, eventQueue->curCycle() + restoreCycle);
    }
    else {
        victimRestoreScheduled = false;
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
    SimpleCycle process_cycle = eventQueue->curCycle() + delayCycles;

    delayQueue.push_back(DelayQueueEntry({full_addr, tag}, process_cycle));

    // This check is tricky without a direct way to see if an event is scheduled.
    // For a simple test, we can just schedule it.
    eventQueue->schedule([this]{ delayQueueEventWrapper(); }, process_cycle);
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
    return (addr >> floorLog2(blockSize)) & tagMask; // Assuming 64-byte blocks
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
        printf_wrapper("BOPPrefetcher: victimOffsetsList is full, can't add offset\n");
        return false;
    }

    printf_wrapper("BOPPrefetcher: Reach %s entry, iter offset: %ld\n",
                    __FUNCTION__, offsetsListIterator->calcOffset());
    // dump offsets:
    printf_wrapper("BOPPrefetcher: offset list:\n");
    for (const auto& it : offsetsList) {
        printf_wrapper("BOPPrefetcher: %d*%d\n", it.offset, it.depth);
    }
    printf_wrapper("BOPPrefetcher: victim offset list:\n");
    for (const auto& it : victimOffsetsList) {
        printf_wrapper("BOPPrefetcher: %d\n", it);
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
                printf_wrapper("BOPPrefetcher: erase offset %d from offset list\n",
                        offsetsList.rbegin()->offset);
                auto end_offset = --offsetsList.end();
                evict_offset = end_offset->offset;
                offsetsList.erase(end_offset);
            } else {
                auto temp = --offsetsListIterator;
                printf_wrapper("BOPPrefetcher: erase offset %d from offset list\n",
                        temp->offset);
                evict_offset = temp->offset;
                offsetsListIterator = offsetsList.erase(temp);
            }
        } else {
            // erase it from set and list
            printf_wrapper("BOPPrefetcher: erase unused offset %d from offset list\n",
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
            printf_wrapper("BOPPrefetcher: %s after erase: iter offset: %ld\n", __FUNCTION__,
                     offsetsListIterator->calcOffset());
        }
        assert(evict_offset != 0);
        if (std::find(originOffsets.begin(), originOffsets.end(), evict_offset) != originOffsets.end()) {
            printf_wrapper("BOPPrefetcher: add offset %d to victimOffsetsList\n", evict_offset);
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
        printf_wrapper("BOPPrefetcher: %s mid: iter offset: %ld\n", __FUNCTION__, offsetsListIterator->calcOffset());
        assert(found);
        // insert it next to the offsetsListIterator
        auto next_it = std::next(offsetsListIterator);
        offsetsList.emplace(next_it, (int32_t) offset, (uint8_t) 0);
        stats.learnOffsetCount++;
        printf_wrapper("BOPPrefetcher: add %ld to offset list\n", offset);

    } else {
        bool found = false;
        for (auto it = offsetsList.begin(); it != offsetsList.end(); it++) {
            if (it->offset == offset) {
                found = true;
                break;
            } else {
                printf_wrapper("BOPPrefetcher: offset %ld != %d\n", offset, it->offset);
            }
        }
        assert(found);
    }
    printf_wrapper("BOPPrefetcher: Reach %s end, iter offset: %ld\n", __FUNCTION__, offsetsListIterator->calcOffset());
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
    printf_wrapper("BOPPrefetcher: Reach %s entry, iter offset: %ld\n",
                    __FUNCTION__, offsetsListIterator->calcOffset());
    Addr offset = offsetsListIterator->calcOffset();
    Addr lookup_addr = x - offset;
    printf_wrapper("BOPPrefetcher: %s: offset: %ld lookup addr: %#lx\n",
                    __FUNCTION__, offset, lookup_addr);
    // There was a hit in the RR table, increment the score for this offset
    auto [exist, rr_entry] = testRR(lookup_addr);
    if (exist) {
        // archDBer is not available in test environment
        // if (archDBer) {
        //     archDBer->bopTrainTraceWrite(curTick(), rr_entry.fullAddr, pfi.getAddr(), offset,
        //                                 offsetsListIterator->score + 1, pfi.isCacheMiss());
        // }

        printf_wrapper("BOPPrefetcher: Address %#lx found in the RR table\n", x);
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
                printf_wrapper("BOPPrefetcher: Late saturates %u, offset updated to %d * %d\n",
                        (uint8_t)offsetsListIterator->late, offsetsListIterator->offset, offsetsListIterator->depth);
                offsetsListIterator->late.reset();
            }
        }

        printf_wrapper("BOPPrefetcher: Offset %d score: %i, late: %i, depth: %i, late sat: %u\n",
                        offsetsListIterator->offset, offsetsListIterator->score, late,
                        offsetsListIterator->depth, (uint8_t)offsetsListIterator->late);
        if (offsetsListIterator->score > bestScore) {
            bestoffsetsListIterator = offsetsListIterator;
            bestScore = (*offsetsListIterator).score;
            phaseBestOffset = offsetsListIterator->calcOffset();
            printf_wrapper("BOPPrefetcher: New best score is %u, phase best offset is %ld\n",
                            bestScore, phaseBestOffset);
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
            printf_wrapper("BOPPrefetcher: update new score: %u round: %u phase best offset: %ld\n",
                    bestScore, round, phaseBestOffset);

            if (bestScore > badScore) {
                issuePrefetchRequests = true;
                printf_wrapper("BOPPrefetcher: Enable prefetch\n");
            } else {
                issuePrefetchRequests = false;
                printf_wrapper("BOPPrefetcher: Disable prefetch\n");
            }

            bestOffset = phaseBestOffset;
            round = 0;
            bestScore = 0;
            phaseBestOffset = 0;
            resetScores();
            //issuePrefetchRequests = true;
            return true;
        } else if ((round >= roundMax/2) && (bestOffset != phaseBestOffset) && (bestScore <= badScore)) {
            printf_wrapper("BOPPrefetcher: last round offset has not enough confidence, early stop\n");
            printf_wrapper("BOPPrefetcher: score %u <  badScore %u\n", bestScore, badScore);
            issuePrefetchRequests = false;
        }
    }
    printf_wrapper("BOPPrefetcher: Reach %s end, iter offset: %ld\n", __FUNCTION__, offsetsListIterator->calcOffset());
    return false;
}

void
BOP::calculatePrefetch(const PrefetchInfo &pfi,
        std::vector<AddrPriority> &addresses, bool late)
{
    // Addr addr = blockAddress(pfi.getAddr());
    Addr addr = pfi.getAddr() & ~((Addr)blockSize - 1); // Simplified blockAddress
    Addr tag_x = tag(addr);

    printf_wrapper("BOPPrefetcher: Train prefetcher with addr %#lx tag %#lx\n", addr, tag_x);

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
        Addr prefetch_addr = addr + (bestOffset * (1ULL << floorLog2(blockSize))); // Assuming 64 byte blocks
        // stats.issuedOffsetDist.sample(bestOffset);
        sendPFWithFilter(pfi, prefetch_addr, addresses, 32, PrefetchSourceType::HWP_BOP);
        printf_wrapper("BOPPrefetcher: Generated prefetch %#lx offset: %ld\n",
                prefetch_addr, bestOffset);
    } else {
        stats.throttledCount++;
        printf_wrapper("BOPPrefetcher: Issue prefetch is false, can't issue\n");
    }

    if (!victimRestoreScheduled && victimOffsetsList.size() > 0) {
        victimRestoreScheduled = true;
        printf_wrapper("BOPPrefetcher: start victimOffset restore\n");
        eventQueue->schedule([this](){restoreEventWrapper();}, eventQueue->curCycle() + restoreCycle);
    }

    printf_wrapper("BOPPrefetcher: Reach %s end, iter offset: %ld\n", __FUNCTION__, offsetsListIterator->calcOffset());
}

bool
BOP::samePage(Addr a, Addr b) const
{
    return roundDown(a, 4096) == roundDown(b, 4096);
}


bool
BOP::sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                      PrefetchSourceType src)
{
    if (!samePage(pfi.getAddr(), addr) && !crossPage) {
        return false;
    }
    // if (archDBer && cache->level() == 1) {
    //     archDBer->l1PFTraceWrite(curTick(), pfi.getPC(), pfi.getAddr(), addr, src);
    // }
    if (filter->contains(addr)) {
        printf_wrapper("BOPPrefetcher: Skip recently prefetched: %lx\n", addr);
        return false;
    } else {
        printf_wrapper("BOPPrefetcher: Send pf: %lx\n", addr);
        filter->insert(addr, 0);
        addresses.push_back(AddrPriority(addr, prio, src));
        return true;
    }
}

void
BOP::notifyFill(const PacketPtr& pkt)
{

}

} // namespace test
} // namespace prefetch
} // namespace gem5
