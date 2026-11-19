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

#include "mem/cache/prefetch/mlop.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "params/MLOPPrefetcher.hh"

namespace gem5
{
namespace prefetch
{

MLOPPrefetcher::MLOPStats::MLOPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(trainings, statistics::units::Count::get(), "MLOP accesses that update offset scores"),
      ADD_STAT(rounds, statistics::units::Count::get(), "MLOP score selection rounds"),
      ADD_STAT(zoneEvictions, statistics::units::Count::get(), "MLOP AMT zone replacements"),
      ADD_STAT(l2Candidates, statistics::units::Count::get(), "MLOP candidates targeting L2"),
      ADD_STAT(crossPageDropped, statistics::units::Count::get(), "MLOP candidates dropped at a zone/page boundary"),
      ADD_STAT(l2SubmittedOffsetDist, statistics::units::Count::get(), "Offsets submitted by MLOP for L2 prefetching"),
      ADD_STAT(l2Accepted, statistics::units::Count::get(), "MLOP L2 candidates accepted by a prefetch queue")
{
    l2SubmittedOffsetDist.init(-63, 63, 1).prereq(l2SubmittedOffsetDist);
}

MLOPPrefetcher::MLOPPrefetcher(const MLOPPrefetcherParams &p)
    : WorkerPrefetcher(p),
      statsMLOP(this),
      maxZones(p.max_zones),
      updatesPerRound(p.num_updates),
      l2Threshold(p.l2_threshold)
{
    fatal_if(maxZones == 0 || updatesPerRound == 0 || updatesPerRound > std::numeric_limits<uint16_t>::max() ||
                 l2Threshold == 0,
             "MLOP requires positive capacity, round and threshold "
             "parameters and at most 65535 updates per round");

    numSets = std::max<std::size_t>(1, (maxZones + AmtWays - 1) / AmtWays);
    amt.resize(maxZones);
}

void
MLOPPrefetcher::setParentInfo(System *sys, ProbeManager *pm, CacheAccessor *cache, unsigned blk_size)
{
    WorkerPrefetcher::setParentInfo(sys, pm, cache, blk_size);
    fatal_if(blkSize != 64 || pageBytes != 4096, "MLOP requires 64-byte blocks and 4-KiB pages");
}

std::size_t
MLOPPrefetcher::setIndex(const ZoneKey &key) const
{
    if (numSets == 1) {
        return 0;
    }

    // DPC-3 folds the zone tag into the set index. Include the secure bit in
    // the same tag so the two GEM5 address spaces cannot alias in the AMT.
    const uint64_t value = key.number ^ (key.secure ? 1ULL : 0ULL);
    unsigned indexBits = 0;
    for (std::size_t sets = 1; sets < numSets; sets <<= 1) {
        ++indexBits;
    }
    const uint64_t mask = (uint64_t{1} << indexBits) - 1;
    uint64_t folded = value;
    for (uint64_t upper = value >> indexBits; upper != 0; upper >>= indexBits) {
        folded ^= upper & mask;
    }
    return static_cast<std::size_t>((folded & mask) % numSets);
}

MLOPPrefetcher::Zone *
MLOPPrefetcher::findZone(const ZoneKey &key)
{
    const auto set = setIndex(key);
    const auto begin = set * AmtWays;
    const auto end = std::min<std::size_t>(begin + AmtWays, amt.size());
    for (std::size_t i = begin; i < end; ++i) {
        if (amt[i].valid && amt[i].key == key) {
            return &amt[i];
        }
    }
    return nullptr;
}

const MLOPPrefetcher::Zone *
MLOPPrefetcher::findZone(const ZoneKey &key) const
{
    const auto set = setIndex(key);
    const auto begin = set * AmtWays;
    const auto end = std::min<std::size_t>(begin + AmtWays, amt.size());
    for (std::size_t i = begin; i < end; ++i) {
        if (amt[i].valid && amt[i].key == key) {
            return &amt[i];
        }
    }
    return nullptr;
}

MLOPPrefetcher::Zone &
MLOPPrefetcher::getZone(const ZoneKey &key)
{
    if (auto *zone = findZone(key)) {
        return *zone;
    }

    const auto set = setIndex(key);
    const auto begin = set * AmtWays;
    const auto end = std::min<std::size_t>(begin + AmtWays, amt.size());
    auto victim = begin;
    for (std::size_t i = begin; i < end; ++i) {
        if (!amt[i].valid) {
            victim = i;
            break;
        }
        if (amt[i].lru < amt[victim].lru) {
            victim = i;
        }
    }

    if (amt[victim].valid) {
        ++zoneEvictionCount;
    }
    amt[victim] = Zone{};
    amt[victim].valid = true;
    amt[victim].key = key;
    amt[victim].lru = ++lruClock;
    return amt[victim];
}

void
MLOPPrefetcher::touchZone(Zone &zone)
{
    zone.lru = ++lruClock;
}

void
MLOPPrefetcher::eraseZone(Zone &zone)
{
    zone.valid = false;
    zone.accessMap.fill(State::Init);
    zone.history.fill(0);
    zone.historySize = 0;
}

void
MLOPPrefetcher::mark(uint64_t block, State state, bool secure)
{
    const ZoneKey key{block / BlocksInZone, secure};
    const auto index = static_cast<unsigned>(block % BlocksInZone);

    Zone *zone = findZone(key);
    if (state == State::Init) {
        if (zone == nullptr) {
            return;
        }
    } else if (zone == nullptr) {
        zone = &getZone(key);
    }

    if (state == State::Access) {
        touchZone(*zone);
        const auto oldSize = static_cast<unsigned>(zone->historySize);
        const auto newSize = std::min<unsigned>(oldSize + 1, HistoryLength);
        for (unsigned i = newSize; i > 1; --i) {
            zone->history[i - 1] = zone->history[i - 2];
        }
        zone->history[0] = static_cast<uint8_t>(index);
        zone->historySize = static_cast<uint8_t>(newSize);
    }

    zone->accessMap[index] = state;

    if (state == State::Init && std::all_of(zone->accessMap.begin(), zone->accessMap.end(),
                                            [](State value) { return value == State::Init; })) {
        eraseZone(*zone);
    }
}

void
MLOPPrefetcher::learn(uint64_t block, bool secure)
{
    const ZoneKey key{block / BlocksInZone, secure};
    auto *zone = findZone(key);
    if (zone == nullptr) {
        return;
    }

    const auto target = static_cast<int>(block % BlocksInZone);
    if (zone->accessMap[target] == State::Access) {
        return;
    }

    ++trainingCount;
    ++updateCount;
    auto accessMap = zone->accessMap;
    for (std::size_t depth = 0; depth <= zone->historySize; ++depth) {
        if (depth != 0) {
            accessMap[zone->history[depth - 1]] = State::Init;
        }
        for (int index = 0; index < static_cast<int>(BlocksInZone); ++index) {
            if (accessMap[index] != State::Access) {
                continue;
            }
            const auto offset = target - index;
            if (offset != 0 && offset >= -static_cast<int>(BlocksInZone - 1) &&
                offset <= static_cast<int>(BlocksInZone - 1)) {
                ++offsetScores[depth][BlocksInZone - 1 + offset];
            }
        }
    }

    if (updateCount == updatesPerRound) {
        finishRound();
    }
}

void
MLOPPrefetcher::finishRound()
{
    updateCount = 0;
    ++roundCount;
    for (auto &offsets : prefetchOffsets) {
        offsets.clear();
    }

    std::array<bool, NumOffsets> selected{};
    for (int depth = static_cast<int>(PrefetchDegree) - 1; depth >= 0; --depth) {
        const auto &scores = offsetScores[depth];
        const auto maxScore = *std::max_element(scores.begin(), scores.end());
        if (maxScore <= l2Threshold) {
            continue;
        }

        for (int offset = -static_cast<int>(BlocksInZone - 1); offset <= static_cast<int>(BlocksInZone - 1);
             ++offset) {
            if (offset == 0) {
                continue;
            }
            const auto index = static_cast<unsigned>(BlocksInZone - 1 + offset);
            if (!selected[index] && scores[index] == maxScore) {
                prefetchOffsets[depth].push_back(offset);
                selected[index] = true;
            }
        }
    }
    offsetScores = {};
}

std::vector<uint64_t>
MLOPPrefetcher::candidates(uint64_t block, bool secure)
{
    const ZoneKey key{block / BlocksInZone, secure};
    const auto *zone = findZone(key);
    if (zone == nullptr) {
        return {};
    }

    const auto origin = static_cast<int>(block % BlocksInZone);
    std::vector<uint64_t> result;
    for (const auto &offsets : prefetchOffsets) {
        for (const int offset : offsets) {
            const int target = origin + offset;
            if (target < 0 || target >= static_cast<int>(BlocksInZone)) {
                ++statsMLOP.crossPageDropped;
                continue;
            }
            const auto index = static_cast<unsigned>(target);
            if (zone->accessMap[index] == State::Init) {
                result.push_back(key.number * BlocksInZone + index);
            }
        }
    }
    return result;
}

void
MLOPPrefetcher::notify(const PacketPtr &pkt, const PrefetchInfo &pfi)
{
    if (pfi.isWrite() || pfi.isStore()) {
        return;
    }

    if (useVirtualAddresses) {
        Queued::notify(pkt, pfi);
        return;
    }

    // Base can supply a virtual address even for a PA-based prefetcher.
    PrefetchInfo physical(pfi, pfi.getPaddr());
    physical.setXsMetadata(pfi.getXsMetadata());
    physical.setPfFirstHit(pfi.isPfFirstHit());
    physical.setPfHit(pfi.isPfHit());
    physical.setEverPrefetched(pfi.isEverPrefetched());
    physical.setReqAfterSquash(pfi.isReqAfterSquash());
    Queued::notify(pkt, physical);
}

void
MLOPPrefetcher::notifyBlockInvalidated(Addr addr, bool is_secure)
{
    mark(addr / blkSize, State::Init, is_secure);
}

void
MLOPPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    if (pfi.isWrite() || pfi.isStore()) {
        return;
    }

    const auto block = pfi.getPaddr() / blkSize;
    const auto secure = pfi.isSecure();

    const auto beforeTraining = trainingCount;
    const auto beforeRound = roundCount;
    const auto beforeEviction = zoneEvictionCount;

    // DPC-3 updates scores only on a miss or on the first hit of a useful
    // prefetch. Every LOAD still enters the access map afterwards.
    if (pfi.isCacheMiss() || pfi.isPfFirstHit()) {
        learn(block, secure);
    }
    mark(block, State::Access, secure);

    for (const auto candidateBlock : candidates(block, secure)) {
        ++statsMLOP.l2Candidates;
        const Addr candidatePaddr = candidateBlock * blkSize;
        // The L2 composite issues VAs, but MLOP always learns physical zones.
        // Its candidates stay in the trigger's physical page.
        const Addr candidateAddr =
            useVirtualAddresses ? pageAddress(pfi.getAddr()) | pageOffset(candidatePaddr) : candidatePaddr;
        const auto offset = static_cast<int64_t>(candidateBlock) - static_cast<int64_t>(block);
        if (usePFBuffer) {
            const bool alreadyBuffered =
                std::any_of(PFRequestBuffer.begin(), PFRequestBuffer.end(), [&](const AddrPriority &pending) {
                    const auto &trigger = pending.pf_trigger_info.pfi_old;
                    const Addr pendingPaddr = useVirtualAddresses
                                                  ? pageAddress(trigger->getPaddr()) | pageOffset(pending.addr)
                                                  : pending.addr;
                    return pendingPaddr == candidatePaddr && trigger->isSecure() == secure;
                });
            if (alreadyBuffered) {
                continue;
            }
        }

        AddrPriority address(candidateAddr, 0, PrefetchSourceType::MLOP);
        if (usePFBuffer) {
            address.pf_trigger_info = pfi.trigger_info;
            InsertPFRequestToBuffer(address);
        } else {
            addresses.push_back(address);
        }
        statsMLOP.l2SubmittedOffsetDist.sample(offset);
    }

    statsMLOP.trainings += trainingCount - beforeTraining;
    statsMLOP.rounds += roundCount - beforeRound;
    statsMLOP.zoneEvictions += zoneEvictionCount - beforeEviction;
}

void
MLOPPrefetcher::onPrefetchAccepted(const DeferredPacket &dpp)
{
    if (dpp.owner == this && dpp.pkt && dpp.pfInfo.getXsMetadata().prefetchSource == PrefetchSourceType::MLOP) {
        recordAccepted(dpp.pkt->getAddr(), dpp.pfInfo.isSecure());
    }
}

void
MLOPPrefetcher::recordAccepted(Addr paddr, bool secure)
{
    mark(paddr / blkSize, State::Prefetch, secure);
    ++statsMLOP.l2Accepted;
}

void
MLOPPrefetcher::onPrefetchRemoved(const DeferredPacket &dpp)
{
    if (dpp.owner == this && dpp.pkt && dpp.pfInfo.getXsMetadata().prefetchSource == PrefetchSourceType::MLOP) {
        recordRemoved(dpp.pkt->getAddr(), dpp.pfInfo.isSecure());
    }
}

void
MLOPPrefetcher::recordRemoved(Addr paddr, bool secure)
{
    const auto block = paddr / blkSize;
    const auto index = block % BlocksInZone;
    const auto *zone = findZone(ZoneKey{block / BlocksInZone, secure});
    // An intervening demand can supersede the queued prefetch.
    if (zone && zone->accessMap[index] == State::Prefetch) {
        mark(block, State::Init, secure);
    }
}

}  // namespace prefetch
}  // namespace gem5
