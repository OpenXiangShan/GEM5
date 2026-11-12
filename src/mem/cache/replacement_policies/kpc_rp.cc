/*
 * Copyright (c) 2026 XiangShan
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "mem/cache/replacement_policies/kpc_rp.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "mem/cache/tags/indexing_policies/base.hh"
#include "params/KPCRP.hh"
#include "params/KPCReplState.hh"

namespace gem5
{
namespace replacement_policy
{

KPCReplState::KPCReplState(const Params &p)
  : SimObject(p), stats(this), numSets(p.num_sets), numWays(p.num_ways),
    blockSize(p.block_size)
{
    fatal_if(!isPowerOf2(numSets) || !numWays || !isPowerOf2(blockSize),
             "KPC requires positive geometry and power-of-two sets/line size");
    fatal_if(!p.sampled_sets || p.sampled_sets > numSets,
             "KPC sampled_sets must be in [1, num_sets]");
    fatal_if(!p.hysteresis_bits || p.hysteresis_bits > 16,
             "KPC hysteresis_bits must be in [1, 16]");
    fatal_if(p.initial_hysteresis >= (1U << p.hysteresis_bits),
             "KPC initial hysteresis exceeds counter range");
    sampler = std::make_unique<KPCSampler>(numSets, numWays, p.sampled_sets,
                                         p.hysteresis_bits,
                                         p.initial_hysteresis);
}

void
KPCReplState::access(unsigned set, const PacketPtr pkt, bool prefetch)
{
    if (prefetch) {
        ++stats.prefetchAccesses;
    } else {
        ++stats.demandAccesses;
    }
    const auto result = sampler->access(set, pkt->getBlockAddr(blockSize),
                                        pkt->isSecure(), prefetch);
    if (result.sampled) {
        if (result.hit) {
            ++stats.samplerHits;
        } else {
            ++stats.samplerMisses;
        }
    }
    switch (result.training) {
      case KPCSampler::Training::DemandReuse:
        ++stats.demandFirstReuse;
        break;
      case KPCSampler::Training::PrefetchReuse:
        ++stats.prefetchFirstReuse;
        break;
      case KPCSampler::Training::DemandDead:
        ++stats.demandDead;
        break;
      case KPCSampler::Training::PrefetchDead:
        ++stats.prefetchDead;
        break;
      case KPCSampler::Training::None:
        break;
    }
    stats.dghValue = sampler->hysteresis(false);
    stats.pghValue = sampler->hysteresis(true);
}

KPC::KPC(const Params &p)
  : Base(p), state(p.state), numSlices(p.num_slices),
    sliceIndex(p.slice_index), promotePrefetchHits(p.promote_prefetch_hits),
    stats(this)
{
    fatal_if(!numSlices || sliceIndex >= numSlices ||
             state->numSets % numSlices,
             "Invalid KPC slice geometry");
}

bool
KPC::isPrefetch(const PacketPtr pkt)
{
    // Prefetch miss packets become reads/responses. Releases may retain the
    // original request flag, but are demand-side allocations at this level.
    return pkt && !pkt->isEviction() &&
        (pkt->cmd.isPrefetch() || pkt->req->isPrefetch());
}

void
KPC::observeAccess(const PacketPtr pkt, const BaseIndexingPolicy *indexing)
{
    // Maintenance and no-data CleanEvict messages are not reuse. Clean and
    // dirty writebacks may allocate in this non-inclusive L2, so train them
    // as demand-side accesses.
    if (pkt->cmd == MemCmd::CleanEvict || pkt->isStorePFTrain() ||
        pkt->req->isCacheMaintenance() || pkt->req->isUncacheable()) {
        return;
    }
    const unsigned set = indexing->extractSet(pkt->getAddr());
    state->access(set * numSlices + sliceIndex, pkt, isPrefetch(pkt));
}

void
KPC::observePrefetch(const PacketPtr pkt, const BaseIndexingPolicy *indexing,
                     const std::shared_ptr<ReplacementData> &data)
{
    observeAccess(pkt, indexing);
    if (data) {
        touch(data, pkt);
    }
}

void
KPC::invalidate(const std::shared_ptr<ReplacementData> &data)
{
    auto entry = std::static_pointer_cast<KPCReplData>(data);
    entry->valid = false;
    entry->rrpv = 3;
    // The independent LRU sampler intentionally retains its own entry.
}

void
KPC::touch(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    if (pkt && (pkt->isEviction() || pkt->isStorePFTrain() ||
                pkt->req->isCacheMaintenance())) {
        return;
    }
    if (isPrefetch(pkt)) {
        if (!promotePrefetchHits) {
            ++stats.prefetchNoPromotion;
            return;
        }
        ++stats.prefetchPromotions;
    } else {
        ++stats.demandPromotions;
    }
    touch(data);
}

void
KPC::touch(const std::shared_ptr<ReplacementData> &data) const
{
    std::static_pointer_cast<KPCReplData>(data)->rrpv = 0;
}

void
KPC::reset(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    auto entry = std::static_pointer_cast<KPCReplData>(data);
    const bool pf = isPrefetch(pkt);
    entry->valid = true;
    entry->rrpv = state->insertionRRPV(pf);
    if (pf) {
        if (entry->rrpv == 3) {
            ++stats.prefetchInsertions3;
        } else {
            ++stats.prefetchInsertions2;
        }
    } else if (entry->rrpv == 3) {
        ++stats.demandInsertions3;
    } else {
        ++stats.demandInsertions2;
    }
}

void
KPC::reset(const std::shared_ptr<ReplacementData> &data) const
{
    auto entry = std::static_pointer_cast<KPCReplData>(data);
    entry->valid = true;
    entry->rrpv = state->insertionRRPV(false);
}

ReplaceableEntry *
KPC::getVictim(const ReplacementCandidates &candidates) const
{
    assert(!candidates.empty());
    auto victim = candidates.front();
    uint8_t maximum = 0;
    for (const auto candidate : candidates) {
        auto entry = std::static_pointer_cast<KPCReplData>(
            candidate->replacementData);
        if (!entry->valid) {
            return candidate;
        }
        if (entry->rrpv > maximum) {
            maximum = entry->rrpv;
            victim = candidate;
        }
    }
    // Equivalent to repeated saturating increments, with one bounded scan.
    if (maximum < 3) {
        for (const auto candidate : candidates) {
            std::static_pointer_cast<KPCReplData>(
                candidate->replacementData)->rrpv += 3 - maximum;
        }
    }
    return victim;
}

std::shared_ptr<ReplacementData>
KPC::instantiateEntry()
{
    return std::make_shared<KPCReplData>();
}

KPCReplState::KPCStateStats::KPCStateStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(demandAccesses, "Demand-side accesses including writebacks"),
    ADD_STAT(prefetchAccesses, "Prefetch accesses including local candidates"),
    ADD_STAT(samplerHits, "Hits in independent LRU sampler"),
    ADD_STAT(samplerMisses, "Misses in independent LRU sampler"),
    ADD_STAT(demandFirstReuse, "First reuse of demand-allocated sampler lines"),
    ADD_STAT(prefetchFirstReuse, "First reuse of PF-allocated sampler lines"),
    ADD_STAT(demandDead, "Unused demand sampler victims"),
    ADD_STAT(prefetchDead, "Unused prefetch sampler victims"),
    ADD_STAT(dghValue, "Current demand global hysteresis"),
    ADD_STAT(pghValue, "Current prefetch global hysteresis")
{
}

KPC::KPCStats::KPCStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(demandInsertions2, "Demand-side insertions at RRPV 2"),
    ADD_STAT(demandInsertions3, "Demand-side insertions at RRPV 3"),
    ADD_STAT(prefetchInsertions2, "Prefetch insertions at RRPV 2"),
    ADD_STAT(prefetchInsertions3, "Prefetch insertions at RRPV 3"),
    ADD_STAT(demandPromotions, "Demand hit promotions to RRPV 0"),
    ADD_STAT(prefetchPromotions, "Prefetch hit promotions to RRPV 0"),
    ADD_STAT(prefetchNoPromotion, "Prefetch hits without promotion")
{
}

} // namespace replacement_policy
} // namespace gem5
