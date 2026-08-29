#include "cpu/valuepred/ideal_constant_lvp.hh"

#include <algorithm>
#include <cassert>
#include <limits>

#include "base/logging.hh"
#include "base/output.hh"
#include "base/stats/units.hh"
#include "cpu/valuepred/valuepred_metadata.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

namespace valuepred
{

IdealConstantLVP::IdealConstantLVP(const Params &params)
    : VPUnit(params),
      idealConstTables(params.numThreads),
      lifetimeProfileTables(params.numThreads),
      roiProfileTables(params.numThreads),
      lifetimeProfileUpdateSequences(params.numThreads, 0),
      roiProfileUpdateSequences(params.numThreads, 0),
      lifetimePredictionIntervals(params.numThreads),
      roiPredictionIntervals(params.numThreads),
      shadowQfTables(),
      shadowPctTables(),
      lifetimeShadowCounters(params.numThreads),
      roiShadowCounters(params.numThreads),
      shadowUpdateSequences(params.numThreads, 0),
      satCounterBits(params.satCounterBits),
      resetConfidence(params.resetConfidence),
      enableProfiling(params.enableProfiling),
      enableShadowProfiling(params.enableShadowProfiling),
      shadowQfEntries(params.shadowQfEntries),
      shadowQfWays(params.shadowQfWays),
      shadowPctEntries(params.shadowPctEntries),
      shadowPctWays(params.shadowPctWays),
      shadowQualification(params.shadowQualification),
      profileStats(this)
{
    fatal_if(satCounterBits > 16,
            "IdealConstantLVP satCounterBits cannot exceed 16");

    if (enableShadowProfiling) {
        fatal_if(shadowQfEntries == 0 || shadowQfWays == 0 ||
                        shadowQfEntries % shadowQfWays != 0,
                "IdealConstantLVP shadow QF entries must be nonzero and "
                "divisible by shadowQfWays");
        fatal_if(shadowPctEntries == 0 || shadowPctWays == 0 ||
                        shadowPctEntries % shadowPctWays != 0,
                "IdealConstantLVP shadow PCT entries must be nonzero and "
                "divisible by shadowPctWays");
        fatal_if(shadowQualification == 0,
                "IdealConstantLVP shadowQualification must be nonzero");
    }

    if (enableShadowProfiling) {
        shadowQfTables.reserve(numThreads);
        shadowPctTables.reserve(numThreads);
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            shadowQfTables.emplace_back(shadowQfEntries, shadowQfWays);
            shadowPctTables.emplace_back(shadowPctEntries, shadowPctWays);
        }
    }

    // Peak table pressure is useful in normal performance runs too, so it is
    // independent of the optional CSV profiling modes.
    statistics::registerResetCallback([this] { resetRoiSaturationStats(); });
    statistics::registerDumpCallback([this] { refreshSaturationStats(); });

    if (enableProfiling || enableShadowProfiling) {
        statistics::registerResetCallback([this] { resetRoiProfile(); });
        statistics::registerDumpCallback([this] { refreshProfileStats(); });
    }
    if (enableProfiling) {
        registerExitCallback([this] {
            dumpProfile();
            dumpPredictionIntervals();
        });
    }
    if (enableShadowProfiling)
        registerExitCallback([this] { dumpShadowProfile(); });
}

IdealConstantLVP::IdealConstantLVPStats::IdealConstantLVPStats(
        statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(profileRoiUpdates, statistics::units::Count::get(),
              "Committed IdealConstantLVP updates after the last stats reset"),
      ADD_STAT(profileRoiDistinctPcs, statistics::units::Count::get(),
              "Distinct committed PCs after the last stats reset"),
      ADD_STAT(profileRoiValueChanges, statistics::units::Count::get(),
              "Committed updates that changed the tracked value after stats reset"),
      ADD_STAT(profileRoiSaturationTransitions, statistics::units::Count::get(),
              "Counter transitions into saturation after the last stats reset"),
      ADD_STAT(profileRoiEverSaturatedPcs, statistics::units::Count::get(),
              "Distinct PCs observed saturated after the last stats reset"),
      ADD_STAT(profileRoiSaturatedAtEndPcs, statistics::units::Count::get(),
              "Profiled PCs still saturated when statistics are dumped"),
      ADD_STAT(profileRoiPeakSaturatedPcs, statistics::units::Count::get(),
              "Maximum simultaneously saturated IdealConstantLVP table "
              "entries (one per (tid, PC)) after the last stats reset"),
      ADD_STAT(profileRoiCommittedSaturatedOffers,
              statistics::units::Count::get(),
              "Committed instructions for which a saturated IdealConstantLVP "
              "candidate was offered after the last stats reset"),
      ADD_STAT(profileRoiPredictionUses, statistics::units::Count::get(),
              "Committed instructions that applied an IdealConstantLVP "
              "prediction after the last stats reset"),
      ADD_STAT(profileRoiCorrectPredictionUses,
              statistics::units::Count::get(),
              "Committed instructions with a correct applied IdealConstantLVP "
              "prediction after the last stats reset"),
      ADD_STAT(profileLifetimePcsAtEnd, statistics::units::Count::get(),
              "All IdealConstantLVP PC entries resident when statistics are dumped"),
      ADD_STAT(profileLifetimeEverSaturatedPcs,
              statistics::units::Count::get(),
              "All distinct PCs that reached saturation since process start"),
      ADD_STAT(profileLifetimeSaturatedPcsAtEnd,
              statistics::units::Count::get(),
              "All IdealConstantLVP entries saturated when statistics are dumped"),
      ADD_STAT(profileLifetimePeakSaturatedPcs,
              statistics::units::Count::get(),
              "Maximum simultaneously saturated IdealConstantLVP table "
              "entries (one per (tid, PC)) since predictor construction"),
      ADD_STAT(shadowRoiCommittedUpdates, statistics::units::Count::get(),
              "Committed updates observed by the bounded shadow model after reset"),
      ADD_STAT(shadowRoiQfLookups, statistics::units::Count::get(),
              "QF shadow lookups after reset"),
      ADD_STAT(shadowRoiQfHits, statistics::units::Count::get(),
              "QF shadow hits after reset"),
      ADD_STAT(shadowRoiQfMisses, statistics::units::Count::get(),
              "QF shadow misses after reset"),
      ADD_STAT(shadowRoiQfPromotions, statistics::units::Count::get(),
              "QF shadow promotions after reset"),
      ADD_STAT(shadowRoiQfEvictions, statistics::units::Count::get(),
              "QF shadow evictions after reset"),
      ADD_STAT(shadowRoiQfQualifiedEvictions, statistics::units::Count::get(),
              "Qualified QF shadow evictions after reset"),
      ADD_STAT(shadowRoiPctFetchLookups, statistics::units::Count::get(),
              "PCT shadow fetch lookups after reset"),
      ADD_STAT(shadowRoiPctFetchHits, statistics::units::Count::get(),
              "PCT shadow fetch hits after reset"),
      ADD_STAT(shadowRoiPctFetchMisses, statistics::units::Count::get(),
              "PCT shadow fetch misses after reset"),
      ADD_STAT(shadowRoiPctCommitLookups, statistics::units::Count::get(),
              "PCT shadow commit lookups after reset"),
      ADD_STAT(shadowRoiPctCommitHits, statistics::units::Count::get(),
              "PCT shadow commit hits after reset"),
      ADD_STAT(shadowRoiPctCommitMisses, statistics::units::Count::get(),
              "PCT shadow commit misses after reset"),
      ADD_STAT(shadowRoiPctMismatches, statistics::units::Count::get(),
              "PCT shadow value mismatches after reset"),
      ADD_STAT(shadowRoiPctDemotions, statistics::units::Count::get(),
              "PCT shadow demotions after reset"),
      ADD_STAT(shadowRoiPctEvictions, statistics::units::Count::get(),
              "PCT shadow evictions after reset"),
      ADD_STAT(shadowRoiPredictionOffers, statistics::units::Count::get(),
              "Hypothetical PCT shadow prediction offers after reset"),
      ADD_STAT(shadowRoiPredictionCorrect, statistics::units::Count::get(),
              "Correct hypothetical PCT shadow predictions after reset"),
      ADD_STAT(shadowRoiPredictionWrong, statistics::units::Count::get(),
              "Wrong hypothetical PCT shadow predictions after reset"),
      ADD_STAT(shadowRoiPredictionEvictedBeforeCommit,
              statistics::units::Count::get(),
              "Shadow predictions whose PCT entry changed before commit"),
      ADD_STAT(shadowRoiPredictionWrongAfterEviction,
              statistics::units::Count::get(),
              "Wrong shadow predictions whose PCT entry changed before commit"),
      ADD_STAT(shadowRoiQfPeakOccupancy, statistics::units::Count::get(),
              "Peak QF shadow occupancy after reset"),
      ADD_STAT(shadowRoiPctPeakOccupancy, statistics::units::Count::get(),
              "Peak PCT shadow occupancy after reset"),
      ADD_STAT(shadowRoiQfMaxSetOccupancy, statistics::units::Count::get(),
              "Maximum QF shadow set occupancy after reset"),
      ADD_STAT(shadowRoiPctMaxSetOccupancy, statistics::units::Count::get(),
              "Maximum PCT shadow set occupancy after reset"),
      ADD_STAT(shadowLifetimeCommittedUpdates, statistics::units::Count::get(),
              "Committed updates observed by the bounded shadow model"),
      ADD_STAT(shadowLifetimeQfPromotions, statistics::units::Count::get(),
              "Lifetime QF shadow promotions"),
      ADD_STAT(shadowLifetimeQfEvictions, statistics::units::Count::get(),
              "Lifetime QF shadow evictions"),
      ADD_STAT(shadowLifetimeQfQualifiedEvictions,
              statistics::units::Count::get(),
              "Lifetime qualified QF shadow evictions"),
      ADD_STAT(shadowLifetimePctMismatches, statistics::units::Count::get(),
              "Lifetime PCT shadow value mismatches"),
      ADD_STAT(shadowLifetimePctDemotions, statistics::units::Count::get(),
              "Lifetime PCT shadow demotions"),
      ADD_STAT(shadowLifetimePctEvictions, statistics::units::Count::get(),
              "Lifetime PCT shadow evictions"),
      ADD_STAT(shadowLifetimePredictionOffers, statistics::units::Count::get(),
              "Lifetime hypothetical PCT shadow prediction offers"),
      ADD_STAT(shadowLifetimePredictionCorrect, statistics::units::Count::get(),
              "Lifetime correct hypothetical PCT shadow predictions"),
      ADD_STAT(shadowLifetimePredictionWrong, statistics::units::Count::get(),
              "Lifetime wrong hypothetical PCT shadow predictions"),
      ADD_STAT(shadowLifetimePredictionWrongAfterEviction,
              statistics::units::Count::get(),
              "Lifetime wrong shadow predictions whose PCT entry changed before commit")
{
}

void
IdealConstantLVP::observeSaturationTransition(bool was_saturated,
        bool is_saturated)
{
    if (was_saturated == is_saturated)
        return;

    if (is_saturated) {
        currentSaturatedPcs++;
        lifetimePeakSaturatedPcs = std::max(lifetimePeakSaturatedPcs,
                currentSaturatedPcs);
        roiPeakSaturatedPcs = std::max(roiPeakSaturatedPcs,
                currentSaturatedPcs);
        return;
    }

    gem5_assert(currentSaturatedPcs > 0,
            "IdealConstantLVP saturated entry accounting underflow\n");
    currentSaturatedPcs--;
}

void
IdealConstantLVP::resetRoiSaturationStats()
{
    // The table persists across resetstats, so pre-existing saturated entries
    // are live capacity demand for the entire following ROI window.
    roiPeakSaturatedPcs = currentSaturatedPcs;
}

void
IdealConstantLVP::refreshSaturationStats()
{
    profileStats.profileRoiPeakSaturatedPcs = roiPeakSaturatedPcs;
    profileStats.profileLifetimePeakSaturatedPcs =
        lifetimePeakSaturatedPcs;
}

void
IdealConstantLVP::updateProfile(ProfileTable &profile_table, Addr pc,
        uint64_t update_sequence, uint64_t committed_seq_no,
        ThreadID tid, PredictionIntervals &prediction_intervals,
        bool value_changed, bool was_saturated, bool is_saturated,
        uint64_t saturation_epoch_started, uint64_t saturation_epoch_ended,
        uint64_t prediction_epoch,
        bool offered_prediction, bool applied_prediction,
        bool correct_prediction,
        bool update_roi_stats)
{
    gem5_assert(!applied_prediction || offered_prediction,
            "Applied IdealConstantLVP prediction was not offered\n");
    gem5_assert(!correct_prediction || applied_prediction,
            "Correct IdealConstantLVP prediction was not applied\n");

    auto [it, inserted] = profile_table.try_emplace(pc);
    auto &profile_entry = it->second;

    if (inserted) {
        profile_entry.firstUpdate = update_sequence;
    }
    profile_entry.updates++;
    profile_entry.lastUpdate = update_sequence;
    if (value_changed) {
        profile_entry.valueChanges++;
    }
    if (!was_saturated && is_saturated) {
        profile_entry.saturationTransitions++;
    }
    if (is_saturated) {
        profile_entry.saturatedUpdates++;
    }
    if (offered_prediction) {
        profile_entry.committedSaturatedOffers++;
    }
    if (applied_prediction) {
        profile_entry.predictionUses++;
    }
    if (correct_prediction) {
        profile_entry.correctPredictionUses++;
    }

    auto get_interval = [&](uint64_t saturation_epoch) -> PredictionInterval & {
        gem5_assert(saturation_epoch != 0,
                "IdealConstantLVP interval has no saturation epoch\n");
        const auto interval_it =
            profile_entry.predictionIntervals.find(saturation_epoch);
        if (interval_it != profile_entry.predictionIntervals.end()) {
            return prediction_intervals[interval_it->second - 1];
        }

        PredictionInterval interval;
        interval.tid = tid;
        interval.pc = pc;
        interval.saturationEpoch = saturation_epoch;
        prediction_intervals.push_back(interval);
        const auto [inserted_it, inserted] =
            profile_entry.predictionIntervals.emplace(
                saturation_epoch, prediction_intervals.size());
        gem5_assert(inserted,
                "IdealConstantLVP interval epoch was inserted twice\n");
        (void)inserted_it;
        return prediction_intervals.back();
    };

    if (saturation_epoch_started != 0) {
        get_interval(saturation_epoch_started);
    }

    if (applied_prediction) {
        auto &interval = get_interval(prediction_epoch);
        if (interval.predictionUses == 0) {
            interval.firstPredictionUseSeqNo = committed_seq_no;
        }
        interval.lastPredictionUseSeqNo = committed_seq_no;
        interval.predictionUses++;
        if (correct_prediction) {
            interval.correctPredictionUses++;
        }
    }

    if (saturation_epoch_ended != 0) {
        auto &interval = get_interval(saturation_epoch_ended);
        interval.saturationEndSeqNo = committed_seq_no;
        interval.openAtEnd = false;
    }

    const bool first_saturated_observation =
        !profile_entry.everSaturated &&
        (was_saturated || is_saturated || offered_prediction);
    if (first_saturated_observation) {
        profile_entry.firstSaturationUpdate = update_sequence;
    }
    profile_entry.everSaturated |=
        was_saturated || is_saturated || offered_prediction;

    if (!update_roi_stats) {
        return;
    }

    profileStats.profileRoiUpdates++;
    if (inserted) {
        profileStats.profileRoiDistinctPcs++;
    }
    if (value_changed) {
        profileStats.profileRoiValueChanges++;
    }
    if (!was_saturated && is_saturated) {
        profileStats.profileRoiSaturationTransitions++;
    }
    if (first_saturated_observation) {
        profileStats.profileRoiEverSaturatedPcs++;
    }
    if (offered_prediction) {
        profileStats.profileRoiCommittedSaturatedOffers++;
    }
    if (applied_prediction) {
        profileStats.profileRoiPredictionUses++;
    }
    if (correct_prediction) {
        profileStats.profileRoiCorrectPredictionUses++;
    }
}

unsigned
IdealConstantLVP::shadowIndex(Addr pc, unsigned sets)
{
    uint64_t folded_pc = static_cast<uint64_t>(pc) >> 1;
    folded_pc ^= folded_pc >> 11;
    folded_pc ^= folded_pc >> 22;
    folded_pc ^= folded_pc >> 33;
    if ((sets & (sets - 1)) == 0)
        return folded_pc & (sets - 1);
    return folded_pc % sets;
}

IdealConstantLVP::ShadowEntry *
IdealConstantLVP::findShadowEntry(ShadowTable &table, Addr pc,
        unsigned &set, unsigned &way) const
{
    set = shadowIndex(pc, table.sets);
    for (way = 0; way < table.ways; ++way) {
        auto &entry = table.entries[set * table.ways + way];
        if (entry.valid && entry.tag == pc)
            return &entry;
    }
    way = 0;
    return nullptr;
}

const IdealConstantLVP::ShadowEntry *
IdealConstantLVP::findShadowEntry(const ShadowTable &table, Addr pc,
        unsigned &set, unsigned &way) const
{
    set = shadowIndex(pc, table.sets);
    for (way = 0; way < table.ways; ++way) {
        const auto &entry = table.entries[set * table.ways + way];
        if (entry.valid && entry.tag == pc)
            return &entry;
    }
    way = 0;
    return nullptr;
}

IdealConstantLVP::ShadowEntry *
IdealConstantLVP::allocateShadowEntry(ShadowTable &table, Addr pc,
        RegVal value, bool qf, ShadowCounters &lifetime,
        ShadowCounters &roi,
        uint64_t commit_sequence, unsigned &set, unsigned &way)
{
    if (auto *existing = findShadowEntry(table, pc, set, way)) {
        existing->value = value;
        touchShadowEntry(table, *existing, commit_sequence, qf, lifetime, roi);
        return existing;
    }

    set = shadowIndex(pc, table.sets);
    bool found_invalid = false;
    unsigned victim_way = 0;
    for (unsigned candidate = 0; candidate < table.ways; ++candidate) {
        const auto &entry = table.entries[set * table.ways + candidate];
        if (!entry.valid) {
            victim_way = candidate;
            found_invalid = true;
            break;
        }
    }
    if (!found_invalid) {
        victim_way = 0;
        for (unsigned candidate = 1; candidate < table.ways; ++candidate) {
            const auto &candidate_entry =
                table.entries[set * table.ways + candidate];
            const auto &victim_entry =
                table.entries[set * table.ways + victim_way];
            if (candidate_entry.lru < victim_entry.lru)
                victim_way = candidate;
        }
    }

    auto &entry = table.entries[set * table.ways + victim_way];
    if (entry.valid) {
        if (qf) {
            lifetime.qfEvictions++;
            roi.qfEvictions++;
            if (entry.qualification >= shadowQualification) {
                lifetime.qfQualifiedEvictions++;
                roi.qfQualifiedEvictions++;
            }
        } else {
            lifetime.pctEvictions++;
            roi.pctEvictions++;
        }
    } else {
        table.occupancy++;
        table.setOccupancy[set]++;
        table.peakOccupancy = std::max(table.peakOccupancy, table.occupancy);
        table.maxSetOccupancy =
            std::max(table.maxSetOccupancy, table.setOccupancy[set]);
        table.roiMaxSetOccupancy =
            std::max(table.roiMaxSetOccupancy, table.setOccupancy[set]);
    }

    uint64_t generation = entry.generation + 1;
    if (generation == 0)
        generation = 1;
    entry = ShadowEntry{};
    entry.valid = true;
    entry.tag = pc;
    entry.value = value;
    entry.generation = generation;
    touchShadowEntry(table, entry, commit_sequence, qf, lifetime, roi);
    way = victim_way;
    return &entry;
}

void
IdealConstantLVP::invalidateShadowEntry(ShadowTable &table, unsigned set,
        unsigned way)
{
    if (set >= table.sets || way >= table.ways)
        return;
    auto &entry = table.entries[set * table.ways + way];
    if (!entry.valid)
        return;

    entry.valid = false;
    entry.lru = 0;
    entry.qualification = 0;
    entry.confidence = 0;
    entry.lastCommit = 0;
    ++entry.generation;
    if (entry.generation == 0)
        entry.generation = 1;
    gem5_assert(table.occupancy > 0 && table.setOccupancy[set] > 0,
            "Invalid shadow occupancy accounting");
    --table.occupancy;
    --table.setOccupancy[set];
}

void
IdealConstantLVP::touchShadowEntry(ShadowTable &table, ShadowEntry &entry,
        uint64_t commit_sequence, bool qf, ShadowCounters &lifetime,
        ShadowCounters &roi)
{
    if (entry.lastCommit != 0 && commit_sequence > entry.lastCommit) {
        const uint64_t distance = commit_sequence - entry.lastCommit;
        if (qf) {
            lifetime.qfReuseReferences++;
            lifetime.qfReuseDistanceSum += distance;
            lifetime.qfReuseDistanceMax =
                std::max(lifetime.qfReuseDistanceMax, distance);
            roi.qfReuseReferences++;
            roi.qfReuseDistanceSum += distance;
            roi.qfReuseDistanceMax = std::max(roi.qfReuseDistanceMax, distance);
        } else {
            lifetime.pctReuseReferences++;
            lifetime.pctReuseDistanceSum += distance;
            lifetime.pctReuseDistanceMax =
                std::max(lifetime.pctReuseDistanceMax, distance);
            roi.pctReuseReferences++;
            roi.pctReuseDistanceSum += distance;
            roi.pctReuseDistanceMax = std::max(roi.pctReuseDistanceMax, distance);
        }
    }
    entry.lastCommit = commit_sequence;
    entry.lru = ++table.lruClock;
}

void
IdealConstantLVP::observeShadowOccupancy(ShadowCounters &counters,
        ThreadID tid, bool roi) const
{
    if (!enableShadowProfiling)
        return;
    const auto &qf_table = shadowQfTables[tid];
    const auto &pct_table = shadowPctTables[tid];
    counters.qfPeakOccupancy =
        std::max<uint64_t>(counters.qfPeakOccupancy, qf_table.occupancy);
    counters.pctPeakOccupancy =
        std::max<uint64_t>(counters.pctPeakOccupancy, pct_table.occupancy);
    counters.qfMaxSetOccupancy = std::max<uint64_t>(
        counters.qfMaxSetOccupancy,
        roi ? qf_table.roiMaxSetOccupancy : qf_table.maxSetOccupancy);
    counters.pctMaxSetOccupancy = std::max<uint64_t>(
        counters.pctMaxSetOccupancy,
        roi ? pct_table.roiMaxSetOccupancy : pct_table.maxSetOccupancy);
}

void
IdealConstantLVP::shadowPredict(Addr pc, ThreadID tid,
        ShadowPredictionRecord &record)
{
    assertValidTid(tid);
    auto &lifetime = lifetimeShadowCounters[tid];
    auto &roi = roiShadowCounters[tid];
    lifetime.pctFetchLookups++;
    roi.pctFetchLookups++;

    auto &table = shadowPctTables[tid];
    unsigned set = 0;
    unsigned way = 0;
    auto *entry = findShadowEntry(table, pc, set, way);
    if (!entry) {
        lifetime.pctFetchMisses++;
        roi.pctFetchMisses++;
        return;
    }

    lifetime.pctFetchHits++;
    roi.pctFetchHits++;
    record.pctHit = true;
    record.pctSet = set;
    record.pctWay = way;
    record.pctGeneration = entry->generation;
    record.shadowPredictedValue = entry->value;
    const uint32_t max_confidence = satCounterBits >= 32 ?
        std::numeric_limits<uint32_t>::max() :
        (satCounterBits == 0 ? 0 : (uint32_t(1) << satCounterBits) - 1);
    record.predictionOffered = entry->confidence >= max_confidence;
}

void
IdealConstantLVP::shadowUpdate(Addr pc, ThreadID tid, RegVal actualValue,
        const VPPredictionRecord *record)
{
    assertValidTid(tid);
    auto &lifetime = lifetimeShadowCounters[tid];
    auto &roi = roiShadowCounters[tid];
    const uint64_t commit_sequence = ++shadowUpdateSequences[tid];
    lifetime.committedUpdates++;
    roi.committedUpdates++;

    const auto *shadow_record =
        dynamic_cast<const ShadowPredictionRecord *>(record);
    if (shadow_record && shadow_record->predictionOffered) {
        const bool correct =
            shadow_record->shadowPredictedValue == actualValue;
        const bool stale = [&]() {
            if (!shadow_record->pctHit)
                return false;
            const auto &table = shadowPctTables[tid];
            if (shadow_record->pctSet >= table.sets ||
                    shadow_record->pctWay >= table.ways) {
                return true;
            }
            const auto &entry = table.entries[
                shadow_record->pctSet * table.ways + shadow_record->pctWay];
            return !entry.valid || entry.tag != pc ||
                entry.generation != shadow_record->pctGeneration ||
                entry.value != shadow_record->shadowPredictedValue;
        }();
        lifetime.predictionOffers++;
        roi.predictionOffers++;
        if (correct) {
            lifetime.predictionCorrect++;
            roi.predictionCorrect++;
        } else {
            lifetime.predictionWrong++;
            roi.predictionWrong++;
        }
        if (stale) {
            lifetime.predictionEvictedBeforeCommit++;
            roi.predictionEvictedBeforeCommit++;
            if (!correct) {
                lifetime.predictionWrongAfterEviction++;
                roi.predictionWrongAfterEviction++;
            }
        }
    }

    auto &pct_table = shadowPctTables[tid];
    unsigned pct_set = 0;
    unsigned pct_way = 0;
    auto *pct_entry = findShadowEntry(pct_table, pc, pct_set, pct_way);
    lifetime.pctCommitLookups++;
    roi.pctCommitLookups++;
    if (pct_entry) {
        lifetime.pctCommitHits++;
        roi.pctCommitHits++;
    } else {
        lifetime.pctCommitMisses++;
        roi.pctCommitMisses++;
    }

    const bool valid_actual = actualValue != 0xdeadbeefULL;
    const bool pct_matches = pct_entry && valid_actual &&
        actualValue == pct_entry->value;
    if (pct_matches) {
        const uint32_t max_confidence = satCounterBits >= 32 ?
            std::numeric_limits<uint32_t>::max() :
            (satCounterBits == 0 ? 0 : (uint32_t(1) << satCounterBits) - 1);
        if (pct_entry->confidence < max_confidence)
            ++pct_entry->confidence;
        touchShadowEntry(pct_table, *pct_entry, commit_sequence, false,
                lifetime, roi);
        observeShadowOccupancy(lifetime, tid, false);
        observeShadowOccupancy(roi, tid, true);
        return;
    }

    if (pct_entry) {
        lifetime.pctMismatches++;
        roi.pctMismatches++;
        lifetime.pctDemotions++;
        roi.pctDemotions++;
        invalidateShadowEntry(pct_table, pct_set, pct_way);
    }

    auto &qf_table = shadowQfTables[tid];
    unsigned qf_set = 0;
    unsigned qf_way = 0;
    auto *qf_entry = findShadowEntry(qf_table, pc, qf_set, qf_way);
    lifetime.qfLookups++;
    roi.qfLookups++;
    if (qf_entry) {
        lifetime.qfHits++;
        roi.qfHits++;
        if (valid_actual && actualValue == qf_entry->value) {
            if (qf_entry->qualification < shadowQualification)
                ++qf_entry->qualification;
        } else {
            qf_entry->value = actualValue;
            qf_entry->qualification = valid_actual ? 1 : 0;
        }
        touchShadowEntry(qf_table, *qf_entry, commit_sequence, true,
                lifetime, roi);

        if (qf_entry->qualification >= shadowQualification) {
            lifetime.qfPromotions++;
            roi.qfPromotions++;
            unsigned promoted_set = 0;
            unsigned promoted_way = 0;
            auto *promoted = allocateShadowEntry(pct_table, pc,
                    qf_entry->value, false, lifetime, roi,
                    commit_sequence, promoted_set, promoted_way);
            if (promoted) {
                const uint32_t max_confidence = satCounterBits >= 32 ?
                    std::numeric_limits<uint32_t>::max() :
                    (satCounterBits == 0 ? 0 :
                        (uint32_t(1) << satCounterBits) - 1);
                promoted->confidence = std::min<uint32_t>(
                    shadowQualification - 1, max_confidence);
                promoted->qualification = 0;
            }
            (void)promoted_set;
            (void)promoted_way;
            invalidateShadowEntry(qf_table, qf_set, qf_way);
        }
    } else {
        lifetime.qfMisses++;
        roi.qfMisses++;
        auto *allocated = allocateShadowEntry(qf_table, pc, actualValue,
                true, lifetime, roi, commit_sequence, qf_set, qf_way);
        if (allocated) {
            allocated->qualification = valid_actual ? 1 : 0;
            if (allocated->qualification >= shadowQualification) {
                lifetime.qfPromotions++;
                roi.qfPromotions++;
                unsigned promoted_set = 0;
                unsigned promoted_way = 0;
                auto *promoted = allocateShadowEntry(pct_table, pc,
                        allocated->value, false, lifetime, roi,
                        commit_sequence, promoted_set, promoted_way);
                if (promoted) {
                    const uint32_t max_confidence = satCounterBits >= 32 ?
                        std::numeric_limits<uint32_t>::max() :
                        (satCounterBits == 0 ? 0 :
                            (uint32_t(1) << satCounterBits) - 1);
                    promoted->confidence = std::min<uint32_t>(
                        shadowQualification - 1, max_confidence);
                    promoted->qualification = 0;
                }
                invalidateShadowEntry(qf_table, qf_set, qf_way);
            }
        }
    }

    // The helper above updates table state once.  Refresh both scopes after
    // every committed update so peak occupancy remains observable after a
    // stats reset without resetting the bounded tables themselves.
    observeShadowOccupancy(lifetime, tid, false);
    observeShadowOccupancy(roi, tid, true);
}

void
IdealConstantLVP::resetRoiProfile()
{
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        roiProfileTables[tid].clear();
        roiProfileUpdateSequences[tid] = 0;
        roiPredictionIntervals[tid].clear();
    }
    resetShadowRoiProfile();
}

void
IdealConstantLVP::resetShadowRoiProfile()
{
    if (!enableShadowProfiling)
        return;
    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        auto &qf_table = shadowQfTables[tid];
        unsigned qf_current_max_set = 0;
        for (const auto occupancy : qf_table.setOccupancy)
            qf_current_max_set = std::max(qf_current_max_set, occupancy);
        qf_table.roiMaxSetOccupancy = qf_current_max_set;
        auto &pct_table = shadowPctTables[tid];
        unsigned pct_current_max_set = 0;
        for (const auto occupancy : pct_table.setOccupancy)
            pct_current_max_set = std::max(pct_current_max_set, occupancy);
        pct_table.roiMaxSetOccupancy = pct_current_max_set;
        roiShadowCounters[tid] = ShadowCounters{};
        observeShadowOccupancy(roiShadowCounters[tid], tid, true);
    }
}

void
IdealConstantLVP::refreshProfileStats()
{
    uint64_t lifetime_pcs = 0;
    uint64_t lifetime_ever_saturated_pcs = 0;
    uint64_t lifetime_saturated_pcs = 0;
    uint64_t roi_saturated_pcs = 0;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        const auto &ideal_const_table = idealConstTables[tid];
        lifetime_pcs += ideal_const_table.size();
        for (const auto &profile_item : lifetimeProfileTables[tid]) {
            if (profile_item.second.everSaturated) {
                lifetime_ever_saturated_pcs++;
            }
        }
        for (const auto &[pc, entry] : ideal_const_table) {
            if (entry.confidence.isSaturated()) {
                lifetime_saturated_pcs++;
            }
        }

        for (const auto &[pc, profile_entry] : roiProfileTables[tid]) {
            const auto it = ideal_const_table.find(pc);
            if (it != ideal_const_table.end() &&
                    it->second.confidence.isSaturated()) {
                roi_saturated_pcs++;
            }
        }
    }

    profileStats.profileRoiSaturatedAtEndPcs = roi_saturated_pcs;
    profileStats.profileLifetimePcsAtEnd = lifetime_pcs;
    profileStats.profileLifetimeEverSaturatedPcs =
        lifetime_ever_saturated_pcs;
    profileStats.profileLifetimeSaturatedPcsAtEnd = lifetime_saturated_pcs;

    refreshShadowStats();
}

void
IdealConstantLVP::refreshShadowStats()
{
    if (!enableShadowProfiling)
        return;

    for (ThreadID tid = 0; tid < numThreads; ++tid) {
        observeShadowOccupancy(lifetimeShadowCounters[tid], tid, false);
        observeShadowOccupancy(roiShadowCounters[tid], tid, true);
    }

    auto sum = [](const std::vector<ShadowCounters> &counters,
            uint64_t ShadowCounters::*member) {
        uint64_t total = 0;
        for (const auto &counter : counters)
            total += counter.*member;
        return total;
    };
    const auto &roi = roiShadowCounters;
    const auto &lifetime = lifetimeShadowCounters;

    profileStats.shadowRoiCommittedUpdates =
        sum(roi, &ShadowCounters::committedUpdates);
    profileStats.shadowRoiQfLookups = sum(roi, &ShadowCounters::qfLookups);
    profileStats.shadowRoiQfHits = sum(roi, &ShadowCounters::qfHits);
    profileStats.shadowRoiQfMisses = sum(roi, &ShadowCounters::qfMisses);
    profileStats.shadowRoiQfPromotions =
        sum(roi, &ShadowCounters::qfPromotions);
    profileStats.shadowRoiQfEvictions = sum(roi, &ShadowCounters::qfEvictions);
    profileStats.shadowRoiQfQualifiedEvictions =
        sum(roi, &ShadowCounters::qfQualifiedEvictions);
    profileStats.shadowRoiPctFetchLookups =
        sum(roi, &ShadowCounters::pctFetchLookups);
    profileStats.shadowRoiPctFetchHits =
        sum(roi, &ShadowCounters::pctFetchHits);
    profileStats.shadowRoiPctFetchMisses =
        sum(roi, &ShadowCounters::pctFetchMisses);
    profileStats.shadowRoiPctCommitLookups =
        sum(roi, &ShadowCounters::pctCommitLookups);
    profileStats.shadowRoiPctCommitHits =
        sum(roi, &ShadowCounters::pctCommitHits);
    profileStats.shadowRoiPctCommitMisses =
        sum(roi, &ShadowCounters::pctCommitMisses);
    profileStats.shadowRoiPctMismatches =
        sum(roi, &ShadowCounters::pctMismatches);
    profileStats.shadowRoiPctDemotions =
        sum(roi, &ShadowCounters::pctDemotions);
    profileStats.shadowRoiPctEvictions =
        sum(roi, &ShadowCounters::pctEvictions);
    profileStats.shadowRoiPredictionOffers =
        sum(roi, &ShadowCounters::predictionOffers);
    profileStats.shadowRoiPredictionCorrect =
        sum(roi, &ShadowCounters::predictionCorrect);
    profileStats.shadowRoiPredictionWrong =
        sum(roi, &ShadowCounters::predictionWrong);
    profileStats.shadowRoiPredictionEvictedBeforeCommit =
        sum(roi, &ShadowCounters::predictionEvictedBeforeCommit);
    profileStats.shadowRoiPredictionWrongAfterEviction =
        sum(roi, &ShadowCounters::predictionWrongAfterEviction);
    profileStats.shadowRoiQfPeakOccupancy =
        sum(roi, &ShadowCounters::qfPeakOccupancy);
    profileStats.shadowRoiPctPeakOccupancy =
        sum(roi, &ShadowCounters::pctPeakOccupancy);
    profileStats.shadowRoiQfMaxSetOccupancy =
        sum(roi, &ShadowCounters::qfMaxSetOccupancy);
    profileStats.shadowRoiPctMaxSetOccupancy =
        sum(roi, &ShadowCounters::pctMaxSetOccupancy);

    profileStats.shadowLifetimeCommittedUpdates =
        sum(lifetime, &ShadowCounters::committedUpdates);
    profileStats.shadowLifetimeQfPromotions =
        sum(lifetime, &ShadowCounters::qfPromotions);
    profileStats.shadowLifetimeQfEvictions =
        sum(lifetime, &ShadowCounters::qfEvictions);
    profileStats.shadowLifetimeQfQualifiedEvictions =
        sum(lifetime, &ShadowCounters::qfQualifiedEvictions);
    profileStats.shadowLifetimePctMismatches =
        sum(lifetime, &ShadowCounters::pctMismatches);
    profileStats.shadowLifetimePctDemotions =
        sum(lifetime, &ShadowCounters::pctDemotions);
    profileStats.shadowLifetimePctEvictions =
        sum(lifetime, &ShadowCounters::pctEvictions);
    profileStats.shadowLifetimePredictionOffers =
        sum(lifetime, &ShadowCounters::predictionOffers);
    profileStats.shadowLifetimePredictionCorrect =
        sum(lifetime, &ShadowCounters::predictionCorrect);
    profileStats.shadowLifetimePredictionWrong =
        sum(lifetime, &ShadowCounters::predictionWrong);
    profileStats.shadowLifetimePredictionWrongAfterEviction =
        sum(lifetime, &ShadowCounters::predictionWrongAfterEviction);
}

void
IdealConstantLVP::dumpProfile() const
{
    auto out_handle = simout.create("ideal_constant_lvp_profile.csv", false,
            true);
    auto &out = *out_handle->stream();

    out << "# ideal_constant_lvp_profile_v2\n";
    out << "# sat_counter_bits=" << satCounterBits << "\n";
    out << "# reset_confidence=" << resetConfidence << "\n";
    out << "scope,tid,pc,updates,first_update,last_update,value_changes,"
           "saturation_transitions,saturated_updates,first_saturation_update,"
           "ever_saturated,saturated_at_end,confidence,value,"
           "committed_saturated_offers,prediction_uses,"
           "correct_prediction_uses\n";

    auto dump_scope = [this, &out](const char *scope,
            const std::vector<ProfileTable> &profile_tables) {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            std::vector<std::pair<Addr, const ProfileEntry *>> entries;
            entries.reserve(profile_tables[tid].size());
            for (const auto &[pc, profile_entry] : profile_tables[tid]) {
                entries.emplace_back(pc, &profile_entry);
            }
            std::sort(entries.begin(), entries.end(),
                    [](const auto &left, const auto &right) {
                        return left.first < right.first;
                    });

            const auto &ideal_const_table = idealConstTables[tid];
            for (const auto &[pc, profile_entry] : entries) {
                const auto ideal_it = ideal_const_table.find(pc);
                gem5_assert(ideal_it != ideal_const_table.end(),
                        "Profiled PC %#llx is missing from IdealConstantLVP\n",
                        static_cast<unsigned long long>(pc));
                const auto &ideal_entry = ideal_it->second;
                out << scope << ',' << tid << ",0x" << std::hex << pc
                    << std::dec << ',' << profile_entry->updates << ','
                    << profile_entry->firstUpdate << ','
                    << profile_entry->lastUpdate << ','
                    << profile_entry->valueChanges << ','
                    << profile_entry->saturationTransitions << ','
                    << profile_entry->saturatedUpdates << ','
                    << profile_entry->firstSaturationUpdate << ','
                    << profile_entry->everSaturated << ','
                    << ideal_entry.confidence.isSaturated() << ','
                    << static_cast<uint64_t>(ideal_entry.confidence) << ",0x"
                    << std::hex << ideal_entry.value << std::dec << ','
                    << profile_entry->committedSaturatedOffers << ','
                    << profile_entry->predictionUses << ','
                    << profile_entry->correctPredictionUses << '\n';
            }
        }
    };

    dump_scope("lifetime", lifetimeProfileTables);
    dump_scope("roi", roiProfileTables);
    simout.close(out_handle);
}

void
IdealConstantLVP::dumpPredictionIntervals() const
{
    auto out_handle = simout.create("ideal_constant_lvp_prediction_intervals.csv",
            false, true);
    auto &out = *out_handle->stream();

    out << "# ideal_constant_lvp_prediction_intervals_v2\n";
    out << "# interval_definition=first_and_last_committed_applied_prediction_"
           "offered_during_the_same_saturated_confidence_epoch\n";
    out << "scope,tid,pc,saturation_epoch,first_prediction_use_seq_no,"
           "last_prediction_use_seq_no,saturation_end_seq_no,"
           "prediction_uses,correct_prediction_uses,open_at_end\n";

    auto dump_scope = [&out](const char *scope,
            const std::vector<PredictionIntervals> &all_intervals) {
        for (const auto &intervals : all_intervals) {
            for (const auto &interval : intervals) {
                if (interval.predictionUses == 0)
                    continue;
                gem5_assert(interval.firstPredictionUseSeqNo != 0,
                        "IdealConstantLVP interval has no first use\n");
                out << scope << ',' << interval.tid << ",0x" << std::hex
                    << interval.pc << std::dec << ','
                    << interval.saturationEpoch << ','
                    << interval.firstPredictionUseSeqNo << ','
                    << interval.lastPredictionUseSeqNo << ','
                    << interval.saturationEndSeqNo << ','
                    << interval.predictionUses << ','
                    << interval.correctPredictionUses << ','
                    << interval.openAtEnd << '\n';
            }
        }
    };

    dump_scope("lifetime", lifetimePredictionIntervals);
    dump_scope("roi", roiPredictionIntervals);
    simout.close(out_handle);
}

void
IdealConstantLVP::dumpShadowProfile() const
{
    if (!enableShadowProfiling || shadowQfTables.empty())
        return;

    auto out_handle = simout.create("ideal_constant_lvp_shadow.csv", false,
            true);
    auto &out = *out_handle->stream();
    const auto &qf_table0 = shadowQfTables.front();
    const auto &pct_table0 = shadowPctTables.front();

    out << "# ideal_constant_lvp_shadow_v1\n";
    out << "# sat_counter_bits=" << satCounterBits << "\n";
    out << "# shadow_qf_entries=" << shadowQfEntries << "\n";
    out << "# shadow_qf_ways=" << shadowQfWays << "\n";
    out << "# shadow_qf_sets=" << qf_table0.sets << "\n";
    out << "# shadow_pct_entries=" << shadowPctEntries << "\n";
    out << "# shadow_pct_ways=" << shadowPctWays << "\n";
    out << "# shadow_pct_sets=" << pct_table0.sets << "\n";
    out << "# shadow_qualification=" << shadowQualification << "\n";
    out << "# replacement=invalid-first-lru\n";
    out << "scope,tid,qf_entries,qf_ways,qf_sets,pct_entries,pct_ways,pct_sets,"
           "qualification,committed_updates,qf_lookups,qf_hits,qf_misses,"
           "qf_promotions,qf_evictions,qf_qualified_evictions,"
           "pct_fetch_lookups,pct_fetch_hits,pct_fetch_misses,"
           "pct_commit_lookups,pct_commit_hits,pct_commit_misses,"
           "pct_mismatches,pct_demotions,pct_evictions,prediction_offers,"
           "prediction_correct,prediction_wrong,prediction_evicted_before_commit,"
           "qf_reuse_references,qf_reuse_distance_sum,qf_reuse_distance_max,"
           "pct_reuse_references,pct_reuse_distance_sum,pct_reuse_distance_max,"
           "qf_current_occupancy,pct_current_occupancy,qf_peak_occupancy,"
           "pct_peak_occupancy,qf_max_set_occupancy,pct_max_set_occupancy,"
           "prediction_wrong_after_eviction\n";

    auto dump_scope = [this, &out](const char *scope,
            const std::vector<ShadowCounters> &counters,
            const std::vector<ShadowTable> &qf_tables,
            const std::vector<ShadowTable> &pct_tables) {
        for (ThreadID tid = 0; tid < numThreads; ++tid) {
            const auto &c = counters[tid];
            const auto &qf = qf_tables[tid];
            const auto &pct = pct_tables[tid];
            out << scope << ',' << tid << ',' << shadowQfEntries << ','
                << shadowQfWays << ',' << qf.sets << ',' << shadowPctEntries
                << ',' << shadowPctWays << ',' << pct.sets << ','
                << shadowQualification << ',' << c.committedUpdates << ','
                << c.qfLookups << ',' << c.qfHits << ',' << c.qfMisses << ','
                << c.qfPromotions << ',' << c.qfEvictions << ','
                << c.qfQualifiedEvictions << ',' << c.pctFetchLookups << ','
                << c.pctFetchHits << ',' << c.pctFetchMisses << ','
                << c.pctCommitLookups << ',' << c.pctCommitHits << ','
                << c.pctCommitMisses << ',' << c.pctMismatches << ','
                << c.pctDemotions << ',' << c.pctEvictions << ','
                << c.predictionOffers << ',' << c.predictionCorrect << ','
                << c.predictionWrong << ','
                << c.predictionEvictedBeforeCommit << ','
                << c.qfReuseReferences << ',' << c.qfReuseDistanceSum << ','
                << c.qfReuseDistanceMax << ',' << c.pctReuseReferences << ','
                << c.pctReuseDistanceSum << ',' << c.pctReuseDistanceMax << ','
                << qf.occupancy << ',' << pct.occupancy << ','
                << c.qfPeakOccupancy << ',' << c.pctPeakOccupancy << ','
                << c.qfMaxSetOccupancy << ',' << c.pctMaxSetOccupancy << ','
                << c.predictionWrongAfterEviction << '\n';
        }
    };

    dump_scope("lifetime", lifetimeShadowCounters, shadowQfTables,
            shadowPctTables);
    dump_scope("roi", roiShadowCounters, shadowQfTables, shadowPctTables);
    simout.close(out_handle);
}

VPResult
IdealConstantLVP::doPredict(Addr pc, ThreadID tid) const
{
    assertValidTid(tid);
    const auto &idealConstTable = idealConstTables[tid];
    auto it = idealConstTable.find(pc);
    if (it != idealConstTable.end()) {
        if (it->second.confidence.isSaturated()) {
            return {true, it->second.value};
        }
    }
    return {false, 0};
}

VPPredictionCandidate
IdealConstantLVP::predict(const VPPredictRequest &request)
{
    VPPredictionCandidate candidate;
    candidate.result = doPredict(request.pc, request.tid);

    if (enableShadowProfiling) {
        auto shadow_record = std::make_unique<ShadowPredictionRecord>();
        shadowPredict(request.pc, request.tid, *shadow_record);
        candidate.record = std::move(shadow_record);
    }

    if (candidate.result.speculative) {
        if (!candidate.record) {
            if (enableProfiling) {
                candidate.record =
                    std::make_unique<IdealConstantPredictionRecord>();
            } else {
                candidate.record = std::make_unique<VPPredictionRecord>();
            }
        }
        if (enableProfiling) {
            auto *profile_record = dynamic_cast<IdealConstantPredictionRecord *>(
                candidate.record.get());
            gem5_assert(profile_record,
                    "IdealConstantLVP profile record type mismatch\n");
            const auto &entry = idealConstTables[request.tid].at(request.pc);
            gem5_assert(entry.saturationEpoch != 0,
                    "Saturated IdealConstantLVP entry has no epoch\n");
            profile_record->saturationEpoch = entry.saturationEpoch;
        }
        candidate.record->offeredPrediction = true;
        candidate.record->predictedValue = candidate.result.value;
    }
    return candidate;
}

void
IdealConstantLVP::doUpdate(Addr pc, ThreadID tid, RegVal actualValue,
        const VPFeedback &feedback, bool is_misprediction,
        uint64_t committed_seq_no, uint64_t prediction_epoch)
{
    assertValidTid(tid);
    auto &idealConstTable = idealConstTables[tid];
    auto it = idealConstTable.find(pc);
    const bool had_entry = it != idealConstTable.end();
    const bool was_saturated = had_entry && it->second.confidence.isSaturated();
    const uint64_t previous_saturation_epoch = was_saturated ?
        it->second.saturationEpoch : 0;
    bool value_changed = false;
    if (it == idealConstTable.end()) {
        // Not found, allocate a new entry
        auto [it, success] = idealConstTable.emplace(std::piecewise_construct,
            std::forward_as_tuple(pc),
            std::forward_as_tuple(satCounterBits, actualValue));

        assert(success);
    } else {
        // Found
        bool validActualValue = actualValue != 0xdeadbeefULL;
        if (validActualValue && actualValue == it->second.value) {
            it->second.confidence++;
        } else {
            value_changed = true;
            if (resetConfidence) {
                it->second.confidence.reset();
            } else {
                it->second.confidence--;
            }
            it->second.value = actualValue;
        }
    }

    auto &entry = idealConstTable.at(pc);
    const bool is_saturated = entry.confidence.isSaturated();
    observeSaturationTransition(was_saturated, is_saturated);

    if (enableProfiling) {
        uint64_t saturation_epoch_started = 0;
        uint64_t saturation_epoch_ended = 0;
        if (!was_saturated && is_saturated) {
            entry.saturationEpoch++;
            gem5_assert(entry.saturationEpoch != 0,
                    "IdealConstantLVP saturation epoch overflow\n");
            saturation_epoch_started = entry.saturationEpoch;
        }
        if (was_saturated && !is_saturated) {
            gem5_assert(previous_saturation_epoch != 0,
                    "Saturated IdealConstantLVP entry has no epoch\n");
            saturation_epoch_ended = previous_saturation_epoch;
        }
        updateProfile(lifetimeProfileTables[tid], pc,
                ++lifetimeProfileUpdateSequences[tid], committed_seq_no,
                tid, lifetimePredictionIntervals[tid], value_changed,
                was_saturated, is_saturated, saturation_epoch_started,
                saturation_epoch_ended, prediction_epoch,
                feedback.offeredPrediction,
                feedback.applied,
                feedback.applied && !is_misprediction, false);
        updateProfile(roiProfileTables[tid], pc,
                ++roiProfileUpdateSequences[tid], committed_seq_no,
                tid, roiPredictionIntervals[tid], value_changed,
                was_saturated, is_saturated, saturation_epoch_started,
                saturation_epoch_ended, prediction_epoch,
                feedback.offeredPrediction,
                feedback.applied,
                feedback.applied && !is_misprediction, true);
    }
}

void
IdealConstantLVP::update(const VPUpdateInfo &updateInfo,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    uint64_t prediction_epoch = 0;
    if (enableProfiling && feedback.offeredPrediction) {
        auto *profile_record =
            dynamic_cast<const IdealConstantPredictionRecord *>(record);
        gem5_assert(profile_record,
                "IdealConstantLVP profile record type mismatch\n");
        prediction_epoch = profile_record->saturationEpoch;
        gem5_assert(prediction_epoch != 0,
                "IdealConstantLVP prediction has no saturation epoch\n");
    }
    doUpdate(updateInfo.pc, updateInfo.tid, updateInfo.actualValue, feedback,
            updateInfo.isMisprediction, updateInfo.seqNo, prediction_epoch);
    if (enableShadowProfiling) {
        shadowUpdate(updateInfo.pc, updateInfo.tid, updateInfo.actualValue,
                record);
    }
}

void
IdealConstantLVP::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    (void)specUpdateInfo;
}

void
IdealConstantLVP::squash(ThreadID tid, const uint64_t seq_no)
{
    (void)tid;
    (void)seq_no;
    // Do nothing
}

} // namespace valuepred

} // namespace gem5
