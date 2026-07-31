/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#ifndef __CPU_O3_UOP_CACHE_HH__
#define __CPU_O3_UOP_CACHE_HH__

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/statistics.hh"
#include "base/types.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/op_class.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

struct BaseO3CPUParams;

namespace o3
{

class CPU;
class DynInst;

struct UCInstDesc
{
    /** Immutable decode result reused by a later fetch of the same context. */
    StaticInstPtr inst;
    Addr pc = 0;
    bool compressed = false;
    Cycles enBufferTime = Cycles(0);

    int sizeBytes() const { return compressed ? 2 : 4; }
    std::string toString() const;
};

struct UopCacheContext
{
    /** Translation and privilege identity under which decoding occurred. */
    RegVal prv = 0;
    RegVal virt = 0;
    RegVal satp = 0;
    RegVal vsatp = 0;
    RegVal hgatp = 0;

    bool operator==(const UopCacheContext &other) const;
    bool operator!=(const UopCacheContext &other) const
    {
        return !(*this == other);
    }
    std::string toString() const;
};

struct UopEntry
{
    /** One self-contained decoded RISC-V instruction and its lookup tag. */
    Addr tag = 0;
    UopCacheContext context;
    UCInstDesc inst;
    bool valid = false;
    Cycles enUopCacheTime = Cycles(0);
    Addr firstInstPC = 0;
    int bytesSize = 0;

    std::string toString() const;
};

class UopCache
{
  public:
    /** Build an optionally enabled set-associative decoded-instruction cache. */
    UopCache(CPU *_cpu, const BaseO3CPUParams &params);

    bool enabled() const { return enableUopCache; }

    /** Stage a normally decoded instruction for refill at the next tick. */
    void addInst(const DynInstPtr &inst);
    /** Commit all staged refills; refill bandwidth is intentionally ideal. */
    void tick();
    /** Close the current single-instruction refill entry if it is valid. */
    void setCurrentUopEntryDone();
    /** Discard a partial refill without invalidating installed entries. */
    void flushCurUopEntry();

    /** Snapshot the privilege/translation identity used as part of the tag. */
    UopCacheContext getContext(ThreadID tid) const;
    /** Lookup one exact PC/context pair and update replacement state on hit. */
    std::pair<bool, int> checkUopCacheHit(
        Addr instAddr, const UopCacheContext &context);
    /** Require a contiguous instruction description for the entire block. */
    bool checkUopCacheBlockHit(Addr startAddr, Addr endAddr,
                               const UopCacheContext &context);
    /** Read from a selected way without transferring entry ownership. */
    const UCInstDesc *findInst(Addr instPC, int hitWay,
                               const UopCacheContext &context,
                               int *instIdx = nullptr);
    /** Lookup and read one instruction, optionally returning its way/index. */
    const UCInstDesc *findInst(Addr instPC,
                               const UopCacheContext &context,
                               int *hitWay = nullptr,
                               int *instIdx = nullptr);
    /** Invalidate every way containing the exact virtual instruction PC. */
    bool invalidateEntry(Addr fetchAddr);

    /**
     * Invalidate all installed and in-flight decoded instructions.
     *
     * The operation clears cache contents, staged refills, the current build
     * entry, and restores build mode.  UopCache owns no fetch-side pending
     * lookup, so a caller integrating fence.i must cancel that state too.
     */
    void invalidateAll();

    /** Invalidate installed and in-flight entries for one exact context. */
    void invalidateContext(const UopCacheContext &context);

    /** Accept refills from ordinary Decode traffic. */
    void switchToBuildMode();
    /** Suppress refills while Decode consumes cache-sourced traffic. */
    void switchToStreamMode();
    bool isBuildMode() const { return isBuildMode_; }
    bool isStreamMode() const { return !isBuildMode_; }

  private:
    using UopEntryPtr = std::unique_ptr<UopEntry>;
    using Way = std::vector<UopEntryPtr>;

    /** Map a virtual halfword address into a possibly non-power-of-two set. */
    Addr getSetIdxFromVaddr(Addr vaddr) const;
    /** Return the quotient/tag paired with getSetIdxFromVaddr(). */
    Addr getTagFromVaddr(Addr vaddr) const;
    /** Allocate a blank entry with all validity state cleared. */
    UopEntry *newEntry() const;
    /** Find the exact PC/context match by scanning the bounded way count. */
    int findWay(Addr instAddr, const UopCacheContext &context) const;
    /** Prefer an empty way, otherwise select the tree-PLRU victim. */
    int chooseReplacementWay(Addr setIdx) const;
    /** Decode the per-set PLRU tree into a victim way number. */
    int getPlruVictim(Addr setIdx) const;
    /** Update PLRU direction bits after either a hit or refill. */
    void updatePlruOnAccess(Addr setIdx, int way);

    CPU *cpu;

    struct UopCacheStats : public statistics::Group
    {
        UopCacheStats(CPU *cpu, unsigned max_inst_bytes_per_entry);
        void regStats() override;

        const unsigned maxInstBytesPerEntry;
        statistics::Scalar entryRefills;
        statistics::Scalar entryInstsTotal;
        statistics::Scalar entryBytesTotal;
        statistics::Scalar entryCapacityBytesTotal;
        statistics::Distribution entryInsts;
        statistics::Distribution entryBytes;
        statistics::Formula avgEntryInsts;
        statistics::Formula avgEntryBytes;
        statistics::Formula entryByteUtilization;
    } stats;

    /** Completed entries waiting for the next idealized refill tick. */
    std::deque<UopEntryPtr> refillEntryBuffer;
    /** Entry currently assembled by Decode; currently exactly one inst. */
    UopEntryPtr currentRefillEntry;
    /** Way-major cache storage. */
    std::vector<Way> cache;
    /** Per-set tree-PLRU direction bits. */
    std::vector<std::vector<uint8_t>> plruTrees;

    /** Construction-time feature gate; false keeps all lookup/refill inert. */
    bool enableUopCache;
    /** Nominal storage bytes charged to each single-instruction entry. */
    int maxInstBytesPerEntry;
    /** Number of index sets in each way. */
    int setNum;
    /** Bounded associativity and lookup scan width. */
    int wayNum;
    /** Select mask/shift indexing instead of modulo/division when possible. */
    bool setNumIsPowerOf2;
    /** Low halfword-address bits used as the set index for power-of-two sets. */
    Addr setIdxMask;
    /** Number of set-index bits removed to form the tag. */
    unsigned setIdxShift;
    /** True for normal-path refill, false for cache-stream consumption. */
    bool isBuildMode_;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_UOP_CACHE_HH__
