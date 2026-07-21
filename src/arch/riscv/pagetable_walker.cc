/*
 * Copyright (c) 2012 ARM Limited
 * Copyright (c) 2020 Barkhausen Institut
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
 * Copyright (c) 2007 The Hewlett-Packard Development Company
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

#include "arch/riscv/pagetable_walker.hh"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <numeric>

#include "arch/riscv/faults.hh"
#include "arch/riscv/page_size.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/tlb.hh"
#include "base/bitfield.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/trie.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/PageTableWalker.hh"
#include "debug/PageTableWalker2.hh"
#include "debug/PageTableWalker3.hh"
#include "debug/PageTableWalkerTwoStage.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"
#include "sim/system.hh"

#ifndef MPT_ENABLED
#define MPT_ENABLED 1
#endif
#define MPT_CACHE_ENABLED 1
#ifndef MPT_FORCE_ALLOW_PERMS
#define MPT_FORCE_ALLOW_PERMS 0
#endif

namespace gem5
{

namespace RiscvISA {
#if MPT_ENABLED

static constexpr Cycles MPT_CACHE_HIT_LATENCY = Cycles(3);

static constexpr uint8_t MPT_ALLOW_ALL_PERMS =
    MPT_PERM_R | MPT_PERM_W | MPT_PERM_X;
static constexpr unsigned MPT_ROOT_ENTRIES = PageBytes / MPT_MPTE_SIZE;
static constexpr Addr MPT_PPN_MASK = (1ULL << 44) - 1;
static constexpr int MPT_CACHE_SP_LEVEL = 5;

static Addr
mptSPCacheKey(Addr aligned, int level)
{
    assert((aligned & 0x7) == 0);
    return aligned | static_cast<Addr>(level & 0x7);
}

static constexpr uint64_t
makeAllowAllLeafMPTE()
{
    uint64_t mpte = 0x3; // valid + leaf
    for (unsigned i = 0; i < MPT_NUM_PERMS; ++i) {
        mpte |= static_cast<uint64_t>(MPT_ALLOW_ALL_PERMS)
            << (10 + i * MPT_PERM_BITS_PER_ENTRY);
    }
    return mpte;
}

static constexpr uint64_t MPT_ALLOW_ALL_LEAF_MPTE =
    makeAllowAllLeafMPTE();

static uint64_t
makeInternalMPTE(Addr nextLevelPaddr)
{
    return 0x1 | ((nextLevelPaddr >> PageShift) << 10);
}

static uint8_t
effectiveMptPerm(uint8_t rawPerm)
{
#if MPT_FORCE_ALLOW_PERMS
    (void)rawPerm;
    return MPT_ALLOW_ALL_PERMS;
#else
    return rawPerm;
#endif
}

static bool
mptPermAllows(uint8_t perm, BaseMMU::Mode mode)
{
    return mptPermAllowsAccess(perm, mode);
}

PLRUTreeN::PLRUTreeN(size_t ways)
    : numWays(ways), bits(ways > 1 ? ways - 1 : 0, false)
{
    assert((ways & (ways - 1)) == 0 && "PLRU requires power-of-two number of ways");
}

size_t PLRUTreeN::getVictim() const {
    size_t idx = 0;
    while (idx < bits.size()) {
        idx = bits[idx] ? (2 * idx + 2) : (2 * idx + 1);
    }
    return idx - bits.size();
}

void PLRUTreeN::access(size_t way) {
    size_t idx = way + bits.size();
    while (idx > 0) {
        size_t parent = (idx - 1) / 2;
        bits[parent] = (idx % 2 == 0); // 右子 = 1，左子 = 0
        idx = parent;
    }
}

void PLRUTreeN::reset() {
    std::fill(bits.begin(), bits.end(), false);
}

//std::unordered_map<const Request*, std::pair<ThreadContext*, BaseMMU::Translation*>> gem5::RiscvISA::mptContextMap;


MPT::MPT() : rootPPN(0), nextPPN(0x10000) {
    //rootPPN = buildSimulatedMPTTree();
    MptReadReq = nullptr;
    hasActiveMptRead = false;
    processingMptResp = false;
    walker = nullptr;
    mptretry = false;
    mptinflight = 0;
    simulatedTreeBuilt = false;
    simulatedRootPaddr = 0;

}

Addr
MPT::buildSimulatedMPTTree(System *sys)
{
    if (sys == nullptr) {
        panic("Cannot build simulated MPT tree without System\n");
    }

    static constexpr Addr MPT_INDEX_MASK = MPT_ROOT_ENTRIES - 1;
    auto mptIndex = [](Addr paddr, int level) {
        return (paddr >> (getPageShiftForLevel(level) + 4)) &
            MPT_INDEX_MASK;
    };
    auto l1TableKey = [](Addr rootIdx, Addr l2Idx) {
        return (rootIdx << 9) | l2Idx;
    };
    auto l0TableKey = [](Addr rootIdx, Addr l2Idx, Addr l1Idx) {
        return (rootIdx << 18) | (l2Idx << 9) | l1Idx;
    };

    AddrRangeList ranges = sys->getPhysMem().getConfAddrRanges();
    const Addr l0TableCoverage =
        MPT_ROOT_ENTRIES * getRegionSizeForLevel(0);
    std::map<Addr, Addr> l2Tables;
    std::map<Addr, Addr> l1Tables;
    std::map<Addr, Addr> l0Tables;

    for (const auto &range : ranges) {
        if (!range.valid() || range.size() == 0) {
            continue;
        }

        Addr chunk = range.start() & ~(l0TableCoverage - 1);
        while (chunk <= range.end()) {
            Addr rootIdx = mptIndex(chunk, 3);
            Addr l2Idx = mptIndex(chunk, 2);
            Addr l1Idx = mptIndex(chunk, 1);
            l2Tables.emplace(rootIdx, 0);
            l1Tables.emplace(l1TableKey(rootIdx, l2Idx), 0);
            l0Tables.emplace(l0TableKey(rootIdx, l2Idx, l1Idx), 0);
            if (chunk > MaxAddr - l0TableCoverage) {
                break;
            }
            chunk += l0TableCoverage;
        }
    }

    const uint64_t tablePages =
        1 + l2Tables.size() + l1Tables.size() + l0Tables.size();
    const uint64_t tableBytes = tablePages * PageBytes;

    const AddrRangeList &reservedRanges = sys->getMptReservedMemRanges();
    if (reservedRanges.empty()) {
        panic("Cannot build simulated MPT tree: no explicit MPT reserved "
              "memory range configured for %llu pages (%llu bytes), "
              "MMPT=%#lx mode=%#lx\n",
              static_cast<unsigned long long>(tablePages),
              static_cast<unsigned long long>(tableBytes),
              static_cast<uint64_t>(mmpt), mmpt.mode);
    }

    Addr selected = 0;
    bool found = false;
    for (const auto &range : reservedRanges) {
        if (!range.valid() || range.size() < tableBytes) {
            continue;
        }

        Addr candidate = (range.start() + PageBytes - 1) & ~(PageBytes - 1);
        if (candidate > MaxAddr - tableBytes + 1) {
            continue;
        }
        if (!range.contains(candidate) ||
                !range.contains(candidate + tableBytes - 1)) {
            continue;
        }

        selected = candidate;
        found = true;
        break;
    }

    if (!found) {
        panic("Cannot build simulated MPT tree: explicit MPT reserved memory "
              "is too small for %llu pages (%llu bytes), MMPT=%#lx "
              "mode=%#lx\n",
              static_cast<unsigned long long>(tablePages),
              static_cast<unsigned long long>(tableBytes),
              static_cast<uint64_t>(mmpt), mmpt.mode);
    }
    if (!sys->isMemAddr(selected) ||
            !sys->isMemAddr(selected + tableBytes - 1)) {
        panic("Cannot build simulated MPT tree: reserved range "
              "[%#lx, %#lx] is not backed by physical memory, MMPT=%#lx "
              "mode=%#lx\n",
              selected, selected + tableBytes - 1,
              static_cast<uint64_t>(mmpt), mmpt.mode);
    }
    if ((selected & (PageBytes - 1)) != 0) {
        panic("Simulated MPT root is not page aligned: root=%#lx MMPT=%#lx "
              "mode=%#lx\n", selected, static_cast<uint64_t>(mmpt),
              mmpt.mode);
    }
    Addr rootPpn = selected >> PageShift;
    rootPPN = rootPpn;
    if ((rootPpn & ~MPT_PPN_MASK) != 0) {
        panic("Simulated MPT root PPN exceeds MMPT.ppn field: root=%#lx "
              "ppn=%#lx MMPT=%#lx mode=%#lx\n",
              selected, rootPpn, static_cast<uint64_t>(mmpt), mmpt.mode);
    }

    Addr nextTablePaddr = selected + PageBytes;
    auto allocTable = [&nextTablePaddr]() {
        Addr tablePaddr = nextTablePaddr;
        nextTablePaddr += PageBytes;
        return tablePaddr;
    };
    auto writeMPTE = [sys](Addr tablePaddr, Addr index, uint64_t mpte) {
        sys->physProxy.write<uint64_t>(
            tablePaddr + index * MPT_MPTE_SIZE,
            mpte, sys->getGuestByteOrder());
    };
    auto clearTable = [&writeMPTE](Addr tablePaddr) {
        for (unsigned i = 0; i < MPT_ROOT_ENTRIES; ++i) {
            writeMPTE(tablePaddr, i, 0);
        }
    };
    auto fillAllowAllLeafTable = [&writeMPTE](Addr tablePaddr) {
        for (unsigned i = 0; i < MPT_ROOT_ENTRIES; ++i) {
            writeMPTE(tablePaddr, i, MPT_ALLOW_ALL_LEAF_MPTE);
        }
    };

    clearTable(selected);
    for (auto &table : l2Tables) {
        table.second = allocTable();
        clearTable(table.second);
    }
    for (auto &table : l1Tables) {
        table.second = allocTable();
        clearTable(table.second);
    }
    for (auto &table : l0Tables) {
        table.second = allocTable();
        fillAllowAllLeafTable(table.second);
    }
    assert(nextTablePaddr == selected + tableBytes);

    for (const auto &table : l2Tables) {
        writeMPTE(selected, table.first, makeInternalMPTE(table.second));
    }
    for (const auto &table : l1Tables) {
        Addr rootIdx = table.first >> 9;
        Addr l2Idx = table.first & MPT_INDEX_MASK;
        writeMPTE(l2Tables[rootIdx], l2Idx, makeInternalMPTE(table.second));
    }
    for (const auto &table : l0Tables) {
        Addr rootIdx = table.first >> 18;
        Addr l2Idx = (table.first >> 9) & MPT_INDEX_MASK;
        Addr l1Idx = table.first & MPT_INDEX_MASK;
        writeMPTE(l1Tables[l1TableKey(rootIdx, l2Idx)], l1Idx,
                  makeInternalMPTE(table.second));
    }

    DPRINTF(PageTableWalker,
            "Built simulated L0-leaf allow-all MPT at paddr %#lx ppn %#lx "
            "pages %llu bytes %llu l2 %lu l1 %lu l0 %lu\n",
            selected, rootPpn, static_cast<unsigned long long>(tablePages),
            static_cast<unsigned long long>(tableBytes), l2Tables.size(),
            l1Tables.size(), l0Tables.size());
    return selected;
}

bool
MPT::ensureSimulatedMPTTree(System *sys, ThreadContext *tc)
{
    if (mmpt.mode == 0 || mmpt.ppn != 0) {
        return false;
    }
    if (tc == nullptr) {
        panic("Cannot initialize simulated MPT tree without ThreadContext: "
              "MMPT=%#lx mode=%#lx\n",
              static_cast<uint64_t>(mmpt), mmpt.mode);
    }

    if (!simulatedTreeBuilt) {
        simulatedRootPaddr = buildSimulatedMPTTree(sys);
        simulatedTreeBuilt = true;
    }

    if ((simulatedRootPaddr & (PageBytes - 1)) != 0) {
        panic("Stored simulated MPT root is not page aligned: root=%#lx "
              "MMPT=%#lx mode=%#lx\n",
              simulatedRootPaddr, static_cast<uint64_t>(mmpt), mmpt.mode);
    }
    Addr rootPpn = simulatedRootPaddr >> PageShift;
    if ((rootPpn & ~MPT_PPN_MASK) != 0) {
        panic("Stored simulated MPT root PPN exceeds MMPT.ppn field: "
              "root=%#lx ppn=%#lx MMPT=%#lx mode=%#lx\n",
              simulatedRootPaddr, rootPpn, static_cast<uint64_t>(mmpt),
              mmpt.mode);
    }

    MMPT newMmpt = mmpt;
    newMmpt.ppn = rootPpn;
    tc->setMiscRegNoEffect(MISCREG_MMPT, newMmpt);
    mmpt = newMmpt;
    if (globalMPTCache != nullptr) {
        globalMPTCache->mfence_all();
    }

    DPRINTF(PageTableWalker,
            "Initialized simulated MPT MMPT=%#lx rootPaddr=%#lx\n",
            static_cast<uint64_t>(mmpt), simulatedRootPaddr);
    return true;
}

static void
discardMptPacket(PacketPtr pkt)
{
    if (pkt == nullptr) {
        return;
    }
    if (pkt->senderState != nullptr) {
        delete pkt->popSenderState();
    }
    delete pkt;
}

static void
addUniqueMptWaiter(std::vector<Walker::WalkerState*> &waiters,
                   Walker::WalkerState *senderState)
{
    if (senderState == nullptr) {
        return;
    }
    if (std::find(waiters.begin(), waiters.end(), senderState) ==
            waiters.end()) {
        waiters.push_back(senderState);
    }
}

bool MPT::sendMptPacket() {
    if (mptinflight > 0) {
        return true;
    }

    if (!hasActiveMptRead) {
        if (pendingMptReads.empty()) {
            return false;
        }
        activeMptRead = pendingMptReads.front();
        hasActiveMptRead = true;
        pendingMptReads.pop_front();
    }

    PacketPtr pkt = activeMptRead.pkt;
    if (pkt == nullptr) {
        panic("MPT active read has no owned request packet: "
              "mptePaddr=%#lx hasActiveMptRead=%d mptinflight=%d "
              "mptretry=%d pending=%lu processingMptResp=%d\n",
              activeMptRead.mptePaddr, hasActiveMptRead, mptinflight,
              mptretry, pendingMptReads.size(), processingMptResp);
    }
    if (!pkt->isRequest()) {
        panic("MPT tried to send non-request packet: pkt=%s "
              "mptePaddr=%#lx hasActiveMptRead=%d mptinflight=%d "
              "mptretry=%d pending=%lu processingMptResp=%d\n",
              pkt->print(), activeMptRead.mptePaddr, hasActiveMptRead,
              mptinflight, mptretry, pendingMptReads.size(),
              processingMptResp);
    }
    MptReadReq = pkt;

    Walker *pktWalker = walker;
    auto *senderState = pkt->findNextSenderState<Walker::WalkerSenderState>();
    if (senderState != nullptr && senderState->senderWalk != nullptr) {
        pktWalker = senderState->senderWalk->walker;
    }
    if (pktWalker == nullptr) {
        panic("MPT packet has no walker to send timing request");
        return false;
    }

    if (pktWalker->port.sendTimingReq(pkt)) {//使用walker port 发送
        DPRINTF(PageTableWalker, "me send MPT packet\n");
        activeMptRead.pkt = nullptr;
        MptReadReq = nullptr;
        mptretry = false;
        mptinflight++;
        return true;
    }

    DPRINTF(PageTableWalker, "mpt port busy\n");
    mptretry = true;
    return true;
}

bool MPT::issueMptPacket()
{
    return sendMptPacket();
}

bool MPT::enqueueMptPacket(PacketPtr pkt, Walker::WalkerState *senderState)
{
    Addr mptePaddr = pkt->req->getPaddr();

    if (!pkt->isRequest()) {
        panic("MPT tried to enqueue non-request packet: pkt=%s "
              "mptePaddr=%#lx hasActiveMptRead=%d mptinflight=%d "
              "mptretry=%d pending=%lu processingMptResp=%d\n",
              pkt->print(), mptePaddr, hasActiveMptRead, mptinflight,
              mptretry, pendingMptReads.size(), processingMptResp);
    }

    if (hasActiveMptRead && activeMptRead.mptePaddr == mptePaddr) {
        addUniqueMptWaiter(activeMptRead.waiters, senderState);
        discardMptPacket(pkt);
        DPRINTF(PageTableWalker, "merge active MPT packet %#lx waiters=%lu\n",
                mptePaddr, activeMptRead.waiters.size());
        return true;
    }

    for (auto &pending : pendingMptReads) {
        if (pending.mptePaddr == mptePaddr) {
            addUniqueMptWaiter(pending.waiters, senderState);
            discardMptPacket(pkt);
            DPRINTF(PageTableWalker, "merge pending MPT packet %#lx waiters=%lu\n",
                    mptePaddr, pending.waiters.size());
            return true;
        }
    }

    MptPendingRead group;
    group.mptePaddr = mptePaddr;
    group.pkt = pkt;
    addUniqueMptWaiter(group.waiters, senderState);

    if (processingMptResp || hasActiveMptRead || mptretry ||
            mptinflight > 0) {
        pendingMptReads.push_back(group);
        DPRINTF(PageTableWalker, "queue MPT packet %#lx pending=%lu\n",
                mptePaddr, pendingMptReads.size());
        return true;
    }

    activeMptRead = group;
    hasActiveMptRead = true;
    return issueMptPacket();
}

void MPT::flushPendingMptReads()
{
    if (hasActiveMptRead && mptinflight == 0) {
        discardMptPacket(activeMptRead.pkt);
        activeMptRead = MptPendingRead();
        hasActiveMptRead = false;
        MptReadReq = nullptr;
    }
    while (!pendingMptReads.empty()) {
        discardMptPacket(pendingMptReads.front().pkt);
        pendingMptReads.pop_front();
    }
    mptretry = false;
    processingMptResp = false;
}

PacketPtr MPT::CreateMptReqPacket(Addr paddr,Walker::WalkerState* senderState) {
    Walker::WalkerSenderState* walker_state = new Walker::WalkerSenderState(senderState);
    DPRINTF(PageTableWalker,"start create  MPT packet");
    Request::Flags flags = Request::PHYSICAL;

    RequestPtr request = std::make_shared<Request>(
        paddr, 8, flags, senderState->walker->requestorId);

    request->setMptWalk(true);

    PacketPtr pkt = new Packet(request, MemCmd::ReadReq);

    pkt->allocate();
    pkt->pushSenderState(walker_state);
    return pkt;
}

bool MPT::MptRecvTimingResp(PacketPtr pkt){
    if (mptinflight <= 0) {
        panic("MPT received timing response with no inflight request: pkt=%s "
              "hasActiveMptRead=%d mptretry=%d pending=%lu "
              "processingMptResp=%d\n",
              pkt->print(), hasActiveMptRead, mptretry,
              pendingMptReads.size(), processingMptResp);
    }
    mptinflight--;
    MptReadReq = nullptr;
    processingMptResp = true;
    MptPendingRead completedRead = activeMptRead;
    activeMptRead = MptPendingRead();
    hasActiveMptRead = false;

    Walker::WalkerSenderState * senderState = nullptr;
    Packet::SenderState * rawSenderState = nullptr;
    if (pkt->senderState != nullptr) {
        rawSenderState = pkt->popSenderState();
        senderState = dynamic_cast<Walker::WalkerSenderState *>(
                rawSenderState);
    }
    if (pkt->isRead()) {
        DPRINTF(PageTableWalker,"hi MptRecvTimingResp");
        // should not have a pending read it we also had one outstanding
        pkt->headerDelay = pkt->payloadDelay = 0;
        uint64_t raw = pkt->getLE<uint64_t>();
        Addr mptePaddr = pkt->req->getPaddr();
        std::vector<Walker::WalkerState*> waiters = completedRead.waiters;
        if (waiters.empty() && senderState != nullptr) {
            waiters.push_back(senderState->senderWalk);
        }

        for (auto *senderWalk : waiters) {
            if (senderWalk == nullptr) {
                continue;
            }
            bool islastMPT = senderWalk->stepMPTwalkFromMPTE(raw, mptePaddr);
            if (islastMPT) {
                completeMptWaiter(senderWalk);
            }
        }
    }
    if (senderState != nullptr) {
        delete senderState;
    } else {
        delete rawSenderState;
    }
    delete pkt;
    processingMptResp = false;
    if (mptinflight == 0 && !mptretry && MptReadReq == nullptr) {
        issueMptPacket();
    }
    return true;
}

void MPT::completeMptWaiter(Walker::WalkerState *senderWalk)
{
    bool walkComplete = senderWalk->completeMPTWalk();
    if (!walkComplete) {
        return;
    }
    std::list<Walker::WalkerState *>::iterator iter;
    bool erased = false;
    for (iter = senderWalk->walker->currStates.begin();
         iter != senderWalk->walker->currStates.end(); iter++) {
        Walker::WalkerState *walkerState = *(iter);
        if (walkerState == senderWalk) {
            DPRINTF(PageTableWalker,
                    "Walk complete for %#lx (pc=%#lx), erase it\n",
                    senderWalk->mainReq->getVaddr(), senderWalk->mainReq->getPC());
            iter = senderWalk->walker->currStates.erase(iter);
            erased = true;
            break;
        }
    }
    if (erased) {
        senderWalk->walker->releasePtwLevel(senderWalk);
        senderWalk->walker->retryPtwLevelBlockedStates();
        senderWalk->walker->retryPtwMissQueue();
        delete senderWalk;
    }
}

bool Walker::WalkerState::stepMPTwalk(){
    Addr paddr= MptReadReq->req->getPaddr();
    //uint64_t raw=MptReadReq->getLE_l2tlb<uint64_t>((paddr>>3)&0b111);
    uint64_t raw=MptReadReq->getLE <uint64_t>( );
    return stepMPTwalkFromMPTE(raw, paddr);
}

bool Walker::WalkerState::stepMPTwalkFromMPTE(uint64_t raw, Addr mptePaddr)
{
    DPRINTF(PageTableWalker, "MPT refill mpte paddr:%#lx raw:%#lx\n",
            mptePaddr, raw);
    MPTE52 mpte(raw);
    uint64_t regionSize = getRegionSizeForLevel(mpt_level);
    Addr aligned = globalMPTCache->regionAlign(PaddrUT, mpt_level);
    MPTCacheEntry entry = {
        aligned, mpte, true, mpt_level, log2floor(regionSize)
    };
    if (!mpte.isValid() || ((!mpte.isLeaf()) && mpt_level == 0)) {
        DPRINTF(PageTableWalker, "MPT walk failed with req:%#lx level:%i mpte:%#lx\n",
                PaddrUT, mpt_level, raw);
        if (mptCheckingPteRead) {
            pteReadMptResult = false;
        } else {
            MPTresult = false;
            mptInfo.invalidate();
        }
        if (isMPTing){
            return true;
        }
        else{
            sendPackets();//就算没过，也要sendpackets，然后recvpacket立马报mptfault，gem5就是这样的
            return false; // Invalid entry.
        }

    }
    int cacheLevel =
        (mpte.isLeaf() && mpt_level > 0) ? MPT_CACHE_SP_LEVEL : mpt_level;
    Addr cacheKey = (cacheLevel == MPT_CACHE_SP_LEVEL) ?
        mptSPCacheKey(aligned, mpt_level) : aligned;
    bool insertedNewEntry =
        globalMPTCache->insertOrRefreshEntry(cacheLevel, cacheKey, entry);


    if (mpte.isLeaf()) {
        // Find the leaf entry and return directly.
        if (insertedNewEntry) {
            if (mpt_level == 0) ++globalMPTCache->mptCacheL0Misses;//统计数据
            else ++globalMPTCache->mptCacheSPMisses;
        }

        uint8_t pi = (PaddrUT >> getPageShiftForLevel(mpt_level)) & 0xF;
        uint8_t rawPerm = mpte.perms(pi);
        uint8_t perm = effectiveMptPerm(rawPerm);
        DPRINTF(PageTableWalker,
                "MPT walk leaf req:%#lx mpte:%#lx rawPerm:%#x effectivePerm:%#x\n",
                PaddrUT, raw, rawPerm, perm);
        if (mptCheckingPteRead) {
            pteReadMptResult = mptPermAllows(perm, mptCheckMode);
        } else {
            MPTresult = mptPermAllows(perm, mptCheckMode);
            mptInfo.write_mpt_raw(perm, mpt_level);
        }
        if (isMPTing) {
            return true;
        }
        else {
            sendPackets();//就算没过，也要sendpackets，然后recvpacket立马报mptfault，gem5就是这样的
            return false;
        }

    } else{
        DPRINTF(PageTableWalker, "MPT walk internal req:%#lx level:%i mpte:%#lx\n",
                PaddrUT, mpt_level, raw);
        if (insertedNewEntry) {
            if (mpt_level == 1) ++globalMPTCache->mptCacheL1Misses;
            else if (mpt_level == 2) ++globalMPTCache->mptCacheL2Misses;
            else if (mpt_level == 3) ++globalMPTCache->mptCacheL3Misses;
        }

        Addr base = mpte.nextLevelPAddr();
        mpt_level--;
        bool Nofault = globalMPT.walk(mpt_level,base,PaddrUT,requestors.front().tc, walker->pma, walker->pmp,this);
        if (!Nofault){
            if (mptCheckingPteRead) {
                pteReadMptResult = false;
            } else {
                MPTresult = false;
                mptInfo.invalidate();
            }
            if (isMPTing){
                return true;
            }
            else{
                sendPackets();//就算没过，也要sendpackets，然后recvpacket立马报mptfault，gem5就是这样的
                return false;
            }
        }
        return false;
    }
}


bool MPT::readMPTE(Addr paddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp ,Walker::WalkerState* senderState )
{

    DPRINTF(PageTableWalker,"start readMPTE");
    walker = senderState->walker;
    PacketPtr req = CreateMptReqPacket(paddr,senderState);
    // 2 PMA check

    pma->check(req->req);

    // 3 Retrieve privilege level: Called by a member function within the class
    //PrivilegeMode pmode = tlb->getMemPriv(tc, BaseMMU::Read);
    PrivilegeMode pmode = PRV_S;
    //reference：pmp->pmpCheck(req, mode, static_cast<MMU *>(tc->getMMUPtr())->getMemPriv(tc, mode), tc);
    gem5::Fault fault = NoFault;
    //gem5::Fault fault = pmp->pmpCheck(req->req, BaseMMU::Read, pmode, tc);
    if (fault != NoFault) {
        return false;
        DPRINTF(PageTableWalker,"PMP blocked access to MPTE at 0x%lx\n", paddr);
    }
    return enqueueMptPacket(req, senderState);
}

// Multi-level Smmpt52 traversal returning an MPTE52 or invalid entry.
bool
MPT::walk(int hit_level, Addr base, Addr paddrUT, ThreadContext *tc,
          PMAChecker *pma, PMP *pmp, Walker::WalkerState *senderState)
{
    //Addr base = rootPPN << 12;  // Page table base address = PPN × 4KB (Page table page size is fixed at 4KB).
    //这里暂时忽略mpte 格式不匹配的af，大概26年3月能修
    MPTCacheEntry entry;
    // Each MPTE controls 16 regions at the current page granularity.
    // The low 4 bits after page shift are the MPTE permission index (pi),
    // so table indexing must skip them.
    size_t shift = getPageShiftForLevel(hit_level) + 4;
    size_t index = (paddrUT >> shift) & 0x1FF;
    Addr paddr = base + index * MPT_MPTE_SIZE;
    DPRINTF(PageTableWalker,
            "start mpt walk with level %i, paddr: %lx, base: %lx, "
            "index: %lx, req: %lx\n",
            hit_level, paddr, base, index, paddrUT);
    // read MPTE
    return readMPTE(paddr, tc, pma, pmp, senderState );
}

bool
MPT::checkFunctional(Addr paddr, BaseMMU::Mode mode, System *sys) const
{
    panic_if(sys == nullptr, "Functional MPT check requires a system\n");

    Addr base = mmpt.ppn << PageShift;
    for (int level = 3; level >= 0; --level) {
        const size_t shift = getPageShiftForLevel(level) + 4;
        const size_t index = (paddr >> shift) & 0x1ff;
        const Addr mptePaddr = base + index * MPT_MPTE_SIZE;
        const uint64_t raw = sys->physProxy.read<uint64_t>(
            mptePaddr, sys->getGuestByteOrder());
        const MPTE52 mpte(raw);

        if (!mpte.isValid()) {
            return false;
        }
        if (mpte.isLeaf()) {
            const uint8_t pi =
                (paddr >> getPageShiftForLevel(level)) & 0xf;
            return mptPermAllows(
                effectiveMptPerm(mpte.perms(pi)), mode);
        }
        if (level == 0) {
            return false;
        }
        base = mpte.nextLevelPAddr();
    }

    return false;
}



#endif // MPT_ENABLED

#if MPT_CACHE_ENABLED

MPTCache52::MPTCache52(size_t capL0, size_t capL1, size_t capL2, size_t capL3, size_t capSP)
    : capacityL0(capL0),
      capacityL1(capL1),
      capacityL2(capL2),
      capacityL3(capL3),
      capacitySP(capSP),
      plruL0(capL0),
      plruL1(capL1),
      plruL2(capL2),
      plruL3(capL3),
      plruSP(capSP)
{
    tagListL0.reserve(capL0);
    tagListL1.reserve(capL1);
    tagListL2.reserve(capL2);
    tagListL3.reserve(capL3);
    tagListSP.reserve(capSP);
}

MPTCache52::MPTCache52()
    : capacityL0(configuredSizeL0),
      capacityL1(configuredSizeL1),
      capacityL2(configuredSizeL2),
      capacityL3(configuredSizeL3),
      capacitySP(configuredSizeSP),
      plruL0(configuredSizeL0),
      plruL1(configuredSizeL1),
      plruL2(configuredSizeL2),
      plruL3(configuredSizeL3),
      plruSP(configuredSizeSP)
{
    tagListL0.reserve(capacityL0);
    tagListL1.reserve(capacityL1);
    tagListL2.reserve(capacityL2);
    tagListL3.reserve(capacityL3);
    tagListSP.reserve(capacitySP);
}


void MPTCache52::configureSize(int sL0, int sL1, int sL2, int sL3, int sSP) {
    configuredSizeL0 = sL0;
    configuredSizeL1 = sL1;
    configuredSizeL2 = sL2;
    configuredSizeL3 = sL3;
    configuredSizeSP = sSP;
}

Addr MPTCache52::regionAlign(Addr pa, int level) const {
    return pa & ~(getRegionSizeForLevel(level) - 1);
}

// non-const version
std::unordered_map<Addr, MPTCacheEntry>& MPTCache52::getTableByLevel(int level) {
    if (level == 0) return tableL0;
    else if (level == 1) return tableL1;
    else if (level == 2) return tableL2;
    else if (level == 3) return tableL3;
    else return tableSP;
}

// const : read only
const std::unordered_map<Addr, MPTCacheEntry>& MPTCache52::getTableByLevel(int level) const {
    if (level == 0) return tableL0;
    else if (level == 1) return tableL1;
    else if (level == 2) return tableL2;
    else if (level == 3) return tableL3;
    else return tableSP;
}


size_t& MPTCache52::getCapacityByLevel(int level) {
    if (level == 0) return capacityL0;
    else if (level == 1) return capacityL1;
    else if (level == 2) return capacityL2;
    else if (level == 3) return capacityL3;
    else return capacitySP;
}


size_t MPTCache52::getCapacityByLevel(int level) const {
    if (level == 0) return capacityL0;
    else if (level == 1) return capacityL1;
    else if (level == 2) return capacityL2;
    else if (level == 3) return capacityL3;
    else return capacitySP;
}

// -------- Supports PLRU (Pseudo-LRU) replacement. --------
std::vector<Addr>& MPTCache52::getTagListByLevel(int level) {
    if (level == 0) return tagListL0;
    else if (level == 1) return tagListL1;
    else if (level == 2) return tagListL2;
    else if (level == 3) return tagListL3;
    else return tagListSP;
}

PLRUTreeN& MPTCache52::getPLRUByLevel(int level) {
    if (level == 0) return plruL0;
    else if (level == 1) return plruL1;
    else if (level == 2) return plruL2;
    else if (level == 3) return plruL3;
    else return plruSP;
}

const std::vector<Addr>& MPTCache52::getTagListByLevel(int level) const {
    if (level == 0) return tagListL0;
    else if (level == 1) return tagListL1;
    else if (level == 2) return tagListL2;
    else if (level == 3) return tagListL3;
    else return tagListSP;
}

const PLRUTreeN& MPTCache52::getPLRUByLevel(int level) const {
    if (level == 0) return plruL0;
    else if (level == 1) return plruL1;
    else if (level == 2) return plruL2;
    else if (level == 3) return plruL3;
    else return plruSP;
}

bool
MPTCache52::insertOrRefreshEntry(int level, Addr aligned,
                                 const MPTCacheEntry &entry)
{
    auto& table_mut = getTableByLevel(level);
    size_t& cap = getCapacityByLevel(level);
    auto& tagList = getTagListByLevel(level);
    auto& plru = getPLRUByLevel(level);

    assert(cap > 0);

    auto existing = table_mut.find(aligned);
    if (existing != table_mut.end()) {
        existing->second = entry;
        auto itTag = std::find(tagList.begin(), tagList.end(), aligned);
        if (itTag != tagList.end()) {
            size_t idx = std::distance(tagList.begin(), itTag);
            plru.access(idx);
        } else {
            warn("MPTCache table/tagList mismatch level %d tag %#lx\n",
                 level, aligned);
            if (tagList.size() < cap) {
                tagList.push_back(aligned);
                plru.access(tagList.size() - 1);
            }
        }
        DPRINTF(PageTableWalker,
                "MPT cache refill duplicate/update level %d tag %#lx\n",
                level, aligned);
        return false;
    }

    if (table_mut.size() >= cap) {
        size_t victimIdx = plru.getVictim();
        assert(victimIdx < tagList.size());
        Addr victimAddr = tagList[victimIdx];
        table_mut.erase(victimAddr);
        tagList[victimIdx] = aligned;
        plru.access(victimIdx);
    } else {
        tagList.push_back(aligned);
        plru.access(tagList.size() - 1);
    }
    table_mut[aligned] = entry;
    DPRINTF(PageTableWalker, "MPT cache refill insert level %d tag %#lx\n",
            level, aligned);
    return true;
}

//int MPTCache52::configuredSize = MPT_CACHE_SIZE;
int MPTCache52::configuredSizeL0 = 8;
int MPTCache52::configuredSizeL1 = 8;
int MPTCache52::configuredSizeL2 = 8;
int MPTCache52::configuredSizeL3 = 8;
int MPTCache52::configuredSizeSP = 8;

//MPTCache52* globalMPTCache = nullptr;


// Called during SimObject initialization to complete the construction.
void MPTCache52::initMPTCacheFromParams(const RiscvTLBParams *params )
{
    // MPTCache52::configureSize(params->mptcache_size);
    // globalMPTCache = new MPTCache52();
    MPTCache52::configureSize(
        params->mptcache_l0_size,
        params->mptcache_l1_size,
        params->mptcache_l2_size,
        params->mptcache_l3_size,
        params->mptcache_sp_size
    );
    globalMPTCache = new MPTCache52();
    globalMPT.walker = params->walker;

    DPRINTF(PageTableWalker,
            "Initialized globalMPTCache with size = L0:%d L1:%d "
            "L2:%d L3:%d SP:%d\n",
            params->mptcache_l0_size,
            params->mptcache_l1_size,
            params->mptcache_l2_size,
            params->mptcache_l3_size,
            params->mptcache_sp_size);
}

std::pair<bool /*hit*/, MPTCacheEntry>
MPTCache52::fetchDelayed(Addr pa, ThreadContext *tc, PMAChecker *pma,
                         PMP *pmp, int& mptlevel,
                         Walker::WalkerState* senderState)
{
    bool hasLeafHit = false;
    int leafHitLevel = -1;
    bool leafHitFromSP = false;
    MPTCacheEntry leafHitEntry;

    bool hasInternalHit = false;
    int internalHitLevel = 4;
    MPTCacheEntry internalHitEntry;

    auto recordPLRUAccess = [this](int level, Addr aligned) {
        auto& tagList = this->getTagListByLevel(level);
        auto& plru = this->getPLRUByLevel(level);
        auto itTag = std::find(tagList.begin(), tagList.end(), aligned);
        if (itTag != tagList.end()) {
            size_t idx = std::distance(tagList.begin(), itTag);
            plru.access(idx);
        }
    };

    auto recordLevelHit = [](int level) {
        if (level == 0) ++globalMPTCache->mptCacheL0Hits;
        else if (level == 1) ++globalMPTCache->mptCacheL1Hits;
        else if (level == 2) ++globalMPTCache->mptCacheL2Hits;
        else if (level == 3) ++globalMPTCache->mptCacheL3Hits;
    };

    auto considerHit =
        [&](const MPTCacheEntry &entry, int level, bool fromSP) {
            if (!entry.valid) {
                DPRINTF(PageTableWalker,
                        "MPT cache invalid entry hit level %i paddr %#lx\n",
                        level, pa);
                return;
            }

            if (entry.mpte.isLeaf()) {
                DPRINTF(PageTableWalker,
                        "MPT cache hit leaf level %i paddr %#lx%s\n",
                        level, pa, fromSP ? " [SP]" : "");
                if (!hasLeafHit || level > leafHitLevel) {
                    leafHitEntry = entry;
                    leafHitLevel = level;
                    leafHitFromSP = fromSP;
                    hasLeafHit = true;
                }
            } else {
                DPRINTF(PageTableWalker,
                        "MPT cache hit internal level %i paddr %#lx%s\n",
                        level, pa, fromSP ? " [SP]" : "");
                if (!hasInternalHit || level < internalHitLevel) {
                    internalHitEntry = entry;
                    internalHitLevel = level;
                    hasInternalHit = true;
                }
            }
        };

    auto& sptable = getTableByLevel(MPT_CACHE_SP_LEVEL);
    for (int i = 0; i <= 3; i++) {
        Addr aligned = regionAlign(pa, i);
        auto& leveltable = getTableByLevel(i);
        auto it = leveltable.find(aligned);
        if (it != leveltable.end()) {
            recordLevelHit(i);
            recordPLRUAccess(i, aligned);
            considerHit(it->second, i, false);
        }

        Addr spKey = mptSPCacheKey(aligned, i);
        auto spit = sptable.find(spKey);
        if (spit != sptable.end() && spit->second.level == i) {
            ++globalMPTCache->mptCacheSPHits;
            recordPLRUAccess(MPT_CACHE_SP_LEVEL, spKey);
            considerHit(spit->second, i, true);
        }
    }

    if (hasLeafHit) {
        if (leafHitLevel > 0 && !leafHitFromSP) {
            ++globalMPTCache->mptCacheSPHits;
        }
        mptlevel = leafHitLevel;
        DPRINTF(PageTableWalker, "mptcache leaf hit,hitlevel=%i\n",
                leafHitLevel);
        return std::make_pair(true, leafHitEntry);
    }

    Addr baseaddr;
    if (hasInternalHit) {
        if (internalHitLevel == 0) {
            DPRINTF(PageTableWalker,
                    "MPT cache hit invalid level0 internal entry for pa %#lx\n",
                    pa);
            return std::make_pair(false, internalHitEntry);
        }
        mptlevel = internalHitLevel - 1;
        baseaddr = internalHitEntry.mpte.nextLevelPAddr();
        DPRINTF(PageTableWalker,
                "MPT table walk resumes after internal cache hit at level %i, start level %i for pa %#lx\n",
                internalHitLevel, mptlevel, pa);
        globalMPT.walk(mptlevel, baseaddr, pa, tc, pma, pmp, senderState);
        return std::make_pair(false, internalHitEntry);
    }

    mptlevel = 3;
    baseaddr = globalMPT.mmpt.ppn << 12;
    DPRINTF(PageTableWalker,
            "MPT table walk start at root level %i for pa %#lx\n",
            mptlevel, pa);
    globalMPT.walk(mptlevel, baseaddr, pa, tc, pma, pmp, senderState);
    return std::make_pair(false, MPTCacheEntry());
}


void MPTCache52::mfence_all(){

    tableL0.clear();
    tableL1.clear();
    tableL2.clear();
    tableL3.clear();
    tableSP.clear();

    tagListL0.clear();
    tagListL1.clear();
    tagListL2.clear();
    tagListL3.clear();
    tagListSP.clear();

    plruL0.reset();
    plruL1.reset();
    plruL2.reset();
    plruL3.reset();
    plruSP.reset();

}


#endif//MPT_CACHE_ENABLED

inline int Walker::getLevelForPageSizeLog2(uint8_t logBytes) {
    switch (logBytes) {
        case 12: return 0; // 4KB
        case 21: return 1; // 2MB
        case 30: return 2; // 1GB
        case 39: return 3; // 512GB
        default: return -1;
    }
}

#if MPT_ENABLED
Fault Walker::createMPTPagefault(Addr vaddr, Addr paForMPTCheck, BaseMMU::Mode mode)
{
    ExceptionCode code;
    switch (mode) {
        case BaseMMU::Read:
            code = ExceptionCode::LOAD_ACCESS;
            break;
        case BaseMMU::Write:
            code = ExceptionCode::STORE_ACCESS;
            break;
        case BaseMMU::Execute:
            code = ExceptionCode::INST_ACCESS;
            break;
        default:
            panic("Unsupported memory mode for MPT page fault");
    }

    DPRINTF(PageTableWalker, "Create MPT page fault #%i on vaddr=%#lx paForMPTCheck=%#lx\n",
            code, vaddr, paForMPTCheck);

    return std::make_shared<AddressFault>(vaddr, paForMPTCheck, code);
}

#endif

Walker::WalkerStats::WalkerStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(ptwMemCount, statistics::units::Count::get(),
               "Number of PTW memory requests sent"),
      ADD_STAT(ptwMemCycle, statistics::units::Cycle::get(),
               "Cycles with at least one PTW memory request in flight"),
      ADD_STAT(ptwAvgMemLatency,
               statistics::units::Rate<
                   statistics::units::Cycle,
                   statistics::units::Count>::get(),
               "Average PTW memory latency",
               ptwMemCycle / ptwMemCount)
{
}

bool
Walker::ptwLevelAvailable(WalkerState *state, int level) const
{
    if (!enablePtwLevelLimit || !state->usePtwLevelLimit())
        return true;

    panic_if(level < 0 || level >= static_cast<int>(ptwLevelLimit.size()),
             "Invalid PTW level %d\n", level);
    panic_if(ptwLevelLimit[level] == 0,
             "PTW level %d limit must be positive when enabled\n", level);
    return state->reservedPtwLevel == level ||
           ptwLevelActive[level] < ptwLevelLimit[level];
}

bool
Walker::reservePtwLevel(WalkerState *state, int level)
{
    if (!enablePtwLevelLimit || !state->usePtwLevelLimit())
        return true;

    panic_if(level < 0 || level >= static_cast<int>(ptwLevelLimit.size()),
             "Invalid PTW level %d\n", level);
    panic_if(ptwLevelLimit[level] == 0,
             "PTW level %d limit must be positive when enabled\n", level);

    if (state->reservedPtwLevel == level)
        return true;

    releasePtwLevel(state);
    if (ptwLevelActive[level] >= ptwLevelLimit[level])
        return false;

    ptwLevelActive[level]++;
    state->reservedPtwLevel = level;
    return true;
}

void
Walker::releasePtwLevel(WalkerState *state)
{
    if (!enablePtwLevelLimit || state->reservedPtwLevel < 0)
        return;

    const int level = state->reservedPtwLevel;
    panic_if(level >= static_cast<int>(ptwLevelActive.size()),
             "Invalid reserved PTW level %d\n", level);
    panic_if(ptwLevelActive[level] == 0,
             "PTW level %d active counter underflow\n", level);
    ptwLevelActive[level]--;
    state->reservedPtwLevel = -1;
}

void
Walker::retryPtwLevelBlockedStates()
{
    if (!enablePtwLevelLimit)
        return;

    for (auto *walker_state : currStates) {
        if (walker_state->retryBlockedPtwLevel())
            break;
    }
}

bool
Walker::usePtwLevelLimitForStart(bool from_forward_pre_req,
                                 bool from_back_pre_req,
                                 bool is_prefetch) const
{
    return enablePtwLevelLimit && !from_forward_pre_req &&
           !from_back_pre_req && !is_prefetch;
}

bool
Walker::canStartPtwLevel(int level, bool from_forward_pre_req,
                         bool from_back_pre_req, bool is_prefetch)
{
    if (!usePtwLevelLimitForStart(from_forward_pre_req, from_back_pre_req,
                                  is_prefetch))
        return true;

    panic_if(level < 0 || level >= static_cast<int>(ptwLevelLimit.size()),
             "Invalid PTW level %d\n", level);
    panic_if(ptwLevelLimit[level] == 0,
             "PTW level %d limit must be positive when enabled\n", level);
    if (ptwLevelActive[level] < ptwLevelLimit[level])
        return true;

    return false;
}

bool
Walker::ptwMissQueueHintMatch(const MissQueueEntry &entry,
                              const TlbEntry &refill_entry,
                              uint8_t translateMode) const
{
    if (!entry.tc || !entry.req)
        return false;
    return tlb->refillHintMaySatisfy(entry.req, entry.tc, entry.mode,
                                     refill_entry, translateMode);
}

void
Walker::notifyTlbRefillHint(const TlbEntry &entry, uint8_t translateMode)
{
    if (translateMode != direct && translateMode != allstage)
        return;
    if (translateMode == allstage && !entry.pte.r && !entry.pte.x)
        return;

    if (!enablePtwLevelLimit || retryingPtwMissQueue ||
        processingPtwMissQueueHint || ptwMissQueue.empty())
        return;

    if (translateMode == allstage) {
        MissQueueEntry mq_entry = ptwMissQueue.front();
        if (!ptwMissQueueHintMatch(mq_entry, entry, translateMode))
            return;

        ptwMissQueue.pop_front();

        processingPtwMissQueueHint = true;
        tlb->retryTimingPtwMiss(mq_entry.tc, mq_entry.translation,
                                mq_entry.req, mq_entry.mode, true);
        processingPtwMissQueueHint = false;
        return;
    }

    std::deque<MissQueueEntry> remaining;
    std::deque<MissQueueEntry> matched;
    processingPtwMissQueueHint = true;

    while (!ptwMissQueue.empty()) {
        MissQueueEntry mq_entry = ptwMissQueue.front();
        ptwMissQueue.pop_front();
        if (ptwMissQueueHintMatch(mq_entry, entry, translateMode)) {
            matched.push_back(mq_entry);
        } else {
            remaining.push_back(mq_entry);
        }
    }

    ptwMissQueue.swap(remaining);

    while (!matched.empty()) {
        MissQueueEntry mq_entry = matched.front();
        matched.pop_front();
        tlb->retryTimingPtwMiss(mq_entry.tc, mq_entry.translation,
                                mq_entry.req, mq_entry.mode, true);
    }

    processingPtwMissQueueHint = false;
}

bool
Walker::enqueuePtwMiss(ThreadContext *tc, BaseMMU::Translation *translation,
                       const RequestPtr &req, BaseMMU::Mode mode, bool front)
{
    if (!enablePtwLevelLimit)
        return false;

    MissQueueEntry entry;
    entry.tc = tc;
    entry.translation = translation;
    entry.req = req;
    entry.mode = mode;

    if (!front && ptwMissQueue.size() >= ptwMissQueueSize) {
        ptwMissQueueWaiters.push_back(entry);
        DPRINTF(PageTableWalker,
                "PTW MissQueue full, hold vaddr %#lx waiter size %u\n",
                req->getVaddr(), ptwMissQueueWaiters.size());
        return true;
    }

    if (front) {
        ptwMissQueue.push_front(entry);
        ptwMissQueueHeadRequeued = true;
    } else {
        ptwMissQueue.push_back(entry);
    }
    DPRINTF(PageTableWalker,
            "Enqueue PTW miss vaddr %#lx queue size %u\n",
            req->getVaddr(), ptwMissQueue.size());
    return true;
}

void
Walker::retryPtwMissQueue()
{
    if (!enablePtwLevelLimit || retryingPtwMissQueue)
        return;

    while (!ptwMissQueueWaiters.empty() &&
           ptwMissQueue.size() < ptwMissQueueSize) {
        ptwMissQueue.push_back(ptwMissQueueWaiters.front());
        ptwMissQueueWaiters.pop_front();
    }
    if (ptwMissQueue.empty())
        return;

    retryingPtwMissQueue = true;
    while (!ptwMissQueue.empty()) {
        const size_t size_before = ptwMissQueue.size();
        ptwMissQueueHeadRequeued = false;
        MissQueueEntry entry = ptwMissQueue.front();
        ptwMissQueue.pop_front();
        DPRINTF(PageTableWalker,
                "Dequeue PTW miss vaddr %#lx queue size %u\n",
                entry.req->getVaddr(), ptwMissQueue.size());
        tlb->retryTimingPtwMiss(entry.tc, entry.translation, entry.req, entry.mode);
        if (ptwMissQueueHeadRequeued ||
            ptwMissQueue.size() >= size_before)
            break;
        while (!ptwMissQueueWaiters.empty() &&
               ptwMissQueue.size() < ptwMissQueueSize) {
            ptwMissQueue.push_back(ptwMissQueueWaiters.front());
            ptwMissQueueWaiters.pop_front();
        }
    }
    retryingPtwMissQueue = false;
}

void
Walker::updatePtwMemCycleStats()
{
    const Tick now = curTick();
    if (outstandingPtwMemReqs != 0 && now > lastPtwMemCycleTick) {
        stats.ptwMemCycle += ticksToCycles(now - lastPtwMemCycleTick);
    }
    lastPtwMemCycleTick = now;
}

void
Walker::preDumpStats()
{
    ClockedObject::preDumpStats();
    updatePtwMemCycleStats();
}

void
Walker::resetStats()
{
    ClockedObject::resetStats();
    lastPtwMemCycleTick = curTick();
}

std::pair<bool, Fault>
Walker::tryCoalesce(ThreadContext *_tc, BaseMMU::Translation *translation,
                    const RequestPtr &req, BaseMMU::Mode mode, bool from_l2tlb,
                    Addr asid, bool from_forward_pre_req, bool from_back_pre_req)
{
    assert(currStates.size());
    for (auto it: currStates) {
        auto &ws = *it;
        auto [coalesced, fault] =
            ws.tryCoalesce(_tc, translation, req, mode, from_l2tlb, asid, from_forward_pre_req, from_back_pre_req);
        if (coalesced) {
            return std::make_pair(true, fault);
        }
    }
    DPRINTF(PageTableWalker, "Coalescing failed on Addr %#lx (pc=%#lx)\n",
            req->getVaddr(), req->getPC());
    return std::make_pair(false, NoFault);
}

Fault
Walker::start(Addr ppn, ThreadContext *_tc, BaseMMU::Translation *_translation,
              const RequestPtr &_req, BaseMMU::Mode _mode, bool from_forward_pre_req,
              bool from_back_pre_req, int f_level, bool from_l2tlb,
              Addr asid)
{
    // TODO: in timing mode, instead of blocking when there are other
    // outstanding requests, see if this request can be coalesced with
    // another one (i.e. either coalesce or start walk)
    DPRINTF(PageTableWalker, "Starting page table walk for %#lx\n",
            _req->getVaddr());
    DPRINTF(PageTableWalker, "from_pre_req %d f_level %d from_l2tlb %d\n", from_forward_pre_req, f_level, from_l2tlb);

    if (autoOpenNextLine) {
        auto regulate = tlb->autoOpenNextline();
        if (!regulate)
            autoOpenNextLine = false;
    }
    if (currStates.size()) {
        auto [coalesced, fault] =
            tryCoalesce(_tc, _translation, _req, _mode, from_l2tlb, asid, from_forward_pre_req, from_back_pre_req);
        if (!coalesced) {
            // create state
            WalkerState *newState = new WalkerState(this, _translation, _req);
            newState->initState(_tc, _req, _mode, sys->isTimingMode(), from_forward_pre_req, from_back_pre_req);
            assert(newState->isTiming());
            // TODO: add to requestors
            DPRINTF(PageTableWalker,
                    "Walks in progress: %d, push req pc: %#lx, addr: %#lx "
                    "into currStates\n",
                    currStates.size(), _req->getPC(), _req->getVaddr());
            currStates.push_back(newState);
            Fault fault = newState->startWalk(ppn, f_level, from_l2tlb, openNextLine, autoOpenNextLine,
                                              from_forward_pre_req, from_back_pre_req);
            bool is_timing = newState->isTiming();
            if (fault != NoFault && is_timing) {
                currStates.remove(newState);
                delete newState;
            }
            if (!is_timing) {
                assert(0);
            }
            return fault;
        } else {
            DPRINTF(PageTableWalker,
                    "Walks in progress: %d. Coalesce req pc: %#lx, addr: %#lx "
                    "into currStates\n",
                    currStates.size(), _req->getPC(), _req->getVaddr());
            return fault;
        }
    } else {
        WalkerState *newState = new WalkerState(this, _translation, _req);
        newState->initState(_tc, _req, _mode, sys->isTimingMode(), from_forward_pre_req, from_back_pre_req);
        currStates.push_back(newState);
        Fault fault = newState->startWalk(ppn, f_level, from_l2tlb, openNextLine, autoOpenNextLine,
                                          from_forward_pre_req, from_back_pre_req);
        bool is_timing = newState->isTiming();
        if (fault != NoFault && is_timing) {
            currStates.remove(newState);
            delete newState;
        } else if (!is_timing) {
            currStates.pop_front();
            delete newState;
        }
        return fault;
    }
}

Fault
Walker::doL2TLBHitSchedule(const RequestPtr &req, ThreadContext *tc, BaseMMU::Translation *translation,
                           BaseMMU::Mode mode, Addr Paddr, TlbEntry *entry, TlbEntry *entryVsstage,
                           TlbEntry *entryGstage)
{
    const bool hasUsableMptInfo =
        entry != nullptr && entry->mptInfo.valid &&
        mptLevelCoversLogBytes(entry->mptInfo.mptlevel, entry->logBytes);
    const bool needsMptCheck = globalMPT.mmpt.mode != 0 &&
        (!tlb->isMptTlbInfoEnabled() || !hasUsableMptInfo);

    if (translation == nullptr) {
        req->setPaddr(Paddr);
        return needsMptCheck ?
            checkMPTFunctional(req->getVaddr(), Paddr, mode) : NoFault;
    }

    if (needsMptCheck) {
        startMPTCheck(req, tc, translation, mode, Paddr, entry,
                      entryVsstage, entryGstage);
        return NoFault;
    }

    DPRINTF(PageTableWalker2, "schedule %d\n", curCycle());
    L2TlbState l2state;
    l2state.req = req;
    l2state.tc = tc;
    l2state.translation = translation;
    l2state.mode = mode;
    l2state.Paddr = Paddr;
    l2state.entry = entry;
    l2state.entryVsstage = entryVsstage;
    l2state.entryGstage = entryGstage;
    L2TLBrequestors.push_back(l2state);
    if (!doL2TLBHitEvent.scheduled()) {
        schedule(doL2TLBHitEvent, curTick());
    }
    return NoFault;
}

void
Walker::startMPTCheck(const RequestPtr &req, ThreadContext *tc,
                      BaseMMU::Translation *translation, BaseMMU::Mode mode,
                      Addr paddr, const TlbEntry *entry,
                      const TlbEntry *entryVsstage,
                      const TlbEntry *entryGstage)
{
    panic_if(translation == nullptr,
             "Timing MPT-only check requires a translation object\n");

    WalkerState *state = new WalkerState(this, translation, req);
    state->initState(tc, req, mode, sys->isTimingMode(), false, false);
    panic_if(!state->isTiming(),
             "MPT-only check started outside timing mode\n");

    state->mptOnly = true;
    state->mptCheckPaddr = paddr;
    state->mptCheckMode = mode;
    state->mptFaultMode = mode;
    state->PaddrUT = paddr;
    state->isMPTing = true;
    state->finishMPTing = false;
    state->mpt_level = 3;
    state->read = nullptr;

    if (entry != nullptr) {
        state->mptOnlyEntry = *entry;
        state->mptOnlyHasEntry = true;
    }
    if (entryVsstage != nullptr) {
        state->mptOnlyVsstageEntry = *entryVsstage;
        state->mptOnlyHasVsstageEntry = true;
    }
    if (entryGstage != nullptr) {
        state->mptOnlyGstageEntry = *entryGstage;
        state->mptOnlyHasGstageEntry = true;
    }

    currStates.push_back(state);
    DPRINTF(PageTableWalker,
            "Start MPT-only check vaddr %#lx paddr %#lx pc %#lx\n",
            req->getVaddr(), paddr, req->getPC());

    if (state->startMPTwalk()) {
        state->finishMPTing = true;
        state->scheduleMptCacheHit();
    }
}

Fault
Walker::checkMPTFunctional(Addr vaddr, Addr paddr, BaseMMU::Mode mode)
{
    if (globalMPT.mmpt.mode == 0 ||
        globalMPT.checkFunctional(paddr, mode, sys)) {
        return NoFault;
    }
    return createMPTPagefault(vaddr, paddr, mode);
}

Fault
Walker::startFunctional(RequestPtr req, ThreadContext * _tc, Addr &addr, unsigned &logBytes,
              BaseMMU::Mode _mode)
{
    funcState.initState(_tc, req, _mode);
    return funcState.startFunctional(addr, logBytes, openNextLine,
                                     autoOpenNextLine, false, false);
}

bool
Walker::WalkerPort::recvTimingResp(PacketPtr pkt)
{
    return walker->recvTimingResp(pkt);
}

bool
Walker::recvTimingResp(PacketPtr pkt)
{

    if (pkt->req->isMptWalk()) {
        DPRINTF(PageTableWalker,"hi mpt resp");
        return  globalMPT.MptRecvTimingResp(pkt);
    }
    WalkerSenderState * senderState =
        dynamic_cast<WalkerSenderState *>(pkt->popSenderState());
    DPRINTF(PageTableWalker,
            "Received timing response for sender state: %#lx\n", senderState);
    if (pkt->isRead()) {
        updatePtwMemCycleStats();
        assert(outstandingPtwMemReqs > 0);
        outstandingPtwMemReqs--;
    }
    WalkerState * senderWalk = senderState->senderWalk;
    bool walkComplete = senderWalk->recvPacket(pkt);
    delete senderState;
    if (walkComplete) {
        std::list<WalkerState *>::iterator iter;
        for (iter = currStates.begin(); iter != currStates.end(); iter++) {
            WalkerState * walkerState = *(iter);
            if (walkerState == senderWalk) {
                DPRINTF(PageTableWalker,
                        "Walk complete for %#lx (pc=%#lx), erase it\n",
                        senderWalk->mainReq->getVaddr(), senderWalk->mainReq->getPC());
                iter = currStates.erase(iter);
                break;
            }
        }
        releasePtwLevel(senderWalk);
        delete senderWalk;
    }
    retryPtwLevelBlockedStates();
    retryPtwMissQueue();
    return true;
}

void
Walker::WalkerPort::recvReqRetry()
{
    walker->recvReqRetry();
}

void
Walker::recvReqRetry()
{
    std::list<WalkerState *>::iterator iter;
    for (iter = currStates.begin(); iter != currStates.end(); iter++) {
        WalkerState * walkerState = *(iter);
        if (walkerState->isRetrying()) {
            walkerState->retry();
        }
    }
    if (globalMPT.mptretry){ //retry mpt
        globalMPT.sendMptPacket();
    }
}

bool Walker::sendTiming(WalkerState* sendingState, PacketPtr pkt)
{
    WalkerSenderState* walker_state = new WalkerSenderState(sendingState);
    DPRINTF(PageTableWalker, "Sending packet %#x with sender state: %lx\n",
            pkt->getAddr(), walker_state);
    pkt->pushSenderState(walker_state);
    if (port.sendTimingReq(pkt)) {
        if (pkt->isRead()) {
            updatePtwMemCycleStats();
            outstandingPtwMemReqs++;
            stats.ptwMemCount++;
        }
        return true;
    } else {
        // undo the adding of the sender state and delete it, as we
        // will do it again the next time we attempt to send it
        pkt->popSenderState();
        delete walker_state;
        return false;
    }

}

Port &
Walker::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "port")
        return port;
    else
        return ClockedObject::getPort(if_name, idx);
}

Walker::WalkerState::~WalkerState()
{
    if (mptCacheHitEvent != nullptr) {
        if (mptCacheHitEvent->scheduled())
            walker->deschedule(*mptCacheHitEvent);
        delete mptCacheHitEvent;
    }
}

void
Walker::WalkerState::initState(ThreadContext *_tc, const RequestPtr &_req, BaseMMU::Mode _mode, bool _isTiming,
                               bool _from_forward_pre_req, bool _from_back_pre_req)
{
    assert(functional || _req != nullptr);
    if (_req && _req->get_two_stage_state()) {
        assert(state == Ready);
        started = false;
        assert(functional || requestors.back().tc == nullptr);
        requestors.back().tc = _tc;
        requestors.back().fromForwardPreReq = false;
        requestors.back().fromBackPreReq = false;
        if (functional) {
            mainReq = _req;
            mainFault = NoFault;
            requestors.back().req = _req;
        }
        mode = _mode;
        timing = _isTiming;
        status = _tc->readMiscReg(MISCREG_STATUS);
        vsstatus = _tc->readMiscReg(MISCREG_VSSTATUS);
        pmode = (PrivilegeMode)(RegVal)_req->get_twoStageTranslateMode();
        satp = 0;
        vsatp = _tc->readMiscReg(MISCREG_VSATP);
        fromPre = false;
        fromBackPre = false;
        translateMode = twoStageMode;
        hgatp = _tc->readMiscReg(MISCREG_HGATP);
        isHInst = _req->get_h_inst();
        isVsatp0Mode = _req->get_vsatp_0_mode();
        virt = _req->get_virt();
        GstageFault = false;
        tlbHit = false;
        isMPTing = false;
        finishMPTing = false;
        MPTresult = false;
        mpt_level = 3;
        mptInfo = MPTInfoInTLB();
        PaddrUT = 0;
        mptCheckPaddr = 0;
        mptCheckMode = BaseMMU::Read;
        mptFaultMode = _mode;
        mptCheckingPteRead = false;
        pteReadMptResult = false;
        pteReadMptChecked = false;
        pteReadMptIsNextline = false;
        pteReadMptFaultPending = false;
        pteReadMptCheckedPaddr = 0;
        mptGranularityClipped = false;
        mptOnly = false;
        mptOnlyHasEntry = false;
        mptOnlyHasVsstageEntry = false;
        mptOnlyHasGstageEntry = false;
        DPRINTF(PageTableWalker, "WalkerState::initState for req %#x (vaddr %#x):\n", _req, _req->getVaddr());
        DPRINTFR(PageTableWalker, "\tvsatp %#x(mode: %d, asid: %#x, ppn:%#x)\n",
                 vsatp, vsatp >> 60, (vsatp >> 44) & 0xffff, vsatp & 0xfffffffffff);
        DPRINTFR(PageTableWalker, "\thgatp %#x(mode: %d, vmid: %#x, ppn:%#x)\n",
                 hgatp, hgatp >> 60, (hgatp >> 44) & 0xffff, hgatp & 0xfffffffffff);
    } else {    // 1-stage state
        assert(state == Ready);
        started = false;
        assert(functional || requestors.back().tc == nullptr);
        requestors.back().tc = _tc;
        requestors.back().fromForwardPreReq = _from_forward_pre_req;
        requestors.back().fromBackPreReq = _from_back_pre_req;
        if (functional) {
            mainReq = _req;
            mainFault = NoFault;
            requestors.back().req = _req;
        }
        mode = _mode;
        timing = _isTiming;
        // fetch these now in case they change during the walk
        status = _tc->readMiscReg(MISCREG_STATUS);
        vsstatus = _tc->readMiscReg(MISCREG_VSSTATUS);
        pmode = walker->tlb->getMemPriv(_tc, mode);
        satp = _tc->readMiscReg(MISCREG_SATP);
        vsatp = 0;
        assert(satp.mode == AddrXlateMode::SV39 || satp.mode == AddrXlateMode::SV48);
        mainReq->setLevel(PTW_TOP_LEVEL(satp.mode));
        fromPre = _from_forward_pre_req;
        fromBackPre = _from_back_pre_req;
        translateMode = defaultmode;
        hgatp = _tc->readMiscReg(MISCREG_HGATP);
        isHInst = false;
        isVsatp0Mode = false;
        GstageFault = false;
        tlbHit = false;
        isMPTing = false;
        finishMPTing = false;
        MPTresult = false;
        mpt_level = 3;
        mptInfo = MPTInfoInTLB();
        PaddrUT = 0;
        mptCheckPaddr = 0;
        mptCheckMode = BaseMMU::Read;
        mptFaultMode = _mode;
        mptCheckingPteRead = false;
        pteReadMptResult = false;
        pteReadMptChecked = false;
        pteReadMptIsNextline = false;
        pteReadMptFaultPending = false;
        pteReadMptCheckedPaddr = 0;
        mptGranularityClipped = false;
        mptOnly = false;
        mptOnlyHasEntry = false;
        mptOnlyHasVsstageEntry = false;
        mptOnlyHasGstageEntry = false;
        assert(functional || !_req->get_h_inst());
    }
}


std::pair<bool, Fault>
Walker::WalkerState::tryCoalesce(ThreadContext *_tc, BaseMMU::Translation *translation, const RequestPtr &req,
                                 BaseMMU::Mode _mode, bool from_l2tlb, Addr asid, bool from_forward_pre_req,
                                 bool from_back_pre_req)
{
    if (mptOnly) {
        return std::make_pair(false, NoFault);
    }

    SATP _satp = _tc->readMiscReg(MISCREG_SATP);
    bool priv_match;
    if (from_l2tlb) {
        priv_match = mode == _mode && satp == _satp &&
                     pmode == walker->tlb->getMemPriv(_tc, _mode) &&
                     status == _tc->readMiscReg(MISCREG_STATUS) &&
                     satp.asid == asid;

    } else {
        priv_match = mode == _mode && satp == _satp &&
                     pmode == walker->tlb->getMemPriv(_tc, _mode) &&
                     status == _tc->readMiscReg(MISCREG_STATUS);
    }

    bool addr_match;
    Addr addr_match_num;
    Addr pre_match_num;
    bool model_match;
    model_match = (mainReq->get_two_stage_state() == req->get_two_stage_state()) &&
                  (mainReq->get_virt() == req->get_virt()) &&
                  (mainReq->get_twoStageTranslateMode() == req->get_twoStageTranslateMode()) &&
                  (mainReq->get_vsatp_0_mode() == req->get_vsatp_0_mode());
    if (fromPre) {
        addr_match_num = mainReq->getForwardPreVaddr();
    } else if (fromBackPre) {
        addr_match_num = mainReq->getBackPreVaddr();
    } else {
        addr_match_num = mainReq->getVaddr();
    }


    if (from_back_pre_req) {
        pre_match_num = req->getBackPreVaddr();
    } else if (from_forward_pre_req) {
        pre_match_num = req->getForwardPreVaddr();
    } else {
        pre_match_num = req->getVaddr();
    }
    addr_match = ((pre_match_num >> PageShift) << PageShift) ==
                 ((addr_match_num >> PageShift) << PageShift);


    DPRINTF(PageTableWalker, "try to coalesce: priv_match %d addr_match %d finishDefaultTranslate %d model_match %d\n",
            priv_match, addr_match, finishDefaultTranslate, model_match);
    if (priv_match && addr_match && (!finishDefaultTranslate) && model_match) {
        // coalesce
        if (from_forward_pre_req || from_back_pre_req) {
            DPRINTF(PageTableWalker, "from_forward_pre_req be coalesced\n");
            return std::make_pair(true, NoFault);

        } else {
            if ((fromPre || fromBackPre) && (!from_forward_pre_req) && (!from_back_pre_req)) {
                DPRINTF(PageTableWalker, "from_forward_pre_req be coalesced\n");
                preHitInPtw = true;
            }
            DPRINTF(PageTableWalker, "Coalescing walk for %#lx(pc=%#lx) into %#lx(pc=%#lx)\n", req->getVaddr(),
                    req->getPC(), mainReq->getVaddr(), mainReq->getPC());
            // add to list of requestors
            requestors.emplace_back(_tc, req, translation);
            requestors.back().fromForwardPreReq = from_forward_pre_req;
            requestors.back().fromBackPreReq = from_back_pre_req;
            auto &r = requestors.back();
            Fault new_fault = NoFault;
            if (mainFault != NoFault) {
                // recreate fault for this txn, we don't have pmp yet
                // TODO: also consider pmp's addr fault
                new_fault = pageFaultOnRequestor(r, false);
            }
            if (requestors.size() == 1) {  // previous requestors are squashed
                DPRINTF(PageTableWalker,
                        "Replace %#lx(pc=%#lx) with %#lx(pc=%#lx) bc main is "
                        "squashed",
                        mainReq->getVaddr(), mainReq->getPC(),
                        r.req->getVaddr(), r.req->getPC());
                mainFault = new_fault;
                mainReq = r.req;
                panic("wrong in ptw Coalesce\n");
            }
            return std::make_pair(true, new_fault);
        }
    }
    return std::make_pair(false, NoFault);
}

void
Walker::dol2TLBHit()
{
    DPRINTF(PageTableWalker2, "dol2tlbhit %d\n", curCycle());
    auto iter = L2TLBrequestors.begin();
    while (iter != L2TLBrequestors.end()) {
        L2TlbState dol2TLBHitrequestors = *iter;
        iter = L2TLBrequestors.erase(iter);
        Fault l2tlbFault;
        PrivilegeMode pmodel2 = tlb->getMemPriv(dol2TLBHitrequestors.tc,
                                                dol2TLBHitrequestors.mode);
        dol2TLBHitrequestors.req->setPaddr(dol2TLBHitrequestors.Paddr);
        pma->check(dol2TLBHitrequestors.req);
        l2tlbFault =
            pmp->pmpCheck(dol2TLBHitrequestors.req, dol2TLBHitrequestors.mode,
                          pmodel2, dol2TLBHitrequestors.tc);//pmpcheck
        //assert(l2tlbFault == NoFault);
        if (l2tlbFault == NoFault) {
            if (enableL1L2replace){ //write back entry from L2 to L1
                if (dol2TLBHitrequestors.entry != nullptr) {
                    TlbEntry l1_entry;
                    if (tlb->isL1DirectCompressionEnabled() &&
                        tlb->buildSingleL1CompressedEntry(dol2TLBHitrequestors.req->getVaddr(),
                                                          *dol2TLBHitrequestors.entry, direct, l1_entry)) {
                        tlb->insert(l1_entry.vaddr, l1_entry, false, direct);
                        tlb->recordL1CompressedEntry(l1_entry);
                    } else if (!tlb->isL1DirectCompressionEnabled()) {
                        tlb->insert(dol2TLBHitrequestors.entry->vaddr, *dol2TLBHitrequestors.entry, false, direct);
                    }
                }
                if (dol2TLBHitrequestors.entryVsstage != nullptr)
                    tlb->insert(dol2TLBHitrequestors.entryVsstage->vaddr, *dol2TLBHitrequestors.entryVsstage, false,
                            vsstage);
                if (dol2TLBHitrequestors.entryGstage != nullptr)
                    tlb->insert(dol2TLBHitrequestors.entryGstage->gpaddr, *dol2TLBHitrequestors.entryGstage, false,
                            gstage);
            }
            dol2TLBHitrequestors.translation->finish(
                l2tlbFault, dol2TLBHitrequestors.req, dol2TLBHitrequestors.tc,
                dol2TLBHitrequestors.mode);
        }
        else{
            warn("pmp fault in l2tlb\n");
            dol2TLBHitrequestors.translation->finish(
                l2tlbFault, dol2TLBHitrequestors.req, dol2TLBHitrequestors.tc,
                dol2TLBHitrequestors.mode);
        }

        DPRINTF(
            PageTableWalker2,
            " *********************dol2tlbhit vaddr %#x paddr %#x pc %#x\n",
            dol2TLBHitrequestors.req->getVaddr(), dol2TLBHitrequestors.Paddr,
            dol2TLBHitrequestors.req->getPC());
    }
    DPRINTF(PageTableWalker2, "finish dol2tlbhit\n");
}
bool
Walker::WalkerState::anyRequestorSquashed() const
{
    bool any_squashed =
        std::accumulate(requestors.begin(), requestors.end(), false,
                        [](bool acc, const RequestorState &r) {
                            return acc || r.translation->squashed();
                        });
    return any_squashed;
}

bool
Walker::WalkerState::allRequestorSquashed() const
{
    bool all_squashed =
        std::accumulate(requestors.begin(), requestors.end(), true,
                        [](bool acc, const RequestorState &r) {
                            return acc && r.translation->squashed();
                        });
    return all_squashed;
}

Fault
Walker::WalkerState::startWalk(Addr ppn, int f_level, bool from_l2tlb,
                               bool open_nextline, bool auto_open_nextline,
                               bool from_forward_req,bool from_back_req)
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    assert(!(from_forward_req && from_back_req));

    if (translateMode == twoStageMode) {
        fault = setupWalk(ppn, mainReq->getVaddr(), f_level, from_l2tlb, open_nextline, auto_open_nextline,
                          from_forward_req, from_back_req);
        if (fault != NoFault)
            return fault;

    } else {
        DPRINTF(PageTableWalker, "startWalk: ppn %#x, f_level %d\n", ppn, f_level);
        if (from_back_req) {
            fault = setupWalk(ppn, mainReq->getBackPreVaddr(), f_level, from_l2tlb, open_nextline,
                              auto_open_nextline, from_forward_req, from_back_req);
        } else if (from_forward_req) {
            fault = setupWalk(ppn, mainReq->getForwardPreVaddr(), f_level, from_l2tlb, open_nextline,
                              auto_open_nextline, from_forward_req, from_back_req);
        } else {
            fault = setupWalk(ppn, mainReq->getVaddr(), f_level, from_l2tlb, open_nextline,
                              auto_open_nextline, from_forward_req, from_back_req);
        }
        if (fault != NoFault)
            return fault;
    }
    if (timing) {
        nextState = state;
        state = Waiting;
        mainFault = NoFault;
        if (!isMPTing)
            sendPackets();
    } else {
        if (translateMode == twoStageMode)
            assert(0);
        do {
            walker->port.sendAtomic(read);
            PacketPtr write = NULL;
            assert(translateMode == twoStageMode);
            fault = stepWalk(write);
            assert(fault == NoFault || read == NULL);
            state = nextState;
            nextState = Ready;
            if (write)
                walker->port.sendAtomic(write);
        } while (read);
        state = Ready;
        nextState = Waiting;
    }
    return fault;
}

Fault
Walker::WalkerState::startFunctional(Addr &addr, unsigned &logBytes,
                                     bool open_nextline, bool auto_open_nextline,
                                     bool from_forward_pre_req,
                                     bool from_back_pre_req)
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    setupWalk(0, addr, 2, false, open_nextline, auto_open_nextline, from_forward_pre_req,
              from_back_pre_req);

    do {
        walker->port.sendFunctional(read);
        // On a functional access (page table lookup), writes should
        // not happen so this pointer is ignored after stepWalk
        PacketPtr write = NULL;
        if ((translateMode == twoStageMode) && (inGstage)) {
            fault = twoStageStepWalk(write);
        } else if ((translateMode == twoStageMode) && (!inGstage)) {
            fault = twoStageWalk(write);
        } else {
            fault = stepWalk(write);
        }
        assert(fault == NoFault || read == NULL);
        state = nextState;
        nextState = Ready;
    } while (read);
    logBytes = entry.logBytes;
    addr = entry.paddr << PageShift;

    return fault;
}

Fault
Walker::WalkerState::twoStageStepWalk(PacketPtr &write)
{
    assert(state != Ready && state != Waiting);
    Fault fault = NoFault;
    write = NULL;
    uint64_t vaddr_choose;
    PTE pte;
    bool doEndWalk = false;
    bool doLLwalk = false;
    Addr PgBase;
    PTE l2pte;
    unsigned oldSize = 64;
    Request::Flags flags = Request::PHYSICAL;

    vaddr_choose = (gPaddr >> (twoStageLevel * LEVEL_BITS + PageShift)) & VADDR_CHOOSE_MASK;
    PacketPtr oldRead = read;
    if (!tlbHit) {
        pte = read->getLE_l2tlb<uint64_t>(vaddr_choose);
        DPRINTF(PageTableWalkerTwoStage,
                "twoStageStepWalk(G): choose (vaddr_choose = %d) in returned l2tlb:\n", vaddr_choose);
        for (int _i = 0; _i < 8; _i++){
            DPRINTFR(PageTableWalkerTwoStage, "\tpte[%d]:%#x\n", _i, read->getLE_l2tlb<uint64_t>(_i));
        }
        // DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk pte %lx ppn %#x vaddr %lx gpaddr %lx\n",
        //         pte, pte.ppn, entry.vaddr, gPaddr);

        flags = oldRead->req->getFlags();

        walker->pma->check(read->req);
        oldSize = oldRead->getSize();

        // Effective privilege mode for pmp checks for page table
        // walks is S mode according to specs
        fault = walker->pmp->pmpCheck(read->req, BaseMMU::Read, RiscvISA::PrivilegeMode::PRV_S, requestors.front().tc,
                                      nextlineEntry.vaddr);
    } else {
        pte = tlbHitPte;
        flags = tlbflags;
    }

    Addr nextRead = 0;
    Addr nextcheck = 0;

    PgBase = pte.ppn << 12;

    DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk(G): got level %d twoStageLevel %d pte %#x "
            "(ppn:%#x, v:%d, r:%d, w:%d, x:%d) for vaddr %#x, gpaddr %#x\n",
            level, twoStageLevel, pte, pte.ppn, pte.v, pte.r, pte.w, pte.x, entry.vaddr, gPaddr);

    if (fault == NoFault) {
        if (pte.v && !pte.r && !pte.w && !pte.x) {
            twoStageLevel--;
            if (twoStageLevel < 0) {
                endWalk();
                warn("pagefault in Gstage ptw twostagelevel <0\n");
                return endGstageWalk();
            } else {
                auto PgIdx = getGVPNi(hgatp.mode, gPaddr, twoStageLevel);
                nextRead = PgBase + (PgIdx * PTESIZE);
                nextcheck = nextRead;
                nextRead = (nextRead >> 6) << 6; // PTESize * L2TlbLineSize (512 bits)
                nextState = Translate;
                DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk(G): need to continue PTW for "
                        "twoStagelevel %d nextRead %#x (base %#x, idx %#x).\n",
                        twoStageLevel, nextRead, PgBase, PgIdx);
                if ((!isVsatp0Mode) && (!tlbHit)) {
                    DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk(G): insert l2tlb\n");
                    int l2_level = twoStageLevel + 1;
                    inl2Entry.gpaddr = gPaddr;
                    inl2Entry.pte = pte;
                    inl2Entry.logBytes = PageShift + (l2_level * LEVEL_BITS);
                    inl2Entry.level = l2_level;
                    for (int l2_i = 0; l2_i < l2tlbLineSize; l2_i++) {
                        inl2Entry.gpaddr = (((gPaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)))
                                             << L2TLB_BLK_OFFSET) +
                                            l2_i)
                                           << ((l2_level * LEVEL_BITS + PageShift));

                        inl2Entry.vaddr = (((entry.vaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)))
                                             << L2TLB_BLK_OFFSET) +
                                            l2_i)
                                           << ((l2_level * LEVEL_BITS + PageShift));
                        l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                        inl2Entry.pte = l2pte;
                        inl2Entry.paddr = l2pte.ppn;

                        if (hgatp.mode == AddrXlateMode::SV48 && l2_level == 3) {
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2L3, l2_i, false,
                                                     gstage);
                        } else if (l2_level == 2) {
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2L2, l2_i, false,
                                                     gstage);
                        } else if (l2_level == 1) {
                            inl2Entry.index =
                                (gPaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) & (walker->tlb->L2TLB_L1_MASK);
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2L1, l2_i, false,
                                                     gstage);
                        }
                    }
                }
            }

        } else if (!pte.v || (!pte.r && pte.w)) {
            endWalk();
            return endGstageWalk();
        } else if (!pte.u) {
            endWalk();
            return endGstageWalk();
        } else if (((mode == BaseMMU::Execute) || isHInst) && (!pte.x)) {
            endWalk();
            return endGstageWalk();
        } else if ((mode == BaseMMU::Read) && (!pte.r && !(status.mxr && pte.x))) {
            endWalk();
            return endGstageWalk();
        } else if ((mode == BaseMMU::Write) && !(pte.r && pte.w)) {
            endWalk();
            GstageFault = true;
            fault = pageFault(true, true);
            return fault;
        } else {
            inGstage = false;
            doEndWalk = true;
            doLLwalk = true;
            entry.gpaddr = gPaddr;
            entry.pte = pte;
            entry.logBytes = PageShift + (twoStageLevel * LEVEL_BITS);
            entry.level = twoStageLevel;

            Addr pg_mask;
            if (twoStageLevel > 0) { // leaf superpage
                pg_mask = ((1ULL << (12 + 9 * twoStageLevel)) - 1);
                if (((pte.ppn << 12) & pg_mask) != 0) {
                    // missaligned superpage
                    warn("missaligned superpage vaddr %lx\n",entry.vaddr);
                    fault = pageFault(true, false);
                    endWalk();
                    return fault;
                }
                PgBase = (PgBase & ~pg_mask) | (gPaddr & pg_mask & ~PGMASK);
            }
            PgBase = PgBase | (gPaddr & PGMASK);
            vaddr_choose_flag = (PgBase & 0x3f) / 8; // 0b00111000
            nextcheck = PgBase;
            nextRead = (PgBase >> 6) << 6;
            gPaddr = nextRead;
            entry.paddr = gPaddr;

            DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk(G): found the leaf pte.\n");

            if (finishGVA && (!isVsatp0Mode)) {
                walker->tlb->insert(entry.gpaddr, entry, false, gstage);
                int l2_level = twoStageLevel;
                inl2Entry.gpaddr = entry.gpaddr;
                inl2Entry.pte = pte;
                inl2Entry.logBytes = PageShift + (l2_level * LEVEL_BITS);
                inl2Entry.level = l2_level;
                if (!tlbHit) {
                    for (int l2_i = 0; l2_i < l2tlbLineSize; l2_i++) {
                        inl2Entry.gpaddr = ((entry.gpaddr >>
                            ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) << L2TLB_BLK_OFFSET) + l2_i)
                            << ((l2_level * LEVEL_BITS + PageShift));
                        inl2Entry.vaddr = ((entry.vaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET))
                                                                 << L2TLB_BLK_OFFSET) +
                                            l2_i)
                                           << ((l2_level * LEVEL_BITS + PageShift));
                        l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                        DPRINTF(PageTableWalker3, "final insert vaddr %#x ppn %#x pte %#x pre %d\n", inl2Entry.vaddr,
                                l2pte.ppn, l2pte, entry.fromForwardPreReq);
                        DPRINTF(PageTableWalker3, "level %d l2_level %d\n", level, l2_level);
                        inl2Entry.paddr = l2pte.ppn;
                        inl2Entry.pte = l2pte;
                        if (l2_level == 0) {
                            inl2Entry.index =
                                (inl2Entry.gpaddr >> (L2TLB_BLK_OFFSET + PageShift)) & walker->tlb->L2TLB_L0_MASK;
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2L0, l2_i, false,
                                                     gstage);
                        }

                        else if (l2_level == 1) {
                            inl2Entry.index = (inl2Entry.gpaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                              (walker->tlb->L2TLB_L1_MASK);
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2sp1, l2_i, false,
                                                     gstage);
                        }  // hit level =1

                        else if (l2_level == 2) {
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2sp2, l2_i, false,
                                                     gstage);
                        }

                        else if (hgatp.mode == AddrXlateMode::SV48 && l2_level == 3) {
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2sp3, l2_i, false,
                                                     gstage);
                        }
                    }
                }
            }

            if ((gPaddr & ~H_VADDR_MASK(hgatp.mode)) != 0) {
                // this is a excep
                panic("address fault\n");
            }
            DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx\n", gPaddr, entry.vaddr);
            gpaddrMode =1;
            mainReq->setgPaddr(gPaddr);
        }

        /// finish PTW or continue PTW.
        DPRINTF(PageTableWalkerTwoStage,
                "twoStageStepWalk(G): finishGVA %d, doLLwalk %d, doEndWalk %d isVsatpOMode %d.\n",
                finishGVA, doLLwalk, doEndWalk, isVsatp0Mode);

        if (doLLwalk && finishGVA) {
            DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk(G): finish PTW.\n");
            //entry.paddr = pte.ppn;
            entry.paddr = gPaddr >> 12;
            entry.pte = pte;
            int put_level = 0;
            put_level = std::min(twoStageLevel, level);

            entry.logBytes = PageShift + (put_level * LEVEL_BITS);
            entry.level = put_level;
            walker->tlb->insert(entry.vaddr, entry, false, allstage);


            endWalk();
            return NoFault;
        } else if ((!doEndWalk) || (doLLwalk)) {
            if (isVsatp0Mode && doLLwalk) {
                DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk(G): finish PTW.\n");
                entry.paddr = gPaddr >> 12;
                entry.pte = pte;
                entry.logBytes = PageShift + (twoStageLevel * LEVEL_BITS);
                entry.level = twoStageLevel;
                entry.gpaddr = entry.vaddr;
                walker->tlb->insert(entry.vaddr, entry, false, gstage);
                endWalk();
                return NoFault;
            }

            DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk(G): continue PTW.\n");
            if (nextRead == 0)
                panic("nextread can't be 0\n");

            if (walker->l2tlb == nullptr)
                panic("walker->l2tlb is none\n");

            if (deferPtwLevelRead(twoStageLevel, nextRead, oldSize, flags, oldRead)) {
                read = nullptr;
                walker->retryPtwLevelBlockedStates();
                return fault;
            }
            delete oldRead;
            oldRead = nullptr;
            RequestPtr request = std::make_shared<Request>(nextRead, oldSize, flags, walker->requestorId);
            DPRINTF(PageTableWalkerTwoStage,
                    "twoStageStepWalk nextRead %lx vaddr %lx gpaddr %lx level %d twolevel %d\n", nextRead,
                    entry.vaddr, gPaddr, level, twoStageLevel);
            DPRINTF(PageTableWalker, "oldread size %d\n", oldSize);

            read = new Packet(request, MemCmd::ReadReq);
            read->allocate();
            fault = startPteReadMPTCheck();
            if (fault != NoFault)
                return fault;
            DPRINTF(PageTableWalker, "Loading level%d PTE from %#x vaddr %#x\n", level, nextRead, entry.vaddr);
        } else {
            panic("wrong in G ptw\n");
        }
    } else {
        panic("wrong in G ptw\n");
    }

    return fault;
}

Fault
Walker::WalkerState::twoStageWalk(PacketPtr &write)
{
    Fault fault;
    bool doEndWalk = false;
    PTE pte;

    PacketPtr oldRead = read;
    Request::Flags flags;

    PTE l2pte;
    unsigned oldSize = 64;

    if (!tlbHit) {
        pte = read->getLE_l2tlb<uint64_t>(vaddr_choose_flag);
        DPRINTF(PageTableWalkerTwoStage,
                "twoStageWalk(VS): choose (vaddr_choose_flag = %d) in returned l2tlb:\n", vaddr_choose_flag);
        for (int _i = 0; _i < 8; _i++){
            DPRINTFR(PageTableWalkerTwoStage, "\tpte[%d]:%#x\n", _i, read->getLE_l2tlb<uint64_t>(_i));
        }
        flags = oldRead->req->getFlags();
        walker->pma->check(read->req);
        oldSize = oldRead->getSize();
        fault = walker->pmp->pmpCheck(read->req, BaseMMU::Read, RiscvISA::PrivilegeMode::PRV_S, requestors.front().tc,
                                      gPaddr);

    } else {
        pte = tlbHitPte;
        flags = tlbflags;
    }
    TlbEntry *e[L_L2SUM] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    int hit_level = PTW_TOP_LEVEL(vsatp.mode);
    bool tlb_hit = false;
    Addr shift = 0;
    Addr idx_f = 0;
    Addr idx = 0;
    Addr nextcheck = 0;

    DPRINTF(PageTableWalkerTwoStage, "twoStageWalk(VS): got level %d twoStageLevel %d pte %#x "
            "(ppn:%#x, v:%d, r:%d, w:%d, x:%d) for vaddr %#x \n",
            level, twoStageLevel, pte, pte.ppn, pte.v, pte.r, pte.w, pte.x, entry.vaddr);

    if (fault == NoFault) {
        if (!pte.v || (!pte.r && pte.w)) {
            doEndWalk = true;
            DPRINTF(PageTableWalker3, "PTE invalid, raising PF\n");
            GstageFault = false;
            fault = pageFault(pte.v, false);
            endWalk();
        } else {
            if (pte.r || pte.x) {
                doEndWalk = true;
                if (virt) {
                    fault = walker->tlb->checkPermissions(vsstatus, pmode, entry.vaddr, mode, pte, 0, false);
                } else {
                    fault = walker->tlb->checkPermissions(status, pmode, entry.vaddr, mode, pte, 0, false);
                }

                if (fault == NoFault) {
                    if ((!pte.a) || ((!pte.d) && (mode == BaseMMU::Write))) {
                        GstageFault = false;
                        fault = pageFault(true,false);
                        endWalk();
                    } else {
                        finishGVA = true;
                        entry.gpaddr = gPaddr;
                        entry.pte = pte;
                        entry.pteVS = pte;
                        entry.logBytes = PageShift + (level * LEVEL_BITS);
                        entry.level = level;

                        gPaddr = pte.ppn << 12;
                        if (level > 0) {
                            Addr pg_mask = (1ULL << (12 + 9 * level)) - 1;
                            if ((pg_mask & (pte.ppn << 12)) != 0) {
                                fault = pageFault(true, false);
                                endWalk();
                                return fault;
                            }
                            gPaddr = ((pte.ppn << 12) & ~pg_mask) | (entry.vaddr & pg_mask & ~PGMASK);
                        }
                        gPaddr = gPaddr | (entry.vaddr & PGMASK);

                        entry.paddr = (gPaddr >> 12) << 12;
                        walker->tlb->insert(entry.vaddr, entry, false, vsstage);
                        if (!tlbHit) {
                            int l2_level = level;
                            inl2Entry.gpaddr = gPaddr;
                            inl2Entry.pte = pte;
                            inl2Entry.logBytes = PageShift + (l2_level * LEVEL_BITS);
                            inl2Entry.level = l2_level;

                            for (int l2_i = 0; l2_i < l2tlbLineSize; l2_i++) {
                                inl2Entry.vaddr =
                                    (((entry.vaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)))
                                      << L2TLB_BLK_OFFSET) +
                                     l2_i)
                                    << ((l2_level * LEVEL_BITS + PageShift));
                                l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                                inl2Entry.pte = l2pte;
                                inl2Entry.paddr = l2pte.ppn;
                                DPRINTF(PageTableWalkerTwoStage, "twoStageWalk(VS) insert leaf pte in l2tlb: "
                                        "pte %#x, ppn %#x, level %d\n",
                                        l2pte, l2pte.ppn, level);
                                if (l2_level == 0) {
                                    inl2Entry.index = (inl2Entry.vaddr >> (L2TLB_BLK_OFFSET + PageShift)) &
                                                      walker->tlb->L2TLB_L0_MASK;
                                    walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L0, l2_i, false,
                                                             vsstage);
                                } else if (l2_level == 1) {
                                    inl2Entry.index =
                                        (inl2Entry.vaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                        walker->tlb->L2TLB_L1_MASK;
                                    walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp1, l2_i,
                                                             false, vsstage);
                                } else if (l2_level == 2) {
                                    walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp2, l2_i,
                                                             false, vsstage);
                                } else if (vsatp.mode == AddrXlateMode::SV48 && l2_level == 3) {
                                    walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp3, l2_i,
                                                             false, vsstage);
                                }
                            }
                        }
                        if ((gPaddr & ~H_VADDR_MASK(vsatp.mode)) != 0) {
                            // this is a excep
                            fault = pageFault(true, true);
                            endWalk();
                            return fault;
                        }
                        DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx\n", gPaddr,
                                entry.vaddr);
                        gpaddrMode =3;
                        mainReq->setgPaddr(gPaddr);
                        nextState = Translate;
                        inGstage = true;
                        twoStageLevel = PTW_TOP_LEVEL(hgatp.mode);
                        tlbHit = false;
                        nextcheck = 0;
                        shift = PageShift + LEVEL_BITS * twoStageLevel;
                        idx = ((gPaddr >> shift) & TWO_STAGE_L2_LEVEL_MASK);
                        nextcheck = gPaddr;


                        if (!tlbHit) {
                            delete oldRead;
                            oldRead = nullptr;
                            read = nullptr;
                            fault = startTwoStageWalk(gPaddr, entry.vaddr);
                            if (fault != NoFault) {
                                endWalk();
                                return fault;
                            }
                        }
                    }
                } else {
                    endWalk();
                }

            } else {
                level--;
                if (level < 0) {
                    doEndWalk = true;
                    GstageFault = false;
                    fault = pageFault(true, false);
                    endWalk();
                } else {
                    entry.pte = pte;
                    entry.logBytes = PageShift + (level * LEVEL_BITS);
                    entry.level = level;

                    shift = (PageShift + LEVEL_BITS * level);
                    idx_f = (entry.vaddr >> shift) & LEVEL_MASK;
                    idx = (idx_f >> L2TLB_BLK_OFFSET) << L2TLB_BLK_OFFSET;
                    gPaddr = (pte.ppn << PageShift) + (idx_f * l2tlbLineSize);
                    entry.paddr = gPaddr;

                    DPRINTF(PageTableWalkerTwoStage,
                            "twoStageWalk(VS) continue PTW for: gPddr %#x, level: %d (idx_f %d)\n",
                            gPaddr, level, idx_f);
                    if (!tlbHit) {
                        int l2_level = level + 1;
                        inl2Entry.gpaddr = gPaddr;
                        inl2Entry.pte = pte;
                        inl2Entry.logBytes = PageShift + (l2_level * LEVEL_BITS);
                        inl2Entry.level = l2_level;

                        for (int l2_i = 0; l2_i < l2tlbLineSize; l2_i++) {
                            inl2Entry.vaddr =
                                (((entry.vaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)))
                                  << L2TLB_BLK_OFFSET) +
                                 l2_i)
                                << ((l2_level * LEVEL_BITS + PageShift));
                            l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                            inl2Entry.pte = l2pte;
                            inl2Entry.paddr = l2pte.ppn;
                            DPRINTF(PageTableWalkerTwoStage, "twoStageWalk(VS) insert middle pte in l2tlb: "
                                    "pte %#x, ppn %#x, level %d\n",
                                    l2pte, l2pte.ppn, l2_level);
                            if (vsatp.mode == AddrXlateMode::SV48 && l2_level == 3) {
                                walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L3, l2_i, false,
                                                         vsstage);
                            } else if (l2_level == 2) {
                                walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L2, l2_i, false,
                                                         vsstage);
                            } else if (l2_level == 1) {
                                inl2Entry.index = (inl2Entry.vaddr>> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                                  (walker->tlb->L2TLB_L1_MASK);
                                walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L1, l2_i, false,
                                                         vsstage);
                            }
                        }
                    }

                    if ((gPaddr & ~H_VADDR_MASK(vsatp.mode)) != 0) {
                        // this is a excep
                        fault = pageFault(true, false);
                        endWalk();
                        return fault;
                    }
                    DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx\n", gPaddr, entry.vaddr);
                    gpaddrMode =2;
                    mainReq->setgPaddr(gPaddr);
                    nextState = Translate;
                    inGstage = true;
                    twoStageLevel = PTW_TOP_LEVEL(hgatp.mode);
                    tlbHit = false;


                    shift = PageShift + LEVEL_BITS * twoStageLevel;
                    idx = ((gPaddr >> shift) & TWO_STAGE_L2_LEVEL_MASK);
                    nextcheck = gPaddr;
                    if (!tlbHit) {
                        delete oldRead;
                        oldRead = nullptr;
                        read = nullptr;
                        fault = startTwoStageWalk(gPaddr, entry.vaddr);
                        if (fault != NoFault) {
                            endWalk();
                            return fault;
                        }
                    }
                }
            }
        }
    } else {
        panic("wrong in G ptw\n");
    }

    return fault;
}


Fault
Walker::WalkerState::stepWalk(PacketPtr &write)
{
    if (!isMPTing) assert(state != Ready && state != Waiting);
    Fault fault = NoFault;
    write = NULL;
    uint64_t vaddr_choose;

    PTE pte ;

    vaddr_choose = (entry.vaddr >> (level * LEVEL_BITS + PageShift)) & VADDR_CHOOSE_MASK;
    pte = read->getLE_l2tlb<uint64_t>(vaddr_choose);

    DPRINTF(PageTableWalker, "%s: get pte from walker cache.\n", __func__);
    for (int i = 0; i < l2tlbLineSize; i++) {
        DPRINTFR(PageTableWalker, "\tpte[%d]: %#x\n", i, read->getLE_l2tlb<uint64_t>(i));
    }

    Addr nextRead = 0;
    bool doWrite = false;
    bool doTLBInsert = false;
    bool doEndWalk = false;
    int l2_i =0;
    PTE l2pte;
    int l2_level;

    DPRINTF(PageTableWalker3,
            "Got level%d PTE: %#x PPN: %#x choose %d vaddr %#x next_line %d "
            "next_vaddr %#x pre %d\n",
            level, pte, pte.ppn, vaddr_choose, entry.vaddr, nextline,
            nextlineEntry.vaddr, entry.fromForwardPreReq);
    // step 2:
    // Performing PMA/PMP checks on physical address of PTE

    if (!nextline){
        walker->pma->check(read->req);
        // Effective privilege mode for pmp checks for page table
        // walks is S mode according to specs
        fault = walker->pmp->pmpCheck(read->req, BaseMMU::Read,
                                      RiscvISA::PrivilegeMode::PRV_S,
                                      requestors.front().tc, entry.vaddr);
    } else {
        walker->pma->check(read->req);
        // Effective privilege mode for pmp checks for page table
        // walks is S mode according to specs
        fault = walker->pmp->pmpCheck(
            read->req, BaseMMU::Read, RiscvISA::PrivilegeMode::PRV_S,
            requestors.front().tc, nextlineEntry.vaddr);
    }

    if (fault != NoFault) {
        DPRINTF(PageTableWalker3, " may pmp fault vaddr %#x\n", entry.vaddr);
    }
    //
    if ((fault == NoFault) && (!nextline)) {
        // step 3
        if (!pte.v || (!pte.r && pte.w)) {
            doEndWalk = true;
            DPRINTF(PageTableWalker3, "PTE invalid, raising PF\n");
            fault = pageFault(pte.v, false);
        }
        else {
            // step 4:
            if (pte.r || pte.x) {
                // step 5: leaf PTE
                doEndWalk = true;
                fault = walker->tlb->checkPermissions(status, pmode, entry.vaddr, mode, pte, 0, false);
                // step 6
                if (fault == NoFault) {
                    if (level >= 1 && pte.ppn0 != 0) {
                        DPRINTF(PageTableWalker3,
                                "PTE has misaligned PPN, raising PF\n");
                        fault = pageFault(true,false);
                    } else if (level >= 2 && pte.ppn1 != 0) {
                        DPRINTF(PageTableWalker3,
                                "PTE has misaligned PPN, raising PF\n");
                        fault = pageFault(true,false);
                    } else if (level >= 3 && pte.ppn2 != 0) {
                        DPRINTF(PageTableWalker3,
                                "PTE has misaligned PPN, raising PF\n");
                        fault = pageFault(true,false);
                    }
                } else {
                    DPRINTF(PageTableWalker3, "checkpremission fault\n");
                }

                if (fault == NoFault) {
                    // step 7
                    if (!pte.a) {
                        DPRINTF(PageTableWalker3,
                                "PTE needs to write pte.a,raising PF\n");
                        fault = pageFault(true,false);
                    }
                    if (!pte.d && mode == BaseMMU::Write) {
                        DPRINTF(PageTableWalker3,
                                "PTE needs to write pte.d,raising PF\n");
                        fault = pageFault(true,false);
                    }
                    // Performing PMA/PMP checks

                    if (doWrite) {

                        // this read will eventually become write
                        // if doWrite is True
                        DPRINTF(PageTableWalker3, "do write\n");

                        walker->pma->check(read->req);

                        fault = walker->pmp->pmpCheck(read->req,
                                            BaseMMU::Write, pmode, requestors.back().tc, entry.vaddr);
                    }
                    // perform step 8 only if pmp checks pass
                    if (fault == NoFault) {
                        unsigned leafLogBytes = PageShift + (level * LEVEL_BITS);
                        Addr leafVaddr = entry.vaddr;
                        Addr leafOffset = leafVaddr & mask(leafLogBytes);
                        Addr translatedPaddr = (pte.ppn << PageShift) | leafOffset;
                        mptCheckPaddr = translatedPaddr;
                        mptCheckMode = mode;
                        mptFaultMode = mode;
                        mptCheckingPteRead = false;
                        if (globalMPT.mmpt.mode != 0 && !isMPTing) {
                            isMPTing = true;
                            DPRINTF(PageTableWalker,"lastMPTwalk\n");
                            bool Cachehit=LastMPTwalk();
                            if (Cachehit) {
                                isMPTing = false;
                                finishMPTing = true;
                            } else {
                                DPRINTF(PageTableWalker,"last mpt not fin,hold return.\n");
                                return fault;
                            }
                        }else{
                            isMPTing=false;
                            finishMPTing = true;
                        }
                        // step 8
                        unsigned tlbLogBytes = leafLogBytes;
                        mptGranularityClipped = false;
                        if (walker->tlb != nullptr &&
                            walker->tlb->isMptTlbInfoEnabled() &&
                            globalMPT.mmpt.mode != 0 && mptInfo.valid) {
                            unsigned mptLogBytes =
                                getPageShiftForLevel(mptInfo.mptlevel);
                            if (mptLogBytes < tlbLogBytes) {
                                tlbLogBytes = mptLogBytes;
                                mptGranularityClipped = true;
                                DPRINTF(PageTableWalker,
                                        "TLB entry clipped by MPT granularity: "
                                        "vaddr %#lx paddr %#lx ptwLogBytes %u "
                                        "mptLogBytes %u tlbLogBytes %u\n",
                                        leafVaddr, translatedPaddr,
                                        leafLogBytes, mptLogBytes,
                                        tlbLogBytes);
                            } else {
                                DPRINTF(PageTableWalker,
                                        "MPT granularity covers PTW leaf: "
                                        "vaddr %#lx paddr %#lx ptwLogBytes %u "
                                        "mptLogBytes %u\n",
                                        leafVaddr, translatedPaddr,
                                        leafLogBytes, mptLogBytes);
                            }
                        }
                        Addr tlbVaddrBase = leafVaddr & ~mask(tlbLogBytes);
                        Addr tlbPaddrBase =
                            translatedPaddr & ~mask(tlbLogBytes);
                        entry.logBytes = tlbLogBytes;
                        entry.paddr = tlbPaddrBase >> PageShift;
                        entry.vaddr = tlbVaddrBase;
                        entry.pte = pte;
                        int tlbEntryLevel =
                            walker->getLevelForPageSizeLog2(tlbLogBytes);
                        assert(tlbEntryLevel >= 0);
                        entry.level = tlbEntryLevel;
                        entry.mptInfo =
                            (walker->tlb != nullptr &&
                             walker->tlb->isMptTlbInfoEnabled()) ?
                            mptInfo : MPTInfoInTLB();
                        // put it non-writable into the TLB to detect
                        // writes and redo the page table walk in order
                        // to update the dirty flag.
                        doTLBInsert = true;
                        DPRINTF(PageTableWalker3,
                                "tlb read paddr %#x vaddr %#x pte %#x level "
                                "%d pre %d\n",
                                entry.paddr, entry.vaddr, entry.pte, level,
                                entry.fromForwardPreReq);
                    }
                }
            } else {
                level--;
                if (level < 0) {
                    DPRINTF(PageTableWalker3,
                            "No leaf PTE found,"
                            "raising PF\n");
                    doEndWalk = true;
                    fault = pageFault(true,false);
                } else {
                    inl2Entry.logBytes =
                        PageShift + ((level + 1) * LEVEL_BITS);

                    l2_level = level + 1;
                    inl2Entry.level = l2_level;

                    bool has_valid_pte = false;
                    for (l2_i = 0; l2_i < l2tlbLineSize; l2_i++) {
                        inl2Entry.vaddr = (((entry.vaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)))
                                            << L2TLB_BLK_OFFSET) +
                                           l2_i)
                                          << ((l2_level * LEVEL_BITS + PageShift));
                        DPRINTF(PageTableWalker3, "inl2Entry.vaddr %#x entry.vaddr %#x pre %d\n", inl2Entry.vaddr,
                                entry.vaddr, entry.fromForwardPreReq);

                        DPRINTF(PageTableWalker3, "no final insert vaddr %#x ppn %#x pte %#x\n", inl2Entry.vaddr,
                                l2pte.ppn, l2pte);
                        DPRINTF(PageTableWalker3, "level %d l2_level %d\n", level, l2_level);

                        l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                        inl2Entry.paddr = l2pte.ppn;
                        inl2Entry.pte = l2pte;
                        if (l2pte.v) has_valid_pte = true;

                        if (satp.mode == AddrXlateMode::SV48 && l2_level == 3) {
                            walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L3, l2_i, false,
                                                     direct);
                        } else if (l2_level == 2) {
                            walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L2, l2_i, false,
                                                     direct);
                        }
                        if (l2_level == 1) {
                            inl2Entry.index = (inl2Entry.vaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                              (walker->tlb->L2TLB_L1_MASK);
                            walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L1, l2_i, false,
                                                     direct);
                        }

                        if (l2_level == 0) {
                            panic("l2_level is 0,may be wrong\n");
                        }
                    }
                    if (!has_valid_pte)
                        panic("stepWalk: entries inserted into l2tlb should have valid one.\n");

                    Addr shift = (PageShift + LEVEL_BITS * level);
                    Addr idx_f = (entry.vaddr >> shift) & LEVEL_MASK;
                    Addr idx = (idx_f >> L2TLB_BLK_OFFSET) << L2TLB_BLK_OFFSET;

                    nextlineLevelMask = LEVEL_MASK;
                    nextlineShift = shift;

                    tlbVaddr = entry.vaddr;
                    tlbppn = pte.ppn;
                    tlbSizePte = sizeof(pte);

                    nextRead = (pte.ppn << PageShift) + (idx * l2tlbLineSize);
                    nextState = Translate;
                    nextlineRead = nextRead;
                    nextlineLevel = level;
                    DPRINTF(PageTableWalker,
                            "next read of pte pte.ppx %#x,idx %#x,sizeof(pte) "
                            "%#x,nextread %#x\n",
                            pte.ppn, idx, sizeof(pte), nextRead);
                    DPRINTF(PageTableWalker, "tlb_ppn %#x vaddr %#x\n", tlbppn, entry.vaddr);
                }
            }
        }
    } else if (nextline) {
        nextlineEntry.logBytes = PageShift + (nextlineLevel * LEVEL_BITS);
        nextlineEntry.vaddr &= ~((1 << nextlineEntry.logBytes) - 1);
        nextlineEntry.level = nextlineLevel;
    } else {
        doEndWalk = true;
    }
    PacketPtr oldRead = read;
    Request::Flags flags = oldRead->req->getFlags();
    if ((doEndWalk)&&(!nextline)) {
        // If we need to write, adjust the read packet to write the modified
        // value back to memory.
        if (!functional && doWrite) {
            DPRINTF(PageTableWalker, "Writing level%d PTE to %#x: %#x\n",
                level, oldRead->getAddr(), pte);
            write = oldRead;
            write->setLE<uint64_t>(pte);
            write->cmd = MemCmd::WriteReq;
            read = NULL;
            panic("wrong in ptw , now don't need do write\n");
        } else {
            write = NULL;
        }

        if (doTLBInsert) {  //write back L1
            if (!functional) {
                if (((!entry.fromForwardPreReq) && (!entry.fromBackPreReq)) || (preHitInPtw)) {
                    if (walker->tlb->isL1DirectCompressionEnabled()) {
                        std::array<PTE, l2tlbLineSize> l1_compress_ptes;
                        for (int compress_i = 0; compress_i < l2tlbLineSize; compress_i++) {
                            l1_compress_ptes[compress_i] = read->getLE_l2tlb<uint64_t>(compress_i);
                        }
                        walker->tlb->recordL1CompressionPotential(entry.vaddr, entry.pte, l1_compress_ptes, direct,
                                                                    level);
                        TlbEntry compressed_entry;
                        if (walker->tlb->buildL1CompressedEntry(entry.vaddr, entry, l1_compress_ptes, direct, level,
                                                                 compressed_entry)) {
                            walker->tlb->insert(compressed_entry.vaddr, compressed_entry, false, direct);
                            TlbEntry *l1_entry = walker->tlb->lookup(entry.vaddr, entry.asid, BaseMMU::Read, true,
                                                                     false, direct);
                            if (l1_entry && l1_entry->isCompressed)
                                walker->tlb->recordL1CompressedEntry(compressed_entry);
                        } else if (walker->tlb->buildSingleL1CompressedEntry(entry.vaddr, entry, direct,
                                                                              compressed_entry)) {
                            walker->tlb->insert(compressed_entry.vaddr, compressed_entry, false, direct);
                            walker->tlb->recordL1CompressedEntry(compressed_entry);
                        }
                    } else {
                        walker->tlb->insert(entry.vaddr, entry, false, direct);
                    }
                }
                finishDefaultTranslate = true;

                if (mptGranularityClipped) {
                    DPRINTF(PageTableWalker,
                            "Skip L2TLB line/nextline insert because MPT "
                            "granularity clipped this TLB entry: vaddr %#x "
                            "logBytes %u\n",
                            entry.vaddr, entry.logBytes);
                } else {
                DPRINTF(PageTableWalker, "l1tlb vaddr %#x \n", entry.vaddr);
                inl2Entry.logBytes = PageShift + (level * LEVEL_BITS);
                l2_level = level;
                inl2Entry.level = level;
                DPRINTF(PageTableWalker3, "final l1tlb vaddr %#x pre %d\n", entry.vaddr, entry.fromForwardPreReq);

                for (l2_i = 0; l2_i < l2tlbLineSize; l2_i++) {
                    inl2Entry.vaddr = ((entry.vaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET))
                                                           << L2TLB_BLK_OFFSET) +
                                       l2_i)
                                      << ((l2_level * LEVEL_BITS + PageShift));
                    l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                    DPRINTF(PageTableWalker3, "final insert vaddr %#x ppn %#x pte %#x pre %d\n", inl2Entry.vaddr,
                            l2pte.ppn, l2pte, entry.fromForwardPreReq);
                    DPRINTF(PageTableWalker3, "level %d l2_level %d\n", level, l2_level);
                    inl2Entry.paddr = l2pte.ppn;
                    inl2Entry.pte = l2pte;
                    if (walker->tlb != nullptr &&
                        walker->tlb->isMptTlbInfoEnabled() &&
                        globalMPT.mmpt.mode != 0 && mptInfo.valid) {
                        Addr checkedMptSlot =
                            mptPermSlotAlign(PaddrUT, mptInfo.mptlevel);
                        Addr entryMptSlot =
                            mptPermSlotAlign(inl2Entry.paddr << PageShift,
                                             mptInfo.mptlevel);
                        inl2Entry.mptInfo =
                            (entryMptSlot == checkedMptSlot) ? mptInfo : MPTInfoInTLB();
                    } else {
                        inl2Entry.mptInfo = MPTInfoInTLB();
                    }
                    if (l2_level == 0) {
                        inl2Entry.index = (entry.vaddr >> (L2TLB_BLK_OFFSET + PageShift)) & walker->tlb->L2TLB_L0_MASK;
                        walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L0, l2_i, false, direct);
                    }

                    else if (l2_level == 1)  {
                        inl2Entry.index = (inl2Entry.vaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                              (walker->tlb->L2TLB_L1_MASK);
                        walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp1, l2_i, false, direct);
                    }// hit level =1

                    else if (l2_level == 2)  {
                        // inl2Entry.index = (inl2Entry.vaddr >> (2 * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                        //                     (walker->tlb->L2TLB_L2_MASK);
                        walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp2, l2_i, false, direct);
                    }

                    else if (satp.mode == AddrXlateMode::SV48 && l2_level == 3)  {
                        walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp3, l2_i, false, direct);
                    }
                }
                if (!doWrite) {
                    nextlineVaddr = entry.vaddr + (l2tlbLineSize << (nextlineLevel * LEVEL_BITS + PageShift));
                    Addr read_num_pre = (nextlineVaddr >> ((nextlineLevel + 1) * LEVEL_BITS + PageShift));
                    Addr read_num = (entry.vaddr >> ((nextlineLevel + 1) * LEVEL_BITS + PageShift));
                    Addr nextline_idx;
                    Addr nextline_idx_f;
                    nextline_idx_f = (nextlineVaddr >> nextlineShift) & nextlineLevelMask;
                    nextline_idx = (nextline_idx_f >> L2TLB_BLK_OFFSET) << L2TLB_BLK_OFFSET;

                    nextRead = (tlbppn << PageShift) + (nextline_idx * l2tlbLineSize);

                    DPRINTF(PageTableWalker3,
                            "nextline basis is vaddr %#x pre vaddr is %#x "
                            "read_num is %#x pre %d\n",
                            entry.vaddr, nextlineVaddr, read_num, entry.fromForwardPreReq);
                    DPRINTF(PageTableWalker3, "nextline tlb_vaddr %#x entry.vaddr is %#x \n", tlbVaddr, entry.vaddr);
                    DPRINTF(PageTableWalker3,
                            "nextline nextread %#x tlb_ppn %#x "
                            "read_num_pre%#x read_num %#x\n",
                            nextRead, tlbppn, read_num_pre, read_num);

                    if ((read_num_pre == read_num) && (nextlineLevel == 0) && timing && openNextline &&
                        autoNextlineSign && (!entry.fromForwardPreReq) && (!entry.fromBackPreReq)) {
                        nextline = true;
                        nextState = Translate;
                        nextlineEntry.vaddr =
                            entry.vaddr + (l2tlbLineSize << (nextlineLevel * LEVEL_BITS + PageShift));

                        RequestPtr request = std::make_shared<Request>(
                            nextRead, oldRead->getSize(), flags,
                            walker->requestorId);
                        if (nextRead == 0)
                            panic("wrong in nextline pre, nextRead can't be 0\n");
                        delete oldRead;
                        oldRead = nullptr;
                        read = new Packet(request, MemCmd::ReadReq);
                        read->allocate();
                        fault = startPteReadMPTCheck();
                        if (fault != NoFault)
                            return fault;

                        DPRINTF(PageTableWalker,
                                "nextline nextline_vaddr %#x "
                                "nextlineEntry.vaddr is %#x\n",
                                nextlineVaddr, nextlineEntry.vaddr);
                        DPRINTF(PageTableWalker,
                                "nextline level %d pte from %#x vaddr %#x "
                                "nextline_vaddr %#x\n",
                                nextlineLevel, nextRead, entry.vaddr, nextlineVaddr);
                        return fault;
                    } else {
                        DPRINTF(PageTableWalker,"no pre\n");
                    }
                }
                }
            } else {
                DPRINTF(PageTableWalker, "Translated %#x -> %#x\n",
                        entry.vaddr, entry.paddr << PageShift |
                        (entry.vaddr & mask(entry.logBytes)));
            }
        }
        endWalk();
    } else if (nextline) {
        if (fault == NoFault) {
            Addr nextline_basic_vaddr = nextlineEntry.vaddr;
            for (int n_l2_i = 0; n_l2_i < l2tlbLineSize; n_l2_i++) {
                nextlineEntry.vaddr =
                    ((nextline_basic_vaddr >> ((nextlineEntry.level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET))
                                                  << L2TLB_BLK_OFFSET) +
                     n_l2_i)
                    << ((nextlineEntry.level * LEVEL_BITS + PageShift));
                l2pte = read->getLE_l2tlb<uint64_t>(n_l2_i);
                nextlineEntry.paddr = l2pte.ppn;
                nextlineEntry.pte = l2pte;
                if (walker->tlb != nullptr &&
                    walker->tlb->isMptTlbInfoEnabled() &&
                    globalMPT.mmpt.mode != 0 && mptInfo.valid) {
                    Addr checkedMptSlot =
                        mptPermSlotAlign(PaddrUT, mptInfo.mptlevel);
                    Addr entryMptSlot =
                        mptPermSlotAlign(nextlineEntry.paddr << PageShift,
                                         mptInfo.mptlevel);
                    nextlineEntry.mptInfo =
                        (entryMptSlot == checkedMptSlot) ? mptInfo : MPTInfoInTLB();
                } else {
                    nextlineEntry.mptInfo = MPTInfoInTLB();
                }
                if (nextlineEntry.level == 0) {
                    nextlineEntry.index =
                        (nextlineEntry.vaddr >> (PageShift + L2TLB_BLK_OFFSET)) & (walker->tlb->L2TLB_L0_MASK);
                    walker->tlb->L2TLBInsert(nextlineEntry.vaddr, nextlineEntry, nextlineLevel, L_L2L0, n_l2_i, false,
                                             direct);
                } else if (nextlineEntry.level == 1) {
                    panic("nextline level can't be 1\n");
                } else if (nextlineEntry.level == 2) {
                    panic("nextline level can't be 2\n");
                } else if (nextlineEntry.level == 3) {
                    panic("nextline level can't be 3\n");
                }

                DPRINTF(PageTableWalker, "nextline vaddr %#x paddr %#x pte %#x\n", nextlineEntry.vaddr,
                        nextlineEntry.paddr, nextlineEntry.pte);
            }
        } else {
            endWalk();
            return NoFault;
        }
        endWalk();
    } else {
        //If we didn't return, we're setting up another read.
        if (!walker->reservePtwLevel(this, level)) {
            if (waitForPtwLevel(level, nextRead, oldRead->getSize(), flags)) {
                delete oldRead;
                oldRead = nullptr;
                read = nullptr;
                walker->retryPtwLevelBlockedStates();
                return fault;
            }
        }
        walker->retryPtwLevelBlockedStates();
        walker->retryPtwMissQueue();
        RequestPtr request = std::make_shared<Request>(
            nextRead, oldRead->getSize(), flags, walker->requestorId);
        if (nextRead == 0)
            panic("nextread can't be 0\n");
        DPRINTF(PageTableWalker, "oldread size %d\n", oldRead->getSize());

        delete oldRead;
        oldRead = nullptr;

        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();
        fault = startPteReadMPTCheck();
        if (fault != NoFault)
            return fault;

        DPRINTF(PageTableWalker, "Loading level%d PTE from %#x vaddr %#x\n",
                level, nextRead, entry.vaddr);
    }
    return fault;
}

void
Walker::WalkerState::endWalk()
{
    nextState = Ready;
    delete read;
    read = NULL;
    walker->releasePtwLevel(this);
}

bool
Walker::WalkerState::usePtwLevelLimit() const
{
    return timing && (translateMode == defaultmode ||
                      translateMode == twoStageMode) &&
           !fromPre && !fromBackPre &&
           mainReq && !mainReq->isPrefetch();
}

int
Walker::WalkerState::currentPtwResourceLevel() const
{
    if (translateMode == twoStageMode && inGstage)
        return twoStageLevel;
    return level;
}

bool
Walker::WalkerState::waitForPtwLevel(int target_level, Addr next_read,
                                     unsigned read_size,
                                     Request::Flags flags)
{
    if (!usePtwLevelLimit())
        return false;

    waitingForPtwLevel = true;
    blockedPtwLevel = target_level;
    blockedPtwRead = next_read;
    blockedPtwReadSize = read_size;
    blockedPtwFlags = flags;
    state = Waiting;
    nextState = Translate;
    DPRINTF(PageTableWalker,
            "PTW level%d busy, defer read %#lx for vaddr %#lx\n",
            target_level, next_read, mainReq->getVaddr());
    return true;
}

bool
Walker::WalkerState::deferPtwLevelRead(int target_level, Addr next_read,
                                       unsigned read_size,
                                       Request::Flags flags,
                                       PacketPtr old_read)
{
    if (walker->reservePtwLevel(this, target_level))
        return false;

    if (!waitForPtwLevel(target_level, next_read, read_size, flags))
        return false;

    PacketPtr pending_read = old_read;
    if (!pending_read && read && read->isResponse())
        pending_read = read;
    if (pending_read) {
        if (read == pending_read)
            read = nullptr;
        delete pending_read;
    }
    return true;
}

bool
Walker::WalkerState::retryBlockedPtwLevel()
{
    if (!waitingForPtwLevel)
        return false;

    if (!walker->ptwLevelAvailable(this, blockedPtwLevel))
        return false;
    if (!walker->reservePtwLevel(this, blockedPtwLevel))
        return false;

    panic_if(inflight != 0,
             "Blocked PTW state has in-flight packet when retried\n");
    if (!read) {
        panic_if(blockedPtwRead == 0,
                 "Blocked PTW read address should not be zero\n");
        RequestPtr request = std::make_shared<Request>(
            blockedPtwRead, blockedPtwReadSize, blockedPtwFlags,
            walker->requestorId);
        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();
    }

    DPRINTF(PageTableWalker,
            "Resume blocked PTW level%d read %#lx for vaddr %#lx\n",
            blockedPtwLevel, read->getAddr(), mainReq->getVaddr());

    waitingForPtwLevel = false;
    blockedPtwLevel = -1;
    blockedPtwRead = 0;
    blockedPtwReadSize = 0;
    blockedPtwFlags = Request::PHYSICAL;

    Fault fault = startPteReadMPTCheck();
    if (fault != NoFault) {
        mainFault = fault;
        return true;
    }
    if (!isMPTing)
        sendPackets();
    return true;
}
Fault
Walker::WalkerState::endGstageWalk()
{
    endWalk();
    GstageFault = true;
    return pageFault(true, true);
}
Fault
Walker::WalkerState::startTwoStageWalkFromTLBNotInG(Addr ppn, Addr vaddr)
{
    Addr PgBase = ppn << 12;
    Addr pg_mask = 0;
    Fault fault = NoFault;
    Addr nextRead = 0;
    inGstage = false;

    if (twoStageLevel > 0) {
        pg_mask = ((1ULL << (12 + 9 * twoStageLevel)) - 1);
        if (((ppn << 12) & pg_mask) != 0) {
            // missaligned superpage
            warn("missaligned superpage vaddr %lx\n", entry.vaddr);
            fault = pageFault(true, false);
            endWalk();
            panic("address check wrong in from tlb ptw\n");
            return fault;
        }
        PgBase = (PgBase & ~pg_mask) | (gPaddr & pg_mask & ~PGMASK);
    }
    PgBase = PgBase | (gPaddr & PGMASK);
    vaddr_choose_flag = (PgBase & 0x3f) / 8;
    nextRead = (PgBase >> 6) << 6;
    gPaddr = nextRead;
    if ((gPaddr & ~H_VADDR_MASK(vsatp.mode)) != 0) {
        // this is a excep
        panic("address check wrong in from tlb ptw\n");
    }
    DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx\n", gPaddr, entry.vaddr);
    gpaddrMode = 1;
    mainReq->setgPaddr(gPaddr);

    if (nextRead == 0)
        panic("nextread can't be 0\n");
    Request::Flags flags = Request::PHYSICAL;
    if (deferPtwLevelRead(level, nextRead, 64, flags, nullptr))
        return NoFault;

    RequestPtr request = std::make_shared<Request>(nextRead, 64, flags, walker->requestorId);
    DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk nextRead %lx vaddr %lx gpaddr %lx level %d twolevel %d\n",
            nextRead, entry.vaddr, gPaddr, level, twoStageLevel);
    read = new Packet(request, MemCmd::ReadReq);
    read->allocate();
    fault = startPteReadMPTCheck();
    if (fault != NoFault)
        return fault;
    DPRINTF(PageTableWalker, "Loading level%d PTE from %#x vaddr %#x\n", level, nextRead, entry.vaddr);
    return NoFault;
}
Fault
Walker::WalkerState::startTwoStageWalkFromTLBInG(Addr ppn, Addr vaddr)
{
    // vaddr_choose = (gPaddr >> (twoStageLevel * LEVEL_BITS + PageShift)) & VADDR_CHOOSE_MASK;
    Addr nextRead = (ppn << PageShift) + (getGVPNi(hgatp.mode, gPaddr, twoStageLevel) * PTESIZE);
    Request::Flags flags = Request::PHYSICAL;
    nextRead = (nextRead >> 6) << 6;
    if (nextRead == 0)
        panic("nextread can't be 0\n");
    if (deferPtwLevelRead(twoStageLevel, nextRead, 64, flags, nullptr))
        return NoFault;

    RequestPtr request = std::make_shared<Request>(nextRead, 64, flags, walker->requestorId);
    read = new Packet(request, MemCmd::ReadReq);
    read->allocate();
    return startPteReadMPTCheck();
}

Fault
Walker::WalkerState::startTwoStageWalk(Addr ppn, Addr vaddr)
{
    Fault fault = NoFault;
    Addr shift = PageShift + LEVEL_BITS * twoStageLevel;
    Addr idx;
    inGstage = true;

    idx = (((gPaddr >> shift) & TWO_STAGE_L2_LEVEL_MASK) >> 3) << 3;
    if (hgatp.mode == AddrXlateMode::SV39 || hgatp.mode == AddrXlateMode::SV48) {
        Addr TwoLevelTopAddr = 0;
        if ((ppn & ~H_VADDR_MASK(hgatp.mode)) != 0) {
            // this is a excep
            panic("address check wrong in start ptw\n");
        }
        TwoLevelTopAddr = (hgatp.ppn << PageShift) + (idx * sizeof(PTE));

        Request::Flags flags = Request::PHYSICAL;
        RequestPtr request = std::make_shared<Request>(TwoLevelTopAddr, 64, flags, walker->requestorId);
        DPRINTF(PageTableWalkerTwoStage, "startTwoStageWalk: request_addr %#x, vaddr %#x, gpaddr %lx\n",
                TwoLevelTopAddr, vaddr, gPaddr);
        // DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk pte %lx vaddr %lx gpaddr %lx level %d twolevel %d\n",
        //         TwoLevelTopAddr, entry.vaddr, gPaddr, level, twoStageLevel);
        if (TwoLevelTopAddr == 0)
            panic("topAddr can't be 0\n");
        // DPRINTF(PageTableWalker, " sv39 size is %d\n", sizeof(PTE));

        if (deferPtwLevelRead(twoStageLevel, TwoLevelTopAddr, 64, flags, nullptr))
            return NoFault;

        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();
        fault = startPteReadMPTCheck();
        if (fault != NoFault)
            return fault;

    } else {
        panic("hgatp.mode != 8 or 9\n");
    }
    return NoFault;
}

Fault
Walker::WalkerState::setupWalk(Addr ppn, Addr vaddr, int f_level, bool from_l2tlb, bool open_nextline,
                               bool auto_open_nextline, bool from_forward_pre_req, bool from_back_pre_req)
{
    Addr topAddr;
    int top_level;
    if (translateMode == twoStageMode) {
        panic_if(vsatp.mode == AddrXlateMode::BARE, "should be processed in isVsatpOMode.");
        top_level = PTW_TOP_LEVEL(vsatp.mode);
    } else {
        top_level = satp.mode == AddrXlateMode::SV48 ? 3: 2;
    }

    if (from_l2tlb){
        level = f_level;
    }
    else {
        level = top_level;
        if (mainReq && mainReq->get_level() != top_level)
            level = mainReq->get_level();
        if (isVsatp0Mode)
            level = 0;
    }

    Addr shift = PageShift + LEVEL_BITS * level;
    Addr idx_f = (vaddr >> shift) & LEVEL_MASK;
    Addr idx = (idx_f >> 3) << 3;
    Fault fault = NoFault;
    if (translateMode == twoStageMode) {
        nextline = false;
        autoNextlineSign = false;
        preHitInPtw = false;
        nextline = false;
        topAddr = (vsatp.ppn << PageShift) + (idx * sizeof(PTE));
        gPaddr = (vsatp.ppn << PageShift) + (idx_f * sizeof(PTE));
        if ((mainReq->get_level() != top_level) && (mainReq->getgPaddr() != 0)) {
            gPaddr = mainReq->getgPaddr();
        }
        if (isVsatp0Mode) {
            gPaddr = vaddr;
        }

        // DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx level %d\n", gPaddr, vaddr, level);
        mainReq->setgPaddr(gPaddr);
        gpaddrMode = 0;
        nextlineLevelMask = LEVEL_MASK;
        nextlineShift = shift;
        tlbVaddr = vaddr;
        tlbppn = ppn;
        nextlineRead = 0;
        nextlineLevel = level;

        state = Translate;
        nextState = Ready;
        entry.vaddr = vaddr;
        entry.asid = vsatp.asid;
        entry.isSquashed = false;
        entry.used = false;
        entry.isPre = false;
        entry.fromForwardPreReq = false;
        entry.fromBackPreReq = false;
        entry.preSign = false;
        entry.vmid = hgatp.vmid;

        inl2Entry.vaddr = vaddr;
        inl2Entry.asid = vsatp.asid;
        inl2Entry.isSquashed = false;
        inl2Entry.used = false;
        inl2Entry.isPre = false;
        inl2Entry.fromForwardPreReq = false;
        inl2Entry.fromBackPreReq = false;
        inl2Entry.preSign = false;
        inl2Entry.vmid = hgatp.vmid;
        inl2Entry.paddr = 0;

        finishGVA = mainReq->get_finish_gva();
        level = mainReq->get_level();
        twoStageLevel = mainReq->get_two_stage_level();
        inGstage = mainReq->get_h_gstage();

        DPRINTF(PageTableWalker, "setupWalk for vaddr %#x: "
                "inGstage %d, level %d, twoStageLevel %d, finishGVA %d, gPaddr %#x\n",
                vaddr, inGstage, level, twoStageLevel, finishGVA, gPaddr);

        if (finishGVA){
            entry.pteVS = mainReq->get_pte();
            inl2Entry.pteVS = mainReq->get_pte();
        }
        if ((!isVsatp0Mode) && inGstage && (twoStageLevel != top_level)) {
            fault = startTwoStageWalkFromTLBInG(mainReq->get_ppn(), vaddr);
        } else if ((!isVsatp0Mode) && (!inGstage) && (level != top_level)) {
            fault = startTwoStageWalkFromTLBNotInG(mainReq->get_ppn(), vaddr);
        } else if ((level == top_level) || (isVsatp0Mode)) {
            fault = startTwoStageWalk(gPaddr, vaddr);
        } else {
            fault = startTwoStageWalk(gPaddr, vaddr);
        }
        if (fault != NoFault) {
            endWalk();
            return fault;
        }
    } else {
        DPRINTF(PageTableWalker, "setupWalk: ppn %#x, vaddr %#x, level %d\n", ppn, vaddr, level);

        vaddr = VADDR_SEXT(satp.mode, vaddr);
        twoStageLevel = 0;

        nextline = false;
        autoNextlineSign = auto_open_nextline;
        preHitInPtw = false;

        if (from_l2tlb) {
            topAddr = (ppn << PageShift) + (idx * sizeof(PTE));
            nextlineLevelMask = LEVEL_MASK;
            nextlineShift = shift;
            tlbVaddr = vaddr;
            tlbppn = ppn;
            nextlineRead = topAddr;
            nextlineLevel = level;
        } else {
            topAddr = (satp.ppn << PageShift) + (idx * sizeof(PTE));
            nextlineLevelMask = LEVEL_MASK;
            nextlineShift = shift;
            tlbVaddr = vaddr;
            tlbppn = satp.ppn;
            nextlineRead = topAddr;
            nextlineLevel = level;
        }

        DPRINTF(PageTableWalker,
                "Performing table walk for address %#x shift %d idx_f %#x ppn %#x "
                "satp.ppn %#x\n",
                vaddr, shift, idx_f, ppn, satp.ppn);
        DPRINTF(PageTableWalker, "Loading level%d PTE from %#x idx %#x idx_shift %#x vaddr %#x\n", level, topAddr, idx,
                idx << shift, vaddr);

        state = Translate;
        nextState = Ready;
        entry.vaddr = vaddr;
        entry.asid = satp.asid;
        entry.isSquashed = false;
        entry.used = false;
        entry.isPre = false;
        entry.fromForwardPreReq = from_forward_pre_req;
        entry.fromBackPreReq = from_back_pre_req;
        entry.preSign = false;


        nextlineEntry.vaddr = vaddr;
        nextlineEntry.asid = satp.asid;
        nextlineEntry.isSquashed = false;
        nextlineEntry.used = false;
        nextlineEntry.isPre = true;
        nextlineEntry.fromBackPreReq = from_back_pre_req;
        nextlineEntry.preSign = false;

        inl2Entry.asid = satp.asid;
        inl2Entry.isSquashed = false;
        inl2Entry.used = false;
        inl2Entry.isPre = false;
        inl2Entry.fromBackPreReq = from_back_pre_req;
        inl2Entry.preSign = false;
        finishDefaultTranslate = false;
        Request::Flags flags = Request::PHYSICAL;
        RequestPtr request = std::make_shared<Request>(topAddr, 64, flags, walker->requestorId);
        if (topAddr == 0)
            panic("topAddr can't be 0\n");
        DPRINTF(PageTableWalker, " pte size is %d\n", sizeof(PTE));

        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();
        fault = startPteReadMPTCheck();
        if (fault != NoFault)
            return fault;
    }
    return NoFault;
}

bool
Walker::WalkerState::recvPacket(PacketPtr pkt)
{

    if (pkt!=read){
        assert(pkt->isResponse());
        assert(inflight);
        assert(state == Waiting);
        inflight--;
    }

    Addr l2vpn_0 = 0;
    int squashed_num = 0;
    int request_num = 0;

    if (requestors.size() == 0) {
        // if were were squashed, return true once inflight is zero and
        // this WalkerState will be freed there.
        DPRINTF(PageTableWalker,
                "%#lx (pc=%#lx) has been previously squashed, inflight=%u\n",
                mainReq->getVaddr(), mainReq->getPC(), inflight);
        return (inflight == 0);
    }
    if (pkt->isRead()) {
        // should not have a pending read it we also had one outstanding

        // @todo someone should pay for this

        if (pkt!=read){
            assert(!read);
            pkt->headerDelay = pkt->payloadDelay = 0;
            state = nextState;
            nextState = Ready;
            read = pkt;
        }
        PacketPtr write = NULL;
        if ((translateMode == twoStageMode) && (inGstage)) {
            DPRINTF(PageTableWalker, "recvPacket in twoStageMode: Gstage.\n");
            mainFault = twoStageStepWalk(write);
        } else if ((translateMode == twoStageMode) && (!inGstage)) {
            DPRINTF(PageTableWalker, "recvPacket in twoStageMode: VSstage.\n");
            mainFault = twoStageWalk(write);
        } else {
            DPRINTF(PageTableWalker, "recvPacket in defaultMode.\n");
            mainFault = stepWalk(write);
        }
        state = Waiting;
        assert(mainFault == NoFault || read == NULL);
        if (write) {
            writes.push_back(write);
        }
        if (isMPTing && !(mptCheckingPteRead && pteReadMptIsNextline))
            return false;
        if (!isMPTing)
            sendPackets();
    } else {
        if (pkt->isError() && mainFault == NoFault)
        {
            if (pkt->req->hasVaddr()) {
                mainFault = walker->pmp->createAddrfault(
                    pkt->req->getVaddr(), mode);
            } else {
                mainFault = walker->pmp->createAddrfault(entry.vaddr, mode);
            }
        }
        DPRINTF(PageTableWalker, "pkt->isError && NOfault\n");

        delete pkt;

        sendPackets();
    }
    if (waitingForPtwLevel)
        return false;

    if ((inflight == 0 && read == NULL && writes.size() == 0) && (translateMode == twoStageMode)) {
        state = Ready;
        nextState = Waiting;
        for (auto &r : requestors) {
            if (mainFault == NoFault) {
                Addr vaddr = r.req->getVaddr();
                //Addr paddr = entry.paddr << PageShift | (vaddr & mask(entry.logBytes));
                Addr paddr = entry.paddr << PageShift | (vaddr & 0xfff);
                r.req->setPaddr(paddr);
                walker->pma->check(r.req);
                mainFault = walker->pmp->pmpCheck(r.req, mode, pmode, r.tc);
                if (mainFault != NoFault) {
                    warn("paddr overflow vaddr: %lx paddr: lx\n", vaddr, paddr);
                    r.translation->finish(mainFault, r.req, r.tc, mode);
                    panic("paddr overflow\n");
                    return false;
                }
                r.translation->finish(mainFault, r.req, r.tc, mode);
            }
            else{
                r.fault = pteReadMptFaultPending ?
                    mainFault : pageFaultOnRequestor(r, GstageFault);
                r.translation->finish(r.fault, r.req, r.tc, mode);
                DPRINTF(PageTableWalkerTwoStage, "translate fault vaddr %lx\n", mainReq->getVaddr());
            }
        }
        pteReadMptFaultPending = false;
        return true;
    }
    if ((inflight == 0 && read == NULL && writes.size() == 0) &&
        (!nextline)) {
        state = Ready;
        nextState = Waiting;
        //int flag_squashed =0;
        DPRINTF(PageTableWalker3,
                " !next_line All ops finished for table walk of %#lx "
                "(pc=%#lx), requestor "
                "size: %lu\n",
                mainReq->getVaddr(), mainReq->getPC(), requestors.size());
        DPRINTF(PageTableWalker3,
                "!nextline finished ptw for %#x finished Dec is %d\n",
                (mainReq->getVaddr() >> 12) << 12,
                (mainReq->getVaddr() >> 12) << 12);
        for (auto &r : requestors) {
            if ((!r.fromForwardPreReq) && (!r.fromBackPreReq)) {
                if (mainFault == NoFault) {
                    /*
                     * Finish the translation. Now that we know the right entry
                     * is in the TLB, this should work with no memory accesses.
                     * There could be new faults unrelated to the table walk
                     * like permissions violations, so we'll need the return
                     * value as well.
                     */
                    Addr vaddr = r.req->getVaddr();
                    vaddr = VADDR_SEXT(satp.mode, vaddr);//donothing

                    if (r.translation->squashed()) {
                        squashed_num++;
                    }
                    request_num++;
                    Addr paddr = walker->tlb->translateWithTLB(vaddr, satp.asid, mode, direct);
                    r.req->setPaddr(paddr);
                    walker->pma->check(r.req);

                    // do pmp check if any checking condition is met.
                    // mainFault will be NoFault if pmp checks are
                    // passed, otherwise an address fault will be returned.
                    mainFault =
                        walker->pmp->pmpCheck(r.req, mode, pmode, r.tc);
                    if (mainFault == NoFault && !MPTresult && globalMPT.mmpt.mode != 0){
                        mainFault=walker->createMPTPagefault(vaddr, paddr, mode);
                    }
                    if (mainFault != NoFault) {
                        // Prefetched assert is ignored
                        if (entry.fromForwardPreReq || entry.fromBackPreReq) {
                            // TLB are not finished to prevent memory leaks
                            warn("tlb-req paddr overflow "
                                "vaddr: %lx paddr: %lx\n", vaddr, paddr);
                            return true;
                        } else {
                            warn("paddr overflow "
                                "vaddr: %lx paddr: %lx\n", vaddr, paddr);
                            r.translation->finish(mainFault, r.req, r.tc, mode);
                            return false;
                        }
                    }
                    // Let the CPU continue.
                    DPRINTF(PageTableWalker,
                            "Finished walk for %#lx (pc=%#lx) Paddr %#x\n",
                            r.req->getVaddr(), r.req->getPC(), paddr);
                    r.translation->finish(mainFault, r.req, r.tc, mode);
                } else {
                    // There was a fault during the walk. Let the CPU know.
                    DPRINTF(PageTableWalker,
                            "Finished fault walk for %#lx (pc=%#lx)\n",
                            r.req->getVaddr(), r.req->getPC());
                    // recreate the fault to ensure that the faulting address matches
                    r.fault = pteReadMptFaultPending ?
                        mainFault : pageFaultOnRequestor(r, false);
                    r.translation->finish(r.fault, r.req, r.tc, mode);
                }

            } else {
                DPRINTF(PageTableWalker,
                        "the req from pre Finished walk for %#lx (pc=%#lx)\n",
                        r.req->getVaddr(), r.req->getPC());
            }
        }
        DPRINTF(PageTableWalker, "finish all walk return true\n");
        pteReadMptFaultPending = false;
        return true;
    }
    if (nextline) {
        if ((inflight == 0 && read == NULL && writes.size() == 0)) {
            DPRINTF(PageTableWalker,
                    "next_line All ops finished for table walk of %#lx (pc=%#lx), requestor size: %lu\n",
                    mainReq->getVaddr(), mainReq->getPC(), requestors.size());
            DPRINTF(PageTableWalker, "finished ptw for %#x finished Dec is %d\n", (mainReq->getVaddr() >> 12) << 12,
                    (mainReq->getVaddr() >> 12) << 12);
            state = Ready;
            nextState = Waiting;
            return true;
        } else {
            for (auto &r : requestors) {
                if (r.fromForwardPreReq != r.req->get_forward_pre_tlb()) {
                    panic( "wrong pref vaddr %lx prevaddr %lx\n", r.req->getVaddr(),r.req->getForwardPreVaddr());
                }
                if (mainFault == NoFault) {
                    /*
                     * Finish the translation. Now that we know the right entry
                     * is in the TLB, this should work with no memory accesses.
                     * There could be new faults unrelated to the table walk
                     * like permissions violations, so we'll need the return
                     * value as well.
                     */
                    Addr vaddr = r.req->getVaddr();
                    vaddr = VADDR_SEXT(satp.mode, vaddr);
                    if (r.translation->squashed()) {
                        squashed_num++;
                    }
                    request_num++;
                    Addr paddr = walker->tlb->translateWithTLB(vaddr, satp.asid, mode, direct);
                    r.req->setPaddr(paddr);
                    walker->pma->check(r.req);

                    // do pmp check if any checking condition is met.
                    // mainFault will be NoFault if pmp checks are
                    // passed, otherwise an address fault will be returned.
                    mainFault =
                        walker->pmp->pmpCheck(r.req, mode, pmode, r.tc);
                    if (mainFault == NoFault && !MPTresult && globalMPT.mmpt.mode != 0) {
                        mainFault=walker->createMPTPagefault(vaddr, paddr, mode);
                    }
                    assert(mainFault == NoFault);

                    // Let the CPU continue.
                    DPRINTF(PageTableWalker,

                            "Finished walk for %#lx (pc=%#lx), requestors size: %lu, ws: %p\n",
                            r.req->getVaddr(), r.req->getPC(), requestors.size(), this);

                    r.translation->finish(mainFault, r.req, r.tc, mode);
                } else {
                    // There was a fault during the walk. Let the CPU know.
                    DPRINTF(PageTableWalker,
                            "Finished fault walk for %#lx (pc=%#lx), requestors size: %lu\n",
                            r.req->getVaddr(), r.req->getPC(), requestors.size());

                    // recreate the fault to ensure that the faulting address matches
                    r.fault = pteReadMptFaultPending ?
                        mainFault : pageFaultOnRequestor(r, false);
                    r.translation->finish(r.fault, r.req, r.tc, mode);
                }
            }
            pteReadMptFaultPending = false;
        }
    }
    return false;
}

bool
Walker::WalkerState::finishPteReadMPTFault()
{
    Addr faultPaddr = mptCheckPaddr;
    isMPTing = false;
    finishMPTing = true;
    mptCheckingPteRead = false;
    pteReadMptResult = false;
    pteReadMptFaultPending = false;

    if (read != nullptr) {
        delete read;
        read = nullptr;
    }

    if (pteReadMptIsNextline) {
        pteReadMptIsNextline = false;
        state = Ready;
        nextState = Waiting;
        return true;
    }

    for (auto &r : requestors) {
        if (r.fromForwardPreReq || r.fromBackPreReq || r.translation == nullptr) {
            continue;
        }
        Fault fault = walker->createMPTPagefault(
            r.req->getVaddr(), faultPaddr, mptFaultMode);
        r.translation->finish(fault, r.req, r.tc, mode);
    }
    requestors.clear();
    state = Ready;
    nextState = Waiting;
    return true;
}

Fault
Walker::WalkerState::startPteReadMPTCheck()
{
    if (!timing || globalMPT.mmpt.mode == 0 || read == nullptr ||
        !read->isRequest() || !read->isRead() || read->req->isMptWalk()) {
        return NoFault;
    }

    Addr pteReadPaddr = read->req->getPaddr();
    if (pteReadMptChecked && pteReadMptCheckedPaddr == pteReadPaddr) {
        return NoFault;
    }

    mptCheckPaddr = pteReadPaddr;
    mptCheckMode = BaseMMU::Read;
    mptFaultMode = mode;
    mptCheckingPteRead = true;
    pteReadMptResult = false;
    pteReadMptIsNextline = nextline;
    pteReadMptFaultPending = false;
    isMPTing = true;
    finishMPTing = false;
    mpt_level = 3;

    DPRINTF(PageTableWalker,
            "Check MPT permission before PTW PTE read paddr %#lx\n",
            pteReadPaddr);

    if (!startMPTwalk()) {
        if (mptCacheHitPending)
            return NoFault;
        walker->releasePtwLevel(this);
        walker->retryPtwLevelBlockedStates();
        walker->retryPtwMissQueue();
        return NoFault;
    }

    isMPTing = false;
    finishMPTing = true;
    mptCheckingPteRead = false;
    if (!pteReadMptResult) {
        if (pteReadMptIsNextline) {
            delete read;
            read = nullptr;
            nextline = false;
            pteReadMptIsNextline = false;
            return NoFault;
        }
        Fault fault = walker->createMPTPagefault(
            entry.vaddr, pteReadPaddr, mptFaultMode);
        pteReadMptFaultPending = true;
        delete read;
        read = nullptr;
        return fault;
    }

    pteReadMptChecked = true;
    pteReadMptCheckedPaddr = pteReadPaddr;
    pteReadMptIsNextline = false;
    pteReadMptFaultPending = false;
    return NoFault;
}

void
// This detours the MMU sendPackets path for MPT packet handling.
Walker::WalkerState::sendPacketsMPT()
{
    if (finishMPTing) {
        completeMPTWalk();
        return;
    }
    if (startMPTwalk()) {
        finishMPTing = true;
        completeMPTWalk();
    }
}

bool Walker::WalkerState::startMPTwalk(){
    assert(mptCheckPaddr != 0);
    Addr paForMPTCheck = mptCheckPaddr;
    PaddrUT= paForMPTCheck ;
    MPTCache52* cache = globalMPTCache;
    assert(cache != nullptr);
    auto [hit, cacheEntry] = cache->fetchDelayed(
        paForMPTCheck, requestors.front().tc, walker->pma, walker->pmp,
        mpt_level, this);
    if (hit){
        const bool retryingCacheHit = mptCacheHitRetry;
        mptCacheHitRetry = false;
        DPRINTF(PageTableWalker, "MPT cace hit\n");
        int mptlevel = cacheEntry.level;
        if (!cacheEntry.valid) {
            DPRINTF(PageTableWalker,
                    "MPTCache fetch failed: paddr=%#lx\n",
                    paForMPTCheck);
            return false;
        }
        if (!cacheEntry.mpte.isLeaf()) {
            DPRINTF(PageTableWalker,
                    "MPTCache hit internode paddr=%#lx\n",
                    paForMPTCheck);
            return false;
        }
        uint8_t pi = (paForMPTCheck >> getPageShiftForLevel(mptlevel)) & 0xF;
        // NAPOT is already handled internally.
        uint8_t rawPerm = cacheEntry.mpte.perms(pi);
        uint8_t perm = effectiveMptPerm(rawPerm);
        DPRINTF(PageTableWalker,
                "MPTCache hit leaf paddr=%#lx rawPerm:%#x effectivePerm:%#x\n",
                paForMPTCheck, rawPerm, perm);
        if (mptCheckingPteRead) {
            pteReadMptResult = mptPermAllows(perm, mptCheckMode);
        } else {
            MPTresult = mptPermAllows(perm, mptCheckMode);
            mptInfo.write_mpt_raw(perm, mptlevel);
        }

        if (!retryingCacheHit && timing &&
            MPT_CACHE_HIT_LATENCY != Cycles(0)) {
            scheduleMptCacheHit();
            return false;
        }
        return true;
    }else{
        DPRINTF(PageTableWalker, "MPT cache all miss for inter mpt check(read only),setup mptwalk\n");
        //mpt_level = 3;
        //PaddrUT= paForMPTCheck;
        //globalMPT.walk(mpt_level,globalMPT.mmpt.ppn,PaddrUT,requestors.front().tc, walker->pma, walker->pmp,this);
        return false;
    }
}

void
Walker::WalkerState::scheduleMptCacheHit()
{
    if (mptCacheHitPending)
        return;

    if (mptCacheHitEvent == nullptr) {
        mptCacheHitEvent = new EventFunctionWrapper(
            [this] {
                mptCacheHitPending = false;
                if (read != nullptr && read->isResponse())
                    mptCacheHitRetry = true;
                // Use the same completion path as an asynchronous MPT
                // memory response.  In particular, a final-leaf response
                // may complete the walk and remove this state from
                // Walker::currStates.
                globalMPT.completeMptWaiter(this);
            }, name() + ".mpt_cache_hit");
    }

    mptCacheHitPending = true;
    walker->schedule(*mptCacheHitEvent,
                     curTick() + walker->cyclesToTicks(MPT_CACHE_HIT_LATENCY));
}

bool Walker::WalkerState::LastMPTwalk(){
    isMPTing= true;
    DPRINTF(PageTableWalker, "inner-LastMPTwalk");
    if (startMPTwalk()){
        return true;
    }
    return false;
}

bool
Walker::WalkerState::completeMPTOnly()
{
    finishMPTing = true;
    isMPTing = false;
    state = Ready;
    nextState = Waiting;

    if (walker->tlb->isMptTlbInfoEnabled() && mptInfo.valid &&
        mptOnlyHasEntry) {
        mptOnlyEntry.mptInfo = mptInfo;
    }

    for (auto &r : requestors) {
        panic_if(r.translation == nullptr,
                 "MPT-only timing request has no translation object\n");

        r.req->setPaddr(mptCheckPaddr);
        walker->pma->check(r.req);
        Fault fault = walker->pmp->pmpCheck(
            r.req, mode, pmode, r.tc);
        if (fault == NoFault && !MPTresult) {
            fault = walker->createMPTPagefault(
                r.req->getVaddr(), mptCheckPaddr, mode);
        }

        if (fault == NoFault && walker->enableL1L2replace) {
            if (mptOnlyHasEntry) {
                TlbEntry l1Entry;
                if (walker->tlb->isL1DirectCompressionEnabled() &&
                    walker->tlb->buildSingleL1CompressedEntry(
                        r.req->getVaddr(), mptOnlyEntry, direct, l1Entry)) {
                    walker->tlb->insert(
                        l1Entry.vaddr, l1Entry, false, direct);
                    walker->tlb->recordL1CompressedEntry(l1Entry);
                } else if (!walker->tlb->isL1DirectCompressionEnabled()) {
                    walker->tlb->insert(
                        mptOnlyEntry.vaddr, mptOnlyEntry, false, direct);
                }
            }
            if (mptOnlyHasVsstageEntry) {
                walker->tlb->insert(
                    mptOnlyVsstageEntry.vaddr, mptOnlyVsstageEntry,
                    false, vsstage);
            }
            if (mptOnlyHasGstageEntry) {
                walker->tlb->insert(
                    mptOnlyGstageEntry.gpaddr, mptOnlyGstageEntry,
                    false, gstage);
            }
        }

        DPRINTF(PageTableWalker,
                "Finish MPT-only check vaddr %#lx paddr %#lx pc %#lx "
                "allowed %d\n",
                r.req->getVaddr(), mptCheckPaddr, r.req->getPC(),
                fault == NoFault);
        r.translation->finish(fault, r.req, r.tc, mode);
    }
    requestors.clear();
    return true;
}

bool Walker::WalkerState::completeMPTWalk()
{
    finishMPTing = true;

    if (mptOnly) {
        return completeMPTOnly();
    }

    /*
     * MPT may be protecting either the already returned PTW response packet
     * or a newly created PTW request. Only the former should re-enter
     * recvPacket(); the latter should be sent as a normal request.
     */
    if (read == nullptr) {
        isMPTing = false;
        sendPackets();
        return inflight == 0 && writes.size() == 0 && requestors.empty();
    }

    if (read->isResponse()) {
        /*
         * Keep isMPTing set while the original PTW response is consumed.
         * stepWalk() uses it to avoid starting the same MPT check again.
         */
        return recvPacket(read);
    }

    if (read->isRequest()) {
        if (mptCheckingPteRead) {
            if (!pteReadMptResult) {
                return finishPteReadMPTFault();
            }
            pteReadMptChecked = true;
            pteReadMptCheckedPaddr = mptCheckPaddr;
            mptCheckingPteRead = false;
            pteReadMptIsNextline = false;
        }
        isMPTing = false;
        sendPackets();
        return false;
    }

    panic("MPT completed with non-request/non-response packet: %s\n",
          read->print());
}

void
Walker::WalkerState::sendPackets()
{
    if (waitingForPtwLevel)
        return;

    //If we're already waiting for the port to become available, just return.
    if (retrying)
        return;

    //Reads always have priority
    if (read) {
        PacketPtr pkt = read;
        if (!pkt->isRequest()) {
            panic("PTW tried to send non-request read packet after MPT "
                  "state transition: pkt=%s isMPTing=%d finishMPTing=%d "
                  "state=%d nextState=%d\n",
                  pkt->print(), isMPTing, finishMPTing, state, nextState);
        }
        const int resource_level = currentPtwResourceLevel();
        if (!walker->reservePtwLevel(this, resource_level)) {
            if (waitForPtwLevel(resource_level, pkt->getAddr(),
                                pkt->getSize(), pkt->req->getFlags())) {
                return;
            }
        }
        read = NULL;
        inflight++;
        if (!walker->sendTiming(this, pkt)) {
            retrying = true;
            read = pkt;
            DPRINTF(PageTableWalker, "Port busy, defer read %#lx\n", read->getAddr());
            inflight--;
            return;
        } else {
            DPRINTF(PageTableWalker, "Send read %#lx\n", pkt->getAddr());
        }
    }

    //Send off as many of the writes as we can.
    while (writes.size()) {
        PacketPtr write = writes.back();
        if (!write->isRequest()) {
            panic("PTW tried to send non-request write packet after MPT "
                  "state transition: pkt=%s isMPTing=%d finishMPTing=%d "
                  "state=%d nextState=%d\n",
                  write->print(), isMPTing, finishMPTing, state, nextState);
        }
        writes.pop_back();
        inflight++;
        if (!walker->sendTiming(this, write)) {
            retrying = true;
            writes.push_back(write);
            DPRINTF(PageTableWalker, "Port busy, defer write %#lx\n", write->getAddr());
            inflight--;
            return;
        } else {
            DPRINTF(PageTableWalker, "Send write %#lx\n", write->getAddr());
        }
    }
}

unsigned
Walker::WalkerState::numInflight() const
{
    return inflight;
}

bool
Walker::WalkerState::isRetrying()
{
    return retrying;
}

bool
Walker::WalkerState::isTiming()
{
    return timing;
}

bool
Walker::WalkerState::wasStarted()
{
    return started;
}

void
Walker::WalkerState::retry()
{
    retrying = false;
    DPRINTF(PageTableWalker, "Start retry\n");
    sendPackets();
}

Fault
Walker::WalkerState::pageFaultOnRequestor(RequestorState &r, bool G)
{
    Addr gpaddr = mainReq->getgPaddr();
    Addr page_start = (entry.vaddr >> PageShift) << PageShift;
    if (G && (gpaddrMode == 0) && isVsatp0Mode) {
        Addr vaddr = 0;
        if (r.req->isInstFetch()) {
            if (r.req->getPC() < page_start) {
                vaddr = page_start;
            } else {
                vaddr = r.req->getPC();
            }

        } else {
            vaddr = r.req->getVaddr();
        }
        gpaddr = ((mainReq->getgPaddr() >> 12) << 12) | (vaddr & 0xfff);
    }
    if (r.req->isInstFetch()) {
        if (r.req->getPC() < page_start) {
            // expected: instruction crosses the page boundary
            if (!r.req->getPC() + 4 >= entry.vaddr) {
                warn("Unexepected fetch page fault: PC: %#x, Page: %#x\n", r.req->getPC(), entry.vaddr);
            }
            return walker->tlb->createPagefault(page_start, gpaddr, mode, G);
        } else {
            return walker->tlb->createPagefault(r.req->getPC(), gpaddr, mode, G);
        }
    } else {
        return walker->tlb->createPagefault(r.req->getVaddr(), gpaddr, mode, G);
    }
}

Addr
Walker::WalkerState::getGVPNi(Addr vaddr, int level)
{
    warn("use new getGVPNi, this function is only used for sv39.");
    if (level == 2)
        return vaddr >> VpniShift(level) & TWO_STAGE_L2_LEVEL_MASK;
    else
        return vaddr >> VpniShift(level) & VPN_MASK;
}

Addr
Walker::WalkerState::getGVPNi(uint8_t addrXlateMode, Addr vaddr, int level)
{
    if ((addrXlateMode == AddrXlateMode::SV48 && level == 3) ||
        (addrXlateMode == AddrXlateMode::SV39 && level == 2))
        return vaddr >> VpniShift(level) & TWO_STAGE_L2_LEVEL_MASK;
    else
        return vaddr >> VpniShift(level) & VPN_MASK;
}


Addr
Walker::WalkerState::VpniShift(int level)
{
    return PGSHFT + LEVEL_BITS * level;
}

Fault
Walker::WalkerState::pageFault(bool present,bool G)
{
    bool found_main = false;
    for (auto &r: requestors) {
        DPRINTF(PageTableWalker, "Mark page fault for req %#lx (pc=%#lx).\n",
                r.req->getVaddr(), r.req->getPC());
        auto _fault = pageFaultOnRequestor(r, G);
        if (r.req->getVaddr() == mainReq->getVaddr()) {
            mainFault = _fault;
            found_main = true;
        } else {
            DPRINTF(PageTableWalker, "req addr: %#lx main addr: %#lx\n",
                    r.req->getVaddr(), mainReq->getVaddr());
        }
    }
    assert(found_main);
    return mainFault;
}

} // namespace RiscvISA
} // namespace gem5

#if MPT_ENABLED
//MPT globalMPT;
gem5::RiscvISA::MPT gem5::RiscvISA::globalMPT;
#endif

gem5::RiscvISA::MPTCache52* gem5::RiscvISA::globalMPTCache = nullptr;
