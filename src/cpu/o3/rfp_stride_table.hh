/*
 * Copyright (c) 2026 Institute of Computing Technology, Chinese Academy of
 * Sciences
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution; neither the name of
 * the copyright holders nor the names of its contributors may be used to
 * endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_RFP_STRIDE_TABLE_HH__
#define __CPU_O3_RFP_STRIDE_TABLE_HH__

#include <cstdint>
#include <optional>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5
{
namespace o3
{

class RfpStrideTable
{
  public:
    enum class RejectReason
    {
        None,
        Miss,
        LowConfidence,
        ZeroStride,
        StrideRange,
        CrossPage,
        AddressOverflow
    };

    struct Prediction
    {
        Addr address = 0;
        uint32_t version = 0;
    };

    struct LookupResult
    {
        RejectReason reject = RejectReason::Miss;
        bool tableHit = false;
        std::optional<Prediction> prediction;
    };

    struct TrainResult
    {
        bool firstSample = false;
        bool strideMatch = false;
        bool strideChange = false;
        bool confidenceInc = false;
        bool confidenceDec = false;
        bool entryEvict = false;
    };

    RfpStrideTable(unsigned entries, unsigned associativity,
                   unsigned confidenceBits, unsigned confidenceThreshold,
                   uint64_t maxStrideBytes, bool requireSamePage);

    LookupResult lookup(Addr pc, uint64_t generation, Tick now);
    bool claimPrediction(Addr pc, uint64_t generation, uint32_t version);
    TrainResult train(Addr pc, Addr address, uint64_t generation,
                      InstSeqNum seq, Tick now);
    bool versionMatches(Addr pc, uint64_t generation,
                        uint32_t version) const;
    void reset();

  private:
    struct Entry
    {
        bool valid = false;
        Addr pcTag = 0;
        Addr lastCommittedVa = 0;
        int64_t stride = 0;
        uint8_t confidence = 0;
        uint32_t version = 1;
        uint64_t generation = 0;
        InstSeqNum lastTrainSeq = 0;
        InstSeqNum lastLaunchTrainSeq = 0;
        Tick lastUseTick = 0;
    };

    unsigned setIndex(Addr pc) const;
    Entry *find(Addr pc, uint64_t generation);
    const Entry *find(Addr pc, uint64_t generation) const;
    bool legalStride(int64_t stride) const;

    const unsigned numEntries;
    const unsigned associativity;
    const unsigned numSets;
    const uint8_t maxConfidence;
    const unsigned confidenceThreshold;
    const uint64_t maxStrideBytes;
    const bool requireSamePage;
    std::vector<Entry> table;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RFP_STRIDE_TABLE_HH__
