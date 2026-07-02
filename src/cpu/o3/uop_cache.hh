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
    StaticInstPtr inst;
    Addr pc = 0;
    bool compressed = false;
    Cycles enBufferTime = Cycles(0);

    std::string toString() const;
};

struct UopEntry
{
    Addr tag = 0;
    std::vector<UCInstDesc> insts;
    std::vector<int> halfwordToInstIdx;
    Cycles enUopCacheTime = Cycles(0);
    Addr firstInstPC = 0;
    int bytesSize = 0;

    std::string toString() const;
};

class UopCache
{
  public:
    UopCache(CPU *_cpu, const BaseO3CPUParams &params);

    bool enabled() const { return enableUopCache; }

    void addInst(const DynInstPtr &inst);
    void tick();
    void setCurrentUopEntryDone();
    void flushCurUopEntry();

    std::pair<bool, int> checkUopCacheHit(Addr fetchAddr,
                                          int fetchTargetBytesSize);
    const UCInstDesc *supplyInst(Addr fetchAddr, int instIdx,
                                 int hitWay);
    const UCInstDesc *findInst(Addr fetchAddr, Addr instPC, int hitWay,
                               int *instIdx = nullptr);
    bool invalidUopEntry(Addr fetchAddr);

    void switchToBuildMode();
    void switchToStreamMode();
    bool isBuildMode() const { return isBuildMode_; }
    bool isStreamMode() const { return !isBuildMode_; }

  private:
    using UopEntryPtr = std::unique_ptr<UopEntry>;
    using Way = std::vector<UopEntryPtr>;

    Addr getSetIdxFromVaddr(Addr vaddr) const;
    Addr getTagFromVaddr(Addr vaddr) const;
    UopEntry *newEntry() const;
    int chooseReplacementWay(Addr setIdx) const;
    int getPlruVictim(Addr setIdx) const;
    void updatePlruOnAccess(Addr setIdx, int way);

    CPU *cpu;
    std::deque<UopEntryPtr> refillEntryBuffer;
    UopEntryPtr currentRefillEntry;
    std::vector<Way> cache;
    std::vector<std::vector<uint8_t>> plruTrees;

    bool enableUopCache;
    int maxInstBytesPerEntry;
    int setNum;
    int wayNum;
    Addr setIdxMask;
    unsigned setIdxShift;
    bool isBuildMode_;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_UOP_CACHE_HH__
