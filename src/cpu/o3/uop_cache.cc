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

#include "cpu/o3/uop_cache.hh"

#include <sstream>

#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "debug/UC.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

UopCache::UopCache(CPU *_cpu, const BaseO3CPUParams &params)
    : cpu(_cpu),
      currentRefillEntry(nullptr),
      enableUopCache(params.hasUopCache),
      maxInstBytesPerEntry(params.uopCacheMaxInstBytesPerEntry),
      setNum(params.uopCacheSetNum),
      wayNum(params.uopCacheWayNum),
      setIdxMask(0),
      setIdxShift(0),
      isBuildMode_(true)
{
    fatal_if(setNum == 0, "uopCacheSetNum must be greater than zero");
    fatal_if(wayNum == 0, "uopCacheWayNum must be greater than zero");
    fatal_if(maxInstBytesPerEntry == 0,
             "uopCacheMaxInstBytesPerEntry must be greater than zero");
    fatal_if(!isPowerOf2(setNum), "uopCacheSetNum must be a power of two");
    fatal_if(!isPowerOf2(wayNum),
             "uopCacheWayNum must be a power of two for tree-PLRU");
    setIdxMask = mask(floorLog2(setNum));
    setIdxShift = floorLog2(setNum);
    cache.resize(wayNum);
    for (auto &way : cache) {
        way.resize(setNum);
    }
    plruTrees.resize(setNum, std::vector<uint8_t>(wayNum - 1, 0));
    currentRefillEntry.reset(newEntry());
}

UopEntry *
UopCache::newEntry() const
{
    auto *entry = new UopEntry();
    entry->insts.reserve(maxInstBytesPerEntry / 2);
    entry->halfwordToInstIdx.reserve(maxInstBytesPerEntry / 2);
    return entry;
}

Addr
UopCache::getSetIdxFromVaddr(Addr vaddr) const
{
    return (vaddr >> 1) & setIdxMask;
}

Addr
UopCache::getTagFromVaddr(Addr vaddr) const
{
    return (vaddr >> 1) >> setIdxShift;
}

void
UopCache::addInst(const DynInstPtr &inst)
{
    if (!enableUopCache || isStreamMode() || inst->isFetchFromUopCache()) {
        return;
    }

    if (inst->isMacroop() || inst->isMicroop()) {
        DPRINTF(UC, "skip macro/micro-op refill pc=%#lx inst=%s\n",
                inst->getPC(), inst->staticInst->disassemble(inst->getPC()));
        setCurrentUopEntryDone();
        return;
    }

    if (currentRefillEntry->insts.empty()) {
        DPRINTF(UC, "curRefillEntry empty, first PC=%#lx\n", inst->getPC());
        currentRefillEntry->firstInstPC = inst->getPC();
    }

    UCInstDesc instDesc;
    instDesc.inst = inst->staticInst;
    instDesc.compressed = inst->getInstBytes() == 2;
    instDesc.pc = inst->getPC();
    instDesc.enBufferTime = cpu->curCycle();

    if (instDesc.compressed) {
        currentRefillEntry->bytesSize += 2;
    } else {
        currentRefillEntry->bytesSize += 4;
    }

    DPRINTF(UC, "addInst: pc=%#lx compressed=%d bytes=%d inst=%s\n",
            instDesc.pc, instDesc.compressed, currentRefillEntry->bytesSize,
            instDesc.inst->disassemble(instDesc.pc));

    const Addr halfword_offset =
        (instDesc.pc - currentRefillEntry->firstInstPC) >> 1;
    if (halfword_offset >= currentRefillEntry->halfwordToInstIdx.size()) {
        currentRefillEntry->halfwordToInstIdx.resize(halfword_offset + 1, -1);
    }
    currentRefillEntry->halfwordToInstIdx[halfword_offset] =
        currentRefillEntry->insts.size();
    currentRefillEntry->insts.push_back(std::move(instDesc));

    if (inst->isLastFtqEntryInst()) {
        setCurrentUopEntryDone();
        return;
    }

    if (currentRefillEntry->bytesSize >= maxInstBytesPerEntry) {
        setCurrentUopEntryDone();
    }
}

void
UopCache::tick()
{
    if (!enableUopCache) {
        refillEntryBuffer.clear();
        return;
    }

    for (auto &refillEntry : refillEntryBuffer) {
        Addr setIdx = getSetIdxFromVaddr(refillEntry->firstInstPC);
        Addr tag = getTagFromVaddr(refillEntry->firstInstPC);
        const int replaceWayIdx = chooseReplacementWay(setIdx);
        auto &replaceWay = cache[replaceWayIdx];
        const bool hasVictim = replaceWay[setIdx] != nullptr;

        refillEntry->tag = tag;
        refillEntry->enUopCacheTime = cpu->curCycle();

        DPRINTF(UC,
                "refill set=%d tag=%#lx way=%d victim=%d insts=%d "
                "firstPC=%#lx\n",
                setIdx, tag, replaceWayIdx, hasVictim,
                refillEntry->insts.size(), refillEntry->firstInstPC);

        replaceWay[setIdx] = std::move(refillEntry);
        updatePlruOnAccess(setIdx, replaceWayIdx);
    }

    refillEntryBuffer.clear();
}

void
UopCache::setCurrentUopEntryDone()
{
    if (!enableUopCache) {
        flushCurUopEntry();
        return;
    }

    if (currentRefillEntry->insts.empty()) {
        return;
    }

    refillEntryBuffer.push_back(std::move(currentRefillEntry));
    currentRefillEntry.reset(newEntry());
}

void
UopCache::flushCurUopEntry()
{
    DPRINTF(UC, "flushCurUopEntry\n");
    currentRefillEntry.reset(newEntry());
}

int
UopCache::chooseReplacementWay(Addr setIdx) const
{
    for (int way = 0; way < cache.size(); ++way) {
        if (!cache[way][setIdx]) {
            return way;
        }
    }
    return getPlruVictim(setIdx);
}

int
UopCache::getPlruVictim(Addr setIdx) const
{
    assert(setIdx < plruTrees.size());
    if (wayNum == 1) {
        return 0;
    }

    const auto &tree = plruTrees[setIdx];
    int node = 0;
    int low = 0;
    int high = wayNum;
    while (node < wayNum - 1) {
        const int mid = low + (high - low) / 2;
        if (tree[node] == 0) {
            node = 2 * node + 1;
            high = mid;
        } else {
            node = 2 * node + 2;
            low = mid;
        }
    }
    return low;
}

void
UopCache::updatePlruOnAccess(Addr setIdx, int way)
{
    assert(setIdx < plruTrees.size());
    assert(way >= 0 && way < wayNum);
    if (wayNum == 1) {
        return;
    }

    auto &tree = plruTrees[setIdx];
    int node = 0;
    int low = 0;
    int high = wayNum;
    while (node < wayNum - 1) {
        const int mid = low + (high - low) / 2;
        if (way < mid) {
            tree[node] = 1;
            node = 2 * node + 1;
            high = mid;
        } else {
            tree[node] = 0;
            node = 2 * node + 2;
            low = mid;
        }
    }
}

std::pair<bool, int>
UopCache::checkUopCacheHit(Addr fetchAddr, int fetchTargetBytesSize)
{
    if (!enableUopCache) {
        return std::make_pair(false, -1);
    }

    Addr setIdx = getSetIdxFromVaddr(fetchAddr);
    Addr tag = getTagFromVaddr(fetchAddr);

    for (int i = 0; i < cache.size(); i++) {
        const auto &way = cache[i];
        const UopEntry *entry = way[setIdx].get();
        if (!entry) {
            continue;
        }

        if (entry->tag == tag &&
            entry->bytesSize >= fetchTargetBytesSize) {
            DPRINTF(UC, "uop cache hit fetchAddr=%#lx size=%d way=%d %s\n",
                    fetchAddr, fetchTargetBytesSize, i,
                    entry->toString().c_str());
            updatePlruOnAccess(setIdx, i);
            return std::make_pair(true, i);
        }
    }

    return std::make_pair(false, -1);
}

const UCInstDesc *
UopCache::supplyInst(Addr fetchAddr, int instIdx, int hitWay)
{
    Addr setIdx = getSetIdxFromVaddr(fetchAddr);
    Addr tag = getTagFromVaddr(fetchAddr);
    assert(hitWay >= 0 && hitWay < cache.size());

    const auto &way = cache[hitWay];
    const UopEntry *entry = way[setIdx].get();
    assert(entry);
    assert(entry->tag == tag);
    assert(instIdx >= 0 && instIdx < entry->insts.size());
    return &entry->insts[instIdx];
}

const UCInstDesc *
UopCache::findInst(Addr fetchAddr, Addr instPC, int hitWay, int *instIdx)
{
    Addr setIdx = getSetIdxFromVaddr(fetchAddr);
    Addr tag = getTagFromVaddr(fetchAddr);
    if (hitWay < 0 || hitWay >= cache.size()) {
        return nullptr;
    }

    const auto &way = cache[hitWay];
    const UopEntry *entry = way[setIdx].get();
    if (!entry || entry->tag != tag) {
        return nullptr;
    }

    if (instPC < entry->firstInstPC) {
        return nullptr;
    }

    const Addr halfword_offset = (instPC - entry->firstInstPC) >> 1;
    if (halfword_offset >= entry->halfwordToInstIdx.size()) {
        return nullptr;
    }

    const int idx = entry->halfwordToInstIdx[halfword_offset];
    if (idx < 0) {
        return nullptr;
    }

    assert(idx < entry->insts.size());
    if (instIdx) {
        *instIdx = idx;
    }

    return &entry->insts[idx];
}

bool
UopCache::invalidUopEntry(Addr fetchAddr)
{
    if (!enableUopCache) {
        return false;
    }

    Addr setIdx = getSetIdxFromVaddr(fetchAddr);
    Addr tag = getTagFromVaddr(fetchAddr);
    bool hasInvalid = false;

    for (auto &way : cache) {
        auto &entry = way[setIdx];
        if (entry && entry->tag == tag) {
            entry.reset();
            hasInvalid = true;
        }
    }

    return hasInvalid;
}

void
UopCache::switchToBuildMode()
{
    flushCurUopEntry();
    isBuildMode_ = true;
}

void
UopCache::switchToStreamMode()
{
    flushCurUopEntry();
    isBuildMode_ = false;
}

std::string
UopEntry::toString() const
{
    std::stringstream ss;
    ss << "UopEntry: bytesSize=" << bytesSize
       << " firstInstPC=0x" << std::hex << firstInstPC
       << " enUopCacheTime=" << std::dec << enUopCacheTime
       << " tag=0x" << std::hex << tag << " insts=[";
    for (const auto &inst : insts) {
        ss << inst.toString() << ", ";
    }
    ss << "]";
    return ss.str();
}

std::string
UCInstDesc::toString() const
{
    std::stringstream ss;
    ss << "{pc=0x" << std::hex << pc << ", compressed=" << std::dec
       << compressed << "}";
    return ss.str();
}

} // namespace o3
} // namespace gem5
