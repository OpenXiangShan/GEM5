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
      maxConfidence(mask(confidenceBits)),
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
    fatal_if(params.thresholdPercent == 0 ||
                    params.thresholdPercent > 100,
            "ConstantLVP thresholdPercent must be in [1, 100]");

    tables.resize(numThreads);
    for (auto &threadTables : tables) {
        threadTables.reserve(numWays);
        for (unsigned way = 0; way < numWays; ++way) {
            threadTables.emplace_back(
                    numSets, Entry(confidenceBits, usefulBits));
        }
    }

    DPRINTF(ConstantLVP,
            "params: ways=%u sets=%u tagBits=%u confidenceBits=%u "
            "usefulBits=%u resetConfidence=%u confidenceThreshold=%u "
            "confidencePenalty=%u\n",
            numWays, numSets, tagBits, confidenceBits, usefulBits,
            resetConfidence, confidenceThreshold, confidencePenalty);
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
      ADD_STAT(mismatchInvalidations, statistics::units::Count::get(),
              "ConstantLVP value mismatches that invalidate the entry"),
      ADD_STAT(invalidAllocations, statistics::units::Count::get(),
              "ConstantLVP allocations into zero-confidence entries"),
      ADD_STAT(usefulReplacements, statistics::units::Count::get(),
              "ConstantLVP replacements of valid zero-useful entries"),
      ADD_STAT(confidenceBasedReplacements, statistics::units::Count::get(),
              "ConstantLVP replacements selected by minimum confidence "
              "among zero-useful candidates"),
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
ConstantLVP::findEntry(Addr pc, ThreadID tid, Location &location)
{
    for (unsigned way = 0; way < numWays; ++way) {
        const auto candidate = locationForWay(pc, way);
        auto &entry = tables[tid][way][candidate.index];
        if (static_cast<uint16_t>(entry.confidence) != 0 &&
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
    if (static_cast<uint16_t>(entry->confidence) < confidenceThreshold) {
        constantStats.lowConfidenceHits++;
        DPRINTF(ConstantLVP,
                "[predict] tid=%u seq=%llu pc=%#llx way=%u set=%u "
                "low confidence=%u threshold=%u value=%#llx\n",
                request.tid,
                static_cast<unsigned long long>(request.seqNo),
                static_cast<unsigned long long>(request.pc),
                location.way, location.index,
                static_cast<uint16_t>(entry->confidence),
                confidenceThreshold,
                static_cast<unsigned long long>(entry->value));
        return candidate;
    }

    candidate.result.speculative = true;
    candidate.record = std::make_unique<VPPredictionRecord>();
    candidate.record->offeredPrediction = true;
    candidate.record->predictedValue = entry->value;
    DPRINTF(ConstantLVP,
            "[predict] tid=%u seq=%llu pc=%#llx way=%u set=%u "
            "confidence=%u useful=%u value=%#llx\n",
            request.tid,
            static_cast<unsigned long long>(request.seqNo),
            static_cast<unsigned long long>(request.pc),
            location.way, location.index,
            static_cast<uint16_t>(entry->confidence),
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

    Location location;
    Entry *entry = findEntry(updateInfo.pc, updateInfo.tid, location);
    if (entry) {
        constantStats.updateHits++;
        if (entry->value == updateInfo.actualValue) {
            constantStats.valueMatches++;
            ++entry->confidence;
            ++entry->useful;
            if (static_cast<uint16_t>(entry->confidence) >=
                    confidenceThreshold) {
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
                "confidence=%u useful=%u value=%#llx\n",
                updateInfo.tid,
                static_cast<unsigned long long>(updateInfo.seqNo),
                static_cast<unsigned long long>(updateInfo.pc),
                location.way, location.index,
                static_cast<uint16_t>(entry->confidence),
                static_cast<uint16_t>(entry->useful),
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
    for (unsigned offset = 0; offset < numWays; ++offset) {
        const unsigned way = (firstWay + offset) % numWays;
        const auto candidate = locationForWay(updateInfo.pc, way);
        auto &candidateEntry =
            tables[updateInfo.tid][way][candidate.index];
        if (static_cast<uint16_t>(candidateEntry.useful) != 0) {
            continue;
        }

        const uint16_t candidateConfidence = candidateEntry.confidence;
        if (!victim || candidateConfidence < victimConfidence) {
            victim = &candidateEntry;
            victimLocation = candidate;
            victimConfidence = candidateConfidence;
        }
    }

    if (victim) {
        allocate(*victim, victimLocation.tag, updateInfo.actualValue);
        constantStats.usefulReplacements++;
        constantStats.confidenceBasedReplacements++;
        DPRINTF(ConstantLVP,
                "[update] tid=%u seq=%llu pc=%#llx replace unuseful "
                "way=%u set=%u oldConfidence=%u value=%#llx\n",
                updateInfo.tid,
                static_cast<unsigned long long>(updateInfo.seqNo),
                static_cast<unsigned long long>(updateInfo.pc),
                victimLocation.way, victimLocation.index, victimConfidence,
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
            "way=%u set=%u confidence=%u useful=%u\n",
            updateInfo.tid,
            static_cast<unsigned long long>(updateInfo.seqNo),
            static_cast<unsigned long long>(updateInfo.pc),
            firstWay, candidate.index,
            static_cast<uint16_t>(candidateEntry.confidence),
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
