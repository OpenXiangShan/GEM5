/*
 * Copyright (c) 2026 XiangShan
 * All rights reserved.
 */

#include "mem/cache/replacement_policies/mockingjay_l2_rp.hh"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "params/MockingjayL2RP.hh"

namespace gem5
{

GEM5_DEPRECATED_NAMESPACE(ReplacementPolicy, replacement_policy);
namespace replacement_policy
{

MockingjayL2::MockingjayL2(const Params &p)
  : Base(p),
    numSets(p.num_sets),
    numWays(p.num_ways),
    blockBits(p.block_bits),
    sliceBits(p.slice_bits),
    historyMultiplier(p.history_multiplier),
    agingGranularity(p.aging_granularity),
    sampledSets(p.sampled_sets),
    sampledCacheSetsPerSet(p.sampled_cache_sets_per_set),
    sampledCacheWays(p.sampled_cache_ways),
    sampledTagBits(p.sampled_tag_bits),
    rdpEntries(p.rdp_entries),
    temporalDifferenceThreshold(p.temporal_difference_threshold),
    scanThresholdMargin(p.scan_threshold_margin),
    prefetchPenaltyPercent(p.prefetch_penalty_percent),
    prefetchMinEtr(p.prefetch_min_etr),
    timestampBits(p.timestamp_bits),
    setBits(0),
    sampledCacheSetBits(0),
    infRd(0),
    maxRd(0),
    infEtr(0),
    timestampModulo(0),
    sampledTagMask(0),
    entryCount(0),
    rdp(),
    setClocks(),
    sampledTimestamps(),
    sampledSetSlots(),
    entriesBySet(),
    stats(this)
{
    fatal_if(numSets == 0, "MockingjayL2 requires num_sets > 0");
    fatal_if(numWays == 0, "MockingjayL2 requires num_ways > 0");
    fatal_if(!isPowerOf2(numSets),
             "MockingjayL2 requires num_sets to be a power of two");
    fatal_if(historyMultiplier == 0,
             "MockingjayL2 requires history_multiplier > 0");
    fatal_if(agingGranularity == 0 ||
                 agingGranularity > std::numeric_limits<uint8_t>::max(),
             "MockingjayL2 aging_granularity must be in [1, %u]",
             static_cast<unsigned>(std::numeric_limits<uint8_t>::max()));
    fatal_if(sampledSets == 0 || sampledSets > numSets ||
                 numSets % sampledSets != 0,
             "MockingjayL2 sampled_sets must divide num_sets");
    fatal_if(!isPowerOf2(sampledSets),
             "MockingjayL2 requires sampled_sets to be a power of two");
    fatal_if(sampledCacheSetsPerSet == 0 || sampledCacheWays == 0,
             "MockingjayL2 sampled cache dimensions must be non-zero");
    fatal_if(!isPowerOf2(sampledCacheSetsPerSet),
             "MockingjayL2 requires sampled_cache_sets_per_set to be a "
             "power of two");
    fatal_if(rdpEntries < 2,
             "MockingjayL2 requires at least two RDP entries");
    fatal_if(!isPowerOf2(rdpEntries),
             "MockingjayL2 requires rdp_entries to be a power of two");
    fatal_if(temporalDifferenceThreshold == 0,
             "MockingjayL2 temporal_difference_threshold must be non-zero");
    fatal_if(prefetchPenaltyPercent < 100,
             "MockingjayL2 prefetch_penalty_percent must be at least 100");
    fatal_if(timestampBits == 0 || timestampBits > 15,
             "MockingjayL2 timestamp_bits must be in [1, 15]");
    fatal_if(sampledTagBits == 0 || sampledTagBits > 63,
             "MockingjayL2 sampled_tag_bits must be in [1, 63]");

    setBits = floorLog2(numSets);
    sampledCacheSetBits = floorLog2(sampledCacheSetsPerSet);
    constexpr unsigned addrBits = std::numeric_limits<Addr>::digits;
    fatal_if(blockBits >= addrBits,
             "MockingjayL2 block_bits must be smaller than the %u-bit Addr "
             "width", addrBits);
    fatal_if(sliceBits >= addrBits,
             "MockingjayL2 slice_bits must be smaller than the %u-bit Addr "
             "width", addrBits);
    fatal_if(blockBits >= addrBits - sliceBits,
             "MockingjayL2 block_bits + slice_bits must leave an address "
             "bit");
    const unsigned localAddrBits = addrBits - blockBits - sliceBits;
    fatal_if(setBits + sampledCacheSetBits >= localAddrBits,
             "MockingjayL2 sampled address fields must leave a tag bit");

    const uint64_t raw_history =
        static_cast<uint64_t>(numWays) * historyMultiplier;
    fatal_if(raw_history < 2 ||
                 raw_history > std::numeric_limits<uint16_t>::max(),
             "MockingjayL2 reuse-distance range is unsupported");
    fatal_if(scanThresholdMargin >= raw_history - 1,
             "MockingjayL2 scan_threshold_margin is too large");

    const uint64_t coarse_history = raw_history / agingGranularity;
    fatal_if(coarse_history < 2 ||
                 coarse_history > std::numeric_limits<int16_t>::max(),
             "MockingjayL2 ETR range is unsupported");

    const unsigned timestamp_modulo = 1U << timestampBits;
    fatal_if(timestamp_modulo <= raw_history,
             "MockingjayL2 timestamp_bits must encode a modulus greater "
             "than the sampled history window");

    const uint64_t cache_entries =
        static_cast<uint64_t>(numSets) * numWays;
    fatal_if(cache_entries > std::numeric_limits<unsigned>::max(),
             "MockingjayL2 cache geometry exceeds the supported entry "
             "count");

    const uint64_t sampled_cache_buckets =
        static_cast<uint64_t>(sampledSets) * sampledCacheSetsPerSet;
    fatal_if(sampled_cache_buckets > sampledCache.max_size(),
             "MockingjayL2 sampled cache geometry exceeds vector capacity");

    infRd = static_cast<uint16_t>(raw_history - 1);
    maxRd = static_cast<uint16_t>(infRd - scanThresholdMargin);
    infEtr = static_cast<int16_t>(coarse_history - 1);
    fatal_if(prefetchMinEtr > static_cast<unsigned>(infEtr),
             "MockingjayL2 prefetch_min_etr must not exceed INF_ETR");
    timestampModulo = static_cast<uint16_t>(timestamp_modulo);
    sampledTagMask = (uint64_t(1) << sampledTagBits) - 1;

    rdp.assign(static_cast<std::size_t>(rdpEntries) + 1, RdpEntry());
    setClocks.assign(numSets, 0);
    sampledTimestamps.assign(numSets, 0);
    sampledSetSlots.assign(numSets, std::numeric_limits<std::size_t>::max());
    entriesBySet.resize(numSets);

    std::size_t sampled_slot = 0;
    for (unsigned set_id = 0; set_id < numSets; ++set_id) {
        setClocks[set_id] = agingGranularity;
        if (isSampledSet(set_id)) {
            sampledSetSlots[set_id] = sampled_slot++;
        }
    }
    fatal_if(sampled_slot != sampledSets,
             "MockingjayL2 sampled-set selection produced %zu sets, expected %u",
             sampled_slot, sampledSets);

    sampledCache.assign(
        static_cast<std::size_t>(sampled_cache_buckets),
        std::vector<SampledEntry>(sampledCacheWays));
}

uint64_t
MockingjayL2::hash(uint64_t value)
{
    constexpr uint64_t crc_polynomial = 3988292384ULL;
    for (unsigned i = 0; i < 3; ++i) {
        value = (value & 1) ? ((value >> 1) ^ crc_polynomial) :
                              (value >> 1);
    }
    return value;
}

MockingjayL2::RdpEntry&
MockingjayL2::rdpEntry(uint32_t signature) const
{
    assert(signature == NoPcSignature || signature < rdpEntries);
    return signature == NoPcSignature ? rdp[rdpEntries] : rdp[signature];
}

bool
MockingjayL2::isSampledSet(unsigned set_id) const
{
    assert(set_id < numSets);
    if (sampledSets == 1) {
        return set_id == 0;
    }

    const unsigned mask_bits = setBits - floorLog2(sampledSets);
    const unsigned mask = (1U << mask_bits) - 1;
    return (set_id & mask) == ((set_id >> (setBits - mask_bits)) & mask);
}

bool
MockingjayL2::isPrefetch(const PacketPtr pkt) const
{
    return pkt && (pkt->cmd.isPrefetch() ||
                   (pkt->req && pkt->req->isPrefetch()));
}

bool
MockingjayL2::isTrainingAccess(const PacketPtr pkt) const
{
    if (!pkt || pkt->isWriteback() || pkt->isEviction() ||
        pkt->cmd.isSWPrefetch() || pkt->cmd == MemCmd::WriteClean ||
        (pkt->req && pkt->req->isCacheMaintenance())) {
        return false;
    }

    // Miss packets turn into ordinary reads below the originating cache, but
    // retain Request::PREFETCH. Software prefetch clones have no PC there;
    // do not let them train the reserved no-PC predictor entry. A no-PC
    // hardware prefetch is likewise not useful for a PC-indexed predictor.
    return !isPrefetch(pkt) || (pkt->req && pkt->req->hasPC());
}

int16_t
MockingjayL2::applyPrefetchPriority(int16_t etr, const PacketPtr pkt) const
{
    if (!isPrefetch(pkt)) {
        return etr;
    }

    return std::max(etr, static_cast<int16_t>(prefetchMinEtr));
}

uint32_t
MockingjayL2::getSignature(const PacketPtr pkt, bool hit) const
{
    if (!pkt || !pkt->req || !pkt->req->hasPC()) {
        if (pkt) {
            stats.noPcSignatures++;
        }
        return NoPcSignature;
    }

    uint64_t input = pkt->req->getPC();
    input = (input << 1) | static_cast<uint64_t>(hit);
    input = (input << 1) | static_cast<uint64_t>(isPrefetch(pkt));

    return static_cast<uint32_t>(hash(input) & (rdpEntries - 1));
}

uint16_t
MockingjayL2::elapsed(uint16_t current, uint16_t previous) const
{
    return current >= previous ? current - previous :
                                 current + timestampModulo - previous;
}

uint64_t
MockingjayL2::sampledTag(Addr addr) const
{
    const uint64_t local_block_addr =
        (addr >> blockBits) >> sliceBits;
    return (local_block_addr >> (setBits + sampledCacheSetBits)) &
        sampledTagMask;
}

std::size_t
MockingjayL2::sampledCacheIndex(unsigned set_id, Addr addr) const
{
    assert(set_id < sampledSetSlots.size());
    const std::size_t sampled_slot = sampledSetSlots[set_id];
    assert(sampled_slot != std::numeric_limits<std::size_t>::max());

    const uint64_t local_block_addr =
        (addr >> blockBits) >> sliceBits;
    const unsigned bucket_offset = static_cast<unsigned>(
        (local_block_addr >> setBits) & (sampledCacheSetsPerSet - 1));
    return static_cast<std::size_t>(sampled_slot) *
        sampledCacheSetsPerSet + bucket_offset;
}

uint16_t
MockingjayL2::prefetchAdjustedDistance(uint16_t distance,
                                       const PacketPtr pkt) const
{
    if (!isPrefetch(pkt)) {
        return distance;
    }

    const uint64_t adjusted = static_cast<uint64_t>(distance) *
        prefetchPenaltyPercent / 100;
    return static_cast<uint16_t>(std::min<uint64_t>(adjusted, infRd));
}

void
MockingjayL2::trainReuse(uint32_t signature, uint16_t distance) const
{
    stats.reuseTrainings++;
    distance = std::min(distance, infRd);

    RdpEntry &entry = rdpEntry(signature);
    if (!entry.valid) {
        entry.valid = true;
        entry.reuseDistance = distance;
        return;
    }

    const unsigned difference = entry.reuseDistance > distance ?
        entry.reuseDistance - distance : distance - entry.reuseDistance;
    if (difference < temporalDifferenceThreshold) {
        return;
    }

    if (distance > entry.reuseDistance) {
        entry.reuseDistance = std::min<uint16_t>(entry.reuseDistance + 1,
                                                 infRd);
    } else if (distance < entry.reuseDistance) {
        entry.reuseDistance--;
    }
}

void
MockingjayL2::trainScan(uint32_t signature) const
{
    stats.scanTrainings++;

    RdpEntry &entry = rdpEntry(signature);
    if (!entry.valid) {
        entry.valid = true;
        entry.reuseDistance = infRd;
    } else if (entry.reuseDistance < infRd) {
        // The public Mockingjay reference increments once on a scan.
        entry.reuseDistance++;
    }
}

void
MockingjayL2::processSampledAccess(const MockingjayReplData &data,
                                   const PacketPtr pkt,
                                   uint32_t signature) const
{
    if (!pkt || !isSampledSet(data.setId)) {
        return;
    }

    const std::size_t index = sampledCacheIndex(data.setId, pkt->getAddr());
    std::vector<SampledEntry> &bucket = sampledCache[index];
    const uint64_t tag = sampledTag(pkt->getAddr());
    const uint16_t timestamp = sampledTimestamps[data.setId];
    SampledEntry *match = nullptr;

    for (SampledEntry &entry : bucket) {
        if (entry.valid && entry.tag == tag) {
            match = &entry;
            break;
        }
    }

    if (match) {
        stats.sampledHits++;
        const uint16_t distance = elapsed(timestamp, match->timestamp);
        if (distance <= infRd) {
            trainReuse(match->signature,
                       prefetchAdjustedDistance(distance, pkt));
        } else {
            trainScan(match->signature);
        }
        match->valid = false;
    } else {
        stats.sampledMisses++;
    }

    bool has_free_entry = false;
    SampledEntry *oldest = nullptr;
    uint16_t oldest_distance = 0;
    for (SampledEntry &entry : bucket) {
        if (!entry.valid) {
            has_free_entry = true;
            continue;
        }

        const uint16_t distance = elapsed(timestamp, entry.timestamp);
        if (distance > infRd) {
            // The reference detrain pass invalidates every stale entry in the
            // sampled bucket before it installs the current access.
            trainScan(entry.signature);
            entry.valid = false;
            has_free_entry = true;
        } else if (!oldest || distance > oldest_distance) {
            oldest = &entry;
            oldest_distance = distance;
        }
    }

    if (!has_free_entry) {
        assert(oldest);
        trainScan(oldest->signature);
        oldest->valid = false;
    }

    SampledEntry *insertion = nullptr;
    for (SampledEntry &entry : bucket) {
        if (!entry.valid) {
            insertion = &entry;
            break;
        }
    }
    assert(insertion);
    insertion->valid = true;
    insertion->tag = tag;
    insertion->signature = signature;
    insertion->timestamp = timestamp;
    sampledTimestamps[data.setId] =
        static_cast<uint16_t>((timestamp + 1) % timestampModulo);
}

int16_t
MockingjayL2::predictEtr(uint32_t signature) const
{
    stats.rdpLookups++;

    const RdpEntry &entry = rdpEntry(signature);
    if (!entry.valid) {
        stats.rdpMisses++;
        return 0;
    }

    stats.rdpHits++;
    if (entry.reuseDistance > maxRd) {
        return infEtr;
    }

    return static_cast<int16_t>(std::min<unsigned>(
        entry.reuseDistance / agingGranularity, infEtr));
}

void
MockingjayL2::ageSet(unsigned set_id,
                     const MockingjayReplData *accessed_data) const
{
    uint8_t &clock = setClocks[set_id];
    if (clock == agingGranularity) {
        for (MockingjayReplData *entry : entriesBySet[set_id]) {
            if (entry != accessed_data && entry->valid &&
                entry->etr > -infEtr && entry->etr < infEtr) {
                entry->etr--;
            }
        }
        clock = 0;
        stats.agingEvents++;
    }
    clock++;
}

ReplaceableEntry*
MockingjayL2::selectVictim(const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());

    ReplaceableEntry *victim = candidates.front();
    auto victim_data = std::static_pointer_cast<MockingjayReplData>(
        victim->replacementData);

    for (ReplaceableEntry *candidate : candidates) {
        auto data = std::static_pointer_cast<MockingjayReplData>(
            candidate->replacementData);
        if (!data->valid) {
            return candidate;
        }

        const int candidate_distance = data->etr < 0 ? -data->etr : data->etr;
        const int victim_distance = victim_data->etr < 0 ?
            -victim_data->etr : victim_data->etr;
        if (!victim_data->valid || candidate_distance > victim_distance ||
            (candidate_distance == victim_distance &&
             data->etr < victim_data->etr)) {
            victim = candidate;
            victim_data = data;
        }
    }

    return victim;
}

void
MockingjayL2::recordVictim(const MockingjayReplData &data) const
{
    if (!data.valid) {
        stats.invalidVictims++;
    } else if (data.etr < 0) {
        stats.negativeEtrVictims++;
    } else {
        stats.positiveEtrVictims++;
    }
}

void
MockingjayL2::invalidate(
    const std::shared_ptr<ReplacementData>& replacement_data)
{
    auto data = std::static_pointer_cast<MockingjayReplData>(replacement_data);
    data->valid = false;
    data->etr = infEtr;
}

void
MockingjayL2::touch(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<MockingjayReplData>(replacement_data);
    if (!data->valid || !isTrainingAccess(pkt)) {
        return;
    }

    const uint32_t signature = getSignature(pkt, true);
    processSampledAccess(*data, pkt, signature);
    ageSet(data->setId, data.get());
    data->etr = predictEtr(signature);
    stats.promotions++;
}

void
MockingjayL2::touch(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<MockingjayReplData>(replacement_data);
    if (data->valid) {
        data->etr = 0;
    }
}

void
MockingjayL2::reset(
    const std::shared_ptr<ReplacementData>& replacement_data,
    const PacketPtr pkt)
{
    auto data = std::static_pointer_cast<MockingjayReplData>(replacement_data);
    const int16_t victim_etr = data->victimEtr;
    const bool has_victim_etr = data->hasVictimEtr;
    data->hasVictimEtr = false;

    if (!pkt) {
        data->valid = true;
        data->etr = 0;
        return;
    }

    if (pkt->isWriteback()) {
        data->valid = true;
        data->etr = -infEtr;
        stats.writebackInsertions++;
        return;
    }

    if (!isTrainingAccess(pkt)) {
        const bool is_prefetch = isPrefetch(pkt);
        data->valid = true;
        data->etr = applyPrefetchPriority(0, pkt);
        if (is_prefetch) {
            stats.prefetchInsertions++;
            if (data->etr != 0) {
                stats.prefetchFloorInsertions++;
            }
        }
        return;
    }

    const bool is_prefetch = isPrefetch(pkt);
    const uint32_t signature = getSignature(pkt, false);
    const RdpEntry pre_training_prediction = rdpEntry(signature);
    const int16_t pre_training_etr = predictEtr(signature);
    const bool predicts_scan = pre_training_prediction.valid &&
        pre_training_prediction.reuseDistance > maxRd;
    processSampledAccess(*data, pkt, signature);
    ageSet(data->setId, data.get());
    data->valid = true;
    const int16_t trained_etr = predictEtr(signature);
    data->etr = applyPrefetchPriority(trained_etr, pkt);
    const int victim_distance = victim_etr < 0 ? -victim_etr : victim_etr;
    const bool force_max_etr = predicts_scan ||
        (has_victim_etr && pre_training_etr > victim_distance);
    if (force_max_etr) {
        // Preserve the pre-training replacement-priority decision while the
        // line follows the normal cache fill path.
        data->etr = infEtr;
        stats.maxEtrInsertions++;
    }
    if (is_prefetch) {
        stats.prefetchInsertions++;
        if (!force_max_etr &&
            trained_etr < static_cast<int16_t>(prefetchMinEtr)) {
            stats.prefetchFloorInsertions++;
        }
    }
    stats.insertions++;
}

void
MockingjayL2::reset(
    const std::shared_ptr<ReplacementData>& replacement_data) const
{
    auto data = std::static_pointer_cast<MockingjayReplData>(replacement_data);
    data->valid = true;
    data->etr = 0;
}

ReplaceableEntry*
MockingjayL2::getVictim(const ReplacementCandidates& candidates) const
{
    ReplaceableEntry *victim = selectVictim(candidates);
    auto data = std::static_pointer_cast<MockingjayReplData>(
        victim->replacementData);
    data->victimEtr = data->etr;
    data->hasVictimEtr = data->valid;
    recordVictim(*data);
    return victim;
}

std::shared_ptr<ReplacementData>
MockingjayL2::instantiateEntry()
{
    auto *data = new MockingjayReplData();
    const unsigned set_id = entryCount / numWays;
    const unsigned way_id = entryCount % numWays;
    fatal_if(set_id >= numSets,
             "MockingjayL2 instantiated more entries than num_sets*num_ways");

    data->setId = set_id;
    data->wayId = way_id;
    data->etr = infEtr;
    entriesBySet[set_id].push_back(data);
    entryCount++;

    return std::shared_ptr<ReplacementData>(data);
}

MockingjayL2::MockingjayStats::MockingjayStats(
    statistics::Group *parent)
  : statistics::Group(parent),
    ADD_STAT(sampledHits, "Sampled-cache hits"),
    ADD_STAT(sampledMisses, "Sampled-cache misses"),
    ADD_STAT(reuseTrainings, "RDP updates from observed reuses"),
    ADD_STAT(scanTrainings, "RDP updates from sampled scans"),
    ADD_STAT(rdpLookups, "RDP prediction lookups"),
    ADD_STAT(rdpHits, "RDP prediction lookups with a trained entry"),
    ADD_STAT(rdpMisses, "RDP prediction lookups without a trained entry"),
    ADD_STAT(noPcSignatures, "Accesses assigned to the no-PC signature"),
    ADD_STAT(promotions, "ETR updates on cache hits"),
    ADD_STAT(insertions, "ETR updates on cache fills"),
    ADD_STAT(writebackInsertions, "Writeback fills assigned a scan ETR"),
    ADD_STAT(prefetchInsertions,
             "Prefetch fills assigned prefetch-aware insertion priority"),
    ADD_STAT(prefetchFloorInsertions,
             "Prefetch fills finally inserted at the configured ETR floor"),
    ADD_STAT(agingEvents, "Per-set periodic ETR aging events"),
    ADD_STAT(maxEtrInsertions,
             "Fills admitted with maximum positive ETR for rapid replacement"),
    ADD_STAT(positiveEtrVictims,
             "Selected candidates with non-negative ETR"),
    ADD_STAT(negativeEtrVictims,
             "Selected candidates with negative ETR"),
    ADD_STAT(invalidVictims,
             "Invalid candidates selected for insertion")
{
}

} // namespace replacement_policy
} // namespace gem5
