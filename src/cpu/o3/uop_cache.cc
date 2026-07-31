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

#include <algorithm>
#include <sstream>

#include "arch/riscv/regs/misc.hh"
#include "base/bitfield.hh"
#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/stats/units.hh"
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
      stats(_cpu, params.uopCacheMaxInstBytesPerEntry),
      currentRefillEntry(nullptr),
      enableUopCache(params.hasUopCache),
      maxInstBytesPerEntry(params.uopCacheMaxInstBytesPerEntry),
      setNum(params.uopCacheSetNum),
      wayNum(params.uopCacheWayNum),
      setNumIsPowerOf2(false),
      setIdxMask(0),
      setIdxShift(0),
      isBuildMode_(true)
{
    fatal_if(setNum == 0, "uopCacheSetNum must be greater than zero");
    fatal_if(wayNum == 0, "uopCacheWayNum must be greater than zero");
    // RISC-V instructions are at most four bytes in this model.  A smaller
    // nominal capacity would contradict the entry and utilization contracts.
    fatal_if(maxInstBytesPerEntry < 4,
             "uopCacheMaxInstBytesPerEntry must be at least 4 bytes");
    fatal_if(!isPowerOf2(wayNum),
             "uopCacheWayNum must be a power of two for tree-PLRU");
    setNumIsPowerOf2 = isPowerOf2(setNum);
    if (setNumIsPowerOf2) {
        setIdxMask = mask(floorLog2(setNum));
        setIdxShift = floorLog2(setNum);
    }
    cache.resize(wayNum);
    for (auto &way : cache) {
        way.resize(setNum);
    }
    plruTrees.resize(setNum, std::vector<uint8_t>(wayNum - 1, 0));
    currentRefillEntry.reset(newEntry());
}

UopCache::UopCacheStats::UopCacheStats(
        CPU *cpu, unsigned max_inst_bytes_per_entry)
    : statistics::Group(cpu, "uop_cache"),
      maxInstBytesPerEntry(max_inst_bytes_per_entry),
      ADD_STAT(entryRefills, statistics::units::Count::get(),
               "Number of uop-cache entries refilled into the cache"),
      ADD_STAT(entryInstsTotal, statistics::units::Count::get(),
               "Total decoded instructions stored by refilled uop-cache entries"),
      ADD_STAT(entryBytesTotal, statistics::units::Byte::get(),
               "Total instruction bytes covered by refilled uop-cache entries"),
      ADD_STAT(entryCapacityBytesTotal, statistics::units::Byte::get(),
               "Total nominal instruction-byte capacity of refilled "
               "single-instruction uop-cache entries"),
      ADD_STAT(entryInsts, statistics::units::Count::get(),
               "Distribution of decoded instructions per refilled uop-cache "
               "entry; single-instruction entries should sample one"),
      ADD_STAT(entryBytes, statistics::units::Byte::get(),
               "Distribution of instruction bytes covered per refilled "
               "uop-cache entry"),
      ADD_STAT(avgEntryInsts, statistics::units::Ratio::get(),
               "Average decoded instructions per refilled uop-cache entry",
               entryInstsTotal / entryRefills),
      ADD_STAT(avgEntryBytes, statistics::units::Byte::get(),
               "Average instruction bytes covered per refilled uop-cache entry",
               entryBytesTotal / entryRefills),
      ADD_STAT(entryByteUtilization, statistics::units::Ratio::get(),
               "Average instruction-byte coverage of refilled "
               "single-instruction uop-cache entries",
               entryBytesTotal / entryCapacityBytesTotal)
{
}

void
UopCache::UopCacheStats::regStats()
{
    statistics::Group::regStats();

    entryRefills.prereq(entryRefills);
    entryInstsTotal.prereq(entryRefills);
    entryBytesTotal.prereq(entryRefills);
    entryCapacityBytesTotal.prereq(entryRefills);
    entryInsts
        .init(1, 1, 1)
        .flags(statistics::pdf | statistics::total)
        .prereq(entryRefills);
    entryBytes
        .init(1, maxInstBytesPerEntry, 1)
        .flags(statistics::pdf | statistics::total)
        .prereq(entryRefills);
    avgEntryInsts.prereq(entryRefills);
    avgEntryBytes.prereq(entryRefills);
    entryByteUtilization.prereq(entryRefills);
}

UopEntry *
UopCache::newEntry() const
{
    return new UopEntry();
}

Addr
UopCache::getSetIdxFromVaddr(Addr vaddr) const
{
    // Index at halfword granularity so a compressed instruction in the upper
    // half of a word does not alias solely because of four-byte alignment.
    const Addr halfword_addr = vaddr >> 1;
    return setNumIsPowerOf2 ? (halfword_addr & setIdxMask) :
                              (halfword_addr % setNum);
}

Addr
UopCache::getTagFromVaddr(Addr vaddr) const
{
    const Addr halfword_addr = vaddr >> 1;
    return setNumIsPowerOf2 ? (halfword_addr >> setIdxShift) :
                              (halfword_addr / setNum);
}

UopCacheContext
UopCache::getContext(ThreadID tid) const
{
    UopCacheContext context;
    context.prv = cpu->readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_PRV, tid);
    context.virt = cpu->readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_VIRMODE, tid);
    context.satp = cpu->readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_SATP, tid);
    context.vsatp = cpu->readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_VSATP, tid);
    context.hgatp = cpu->readMiscRegNoEffect(
        RiscvISA::MiscRegIndex::MISCREG_HGATP, tid);
    return context;
}

void
UopCache::addInst(const DynInstPtr &inst)
{
    if (!enableUopCache || isStreamMode() || inst->isFetchFromUopCache()) {
        return;
    }

    // Macro/micro-op expansion depends on decoder-owned sequencing state.
    // Only self-contained StaticInst objects are safe to replay here.
    if (inst->isMacroop() || inst->isMicroop()) {
        DPRINTF(UC, "skip macro/micro-op refill pc=%#lx inst=%s\n",
                inst->getPC(), inst->staticInst->disassemble(inst->getPC()));
        setCurrentUopEntryDone();
        return;
    }

    currentRefillEntry->firstInstPC = inst->getPC();
    currentRefillEntry->context = getContext(inst->threadNumber);

    UCInstDesc instDesc;
    instDesc.inst = inst->staticInst;
    instDesc.compressed = inst->getInstBytes() == 2;
    instDesc.pc = inst->getPC();
    instDesc.enBufferTime = cpu->curCycle();

    currentRefillEntry->bytesSize = instDesc.sizeBytes();
    currentRefillEntry->inst = std::move(instDesc);
    currentRefillEntry->valid = true;

    DPRINTF(UC, "addInst: pc=%#lx compressed=%d bytes=%d ctx=%s inst=%s\n",
            currentRefillEntry->inst.pc, currentRefillEntry->inst.compressed,
            currentRefillEntry->bytesSize,
            currentRefillEntry->context.toString().c_str(),
            currentRefillEntry->inst.inst->disassemble(
                currentRefillEntry->inst.pc));

    setCurrentUopEntryDone();
}

void
UopCache::tick()
{
    if (!enableUopCache) {
        refillEntryBuffer.clear();
        return;
    }

    // This models an ideal refill port: every completed entry can install in
    // one tick.  It is a documented performance upper bound, not RTL timing.
    for (auto &refillEntry : refillEntryBuffer) {
        Addr setIdx = getSetIdxFromVaddr(refillEntry->firstInstPC);
        Addr tag = getTagFromVaddr(refillEntry->firstInstPC);
        const int existingWay =
            findWay(refillEntry->firstInstPC, refillEntry->context);
        const int replaceWayIdx =
            existingWay >= 0 ? existingWay : chooseReplacementWay(setIdx);
        auto &replaceWay = cache[replaceWayIdx];
        const bool hasVictim = replaceWay[setIdx] != nullptr;

        refillEntry->tag = tag;
        refillEntry->enUopCacheTime = cpu->curCycle();

        stats.entryRefills++;
        stats.entryInstsTotal += 1;
        stats.entryBytesTotal += refillEntry->bytesSize;
        stats.entryCapacityBytesTotal += maxInstBytesPerEntry;
        stats.entryInsts.sample(1);
        stats.entryBytes.sample(refillEntry->bytesSize);

        DPRINTF(UC,
                "refill set=%d tag=%#lx way=%d victim=%d insts=1 "
                "firstPC=%#lx ctx=%s\n",
                setIdx, tag, replaceWayIdx, hasVictim,
                refillEntry->firstInstPC,
                refillEntry->context.toString().c_str());

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

    if (!currentRefillEntry->valid) {
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
UopCache::findWay(Addr instAddr, const UopCacheContext &context) const
{
    const Addr setIdx = getSetIdxFromVaddr(instAddr);
    const Addr tag = getTagFromVaddr(instAddr);

    for (int way = 0; way < cache.size(); ++way) {
        const UopEntry *entry = cache[way][setIdx].get();
        if (entry && entry->valid && entry->tag == tag &&
            entry->context == context &&
            entry->firstInstPC == instAddr) {
            return way;
        }
    }

    return -1;
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
UopCache::checkUopCacheHit(Addr instAddr,
                           const UopCacheContext &context)
{
    if (!enableUopCache) {
        return std::make_pair(false, -1);
    }

    const int hitWay = findWay(instAddr, context);
    if (hitWay >= 0) {
        const Addr setIdx = getSetIdxFromVaddr(instAddr);
        const auto &entry = cache[hitWay][setIdx];
        DPRINTF(UC, "uop cache hit instAddr=%#lx way=%d ctx=%s %s\n",
                instAddr, hitWay, context.toString().c_str(),
                entry->toString().c_str());
        updatePlruOnAccess(setIdx, hitWay);
        return std::make_pair(true, hitWay);
    }

    return std::make_pair(false, -1);
}

bool
UopCache::checkUopCacheBlockHit(Addr startAddr, Addr endAddr,
                                const UopCacheContext &context)
{
    if (!enableUopCache || endAddr <= startAddr) {
        return false;
    }

    for (Addr pc = startAddr; pc < endAddr;) {
        const UCInstDesc *instDesc = findInst(pc, context);
        if (!instDesc) {
            DPRINTF(UC, "uop cache block miss start=%#lx end=%#lx "
                    "missPC=%#lx ctx=%s\n", startAddr, endAddr, pc,
                    context.toString().c_str());
            return false;
        }
        pc += instDesc->sizeBytes();
    }

    DPRINTF(UC, "uop cache block hit start=%#lx end=%#lx ctx=%s\n",
            startAddr, endAddr, context.toString().c_str());
    return true;
}

const UCInstDesc *
UopCache::findInst(Addr instPC, int hitWay,
                   const UopCacheContext &context, int *instIdx)
{
    if (hitWay < 0 || hitWay >= cache.size()) {
        return nullptr;
    }

    Addr setIdx = getSetIdxFromVaddr(instPC);
    Addr tag = getTagFromVaddr(instPC);
    const auto &way = cache[hitWay];
    const UopEntry *entry = way[setIdx].get();
    if (!entry || !entry->valid || entry->tag != tag ||
        entry->context != context ||
        entry->firstInstPC != instPC) {
        return nullptr;
    }

    if (instIdx) {
        *instIdx = 0;
    }

    return &entry->inst;
}

const UCInstDesc *
UopCache::findInst(Addr instPC, const UopCacheContext &context,
                   int *hitWay, int *instIdx)
{
    const int way = findWay(instPC, context);
    if (way < 0) {
        return nullptr;
    }
    if (hitWay) {
        *hitWay = way;
    }
    return findInst(instPC, way, context, instIdx);
}

bool
UopCache::invalidateEntry(Addr fetchAddr)
{
    if (!enableUopCache) {
        return false;
    }

    Addr setIdx = getSetIdxFromVaddr(fetchAddr);
    Addr tag = getTagFromVaddr(fetchAddr);
    bool hasInvalid = false;

    for (auto &way : cache) {
        auto &entry = way[setIdx];
        if (entry && entry->valid && entry->tag == tag &&
            entry->firstInstPC == fetchAddr) {
            entry.reset();
            hasInvalid = true;
        }
    }

    return hasInvalid;
}

void
UopCache::invalidateAll()
{
    // Staged refills are part of the invalidation domain.  Otherwise a stale
    // decoded instruction could be reinstalled immediately after fence.i.
    for (auto &way : cache) {
        for (auto &entry : way) {
            entry.reset();
        }
    }
    refillEntryBuffer.clear();
    currentRefillEntry.reset(newEntry());
    isBuildMode_ = true;
    DPRINTF(UC, "invalidated all cached and in-flight uop entries\n");
}

void
UopCache::invalidateContext(const UopCacheContext &context)
{
    for (auto &way : cache) {
        for (auto &entry : way) {
            if (entry && entry->context == context) {
                entry.reset();
            }
        }
    }

    // Apply the same boundary to entries not installed yet so invalidation
    // cannot be undone by the next idealized refill tick.
    refillEntryBuffer.erase(
        std::remove_if(refillEntryBuffer.begin(), refillEntryBuffer.end(),
            [&context](const UopEntryPtr &entry) {
                return entry && entry->context == context;
            }),
        refillEntryBuffer.end());
    if (currentRefillEntry && currentRefillEntry->valid &&
        currentRefillEntry->context == context) {
        currentRefillEntry.reset(newEntry());
    }
    DPRINTF(UC, "invalidated uop entries for context %s\n",
            context.toString().c_str());
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
       << " tag=0x" << std::hex << tag
       << " ctx=" << context.toString()
       << " inst=";
    if (valid) {
        ss << inst.toString();
    } else {
        ss << "<invalid>";
    }
    return ss.str();
}

bool
UopCacheContext::operator==(const UopCacheContext &other) const
{
    return prv == other.prv &&
           virt == other.virt &&
           satp == other.satp &&
           vsatp == other.vsatp &&
           hgatp == other.hgatp;
}

std::string
UopCacheContext::toString() const
{
    std::stringstream ss;
    ss << "{prv=" << std::dec << prv
       << ", virt=" << virt
       << ", satp=0x" << std::hex << satp
       << ", vsatp=0x" << vsatp
       << ", hgatp=0x" << hgatp
       << "}";
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
