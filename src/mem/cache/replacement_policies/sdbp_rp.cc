/*
 * Copyright (c) 2026 Institute of Computing Technology, CAS
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mem/cache/replacement_policies/sdbp_rp.hh"

#include <stdexcept>

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

SDBP::SDBP(const Params &p) : Base(p), core(makeCore(p)), enableBypass(p.enable_bypass), stats(this)
{
    inform(
        "%s: SDBP num_sets=%u sampler_sets=%u half_set_bits=%u "
        "sampler_assoc=%u bypass=%d",
        name(), core.numSets(), core.samplerSets(), core.halfBits(), p.sampler_assoc, enableBypass);
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
    // Read responses retain the original Request but are not isDemand().
    return pkt && pkt->req && pkt->req->hasPC() && !pkt->req->isPrefetch() && !pkt->cmd.isPrefetch() &&
           !pkt->isEviction() && pkt->cmd != MemCmd::WriteClean &&
           (pkt->isDemand() || (pkt->isResponse() && (pkt->isRead() || pkt->isWrite())));
}

void
SDBP::observe(const PacketPtr pkt, unsigned set, Addr tag, bool hit)
{
    ++stats.lookups;
    if (!eligible(pkt)) {
        if (pkt->isDemand() && !pkt->req->hasPC())
            ++stats.noPcAccesses;
        else
            ++stats.excludedAccesses;
        return;
    }
    ++stats.eligibleAccesses;
    if (!hit)
        ++stats.eligibleMisses;
    const auto result = core.access(set, tag, pkt->isSecure(), pkt->req->getPC());
    if (!result.sampled)
        return;
    ++stats.samplerAccesses;
    if (result.hit) {
        ++stats.samplerHits;
        ++stats.liveTraining;
        if (result.previousDead)
            ++stats.samplerDeadHits;
    } else if (result.evicted) {
        ++stats.samplerEvictions;
        ++stats.deadTraining;
        if (result.previousDead)
            ++stats.samplerDeadEvictions;
    }
    DPRINTF(SDBP,
            "sample addr=%#x pc=%#x set=%u tag=%#x cache_hit=%d "
            "sampler_hit=%d evicted=%d old_sig=%#x old_dead=%d\n",
            pkt->getAddr(), pkt->req->getPC(), set, tag, hit, result.hit, result.evicted, result.previousSignature,
            result.previousDead);
}

bool
SDBP::predict(const PacketPtr pkt) const
{
    if (!eligible(pkt))
        return false;
    ++stats.predictionQueries;
    const auto signature = core.signature(pkt->req->getPC());
    const bool dead = core.predict(signature);
    if (dead)
        ++stats.deadPredictions;
    DPRINTF(SDBP,
            "predict addr=%#x pc=%#x signature=%#x confidence=%u "
            "dead=%d\n",
            pkt->getAddr(), pkt->req->getPC(), signature, core.confidence(signature), dead);
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
    if (entry.dead && eligible(pkt))
        ++stats.deadHits;
    entry.lastUse = ++sequence;
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
    std::static_pointer_cast<SDBPReplData>(data)->dead = predict(pkt);
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
        if (entry.dead && !dead)
            dead = candidate;
        if (entry.lastUse < oldest) {
            oldest = entry.lastUse;
            lru = candidate;
        }
    }
    if (enableBypass && predict(pkt)) {
        ++stats.bypasses;
        DPRINTF(SDBP, "bypass addr=%#x pc=%#x\n", pkt->getAddr(), pkt->req->getPC());
        return nullptr;
    }
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
      ADD_STAT(eligibleAccesses, "Demand lookups carrying a PC"),
      ADD_STAT(excludedAccesses, "Lookups excluded by request type"),
      ADD_STAT(noPcAccesses, "Demand lookups without a PC"),
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
      ADD_STAT(bypasses, "Full-set allocation attempts bypassed by SDBP")
{
}

}  // namespace replacement_policy
}  // namespace gem5
