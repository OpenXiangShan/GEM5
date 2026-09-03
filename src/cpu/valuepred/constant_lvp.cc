#include "cpu/valuepred/constant_lvp.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/random.hh"
#include "base/stats/units.hh"
#include "base/trace.hh"
#include "cpu/valuepred/constant_lvp_policy.hh"
#include "debug/ConstantLVP.hh"

namespace gem5
{

namespace valuepred
{

ConstantLVP::ConstantLVP(const Params &params)
    : VPUnit(params),
      numWays(params.numWays),
      numSets(params.numSets),
      setBits(numSets <= 1 ? 0 : floorLog2(numSets)),
      tagBits(params.tagBits),
      confidenceBits(params.confidenceBits),
      usefulBits(params.usefulBits),
      resetConfidence(params.resetConfidence),
      enableCriticality(params.enableCriticality),
      criticalCounterBits(params.criticalCounterBits),
      criticalBlockCycleFactor(params.criticalBlockCycleFactor),
      maxConfidence(mask(confidenceBits)),
      maxCritical(mask(criticalCounterBits)),
      confidenceThreshold(static_cast<uint16_t>(std::max<double>(1.0,
              std::ceil(params.thresholdPercent * maxConfidence / 100.0)))),
      confidencePenalty(params.confidencePenalty == 0 ?
              maxConfidence : params.confidencePenalty),
      constantStats(this)
{
    fatal_if(numWays == 0, "ConstantLVP numWays must be nonzero");
    fatal_if(numWays > 64, "ConstantLVP numWays cannot exceed 64");
    fatal_if(numSets == 0 || !isPowerOf2(numSets),
            "ConstantLVP numSets must be a nonzero power of two");
    fatal_if(tagBits == 0 || tagBits > 64,
            "ConstantLVP tagBits must be in [1, 64]");
    fatal_if(confidenceBits == 0 || confidenceBits > 16,
            "ConstantLVP confidenceBits must be in [1, 16]");
    fatal_if(params.confidencePenalty > maxConfidence,
            "ConstantLVP confidencePenalty must be zero or no greater than "
            "the confidence counter maximum");
    fatal_if(usefulBits == 0 || usefulBits > 16,
            "ConstantLVP usefulBits must be in [1, 16]");
    fatal_if(criticalCounterBits == 0 || criticalCounterBits > 16,
            "ConstantLVP criticalCounterBits must be in [1, 16]");
    fatal_if(criticalBlockCycleFactor == 0,
            "ConstantLVP criticalBlockCycleFactor must be nonzero");
    fatal_if(params.thresholdPercent == 0 ||
                    params.thresholdPercent > 100,
            "ConstantLVP thresholdPercent must be in [1, 100]");

    tables.resize(numThreads);
    for (auto &threadTables : tables) {
        threadTables.reserve(numWays);
        for (unsigned way = 0; way < numWays; ++way) {
            threadTables.emplace_back(
                    numSets, Entry(confidenceBits, usefulBits,
                            criticalCounterBits));
        }
    }

    constantStats.criticalCounterValue
        .init(maxCritical + 1)
        .flags(statistics::total | statistics::pdf);
    for (unsigned value = 0; value <= maxCritical; ++value) {
        constantStats.criticalCounterValue.subname(
                value, std::to_string(value));
    }

    DPRINTF(ConstantLVP,
            "params: ways=%u sets=%u tagBits=%u confidenceBits=%u "
            "usefulBits=%u resetConfidence=%u enableCriticality=%u "
            "criticalCounterBits=%u criticalBlockCycleFactor=%llu "
            "confidenceThreshold=%u thresholdReductionStep=%u "
            "confidencePenalty=%u\n",
            numWays, numSets, tagBits, confidenceBits, usefulBits,
            resetConfidence, enableCriticality, criticalCounterBits,
            static_cast<unsigned long long>(criticalBlockCycleFactor),
            confidenceThreshold,
            static_cast<unsigned>(confidenceThreshold /
                (static_cast<uint64_t>(1) << criticalCounterBits)),
            confidencePenalty);
}

ConstantLVP::ConstantLVPStats::ConstantLVPStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(lookups, statistics::units::Count::get(),
              "ConstantLVP fetch-time table lookups"),
      ADD_STAT(lookupHits, statistics::units::Count::get(),
              "ConstantLVP fetch-time tag hits"),
      ADD_STAT(lookupMisses, statistics::units::Count::get(),
              "ConstantLVP fetch-time tag misses"),
      ADD_STAT(lowConfidenceHits, statistics::units::Count::get(),
              "ConstantLVP tag hits below the prediction threshold"),
      ADD_STAT(updates, statistics::units::Count::get(),
              "ConstantLVP committed updates"),
      ADD_STAT(updateHits, statistics::units::Count::get(),
              "ConstantLVP committed updates that hit an entry"),
      ADD_STAT(updateMisses, statistics::units::Count::get(),
              "ConstantLVP committed updates that miss all ways"),
      ADD_STAT(valueMatches, statistics::units::Count::get(),
              "ConstantLVP hit updates whose value remains constant"),
      ADD_STAT(valueMismatches, statistics::units::Count::get(),
              "ConstantLVP hit updates whose value changes"),
      ADD_STAT(criticalUpdates, statistics::units::Count::get(),
              "ConstantLVP resident-entry criticality updates"),
      ADD_STAT(criticalIncreaseUpdates, statistics::units::Count::get(),
              "ConstantLVP criticality updates that increase the counter"),
      ADD_STAT(criticalDecreaseUpdates, statistics::units::Count::get(),
              "ConstantLVP criticality updates that decrease the counter"),
      ADD_STAT(robHeadBlockedCycles, statistics::units::Cycle::get(),
              "ROB-head blocked cycles observed by resident ConstantLVP "
              "entries"),
      ADD_STAT(criticalityEnabledPredictions, statistics::units::Count::get(),
              "ConstantLVP predictions admitted only by criticality"),
      ADD_STAT(criticalOnlyUpdateHits, statistics::units::Count::get(),
              "ConstantLVP updates that recover a zero-confidence entry "
              "retained by criticality"),
      ADD_STAT(criticalCounterValue, statistics::units::Count::get(),
              "Critical counter value after each resident-entry update"),
      ADD_STAT(mismatchInvalidations, statistics::units::Count::get(),
              "ConstantLVP value mismatches that invalidate the entry"),
      ADD_STAT(invalidAllocations, statistics::units::Count::get(),
              "ConstantLVP allocations into zero-confidence entries"),
      ADD_STAT(usefulReplacements, statistics::units::Count::get(),
              "ConstantLVP replacements of valid zero-useful entries"),
      ADD_STAT(confidenceBasedReplacements, statistics::units::Count::get(),
              "ConstantLVP replacements selected by minimum criticality-"
              "confidence score among zero-useful candidates"),
      ADD_STAT(allocationFailures, statistics::units::Count::get(),
              "ConstantLVP misses with no immediately replaceable entry"),
      ADD_STAT(usefulDecrements, statistics::units::Count::get(),
              "ConstantLVP useful counter decrements after allocation failure")
{
}

unsigned
ConstantLVP::pcHashToWayIndex(Addr pc, unsigned way) const
{
    uint64_t hash = pc;
    for (unsigned k = 1; k <= numWays; ++k) {
        int64_t shift =
            (static_cast<int64_t>(k) * setBits - way) % 64;
        if (shift < 0) {
            shift += 64;
        }
        hash ^= pc >> shift;
    }
    return hash & (numSets - 1);
}

uint64_t
ConstantLVP::pcHashToTag(Addr pc, unsigned way) const
{
    const int64_t wayOffset =
        static_cast<int64_t>(numWays) - static_cast<int64_t>(way);
    uint64_t hash = pc;
    for (unsigned k = 1; k <= numWays + 1; ++k) {
        int64_t shift =
            (static_cast<int64_t>(k) * setBits - wayOffset) % 64;
        if (shift < 0) {
            shift += 64;
        }
        hash ^= pc >> shift;
    }
    return hash & mask(tagBits);
}

ConstantLVP::Location
ConstantLVP::locationForWay(Addr pc, unsigned way) const
{
    pc = pc >> 1;
    return {
        .way = way,
        .index = pcHashToWayIndex(pc, way),
        .tag = pcHashToTag(pc, way),
    };
}

ConstantLVP::Entry *
ConstantLVP::findEntry(Addr pc, ThreadID tid, Location &location,
        bool include_critical_only)
{
    for (unsigned way = 0; way < numWays; ++way) {
        const auto candidate = locationForWay(pc, way);
        auto &entry = tables[tid][way][candidate.index];
        const bool resident = static_cast<uint16_t>(entry.confidence) != 0 ||
            (include_critical_only && enableCriticality &&
             static_cast<uint16_t>(entry.critical) != 0);
        if (resident &&
                entry.tag == candidate.tag) {
            location = candidate;
            return &entry;
        }
    }
    return nullptr;
}

void
ConstantLVP::allocate(Entry &entry, uint64_t tag, RegVal value)
{
    entry.tag = tag;
    entry.value = value;
    entry.confidence.reset();
    ++entry.confidence;
    entry.useful.reset();
    entry.critical.reset();
}

uint16_t
ConstantLVP::effectiveConfidenceThreshold(const Entry &entry) const
{
    if (!enableCriticality) {
        return confidenceThreshold;
    }

    return constant_lvp::effectiveConfidenceThreshold(
            confidenceThreshold, static_cast<uint16_t>(entry.critical),
            criticalCounterBits);
}

void
ConstantLVP::updateCriticality(
        Entry &entry, const VPUpdateInfo &update_info)
{
    if (!enableCriticality) {
        return;
    }

    const auto *criticality =
        update_info.getExt<LoadCriticalityUpdateInfoExt>();
    if (!criticality) {
        return;
    }

    const uint16_t old_value = entry.critical;
    const uint16_t new_value = constant_lvp::updatedCriticalCounter(
            old_value, criticality->robHeadBlockedCycles,
            criticalBlockCycleFactor, maxCritical);
    if (new_value > old_value) {
        entry.critical += new_value - old_value;
        constantStats.criticalIncreaseUpdates++;
    } else if (new_value < old_value) {
        entry.critical -= old_value - new_value;
        constantStats.criticalDecreaseUpdates++;
    }

    constantStats.criticalUpdates++;
    constantStats.robHeadBlockedCycles +=
        criticality->robHeadBlockedCycles;
    constantStats.criticalCounterValue[new_value]++;
}

bool
ConstantLVP::tryDecUseful(Entry &entry)
{
    const uint16_t confidence = entry.confidence;
    const unsigned randomBits =
        2 + 2 * (confidence > maxConfidence / 8) +
        2 * (confidence >= maxConfidence / 4);
    if ((random_mt.random<uint32_t>() & mask(randomBits)) != 0) {
        return false;
    }

    --entry.useful;
    return true;
}

VPPredictionCandidate
ConstantLVP::predict(const VPPredictRequest &request)
{
    assertValidTid(request.tid);
    constantStats.lookups++;

    Location location;
    const Entry *entry = findEntry(request.pc, request.tid, location);
    if (!entry) {
        constantStats.lookupMisses++;
        DPRINTF(ConstantLVP,
                "[predict] tid=%u seq=%llu pc=%#llx miss\n",
                request.tid,
                static_cast<unsigned long long>(request.seqNo),
                static_cast<unsigned long long>(request.pc));
        return {};
    }

    constantStats.lookupHits++;
    VPPredictionCandidate candidate;
    candidate.result.value = entry->value;
    const uint16_t effective_threshold =
        effectiveConfidenceThreshold(*entry);
    if (static_cast<uint16_t>(entry->confidence) < effective_threshold) {
        constantStats.lowConfidenceHits++;
        DPRINTF(ConstantLVP,
                "[predict] tid=%u seq=%llu pc=%#llx way=%u set=%u "
                "low confidence=%u critical=%u threshold=%u value=%#llx\n",
                request.tid,
                static_cast<unsigned long long>(request.seqNo),
                static_cast<unsigned long long>(request.pc),
                location.way, location.index,
                static_cast<uint16_t>(entry->confidence),
                static_cast<uint16_t>(entry->critical),
                effective_threshold,
                static_cast<unsigned long long>(entry->value));
        return candidate;
    }

    if (effective_threshold < confidenceThreshold &&
            static_cast<uint16_t>(entry->confidence) <
                confidenceThreshold) {
        constantStats.criticalityEnabledPredictions++;
    }
    candidate.result.speculative = true;
    candidate.record = std::make_unique<VPPredictionRecord>();
    candidate.record->offeredPrediction = true;
    candidate.record->predictedValue = entry->value;
    DPRINTF(ConstantLVP,
            "[predict] tid=%u seq=%llu pc=%#llx way=%u set=%u "
            "confidence=%u critical=%u threshold=%u useful=%u value=%#llx\n",
            request.tid,
            static_cast<unsigned long long>(request.seqNo),
            static_cast<unsigned long long>(request.pc),
            location.way, location.index,
            static_cast<uint16_t>(entry->confidence),
            static_cast<uint16_t>(entry->critical), effective_threshold,
            static_cast<uint16_t>(entry->useful),
            static_cast<unsigned long long>(entry->value));
    return candidate;
}

void
ConstantLVP::update(const VPUpdateInfo &updateInfo,
        const VPPredictionRecord *record, const VPFeedback &feedback)
{
    (void)record;
    (void)feedback;
    assertValidTid(updateInfo.tid);
    constantStats.updates++;
    const auto *criticality =
        updateInfo.getExt<LoadCriticalityUpdateInfoExt>();

    Location location;
    Entry *entry = findEntry(
            updateInfo.pc, updateInfo.tid, location, true);
    if (entry) {
        constantStats.updateHits++;
        if (static_cast<uint16_t>(entry->confidence) == 0) {
            constantStats.criticalOnlyUpdateHits++;
        }
        updateCriticality(*entry, updateInfo);
        if (entry->value == updateInfo.actualValue) {
            constantStats.valueMatches++;
            ++entry->confidence;
            ++entry->useful;
            if (static_cast<uint16_t>(entry->confidence) >=
                    effectiveConfidenceThreshold(*entry)) {
                entry->useful.saturate();
            }
        } else {
            constantStats.valueMismatches++;
            if (resetConfidence) {
                entry->confidence.reset();
            } else {
                entry->confidence -= confidencePenalty;
            }
            if (static_cast<uint16_t>(entry->confidence) == 0) {
                entry->useful.reset();
                constantStats.mismatchInvalidations++;
            }
            entry->value = updateInfo.actualValue;
        }

        DPRINTF(ConstantLVP,
                "[update] tid=%u seq=%llu pc=%#llx hit way=%u set=%u "
                "confidence=%u critical=%u threshold=%u useful=%u "
                "blockedCycles=%llu value=%#llx\n",
                updateInfo.tid,
                static_cast<unsigned long long>(updateInfo.seqNo),
                static_cast<unsigned long long>(updateInfo.pc),
                location.way, location.index,
                static_cast<uint16_t>(entry->confidence),
                static_cast<uint16_t>(entry->critical),
                effectiveConfidenceThreshold(*entry),
                static_cast<uint16_t>(entry->useful),
                static_cast<unsigned long long>(
                    criticality ? criticality->robHeadBlockedCycles : 0),
                static_cast<unsigned long long>(entry->value));
        return;
    }

    constantStats.updateMisses++;
    const unsigned firstWay = random_mt.random<unsigned>(0, numWays - 1);

    for (unsigned offset = 0; offset < numWays; ++offset) {
        const unsigned way = (firstWay + offset) % numWays;
        const auto candidate = locationForWay(updateInfo.pc, way);
        auto &candidateEntry =
            tables[updateInfo.tid][way][candidate.index];
        if (static_cast<uint16_t>(candidateEntry.confidence) == 0) {
            allocate(candidateEntry, candidate.tag, updateInfo.actualValue);
            constantStats.invalidAllocations++;
            DPRINTF(ConstantLVP,
                    "[update] tid=%u seq=%llu pc=%#llx allocate invalid "
                    "way=%u set=%u value=%#llx\n",
                    updateInfo.tid,
                    static_cast<unsigned long long>(updateInfo.seqNo),
                    static_cast<unsigned long long>(updateInfo.pc),
                    way, candidate.index,
                    static_cast<unsigned long long>(updateInfo.actualValue));
            return;
        }
    }

    Entry *victim = nullptr;
    Location victimLocation;
    uint16_t victimConfidence = 0;
    uint16_t victimCritical = 0;
    uint64_t victimScore = 0;
    for (unsigned offset = 0; offset < numWays; ++offset) {
        const unsigned way = (firstWay + offset) % numWays;
        const auto candidate = locationForWay(updateInfo.pc, way);
        auto &candidateEntry =
            tables[updateInfo.tid][way][candidate.index];
        if (static_cast<uint16_t>(candidateEntry.useful) != 0) {
            continue;
        }

        const uint16_t candidateConfidence = candidateEntry.confidence;
        const uint16_t candidateCritical = candidateEntry.critical;
        const uint64_t candidateScore = enableCriticality ?
            constant_lvp::replacementScore(
                candidateConfidence, candidateCritical) :
            candidateConfidence;
        if (!victim || candidateScore < victimScore ||
                (candidateScore == victimScore &&
                 candidateConfidence < victimConfidence)) {
            victim = &candidateEntry;
            victimLocation = candidate;
            victimConfidence = candidateConfidence;
            victimCritical = candidateCritical;
            victimScore = candidateScore;
        }
    }

    if (victim) {
        allocate(*victim, victimLocation.tag, updateInfo.actualValue);
        constantStats.usefulReplacements++;
        constantStats.confidenceBasedReplacements++;
        DPRINTF(ConstantLVP,
                "[update] tid=%u seq=%llu pc=%#llx replace unuseful "
                "way=%u set=%u oldConfidence=%u oldCritical=%u "
                "replacementScore=%llu value=%#llx\n",
                updateInfo.tid,
                static_cast<unsigned long long>(updateInfo.seqNo),
                static_cast<unsigned long long>(updateInfo.pc),
                victimLocation.way, victimLocation.index, victimConfidence,
                victimCritical,
                static_cast<unsigned long long>(victimScore),
                static_cast<unsigned long long>(updateInfo.actualValue));
        return;
    }

    constantStats.allocationFailures++;
    const auto candidate = locationForWay(updateInfo.pc, firstWay);
    auto &candidateEntry =
        tables[updateInfo.tid][firstWay][candidate.index];
    if (tryDecUseful(candidateEntry)) {
        constantStats.usefulDecrements++;
    }
    DPRINTF(ConstantLVP,
            "[update] tid=%u seq=%llu pc=%#llx allocation blocked "
            "way=%u set=%u confidence=%u critical=%u useful=%u\n",
            updateInfo.tid,
            static_cast<unsigned long long>(updateInfo.seqNo),
            static_cast<unsigned long long>(updateInfo.pc),
            firstWay, candidate.index,
            static_cast<uint16_t>(candidateEntry.confidence),
            static_cast<uint16_t>(candidateEntry.critical),
            static_cast<uint16_t>(candidateEntry.useful));
}

void
ConstantLVP::specUpdate(const VPSpecUpdateInfo &specUpdateInfo)
{
    (void)specUpdateInfo;
}

void
ConstantLVP::squash(ThreadID tid, const uint64_t seq_no)
{
    (void)seq_no;
    assertValidTid(tid);
}

} // namespace valuepred

} // namespace gem5
