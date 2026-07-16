/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 */

#include "mem/cache/replacement_policies/leeway_l2_rp.hh"

#include <algorithm>
#include <cassert>
#include <climits>

#include "base/logging.hh"
#include "params/LeewayL2RP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

LeewayL2::LeewayL2ReplData::LeewayL2ReplData()
  : valid(false), setId(0), wayId(0), isSampledSet(false), nruVal(0),
    predictedLd(0), signature(NoSig), currentLd(BypassLd)
{
}

LeewayL2::Mode
LeewayL2::parseMode(const std::string &mode_str)
{
    if (mode_str == "lru") {
        return Mode::LRU;
    }
    if (mode_str == "nru") {
        return Mode::NRU;
    }
    fatal("Unsupported LeewayL2 mode '%s'. Use 'lru' or 'nru'.\n",
          mode_str.c_str());
}

LeewayL2::LeewayL2(const Params &p)
  : Base(p),
    mode(parseMode(p.mode)),
    numSets(p.num_sets),
    numWays(p.num_ways),
    ldptEntries(p.ldpt_entries),
    sampleSets(p.sample_sets),
    nruBits(p.nru_bits),
    requirePcSignature(p.require_pc_signature),
    ldIncreaseThreshold(p.ld_increase_threshold),
    ldDecreaseThreshold(p.ld_decrease_threshold),
    maxNruVal(mode == Mode::LRU ? p.num_ways - 1 :
              (static_cast<unsigned>(1) << p.nru_bits) - 1),
    maxLd(maxNruVal > 0 ? maxNruVal - 1 : 0),
    entryCount(0),
    ldpt(ldptEntries),
    entriesBySet(numSets),
    stats(this)
{
    fatal_if(numSets == 0, "LeewayL2 requires num_sets > 0");
    fatal_if(numWays == 0, "LeewayL2 requires num_ways > 0");
    fatal_if(ldptEntries <= SpecialSigs,
             "LeewayL2 requires ldpt_entries > %u", SpecialSigs);
    fatal_if(mode == Mode::LRU && numWays > 256,
             "LeewayL2 LRU mode stores stack positions in uint8_t");
    fatal_if(mode == Mode::NRU && (nruBits == 0 || nruBits > 8),
             "LeewayL2 NRU mode requires 1 <= nru_bits <= 8");
    fatal_if(ldIncreaseThreshold == 0 || ldDecreaseThreshold == 0,
             "LeewayL2 LDPT thresholds must be non-zero");

    for (auto &entry : ldpt) {
        entry.stableLd = maxLd;
    }
}

bool
LeewayL2::isSampledSet(unsigned set_id) const
{
    if (sampleSets == 0) {
        return false;
    }
    if (sampleSets >= numSets) {
        return true;
    }

    return ((set_id * sampleSets) % numSets) < sampleSets;
}

uint32_t
LeewayL2::getSignature(const PacketPtr pkt) const
{
    if (!pkt) {
        return NoSig;
    }

    if (pkt->req && pkt->req->isPrefetch()) {
        return PrefetchSig;
    }

    if (pkt->isWriteback()) {
        return WritebackSig;
    }

    if (!pkt->req || !pkt->req->hasPC()) {
        if (requirePcSignature) {
            stats.noPcSkipTrain++;
            return NoSig;
        }
        return NoSig;
    }

    const Addr pc = pkt->req->getPC();
    uint32_t sig = static_cast<uint32_t>((pc ^ (pc >> 2) ^ (pc >> 16)) %
                                         ldptEntries);
    if (sig < SpecialSigs) {
        sig += SpecialSigs;
        if (sig >= ldptEntries) {
            sig %= ldptEntries;
        }
    }

    return sig;
}

LeewayL2::LDPTEntry &
LeewayL2::getEntry(uint32_t sig) const
{
    assert(sig < ldptEntries);
    return ldpt[sig];
}

void
LeewayL2::updateEntry(uint32_t sig, int16_t current_ld) const
{
    assert(sig != NoSig);
    assert(sig < ldptEntries);
    assert(current_ld <= maxLd);

    auto &entry = getEntry(sig);

    if (entry.stableLd == current_ld) {
        entry.varianceConf = 0;
        return;
    }

    if (current_ld > entry.stableLd) {
        if (entry.varianceDir == 1) {
            entry.varianceDir = 0;
            entry.varianceConf = 1;
        } else if (entry.varianceConf < UINT8_MAX) {
            entry.varianceConf++;
        }

        if (entry.varianceConf >= ldIncreaseThreshold) {
            entry.varianceConf = 0;
            entry.stableLd = current_ld;
        }
    } else {
        if (entry.varianceDir == 0) {
            entry.varianceDir = 1;
            entry.varianceConf = 1;
        } else if (entry.varianceConf < UINT8_MAX) {
            entry.varianceConf++;
        }

        if (entry.varianceConf >= ldDecreaseThreshold) {
            entry.varianceConf = 0;
            entry.stableLd = current_ld;
        }
    }
}

void
LeewayL2::train(const std::shared_ptr<LeewayL2ReplData> &data) const
{
    if (!data->valid || !data->isSampledSet || data->signature == NoSig) {
        return;
    }

    updateEntry(data->signature, data->currentLd);
    stats.leaderTrainings++;
}

void
LeewayL2::promoteLRU(LeewayL2ReplData &data)
{
    const uint8_t old_pos = data.nruVal;
    assert(data.setId < entriesBySet.size());

    for (auto *peer : entriesBySet[data.setId]) {
        if (!peer || peer == &data || !peer->valid) {
            continue;
        }
        if (peer->nruVal < old_pos && peer->nruVal < maxNruVal) {
            peer->nruVal++;
        }
    }

    data.nruVal = 0;
    stats.lruPromotions++;
}

void
LeewayL2::promote(const std::shared_ptr<LeewayL2ReplData> &data)
{
    if (mode == Mode::LRU) {
        promoteLRU(*data);
    } else {
        data->nruVal = 0;
    }
}

void
LeewayL2::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<LeewayL2ReplData>(replacement_data);

    train(data);

    data->valid = false;
    data->signature = NoSig;
    data->currentLd = BypassLd;
    data->predictedLd = maxLd;
    data->nruVal = maxNruVal;
}

void
LeewayL2::touch(const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<LeewayL2ReplData>(replacement_data);

    if (!data->valid) {
        return;
    }

    if (pkt && pkt->isWriteback()) {
        return;
    }

    if (data->isSampledSet && data->signature != NoSig) {
        data->currentLd = std::max(data->currentLd,
            static_cast<int16_t>(std::min<int>(data->nruVal, maxLd)));
    }

    data->predictedLd = std::max(data->predictedLd,
        static_cast<int16_t>(std::min<int>(data->nruVal, maxLd)));

    promote(data);
}

void
LeewayL2::touch(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<LeewayL2ReplData>(replacement_data);
    data->nruVal = 0;
}

void
LeewayL2::reset(const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<LeewayL2ReplData>(replacement_data);

    train(data);

    const uint32_t sig = getSignature(pkt);
    data->signature = NoSig;
    data->currentLd = BypassLd;
    data->predictedLd = (sig == NoSig) ? maxLd : getEntry(sig).stableLd;
    data->valid = true;

    if (data->isSampledSet && sig != NoSig) {
        data->signature = sig;
        stats.sampledSetRefills++;
    } else {
        stats.nonSampledSetRefills++;
    }

    promote(data);
}

void
LeewayL2::reset(const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<LeewayL2ReplData>(replacement_data);

    data->valid = true;
    data->signature = NoSig;
    data->currentLd = BypassLd;
    data->predictedLd = maxLd;
    data->nruVal = 0;
}

void
LeewayL2::ageNRUSet(const ReplacementCandidates &candidates) const
{
    bool aged = false;
    for (const auto &candidate : candidates) {
        auto data = std::static_pointer_cast<LeewayL2ReplData>(
            candidate->replacementData);
        if (data->valid && data->nruVal < maxNruVal) {
            data->nruVal++;
            aged = true;
        }
    }

    if (aged) {
        stats.nruAgingEvents++;
    }
}

ReplaceableEntry *
LeewayL2::findOldest(const ReplacementCandidates &candidates) const
{
    ReplaceableEntry *victim = candidates[0];
    auto victim_data = std::static_pointer_cast<LeewayL2ReplData>(
        victim->replacementData);

    for (const auto &candidate : candidates) {
        auto data = std::static_pointer_cast<LeewayL2ReplData>(
            candidate->replacementData);
        if (data->nruVal > victim_data->nruVal) {
            victim = candidate;
            victim_data = data;
        }
    }

    return victim;
}

ReplaceableEntry*
LeewayL2::getVictim(const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());

    ReplaceableEntry *dead_victim = nullptr;
    auto best_dead_data = std::shared_ptr<LeewayL2ReplData>();

    for (const auto &candidate : candidates) {
        auto data = std::static_pointer_cast<LeewayL2ReplData>(
            candidate->replacementData);

        if (!data->valid) {
            return candidate;
        }

        const bool is_dead = data->predictedLd < maxLd &&
            data->nruVal > data->predictedLd;
        if (is_dead && (!dead_victim || data->nruVal > best_dead_data->nruVal)) {
            dead_victim = candidate;
            best_dead_data = data;
        }
    }

    if (dead_victim) {
        stats.deadVictims++;
        return dead_victim;
    }

    ReplaceableEntry *victim = findOldest(candidates);
    auto victim_data = std::static_pointer_cast<LeewayL2ReplData>(
        victim->replacementData);

    if (mode == Mode::NRU && victim_data->nruVal < maxNruVal) {
        ageNRUSet(candidates);
        victim = findOldest(candidates);
    }

    stats.fallbackVictims++;
    return victim;
}

std::shared_ptr<ReplacementData>
LeewayL2::instantiateEntry()
{
    auto *data = new LeewayL2ReplData();

    const unsigned set_id = entryCount / numWays;
    const unsigned way_id = entryCount % numWays;
    fatal_if(set_id >= numSets,
             "LeewayL2 instantiated more entries than num_sets*num_ways");

    data->setId = set_id;
    data->wayId = way_id;
    data->isSampledSet = isSampledSet(set_id);
    data->nruVal = maxNruVal;
    data->predictedLd = maxLd;

    entriesBySet[set_id].push_back(data);
    entryCount++;

    return std::shared_ptr<ReplacementData>(data);
}

LeewayL2::LeewayStats::LeewayStats(statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(deadVictims, "Victims selected by Leeway dead-block prediction"),
    ADD_STAT(fallbackVictims, "Victims selected by fallback recency policy"),
    ADD_STAT(leaderTrainings, "LDPT trainings from sampled sets"),
    ADD_STAT(noPcSkipTrain, "Ordinary accesses skipped because no PC exists"),
    ADD_STAT(lruPromotions, "LRU promotions performed"),
    ADD_STAT(nruAgingEvents, "NRU set aging events performed"),
    ADD_STAT(sampledSetRefills, "Refills in sampled sets with valid signature"),
    ADD_STAT(nonSampledSetRefills,
             "Refills not recorded as sampled training entries")
{
}

} // namespace replacement_policy
} // namespace gem5
