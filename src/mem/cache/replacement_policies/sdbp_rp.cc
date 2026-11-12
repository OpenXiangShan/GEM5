/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/cache/replacement_policies/sdbp_rp.hh"

#include <stdexcept>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/SDBP.hh"
#include "params/SDBPRP.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace replacement_policy
{
namespace
{

SDBPCore
makeCore(const SDBPRPParams &p)
{
    SDBPCore::Config cfg;
    cfg.numSets = p.num_sets;
    cfg.samplerNum = p.sampler_num;
    cfg.samplerAssoc = p.sampler_assoc;
    cfg.enableSampler = !p.observation_only;
    cfg.samplerDeadFirst = p.sampler_dead_first;
    cfg.samplerDemandPcOnly = p.sampler_demand_pc_only;
    cfg.samplerProtectYoungest = p.sampler_protect_youngest;
    cfg.predictorTables = p.predictor_tables;
    cfg.predictorEntries = p.predictor_entries;
    cfg.counterBits = p.counter_bits;
    cfg.deadThreshold = p.dead_threshold;
    cfg.partialTagBits = p.partial_tag_bits;
    cfg.partialPcBits = p.partial_pc_bits;
    cfg.pcShift = p.pc_shift;
    cfg.pcHashSeed = p.pc_hash_seed;
    cfg.pcHashType = p.pc_hash_type;
    cfg.indexHashType = p.index_hash_type;
    cfg.tableHashSeeds = p.table_hash_seeds;
    cfg.crcLiveUpdate = p.crc_live_update;
    try {
        return SDBPCore(cfg);
    } catch (const std::invalid_argument &error) {
        fatal("%s: invalid SDBP configuration: %s", p.name, error.what());
    }
}

}  // anonymous namespace

SDBP::SDBP(const Params &p)
    : Base(p), core(makeCore(p)), enableBypass(p.enable_bypass),
      l1CopyProtection(p.l1_copy_protection),
      observationOnly(p.observation_only), stats(this)
{
    fatal_if(observationOnly && enableBypass,
             "%s: observation-only SDBP cannot bypass fills", name());
    fatal_if(observationOnly && l1CopyProtection,
             "%s: observation-only SDBP does not use L1-copy protection", name());
    inform(
        "%s: SDBP num_sets=%u sampler_sets=%u half_set_bits=%u "
        "sampler_assoc=%u sampler_dead_first=%d sampler_protect_youngest=%u "
        "sampler_demand_pc_only=%d "
        "l1_copy_protection=%d bypass=%d observation_only=%d",
        name(), core.numSets(), core.samplerSets(), core.halfBits(),
        p.sampler_assoc, p.sampler_dead_first, p.sampler_protect_youngest,
        p.sampler_demand_pc_only, l1CopyProtection, enableBypass,
        observationOnly);
}

void
SDBP::bindCache(unsigned num_sets)
{
    fatal_if(bound, "%s: SDBP cannot be shared by tag stores", name());
    fatal_if(num_sets != core.numSets(), "%s: num_sets=%u does not match cache sets=%u", name(), core.numSets(),
             num_sets);
    bound = true;
}

void
SDBP::setL1CopyProbe(L1CopyProbe probe)
{
    fatal_if(l1CopyProbe || !probe, "%s: invalid L1-copy probe binding", name());
    l1CopyProbe = std::move(probe);
}

void
SDBP::startup()
{
    Base::startup();
    fatal_if(!bound,
             "%s: SDBP requires direct use by BaseSetAssoc tags "
             "with SetAssociative indexing (no DuelingRP or Ruby)",
             name());
}

void
SDBP::resetStats()
{
    Base::resetStats();
    if (!observationOnly)
        return;

    // Reset counters without forgetting predictions made during warmup.
    for (const auto &weak : observationEntries) {
        const auto entry = weak.lock();
        if (!entry || !entry->valid)
            continue;
        if (entry->signatureValid) {
            const auto source = static_cast<unsigned>(entry->signatureType);
            ++stats.fullCachePending;
            ++stats.fullCachePendingBySource[source];
            ++stats.fullCacheCarriedInBySource[source];
        }
        if (entry->prefetchOrigin && !entry->prefetchDemandUsed) {
            const auto source = static_cast<unsigned>(entry->fillType);
            ++stats.fullCachePrefetchPendingBySource[source];
            ++stats.fullCachePrefetchCarriedInBySource[source];
        }
    }
}

bool
SDBP::eligible(const PacketPtr pkt)
{
    return pkt && pkt->req &&
           pkt->req->l2RequestSource() != Request::L2RequestSource::Other &&
           !pkt->req->isInstFetch() && !pkt->req->isPTWalk() &&
           !pkt->req->isUncacheable() && !pkt->req->isCacheMaintenance() &&
           !pkt->isStorePFTrain() && !pkt->cmd.isSWPrefetch() &&
           (isDemand(pkt) || isPrefetch(pkt));
}

bool
SDBP::isPrefetch(const PacketPtr pkt)
{
    if (!pkt || !pkt->req || pkt->isSamplerDemand() || pkt->isEviction() ||
        pkt->cmd == MemCmd::WriteClean) {
        return false;
    }
    const auto source = pkt->req->l2RequestSource();
    return source == Request::L2RequestSource::L1Prefetch ||
           source == Request::L2RequestSource::L2Prefetch ||
           pkt->req->isPrefetch() || pkt->cmd.isHWPrefetch();
}

bool
SDBP::isDemand(const PacketPtr pkt)
{
    return pkt && pkt->req && !pkt->isEviction() &&
           pkt->cmd != MemCmd::WriteClean && !isPrefetch(pkt) &&
           (pkt->req->l2RequestSource() ==
                Request::L2RequestSource::L1Demand ||
            pkt->isDemand() || pkt->isRead() || pkt->isWrite() ||
            pkt->isUpgrade());
}

SDBPCore::RequestType
SDBP::requestType(const PacketPtr pkt)
{
    if (!isPrefetch(pkt))
        return SDBPCore::RequestType::Demand;
    const auto source = pkt->req->l2RequestSource();
    if (source == Request::L2RequestSource::L2Prefetch ||
        (source == Request::L2RequestSource::Unclassified &&
         pkt->cmd.isHWPrefetch())) {
        return SDBPCore::RequestType::L2Prefetch;
    }
    return SDBPCore::RequestType::L1Prefetch;
}

bool
SDBP::hasPc(const PacketPtr pkt)
{
    return pkt && pkt->req &&
           ((isPrefetch(pkt) && pkt->req->hasPrefetchTriggerPC()) ||
            pkt->req->hasPC());
}

Addr
SDBP::requestPc(const PacketPtr pkt)
{
    assert(hasPc(pkt));
    return isPrefetch(pkt) && pkt->req->hasPrefetchTriggerPC() ?
        pkt->req->getPrefetchTriggerPC() : pkt->req->getPC();
}

void
SDBP::sample(const PacketPtr pkt, unsigned set, Addr tag)
{
    const auto type = requestType(pkt);
    const auto result = core.access(set, tag, type, true, requestPc(pkt));
    if (!result.sampled)
        return;
    ++stats.samplerAccesses;
    if (pkt->bypassL2)
        ++stats.samplerBypassAccesses;
    ++stats.samplerAccessesBySource[static_cast<unsigned>(type)];
    if (result.hit) {
        ++stats.samplerHits;
        ++stats.samplerHitsBySource[static_cast<unsigned>(type)];
        if (result.previousSignatureValid) {
            ++stats.liveTraining;
            ++stats.liveTrainingBySource[
                static_cast<unsigned>(result.previousType)];
        }
        if (result.previousDead)
            ++stats.samplerDeadHits;
    } else if (result.evicted) {
        ++stats.samplerEvictions;
        if (result.previousSignatureValid) {
            ++stats.deadTraining;
            ++stats.deadTrainingBySource[
                static_cast<unsigned>(result.previousType)];
        }
        if (result.previousDead)
            ++stats.samplerDeadEvictions;
    }
    DPRINTF(SDBP,
            "sample addr=%#x pc=%#x source=%u set=%u tag=%#x "
            "sampler_hit=%d evicted=%d old_sig=%#x old_source=%u "
            "old_dead=%d\n",
            pkt->getAddr(), requestPc(pkt), unsigned(type), set, tag,
            result.hit, result.evicted, result.previousSignature,
            unsigned(result.previousType), result.previousDead);
}

void
SDBP::observe(const PacketPtr pkt, unsigned set, Addr tag, bool hit)
{
    ++stats.lookups;
    if (!eligible(pkt)) {
        ++stats.excludedAccesses;
        return;
    }
    if (!hasPc(pkt)) {
        ++stats.noPcAccesses;
        return;
    }
    ++stats.eligibleAccesses;
    if (!hit) {
        ++stats.eligibleMisses;
        return;
    }
    if (!observationOnly)
        sample(pkt, set, tag);
}

void
SDBP::observeFill(const PacketPtr pkt, unsigned set, Addr tag)
{
    ++stats.lookups;
    if (!eligible(pkt)) {
        ++stats.excludedAccesses;
        return;
    }
    if (!hasPc(pkt)) {
        ++stats.noPcAccesses;
        return;
    }
    ++stats.eligibleAccesses;
    ++stats.eligibleMisses;
    if (!observationOnly)
        sample(pkt, set, tag);
}

void
SDBP::observeLocalPrefetch(
    const PacketPtr pkt, const std::shared_ptr<ReplacementData> &data)
{
    if (observationOnly)
        updateLine(data, pkt, false);
}

void
SDBP::notifyReplacementEviction(
    const std::shared_ptr<ReplacementData> &data)
{
    if (!observationOnly)
        return;

    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    resolvePrediction(entry, true);
    if (entry.prefetchOrigin && !entry.prefetchDemandUsed) {
        ++stats.fullCachePrefetchUnusedEvictionsBySource[
            static_cast<unsigned>(entry.fillType)];
        --stats.fullCachePrefetchPendingBySource[
            static_cast<unsigned>(entry.fillType)];
    }
    entry.prefetchOrigin = false;
}

bool
SDBP::transferEntry(const std::shared_ptr<ReplacementData> &src_data,
                    const std::shared_ptr<ReplacementData> &dest_data)
{
    if (!observationOnly)
        return false;

    auto &src = *std::static_pointer_cast<SDBPReplData>(src_data);
    auto &dest = *std::static_pointer_cast<SDBPReplData>(dest_data);
    dest = src;
    dest.lastUse = ++sequence;
    dest.lastTouchTick = curTick();
    src = SDBPReplData();
    return true;
}

bool
SDBP::predict(const PacketPtr pkt) const
{
    if (!eligible(pkt) || !hasPc(pkt))
        return false;
    const auto type = requestType(pkt);
    const auto source = static_cast<unsigned>(type);
    ++stats.predictionQueries;
    ++stats.predictionQueriesBySource[source];
    const auto signature = core.signature(requestPc(pkt));
    const bool dead = core.predict(signature, type);
    if (dead) {
        ++stats.deadPredictions;
        ++stats.deadPredictionsBySource[source];
    }
    DPRINTF(SDBP,
            "predict addr=%#x pc=%#x source=%u signature=%#x "
            "confidence=%u dead=%d\n",
            pkt->getAddr(), requestPc(pkt), source, signature,
            core.confidence(signature, type), dead);
    return dead;
}

void
SDBP::invalidate(const std::shared_ptr<ReplacementData> &data)
{
    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    if (observationOnly) {
        censorPrediction(entry);
        if (entry.prefetchOrigin && !entry.prefetchDemandUsed) {
            ++stats.fullCachePrefetchUnusedInvalidationsBySource[
                static_cast<unsigned>(entry.fillType)];
            --stats.fullCachePrefetchPendingBySource[
                static_cast<unsigned>(entry.fillType)];
        }
    }
    entry = SDBPReplData();
}

void
SDBP::touch(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    if (observationOnly) {
        updateLine(data, pkt, true);
        return;
    }

    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    if (entry.dead && isDemand(pkt) && hasPc(pkt))
        ++stats.deadHits;
    entry.lastUse = ++sequence;
    entry.lastTouchTick = curTick();
    if (eligible(pkt) && (isDemand(pkt) || hasPc(pkt)))
        entry.dead = predict(pkt);
}

void
SDBP::touch(const std::shared_ptr<ReplacementData> &data) const
{
    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    entry.lastUse = ++sequence;
    entry.lastTouchTick = curTick();
    if (!observationOnly)
        entry.dead = false;
}

void
SDBP::reset(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    reset(data);
    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    if (observationOnly) {
        entry.signatureValid = false;
        entry.prefetchOrigin = eligible(pkt) && isPrefetch(pkt);
        entry.prefetchDemandUsed = false;
        if (entry.prefetchOrigin) {
            entry.fillType = requestType(pkt);
            ++stats.fullCachePrefetchFillsBySource[
                static_cast<unsigned>(entry.fillType)];
            ++stats.fullCachePrefetchPendingBySource[
                static_cast<unsigned>(entry.fillType)];
        }
        if (eligible(pkt) && hasPc(pkt))
            startPrediction(entry, pkt);
    } else {
        entry.dead = predict(pkt);
    }
    ++stats.fills;
}

void
SDBP::reset(const std::shared_ptr<ReplacementData> &data) const
{
    touch(data);
    std::static_pointer_cast<SDBPReplData>(data)->valid = true;
}

ReplaceableEntry *
SDBP::getVictim(const ReplacementCandidates &candidates) const
{
    return getVictim(candidates, nullptr);
}

ReplaceableEntry *
SDBP::getVictim(const ReplacementCandidates &candidates, const PacketPtr pkt) const
{
    assert(!candidates.empty());
    if (observationOnly) {
        auto *lru = candidates.front();
        auto oldest = std::static_pointer_cast<SDBPReplData>(
            lru->replacementData)->lastTouchTick;
        for (auto *candidate : candidates) {
            const auto tick = std::static_pointer_cast<SDBPReplData>(
                candidate->replacementData)->lastTouchTick;
            if (tick < oldest) {
                oldest = tick;
                lru = candidate;
            }
        }
        if (!std::static_pointer_cast<SDBPReplData>(
                lru->replacementData)->valid) {
            ++stats.invalidVictims;
        }
        ++stats.lruVictims;
        return lru;
    }

    ReplaceableEntry *lru = candidates.front();
    ReplaceableEntry *dead = nullptr;
    uint64_t oldest = std::static_pointer_cast<SDBPReplData>(lru->replacementData)->lastUse;
    for (auto *candidate : candidates) {
        const auto &entry = *std::static_pointer_cast<SDBPReplData>(candidate->replacementData);
        if (!entry.valid) {
            ++stats.invalidVictims;
            return candidate;
        }
        if (entry.lastUse < oldest) {
            oldest = entry.lastUse;
            lru = candidate;
        }
    }
    // Preserve invalid-way priority. Bypass only predicted-dead demand data
    // refills; the independent hierarchy policy controls grants and routing.
    if (enableBypass && pkt && pkt->isResponse() && pkt->isRead() &&
        isDemand(pkt) && predict(pkt)) {
        ++stats.bypasses;
        pkt->bypassL2 = true;
        DPRINTF(SDBP, "bypass addr=%#x pc=%#x\n",
                pkt->getAddr(), requestPc(pkt));
        return nullptr;
    }
    bool protectedDead = false;
    for (auto *candidate : candidates) {
        const auto &entry = *std::static_pointer_cast<SDBPReplData>(
            candidate->replacementData);
        if (!entry.dead)
            continue;
        if (l1CopyProtection && l1CopyProbe) {
            ++stats.l1CopyChecks;
            if (l1CopyProbe(candidate)) {
                ++stats.l1CopyProtectedDead;
                protectedDead = true;
                continue;
            }
        }
        dead = candidate;
        break;
    }
    // The fallback is the original LRU, even when it has an L1 copy.
    if (!dead && protectedDead)
        ++stats.l1CopyLruFallbacks;
    if (dead)
        ++stats.deadVictims;
    else
        ++stats.lruVictims;
    if (dead && dead != lru)
        ++stats.nonLruDeadVictims;
    auto *victim = dead ? dead : lru;
    DPRINTF(SDBP, "victim set=%u way=%u dead=%d lru_way=%u\n", victim->getSet(), victim->getWay(), dead != nullptr,
            lru->getWay());
    return victim;
}

std::shared_ptr<ReplacementData>
SDBP::instantiateEntry()
{
    auto entry = std::make_shared<SDBPReplData>();
    if (observationOnly)
        observationEntries.emplace_back(entry);
    return entry;
}

void
SDBP::updateLine(const std::shared_ptr<ReplacementData> &data,
                 const PacketPtr pkt, bool updateRecency)
{
    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    if (updateRecency) {
        entry.lastUse = ++sequence;
        entry.lastTouchTick = curTick();
    }
    if (!eligible(pkt))
        return;

    const auto type = requestType(pkt);
    if (entry.prefetchOrigin && !entry.prefetchDemandUsed && isDemand(pkt)) {
        entry.prefetchDemandUsed = true;
        ++stats.fullCachePrefetchDemandUsesBySource[
            static_cast<unsigned>(entry.fillType)];
        --stats.fullCachePrefetchPendingBySource[
            static_cast<unsigned>(entry.fillType)];
    }
    if (entry.signatureValid) {
        if (entry.dead && isDemand(pkt) && hasPc(pkt))
            ++stats.deadHits;
        resolvePrediction(entry, false, type);
    }
    if (hasPc(pkt)) {
        startPrediction(entry, pkt);
    } else {
        entry.dead = false;
    }
}

void
SDBP::startPrediction(SDBPReplData &entry, const PacketPtr pkt)
{
    entry.signature = core.signature(requestPc(pkt));
    entry.signatureType = requestType(pkt);
    entry.signatureValid = true;
    entry.dead = predict(pkt);
    ++stats.fullCachePredictions;
    ++stats.fullCachePredictionsBySource[
        static_cast<unsigned>(entry.signatureType)];
    ++stats.fullCachePending;
    ++stats.fullCachePendingBySource[
        static_cast<unsigned>(entry.signatureType)];
}

void
SDBP::resolvePrediction(SDBPReplData &entry, bool actualDead,
                        SDBPCore::RequestType reuseType)
{
    if (!entry.signatureValid)
        return;

    const auto source = static_cast<unsigned>(entry.signatureType);
    ++stats.fullCacheResolved;
    ++stats.fullCacheResolvedBySource[source];
    --stats.fullCachePending;
    --stats.fullCachePendingBySource[source];
    if (entry.dead) {
        if (actualDead) {
            ++stats.fullCacheDeadTruePositiveBySource[source];
        } else {
            ++stats.fullCacheDeadFalsePositiveBySource[source];
            if (reuseType == SDBPCore::RequestType::Demand)
                ++stats.fullCachePredDeadDemandReuseBySource[source];
        }
    } else if (actualDead) {
        ++stats.fullCacheLiveFalseNegativeBySource[source];
    } else {
        ++stats.fullCacheLiveTrueNegativeBySource[source];
    }

    if (actualDead) {
        core.train(entry.signature, true, entry.signatureType);
        ++stats.deadTraining;
        ++stats.deadTrainingBySource[source];
    } else {
        core.train(entry.signature, false, entry.signatureType);
        ++stats.liveTraining;
        ++stats.liveTrainingBySource[source];
        if (reuseType == SDBPCore::RequestType::Demand)
            ++stats.fullCacheDemandReuseBySource[source];
        else
            ++stats.fullCachePrefetchReuseBySource[source];
    }
    entry.signatureValid = false;
}

void
SDBP::censorPrediction(SDBPReplData &entry)
{
    if (!entry.signatureValid)
        return;

    const auto source = static_cast<unsigned>(entry.signatureType);
    ++stats.fullCacheCensored;
    ++stats.fullCacheCensoredBySource[source];
    --stats.fullCachePending;
    --stats.fullCachePendingBySource[source];
    entry.signatureValid = false;
}

SDBP::SDBPStats::SDBPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, "Tag lookups observed by SDBP"),
      ADD_STAT(eligibleAccesses, "Eligible lookups carrying a PC"),
      ADD_STAT(excludedAccesses, "Lookups excluded by request type"),
      ADD_STAT(noPcAccesses, "Demand or prefetch lookups without a PC"),
      ADD_STAT(eligibleMisses, "Eligible lookups that miss the real cache"),
      ADD_STAT(samplerAccesses, "Eligible lookups to sampled sets"),
      ADD_STAT(samplerBypassAccesses, "Sampled demand fills bypassing L2"),
      ADD_STAT(samplerHits, "Partial-tag hits in the sampler"),
      ADD_STAT(samplerEvictions, "Valid entries evicted from the sampler"),
      ADD_STAT(liveTraining, "Old signatures trained live"),
      ADD_STAT(deadTraining, "Old signatures trained dead"),
      ADD_STAT(samplerDeadHits, "Sampled dead predictions followed by reuse"),
      ADD_STAT(samplerDeadEvictions, "Sampled dead predictions followed by eviction"),
      ADD_STAT(predictionQueries, "Real-cache prediction queries, including bypass"),
      ADD_STAT(deadPredictions, "Real-cache queries predicting dead"),
      ADD_STAT(deadHits, "Eligible real-cache hits on predicted-dead entries"),
      ADD_STAT(fills, "Real-cache packet-bearing insertions"),
      ADD_STAT(invalidVictims, "Victim selections using an invalid entry"),
      ADD_STAT(deadVictims, "Victim selections using a predicted-dead entry"),
      ADD_STAT(nonLruDeadVictims, "Dead victims different from the LRU victim"),
      ADD_STAT(lruVictims, "Victim selections falling back to LRU"),
      ADD_STAT(bypasses, "Predicted-dead data refills bypassed by SDBP"),
      ADD_STAT(l1CopyChecks, "Dead candidates checked for resident L1 copies"),
      ADD_STAT(l1CopyProtectedDead, "Dead candidates skipped due to resident L1 copies"),
      ADD_STAT(l1CopyLruFallbacks, "Selections using LRU because every dead candidate has an L1 copy"),
      ADD_STAT(fullCachePredictions,
               "Full-cache line predictions started"),
      ADD_STAT(fullCacheResolved,
               "Full-cache line predictions resolved by reuse or replacement"),
      ADD_STAT(fullCacheCensored,
               "Full-cache line predictions censored by non-replacement invalidation"),
      ADD_STAT(fullCachePending,
               "Full-cache line predictions still resident and unresolved"),
      ADD_STAT(samplerAccessesBySource, statistics::units::Count::get(),
               "Sampled accesses by incoming request source"),
      ADD_STAT(samplerHitsBySource, statistics::units::Count::get(),
               "Sampler hits by incoming request source"),
      ADD_STAT(liveTrainingBySource, statistics::units::Count::get(),
               "Live training by the old sampler signature source"),
      ADD_STAT(deadTrainingBySource, statistics::units::Count::get(),
               "Dead training by the old sampler signature source"),
      ADD_STAT(predictionQueriesBySource, statistics::units::Count::get(),
               "Real-cache predictor queries by request source"),
      ADD_STAT(deadPredictionsBySource, statistics::units::Count::get(),
               "Real-cache dead predictions by request source"),
      ADD_STAT(fullCachePredictionsBySource,
               statistics::units::Count::get(),
               "Full-cache predictions by signature source"),
      ADD_STAT(fullCacheCarriedInBySource,
               statistics::units::Count::get(),
               "Unresolved predictions carried across the last stats reset"),
      ADD_STAT(fullCacheResolvedBySource, statistics::units::Count::get(),
               "Resolved full-cache predictions by signature source"),
      ADD_STAT(fullCacheCensoredBySource, statistics::units::Count::get(),
               "Censored full-cache predictions by signature source"),
      ADD_STAT(fullCacheDeadTruePositiveBySource,
               statistics::units::Count::get(),
               "Predicted-dead lines replaced before reuse by signature source"),
      ADD_STAT(fullCacheDeadFalsePositiveBySource,
               statistics::units::Count::get(),
               "Predicted-dead lines reused before replacement by signature source"),
      ADD_STAT(fullCacheLiveTrueNegativeBySource,
               statistics::units::Count::get(),
               "Predicted-live lines reused before replacement by signature source"),
      ADD_STAT(fullCacheLiveFalseNegativeBySource,
               statistics::units::Count::get(),
               "Predicted-live lines replaced before reuse by signature source"),
      ADD_STAT(fullCachePendingBySource, statistics::units::Count::get(),
               "Unresolved full-cache predictions by signature source"),
      ADD_STAT(fullCacheDemandReuseBySource, statistics::units::Count::get(),
               "Resolved predictions reused by demand by signature source"),
      ADD_STAT(fullCachePrefetchReuseBySource,
               statistics::units::Count::get(),
               "Resolved predictions reused by prefetch by signature source"),
      ADD_STAT(fullCachePredDeadDemandReuseBySource,
               statistics::units::Count::get(),
               "Predicted-dead lines later reused by demand by signature source"),
      ADD_STAT(fullCachePrefetchFillsBySource,
               statistics::units::Count::get(),
               "Prefetch-origin line fills by prefetch source"),
      ADD_STAT(fullCachePrefetchCarriedInBySource,
               statistics::units::Count::get(),
               "Unused prefetch-origin lines carried across the last stats reset"),
      ADD_STAT(fullCachePrefetchPendingBySource,
               statistics::units::Count::get(),
               "Prefetch-origin lines awaiting first demand use"),
      ADD_STAT(fullCachePrefetchDemandUsesBySource,
               statistics::units::Count::get(),
               "Prefetch-origin lines used by demand before removal"),
      ADD_STAT(fullCachePrefetchUnusedEvictionsBySource,
               statistics::units::Count::get(),
               "Prefetch-origin lines replaced before demand use"),
      ADD_STAT(fullCachePrefetchUnusedInvalidationsBySource,
               statistics::units::Count::get(),
               "Prefetch-origin lines invalidated before demand use")
{
    for (auto *vector : {&samplerAccessesBySource, &samplerHitsBySource,
                         &liveTrainingBySource, &deadTrainingBySource,
                         &predictionQueriesBySource,
                         &deadPredictionsBySource,
                         &fullCachePredictionsBySource,
                         &fullCacheCarriedInBySource,
                         &fullCacheResolvedBySource,
                         &fullCacheCensoredBySource,
                         &fullCacheDeadTruePositiveBySource,
                         &fullCacheDeadFalsePositiveBySource,
                         &fullCacheLiveTrueNegativeBySource,
                         &fullCacheLiveFalseNegativeBySource,
                         &fullCachePendingBySource,
                         &fullCacheDemandReuseBySource,
                         &fullCachePrefetchReuseBySource,
                         &fullCachePredDeadDemandReuseBySource,
                         &fullCachePrefetchFillsBySource,
                         &fullCachePrefetchCarriedInBySource,
                         &fullCachePrefetchPendingBySource,
                         &fullCachePrefetchDemandUsesBySource,
                         &fullCachePrefetchUnusedEvictionsBySource,
                         &fullCachePrefetchUnusedInvalidationsBySource}) {
        vector->init(SDBPCore::NumRequestTypes);
        vector->flags(statistics::total | statistics::nozero);
        vector->subname(0, "l1Demand");
        vector->subname(1, "l1Prefetch");
        vector->subname(2, "l2Prefetch");
    }
}

}  // namespace replacement_policy
}  // namespace gem5
