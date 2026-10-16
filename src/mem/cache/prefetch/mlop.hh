/*
 * Copyright (c) 2021 SAFARI Research Group at ETH Zurich University
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef __MEM_CACHE_PREFETCH_MLOP_HH__
#define __MEM_CACHE_PREFETCH_MLOP_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "mem/cache/prefetch/worker.hh"

namespace gem5
{

struct MLOPPrefetcherParams;

namespace prefetch
{

/**
 * Multi-Lookahead Offset Prefetcher.
 *
 * The state layout follows the DPC-3 implementation: an access-map table
 * tracks 64-block zones, each zone retains the last 15 accesses, and one
 * score vector is maintained for every one of 16 lookahead levels.
 */
class MLOPPrefetcher : public WorkerPrefetcher
{
  private:
    static constexpr unsigned BlocksInZone = 64;
    static constexpr unsigned PrefetchDegree = 16;
    static constexpr unsigned HistoryLength = PrefetchDegree - 1;
    static constexpr unsigned NumOffsets = 2 * BlocksInZone - 1;
    static constexpr unsigned AmtWays = 16;
    static constexpr unsigned DefaultAmtEntries = 256;

    enum class State : uint8_t { Init, Access, Prefetch };

    struct ZoneKey
    {
        uint64_t number = 0;
        bool secure = false;

        bool operator==(const ZoneKey &other) const { return number == other.number && secure == other.secure; }
    };

    struct Zone
    {
        bool valid = false;
        ZoneKey key;
        // Keep the state and target level explicit. This is easier to inspect
        // in the software model than a packed hardware-style bit array.
        std::array<State, BlocksInZone> accessMap{};
        std::array<uint8_t, BlocksInZone> blockPrefetchLevel{};
        std::array<uint8_t, HistoryLength> history{};
        uint8_t historySize = 0;
        uint64_t lru = 0;
    };

    struct MLOPStats : public statistics::Group
    {
        explicit MLOPStats(statistics::Group *parent);

        statistics::Scalar trainings;
        statistics::Scalar rounds;
        statistics::Scalar zoneEvictions;
        statistics::Scalar l1Candidates;
        statistics::Scalar l2Candidates;
        statistics::Scalar llcCandidates;
        statistics::Scalar l1Accepted;
        statistics::Scalar l2Accepted;
        statistics::Scalar llcAccepted;
        statistics::Scalar downstreamUnavailable;
    } statsMLOP;

    std::vector<Zone> amt;
    std::size_t numSets = 1;
    uint64_t lruClock = 0;

    std::array<std::array<uint16_t, NumOffsets>, PrefetchDegree> offsetScores{};
    std::array<std::vector<int>, PrefetchDegree> prefetchOffsets;
    std::array<uint8_t, PrefetchDegree> prefetchLevel{};

    unsigned maxZones = DefaultAmtEntries;
    unsigned attachedLevel = 2;
    unsigned updatesPerRound = 500;
    unsigned l1Threshold = 200;
    unsigned l2Threshold = 200;
    unsigned llcThreshold = 150;
    unsigned updateCount = 0;
    uint64_t trainingCount = 0;
    uint64_t roundCount = 0;
    uint64_t zoneEvictionCount = 0;

    std::size_t setIndex(const ZoneKey &key) const;
    Zone *findZone(const ZoneKey &key);
    const Zone *findZone(const ZoneKey &key) const;
    Zone &getZone(const ZoneKey &key);
    void touchZone(Zone &zone);
    void eraseZone(Zone &zone);
    void mark(uint64_t block, State state, uint8_t level, bool secure);
    void learn(uint64_t block, bool secure);
    void finishRound();
    std::vector<std::pair<uint64_t, uint8_t>> candidates(uint64_t block, bool secure) const;

  protected:
    void onPrefetchAccepted(const DeferredPacket &dpp) override;
    void onPrefetchRemoved(const DeferredPacket &dpp) override;

  public:
    explicit MLOPPrefetcher(const MLOPPrefetcherParams &p);

    void setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *cache, unsigned blk_size) override;
    void notify(const PacketPtr &pkt, const PrefetchInfo &pfi) override;
    void notifyBlockInvalidated(Addr addr, bool is_secure) override;
    void calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses) override;
};

}  // namespace prefetch
}  // namespace gem5

#endif  // __MEM_CACHE_PREFETCH_MLOP_HH__
