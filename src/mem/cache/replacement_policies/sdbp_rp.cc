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
      cacheProtectYoungest(p.cache_protect_youngest), stats(this)
{
    inform(
        "%s: SDBP num_sets=%u sampler_sets=%u half_set_bits=%u "
        "sampler_assoc=%u sampler_dead_first=%d sampler_protect_youngest=%u "
        "sampler_demand_pc_only=%d "
        "l1_copy_protection=%d cache_protect_youngest=%u bypass=%d",
        name(), core.numSets(), core.samplerSets(), core.halfBits(),
        p.sampler_assoc, p.sampler_dead_first, p.sampler_protect_youngest,
        p.sampler_demand_pc_only, l1CopyProtection, cacheProtectYoungest,
        enableBypass);
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
    sample(pkt, set, tag);
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
    entry = SDBPReplData();
}

void
SDBP::touch(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    if (entry.dead && isDemand(pkt) && hasPc(pkt))
        ++stats.deadHits;
    entry.lastUse = ++sequence;
    if (eligible(pkt) && (isDemand(pkt) || hasPc(pkt)))
        entry.dead = predict(pkt);
}

void
SDBP::touch(const std::shared_ptr<ReplacementData> &data) const
{
    auto &entry = *std::static_pointer_cast<SDBPReplData>(data);
    entry.lastUse = ++sequence;
    entry.dead = false;
}

void
SDBP::reset(const std::shared_ptr<ReplacementData> &data, const PacketPtr pkt)
{
    reset(data);
    std::static_pointer_cast<SDBPReplData>(data)->dead =
        predict(pkt);
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
    if (enableBypass && predict(pkt)) {
        ++stats.bypasses;
        DPRINTF(SDBP, "bypass addr=%#x pc=%#x\n", pkt->getAddr(), requestPc(pkt));
        return nullptr;
    }
    bool protectedDead = false;
    for (auto *candidate : candidates) {
        const auto &entry = *std::static_pointer_cast<SDBPReplData>(
            candidate->replacementData);
        if (!entry.dead)
            continue;
        if (cacheProtectYoungest) {
            unsigned younger = 0;
            for (const auto *other : candidates) {
                const auto &other_entry =
                    *std::static_pointer_cast<SDBPReplData>(
                        other->replacementData);
                if (other_entry.lastUse > entry.lastUse)
                    ++younger;
            }
            if (younger < cacheProtectYoungest) {
                ++stats.skippedYoungDead;
                continue;
            }
        }
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
    return std::make_shared<SDBPReplData>();
}

SDBP::SDBPStats::SDBPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, "Tag lookups observed by SDBP"),
      ADD_STAT(eligibleAccesses, "Eligible lookups carrying a PC"),
      ADD_STAT(excludedAccesses, "Lookups excluded by request type"),
      ADD_STAT(noPcAccesses, "Demand or prefetch lookups without a PC"),
      ADD_STAT(eligibleMisses, "Eligible lookups that miss the real cache"),
      ADD_STAT(samplerAccesses, "Eligible lookups to sampled sets"),
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
      ADD_STAT(skippedYoungDead,
               "Dead real-cache candidates skipped by recency protection"),
      ADD_STAT(bypasses, "Full-set allocation attempts bypassed by SDBP"),
      ADD_STAT(l1CopyChecks, "Dead candidates checked for resident L1 copies"),
      ADD_STAT(l1CopyProtectedDead, "Dead candidates skipped due to resident L1 copies"),
      ADD_STAT(l1CopyLruFallbacks, "Selections using LRU because every dead candidate has an L1 copy"),
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
               "Real-cache dead predictions by request source")
{
    for (auto *vector : {&samplerAccessesBySource, &samplerHitsBySource,
                         &liveTrainingBySource, &deadTrainingBySource,
                         &predictionQueriesBySource,
                         &deadPredictionsBySource}) {
        vector->init(SDBPCore::NumRequestTypes);
        vector->flags(statistics::total | statistics::nozero);
        vector->subname(0, "l1Demand");
        vector->subname(1, "l1Prefetch");
        vector->subname(2, "l2Prefetch");
    }
}

}  // namespace replacement_policy
}  // namespace gem5
