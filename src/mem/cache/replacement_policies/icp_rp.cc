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
      blockSize(p.block_size),
      counterMax(p.counter_bits > 0 && p.counter_bits <= 16 ?
                 (1U << p.counter_bits) - 1 : 0),
      threshold(p.accuracy_threshold), highPriority(p.high_priority),
      demoteOnFirstUse(p.demote_on_first_use), epfAssoc(p.epf_assoc),
      epfSets(p.epf_assoc ? p.epf_entries / p.epf_assoc : 0),
      sets(numSets), ghosts(p.epf_entries), stats(this)
{
    fatal_if(!numSets || !numWays, "ICP requires nonzero cache geometry");
    fatal_if(!isPowerOf2(blockSize), "ICP block size must be a power of 2");
    fatal_if(!counterMax || threshold > counterMax ||
             p.initial_accuracy > counterMax, "Invalid ICP accuracy counter");
    fatal_if(highPriority >= numWays, "ICP insertion rank exceeds associativity");
    fatal_if(p.epf_entries && (!epfAssoc || !epfSets ||
             p.epf_entries % epfAssoc), "Invalid ICP EPF geometry");
    accuracy.fill(p.initial_accuracy);
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

void
ICP::train(unsigned source, bool useful) const
{
    auto &counter = accuracy[source];
    if (useful) {
        counter = std::min(counter + 1, counterMax);
    } else if (counter) {
        --counter;
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
        train(victim->source, false);
        ++stats.epfReplacements;
    }
    *victim = {data.address, data.secure, true, data.source, ++sequence};
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
            train(ghost.source, demand);
            if (demand) {
                ++stats.epfHits;
                ++stats.epfHitsBySource[ghost.source];
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
        train(entry.source, true);
        ++stats.usefulPrefetches;
        ++stats.usefulBySource[entry.source];
        entry.prefetched = false;
        position(entry, demoteOnFirstUse ? numWays - 1 : 0);
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
            train(entry.source, false);
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
        if (accuracy[sourceOf(pkt)] >= threshold) {
            position(entry, highPriority);
        } else {
            ++stats.ignoredPrefetchHits;
        }
        return;
    }
    if (entry.prefetched && isDemand(pkt)) {
        train(entry.source, true);
        ++stats.usefulPrefetches;
        ++stats.usefulBySource[entry.source];
        entry.prefetched = false;
        if (demoteOnFirstUse) {
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
    entry.address = pkt->getBlockAddr(blockSize);
    entry.secure = pkt->isSecure();
    // A new residency supersedes the ghost record; never count it twice.
    forget(entry.address, entry.secure, false);
    entry.prefetched = isPrefetch(pkt);
    entry.predictedLow = false;
    unsigned rank = 0;
    if (entry.prefetched) {
        entry.source = sourceOf(pkt);
        entry.predictedLow = accuracy[entry.source] < threshold;
        rank = entry.predictedLow ? numWays - 1 : highPriority;
        ++stats.prefetchFills;
        ++stats.fillsBySource[entry.source];
        if (entry.predictedLow) {
            ++stats.lowPriorityFills;
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
      ADD_STAT(highPriorityFills, "Predicted-accurate prefetch insertions"),
      ADD_STAT(lowPriorityFills, "Predicted-inaccurate prefetch insertions"),
      ADD_STAT(usefulPrefetches, "First demand uses of resident prefetches"),
      ADD_STAT(unusedPrefetches, "Prefetches invalidated before demand use"),
      ADD_STAT(epfHits, "Demand misses to prematurely evicted prefetches"),
      ADD_STAT(epfReplacements, "Unused prefetch records displaced from EPF"),
      ADD_STAT(ignoredPrefetchHits, "Low-confidence prefetch hits not promoted"),
      ADD_STAT(fillsBySource, "Prefetch fills by source"),
      ADD_STAT(usefulBySource, "Useful resident prefetches by source"),
      ADD_STAT(epfHitsBySource, "EPF corrections by source")
{
    fillsBySource.init(NUM_PF_SOURCES);
    usefulBySource.init(NUM_PF_SOURCES);
    epfHitsBySource.init(NUM_PF_SOURCES);
    for (unsigned i = 0; i < NUM_PF_SOURCES; ++i) {
        const auto name = enums::PrefetchSourceTypeStrings[i];
        fillsBySource.subname(i, name);
        usefulBySource.subname(i, name);
        epfHitsBySource.subname(i, name);
    }
}

} // namespace replacement_policy
} // namespace gem5
