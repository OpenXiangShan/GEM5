/*
 * Copyright (c) 2026 Beijing Institute of Open Source Chip
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/cache/prefetch/step.hh"

#include <climits>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/XSCompositePrefetcher.hh"
#include "mem/cache/prefetch/associative_set_impl.hh"
#include "mem/cache/prefetch/context_key.hh"

namespace gem5
{
namespace prefetch
{

StepSpatialPrefetcher::StepSpatialPrefetcher(
    const XSCompositePrefetcherParams &p, statistics::Group *parent)
    : stats(parent),
      regionSize(p.step_region_size),
      blockSize(p.block_size),
      regionBlks(p.step_region_size / p.block_size),
      pcHashBits(p.step_pc_hash_bits),
      confidenceEntries(p.step_confidence_entries),
      confidenceThreshold(p.step_confidence_threshold),
      enableFoe(p.step_enable_foe),
      enableSoe(p.step_enable_soe),
      enableToe(p.step_enable_toe),
      filterTable(p.step_ft_assoc, p.step_ft_entries,
                  p.step_ft_indexing_policy,
                  p.step_ft_replacement_policy, FilterEntry()),
      activeTable(p.step_act_assoc, p.step_act_entries,
                  p.step_act_indexing_policy, p.step_act_replacement_policy,
                  ActiveEntry()),
      patternTable(p.step_pht_assoc, p.step_pht_entries,
                   p.step_pht_indexing_policy,
                   p.step_pht_replacement_policy, PatternEntry())
{
    validateConfig(p.step_pht_assoc, p.step_pf_level);
}

StepSpatialPrefetcher::StepSpatialPrefetcher(
    const Config &config, statistics::Group *parent)
    : stats(parent),
      regionSize(config.regionSize),
      blockSize(config.blockSize),
      regionBlks(config.regionSize / config.blockSize),
      pcHashBits(config.pcHashBits),
      confidenceEntries(config.confidenceEntries),
      confidenceThreshold(config.confidenceThreshold),
      enableFoe(config.enableFoe),
      enableSoe(config.enableSoe),
      enableToe(config.enableToe),
      filterTable(config.ftAssoc, config.ftEntries, config.ftIndexingPolicy,
                  config.ftReplacementPolicy, FilterEntry()),
      activeTable(config.atAssoc, config.atEntries, config.atIndexingPolicy,
                  config.atReplacementPolicy, ActiveEntry()),
      patternTable(config.phtAssoc, config.phtEntries,
                   config.phtIndexingPolicy, config.phtReplacementPolicy,
                   PatternEntry())
{
    validateConfig(config.phtAssoc, 2);
}

void
StepSpatialPrefetcher::validateConfig(unsigned pht_assoc, int pf_level) const
{
    fatal_if(!isPowerOf2(regionSize),
             "STEP region size must be a power of two: %u", regionSize);
    fatal_if(regionSize % blockSize != 0 || regionBlks == 0 ||
                 regionBlks > 64,
             "STEP requires 1 to 64 blocks per region (size=%u, block=%u)",
             regionSize, blockSize);
    fatal_if(pcHashBits == 0 || pcHashBits > 16,
             "STEP PC hash width must be in [1, 16]: %u", pcHashBits);
    fatal_if(confidenceEntries == 0 ||
                 confidenceEntries > MaxHistoryEntries ||
                 confidenceEntries > pht_assoc,
             "STEP confidence entries must be in [1, min(%u, PHT assoc)]: %u",
             MaxHistoryEntries, confidenceEntries);
    fatal_if(confidenceThreshold > 100,
             "STEP confidence threshold must be in [0, 100]: %u",
             confidenceThreshold);
    fatal_if(pf_level < 1 || pf_level > 3,
             "STEP prefetch target level must be in [1, 3]: %d",
             pf_level);
}

Addr
StepSpatialPrefetcher::regionAddress(Addr address) const
{
    return address / regionSize;
}

uint8_t
StepSpatialPrefetcher::regionOffset(Addr address) const
{
    return (address / blockSize) % regionBlks;
}

uint64_t
StepSpatialPrefetcher::offsetBit(uint8_t offset) const
{
    assert(offset < regionBlks);
    return uint64_t(1) << offset;
}

uint16_t
StepSpatialPrefetcher::hashPc(Addr pc) const
{
    const uint64_t mask = (uint64_t(1) << pcHashBits) - 1;
    const uint64_t folded = (pc >> 1) ^ (pc >> 17) ^ (pc >> 34) ^
        (pc >> 51);
    return static_cast<uint16_t>(folded & mask);
}

Addr
StepSpatialPrefetcher::regionKey(Addr region, ContextID context_id) const
{
    return contextKey(region, tableContext(context_id));
}

Addr
StepSpatialPrefetcher::patternKey(uint8_t first_offset,
                                  ContextID context_id) const
{
    return contextKey(first_offset, tableContext(context_id));
}

ContextID
StepSpatialPrefetcher::tableContext(ContextID context_id) const
{
    // contextKey() intentionally folds InvalidContextID into context zero to
    // preserve legacy single-context indexing. STEP is a new table and needs
    // the two ownership domains to remain distinct. Exact payload checks below
    // still protect against any finite-width key collision.
    return context_id == InvalidContextID ? INT_MAX : context_id;
}

StepSpatialPrefetcher::FilterEntry *
StepSpatialPrefetcher::findFilterEntry(Addr region, ContextID context_id,
                                       bool secure)
{
    const Addr key = regionKey(region, context_id);
    for (FilterEntry *entry : filterTable.getPossibleEntries(key)) {
        if (entry->isValid() && entry->isSecure() == secure &&
            entry->region == region && entry->contextId == context_id) {
            return entry;
        }
    }
    return nullptr;
}

StepSpatialPrefetcher::ActiveEntry *
StepSpatialPrefetcher::findActiveEntry(Addr region, ContextID context_id,
                                       bool secure)
{
    const Addr key = regionKey(region, context_id);
    for (ActiveEntry *entry : activeTable.getPossibleEntries(key)) {
        if (entry->isValid() && entry->isSecure() == secure &&
            entry->region == region && entry->contextId == context_id) {
            return entry;
        }
    }
    return nullptr;
}

StepSpatialPrefetcher::PatternEntry *
StepSpatialPrefetcher::findPatternVictim(Addr key, bool *victim_secure,
                                         bool *victim_valid)
{
    // LRU timestamps can tie for multiple learn events in one simulation
    // cycle. Prefer an invalid way explicitly so a burst of completed regions
    // can populate the recent history instead of repeatedly overwriting way 0.
    for (PatternEntry *entry : patternTable.getPossibleEntries(key)) {
        if (!entry->isValid()) {
            if (victim_secure) {
                *victim_secure = entry->isSecure();
            }
            if (victim_valid) {
                *victim_valid = false;
            }
            patternTable.invalidate(entry);
            return entry;
        }
    }
    // No invalid way was available, so the replacement policy must select a
    // valid entry. Its payload remains readable after invalidation, allowing
    // the caller to apply STEP's maturity heuristic without extending the
    // shared AssociativeSet interface.
    if (victim_valid) {
        *victim_valid = true;
    }
    return patternTable.findVictim(key, victim_secure);
}

std::optional<StepSpatialPrefetcher::Decision>
StepSpatialPrefetcher::observe(Addr address, Addr pc, ContextID context_id,
                                bool secure, bool allow_issue)
{
    stats.demandObservations++;
    const Addr region = regionAddress(address);
    const uint8_t offset = regionOffset(address);
    const uint64_t bit = offsetBit(offset);
    const Addr key = regionKey(region, context_id);

    ActiveEntry *active_entry = findActiveEntry(region, context_id, secure);
    if (active_entry) {
        activeTable.accessEntry(active_entry);
        active_entry->footprint |= bit;
        return std::nullopt;
    }

    FilterEntry *filter_entry = findFilterEntry(region, context_id, secure);
    if (!filter_entry) {
        filter_entry = filterTable.findVictim(key);
        filter_entry->region = region;
        filter_entry->contextId = context_id;
        filter_entry->firstOffset = offset;
        filter_entry->secondOffset = InvalidOffset;
        filter_entry->pcHash = hashPc(pc);
        filter_entry->issued = false;
        filter_entry->generation = ++nextFilterGeneration;
        filter_entry->pendingDecisionId = 0;
        filter_entry->pendingBlocks = 0;
        filterTable.insertEntry(key, secure, filter_entry);
        stats.ftAllocations++;

        return lookup(*filter_entry, TriggerStage::Foe, InvalidOffset, secure,
                      allow_issue && enableFoe);
    }

    filterTable.accessEntry(filter_entry);
    if (filter_entry->secondOffset == InvalidOffset) {
        filter_entry->secondOffset = offset;
        return lookup(*filter_entry, TriggerStage::Soe, InvalidOffset, secure,
                      allow_issue && !filter_entry->issued &&
                          filter_entry->pendingBlocks == 0 && enableSoe);
    }

    auto decision = lookup(*filter_entry, TriggerStage::Toe, offset, secure,
                           allow_issue && !filter_entry->issued &&
                               filter_entry->pendingBlocks == 0 && enableToe);
    allocateActive(*filter_entry, offset, secure);
    filterTable.invalidate(filter_entry);
    return decision;
}

std::optional<StepSpatialPrefetcher::Decision>
StepSpatialPrefetcher::lookup(FilterEntry &filter_entry, TriggerStage stage,
                               uint8_t third_offset, bool secure,
                               bool allow_issue)
{
    if (!allow_issue) {
        return std::nullopt;
    }

    recordLookup(stage);
    std::array<PatternEntry *, MaxHistoryEntries> candidates{};
    const unsigned count = selectCandidates(filter_entry, stage, third_offset,
                                            secure, candidates);
    if (count == 0) {
        if (stage == TriggerStage::Toe) {
            stats.toeMisses++;
        }
        return std::nullopt;
    }

    recordPhtHit(stage);
    if (stage == TriggerStage::Foe && count == 1 && !candidates[0]->mature) {
        stats.foeDeferredMaturity++;
        return std::nullopt;
    }

    std::array<uint64_t, MaxHistoryEntries> footprints{};
    for (unsigned i = 0; i < count; ++i) {
        footprints[i] = candidates[i]->footprint;
    }

    if (stage != TriggerStage::Toe &&
        !footprintsConverge(footprints, count, confidenceThreshold)) {
        if (stage == TriggerStage::Foe) {
            stats.foeDeferredConfidence++;
        } else {
            stats.soeDeferredConfidence++;
        }
        return std::nullopt;
    }

    const uint64_t observed = offsetBit(filter_entry.firstOffset) |
        (filter_entry.secondOffset == InvalidOffset ? 0 :
         offsetBit(filter_entry.secondOffset)) |
        (third_offset == InvalidOffset ? 0 : offsetBit(third_offset));
    const uint64_t footprint = stage == TriggerStage::Toe ? footprints[0] :
        intersectFootprints(footprints, count);
    const uint64_t candidates_to_issue = footprint & ~observed;
    if (candidates_to_issue == 0) {
        return std::nullopt;
    }

    recordDecision(stage, __builtin_popcountll(candidates_to_issue));
    DPRINTF(XSCompositePrefetcher,
            "STEP %u decision: region=%#lx FO=%u SO=%u TO=%u matches=%u "
            "footprint=%#llx candidates=%#llx\n",
            static_cast<unsigned>(stage), filter_entry.region,
            filter_entry.firstOffset, filter_entry.secondOffset, third_offset,
            count, static_cast<unsigned long long>(footprint),
            static_cast<unsigned long long>(candidates_to_issue));
    return Decision{stage, filter_entry.region, candidates_to_issue, observed,
                    filter_entry.contextId, secure, filter_entry.generation};
}

void
StepSpatialPrefetcher::recordBuffered(const Decision &decision,
                                      uint64_t buffered_blocks)
{
    stats.preBufferFilteredBlocks += __builtin_popcountll(
        decision.candidates & ~buffered_blocks);
    if (buffered_blocks == 0) {
        return;
    }

    recordBuffered(decision.stage, __builtin_popcountll(buffered_blocks));
    FilterEntry *filter_entry = findFilterEntry(decision.region,
                                                decision.contextId,
                                                decision.secure);
    if (!filter_entry || filter_entry->issued ||
        filter_entry->generation != decision.id) {
        return;
    }

    if (filter_entry->pendingBlocks == 0) {
        filter_entry->pendingDecisionId = decision.id;
    }
    if (filter_entry->pendingDecisionId != decision.id) {
        return;
    }

    filter_entry->pendingBlocks += __builtin_popcountll(buffered_blocks);
    filterTable.accessEntry(filter_entry);
}

void
StepSpatialPrefetcher::recordHandoff(Addr region, ContextID context_id,
                                     bool secure, uint64_t decision_id)
{
    if (decision_id == 0) {
        return;
    }

    FilterEntry *filter_entry = findFilterEntry(region, context_id, secure);
    // A PB entry can outlive its source FT entry. The generation prevents an
    // old staged candidate from suppressing a later visit to the same region.
    if (filter_entry && filter_entry->generation == decision_id) {
        filter_entry->issued = true;
        filter_entry->pendingDecisionId = 0;
        filter_entry->pendingBlocks = 0;
        filterTable.accessEntry(filter_entry);
    }
}

void
StepSpatialPrefetcher::recordTerminalFailure(Addr region, ContextID context_id,
                                             bool secure,
                                             uint64_t decision_id)
{
    if (decision_id == 0) {
        return;
    }

    FilterEntry *filter_entry = findFilterEntry(region, context_id, secure);
    if (!filter_entry || filter_entry->issued ||
        filter_entry->generation != decision_id ||
        filter_entry->pendingDecisionId != decision_id ||
        filter_entry->pendingBlocks == 0) {
        return;
    }

    --filter_entry->pendingBlocks;
    if (filter_entry->pendingBlocks == 0) {
        filter_entry->pendingDecisionId = 0;
    }
    filterTable.accessEntry(filter_entry);
}

void
StepSpatialPrefetcher::recordBufferHandoffFiltered()
{
    stats.bufferHandoffFilteredBlocks++;
}

void
StepSpatialPrefetcher::recordBufferHandoffRejected()
{
    stats.bufferHandoffRejectedBlocks++;
}

void
StepSpatialPrefetcher::flushActiveEntriesForTest()
{
    for (ActiveEntry &entry : activeTable) {
        if (entry.isValid()) {
            const bool secure = entry.isSecure();
            train(entry, secure);
            activeTable.invalidate(&entry);
        }
    }
}

unsigned
StepSpatialPrefetcher::selectCandidates(
    const FilterEntry &filter_entry, TriggerStage stage, uint8_t third_offset,
    bool secure,
    std::array<PatternEntry *, MaxHistoryEntries> &candidates)
{
    const unsigned limit = stage == TriggerStage::Toe ? 1 : confidenceEntries;
    unsigned count = 0;
    const auto entries = patternTable.getPossibleEntries(
        patternKey(filter_entry.firstOffset, filter_entry.contextId));

    for (PatternEntry *entry : entries) {
        if (!entry->isValid() || entry->isSecure() != secure ||
            entry->contextId != filter_entry.contextId ||
            entry->firstOffset != filter_entry.firstOffset) {
            continue;
        }

        bool matches = false;
        switch (stage) {
          case TriggerStage::Foe:
            matches = entry->pcHash == filter_entry.pcHash;
            break;
          case TriggerStage::Soe:
            matches = entry->secondOffset == filter_entry.secondOffset;
            break;
          case TriggerStage::Toe:
            matches = entry->secondOffset == filter_entry.secondOffset &&
                entry->thirdOffset == third_offset;
            break;
        }
        if (!matches) {
            continue;
        }

        unsigned insert_pos = count;
        if (count == limit) {
            if (entry->sequence <= candidates[limit - 1]->sequence) {
                continue;
            }
            insert_pos = limit - 1;
        }
        while (insert_pos > 0 &&
               candidates[insert_pos - 1]->sequence < entry->sequence) {
            if (insert_pos < limit) {
                candidates[insert_pos] = candidates[insert_pos - 1];
            }
            --insert_pos;
        }
        candidates[insert_pos] = entry;
        if (count < limit) {
            ++count;
        }
    }

    for (unsigned i = 0; i < count; ++i) {
        patternTable.accessEntry(candidates[i]);
    }
    return count;
}

void
StepSpatialPrefetcher::allocateActive(const FilterEntry &filter_entry,
                                      uint8_t third_offset, bool secure)
{
    const Addr key = regionKey(filter_entry.region, filter_entry.contextId);
    bool victim_secure = false;
    ActiveEntry *victim = activeTable.findVictim(key, &victim_secure);
    train(*victim, victim_secure);

    victim->region = filter_entry.region;
    victim->contextId = filter_entry.contextId;
    victim->firstOffset = filter_entry.firstOffset;
    victim->secondOffset = filter_entry.secondOffset;
    victim->thirdOffset = third_offset;
    victim->pcHash = filter_entry.pcHash;
    victim->footprint = offsetBit(filter_entry.firstOffset) |
        offsetBit(filter_entry.secondOffset) | offsetBit(third_offset);
    victim->hasFootprint = true;
    activeTable.insertEntry(key, secure, victim);
    stats.atAllocations++;
}

void
StepSpatialPrefetcher::train(ActiveEntry &active_entry, bool secure)
{
    if (!active_entry.hasFootprint ||
        active_entry.firstOffset == InvalidOffset) {
        return;
    }
    // findVictim() invalidates before returning. Clear the payload marker so
    // a later allocation of this invalid slot cannot retrain stale state.
    active_entry.hasFootprint = false;

    const Addr key = patternKey(active_entry.firstOffset,
                                active_entry.contextId);
    bool victim_secure = false;
    bool victim_valid = false;
    PatternEntry *victim = findPatternVictim(
        key, &victim_secure, &victim_valid);
    // PHT entries are a recent history, not a key-unique cache: equivalent
    // FO/SO/TO events may carry different completed footprints and must stay
    // available together for the confidence evaluator.  Every AT eviction
    // therefore writes one history position.  The paper's maturity heuristic
    // compares the new entry with the entry displaced from that position.
    const bool same_domain = victim_valid && victim_secure == secure &&
        victim->contextId == active_entry.contextId &&
        victim->firstOffset == active_entry.firstOffset;
    const bool mature = same_domain && victim->pcHash == active_entry.pcHash;
    if (victim_valid) {
        stats.phtVictims++;
    }

    victim->contextId = active_entry.contextId;
    victim->footprint = active_entry.footprint;
    victim->sequence = ++nextSequence;
    victim->firstOffset = active_entry.firstOffset;
    victim->secondOffset = active_entry.secondOffset;
    victim->thirdOffset = active_entry.thirdOffset;
    victim->pcHash = active_entry.pcHash;
    victim->mature = mature;
    patternTable.insertEntry(key, secure, victim);
    stats.phtInsertions++;

    DPRINTF(XSCompositePrefetcher,
            "STEP train: region=%#lx FO=%u SO=%u TO=%u footprint=%#llx "
            "mature=%d\n",
            active_entry.region, active_entry.firstOffset,
            active_entry.secondOffset, active_entry.thirdOffset,
            static_cast<unsigned long long>(active_entry.footprint), mature);
}

void
StepSpatialPrefetcher::recordLookup(TriggerStage stage)
{
    switch (stage) {
      case TriggerStage::Foe:
        stats.foeLookups++;
        break;
      case TriggerStage::Soe:
        stats.soeLookups++;
        break;
      case TriggerStage::Toe:
        stats.toeLookups++;
        break;
    }
}

void
StepSpatialPrefetcher::recordPhtHit(TriggerStage stage)
{
    switch (stage) {
      case TriggerStage::Foe:
        stats.foePhtHits++;
        break;
      case TriggerStage::Soe:
        stats.soePhtHits++;
        break;
      case TriggerStage::Toe:
        stats.toePhtHits++;
        break;
    }
}

void
StepSpatialPrefetcher::recordDecision(TriggerStage stage,
                                      unsigned candidate_blocks)
{
    switch (stage) {
      case TriggerStage::Foe:
        stats.foeDecisions++;
        stats.foeCandidateBlocks += candidate_blocks;
        break;
      case TriggerStage::Soe:
        stats.soeDecisions++;
        stats.soeCandidateBlocks += candidate_blocks;
        break;
      case TriggerStage::Toe:
        stats.toeDecisions++;
        stats.toeCandidateBlocks += candidate_blocks;
        break;
    }
}

void
StepSpatialPrefetcher::recordBuffered(TriggerStage stage,
                                      unsigned buffered_blocks)
{
    switch (stage) {
      case TriggerStage::Foe:
        stats.foeBufferedBlocks += buffered_blocks;
        break;
      case TriggerStage::Soe:
        stats.soeBufferedBlocks += buffered_blocks;
        break;
      case TriggerStage::Toe:
        stats.toeBufferedBlocks += buffered_blocks;
        break;
    }
}

StepSpatialPrefetcher::Stats::Stats(statistics::Group *parent)
    : statistics::Group(parent, "step"),
      ADD_STAT(demandObservations, statistics::units::Count::get(),
               "Eligible demand accesses observed by STEP"),
      ADD_STAT(foeLookups, statistics::units::Count::get(),
               "STEP first-offset PHT lookups"),
      ADD_STAT(soeLookups, statistics::units::Count::get(),
               "STEP second-offset PHT lookups"),
      ADD_STAT(toeLookups, statistics::units::Count::get(),
               "STEP third-offset PHT lookups"),
      ADD_STAT(foePhtHits, statistics::units::Count::get(),
               "STEP first-offset PHT hits"),
      ADD_STAT(soePhtHits, statistics::units::Count::get(),
               "STEP second-offset PHT hits"),
      ADD_STAT(toePhtHits, statistics::units::Count::get(),
               "STEP third-offset PHT hits"),
      ADD_STAT(foeDeferredMaturity, statistics::units::Count::get(),
               "STEP FOE decisions deferred by immature single matches"),
      ADD_STAT(foeDeferredConfidence, statistics::units::Count::get(),
               "STEP FOE decisions deferred by low confidence"),
      ADD_STAT(soeDeferredConfidence, statistics::units::Count::get(),
               "STEP SOE decisions deferred by low confidence"),
      ADD_STAT(toeMisses, statistics::units::Count::get(),
               "STEP TOE exact-match misses"),
      ADD_STAT(foeDecisions, statistics::units::Count::get(),
               "STEP FOE footprint decisions"),
      ADD_STAT(soeDecisions, statistics::units::Count::get(),
               "STEP SOE footprint decisions"),
      ADD_STAT(toeDecisions, statistics::units::Count::get(),
               "STEP TOE footprint decisions"),
      ADD_STAT(foeCandidateBlocks, statistics::units::Count::get(),
               "STEP FOE candidate cache blocks"),
      ADD_STAT(soeCandidateBlocks, statistics::units::Count::get(),
               "STEP SOE candidate cache blocks"),
      ADD_STAT(toeCandidateBlocks, statistics::units::Count::get(),
               "STEP TOE candidate cache blocks"),
      ADD_STAT(foeBufferedBlocks, statistics::units::Count::get(),
               "STEP FOE blocks handed to the finite STEP prefetch buffer"),
      ADD_STAT(soeBufferedBlocks, statistics::units::Count::get(),
               "STEP SOE blocks handed to the finite STEP prefetch buffer"),
      ADD_STAT(toeBufferedBlocks, statistics::units::Count::get(),
               "STEP TOE blocks handed to the finite STEP prefetch buffer"),
      ADD_STAT(preBufferFilteredBlocks, statistics::units::Count::get(),
               "STEP candidates rejected before entering the prefetch buffer"),
      ADD_STAT(bufferHandoffFilteredBlocks, statistics::units::Count::get(),
               "STEP buffer blocks rejected by shared filtering at handoff"),
      ADD_STAT(bufferHandoffRejectedBlocks, statistics::units::Count::get(),
               "STEP buffer blocks rejected by Queued admission"),
      ADD_STAT(ftAllocations, statistics::units::Count::get(),
               "STEP filter-table allocations"),
      ADD_STAT(atAllocations, statistics::units::Count::get(),
               "STEP accumulation-table allocations"),
      ADD_STAT(phtInsertions, statistics::units::Count::get(),
               "STEP pattern-history-table insertions"),
      ADD_STAT(phtVictims, statistics::units::Count::get(),
               "STEP valid pattern-history-table victims")
{
}

}  // namespace prefetch
}  // namespace gem5
