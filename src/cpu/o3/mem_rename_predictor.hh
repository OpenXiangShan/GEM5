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

#ifndef __CPU_O3_MEM_RENAME_PREDICTOR_HH__
#define __CPU_O3_MEM_RENAME_PREDICTOR_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"

namespace gem5
{

namespace o3
{

class DynInst;

/** Predicts the number of logical stores between a load and its producer. */
class MemRenamePredictor : public statistics::Group
{
  public:
    struct Prediction
    {
        bool confident = false;
        uint32_t distance = 0;
        unsigned set = 0;
        unsigned way = 0;
        unsigned confidence = 0;
    };

    MemRenamePredictor(statistics::Group *parent, bool enabled,
                       unsigned entries, unsigned ways, unsigned tag_bits,
                       unsigned confidence_bits, unsigned confidence_threshold,
                       unsigned confidence_rest_percent,
                       unsigned useful_bits, uint64_t virtual_sq_entries);

    bool enabled() const { return enabledFlag; }
    uint64_t virtualSQEntries() const { return virtualSQSize; }

    static bool supportsLoad(const DynInst &inst);
    static bool supportsStore(const DynInst &inst);

    Prediction lookup(Addr load_pc);
    void train(Addr load_pc, uint32_t actual_distance);
    bool suppressConfidence(Addr load_pc, uint32_t predicted_distance);

    void recordPredictionOffered() { predictionsOffered++; }
    void recordSpqLookupApplied(bool is_float);
    void recordSpqDistanceOutOfRange() { spqDistanceOutOfRange++; }
    void recordSpqPregClassMismatch() { spqPregClassMismatch++; }
    void recordSpqEviction() { spqEvictions++; }
    void recordValidationSuccess() { validationSuccess++; }
    void recordValidationFailNoSQForward() { validationFailNoSQForward++; }
    void recordValidationFailDistance() { validationFailDistance++; }
    void recordValidationFailPreg() { validationFailPreg++; }
    void recordValidationFailIneligiblePair()
    {
        validationFailIneligiblePair++;
    }
    void recordValidationFailFault() { validationFailFault++; }
    void recordRecoverySquash() { recoverySquashes++; }
    void updateSpqOccupancy(unsigned occupancy);

  private:
    struct Entry
    {
        bool valid = false;
        uint64_t tag = 0;
        uint32_t distance = 0;
        unsigned confidence = 0;
        unsigned useful = 0;
    };

    struct Location
    {
        unsigned set = 0;
        unsigned way = 0;
    };

    Location locate(Addr load_pc) const;
    Entry *find(Addr load_pc, Location &location);
    const Entry *find(Addr load_pc, Location &location) const;
    unsigned allocate(const Location &location) const;
    uint64_t tagFor(Addr load_pc) const;

    const bool enabledFlag;
    const unsigned ways;
    const uint64_t tagMask;
    const unsigned confidenceMax;
    const unsigned confidenceThreshold;
    const unsigned confidenceRestPercent;
    const unsigned usefulMax;
    const uint64_t virtualSQSize;
    unsigned maxSpqOccupancy = 0;
    std::vector<std::vector<Entry>> table;

  public:
    statistics::Scalar predictorLookups;
    statistics::Scalar predictorTagHits;
    statistics::Scalar predictorConfident;
    statistics::Scalar predictionsOffered;
    statistics::Formula predictionCoverage;
    statistics::Scalar spqLookupApplied;
    statistics::Scalar spqDistanceOutOfRange;
    statistics::Scalar spqPregClassMismatch;
    statistics::Scalar spqEvictions;
    statistics::Scalar intAliasesApplied;
    statistics::Scalar fpAliasesApplied;
    statistics::Scalar validationSuccess;
    statistics::Scalar validationFailNoSQForward;
    statistics::Scalar validationFailDistance;
    statistics::Scalar validationFailPreg;
    statistics::Scalar validationFailIneligiblePair;
    statistics::Scalar validationFailFault;
    statistics::Formula predictionAccuracy;
    statistics::Scalar failureConfidenceSuppressions;
    statistics::Scalar commitTrainAllocations;
    statistics::Scalar commitTrainMatches;
    statistics::Scalar commitTrainDistanceChanges;
    statistics::Scalar pregAllocationsSaved;
    statistics::Scalar spqOccupancy;
    statistics::Scalar spqMaxOccupancy;
    statistics::Scalar recoverySquashes;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MEM_RENAME_PREDICTOR_HH__
