/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/cache/replacement_policies/fdp_rp.hh"

#include "base/logging.hh"
#include "params/FDPRP.hh"

namespace gem5
{
namespace replacement_policy
{
namespace
{

fdp::Feedback
makeFeedback(const FDPRPParams &p)
{
    // Validate before allocating the pollution filter or shifting its size.
    fatal_if(p.num_ways == 0, "FDP requires at least one way");
    fatal_if(p.lru_fallback && (p.num_sets < 32 ||
             (p.num_sets & (p.num_sets - 1))),
             "FDP sampled fallback requires power-of-two sets, at least 32");
    fatal_if(p.filter_bits == 0 || p.filter_bits > 24,
             "FDP filter_bits must be between 1 and 24");
    fatal_if(p.block_bits > 32, "FDP block_bits must be at most 32");
    fatal_if(p.interval_evictions == 0, "FDP interval must be positive");
    fatal_if(!(p.pollution_low >= 0 && p.pollution_high <= 1 &&
               p.pollution_low < p.pollution_high),
             "FDP requires 0 <= pollution_low < pollution_high <= 1");
    return fdp::Feedback(p.block_bits, p.filter_bits, p.interval_evictions,
                         p.pollution_low, p.pollution_high, p.tagged_filter);
}

} // anonymous namespace

FDP::FDP(const Params &p)
  : Base(p), numWays(p.num_ways), feedback(makeFeedback(p)),
    sourceFeedback(p.source_feedback), confidence(NUM_PF_SOURCES),
    lruFallback(p.lru_fallback), fdpStats(this)
{
    if (lruFallback)
        fdpStats.fallbackSelector = insertion.value();
}

void
FDP::sampleDemandFill(const std::shared_ptr<FDPReplData> &data)
{
    if (!lruFallback)
        return;
    insertion.demandFill(data->sample);
    if (data->sample == fdp::SampledInsertion::FdpSample)
        fdpStats.fdpSampleFills++;
    else if (data->sample == fdp::SampledInsertion::LruSample)
        fdpStats.lruSampleFills++;
    fdpStats.fallbackSelector = insertion.value();
}

bool
FDP::isPrefetch(const PacketPtr pkt)
{
    return pkt && (pkt->cmd.isPrefetch() ||
        (!pkt->isEviction() && pkt->req->isPrefetch()));
}

void
FDP::invalidate(const std::shared_ptr<ReplacementData> &data)
{
    const auto d = std::static_pointer_cast<FDPReplData>(data);
    d->stack->invalidate(d->way);
}

void
FDP::touch(const std::shared_ptr<ReplacementData> &data,
           const PacketPtr pkt)
{
    const auto d = std::static_pointer_cast<FDPReplData>(data);
    if (pkt && pkt->cmd == MemCmd::StorePFTrain)
        return;
    const bool fallback = lruFallback && insertion.useLru(d->sample);
    if (sourceFeedback && !fallback && d->source != PF_NONE &&
        isPrefetch(pkt) && d->line().unusedPrefetch &&
        !confidence.protect(d->source)) {
        // A repeated prediction is not evidence that demand needs this line.
        fdpStats.ignoredPrefetchHits++;
        return;
    }
    const bool demand = pkt && pkt->isDemand() && !isPrefetch(pkt);
    if (demand && d->line().unusedPrefetch) {
        fdpStats.usefulPrefetches++;
        if (sourceFeedback)
            confidence.train(d->source, true);
    }
    d->stack->touch(d->way, demand);
}

void
FDP::touch(const std::shared_ptr<ReplacementData> &data) const
{
    const auto d = std::static_pointer_cast<FDPReplData>(data);
    d->stack->touch(d->way, true);
}

void
FDP::reset(const std::shared_ptr<ReplacementData> &data,
           const PacketPtr pkt)
{
    const auto d = std::static_pointer_cast<FDPReplData>(data);
    const bool prefetch = isPrefetch(pkt);
    unsigned rank = 0;
    if (prefetch) {
        const unsigned source = static_cast<unsigned>(pkt->req->getPFSource());
        d->source = source < NUM_PF_SOURCES ? source :
            static_cast<unsigned>(PF_NONE);
        const bool fallback = lruFallback && insertion.useLru(d->sample);
        const bool protect = !fallback && sourceFeedback &&
            d->source != PF_NONE && confidence.protect(d->source);
        rank = fallback || protect ? 0 : feedback.insertionRank(numWays);
        if (fallback)
            fdpStats.fallbackPrefetches++;
        feedback.prefetchFill(pkt->getAddr(), pkt->isSecure());
        const unsigned position = feedback.getPosition();
        fdpStats.prefetchInsertions[fallback || protect ? 3 : position]++;
        if (protect)
            fdpStats.protectedBySource[d->source]++;
    } else if (pkt) {
        feedback.demandFill(pkt->getAddr(), pkt->isSecure());
        // Fills normally carry response commands; isDemand() covers requests.
        const bool demand = pkt->isDemand() || (pkt->isResponse() &&
            (pkt->isRead() || pkt->isWrite()));
        if (demand && !pkt->isEviction() &&
            !pkt->req->isUncacheable() &&
            !pkt->req->isCacheMaintenance() &&
            pkt->cmd != MemCmd::StorePFTrain) {
            sampleDemandFill(d);
        }
    }
    d->stack->insert(d->way, prefetch, rank);
}

void
FDP::reset(const std::shared_ptr<ReplacementData> &data) const
{
    const auto d = std::static_pointer_cast<FDPReplData>(data);
    d->stack->insert(d->way, false, 0);
}

void
FDP::notifyDemandUse(const std::shared_ptr<ReplacementData> &data)
{
    const auto d = std::static_pointer_cast<FDPReplData>(data);
    if (!d->line().unusedPrefetch)
        return;
    fdpStats.usefulPrefetches++;
    fdpStats.latePrefetchUses++;
    // A merged consumer also waited for a fill; count it once, not as a hit.
    sampleDemandFill(d);
    if (sourceFeedback)
        confidence.train(d->source, true);
    d->stack->touch(d->way, true);
}

void
FDP::notifyMiss(const PacketPtr pkt)
{
    if (!pkt->isDemand() || pkt->cmd == MemCmd::StorePFTrain ||
        isPrefetch(pkt) || pkt->req->isUncacheable() ||
        pkt->req->isCacheMaintenance()) {
        return;
    }
    fdpStats.demandMisses++;
    const auto aliases = feedback.rejectedAliases();
    if (feedback.demandMiss(pkt->getAddr(), pkt->isSecure()))
        fdpStats.pollutionMisses++;
    fdpStats.filterAliasesRejected += feedback.rejectedAliases() - aliases;
}

void
FDP::notifyEviction(const std::shared_ptr<ReplacementData> &data,
                    Addr addr, bool secure, const PacketPtr incoming)
{
    const auto d = std::static_pointer_cast<FDPReplData>(data);
    const auto &line = d->line();
    const bool prefetch = isPrefetch(incoming);
    if (prefetch && line.demandFetched)
        fdpStats.demandEvictedByPrefetch++;
    if (line.unusedPrefetch) {
        fdpStats.unusedPrefetchEvictions++;
        if (sourceFeedback)
            confidence.train(d->source, false);
    } else {
        fdpStats.usefulEvictions++;
    }

    if (feedback.evict(addr, secure, line.demandFetched,
                       !line.unusedPrefetch, prefetch)) {
        fdpStats.intervals++;
        fdpStats.pollutionRate = feedback.getPollution();
        fdpStats.intervalPositions[feedback.getPosition()]++;
    }
}

ReplaceableEntry *
FDP::getVictim(const ReplacementCandidates &candidates) const
{
    assert(!candidates.empty());
    ReplaceableEntry *victim = candidates.front();
    unsigned oldest = 0;
    for (auto *candidate : candidates) {
        const auto d = std::static_pointer_cast<FDPReplData>(
            candidate->replacementData);
        if (!d->line().valid)
            return candidate;
        if (d->line().rank >= oldest) {
            oldest = d->line().rank;
            victim = candidate;
        }
    }
    return victim;
}

std::shared_ptr<ReplacementData>
FDP::instantiateEntry()
{
    const auto sample = fdp::SampledInsertion::classifySet(
        entryCount / numWays);
    const unsigned way = entryCount++ % numWays;
    if (way == 0)
        currentSet = std::make_shared<fdp::Stack>(numWays);
    return std::make_shared<FDPReplData>(currentSet, way, sample);
}

FDP::FDPStats::FDPStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(demandMisses, "Demand tag misses observed by FDP"),
    ADD_STAT(pollutionMisses, "Demand misses matching the pollution filter"),
    ADD_STAT(filterAliasesRejected, "Tag misses rejecting pollution aliases"),
    ADD_STAT(demandEvictedByPrefetch, "Demand-filled lines evicted by prefetch"),
    ADD_STAT(usefulEvictions, "Useful lines evicted (FDP interval clock)"),
    ADD_STAT(unusedPrefetchEvictions, "Unused prefetch lines evicted"),
    ADD_STAT(usefulPrefetches, "First demand uses of prefetched lines"),
    ADD_STAT(latePrefetchUses, "First demand uses during fill target service"),
    ADD_STAT(ignoredPrefetchHits, "Unused prefetch hits without source confidence"),
    ADD_STAT(intervals, "Completed FDP feedback intervals"),
    ADD_STAT(pollutionRate, "Most recent smoothed pollution rate"),
    ADD_STAT(fdpSampleFills, "Demand fills in FDP sample sets"),
    ADD_STAT(lruSampleFills, "Demand fills in LRU sample sets"),
    ADD_STAT(fallbackPrefetches, "Prefetch fills using sampled LRU fallback"),
    ADD_STAT(fallbackSelector, "FDP/LRU insertion selector (LRU if >= 512)"),
    ADD_STAT(prefetchInsertions, "Prefetch fills by insertion position"),
    ADD_STAT(intervalPositions, "Feedback intervals by selected position"),
    ADD_STAT(protectedBySource, "High-confidence prefetch fills placed at MRU")
{
    for (auto *v : {&prefetchInsertions, &intervalPositions}) {
        v->init(v == &prefetchInsertions ? 4 : 3);
        v->subname(fdp::Feedback::Mid, "mid");
        v->subname(fdp::Feedback::Quarter, "quarter");
        v->subname(fdp::Feedback::Lru, "lru");
    }
    prefetchInsertions.subname(3, "mru");
    protectedBySource.init(NUM_PF_SOURCES);
    for (unsigned i = 0; i < NUM_PF_SOURCES; ++i)
        protectedBySource.subname(i, enums::PrefetchSourceTypeStrings[i]);
}

} // namespace replacement_policy
} // namespace gem5
