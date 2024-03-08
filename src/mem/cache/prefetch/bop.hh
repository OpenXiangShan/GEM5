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

/**
 * Implementation of the 'A Best-Offset Prefetcher'
 * Reference:
 *   Michaud, P. (2015, June). A best-offset prefetcher.
 *   In 2nd Data Prefetching Championship.
 */

#ifndef __MEM_CACHE_PREFETCH_BOP_HH__
#define __MEM_CACHE_PREFETCH_BOP_HH__

#include <queue>
#include <set>
#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct BOPPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class BOP : public Queued
{
    private:

        enum RRWay
        {
            Left,
            Right
        };

        /** Learning phase parameters */
        const unsigned int scoreMax;
        const unsigned int roundMax;
        const unsigned int badScore;
        /** Recent requests table parameteres */
        const unsigned int rrEntries;
        const unsigned int tagMask;
        /** Delay queue parameters */
        const bool         delayQueueEnabled;
        const unsigned int delayQueueSize;
        const unsigned int delayTicks;

        const int victimListSize;
        const int restoreCycle;

        bool victimRestoreScheduled = false;
        Event *restore_event;

        struct RREntryDebug
        {
            Addr fullAddr;
            Addr hashAddr;

            RREntryDebug(Addr full_addr, Addr hash_addr) : fullAddr(full_addr), hashAddr(hash_addr) {}
            RREntryDebug() : fullAddr(0), hashAddr(0) {}
        };

        std::vector<RREntryDebug> rrLeft;
        std::vector<RREntryDebug> rrRight;

        /** Structure to save the offset and the score */
        // typedef std::pair<int16_t, uint8_t> OffsetListEntry;
        struct OffsetListEntry{
            int32_t offset;  // offset, name it as first to make it compatible with pair
            uint8_t score;  // score, name it as second to make it compatible with pair
            int16_t depth;
            SatCounter8 late;

            OffsetListEntry(int32_t x, uint8_t y)
                : offset(x), score(y), depth(1), late(6, 32)
            {}

            int64_t calcOffset() const
            {
                assert(offset != 0);
                return offset * depth;
            }

            bool operator==(const int64_t t){
                return offset == t;
            }
        };
        std::vector<int> originOffsets;
        std::list<OffsetListEntry> offsetsList;
        std::list<int> victimOffsetsList;

        size_t maxOffsetCount{32};

        // std::set<int32_t> offsets;

        /** In a first implementation of the BO prefetcher, both banks of the
         *  RR were written simultaneously when a prefetched line is inserted
         *  into the cache. Adding the delay queue tries to avoid always
         *  striving for timeless prefetches, which has been found to not
         *  always being optimal.
         */
        struct DelayQueueEntry
        {
            RREntryDebug rrEntry;
            Tick processTick;

            DelayQueueEntry(const RREntryDebug &other, Tick t) : rrEntry(other), processTick(t)
            {}
        };

        std::deque<DelayQueueEntry> delayQueue;

        /** Event to handle the delay queue processing */
        void delayQueueEventWrapper();
        EventFunctionWrapper delayQueueEvent;

        /** Hardware prefetcher enabled */
        bool issuePrefetchRequests;
        /** Current best offset to issue prefetches */
        int64_t bestOffset;
        /** Current best offset found in the learning phase */
        int64_t phaseBestOffset;
        /** Current test offset index */
        std::list<OffsetListEntry>::iterator offsetsListIterator;

        std::list<OffsetListEntry>::iterator bestoffsetsListIterator;

        /** Max score found so far */
        unsigned int bestScore;
        /** Current round */
        unsigned int round;

        std::list<OffsetListEntry>::iterator getBestOffsetIter();

        /** Generate a hash for the specified address to index the RR table
         *  @param addr: address to hash
         *  @param way:  RR table to which is addressed (left/right)
         */
        unsigned int hash(Addr addr, unsigned int way) const;

        /** Insert the specified address into the RR table
         *  @param addr: full address to insert
         *  @param tag: hashed address to insert
         *  @param way: RR table to which the address will be inserted
         */
        void insertIntoRR(Addr full_addr, Addr tag, unsigned int way);

        /** Insert the specified address into the RR table
         *  @param rr_entry: rr_entry to insert
         *  @param way: RR table to which the address will be inserted
         */
        void insertIntoRR(RREntryDebug rr_entry, unsigned int way);

        /** Insert the specified address into the delay queue. This will
         *  trigger an event after the delay cycles pass
         *  @param addr: full address to insert
         *  @param tag: hashed address to insert
         */
        void insertIntoDelayQueue(Addr full_addr, Addr tag);

        /** Reset all the scores from the offset list */
        void resetScores();

        /** Generate the tag for the specified address based on the tag bits
         *  and the block size
         *  @param addr: address to get the tag from
        */
        Addr tag(Addr addr) const;

        /** Test if @X-O is hitting in the RR table to update the
            offset score */
        std::pair<bool, RREntryDebug> testRR(Addr tag) const;

        /** Learning phase of the BOP. Update the intermediate values of the
            round and update the best offset if found */
        bool bestOffsetLearning(Addr hashed_addr, bool late, const PrefetchInfo &pfi);

        unsigned missCount{0};

        bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr, std::vector<AddrPriority> &addresses, int prio,
                              PrefetchSourceType src);

        struct BopStats : public statistics::Group
        {
            BopStats(statistics::Group *parent);
            statistics::Distribution issuedOffsetDist;
            statistics::Scalar learnOffsetCount;
            statistics::Scalar throttledCount;
        } stats;

    public:
        boost::compute::detail::lru_cache<Addr, Addr> *filter;

        /** Update the RR right table after a prefetch fill */
        void notifyFill(const PacketPtr& pkt) override;

        BOP(const BOPPrefetcherParams &p);
        ~BOP() = default;

        void calculatePrefetch(const PrefetchInfo &pfi,
                               std::vector<AddrPriority> &addresses) override
        {
            panic("not implemented");
        };

        using Queued::calculatePrefetch;

        void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses, bool late);
        
        bool tryAddOffset(int64_t offset, bool late = false);
};

} // namespace prefetch
} // namespace gem5

#endif /* __MEM_CACHE_PREFETCH_BOP_HH__ */
