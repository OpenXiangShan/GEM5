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

uint64_t
RfpStreamTracker::onRename(
    Addr pc, uint64_t generation, InstSeqNum seq)
{
    panic_if(!occurrences.empty() && seq <= occurrences.back().seq,
             "RFP occurrences must be registered in rename order: "
             "new seq=%llu, back seq=%llu",
             seq, occurrences.back().seq);

    auto &count = perGenerationOutstanding[generation][pc];
    panic_if(count == std::numeric_limits<uint64_t>::max(),
             "RFP same-PC occurrence count overflow");
    ++count;
    occurrences.push_back(Occurrence{pc, generation, seq});
    return count;
}

void
RfpStreamTracker::release(Addr pc, uint64_t generation)
{
    auto generation_it = perGenerationOutstanding.find(generation);
    panic_if(generation_it == perGenerationOutstanding.end(),
             "RFP occurrence generation %llu is not tracked", generation);
    auto &per_pc = generation_it->second;
    auto pc_it = per_pc.find(pc);
    panic_if(pc_it == per_pc.end() || pc_it->second == 0,
             "RFP occurrence pc %#lx generation %llu is not tracked",
             pc, generation);

    if (--pc_it->second == 0) {
        per_pc.erase(pc_it);
    }
    if (per_pc.empty()) {
        perGenerationOutstanding.erase(generation_it);
    }
}

void
RfpStreamTracker::onCommit(
    Addr pc, uint64_t generation, InstSeqNum seq)
{
    panic_if(occurrences.empty(),
             "RFP committed occurrence seq=%llu is not tracked", seq);
    const auto &front = occurrences.front();
    panic_if(front.pc != pc || front.generation != generation ||
                 front.seq != seq,
             "RFP occurrences must retire in order: got "
             "pc=%#lx generation=%llu seq=%llu, expected "
             "pc=%#lx generation=%llu seq=%llu",
             pc, generation, seq, front.pc, front.generation, front.seq);
    release(front.pc, front.generation);
    occurrences.pop_front();
}

size_t
RfpStreamTracker::squash(InstSeqNum last_valid_seq)
{
    size_t removed = 0;
    while (!occurrences.empty() && occurrences.back().seq > last_valid_seq) {
        const auto occurrence = occurrences.back();
        release(occurrence.pc, occurrence.generation);
        occurrences.pop_back();
        ++removed;
    }
    return removed;
}

void
RfpStreamTracker::reset()
{
    occurrences.clear();
    perGenerationOutstanding.clear();
}

uint64_t
RfpStreamTracker::outstanding(Addr pc, uint64_t generation) const
{
    const auto generation_it = perGenerationOutstanding.find(generation);
    if (generation_it == perGenerationOutstanding.end()) {
        return 0;
    }
    const auto pc_it = generation_it->second.find(pc);
    return pc_it == generation_it->second.end() ? 0 : pc_it->second;
}

void
RfpStreamTracker::checkInvariants() const
{
    std::unordered_map<uint64_t,
        std::unordered_map<Addr, uint64_t>> reconstructed;
    InstSeqNum previous_seq = 0;
    bool first = true;
    for (const auto &occurrence : occurrences) {
        panic_if(!first && occurrence.seq <= previous_seq,
                 "RFP occurrence sequence is not strictly increasing");
        first = false;
        previous_seq = occurrence.seq;
        ++reconstructed[occurrence.generation][occurrence.pc];
    }
    panic_if(reconstructed != perGenerationOutstanding,
             "RFP occurrence counts do not match rename-order ledger");
}

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
RfpStrideTable::lookup(
    Addr pc, uint64_t generation, uint64_t lookahead, Tick now)
{
    LookupResult result;
    panic_if(lookahead == 0, "RFP prediction lookahead must be non-zero");
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

    const __int128 predicted_wide =
        static_cast<__int128>(entry->lastCommittedVa) +
        static_cast<__int128>(entry->stride) *
        static_cast<__int128>(lookahead);
    if (predicted_wide < 0 ||
        predicted_wide > std::numeric_limits<Addr>::max()) {
        result.reject = RejectReason::AddressOverflow;
        return result;
    }
    const Addr predicted = static_cast<Addr>(predicted_wide);

    if (requireSamePage &&
        entry->lastCommittedVa / PageBytes != predicted / PageBytes) {
        result.reject = RejectReason::CrossPage;
        return result;
    }

    result.reject = RejectReason::None;
    result.prediction = Prediction{predicted, entry->version, lookahead};
    return result;
}

uint64_t
RfpStrideTable::allocateVersion()
{
    const uint64_t version = nextVersion++;
    panic_if(version == 0 || nextVersion == 0,
             "RFP predictor version space exhausted");
    return version;
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
        *entry = Entry{};
        entry->valid = true;
        entry->pcTag = pc;
        entry->lastCommittedVa = address;
        entry->version = allocateVersion();
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

    result.strideMismatch = observed_legal && entry->stride != observed;
    result.illegalStride = !observed_legal;

    if (observed_legal && entry->stride == observed) {
        result.strideMatch = true;
        if (entry->confidence < maxConfidence) {
            ++entry->confidence;
            result.confidenceInc = true;
        }
    } else {
        if (entry->confidence > 0) {
            const uint8_t old_confidence = entry->confidence;
            entry->confidence >>= 1;
            result.confidenceDec = entry->confidence != old_confidence;
        }
        if (entry->stride == 0 || entry->confidence == 0) {
            const int64_t next_stride = observed_legal ? observed : 0;
            if (entry->stride != next_stride) {
                entry->version = allocateVersion();
                result.strideChange = true;
            }
            entry->stride = next_stride;
        }
    }

    entry->lastCommittedVa = address;
    entry->lastTrainSeq = seq;
    entry->lastUseTick = now;
    return result;
}

bool
RfpStrideTable::versionMatches(Addr pc, uint64_t generation,
                               uint64_t version) const
{
    const auto *entry = find(pc, generation);
    return entry && entry->version == version;
}

void
RfpStrideTable::reset()
{
    std::fill(table.begin(), table.end(), Entry{});
    nextVersion = 1;
}

} // namespace o3
} // namespace gem5
