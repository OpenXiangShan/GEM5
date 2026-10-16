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
      ADD_STAT(l1Candidates, statistics::units::Count::get(), "MLOP candidates targeting L1"),
      ADD_STAT(l2Candidates, statistics::units::Count::get(), "MLOP candidates targeting L2"),
      ADD_STAT(llcCandidates, statistics::units::Count::get(), "MLOP candidates targeting LLC"),
      ADD_STAT(l1Accepted, statistics::units::Count::get(), "MLOP L1 candidates accepted by a prefetch queue"),
      ADD_STAT(l2Accepted, statistics::units::Count::get(), "MLOP L2 candidates accepted by a prefetch queue"),
      ADD_STAT(llcAccepted, statistics::units::Count::get(), "MLOP LLC candidates handed to a downstream prefetcher"),
      ADD_STAT(downstreamUnavailable, statistics::units::Count::get(),
               "MLOP downstream candidates without a downstream prefetcher")
{
}

MLOPPrefetcher::MLOPPrefetcher(const MLOPPrefetcherParams &p)
    : WorkerPrefetcher(p),
      statsMLOP(this),
      maxZones(p.max_zones),
      attachedLevel(p.attached_level),
      updatesPerRound(p.num_updates),
      l1Threshold(p.l1_threshold),
      l2Threshold(p.l2_threshold),
      llcThreshold(p.llc_threshold)
{
    fatal_if(maxZones == 0 || attachedLevel < 1 || attachedLevel > 2 || updatesPerRound == 0 ||
                 updatesPerRound > std::numeric_limits<uint16_t>::max() || l1Threshold == 0 || l2Threshold == 0 ||
                 llcThreshold == 0,
             "MLOP requires positive capacity, level, round, and threshold "
             "parameters");
    if (attachedLevel == 1) {
        fatal_if(l1Threshold < l2Threshold, "L1 MLOP requires l1_threshold >= l2_threshold");
    }

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
    zone.blockPrefetchLevel.fill(0);
    zone.history.fill(0);
    zone.historySize = 0;
}

void
MLOPPrefetcher::mark(uint64_t block, State state, uint8_t level, bool secure)
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
    zone->blockPrefetchLevel[index] = state == State::Prefetch ? level : 0;

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
    prefetchLevel.fill(0);
    for (auto &offsets : prefetchOffsets) {
        offsets.clear();
    }

    std::array<uint16_t, PrefetchDegree> maxScores{};
    for (std::size_t depth = 0; depth < PrefetchDegree; ++depth) {
        maxScores[depth] = *std::max_element(offsetScores[depth].begin(), offsetScores[depth].end());
    }

    std::array<bool, NumOffsets> selected{};
    for (int depth = static_cast<int>(PrefetchDegree) - 1; depth >= 0; --depth) {
        uint8_t level = 0;
        if (attachedLevel == 1) {
            if (maxScores[depth] >= l1Threshold) {
                level = 1;
            } else if (maxScores[depth] >= l2Threshold) {
                level = 2;
            } else if (maxScores[depth] >= llcThreshold) {
                level = 3;
            }
        } else {
            if (maxScores[depth] >= l2Threshold) {
                level = 2;
            } else if (maxScores[depth] >= llcThreshold) {
                level = 3;
            }
        }

        if (level == 0)
            continue;

        prefetchLevel[depth] = level;
        for (int offset = -static_cast<int>(BlocksInZone - 1); offset <= static_cast<int>(BlocksInZone - 1);
             ++offset) {
            if (offset == 0) {
                continue;
            }
            const auto index = static_cast<unsigned>(BlocksInZone - 1 + offset);
            if (!selected[index] && offsetScores[depth][index] == maxScores[depth]) {
                prefetchOffsets[depth].push_back(offset);
                selected[index] = true;
            }
        }
    }
    offsetScores = {};
}

std::vector<std::pair<uint64_t, uint8_t>>
MLOPPrefetcher::candidates(uint64_t block, bool secure) const
{
    const ZoneKey key{block / BlocksInZone, secure};
    const auto *zone = findZone(key);
    if (zone == nullptr) {
        return {};
    }

    const auto origin = static_cast<int>(block % BlocksInZone);
    std::vector<std::pair<uint64_t, uint8_t>> result;
    for (unsigned depth = 0; depth < PrefetchDegree; ++depth) {
        for (const int offset : prefetchOffsets[depth]) {
            const int target = origin + offset;
            if (target < 0 || target >= static_cast<int>(BlocksInZone)) {
                continue;
            }
            const auto index = static_cast<unsigned>(target);
            if (zone->accessMap[index] == State::Access || (zone->accessMap[index] == State::Prefetch &&
                                                            zone->blockPrefetchLevel[index] <= prefetchLevel[depth])) {
                continue;
            }
            result.emplace_back(key.number * BlocksInZone + index, prefetchLevel[depth]);
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

    // MLOP is an address-space prefetcher. Keep the physical address in the
    // copy passed to Queued even when the surrounding cache uses VAs.
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
    mark(addr / blkSize, State::Init, 0, is_secure);
}

void
MLOPPrefetcher::calculatePrefetch(const PrefetchInfo &pfi, std::vector<AddrPriority> &addresses)
{
    const auto block = pfi.getAddr() / blkSize;
    const auto secure = pfi.isSecure();

    const auto beforeTraining = trainingCount;
    const auto beforeRound = roundCount;
    const auto beforeEviction = zoneEvictionCount;

    // DPC-3 updates scores only on a miss or on the first hit of a useful
    // prefetch. Every LOAD still enters the access map afterwards.
    if (pfi.isCacheMiss() || pfi.isPfFirstHit()) {
        learn(block, secure);
    }
    mark(block, State::Access, 0, secure);

    const auto generated = candidates(block, secure);
    for (const auto &candidate : generated) {
        const auto candidateBlock = candidate.first;
        const auto level = candidate.second;
        const bool local = level == attachedLevel;
        if (!local && !hasHintDownStream()) {
            ++statsMLOP.downstreamUnavailable;
            continue;
        }

        switch (level) {
            case 1:
                ++statsMLOP.l1Candidates;
                break;
            case 2:
                ++statsMLOP.l2Candidates;
                break;
            case 3:
                ++statsMLOP.llcCandidates;
                break;
            default:
                panic("MLOP generated an invalid fill level");
        }

        const Addr candidateAddr = candidateBlock * blkSize;
        if (usePFBuffer) {
            const bool alreadyBuffered =
                std::any_of(PFRequestBuffer.begin(), PFRequestBuffer.end(), [&](const AddrPriority &pending) {
                    return pending.addr == candidateAddr && pending.pfahead_host == (local ? 0 : level) &&
                           pending.pf_trigger_info.pfi_old && pending.pf_trigger_info.pfi_old->isSecure() == secure;
                });
            if (alreadyBuffered) {
                continue;
            }
        }

        AddrPriority address(candidateAddr, 0, PrefetchSourceType::MLOP);
        if (!local) {
            address.pfahead = true;
            address.pfahead_host = level;
        }
        if (usePFBuffer) {
            address.pf_trigger_info = pfi.trigger_info;
            InsertPFRequestToBuffer(address);
        } else {
            addresses.push_back(address);
        }
    }

    statsMLOP.trainings += trainingCount - beforeTraining;
    statsMLOP.rounds += roundCount - beforeRound;
    statsMLOP.zoneEvictions += zoneEvictionCount - beforeEviction;
}

void
MLOPPrefetcher::onPrefetchAccepted(const DeferredPacket &dpp)
{
    if (dpp.owner != this || dpp.pfInfo.getXsMetadata().prefetchSource != PrefetchSourceType::MLOP) {
        return;
    }

    const uint8_t level = dpp.pfahead ? dpp.pfahead_host : attachedLevel;
    mark(dpp.pfInfo.getAddr() / blkSize, State::Prefetch, level, dpp.pfInfo.isSecure());
    switch (level) {
        case 1:
            ++statsMLOP.l1Accepted;
            break;
        case 2:
            ++statsMLOP.l2Accepted;
            break;
        case 3:
            ++statsMLOP.llcAccepted;
            break;
        default:
            panic("MLOP accepted an invalid fill level");
    }
}

void
MLOPPrefetcher::onPrefetchRemoved(const DeferredPacket &dpp)
{
    if (dpp.owner != this || dpp.pfInfo.getXsMetadata().prefetchSource != PrefetchSourceType::MLOP) {
        return;
    }

    const uint8_t level = dpp.pfahead ? dpp.pfahead_host : attachedLevel;
    mark(dpp.pfInfo.getAddr() / blkSize, State::Init, level, dpp.pfInfo.isSecure());
}

}  // namespace prefetch
}  // namespace gem5
