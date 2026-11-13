/*
 * Copyright (c) 2026
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/cache/replacement_policies/icp_rp.hh"

#include <algorithm>
#include <cassert>
#include <memory>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "params/ICPRP.hh"

namespace gem5
{
namespace replacement_policy
{

ICP::ICP(const Params &p)
    : Base(p), numSets(p.num_sets), numWays(p.num_ways),
      blockSize(p.block_size), pcEntries(p.pc_entries),
      counterMax(p.counter_bits > 0 && p.counter_bits <= 16 ?
                 (1U << p.counter_bits) - 1 : 0),
      threshold(p.accuracy_threshold), highPriority(p.high_priority),
      lowPriority(p.low_priority == -1 ? numWays - 1 : p.low_priority),
      demoteOnFirstUse(p.demote_on_first_use), adaptive(p.adaptive),
      sampleStride(p.sample_stride), epfAssoc(p.epf_assoc),
      epfSets(p.epf_assoc ? p.epf_entries / p.epf_assoc : 0),
      accuracy(NUM_PF_SOURCES * (std::min(pcEntries, 4096U) + 1)),
      sets(numSets), ghosts(p.epf_entries),
      setRoles(numSets, SetRole::Follower), stats(this)
{
    fatal_if(!numSets || !numWays, "ICP requires nonzero cache geometry");
    fatal_if(!isPowerOf2(blockSize), "ICP block size must be a power of 2");
    fatal_if(!counterMax || threshold > counterMax ||
             p.initial_accuracy > counterMax, "Invalid ICP accuracy counter");
    fatal_if(p.low_priority < -1 || highPriority > lowPriority ||
             lowPriority >= numWays,
             "ICP insertion ranks must satisfy high <= low < associativity");
    fatal_if(p.epf_entries && (!epfAssoc || !epfSets ||
             p.epf_entries % epfAssoc), "Invalid ICP EPF geometry");
    fatal_if(pcEntries > 4096 || (pcEntries && !isPowerOf2(pcEntries)),
             "ICP PC entries must be zero or a power of two up to 4096");
    fatal_if(adaptive && (!isPowerOf2(numSets) || sampleStride < 4 ||
             !isPowerOf2(sampleStride) || numSets < sampleStride),
             "ICP sampling requires power-of-two sets and stride >= 4");
    std::fill(accuracy.begin(), accuracy.end(), p.initial_accuracy);
    stats.selectorValue.functor([this] { return selector; });
    if (adaptive) {
        for (unsigned set = 0; set < numSets; ++set) {
            // This permutation preserves the number of leaders while
            // spreading them across regular address strides. Multiplication
            // by an odd constant and XOR shifts are invertible on set bits.
            unsigned mixed = set ^ (set >> 4);
            mixed = (mixed * 0x9e3779b1U) & (numSets - 1);
            mixed ^= mixed >> 5;
            const unsigned group = mixed & (sampleStride - 1);
            if (group == 0) {
                setRoles[set] = SetRole::LruLeader;
            } else if (group == sampleStride - 1) {
                setRoles[set] = SetRole::IcpLeader;
            }
        }
    }
}

bool
ICP::isPrefetch(const PacketPtr pkt)
{
    return pkt && (pkt->cmd.isHWPrefetch() || pkt->cmd.isSWPrefetch() ||
        (!pkt->isEviction() && pkt->req->isPrefetch()));
}

bool
ICP::isDemand(const PacketPtr pkt)
{
    return pkt && (pkt->isDemand() || pkt->isLLSC()) &&
        pkt->cmd != MemCmd::StorePFTrain &&
        !isPrefetch(pkt);
}

unsigned
ICP::sourceOf(const PacketPtr pkt) const
{
    const auto source = static_cast<unsigned>(pkt->req->getPFSource());
    return source < NUM_PF_SOURCES ? source : 0;
}

unsigned
ICP::predictorOf(const PacketPtr pkt) const
{
    const unsigned offset = sourceOf(pkt) * (pcEntries + 1);
    if (!pcEntries || !pkt->req->hasPC()) {
        return offset;
    }
    // Keep one source-only fallback counter separate from the PC buckets.
    const Addr pc = pkt->req->getPC();
    const unsigned bucket = ((pc >> 1) ^ (pc >> 11) ^ (pc >> 21)) &
                            (pcEntries - 1);
    return offset + 1 + bucket;
}

void
ICP::train(unsigned predictor, bool useful) const
{
    auto &counter = accuracy[predictor];
    if (useful) {
        counter = std::min(counter + 1, counterMax);
    } else if (counter) {
        --counter;
    }
}

bool
ICP::useIcp(unsigned set) const
{
    if (!adaptive || setRoles[set] == SetRole::IcpLeader) {
        return true;
    }
    return setRoles[set] == SetRole::Follower && selector >= SelectorThreshold;
}

void
ICP::trainSelector(unsigned set)
{
    if (!adaptive || setRoles[set] == SetRole::Follower) {
        return;
    }
    const bool previous = selector >= SelectorThreshold;
    if (setRoles[set] == SetRole::LruLeader) {
        selector = std::min(selector + 1, SelectorMax);
        ++stats.lruLeaderDemandFills;
    } else {
        if (selector) {
            --selector;
        }
        ++stats.icpLeaderDemandFills;
    }
    if (previous != (selector >= SelectorThreshold)) {
        ++stats.policySwitches;
    }
}

void
ICP::position(ICPReplData &data, unsigned rank) const
{
    // Remove the entry before inserting it at the requested stack position.
    unsigned valid = 0;
    for (auto *other : sets[data.set]) {
        if (other != &data && other->valid) {
            if (data.valid && other->rank > data.rank) {
                --other->rank;
            }
            ++valid;
        }
    }
    rank = std::min(rank, valid);
    for (auto *other : sets[data.set]) {
        if (other != &data && other->valid && other->rank >= rank) {
            ++other->rank;
        }
    }
    data.rank = rank;
    data.valid = true;
}

unsigned
ICP::ghostSet(Addr address, bool secure) const
{
    const Addr line = address / blockSize;
    return (line ^ (line >> 12) ^ static_cast<Addr>(secure)) % epfSets;
}

void
ICP::remember(const ICPReplData &data) const
{
    const unsigned start = ghostSet(data.address, data.secure) * epfAssoc;
    Ghost *victim = &ghosts[start];
    for (unsigned i = start; i < start + epfAssoc; ++i) {
        auto &ghost = ghosts[i];
        if (!ghost.valid) {
            victim = &ghost;
            break;
        }
        if (ghost.address == data.address && ghost.secure == data.secure) {
            victim = &ghost;
            break;
        }
        if (ghost.sequence < victim->sequence) {
            victim = &ghost;
        }
    }
    if (victim->valid) {
        train(victim->predictor, false);
        ++stats.epfReplacements;
    }
    *victim = {data.address, data.secure, true, data.source, data.predictor,
               ++sequence};
}

void
ICP::forget(Addr address, bool secure, bool demand) const
{
    if (!epfSets) {
        return;
    }
    const unsigned start = ghostSet(address, secure) * epfAssoc;
    for (unsigned i = start; i < start + epfAssoc; ++i) {
        auto &ghost = ghosts[i];
        if (ghost.valid && ghost.address == address && ghost.secure == secure) {
            if (demand) {
                train(ghost.predictor, true);
                ++stats.epfHits;
                ++stats.epfHitsBySource[ghost.source];
            } else {
                // A new residency makes the old evidence unresolved; it
                // does not demonstrate that the old prefetch was useless.
                ++stats.epfSuperseded;
            }
            ghost.valid = false;
            return;
        }
    }
}

void
ICP::notifyAccess(const PacketPtr pkt, bool hit)
{
    if (!hit && isDemand(pkt)) {
        forget(pkt->getBlockAddr(blockSize), pkt->isSecure(), true);
    }
}

void
ICP::notifyPrefetchFill(const std::shared_ptr<ReplacementData> &data,
                        bool demand_merged)
{
    auto &entry = *std::static_pointer_cast<ICPReplData>(data);
    if (entry.prefetched && demand_merged) {
        trainSelector(entry.set);
        train(entry.predictor, true);
        ++stats.usefulPrefetches;
        ++stats.usefulBySource[entry.source];
        entry.prefetched = false;
        position(entry, demoteOnFirstUse && useIcp(entry.set) ? numWays - 1 : 0);
    }
}

void
ICP::invalidate(const std::shared_ptr<ReplacementData> &data)
{
    auto &entry = *std::static_pointer_cast<ICPReplData>(data);
    if (!entry.valid) {
        return;
    }
    if (entry.prefetched) {
        ++stats.unusedPrefetches;
        if (entry.predictedLow && epfSets) {
            remember(entry);
        } else {
            train(entry.predictor, false);
        }
    }
    for (auto *other : sets[entry.set]) {
        if (other->valid && other->rank > entry.rank) {
            --other->rank;
        }
    }
    entry.valid = false;
    entry.prefetched = false;
}

void
ICP::touch(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    auto &entry = *std::static_pointer_cast<ICPReplData>(data);
    if (pkt->cmd == MemCmd::StorePFTrain) {
        return;
    }
    if (isPrefetch(pkt)) {
        if (!useIcp(entry.set) || accuracy[predictorOf(pkt)] >= threshold) {
            // An insertion rank must not demote a more recent line on a hit.
            position(entry, 0);
        } else {
            ++stats.ignoredPrefetchHits;
        }
        return;
    }
    if (entry.prefetched && isDemand(pkt)) {
        train(entry.predictor, true);
        ++stats.usefulPrefetches;
        ++stats.usefulBySource[entry.source];
        entry.prefetched = false;
        if (demoteOnFirstUse && useIcp(entry.set)) {
            position(entry, numWays - 1);
            return;
        }
    }
    position(entry, 0);
}

void
ICP::touch(const std::shared_ptr<ReplacementData> &data) const
{
    position(*std::static_pointer_cast<ICPReplData>(data), 0);
}

void
ICP::reset(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    auto &entry = *std::static_pointer_cast<ICPReplData>(data);
    // Tag allocation normally observes the returning read response, whereas
    // isDemand() classifies request commands. Eviction/writeback allocations
    // and prefetch responses must not train the policy selector.
    const bool demandFill = isDemand(pkt) ||
        (pkt->isResponse() && pkt->isRead() && !isPrefetch(pkt));
    if (demandFill) {
        trainSelector(entry.set);
    }
    entry.address = pkt->getBlockAddr(blockSize);
    entry.secure = pkt->isSecure();
    // A new residency supersedes the ghost record; never count it twice.
    forget(entry.address, entry.secure, false);
    entry.prefetched = isPrefetch(pkt);
    entry.predictedLow = false;
    unsigned rank = 0;
    if (entry.prefetched) {
        entry.source = sourceOf(pkt);
        entry.predictor = predictorOf(pkt);
        const bool icp = useIcp(entry.set);
        entry.predictedLow = icp && accuracy[entry.predictor] < threshold;
        rank = icp ? (entry.predictedLow ? lowPriority : highPriority) : 0;
        if (adaptive && setRoles[entry.set] == SetRole::Follower) {
            if (icp) {
                ++stats.followerIcpPrefetchFills;
            } else {
                ++stats.followerLruPrefetchFills;
            }
        }
        ++stats.prefetchFills;
        ++stats.fillsBySource[entry.source];
        if (pcEntries && pkt->req->hasPC()) {
            ++stats.pcPrefetchFills;
        } else {
            ++stats.fallbackPrefetchFills;
        }
        if (entry.predictedLow) {
            ++stats.lowPriorityFills;
            ++stats.lowFillsBySource[entry.source];
        } else {
            ++stats.highPriorityFills;
        }
    }
    position(entry, rank);
}

void
ICP::reset(const std::shared_ptr<ReplacementData> &data) const
{
    auto &entry = *std::static_pointer_cast<ICPReplData>(data);
    entry.prefetched = false;
    entry.predictedLow = false;
    position(entry, 0);
}

ReplaceableEntry *
ICP::getVictim(const ReplacementCandidates &candidates) const
{
    assert(!candidates.empty());
    ReplaceableEntry *victim = candidates.front();
    unsigned oldest = 0;
    for (auto *candidate : candidates) {
        const auto &entry = *std::static_pointer_cast<ICPReplData>(
            candidate->replacementData);
        if (!entry.valid) {
            return candidate;
        }
        if (entry.rank >= oldest) {
            oldest = entry.rank;
            victim = candidate;
        }
    }
    return victim;
}

std::shared_ptr<ReplacementData>
ICP::instantiateEntry()
{
    fatal_if(entryCount >= numSets * numWays,
             "ICP cache geometry does not match tag entries");
    auto entry = std::make_shared<ICPReplData>();
    entry->set = entryCount++ / numWays;
    sets[entry->set].push_back(entry.get());
    return entry;
}

ICP::ICPStats::ICPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(prefetchFills, "Prefetch block insertions"),
      ADD_STAT(pcPrefetchFills, "Prefetch fills using a source and PC counter"),
      ADD_STAT(fallbackPrefetchFills, "Prefetch fills using a source-only counter"),
      ADD_STAT(highPriorityFills, "Predicted-accurate prefetch insertions"),
      ADD_STAT(lowPriorityFills, "Predicted-inaccurate prefetch insertions"),
      ADD_STAT(usefulPrefetches, "First demand uses of resident prefetches"),
      ADD_STAT(unusedPrefetches, "Prefetches invalidated before demand use"),
      ADD_STAT(epfHits, "Demand misses to prematurely evicted prefetches"),
      ADD_STAT(epfReplacements, "Unused prefetch records displaced from EPF"),
      ADD_STAT(epfSuperseded, "Unresolved EPF records retired by a new residency"),
      ADD_STAT(ignoredPrefetchHits, "Low-confidence prefetch hits not promoted"),
      ADD_STAT(lruLeaderDemandFills, "Demand fills in LRU sampling sets"),
      ADD_STAT(icpLeaderDemandFills, "Demand fills in ICP sampling sets"),
      ADD_STAT(followerLruPrefetchFills, "Follower prefetches using LRU"),
      ADD_STAT(followerIcpPrefetchFills, "Follower prefetches using ICP"),
      ADD_STAT(policySwitches, "Changes in the follower policy selection"),
      ADD_STAT(selectorValue, "Current LRU/ICP policy selector value"),
      ADD_STAT(fillsBySource, "Prefetch fills by source"),
      ADD_STAT(lowFillsBySource, "Low-priority prefetch fills by source"),
      ADD_STAT(usefulBySource, "Useful resident prefetches by source"),
      ADD_STAT(epfHitsBySource, "EPF corrections by source")
{
    fillsBySource.init(NUM_PF_SOURCES);
    lowFillsBySource.init(NUM_PF_SOURCES);
    usefulBySource.init(NUM_PF_SOURCES);
    epfHitsBySource.init(NUM_PF_SOURCES);
    for (unsigned i = 0; i < NUM_PF_SOURCES; ++i) {
        const auto name = enums::PrefetchSourceTypeStrings[i];
        fillsBySource.subname(i, name);
        lowFillsBySource.subname(i, name);
        usefulBySource.subname(i, name);
        epfHitsBySource.subname(i, name);
    }
}

} // namespace replacement_policy
} // namespace gem5
