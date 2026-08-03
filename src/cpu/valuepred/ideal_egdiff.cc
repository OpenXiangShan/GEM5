#include "cpu/valuepred/ideal_egdiff.hh"

#include <algorithm>
#include <tuple>
#include <utility>

#include "base/logging.hh"
#include "base/stats/units.hh"
#include "base/trace.hh"
#include "debug/IdealEgDiff.hh"

namespace gem5
{

namespace valuepred
{

IdealEgDiff::IdealEgDiffStats::IdealEgDiffStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(dispatchSlots, statistics::units::Count::get(),
               "Dynamic load slots allocated at dispatch"),
      ADD_STAT(valueAvailableUpdates, statistics::units::Count::get(),
               "Actual load values written into global history"),
      ADD_STAT(latePredictAttempts, statistics::units::Count::get(),
               "Issue-stage late prediction attempts"),
      ADD_STAT(latePredictions, statistics::units::Count::get(),
               "Issue-stage predictions offered"),
      ADD_STAT(lateCorrect, statistics::units::Count::get(),
               "Issue-stage predictions verified as correct at commit"),
      ADD_STAT(lateIncorrect, statistics::units::Count::get(),
               "Issue-stage predictions verified as incorrect at commit"),
      ADD_STAT(lateBaseUnavailable, statistics::units::Count::get(),
               "Late predictions blocked by an unavailable base value"),
      ADD_STAT(lateNoEntry, statistics::units::Count::get(),
               "Late predictions without a trained PC entry"),
      ADD_STAT(lateConfidenceSuppressed, statistics::units::Count::get(),
               "Late predictions suppressed by confidence"),
      ADD_STAT(diffMatches, statistics::units::Count::get(),
               "Commit updates matching the stored global difference"),
      ADD_STAT(diffMismatches, statistics::units::Count::get(),
               "Commit updates mismatching the stored global difference"),
      ADD_STAT(pollingDistanceChanges, statistics::units::Count::get(),
               "Distance changes made by deterministic polling"),
      ADD_STAT(squashedSlots, statistics::units::Count::get(),
               "Speculative global-history slots removed by squash"),
      ADD_STAT(staleValueCallbacks, statistics::units::Count::get(),
               "Value callbacks discarded after slot removal or reuse"),
      ADD_STAT(historyEntries, statistics::units::Count::get(),
               "Current global-history entries across all threads"),
      ADD_STAT(maxHistoryEntries, statistics::units::Count::get(),
               "Maximum global-history entries across all threads"),
      ADD_STAT(prunedHistoryEntries, statistics::units::Count::get(),
               "Committed global-history entries removed as unreachable")
{
}

IdealEgDiff::IdealEgDiff(const Params &params)
    : VPUnit(params), order(params.order),
      confidenceBits(params.confidenceBits), states(params.numThreads),
      egdiffStats(this)
{
    fatal_if(order == 0, "IdealEgDiff order must be non-zero");
    fatal_if(confidenceBits == 0 || confidenceBits > 16,
             "IdealEgDiff confidenceBits must be in [1, 16]");
}

IdealEgDiffPredictionRecord *
IdealEgDiff::getRecord(VPPredictionRecord *record) const
{
    auto *egdiff_record =
        dynamic_cast<IdealEgDiffPredictionRecord *>(record);
    gem5_assert(egdiff_record, "IdealEgDiff expects its prediction record");
    return egdiff_record;
}

const IdealEgDiffPredictionRecord *
IdealEgDiff::getRecord(const VPPredictionRecord *record) const
{
    auto *egdiff_record =
        dynamic_cast<const IdealEgDiffPredictionRecord *>(record);
    gem5_assert(egdiff_record, "IdealEgDiff expects its prediction record");
    return egdiff_record;
}

IdealEgDiff::HistoryEntry *
IdealEgDiff::findHistory(ThreadState &state, uint64_t ordinal)
{
    auto it = state.history.find(ordinal);
    return it == state.history.end() ? nullptr : &it->second;
}

const IdealEgDiff::HistoryEntry *
IdealEgDiff::findHistory(const ThreadState &state, uint64_t ordinal) const
{
    auto it = state.history.find(ordinal);
    return it == state.history.end() ? nullptr : &it->second;
}

void
IdealEgDiff::updateHistoryOccupancyStats()
{
    uint64_t entries = 0;
    for (const auto &state : states) {
        entries += state.history.size();
    }
    maxHistoryEntriesSeen = std::max(maxHistoryEntriesSeen, entries);
    egdiffStats.historyEntries = entries;
    egdiffStats.maxHistoryEntries = maxHistoryEntriesSeen;
}

void
IdealEgDiff::pruneHistory(ThreadState &state, ThreadID tid)
{
    uint64_t oldest_inflight = state.nextOrdinal;
    for (const auto &[ordinal, slot] : state.history) {
        if (!slot.committed) {
            oldest_inflight = ordinal;
            break;
        }
    }

    const uint64_t oldest_needed = oldest_inflight > order ?
        oldest_inflight - order : 0;
    unsigned removed = 0;
    for (auto it = state.history.begin();
         it != state.history.end() && it->first < oldest_needed;) {
        if (it->second.committed) {
            it = state.history.erase(it);
            removed++;
        } else {
            ++it;
        }
    }

    egdiffStats.prunedHistoryEntries += removed;
    updateHistoryOccupancyStats();
    if (removed != 0) {
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][prune] tid=%u removed=%u oldestInflight=%llu "
                "oldestNeeded=%llu historyEntries=%zu nextOrdinal=%llu\n",
                tid, removed, oldest_inflight, oldest_needed,
                state.history.size(), state.nextOrdinal);
    }
}

VPPredictionCandidate
IdealEgDiff::predict(const VPPredictRequest &request)
{
    assertValidTid(request.tid);
    VPPredictionCandidate candidate;
    candidate.record = std::make_unique<IdealEgDiffPredictionRecord>();

    const auto &table = states[request.tid].table;
    auto it = table.find(request.pc);
    if (it == table.end()) {
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][fetch] tid=%u seq=%llu pc=%#lx entry=miss\n",
                request.tid, request.seqNo, request.pc);
    } else {
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][fetch] tid=%u seq=%llu pc=%#lx "
                "entry=hit distance=%u diff=%#llx confidence=%u/%u\n",
                request.tid, request.seqNo, request.pc, it->second.distance,
                static_cast<unsigned long long>(it->second.diff),
                static_cast<unsigned>(it->second.confidence),
                (1U << confidenceBits) - 1);
    }
    return candidate;
}

void
IdealEgDiff::dispatch(const VPDispatchInfo &dispatchInfo,
        VPPredictionRecord *record)
{
    assertValidTid(dispatchInfo.tid);
    auto *egdiff_record = getRecord(record);
    if (egdiff_record->slotAllocated) {
        return;
    }

    auto &state = states[dispatchInfo.tid];
    const uint64_t ordinal = state.nextOrdinal++;
    auto [history_it, inserted] = state.history.emplace(ordinal,
            HistoryEntry{ordinal, dispatchInfo.seqNo, dispatchInfo.pc});
    gem5_assert(inserted, "IdealEgDiff allocated a duplicate load ordinal");
    (void)history_it;

    egdiff_record->slotAllocated = true;
    egdiff_record->loadOrdinal = ordinal;
    egdiffStats.dispatchSlots++;
    updateHistoryOccupancyStats();
    DPRINTF(IdealEgDiff,
            "[IdealEgDiff][dispatch] tid=%u seq=%llu pc=%#lx ordinal=%llu\n",
            dispatchInfo.tid, dispatchInfo.seqNo, dispatchInfo.pc, ordinal);
}

VPPredictionCandidate
IdealEgDiff::latePredict(const VPLatePredictRequest &request,
        VPPredictionRecord *record)
{
    assertValidTid(request.tid);
    VPPredictionCandidate candidate;
    auto *egdiff_record = getRecord(record);
    egdiffStats.latePredictAttempts++;

    if (!egdiff_record->slotAllocated) {
        egdiffStats.lateBaseUnavailable++;
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][late] tid=%u seq=%llu pc=%#lx "
                "result=no-slot\n",
                request.tid, request.seqNo, request.pc);
        return candidate;
    }

    auto &state = states[request.tid];
    auto table_it = state.table.find(request.pc);
    if (table_it == state.table.end()) {
        egdiffStats.lateNoEntry++;
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][late] tid=%u seq=%llu pc=%#lx ordinal=%llu "
                "result=no-entry\n",
                request.tid, request.seqNo, request.pc,
                egdiff_record->loadOrdinal);
        return candidate;
    }

    auto &entry = table_it->second;
    if (!entry.confidence.isSaturated()) {
        egdiffStats.lateConfidenceSuppressed++;
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][late] tid=%u seq=%llu pc=%#lx ordinal=%llu "
                "distance=%u diff=%#llx confidence=%u/%u "
                "result=confidence-suppressed\n",
                request.tid, request.seqNo, request.pc,
                egdiff_record->loadOrdinal, entry.distance,
                static_cast<unsigned long long>(entry.diff),
                static_cast<unsigned>(entry.confidence),
                (1U << confidenceBits) - 1);
        return candidate;
    }
    if (egdiff_record->loadOrdinal < entry.distance) {
        egdiffStats.lateBaseUnavailable++;
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][late] tid=%u seq=%llu pc=%#lx ordinal=%llu "
                "distance=%u result=history-too-short\n",
                request.tid, request.seqNo, request.pc,
                egdiff_record->loadOrdinal, entry.distance);
        return candidate;
    }

    const uint64_t base_ordinal =
        egdiff_record->loadOrdinal - entry.distance;
    const auto *base = findHistory(state, base_ordinal);
    if (!base || !base->actualValid) {
        egdiffStats.lateBaseUnavailable++;
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][late] tid=%u seq=%llu pc=%#lx ordinal=%llu "
                "distance=%u baseOrdinal=%llu result=base-unavailable\n",
                request.tid, request.seqNo, request.pc,
                egdiff_record->loadOrdinal, entry.distance, base_ordinal);
        return candidate;
    }

    const RegVal predicted = base->actualValue + entry.diff;
    candidate.result = {true, predicted};
    candidate.score = static_cast<uint64_t>(entry.confidence);
    egdiff_record->offeredPrediction = true;
    egdiff_record->predictedValue = predicted;
    egdiffStats.latePredictions++;
    DPRINTF(IdealEgDiff,
            "[IdealEgDiff][late] tid=%u seq=%llu pc=%#lx ordinal=%llu "
            "baseSeq=%llu basePc=%#lx baseOrdinal=%llu base=%#llx "
            "distance=%u diff=%#llx predicted=%#llx result=offered\n",
            request.tid, request.seqNo, request.pc,
            egdiff_record->loadOrdinal, base->seqNo, base->pc, base_ordinal,
            static_cast<unsigned long long>(base->actualValue),
            entry.distance, static_cast<unsigned long long>(entry.diff),
            static_cast<unsigned long long>(predicted));
    return candidate;
}

void
IdealEgDiff::valueAvailable(const VPValueAvailableInfo &valueInfo,
        VPPredictionRecord *record)
{
    assertValidTid(valueInfo.tid);
    auto *egdiff_record = getRecord(record);
    if (!egdiff_record->slotAllocated) {
        return;
    }

    auto &state = states[valueInfo.tid];
    auto *slot = findHistory(state, egdiff_record->loadOrdinal);
    if (!slot || slot->seqNo != valueInfo.seqNo) {
        egdiffStats.staleValueCallbacks++;
        DPRINTF(IdealEgDiff,
                "[IdealEgDiff][value-stale] tid=%u seq=%llu pc=%#lx "
                "recordOrdinal=%llu slotPresent=%d slotSeq=%llu "
                "nextOrdinal=%llu\n",
                valueInfo.tid, valueInfo.seqNo, valueInfo.pc,
                egdiff_record->loadOrdinal, slot != nullptr,
                slot ? slot->seqNo : 0, state.nextOrdinal);
        return;
    }
    slot->actualValue = valueInfo.actualValue;
    slot->actualValid = true;
    egdiffStats.valueAvailableUpdates++;
    DPRINTF(IdealEgDiff,
            "[IdealEgDiff][value] tid=%u seq=%llu pc=%#lx ordinal=%llu "
            "actual=%#llx\n",
            valueInfo.tid, valueInfo.seqNo, valueInfo.pc,
            egdiff_record->loadOrdinal,
            static_cast<unsigned long long>(valueInfo.actualValue));
}

void
IdealEgDiff::update(const VPUpdateInfo &updateInfo,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    assertValidTid(updateInfo.tid);
    const auto *egdiff_record = getRecord(record);
    if (!egdiff_record->slotAllocated) {
        return;
    }

    auto &state = states[updateInfo.tid];
    auto *target = findHistory(state, egdiff_record->loadOrdinal);
    gem5_assert(target && target->seqNo == updateInfo.seqNo,
            "IdealEgDiff commit does not match its history slot");
    target->actualValue = updateInfo.actualValue;
    target->actualValid = true;
    target->committed = true;
    if (feedback.offeredPrediction) {
        if (feedback.wouldHaveBeenCorrect) {
            egdiffStats.lateCorrect++;
        } else {
            egdiffStats.lateIncorrect++;
        }
    }

    auto table_it = state.table.find(updateInfo.pc);
    if (table_it == state.table.end()) {
        if (egdiff_record->loadOrdinal >= order) {
            const auto *base = findHistory(
                    state, egdiff_record->loadOrdinal - order);
            if (base && base->actualValid) {
                const RegVal diff = updateInfo.actualValue - base->actualValue;
                state.table.emplace(std::piecewise_construct,
                        std::forward_as_tuple(updateInfo.pc),
                        std::forward_as_tuple(order, diff, confidenceBits));
                DPRINTF(IdealEgDiff,
                        "[IdealEgDiff][commit] tid=%u seq=%llu pc=%#lx "
                        "allocate distance=%u basePc=%#lx diff=%#llx\n",
                        updateInfo.tid, updateInfo.seqNo, updateInfo.pc,
                        order, base->pc,
                        static_cast<unsigned long long>(diff));
            }
        }
    } else {
        auto &entry = table_it->second;
        if (egdiff_record->loadOrdinal >= entry.distance) {
            const auto *base = findHistory(state,
                    egdiff_record->loadOrdinal - entry.distance);
            if (base && base->actualValid) {
                const RegVal actual_diff =
                    updateInfo.actualValue - base->actualValue;
                if (actual_diff == entry.diff) {
                    entry.confidence++;
                    egdiffStats.diffMatches++;
                    DPRINTF(IdealEgDiff,
                            "[IdealEgDiff][commit] tid=%u seq=%llu pc=%#lx "
                            "match basePc=%#lx distance=%u diff=%#llx "
                            "confidence=%u/%u applied=%d correct=%d\n",
                            updateInfo.tid, updateInfo.seqNo, updateInfo.pc,
                            base->pc, entry.distance,
                            static_cast<unsigned long long>(entry.diff),
                            static_cast<unsigned>(entry.confidence),
                            (1U << confidenceBits) - 1,
                            feedback.applied, feedback.wouldHaveBeenCorrect);
                } else {
                    entry.confidence.reset();
                    egdiffStats.diffMismatches++;
                    entry.distance = entry.distance > 1 ?
                        entry.distance - 1 : order;
                    egdiffStats.pollingDistanceChanges++;
                    if (egdiff_record->loadOrdinal >= entry.distance) {
                        const auto *new_base = findHistory(state,
                                egdiff_record->loadOrdinal - entry.distance);
                        if (new_base && new_base->actualValid) {
                            entry.diff = updateInfo.actualValue -
                                new_base->actualValue;
                            DPRINTF(IdealEgDiff,
                                    "[IdealEgDiff][commit] tid=%u seq=%llu "
                                    "pc=%#lx mismatch oldActualDiff=%#llx "
                                    "newDistance=%u newBasePc=%#lx "
                                    "newDiff=%#llx\n",
                                    updateInfo.tid, updateInfo.seqNo,
                                    updateInfo.pc,
                                    static_cast<unsigned long long>(actual_diff),
                                    entry.distance, new_base->pc,
                                    static_cast<unsigned long long>(entry.diff));
                        }
                    }
                }
            }
        }
    }

    DPRINTF(IdealEgDiff,
            "[IdealEgDiff][commit] tid=%u seq=%llu pc=%#lx ordinal=%llu "
            "actual=%#llx mispred=%d committed=1\n",
            updateInfo.tid, updateInfo.seqNo, updateInfo.pc,
            egdiff_record->loadOrdinal,
            static_cast<unsigned long long>(updateInfo.actualValue),
            updateInfo.isMisprediction);
    pruneHistory(state, updateInfo.tid);
}

void
IdealEgDiff::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    (void)specUpdateInfo;
}

void
IdealEgDiff::squash(ThreadID tid, const uint64_t seq_no)
{
    assertValidTid(tid);
    auto &state = states[tid];
    unsigned removed = 0;
    uint64_t rewind_ordinal = state.nextOrdinal;
    for (auto it = state.history.begin(); it != state.history.end();) {
        if (!it->second.committed && it->second.seqNo > seq_no) {
            rewind_ordinal = std::min(rewind_ordinal, it->first);
            it = state.history.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    state.nextOrdinal = rewind_ordinal;
    egdiffStats.squashedSlots += removed;
    updateHistoryOccupancyStats();
    DPRINTF(IdealEgDiff,
            "[IdealEgDiff][squash] tid=%u retainedSeq=%llu removed=%u "
            "nextOrdinal=%llu\n",
            tid, seq_no, removed, state.nextOrdinal);
}

} // namespace valuepred
} // namespace gem5
