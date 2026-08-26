/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences
 * All rights reserved.
 *
 * The license is the same as that in rfp_stride_table.hh.
 */

#include "cpu/o3/rfp_stride_table.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"

namespace gem5
{
namespace o3
{

namespace
{

constexpr Addr PageBytes = 4096;

} // anonymous namespace

RfpStrideTable::RfpStrideTable(
    unsigned entries, unsigned assoc, unsigned confidence_bits,
    unsigned confidence_threshold, uint64_t max_stride_bytes,
    bool require_same_page)
    : numEntries(entries), associativity(assoc),
      numSets(assoc ? entries / assoc : 0),
      maxConfidence(confidence_bits < 8 ?
          static_cast<uint8_t>((1U << confidence_bits) - 1) :
          std::numeric_limits<uint8_t>::max()),
      confidenceThreshold(confidence_threshold),
      maxStrideBytes(max_stride_bytes),
      requireSamePage(require_same_page), table(entries)
{
    panic_if(numEntries == 0, "RFP table must contain at least one entry");
    panic_if(associativity == 0 || numEntries % associativity != 0,
             "RFP table entries (%u) must be divisible by associativity "
             "(%u)", numEntries, associativity);
    panic_if(confidence_bits == 0 || confidence_bits > 8,
             "RFP confidence bits must be in [1, 8], got %u",
             confidence_bits);
    panic_if(confidenceThreshold > maxConfidence,
             "RFP confidence threshold %u exceeds maximum %u",
             confidenceThreshold, maxConfidence);
}

unsigned
RfpStrideTable::setIndex(Addr pc) const
{
    return (pc >> 1) % numSets;
}

RfpStrideTable::Entry *
RfpStrideTable::find(Addr pc, uint64_t generation)
{
    const unsigned base = setIndex(pc) * associativity;
    for (unsigned way = 0; way < associativity; ++way) {
        auto &entry = table[base + way];
        if (entry.valid && entry.pcTag == pc &&
            entry.generation == generation) {
            return &entry;
        }
    }
    return nullptr;
}

const RfpStrideTable::Entry *
RfpStrideTable::find(Addr pc, uint64_t generation) const
{
    const unsigned base = setIndex(pc) * associativity;
    for (unsigned way = 0; way < associativity; ++way) {
        const auto &entry = table[base + way];
        if (entry.valid && entry.pcTag == pc &&
            entry.generation == generation) {
            return &entry;
        }
    }
    return nullptr;
}

bool
RfpStrideTable::legalStride(int64_t stride) const
{
    if (stride == 0) {
        return false;
    }
    const uint64_t magnitude = stride < 0 ?
        static_cast<uint64_t>(-(stride + 1)) + 1 :
        static_cast<uint64_t>(stride);
    return magnitude <= maxStrideBytes;
}

RfpStrideTable::LookupResult
RfpStrideTable::lookup(Addr pc, uint64_t generation, Tick now)
{
    LookupResult result;
    auto *entry = find(pc, generation);
    if (!entry) {
        return result;
    }

    result.tableHit = true;
    entry->lastUseTick = now;
    if (entry->confidence < confidenceThreshold) {
        result.reject = RejectReason::LowConfidence;
        return result;
    }
    if (entry->stride == 0) {
        result.reject = RejectReason::ZeroStride;
        return result;
    }
    if (!legalStride(entry->stride)) {
        result.reject = RejectReason::StrideRange;
        return result;
    }

    Addr predicted = 0;
    if (entry->stride > 0) {
        const auto delta = static_cast<Addr>(entry->stride);
        if (entry->lastCommittedVa >
            std::numeric_limits<Addr>::max() - delta) {
            result.reject = RejectReason::AddressOverflow;
            return result;
        }
        predicted = entry->lastCommittedVa + delta;
    } else {
        const auto delta = static_cast<Addr>(-(entry->stride + 1)) + 1;
        if (entry->lastCommittedVa < delta) {
            result.reject = RejectReason::AddressOverflow;
            return result;
        }
        predicted = entry->lastCommittedVa - delta;
    }

    if (requireSamePage &&
        entry->lastCommittedVa / PageBytes != predicted / PageBytes) {
        result.reject = RejectReason::CrossPage;
        return result;
    }

    result.reject = RejectReason::None;
    result.prediction = Prediction{predicted, entry->version};
    return result;
}

bool
RfpStrideTable::claimPrediction(
    Addr pc, uint64_t generation, uint32_t version)
{
    auto *entry = find(pc, generation);
    if (!entry || entry->version != version ||
        entry->lastLaunchTrainSeq == entry->lastTrainSeq) {
        return false;
    }

    entry->lastLaunchTrainSeq = entry->lastTrainSeq;
    return true;
}

RfpStrideTable::TrainResult
RfpStrideTable::train(Addr pc, Addr address, uint64_t generation,
                      InstSeqNum seq, Tick now)
{
    TrainResult result;
    Entry *entry = find(pc, generation);
    if (!entry) {
        const unsigned base = setIndex(pc) * associativity;
        entry = &table[base];
        for (unsigned way = 0; way < associativity; ++way) {
            auto &candidate = table[base + way];
            if (!candidate.valid) {
                entry = &candidate;
                break;
            }
            if (candidate.lastUseTick < entry->lastUseTick) {
                entry = &candidate;
            }
        }

        const bool evicted = entry->valid;
        const uint32_t next_version = entry->version + 1;
        *entry = Entry{};
        entry->valid = true;
        entry->pcTag = pc;
        entry->lastCommittedVa = address;
        entry->version = next_version ? next_version : 1;
        entry->generation = generation;
        entry->lastTrainSeq = seq;
        entry->lastUseTick = now;
        result.firstSample = true;
        result.entryEvict = evicted;
        return result;
    }

    const __int128 delta = static_cast<__int128>(address) -
                           static_cast<__int128>(entry->lastCommittedVa);
    const bool representable =
        delta >= std::numeric_limits<int64_t>::min() &&
        delta <= std::numeric_limits<int64_t>::max();
    const int64_t observed = representable ? static_cast<int64_t>(delta) : 0;
    const bool observed_legal = representable && legalStride(observed);

    if (observed_legal && entry->stride == observed) {
        result.strideMatch = true;
        if (entry->confidence < maxConfidence) {
            ++entry->confidence;
            result.confidenceInc = true;
        }
    } else {
        if (entry->confidence > 0) {
            --entry->confidence;
            result.confidenceDec = true;
        }
        if (entry->stride == 0 || entry->confidence == 0) {
            if (entry->stride != observed || !observed_legal) {
                ++entry->version;
                if (entry->version == 0) {
                    entry->version = 1;
                }
                result.strideChange = true;
            }
            entry->stride = observed_legal ? observed : 0;
        }
    }

    entry->lastCommittedVa = address;
    entry->lastTrainSeq = seq;
    entry->lastUseTick = now;
    return result;
}

bool
RfpStrideTable::versionMatches(Addr pc, uint64_t generation,
                               uint32_t version) const
{
    const auto *entry = find(pc, generation);
    return entry && entry->version == version;
}

void
RfpStrideTable::reset()
{
    std::fill(table.begin(), table.end(), Entry{});
}

} // namespace o3
} // namespace gem5
