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

#ifndef __MEM_CACHE_PREFETCH_STEP_HH__
#define __MEM_CACHE_PREFETCH_STEP_HH__

#include <array>
#include <cstdint>
#include <optional>

#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/prefetch/associative_set.hh"
#include "mem/cache/tags/tagged_entry.hh"
#include "params/XSCompositePrefetcher.hh"

namespace gem5
{

namespace prefetch
{

/**
 * Spatial footprint predictor used by XSCompositePrefetcher when STEP is
 * enabled. It owns only STEP's FT/AT/PHT state; the parent owns candidate
 * filtering, buffering, and cache-level issuance.
 */
class StepSpatialPrefetcher
{
  public:
    static constexpr unsigned MaxHistoryEntries = 16;

    enum class TriggerStage : uint8_t
    {
        Foe,
        Soe,
        Toe,
    };

    struct Decision
    {
        TriggerStage stage;
        Addr region;
        uint64_t candidates;
        uint64_t observed;
        ContextID contextId;
        bool secure;
        uint64_t id;
    };

    struct Config
    {
        unsigned regionSize;
        unsigned blockSize;
        unsigned pcHashBits;
        unsigned confidenceEntries;
        unsigned confidenceThreshold;
        bool enableFoe;
        bool enableSoe;
        bool enableToe;
        unsigned ftEntries;
        unsigned ftAssoc;
        gem5::BaseIndexingPolicy *ftIndexingPolicy;
        gem5::replacement_policy::Base *ftReplacementPolicy;
        unsigned atEntries;
        unsigned atAssoc;
        gem5::BaseIndexingPolicy *atIndexingPolicy;
        gem5::replacement_policy::Base *atReplacementPolicy;
        unsigned phtEntries;
        unsigned phtAssoc;
        gem5::BaseIndexingPolicy *phtIndexingPolicy;
        gem5::replacement_policy::Base *phtReplacementPolicy;
    };

    StepSpatialPrefetcher(const XSCompositePrefetcherParams &p,
                          statistics::Group *parent);
    StepSpatialPrefetcher(const Config &config, statistics::Group *parent);

    /** Observe one eligible demand access and return a prefetch decision. */
    std::optional<Decision> observe(Addr address, Addr pc, ContextID context_id,
                                    bool secure, bool allow_issue);

    /** Record candidate blocks admitted to the finite STEP prefetch buffer. */
    void recordBuffered(const Decision &decision, uint64_t buffered_blocks);

    /** Mark a live FT entry issued after one buffered block reaches its target PFQ. */
    void recordHandoff(Addr region, ContextID context_id, bool secure,
                       uint64_t decision_id);

    /** Resolve one buffered candidate that never reached its target PFQ. */
    void recordTerminalFailure(Addr region, ContextID context_id, bool secure,
                               uint64_t decision_id);

    /** Record one STEP prefetch-buffer handoff rejected by shared filtering. */
    void recordBufferHandoffFiltered();

    /** Record one PB candidate rejected after it reached Queued. */
    void recordBufferHandoffRejected();

    /** Complete and evict all active footprints. Intended for focused tests. */
    void flushActiveEntriesForTest();

    /** Pure helpers kept public for focused confidence tests. */
    static uint64_t intersectFootprints(
        const std::array<uint64_t, MaxHistoryEntries> &footprints,
        unsigned count)
    {
        if (count == 0) {
            return 0;
        }

        uint64_t intersection = footprints[0];
        for (unsigned i = 1; i < count; ++i) {
            intersection &= footprints[i];
        }
        return intersection;
    }

    static bool footprintsConverge(
        const std::array<uint64_t, MaxHistoryEntries> &footprints,
        unsigned count, unsigned threshold_percent)
    {
        if (count == 0 || threshold_percent > 100) {
            return false;
        }

        const uint64_t newest = footprints[0];
        for (unsigned i = 1; i < count; ++i) {
            const uint64_t union_bits = newest | footprints[i];
            if (union_bits == 0) {
                return false;
            }

            const unsigned intersection = __builtin_popcountll(
                newest & footprints[i]);
            const unsigned union_count = __builtin_popcountll(union_bits);
            // The paper defines convergence as Jaccard similarity strictly
            // greater than the threshold. Keep the comparison integral so
            // a 3/4 match does not pass the default 75% threshold.
            if (intersection * 100 <= union_count * threshold_percent) {
                return false;
            }
        }
        return true;
    }

    struct Stats : public statistics::Group
    {
        Stats(statistics::Group *parent);

        statistics::Scalar demandObservations;
        statistics::Scalar foeLookups;
        statistics::Scalar soeLookups;
        statistics::Scalar toeLookups;
        statistics::Scalar foePhtHits;
        statistics::Scalar soePhtHits;
        statistics::Scalar toePhtHits;
        statistics::Scalar foeDeferredMaturity;
        statistics::Scalar foeDeferredConfidence;
        statistics::Scalar soeDeferredConfidence;
        statistics::Scalar toeMisses;
        statistics::Scalar foeDecisions;
        statistics::Scalar soeDecisions;
        statistics::Scalar toeDecisions;
        statistics::Scalar foeCandidateBlocks;
        statistics::Scalar soeCandidateBlocks;
        statistics::Scalar toeCandidateBlocks;
        statistics::Scalar foeBufferedBlocks;
        statistics::Scalar soeBufferedBlocks;
        statistics::Scalar toeBufferedBlocks;
        statistics::Scalar preBufferFilteredBlocks;
        statistics::Scalar bufferHandoffFilteredBlocks;
        statistics::Scalar bufferHandoffRejectedBlocks;
        statistics::Scalar ftAllocations;
        statistics::Scalar atAllocations;
        statistics::Scalar phtInsertions;
        statistics::Scalar phtVictims;
    } stats;

  private:
    static constexpr uint8_t InvalidOffset = UINT8_MAX;

    class FilterEntry : public TaggedEntry
    {
      public:
        Addr region = 0;
        ContextID contextId = InvalidContextID;
        uint8_t firstOffset = InvalidOffset;
        uint8_t secondOffset = InvalidOffset;
        uint16_t pcHash = 0;
        bool issued = false;
        uint64_t generation = 0;
        uint64_t pendingDecisionId = 0;
        unsigned pendingBlocks = 0;
    };

    class ActiveEntry : public TaggedEntry
    {
      public:
        Addr region = 0;
        ContextID contextId = InvalidContextID;
        uint64_t footprint = 0;
        uint8_t firstOffset = InvalidOffset;
        uint8_t secondOffset = InvalidOffset;
        uint8_t thirdOffset = InvalidOffset;
        uint16_t pcHash = 0;
        bool hasFootprint = false;
    };

    class PatternEntry : public TaggedEntry
    {
      public:
        ContextID contextId = InvalidContextID;
        uint64_t footprint = 0;
        uint64_t sequence = 0;
        uint8_t firstOffset = InvalidOffset;
        uint8_t secondOffset = InvalidOffset;
        uint8_t thirdOffset = InvalidOffset;
        uint16_t pcHash = 0;
        bool mature = false;
    };

    const unsigned regionSize;
    const unsigned blockSize;
    const unsigned regionBlks;
    const unsigned pcHashBits;
    const unsigned confidenceEntries;
    const unsigned confidenceThreshold;
    const bool enableFoe;
    const bool enableSoe;
    const bool enableToe;

    AssociativeSet<FilterEntry> filterTable;
    AssociativeSet<ActiveEntry> activeTable;
    AssociativeSet<PatternEntry> patternTable;
    uint64_t nextSequence = 0;
    uint64_t nextFilterGeneration = 0;

    void validateConfig(unsigned pht_assoc, int pf_level) const;

    Addr regionAddress(Addr address) const;
    uint8_t regionOffset(Addr address) const;
    uint64_t offsetBit(uint8_t offset) const;
    uint16_t hashPc(Addr pc) const;
    Addr regionKey(Addr region, ContextID context_id) const;
    Addr patternKey(uint8_t first_offset, ContextID context_id) const;
    ContextID tableContext(ContextID context_id) const;
    FilterEntry *findFilterEntry(Addr region, ContextID context_id,
                                 bool secure);
    ActiveEntry *findActiveEntry(Addr region, ContextID context_id,
                                 bool secure);
    PatternEntry *findPatternVictim(Addr key, bool *victim_secure,
                                    bool *victim_valid);
    std::optional<Decision> lookup(FilterEntry &filter_entry,
                                   TriggerStage stage, uint8_t third_offset,
                                   bool secure, bool allow_issue);
    unsigned selectCandidates(
        const FilterEntry &filter_entry, TriggerStage stage,
        uint8_t third_offset, bool secure,
        std::array<PatternEntry *, MaxHistoryEntries> &candidates);
    void train(ActiveEntry &active_entry, bool secure);
    void allocateActive(const FilterEntry &filter_entry, uint8_t third_offset,
                        bool secure);

    void recordLookup(TriggerStage stage);
    void recordPhtHit(TriggerStage stage);
    void recordDecision(TriggerStage stage, unsigned candidate_blocks);
    void recordBuffered(TriggerStage stage, unsigned buffered_blocks);
};

}  // namespace prefetch
}  // namespace gem5

#endif  // __MEM_CACHE_PREFETCH_STEP_HH__
