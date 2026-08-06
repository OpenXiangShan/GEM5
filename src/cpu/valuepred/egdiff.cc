#include "cpu/valuepred/egdiff.hh"

#include <algorithm>
#include <tuple>
#include <utility>

#include "base/logging.hh"
#include "base/stats/units.hh"
#include "base/trace.hh"
#include "debug/EgDiff.hh"

namespace gem5
{

namespace valuepred
{

EgDiff::EgDiffStats::EgDiffStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(dispatchSlots, statistics::units::Count::get(),
               "Dynamic load slots allocated at dispatch"),
      ADD_STAT(predictionRequests, statistics::units::Count::get(),
               "Eligible prediction requests created at dispatch"),
      ADD_STAT(normalRequests, statistics::units::Count::get(),
               "Requests whose base was available at dispatch"),
      ADD_STAT(deferredBindings, statistics::units::Count::get(),
               "Requests bound to an unavailable base"),
      ADD_STAT(deferredWakeups, statistics::units::Count::get(),
               "Deferred requests awakened by base availability"),
      ADD_STAT(predictionsOffered, statistics::units::Count::get(),
               "Ready EgDiff predictions offered to arbitration"),
      ADD_STAT(predictionsApplied, statistics::units::Count::get(),
               "EgDiff predictions selected and applied"),
      ADD_STAT(appliedCorrect, statistics::units::Count::get(),
               "Applied EgDiff predictions verified correct"),
      ADD_STAT(appliedIncorrect, statistics::units::Count::get(),
               "Applied EgDiff predictions verified incorrect"),
      ADD_STAT(actualBaseUses, statistics::units::Count::get(),
               "Offered predictions based on actual values"),
      ADD_STAT(predictedBaseUses, statistics::units::Count::get(),
               "Offered predictions based on predicted values"),
      ADD_STAT(confidenceSuppressions, statistics::units::Count::get(),
               "Dispatches suppressed because FPC was not saturated"),
      ADD_STAT(lastMispSuppressions, statistics::units::Count::get(),
               "Prediction opportunities suppressed by last-misp"),
      ADD_STAT(noEntry, statistics::units::Count::get(),
               "Dispatches without a trained exact-PC entry"),
      ADD_STAT(historyTooShort, statistics::units::Count::get(),
               "Eligible dispatches without enough global history"),
      ADD_STAT(targetCompletedDrops, statistics::units::Count::get(),
               "Deferred requests dropped after target completion"),
      ADD_STAT(valueAvailableUpdates, statistics::units::Count::get(),
               "Actual values written into speculative history"),
      ADD_STAT(diffMatches, statistics::units::Count::get(),
               "Committed differences matching the table entry"),
      ADD_STAT(diffMismatches, statistics::units::Count::get(),
               "Committed differences mismatching the table entry"),
      ADD_STAT(fpcAdvances, statistics::units::Count::get(),
               "Probabilistic FPC forward transitions"),
      ADD_STAT(fpcHolds, statistics::units::Count::get(),
               "Matching updates that did not advance the FPC"),
      ADD_STAT(pollingDistanceChanges, statistics::units::Count::get(),
               "Distance changes made by polling"),
      ADD_STAT(lastMispActivations, statistics::units::Count::get(),
               "Applied EgDiff mispredictions starting suppression"),
      ADD_STAT(squashedSlots, statistics::units::Count::get(),
               "Speculative global-history slots removed by squash"),
      ADD_STAT(cancelledRequests, statistics::units::Count::get(),
               "Pending or ready requests removed by squash"),
      ADD_STAT(staleValueCallbacks, statistics::units::Count::get(),
               "Value callbacks discarded after removal or ordinal reuse"),
      ADD_STAT(tableEntries, statistics::units::Count::get(),
               "Exact-PC prediction-table entries"),
      ADD_STAT(tableConflicts, statistics::units::Count::get(),
               "Table conflicts; always zero for the exact table"),
      ADD_STAT(tableReplacements, statistics::units::Count::get(),
               "Table replacements; always zero for the unbounded table"),
      ADD_STAT(tableEvictions, statistics::units::Count::get(),
               "Table evictions; always zero for the unbounded table"),
      ADD_STAT(historyCapacityDrops, statistics::units::Count::get(),
               "History capacity drops; always zero for exact history"),
      ADD_STAT(historyEntries, statistics::units::Count::get(),
               "Current global-history entries across all threads"),
      ADD_STAT(maxHistoryEntries, statistics::units::Count::get(),
               "Maximum global-history entries across all threads"),
      ADD_STAT(prunedHistoryEntries, statistics::units::Count::get(),
               "Committed global-history entries removed as unreachable")
{
}

EgDiff::EgDiff(const Params &params)
    : VPUnit(params), order(params.order), fpcSeed(params.fpcSeed),
      normalPredictionLatency(params.normalPredictionLatency),
      deferredPredictionLatency(params.deferredPredictionLatency),
      lastMispWindow(params.lastMispWindow), states(params.numThreads),
      egdiffStats(this)
{
    fatal_if(order == 0, "EgDiff order must be non-zero");
    fatal_if(normalPredictionLatency == 0,
             "EgDiff normal prediction latency must be non-zero");
    fatal_if(deferredPredictionLatency == 0,
             "EgDiff deferred prediction latency must be non-zero");
    fatal_if(lastMispWindow == 0,
             "EgDiff last-misprediction window must be non-zero");
}

EgDiffPredictionRecord *
EgDiff::getRecord(VPPredictionRecord *record) const
{
    auto *egdiff_record = dynamic_cast<EgDiffPredictionRecord *>(record);
    gem5_assert(egdiff_record, "EgDiff expects its prediction record");
    return egdiff_record;
}

const EgDiffPredictionRecord *
EgDiff::getRecord(const VPPredictionRecord *record) const
{
    auto *egdiff_record = dynamic_cast<const EgDiffPredictionRecord *>(record);
    gem5_assert(egdiff_record, "EgDiff expects its prediction record");
    return egdiff_record;
}

EgDiff::HistoryEntry *
EgDiff::findHistory(ThreadState &state, uint64_t ordinal)
{
    auto it = state.history.find(ordinal);
    return it == state.history.end() ? nullptr : &it->second;
}

const EgDiff::HistoryEntry *
EgDiff::findHistory(const ThreadState &state, uint64_t ordinal) const
{
    auto it = state.history.find(ordinal);
    return it == state.history.end() ? nullptr : &it->second;
}

uint64_t
EgDiff::initialRandomState(ThreadID tid, Addr pc) const
{
    uint64_t value = fpcSeed ^ (static_cast<uint64_t>(tid) << 56) ^ pc;
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value ? value : 1;
}

bool
EgDiff::advanceFpc(Entry &entry)
{
    if (entry.fpc == MaxFpc) {
        return false;
    }

    if (entry.fpc == 0) {
        entry.fpc = 1;
        return true;
    }

    entry.randomState ^= entry.randomState >> 12;
    entry.randomState ^= entry.randomState << 25;
    entry.randomState ^= entry.randomState >> 27;
    const uint64_t sample =
        entry.randomState * 0x2545f4914f6cdd1dULL;
    const bool advance = entry.fpc <= 2 ?
        ((sample & 3) == 0) : ((sample & 7) == 0);
    if (advance) {
        entry.fpc++;
    }
    return advance;
}

const char *
EgDiff::valueSourceName(ValueSource source)
{
    switch (source) {
      case ValueSource::Predicted:
        return "predicted";
      case ValueSource::Actual:
        return "actual";
      default:
        return "none";
    }
}

void
EgDiff::wakeDeferred(ThreadState &state, ThreadID tid,
        HistoryEntry &base, uint64_t cycle)
{
    for (auto &[ordinal, target] : state.history) {
        if (!target.requestPending || target.baseOrdinal != base.ordinal) {
            continue;
        }
        if (target.completed) {
            target.requestPending = false;
            egdiffStats.targetCompletedDrops++;
            DPRINTF(EgDiff,
                    "[EgDiff][deferred-drop] tid=%u seq=%llu pc=%#lx "
                    "ordinal=%llu baseOrdinal=%llu reason=target-completed\n",
                    tid, target.seqNo, target.pc, ordinal, base.ordinal);
            continue;
        }
        gem5_assert(ordinal >= target.predictionDistance &&
                    ordinal - target.predictionDistance == base.ordinal,
                    "EgDiff deferred distance no longer identifies its base");
        target.requestPending = false;
        target.requestReady = true;
        target.readyCycle = cycle + deferredPredictionLatency;
        egdiffStats.deferredWakeups++;
        DPRINTF(EgDiff,
                "[EgDiff][deferred-wake] tid=%u seq=%llu pc=%#lx "
                "ordinal=%llu baseSeq=%llu basePc=%#lx baseOrdinal=%llu "
                "readyCycle=%llu\n",
                tid, target.seqNo, target.pc, ordinal, base.seqNo, base.pc,
                base.ordinal, target.readyCycle);
    }
}

void
EgDiff::makeSpecValueAvailable(ThreadState &state, ThreadID tid,
        HistoryEntry &base, RegVal value, ValueSource source, uint64_t cycle)
{
    base.specValue = value;
    base.specValueValid = true;
    base.valueSource = source;
    wakeDeferred(state, tid, base, cycle);
}

void
EgDiff::updateHistoryOccupancyStats()
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
EgDiff::updateTableEntryStats()
{
    uint64_t entries = 0;
    for (const auto &state : states) {
        entries += state.table.size();
    }
    egdiffStats.tableEntries = entries;
}

void
EgDiff::pruneHistory(ThreadState &state, ThreadID tid)
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
    if (removed) {
        DPRINTF(EgDiff,
                "[EgDiff][prune] tid=%u removed=%u oldestInflight=%llu "
                "oldestNeeded=%llu historyEntries=%zu\n",
                tid, removed, oldest_inflight, oldest_needed,
                state.history.size());
    }
}

VPPredictionCandidate
EgDiff::predict(const VPPredictRequest &request)
{
    assertValidTid(request.tid);
    VPPredictionCandidate candidate;
    candidate.record = std::make_unique<EgDiffPredictionRecord>();
    return candidate;
}

void
EgDiff::dispatch(const VPDispatchInfo &info, VPPredictionRecord *record)
{
    assertValidTid(info.tid);
    auto *egdiff_record = getRecord(record);
    if (egdiff_record->slotAllocated) {
        return;
    }

    auto &state = states[info.tid];
    const uint64_t ordinal = state.nextOrdinal++;
    HistoryEntry new_entry;
    new_entry.ordinal = ordinal;
    new_entry.seqNo = info.seqNo;
    new_entry.pc = info.pc;
    auto [it, inserted] = state.history.emplace(ordinal, new_entry);
    gem5_assert(inserted, "EgDiff allocated a duplicate load ordinal");
    auto &target = it->second;
    egdiff_record->slotAllocated = true;
    egdiff_record->loadOrdinal = ordinal;
    egdiffStats.dispatchSlots++;
    updateHistoryOccupancyStats();
    DPRINTF(EgDiff,
            "[EgDiff][dispatch] cycle=%llu tid=%u seq=%llu pc=%#lx "
            "ordinal=%llu\n",
            info.cycle, info.tid, info.seqNo, info.pc, ordinal);

    auto table_it = state.table.find(info.pc);
    if (table_it == state.table.end()) {
        egdiffStats.noEntry++;
        return;
    }
    const auto &entry = table_it->second;
    if (entry.fpc != MaxFpc) {
        egdiffStats.confidenceSuppressions++;
        return;
    }
    if (lastMispActive()) {
        egdiffStats.lastMispSuppressions++;
        return;
    }
    if (ordinal < entry.distance) {
        egdiffStats.historyTooShort++;
        return;
    }

    target.predictionDistance = entry.distance;
    target.predictionDiff = entry.diff;
    target.baseOrdinal = ordinal - entry.distance;
    egdiffStats.predictionRequests++;
    const auto *base = findHistory(state, target.baseOrdinal);
    if (base && base->specValueValid) {
        target.requestReady = true;
        target.readyCycle = info.cycle + normalPredictionLatency;
        egdiffStats.normalRequests++;
        DPRINTF(EgDiff,
                "[EgDiff][request] tid=%u seq=%llu pc=%#lx ordinal=%llu "
                "baseOrdinal=%llu distance=%u diff=%#llx kind=normal "
                "readyCycle=%llu\n",
                info.tid, info.seqNo, info.pc, ordinal, target.baseOrdinal,
                target.predictionDistance,
                static_cast<unsigned long long>(target.predictionDiff),
                target.readyCycle);
    } else {
        target.requestPending = true;
        egdiffStats.deferredBindings++;
        DPRINTF(EgDiff,
                "[EgDiff][request] tid=%u seq=%llu pc=%#lx ordinal=%llu "
                "baseOrdinal=%llu distance=%u diff=%#llx kind=deferred\n",
                info.tid, info.seqNo, info.pc, ordinal, target.baseOrdinal,
                target.predictionDistance,
                static_cast<unsigned long long>(target.predictionDiff));
    }
}

VPPredictionCandidate
EgDiff::latePredict(const VPLatePredictRequest &request,
        VPPredictionRecord *record)
{
    assertValidTid(request.tid);
    VPPredictionCandidate candidate;
    auto *egdiff_record = getRecord(record);
    if (!egdiff_record->slotAllocated) {
        return candidate;
    }
    auto &state = states[request.tid];
    auto *target = findHistory(state, egdiff_record->loadOrdinal);
    if (!target || target->seqNo != request.seqNo || target->completed ||
        target->requestDelivered || !target->requestReady ||
        request.cycle < target->readyCycle) {
        return candidate;
    }
    if (lastMispActive()) {
        target->requestReady = false;
        target->requestDelivered = true;
        egdiffStats.lastMispSuppressions++;
        return candidate;
    }
    const auto *base = findHistory(state, target->baseOrdinal);
    if (!base || !base->specValueValid) {
        return candidate;
    }

    const RegVal predicted = base->specValue + target->predictionDiff;
    candidate.result = {true, predicted};
    candidate.score = MaxFpc;
    target->requestReady = false;
    target->requestDelivered = true;
    egdiff_record->offeredPrediction = true;
    egdiff_record->predictedValue = predicted;
    egdiffStats.predictionsOffered++;
    if (base->valueSource == ValueSource::Predicted) {
        egdiffStats.predictedBaseUses++;
    } else {
        egdiffStats.actualBaseUses++;
    }
    DPRINTF(EgDiff,
            "[EgDiff][offer] cycle=%llu tid=%u seq=%llu pc=%#lx "
            "ordinal=%llu baseSeq=%llu basePc=%#lx baseOrdinal=%llu "
            "base=%#llx baseSource=%s distance=%u diff=%#llx "
            "predicted=%#llx\n",
            request.cycle, request.tid, request.seqNo, request.pc,
            target->ordinal, base->seqNo, base->pc, base->ordinal,
            static_cast<unsigned long long>(base->specValue),
            valueSourceName(base->valueSource),
            target->predictionDistance,
            static_cast<unsigned long long>(target->predictionDiff),
            static_cast<unsigned long long>(predicted));
    return candidate;
}

void
EgDiff::predictionApplied(const VPPredictionAppliedInfo &info,
        VPPredictionRecord *record)
{
    assertValidTid(info.tid);
    auto *egdiff_record = getRecord(record);
    if (!egdiff_record->slotAllocated) {
        return;
    }
    if (egdiff_record->appliedValueRecorded) {
        return;
    }
    auto &state = states[info.tid];
    auto *slot = findHistory(state, egdiff_record->loadOrdinal);
    if (!slot || slot->seqNo != info.seqNo || slot->completed) {
        return;
    }
    egdiff_record->selected = info.producedByReceiver;
    egdiff_record->appliedValueRecorded = true;
    if (info.producedByReceiver) {
        egdiffStats.predictionsApplied++;
    }
    makeSpecValueAvailable(state, info.tid, *slot, info.value,
            ValueSource::Predicted, info.cycle);
    DPRINTF(EgDiff,
            "[EgDiff][spec-value] cycle=%llu tid=%u seq=%llu pc=%#lx "
            "ordinal=%llu value=%#llx producer=%s\n",
            info.cycle, info.tid, info.seqNo, info.pc, slot->ordinal,
            static_cast<unsigned long long>(info.value),
            info.producedByReceiver ? "egdiff" : "other");
}

void
EgDiff::valueAvailable(const VPValueAvailableInfo &info,
        VPPredictionRecord *record)
{
    assertValidTid(info.tid);
    auto *egdiff_record = getRecord(record);
    if (!egdiff_record->slotAllocated) {
        return;
    }
    auto &state = states[info.tid];
    auto *slot = findHistory(state, egdiff_record->loadOrdinal);
    if (!slot || slot->seqNo != info.seqNo) {
        egdiffStats.staleValueCallbacks++;
        return;
    }
    if (slot->actualValid) {
        return;
    }
    if (egdiff_record->selected) {
        if (egdiff_record->predictedValue == info.actualValue) {
            egdiffStats.appliedCorrect++;
        } else {
            egdiffStats.appliedIncorrect++;
        }
    }
    slot->actualValue = info.actualValue;
    slot->actualValid = true;
    slot->completed = true;
    if (slot->requestPending || slot->requestReady) {
        egdiffStats.targetCompletedDrops++;
    }
    slot->requestPending = false;
    slot->requestReady = false;
    makeSpecValueAvailable(state, info.tid, *slot, info.actualValue,
            ValueSource::Actual, info.cycle);
    egdiffStats.valueAvailableUpdates++;
    DPRINTF(EgDiff,
            "[EgDiff][actual] cycle=%llu tid=%u seq=%llu pc=%#lx "
            "ordinal=%llu value=%#llx\n",
            info.cycle, info.tid, info.seqNo, info.pc, slot->ordinal,
            static_cast<unsigned long long>(info.actualValue));
}

void
EgDiff::valueMispredicted(const VPMispredictionInfo &info,
        VPPredictionRecord *record)
{
    assertValidTid(info.tid);
    auto *egdiff_record = getRecord(record);
    if (!egdiff_record->selected) {
        return;
    }
    egdiff_record->skipFpcAdvanceAtCommit = true;
    lastMispActivations.push_back({info.tid, info.seqNo, lastMispWindow});
    egdiffStats.lastMispActivations++;
    DPRINTF(EgDiff,
            "[EgDiff][last-misp-start] cycle=%llu tid=%u seq=%llu pc=%#lx "
            "window=%llu\n",
            info.cycle, info.tid, info.seqNo, info.pc, lastMispWindow);
}

void
EgDiff::commitInstruction(const VPCommitInfo &info)
{
    assertValidTid(info.tid);
    for (auto &activation : lastMispActivations) {
        if (info.tid != activation.tid || info.seqNo > activation.seqNo) {
            activation.remaining--;
        }
    }
    const auto old_size = lastMispActivations.size();
    lastMispActivations.erase(
        std::remove_if(lastMispActivations.begin(),
            lastMispActivations.end(),
            [](const auto &activation) { return activation.remaining == 0; }),
        lastMispActivations.end());
    if (old_size != 0 && lastMispActivations.empty()) {
        DPRINTF(EgDiff,
                "[EgDiff][last-misp-end] tid=%u seq=%llu\n",
                info.tid, info.seqNo);
    }
}

void
EgDiff::update(const VPUpdateInfo &info,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    assertValidTid(info.tid);
    const auto *egdiff_record = getRecord(record);
    if (!egdiff_record->slotAllocated) {
        return;
    }
    auto &state = states[info.tid];
    auto *target = findHistory(state, egdiff_record->loadOrdinal);
    gem5_assert(target && target->seqNo == info.seqNo,
            "EgDiff commit does not match its global-history slot");
    target->actualValue = info.actualValue;
    target->actualValid = true;
    target->specValue = info.actualValue;
    target->specValueValid = true;
    target->valueSource = ValueSource::Actual;
    target->completed = true;
    target->committed = true;

    if (target->ordinal > 0) {
        const auto *previous = findHistory(state, target->ordinal - 1);
        gem5_assert(previous && previous->committed && previous->actualValid,
                "EgDiff distance-1 trace requires a committed actual base");
        const RegVal distance_one_diff =
            info.actualValue - previous->actualValue;
        DPRINTF(EgDiff,
                "[EgDiff][commit-value] tid=%u seq=%llu pc=%#lx "
                "ordinal=%llu actual=%#llx previousPc=%#lx "
                "distanceOneDiff=%#llx\n",
                info.tid, info.seqNo, info.pc, target->ordinal,
                static_cast<unsigned long long>(info.actualValue),
                previous->pc,
                static_cast<unsigned long long>(distance_one_diff));
    }

    auto table_it = state.table.find(info.pc);
    if (table_it == state.table.end()) {
        if (target->ordinal >= order) {
            const auto *base = findHistory(state, target->ordinal - order);
            if (base && base->committed && base->actualValid) {
                const RegVal diff = info.actualValue - base->actualValue;
                state.table.emplace(std::piecewise_construct,
                        std::forward_as_tuple(info.pc),
                        std::forward_as_tuple(order, diff,
                            initialRandomState(info.tid, info.pc)));
                updateTableEntryStats();
                DPRINTF(EgDiff,
                        "[EgDiff][allocate] tid=%u seq=%llu pc=%#lx "
                        "basePc=%#lx distance=%u diff=%#llx fpc=0\n",
                        info.tid, info.seqNo, info.pc, base->pc, order,
                        static_cast<unsigned long long>(diff));
            }
        }
    } else {
        auto &entry = table_it->second;
        if (target->ordinal >= entry.distance) {
            const auto *base = findHistory(
                    state, target->ordinal - entry.distance);
            if (base && base->committed && base->actualValid) {
                const RegVal actual_diff =
                    info.actualValue - base->actualValue;
                const uint8_t old_fpc = entry.fpc;
                if (actual_diff == entry.diff) {
                    egdiffStats.diffMatches++;
                    if (egdiff_record->skipFpcAdvanceAtCommit) {
                        entry.fpc = 0;
                    } else if (advanceFpc(entry)) {
                        egdiffStats.fpcAdvances++;
                    } else if (old_fpc != MaxFpc) {
                        egdiffStats.fpcHolds++;
                    }
                    DPRINTF(EgDiff,
                            "[EgDiff][train-match] tid=%u seq=%llu pc=%#lx "
                            "basePc=%#lx distance=%u diff=%#llx "
                            "fpc=%u->%u\n",
                            info.tid, info.seqNo, info.pc, base->pc,
                            entry.distance,
                            static_cast<unsigned long long>(entry.diff),
                            static_cast<unsigned>(old_fpc),
                            static_cast<unsigned>(entry.fpc));
                } else {
                    egdiffStats.diffMismatches++;
                    const unsigned old_distance = entry.distance;
                    const RegVal old_diff = entry.diff;
                    const unsigned new_distance = entry.distance > 1 ?
                        entry.distance - 1 : order;
                    gem5_assert(target->ordinal >= new_distance,
                            "EgDiff polling base must precede committed target");
                    const auto *new_base = findHistory(
                            state, target->ordinal - new_distance);
                    gem5_assert(new_base && new_base->committed &&
                                new_base->actualValid,
                            "EgDiff polling requires a committed actual base");
                    entry.fpc = 0;
                    entry.distance = new_distance;
                    entry.diff = info.actualValue - new_base->actualValue;
                    egdiffStats.pollingDistanceChanges++;
                    DPRINTF(EgDiff,
                            "[EgDiff][train-mismatch] tid=%u seq=%llu pc=%#lx "
                            "oldDistance=%u oldDiff=%#llx actualDiff=%#llx "
                            "newBasePc=%#lx newDistance=%u newDiff=%#llx "
                            "fpc=%u->0\n",
                            info.tid, info.seqNo, info.pc,
                            old_distance,
                            static_cast<unsigned long long>(old_diff),
                            static_cast<unsigned long long>(actual_diff),
                            new_base->pc, entry.distance,
                            static_cast<unsigned long long>(entry.diff),
                            static_cast<unsigned>(old_fpc));
                }
            }
        }
    }
    (void)feedback;
    pruneHistory(state, info.tid);
}

void
EgDiff::specUpdate(const VPSpecUpdateInfo &info)
{
    (void)info;
}

void
EgDiff::squash(ThreadID tid, const uint64_t seq_no)
{
    assertValidTid(tid);
    auto &state = states[tid];
    unsigned removed = 0;
    unsigned cancelled = 0;
    uint64_t rewind_ordinal = state.nextOrdinal;
    for (auto it = state.history.begin(); it != state.history.end();) {
        if (!it->second.committed && it->second.seqNo > seq_no) {
            rewind_ordinal = std::min(rewind_ordinal, it->first);
            cancelled += it->second.requestPending || it->second.requestReady;
            it = state.history.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    state.nextOrdinal = rewind_ordinal;
    lastMispActivations.erase(
        std::remove_if(lastMispActivations.begin(),
            lastMispActivations.end(),
            [tid, seq_no](const auto &activation) {
                return activation.tid == tid && activation.seqNo > seq_no;
            }),
        lastMispActivations.end());
    egdiffStats.squashedSlots += removed;
    egdiffStats.cancelledRequests += cancelled;
    updateHistoryOccupancyStats();
    DPRINTF(EgDiff,
            "[EgDiff][squash] tid=%u retainedSeq=%llu removed=%u "
            "cancelled=%u nextOrdinal=%llu\n",
            tid, seq_no, removed, cancelled, state.nextOrdinal);
}

} // namespace valuepred
} // namespace gem5
