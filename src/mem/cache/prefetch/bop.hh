/**
 * Copyright (c) 2018 Metempsy Technology Consulting
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

/**
 * Implementation of the 'A Best-Offset Prefetcher'
 * Reference:
 *   Michaud, P. (2015, June). A best-offset prefetcher.
 *   In 2nd Data Prefetching Championship.
 */

#ifndef __MEM_CACHE_PREFETCH_BOP_HH__
#define __MEM_CACHE_PREFETCH_BOP_HH__

#include <array>
#include <cstdint>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include <boost/compute/detail/lru_cache.hpp>

#include "base/sat_counter.hh"
#include "base/statistics.hh"
#include "mem/cache/prefetch/direct_quality_gate.hh"
#include "mem/cache/prefetch/queued.hh"
#include "mem/packet.hh"

namespace gem5
{

struct BOPPrefetcherParams;

GEM5_DEPRECATED_NAMESPACE(Prefetcher, prefetch);
namespace prefetch
{

class BOP : public Queued
{
    private:

        enum RRWay
        {
            Left,
            Right
        };

        /** Learning phase parameters */
        const unsigned int scoreMax;
        const unsigned int roundMax;
        const unsigned int badScore;
        /** Recent requests table parameteres */
        const unsigned int rrEntries;
        const unsigned int tagMask;
        /** Delay queue parameters */
        const bool         delayQueueEnabled;
        const unsigned int delayQueueSize;
        const unsigned int delayTicks;
        /** Cross page parameters */
        const bool crossPage;
        /** Adapt Bop Offset */
        const bool enableAdaptOffset;
        const bool negativeOffsetsEnable;
        const bool autoLearning;
        /** Revalidate the current best offset before issuing a prefetch */
        const bool enableIssueValidation;
        /** Grade issue validation misses with shared per-PC confidence */
        const bool enablePCValidationConfidence;
        /** Attribute RR hits to producers and gate cross-PC consumers */
        const bool enablePCValidationProducerConsumer;
        /** Bypass PC validation when recent global BOP outcomes are healthy */
        const bool enableGlobalBOPCoverageGuard;
        /** Optional online Tier20/P8 direct-quality gate. */
        const bool enableDirectQualityGate;

        const unsigned int pcValidationEntries;
        const unsigned int pcValidationTagBits;
        const unsigned int pcValidationCounterBits;
        const unsigned int pcValidationInitial;
        const unsigned int pcValidationMediumThreshold;
        const unsigned int pcValidationHighThreshold;
        const unsigned int pcValidationHitIncrement;
        const unsigned int pcValidationMediumSamplePeriod;
        const unsigned int pcValidationMissDecayPeriod;
        const unsigned int pcValidationLowEntryMissStreakThreshold;
        const unsigned int pcValidationEpochBits;
        const unsigned int pcValidationOffsetContextSlots;
        const unsigned int globalBOPUnusedThreshold;
        const unsigned int globalBOPMinResolvedCoverageShift;

        const DirectQualityGate::Config directQualityConfig;

        const int victimListSize;
        const int restoreCycle;

        bool victimRestoreScheduled = false;
        Event *restore_event;

        /**
         * Compressed PC-table identity carried by an RR entry. The key uses
         * the same set and partial tag already used by PC validation, so RR
         * ownership does not require a full architectural PC.
         */
        struct PCValidationKey
        {
            bool valid = false;
            unsigned int set = 0;
            Addr tag = 0;

            bool operator==(const PCValidationKey &other) const
            {
                return valid == other.valid && set == other.set &&
                       tag == other.tag;
            }
        };

        struct RREntryDebug
        {
            Addr fullAddr;
            Addr hashAddr;
            PCValidationKey owner;

            RREntryDebug(Addr full_addr, Addr hash_addr,
                         PCValidationKey owner_key)
                : fullAddr(full_addr), hashAddr(hash_addr), owner(owner_key)
            {}
            RREntryDebug() : fullAddr(0), hashAddr(0), owner() {}
        };

        std::vector<RREntryDebug> rrLeft;
        std::vector<RREntryDebug> rrRight;

        /** Structure to save the offset and the score */
        // typedef std::pair<int16_t, uint8_t> OffsetListEntry;
        struct OffsetListEntry{
            int32_t offset;  // offset, name it as first to make it compatible with pair
            uint8_t score;  // score, name it as second to make it compatible with pair
            int16_t depth;
            SatCounter8 late;

            OffsetListEntry(int32_t x, uint8_t y)
                : offset(x), score(y), depth(1), late(6, 32)
            {}

            int64_t calcOffset() const
            {
                assert(offset != 0);
                return offset * depth;
            }

            bool operator==(const int64_t t){
                return offset == t;
            }
        };
        std::vector<int> originOffsets;
        std::list<OffsetListEntry> offsetsList;
        std::list<int> victimOffsetsList;

        size_t maxOffsetCount{32};

        // std::set<int32_t> offsets;

        /** In a first implementation of the BO prefetcher, both banks of the
         *  RR were written simultaneously when a prefetched line is inserted
         *  into the cache. Adding the delay queue tries to avoid always
         *  striving for timeless prefetches, which has been found to not
         *  always being optimal.
         */
        struct DelayQueueEntry
        {
            RREntryDebug rrEntry;
            Tick processTick;

            DelayQueueEntry(const RREntryDebug &other, Tick t) : rrEntry(other), processTick(t)
            {}
        };

        std::deque<DelayQueueEntry> delayQueue;

        enum class PCConfidenceState : int
        {
            None = -1,
            Low = 0,
            Medium = 1,
            High = 2
        };

        enum class PCValidationKind : uint8_t
        {
            Generic = 0,
            Large = 1,
            Small = 2
        };

        static constexpr unsigned int PC_VALIDATION_KIND_COUNT = 3;
        static constexpr unsigned int PC_VALIDATION_ASSOCIATIVITY = 4;

        static unsigned int pcValidationKindIndex(PCValidationKind kind);
        static const char *pcValidationKindName(PCValidationKind kind);

        class PCValidationConfidenceTable
        {
          private:
            // Keep the global policy fixed so unused rate is its only knob.
            static constexpr unsigned int GLOBAL_OUTCOME_WINDOW_SIZE = 512;
            static constexpr unsigned int GLOBAL_OUTCOME_WINDOW_SHIFT = 9;
            static constexpr unsigned int GLOBAL_EWMA_SHIFT = 3;
            static constexpr unsigned int GLOBAL_IDLE_RESET_CHECKS = 4096;
            static constexpr unsigned int GLOBAL_UNUSED_EWMA_INITIAL = 255;
            static constexpr unsigned int
                PC_VALIDATION_MAX_OFFSET_CONTEXT_SLOTS = 4;

            struct OffsetContext
            {
                bool valid = false;
                int64_t offset = 0;
                uint8_t confidence = 0;
                uint8_t lowEntryMissStreak = 0;
            };

            struct Entry
            {
                bool valid = false;
                Addr tag = 0;
                std::array<OffsetContext,
                           PC_VALIDATION_MAX_OFFSET_CONTEXT_SLOTS> contexts;
                uint8_t contextPLRU = 0;
            };

            struct PendingUpdate
            {
                bool valid = false;
                bool validationHit = false;
                PCValidationKind kind = PCValidationKind::Generic;
                Addr pc = 0;
                Addr triggerLine = 0;
                unsigned int index = 0;
                unsigned int set = 0;
                unsigned int way = 0;
                unsigned int contextWay = 0;
                Addr tag = 0;
                int64_t offset = 0;
                unsigned int participants = 0;
            };

            const unsigned int entries;
            const unsigned int sets;
            const unsigned int setBits;
            const unsigned int tagBits;
            const Addr tagMask;
            const unsigned int counterMax;
            const unsigned int initialConfidence;
            const unsigned int mediumThreshold;
            const unsigned int highThreshold;
            const unsigned int hitIncrement;
            const unsigned int mediumSamplePeriod;
            const unsigned int missDecayPeriod;
            const unsigned int lowEntryMissStreakThreshold;
            const unsigned int offsetContextSlots;
            const bool globalCoverageGuardEnabled;
            const unsigned int globalUnusedThreshold;
            const unsigned int globalMinResolvedCoverageShift;

            static constexpr unsigned int
                PC_VALIDATION_MAX_PENDING_UPDATES_PER_KIND = 2;
            std::array<std::array<PendingUpdate,
                                  PC_VALIDATION_MAX_PENDING_UPDATES_PER_KIND>,
                       PC_VALIDATION_KIND_COUNT> pending = {};
            std::vector<Entry> table;
            std::vector<uint8_t> plruState;
            unsigned int globalOutcomeWindowResolved = 0;
            unsigned int globalOutcomeWindowUnused = 0;
            unsigned int globalIssuedWindowIssued = 0;
            unsigned int globalUnusedEwma = GLOBAL_UNUSED_EWMA_INITIAL;
            unsigned int globalChecksSinceOutcome = 0;
            bool globalBypassPCValidation = false;

            Addr foldedPC(Addr pc) const;
            Addr signature(Addr pc, PCValidationKind kind) const;
            bool sample(Addr pc, PCValidationKind kind, int64_t offset,
                        Addr line,
                        unsigned int period, Addr salt) const;
            Entry &entryAt(unsigned int set, unsigned int way);
            const Entry &entryAt(unsigned int set, unsigned int way) const;
            unsigned int plruVictim(unsigned int set) const;
            void touchPLRU(unsigned int set, unsigned int way);
            unsigned int contextVictim(const Entry &entry) const;
            void touchContext(Entry &entry, unsigned int context_way);
            void resetGlobalBypassPolicy();

          public:
            struct LookupResult
            {
                unsigned int index = 0;
                unsigned int set = 0;
                unsigned int way = 0;
                unsigned int contextWay = 0;
                Addr tag = 0;
                PCValidationKind kind = PCValidationKind::Generic;
                bool entryHit = false;
                bool replaced = false;
                bool contextHit = false;
                bool contextReplaced = false;
                int64_t offset = 0;
                unsigned int confidence = 0;
                unsigned int lowEntryMissStreak = 0;
                PCConfidenceState state = PCConfidenceState::None;
            };

            struct CommitResult
            {
                bool hadPending = false;
                bool hadValidation = false;
                bool validationHit = false;
                bool decayed = false;
                PCValidationKind kind = PCValidationKind::Generic;
                Addr pc = 0;
                Addr triggerLine = 0;
                unsigned int index = 0;
                unsigned int set = 0;
                unsigned int way = 0;
                unsigned int contextWay = 0;
                Addr tag = 0;
                int64_t offset = 0;
                unsigned int participants = 0;
                int confidenceBefore = -1;
                int confidenceAfter = -1;
                int lowEntryMissStreakBefore = -1;
                int lowEntryMissStreakAfter = -1;
                bool lowEntryHysteresisHeld = false;
                bool lowEntryHysteresisTransition = false;
            };

          private:
            CommitResult commitOne(PendingUpdate &update);

          public:
            struct GlobalOutcomeResult
            {
                bool enabled = false;
                bool ewmaUpdated = false;
                bool bypassModeEntered = false;
                bool bypassModeExited = false;
                bool resolvedCoverageGood = true;
                bool bypassBlockedByLowCoverage = false;
                unsigned int unusedEwma = 0;
                unsigned int issuedWindowIssued = 0;
                unsigned int resolvedCoverageQ08 = 255;
            };

            PCValidationConfidenceTable(
                unsigned int entries, unsigned int tag_bits,
                unsigned int counter_bits, unsigned int initial_confidence,
                unsigned int medium_threshold, unsigned int high_threshold,
                unsigned int hit_increment,
                unsigned int medium_sample_period,
                unsigned int miss_decay_period,
                unsigned int low_entry_miss_streak_threshold,
                unsigned int offset_context_slots,
                bool enable_global_coverage_guard,
                unsigned int global_unused_threshold,
                unsigned int global_min_resolved_coverage_shift);

            LookupResult lookup(Addr pc, PCValidationKind kind,
                                int64_t offset);
            PCValidationKey keyForPC(Addr pc, PCValidationKind kind) const;
            LookupResult lookup(const PCValidationKey &key,
                                PCValidationKind kind, int64_t offset);
            bool sampleMediumIssue(Addr pc, PCValidationKind kind,
                                   int64_t offset,
                                   Addr line) const;
            bool notePCValidationMiss();
            bool bypassPCValidationActive() const;
            void noteGlobalBOPIssued();
            GlobalOutcomeResult noteGlobalBOPOutcome(bool useful);
            void submitValidation(const LookupResult &lookup, Addr pc,
                                  Addr trigger_line, bool validation_hit);
            std::vector<CommitResult> commit();
            bool configMatches(const PCValidationConfidenceTable &other) const;
        };

        std::shared_ptr<PCValidationConfidenceTable> pcValidationTable;
        bool pcValidationTableShared = false;
        std::shared_ptr<DirectQualityGate> directQualityGate;
        PCValidationKind pcValidationKind = PCValidationKind::Generic;
        std::string pcValidationGenericName;
        std::string pcValidationLargeName;
        std::string pcValidationSmallName;
        bool replayMetaWritten = false;
        uint64_t replayOrder = 0;

        /** Event to handle the delay queue processing */
        void delayQueueEventWrapper();
        EventFunctionWrapper delayQueueEvent;

        /** Hardware prefetcher enabled */
        bool issuePrefetchRequests;
        /** Current best offset to issue prefetches */
        int64_t bestOffset;
        /** Current best offset found in the learning phase */
        int64_t phaseBestOffset;
        /** Current test offset index */
        std::list<OffsetListEntry>::iterator offsetsListIterator;

        std::list<OffsetListEntry>::iterator bestoffsetsListIterator;

        /** Max score found so far */
        unsigned int bestScore;
        /** Current round */
        unsigned int round;

        std::list<OffsetListEntry>::iterator getBestOffsetIter();

        /** Generate a hash for the specified address to index the RR table
         *  @param addr: address to hash
         *  @param way:  RR table to which is addressed (left/right)
         */
        unsigned int hash(Addr addr, unsigned int way) const;

        /** Insert the specified address into the RR table
         *  @param addr: full address to insert
         *  @param tag: hashed address to insert
         *  @param way: RR table to which the address will be inserted
         */
        void insertIntoRR(Addr full_addr, Addr tag, unsigned int way);

        void insertIntoRR(Addr full_addr, Addr tag,
                          PCValidationKey owner_key, unsigned int way);

        /** Insert the specified address into the RR table
         *  @param rr_entry: rr_entry to insert
         *  @param way: RR table to which the address will be inserted
         */
        void insertIntoRR(RREntryDebug rr_entry, unsigned int way);

        /** Insert the specified address into the delay queue. This will
         *  trigger an event after the delay cycles pass
         *  @param addr: full address to insert
         *  @param tag: hashed address to insert
         */
        void insertIntoDelayQueue(Addr full_addr, Addr tag,
                                  PCValidationKey owner_key,
                                  uint64_t replay_order = 0);

        void writeBOPReplayDelayAction(const char *action,
                                       uint64_t replay_order, Addr addr,
                                       Tick process_tick,
                                       unsigned int queue_size_after);

        /** Reset all the scores from the offset list */
        void resetScores();

        /** Generate the tag for the specified address based on the tag bits
         *  and the block size
         *  @param addr: address to get the tag from
        */
        Addr tag(Addr addr) const;

        /** Test if @X-O is hitting in the RR table to update the
            offset score */
        std::pair<bool, RREntryDebug> testRR(Addr tag) const;

        /** Learning phase of the BOP. Update the intermediate values of the
            round and update the best offset if found */
        bool bestOffsetLearning(Addr hashed_addr, bool late, const PrefetchInfo &pfi);

        unsigned missCount{0};

        bool sendPFWithFilter(const PrefetchInfo &pfi, Addr addr,
                              std::vector<AddrPriority> &addresses, int prio,
                              PrefetchSourceType src,
                              const DirectQualityGate::Decision *decision);

        const char *pcValidationTraceName(PCValidationKind kind) const;
        void tracePCValidationUpdate(
            const PCValidationConfidenceTable::CommitResult &result);
        void updateDirectQualityStats();

        struct BopStats : public statistics::Group
        {
            BopStats(statistics::Group *parent);
            statistics::Distribution issuedOffsetDist;
            statistics::Scalar learnOffsetCount;
            statistics::Scalar throttledCount;
            statistics::Scalar issueValidationChecks;
            statistics::Scalar issueValidationHits;
            statistics::Scalar issueValidationSuppressed;
            statistics::Scalar pcValidationTableLookups;
            statistics::Scalar pcValidationTableHits;
            statistics::Scalar pcValidationTableMisses;
            statistics::Scalar pcValidationTableReplacements;
            statistics::Scalar pcValidationOffsetContextHits;
            statistics::Scalar pcValidationOffsetContextMisses;
            statistics::Scalar pcValidationOffsetContextReplacements;
            statistics::Scalar pcValidationEpochResets;
            statistics::Scalar pcValidationNoPCSuppressions;
            statistics::Scalar pcValidationHighMissIssued;
            statistics::Scalar pcValidationMediumMissIssued;
            statistics::Scalar pcValidationMediumMissSuppressed;
            statistics::Scalar pcValidationLowMissSuppressed;
            statistics::Scalar pcValidationHitUpdates;
            statistics::Scalar pcValidationMissDecays;
            statistics::Scalar pcValidationMissNoDecays;
            statistics::Scalar pcValidationLowEntryHysteresisHolds;
            statistics::Scalar pcValidationLowEntryHysteresisTransitions;
            statistics::Scalar pcValidationOffsetEpochChanges;
            statistics::Scalar rrOwnerValidHits;
            statistics::Scalar rrOwnerInvalidHits;
            statistics::Scalar rrOwnerSamePCHits;
            statistics::Scalar rrOwnerCrossPCHits;
            statistics::Scalar pcValidationProducerHitUpdates;
            statistics::Scalar pcValidationConsumerMissUpdates;
            statistics::Distribution pcValidationConfidenceDist;
            statistics::Scalar globalBOPOutcomeUseful;
            statistics::Scalar globalBOPOutcomeUnused;
            statistics::Scalar globalBOPIssued;
            statistics::Scalar globalBOPUnusedEwmaUpdates;
            statistics::Scalar globalBOPResolvedCoverageGood;
            statistics::Scalar globalBOPResolvedCoverageBad;
            statistics::Scalar globalBOPBypassBlockedLowCoverage;
            statistics::Scalar globalBOPBypassModeEntries;
            statistics::Scalar globalBOPBypassModeExits;
            statistics::Scalar globalBOPBypassModeIdleResets;
            statistics::Scalar globalBOPBypassModeChecks;
            statistics::Scalar globalBOPBypassModeIssued;
            statistics::Scalar globalBOPBypassModeHighIssued;
            statistics::Scalar globalBOPBypassModeMediumIssued;
            statistics::Scalar globalBOPBypassModeLowIssued;
            statistics::Scalar globalBOPBypassModeNoPCIssued;
            statistics::Distribution globalBOPUnusedEwma;
            statistics::Distribution globalBOPResolvedCoverage;
            statistics::Scalar directQualityIssued;
            statistics::Scalar directQualitySuppressed;
            statistics::Scalar directQualitySampled;
            statistics::Scalar directQualityUseful;
            statistics::Scalar directQualityUnused;
            statistics::Scalar directQualityFeedbackConflicts;
            statistics::Scalar directQualityFeedbackReplacements;
            statistics::Scalar directQualityFeedbackExpiries;
            statistics::Scalar directQualityFeedbackExpiryUnused;
            statistics::Scalar directQualityUnknownDrops;
            statistics::Scalar directQualityFeedbackTokenDrops;
            statistics::Scalar directQualityOrphanOutcomes;
            statistics::Scalar directQualityStateTransitions;
            statistics::Scalar directQualityBlockToRecoverTransitions;
            statistics::Scalar directQualityRecoverToOpenTransitions;
            statistics::Scalar directQualityRecoverToBlockTransitions;
            statistics::Scalar directQualityPeakOutstanding;
        } stats;

    public:
        boost::compute::detail::lru_cache<Addr, Addr> *filter;

        /** Update the RR right table after a prefetch fill */
        void notifyFill(const PacketPtr& pkt) override;

        BOP(const BOPPrefetcherParams &p);
        ~BOP() = default;

        void calculatePrefetch(const PrefetchInfo &pfi,
                               std::vector<AddrPriority> &addresses) override
        {
            panic("not implemented");
        };

        using Queued::calculatePrefetch;

        void calculatePrefetch(const PrefetchInfo &pfi,
                               std::vector<AddrPriority> &addresses,
                               bool late, uint64_t replay_event_id = 0);

        /** Record this BOP configuration once when replay tracing is enabled. */
        void writeBOPReplayMeta();

        /** Share one physical PC confidence table with another BOP instance. */
        void sharePCValidationConfidenceWith(BOP &other);

        /** Share one bounded direct-quality ledger across Large and Small BOP. */
        void shareDirectQualityGateWith(BOP &other);

        /** Attach optional physical direct-quality trace observation. */
        void setDirectQualityTraceSink(DirectQualityGate::TraceSink *sink);

        /** Apply the one-per-demand update merged across shared BOPs. */
        void commitPCValidationConfidence();

        /** Receive a source-only useful/unused outcome from the L2 cache. */
        void notifyGlobalBOPOutcome(bool useful);

        /** Online direct-quality physical-issue and L2-demand hooks. */
        void notifyDirectQualityIssued(Addr paddr, uint8_t kind,
                                       unsigned quality_set,
                                       unsigned quality_way,
                                       uint8_t quality_generation);
        void notifyDirectQualityDemand(Addr paddr);

        bool tryAddOffset(int64_t offset, bool late = false);
};

} // namespace prefetch
} // namespace gem5

#endif /* __MEM_CACHE_PREFETCH_BOP_HH__ */
