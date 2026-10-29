/*
 * Copyright (c) 2026 The Regents of The University of California
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

#include "cpu/o3/mem_rename_predictor.hh"

#include <algorithm>
#include <limits>

#include "base/logging.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/op_class.hh"
#include "cpu/reg_class.hh"
#include "debug/MemRename.hh"

namespace gem5
{

namespace o3
{

namespace
{

uint64_t
maskForBits(unsigned bits)
{
    if (bits >= 64) {
        return std::numeric_limits<uint64_t>::max();
    }
    return bits == 0 ? 0 : (uint64_t(1) << bits) - 1;
}

unsigned
maxCounterValue(unsigned bits)
{
    if (bits >= 32) {
        return std::numeric_limits<unsigned>::max();
    }
    return bits == 0 ? 0 : (1u << bits) - 1;
}

uint64_t
hashLoadPC(Addr load_pc)
{
    // Remove only the invariant halfword-alignment bit. RISC-V compressed
    // instructions may start at PC[1] == 1, so shifting by two would merge
    // adjacent load PCs and make different store distances share an entry.
    uint64_t hash = load_pc >> 1;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdULL;
    hash ^= hash >> 33;
    hash *= 0xc4ceb9fe1a85ec53ULL;
    hash ^= hash >> 33;
    return hash;
}

} // anonymous namespace

MemRenamePredictor::MemRenamePredictor(
    statistics::Group *parent, bool enabled, unsigned entries,
    unsigned ways, unsigned tag_bits, unsigned confidence_bits,
    unsigned confidence_threshold, unsigned confidence_rest_percent,
    unsigned useful_bits, uint64_t virtual_sq_entries)
    : statistics::Group(parent, "memRename"),
      enabledFlag(enabled),
      ways(ways),
      tagMask(maskForBits(tag_bits)),
      confidenceMax(maxCounterValue(confidence_bits)),
      confidenceThreshold(confidence_threshold),
      confidenceRestPercent(confidence_rest_percent),
      usefulMax(maxCounterValue(useful_bits)),
      virtualSQSize(virtual_sq_entries),
      ADD_STAT(predictorLookups, statistics::units::Count::get(),
               "Decode lookups in the memory-renaming predictor"),
      ADD_STAT(predictorTagHits, statistics::units::Count::get(),
               "Memory-renaming predictor tag hits"),
      ADD_STAT(predictorConfident, statistics::units::Count::get(),
               "Memory-renaming hits above the confidence threshold"),
      ADD_STAT(predictionsOffered, statistics::units::Count::get(),
               "Confident store-distance predictions offered to Rename"),
      ADD_STAT(predictionCoverage, statistics::units::Ratio::get(),
               "Decode prediction coverage: offered predictions per supported "
               "load lookup",
               predictionsOffered / predictorLookups),
      ADD_STAT(spqLookupApplied, statistics::units::Count::get(),
               "Predictions that found a compatible store Preg in SPQ"),
      ADD_STAT(spqDistanceOutOfRange, statistics::units::Count::get(),
               "Predicted store positions unavailable in the SPQ window"),
      ADD_STAT(spqPregClassMismatch, statistics::units::Count::get(),
               "SPQ target Preg class or store-data mapping mismatches"),
      ADD_STAT(spqEvictions, statistics::units::Count::get(),
               "Oldest SPQ entries evicted when the bounded window is full"),
      ADD_STAT(intAliasesApplied, statistics::units::Count::get(),
               "Integer load RAT aliases applied"),
      ADD_STAT(fpAliasesApplied, statistics::units::Count::get(),
               "Floating-point load RAT aliases applied"),
      ADD_STAT(validationSuccess, statistics::units::Count::get(),
               "Memory-renamed loads validated against the SQ producer"),
      ADD_STAT(validationFailNoSQForward, statistics::units::Count::get(),
               "Memory-renamed loads not fully forwarded by the SQ"),
      ADD_STAT(validationFailDistance, statistics::units::Count::get(),
               "Memory-renamed loads forwarded by a different store distance"),
      ADD_STAT(validationFailPreg, statistics::units::Count::get(),
               "Memory-renamed loads forwarded by a different data Preg"),
      ADD_STAT(validationFailIneligiblePair, statistics::units::Count::get(),
               "Memory-renamed loads with unsupported forwarding pairs"),
      ADD_STAT(validationFailFault, statistics::units::Count::get(),
               "Faulting memory-renamed loads"),
      ADD_STAT(predictionAccuracy, statistics::units::Ratio::get(),
               "Accuracy among applied predictions that completed validation",
               validationSuccess /
                   (validationSuccess + validationFailNoSQForward +
                    validationFailDistance + validationFailPreg +
                    validationFailIneligiblePair + validationFailFault)),
      ADD_STAT(failureConfidenceSuppressions, statistics::units::Count::get(),
               "Provider confidence suppressions after failed validation"),
      ADD_STAT(commitTrainAllocations, statistics::units::Count::get(),
               "Predictor entries allocated by committed eligible loads"),
      ADD_STAT(commitTrainMatches, statistics::units::Count::get(),
               "Committed loads matching their learned store distance"),
      ADD_STAT(commitTrainDistanceChanges, statistics::units::Count::get(),
               "Predictor distance changes learned at commit"),
      ADD_STAT(pregAllocationsSaved, statistics::units::Count::get(),
               "Destination Preg allocations avoided by memory renaming"),
      ADD_STAT(spqOccupancy, statistics::units::Count::get(),
               "Current memory-renaming SPQ occupancy"),
      ADD_STAT(spqMaxOccupancy, statistics::units::Count::get(),
               "Maximum memory-renaming SPQ occupancy"),
      ADD_STAT(recoverySquashes, statistics::units::Count::get(),
               "Load-inclusive recovery squashes from validation failures")
{
    predictionCoverage.precision(6);
    predictionAccuracy.precision(6);

    fatal_if(ways == 0, "memRenameWays must be greater than zero");
    fatal_if(tag_bits == 0 || tag_bits > 64,
             "memRenameTagBits must be in [1, 64]");
    fatal_if(confidence_bits == 0 || confidence_bits > 16,
             "memRenameConfBits must be in [1, 16]");
    fatal_if(useful_bits == 0 || useful_bits > 8,
             "memRenameUsefulBits must be in [1, 8]");
    fatal_if(confidenceThreshold == 0 || confidenceThreshold > confidenceMax,
             "memRenameConfThreshold must be in [1, max confidence]");
    fatal_if(confidenceRestPercent > 100,
             "memRenameConfRestPercent must be in [0, 100]");
    fatal_if(virtualSQSize == 0 ||
             virtualSQSize > std::numeric_limits<uint32_t>::max(),
             "VirtualSQ capacity must fit the memory-renaming distance");
    fatal_if(enabledFlag && (entries < ways || entries % ways != 0),
             "Enabled memory-renaming table entries must be divisible by ways");

    if (enabledFlag) {
        table.resize(entries / ways, std::vector<Entry>(ways));
    }
}

bool
MemRenamePredictor::supportsLoad(const DynInst &inst)
{
    if (!inst.isLoad() || inst.isAtomic() || inst.isLoadReserved() ||
        inst.isVector() || inst.isInstPrefetch() || inst.isFusion() ||
        inst.isNonSpeculative() || inst.isSerializeBefore() ||
        inst.isSerializeAfter() || inst.operWid() != 64 ||
        (inst.opClass() != MemReadOp &&
         inst.opClass() != FloatMemReadOp) ||
        inst.numDestRegs() != 1) {
        return false;
    }

    const auto &dest = inst.destRegIdx(0);
    const auto dest_class = dest.classValue();
    return dest.isRenameable() &&
        (dest_class == IntRegClass || dest_class == FloatRegClass);
}

bool
MemRenamePredictor::supportsStore(const DynInst &inst)
{
    if (!inst.isStore() || inst.isAtomic() || inst.isStoreConditional() ||
        inst.isVector() || inst.isFusion() || !inst.isSplitStoreAddr() ||
        inst.operWid() != 64 ||
        (inst.opClass() != MemWriteOp &&
         inst.opClass() != FloatMemWriteOp) ||
        inst.numSrcRegs() < 2) {
        return false;
    }

    const auto data_class = inst.srcRegIdx(1).classValue();
    return data_class == IntRegClass || data_class == FloatRegClass;
}

MemRenamePredictor::Location
MemRenamePredictor::locate(Addr load_pc) const
{
    const uint64_t hash = hashLoadPC(load_pc);
    return {static_cast<unsigned>(hash % table.size()), 0};
}

uint64_t
MemRenamePredictor::tagFor(Addr load_pc) const
{
    return hashLoadPC(load_pc) & tagMask;
}

MemRenamePredictor::Entry *
MemRenamePredictor::find(Addr load_pc, Location &location)
{
    if (!enabledFlag) {
        return nullptr;
    }
    location = locate(load_pc);
    const uint64_t tag = tagFor(load_pc);
    for (unsigned way = 0; way < ways; ++way) {
        Entry &entry = table[location.set][way];
        if (entry.valid && entry.tag == tag) {
            location.way = way;
            return &entry;
        }
    }
    return nullptr;
}

const MemRenamePredictor::Entry *
MemRenamePredictor::find(Addr load_pc, Location &location) const
{
    if (!enabledFlag) {
        return nullptr;
    }
    location = locate(load_pc);
    const uint64_t tag = tagFor(load_pc);
    for (unsigned way = 0; way < ways; ++way) {
        const Entry &entry = table[location.set][way];
        if (entry.valid && entry.tag == tag) {
            location.way = way;
            return &entry;
        }
    }
    return nullptr;
}

MemRenamePredictor::Prediction
MemRenamePredictor::lookup(Addr load_pc)
{
    Prediction prediction;
    if (!enabledFlag) {
        return prediction;
    }

    ++predictorLookups;
    Location location;
    const Entry *entry = find(load_pc, location);
    if (!entry) {
        return prediction;
    }

    ++predictorTagHits;
    prediction.distance = entry->distance;
    prediction.set = location.set;
    prediction.way = location.way;
    prediction.confidence = entry->confidence;
    prediction.confident = entry->confidence >= confidenceThreshold;
    if (prediction.confident) {
        ++predictorConfident;
    }

    DPRINTF(MemRename,
            "lookup pc=%#lx set=%u way=%u distance=%u confidence=%u\n",
            load_pc, prediction.set, prediction.way, prediction.distance,
            prediction.confidence);
    return prediction;
}

unsigned
MemRenamePredictor::allocate(const Location &location) const
{
    for (unsigned way = 0; way < ways; ++way) {
        if (!table[location.set][way].valid) {
            return way;
        }
    }

    // Prefer entries with the least demonstrated usefulness and confidence.
    unsigned victim = 0;
    for (unsigned way = 1; way < ways; ++way) {
        const Entry &candidate = table[location.set][way];
        const Entry &current = table[location.set][victim];
        if (candidate.useful < current.useful ||
            (candidate.useful == current.useful &&
             candidate.confidence < current.confidence)) {
            victim = way;
        }
    }
    return victim;
}

void
MemRenamePredictor::train(Addr load_pc, uint32_t actual_distance)
{
    if (!enabledFlag) {
        return;
    }
    panic_if(actual_distance >= virtualSQSize,
             "Committed memory-renaming distance %u exceeds VirtualSQ capacity %llu",
             actual_distance,
             static_cast<unsigned long long>(virtualSQSize));

    Location location;
    Entry *entry = find(load_pc, location);
    if (!entry) {
        const unsigned way = allocate(location);
        Entry &replacement = table[location.set][way];
        replacement.valid = true;
        replacement.tag = tagFor(load_pc);
        replacement.distance = actual_distance;
        replacement.confidence = 1;
        replacement.useful = 0;
        ++commitTrainAllocations;
        DPRINTF(MemRename,
                "commit allocation pc=%#lx set=%u way=%u distance=%u\n",
                load_pc, location.set, way, actual_distance);
        return;
    }

    if (entry->distance == actual_distance) {
        entry->confidence = std::min(confidenceMax, entry->confidence + 1);
        entry->useful = std::min(usefulMax, entry->useful + 1);
        ++commitTrainMatches;
        return;
    }

    if (entry->confidence > 0) {
        --entry->confidence;
    }
    if (entry->useful > 0) {
        --entry->useful;
    }
    if (entry->confidence == 0 && entry->useful == 0) {
        entry->distance = actual_distance;
        entry->confidence = 1;
        ++commitTrainDistanceChanges;
    }
}

bool
MemRenamePredictor::suppressConfidence(Addr load_pc,
                                       uint32_t predicted_distance)
{
    if (!enabledFlag) {
        return false;
    }

    Location location;
    Entry *entry = find(load_pc, location);
    if (!entry || entry->distance != predicted_distance) {
        return false;
    }

    // Restore confidence relative to the activation threshold so the
    // re-enable delay scales consistently when the threshold is tuned. Even
    // 100% must suppress at least one level to make this feedback effective.
    const unsigned rest_target =
        confidenceThreshold * confidenceRestPercent / 100;
    const unsigned suppressed =
        std::min(rest_target, confidenceThreshold - 1);
    if (entry->confidence > suppressed) {
        const unsigned old_confidence = entry->confidence;
        entry->confidence = suppressed;
        ++failureConfidenceSuppressions;
        DPRINTF(MemRename,
                "suppress pc=%#lx set=%u way=%u distance=%u "
                "confidence=%u->%u rest=%u%%\n",
                load_pc, location.set, location.way, entry->distance,
                old_confidence, entry->confidence, confidenceRestPercent);
        return true;
    }
    return false;
}

void
MemRenamePredictor::recordSpqLookupApplied(bool is_float)
{
    ++spqLookupApplied;
    ++pregAllocationsSaved;
    if (is_float) {
        ++fpAliasesApplied;
    } else {
        ++intAliasesApplied;
    }
}

void
MemRenamePredictor::updateSpqOccupancy(unsigned occupancy)
{
    spqOccupancy = occupancy;
    maxSpqOccupancy = std::max(maxSpqOccupancy, occupancy);
    spqMaxOccupancy = maxSpqOccupancy;
}

} // namespace o3
} // namespace gem5
