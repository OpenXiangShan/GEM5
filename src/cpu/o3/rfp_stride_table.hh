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

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5
{
namespace o3
{

class RfpStreamTracker
{
  public:
    struct Occurrence
    {
        Addr pc = 0;
        uint64_t generation = 0;
        InstSeqNum seq = 0;
    };

    uint64_t onRename(Addr pc, uint64_t generation, InstSeqNum seq);
    void onCommit(Addr pc, uint64_t generation, InstSeqNum seq);
    size_t squash(InstSeqNum last_valid_seq);
    void reset();
    void checkInvariants() const;

    bool empty() const { return occurrences.empty(); }
    size_t size() const { return occurrences.size(); }
    uint64_t outstanding(Addr pc, uint64_t generation) const;

  private:
    void release(Addr pc, uint64_t generation);

    std::deque<Occurrence> occurrences;
    std::unordered_map<uint64_t,
        std::unordered_map<Addr, uint64_t>> perGenerationOutstanding;
};

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
        uint64_t version = 0;
        uint64_t lookahead = 0;
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
        bool strideMismatch = false;
        bool illegalStride = false;
        bool strideChange = false;
        bool confidenceInc = false;
        bool confidenceDec = false;
        bool entryEvict = false;
    };

    RfpStrideTable(unsigned entries, unsigned associativity,
                   unsigned confidenceBits, unsigned confidenceThreshold,
                   uint64_t maxStrideBytes, bool requireSamePage);

    LookupResult lookup(Addr pc, uint64_t generation, uint64_t lookahead,
                        Tick now);
    TrainResult train(Addr pc, Addr address, uint64_t generation,
                      InstSeqNum seq, Tick now);
    bool versionMatches(Addr pc, uint64_t generation,
                        uint64_t version) const;
    void reset();

  private:
    struct Entry
    {
        bool valid = false;
        Addr pcTag = 0;
        Addr lastCommittedVa = 0;
        int64_t stride = 0;
        uint8_t confidence = 0;
        uint64_t version = 0;
        uint64_t generation = 0;
        InstSeqNum lastTrainSeq = 0;
        Tick lastUseTick = 0;
    };

    unsigned setIndex(Addr pc) const;
    Entry *find(Addr pc, uint64_t generation);
    const Entry *find(Addr pc, uint64_t generation) const;
    bool legalStride(int64_t stride) const;
    uint64_t allocateVersion();

    const unsigned numEntries;
    const unsigned associativity;
    const unsigned numSets;
    const uint8_t maxConfidence;
    const unsigned confidenceThreshold;
    const uint64_t maxStrideBytes;
    const bool requireSamePage;
    uint64_t nextVersion = 1;
    std::vector<Entry> table;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_RFP_STRIDE_TABLE_HH__
