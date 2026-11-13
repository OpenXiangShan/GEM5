// Copyright (c) 2026 Beijing Institute of Open Source Chip (BOSC)
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>

#include "arch/riscv/faults.hh"
#include "cpu/o3/cpu.hh"
#include "cpu/o3/fetch.hh"
#include "cpu/o3/tea.hh"
#include "debug/DecoupleBP.hh"
#include "debug/Fetch.hh"
#include "sim/system.hh"

namespace gem5::o3
{

bool
Fetch::useFetchGroups() const
{
    return enableTwoFetch && !isTraceMode();
}

void
Fetch::resolveTwoFetchIcache()
{
    fatal_if(!icachePort.isConnected(), "Two fetch requires a connected instruction port");
    twoFetchIcache = BaseCache::fromCpuSidePort(icachePort.getPeer());
    fatal_if(!twoFetchIcache || !twoFetchIcache->supportsTwoFetch(),
             "Two fetch requires a directly connected classic read-only 64B L1 ICache");
}

void
Fetch::retryLegacyTag(PacketPtr pkt)
{
    if (!useFetchGroups() || !pkt->icacheTagPortRejected)
        return;
    ++fetchStats.twoFetchTagWaits;
    if (!legacyTagRetry->scheduled())
        cpu->schedule(*legacyTagRetry, twoFetchIcache->clockEdge(Cycles(1)));
}

void
Fetch::clearFetchGroup(bool cancelled)
{
    if (groupTagRetry && groupTagRetry->scheduled())
        cpu->deschedule(*groupTagRetry);
    if (!fetchGroup)
        return;
    // Accepted packets belong to the memory system. Their sender state carries
    // the generation, so a late response cannot access a replacement group.
    if (cancelled && fetchGroup->count == 2 && fetchGroup->current == 0)
        ++fetchStats.twoFetchCancelled;
    for (auto &fragment : fetchGroup->fragments) {
        if (fragment.pending) {
            delete fragment.pending->popSenderState();
            delete fragment.pending;
        }
    }
    fetchGroup.reset();
}

void
Fetch::rejectGroupPair(two_fetch::Reject reason)
{
    ++fetchStats.twoFetchRejected[static_cast<unsigned>(reason)];
    DPRINTF(DecoupleBP, "TwoFetch pair rejected: reason=%u\n", static_cast<unsigned>(reason));
}

bool
Fetch::startFetchGroup(ThreadID tid)
{
    using two_fetch::Reject;
    assert(!fetchGroup && tid == 0);
    const auto first = dbpbtb->ftqFetchBlock(tid);
    if (two_fetch::controlAlreadyPassed(first.taken, first.controlPC, threads[tid].fetchpc->instAddr())) {
        // A preceding 32b instruction can consume the first halfword of this
        // block. A control predicted in that halfword is a false BTB hit.
        dbpbtb->nonControlSquash(first.ftqId, *threads[tid].fetchpc, 0, tid, 0);
        threads[tid].cacheReq.reset();
        threads[tid].valid = false;
        return true;
    }
    const auto end0 =
        two_fetch::effectiveEnd(first.startPC, first.endPC, first.taken, first.controlPC, first.controlSize);
    // Cross-page windows retain the legacy single-block/exception behavior.
    if (!end0 || (first.startPC >> 12) != ((*end0 - 1) >> 12)) {
        rejectGroupPair(end0 ? Reject::CrossPage : Reject::Range);
        return false;
    }

    threads[tid].cacheReq.reset();
    fetchGroup = std::make_unique<FetchGroup>();
    auto &group = *fetchGroup;
    group.generation = ++groupGeneration;
    group.blocks[0] = first;
    group.ends[0] = *end0;
    ++fetchStats.twoFetchAttempts;
    if (dbpbtb->ftqHasNext(tid)) {
        const auto second = dbpbtb->ftqFetchBlock(tid, 1);
        const auto end1 =
            two_fetch::effectiveEnd(second.startPC, second.endPC, second.taken, second.controlPC, second.controlSize);
        const auto reason = end1 ? two_fetch::canPair(first.startPC, *end0, first.taken ? first.target : first.endPC,
                                                      second.startPC, *end1, twoFetchMaxBytes)
                                 : Reject::Range;
        if (reason == Reject::None) {
            group.blocks[1] = second;
            group.ends[1] = *end1;
            group.count = 2;
        } else {
            rejectGroupPair(reason);
        }
    } else {
        rejectGroupPair(Reject::NoNext);
    }

    // Merge reads of the same virtual line. Keep block membership separately:
    // byte reuse never reduces the logical size charged for either block.
    for (unsigned block = 0; block < group.count; ++block) {
        for (Addr pc = group.blocks[block].startPC; pc < group.ends[block];) {
            const Addr end = std::min(group.ends[block], (pc & ~Addr(63)) + 64);
            auto it = std::find_if(group.fragments.begin(), group.fragments.end(),
                                   [pc](const auto &f) { return (f.request->getVaddr() >> 6) == (pc >> 6); });
            if (it == group.fragments.end()) {
                GroupFragment fragment;
                fragment.request = std::make_shared<Request>(pc, end - pc, Request::INST_FETCH, cpu->instRequestorId(),
                                                             first.startPC, cpu->thread[tid]->contextId());
                fragment.request->taskId(cpu->taskId());
                fragment.blockMask = 1U << block;
                group.fragments.push_back(std::move(fragment));
            } else {
                const Addr begin = std::min(pc, it->request->getVaddr());
                const Addr merged_end = std::max(end, it->request->getVaddr() + it->request->getSize());
                it->request->setVirt(begin, merged_end - begin, Request::INST_FETCH, cpu->instRequestorId(),
                                     first.startPC);
                it->request->setContext(cpu->thread[tid]->contextId());
                it->blockMask |= 1U << block;
            }
            pc = end;
        }
    }
    assert(group.fragments.size() <= 4);
    groupLineRequestsCreated += group.fragments.size();
    group.initialFragments = group.fragments.size();
    setThreadStatus(tid, WaitingCache);
    threads[tid].valid = false;
    const auto generation = group.generation;
    const auto count = group.fragments.size();
    for (unsigned i = 0; i < count; ++i) {
        if (!fetchGroup || fetchGroup->generation != generation)
            break;
        translateGroupFragment(i);
    }
    return true;
}

void
Fetch::translateGroupFragment(unsigned index)
{
    const auto request = fetchGroup->fragments[index].request;
    auto *translation = new GroupTranslation(*this, fetchGroup->generation, index);
    cpu->mmu->translateTiming(request, cpu->thread[0]->getTC(), translation, BaseMMU::Execute);
}

void
Fetch::finishGroupTranslation(uint64_t generation, unsigned index, const Fault &fault, bool delayed)
{
    if (!fetchGroup || fetchGroup->generation != generation)
        return;
    auto &fragment = fetchGroup->fragments.at(index);
    fragment.fault = fault;
    fragment.state = fault == NoFault ? FragmentState::Ready : FragmentState::Failed;
    if (cpu->getTea() && delayed)
        fragment.tea |= TeaEvent::bit(TeaEvent::DR_TLB);
    if (fetchGroup->finalized && fault == NoFault) {
        if (!cpu->system->isMemAddr(fragment.request->getPaddr())) {
            fragment.fault =
                std::make_shared<RiscvISA::AddressFault>(fragment.request->getVaddr(), 0, RiscvISA::INST_ACCESS);
            fragment.state = FragmentState::Failed;
        } else if (fragment.request->isUncacheable() || fragment.request->isStrictlyOrdered()) {
            const Addr start = fetchGroup->blocks[fetchGroup->current].startPC;
            clearFetchGroup();
            threads[0].valid = false;
            fetchCacheLine(start, 0, threads[0].fetchpc->instAddr());
            return;
        }
    }
    if (std::any_of(fetchGroup->fragments.begin(), fetchGroup->fragments.end(),
                    [](const auto &f) { return f.state == FragmentState::Translating; }))
        return;
    if (!fetchGroup->finalized)
        finalizeFetchGroup();
    else
        sendGroupPackets();
}

void
Fetch::finalizeFetchGroup()
{
    auto &group = *fetchGroup;
    // No timing packets have been sent yet. Revert to the old single-block
    // path on translation/uncacheable exceptions, preserving precise traps.
    const bool exception = std::any_of(group.fragments.begin(), group.fragments.end(), [this](const auto &f) {
        return f.fault != NoFault || f.request->isUncacheable() || f.request->isStrictlyOrdered() ||
               !cpu->system->isMemAddr(f.request->getPaddr());
    });
    if (exception) {
        const Addr start = group.blocks[0].startPC;
        rejectGroupPair(two_fetch::Reject::Exception);
        clearFetchGroup();
        fetchCacheLine(start, 0, threads[0].fetchpc->instAddr());
        return;
    }

    if (group.count == 2) {
        std::array<std::vector<icache_banks::ReadRange>, 2> ranges;
        for (unsigned b = 0; b < 2; ++b) {
            for (const auto &f : group.fragments) {
                if (!(f.blockMask & (1U << b)))
                    continue;
                const Addr begin = std::max(group.blocks[b].startPC, f.request->getVaddr());
                const Addr end = std::min(group.ends[b], f.request->getVaddr() + f.request->getSize());
                ranges[b].push_back({begin, f.request->getPaddr() + begin - f.request->getVaddr(),
                                     unsigned(end - begin), f.request->isSecure()});
            }
        }
        if (twoFetchIcache->twoFetchDataConflict(ranges)) {
            rejectGroupPair(two_fetch::Reject::Bank);
            group.count = 1;
            group.fragments.erase(std::remove_if(group.fragments.begin(), group.fragments.end(),
                                                 [](const auto &f) { return !(f.blockMask & 1); }),
                                  group.fragments.end());
            // Trim shared line reads back to the first block's actual range.
            for (auto &f : group.fragments) {
                const Addr begin = std::max(group.blocks[0].startPC, f.request->getVaddr());
                const Addr end = std::min(group.ends[0], f.request->getVaddr() + f.request->getSize());
                const Addr pa = f.request->getPaddr() + begin - f.request->getVaddr();
                const auto flags = f.request->getFlags();
                f.request->setVirt(begin, end - begin, flags, cpu->instRequestorId(), group.blocks[0].startPC);
                f.request->setContext(cpu->thread[0]->contextId());
                f.request->setPaddr(pa);
                f.blockMask = 1;
            }
        }
    }
    group.initialFragments = group.fragments.size();
    group.finalized = true;
    DPRINTF(DecoupleBP,
            "TwoFetch group %lu: blocks=%u FTQ=%lu/%lu "
            "ranges=[%#lx,%#lx)/[%#lx,%#lx)\n",
            group.generation, group.count, group.blocks[0].ftqId, group.blocks[1].ftqId, group.blocks[0].startPC,
            group.ends[0], group.blocks[1].startPC, group.ends[1]);
    sendGroupPackets();
}

void
Fetch::sendGroupPackets()
{
    if (!fetchGroup || !fetchGroup->finalized || fetchGroup->cacheWait)
        return;
    auto &group = *fetchGroup;
    for (unsigned i = 0; i < group.fragments.size(); ++i) {
        auto &fragment = group.fragments[i];
        if (fragment.state != FragmentState::Ready)
            continue;
        if (!fragment.pending) {
            auto *pkt = new Packet(fragment.request, MemCmd::ReadReq);
            pkt->dataDynamic(new uint8_t[pkt->getSize()]);
            pkt->setSendRightAway();
            pkt->pushSenderState(new GroupSenderState(group.generation, i));
            fragment.pending = pkt;
        }
        auto *pkt = fragment.pending;
        pkt->tagReadFail = false;
        if (!icachePort.sendTimingReq(pkt)) {
            if (pkt->icacheTagPortRejected) {
                ++fetchStats.twoFetchTagWaits;
                if (!groupTagRetry->scheduled())
                    cpu->schedule(*groupTagRetry, twoFetchIcache->clockEdge(Cycles(1)));
            } else {
                group.cacheWait = true;
            }
            return;
        }
        fragment.pending = nullptr;
        fragment.state = FragmentState::Sent;
        ppFetchRequestSent->notify(fragment.request);
        if (!group.issued) {
            group.issued = true;
            group.firstSend = twoFetchIcache->curCycle();
            if (group.count == 2)
                ++fetchStats.twoFetchIssued;
            else
                ++fetchStats.singleFetchIssued;
        }
        if (i + 1 == group.initialFragments && group.count == 2 && group.firstSend == twoFetchIcache->curCycle())
            ++fetchStats.twoFetchSameCycleAccepted;
    }
    updateGroupReady();
}

void
Fetch::completeGroupPacket(PacketPtr pkt)
{
    auto *state = static_cast<GroupSenderState *>(pkt->popSenderState());
    const uint64_t generation = state->generation;
    const unsigned index = state->fragment;
    delete state;
    if (fetchGroup && fetchGroup->generation == generation) {
        auto &fragment = fetchGroup->fragments.at(index);
        fragment.data.assign(pkt->getConstPtr<uint8_t>(), pkt->getConstPtr<uint8_t>() + pkt->getSize());
        fragment.state = FragmentState::Complete;
        if (cpu->getTea() && pkt->cacheAccessDepth > 0)
            fragment.tea |= TeaEvent::bit(TeaEvent::DR_L1);
        updateGroupReady();
    }
    delete pkt;
}

void
Fetch::updateGroupReady()
{
    if (!fetchGroup)
        return;
    if (std::all_of(fetchGroup->fragments.begin(), fetchGroup->fragments.end(), [](const auto &f) {
            return f.state == FragmentState::Complete || f.state == FragmentState::Failed;
        })) {
        threads[0].valid = true;
        setThreadStatus(0, Running);
        cpu->wakeCPU();
        switchToActive();
    }
}

bool
Fetch::groupByte(Addr pc, uint8_t &value) const
{
    for (const auto &f : fetchGroup->fragments) {
        if (f.state == FragmentState::Complete && pc >= f.request->getVaddr() &&
            pc - f.request->getVaddr() < f.data.size()) {
            value = f.data[pc - f.request->getVaddr()];
            return true;
        }
    }
    return false;
}

void
Fetch::supplementGroup(Addr pc, unsigned size)
{
    auto &group = *fetchGroup;
    // One tail is sufficient for each of the two block boundaries. Supplements
    // are never counted as another paired block and never read functionally.
    assert(group.fragments.size() < 8);
    GroupFragment fragment;
    fragment.request = std::make_shared<Request>(pc, size, Request::INST_FETCH, cpu->instRequestorId(),
                                                 threads[0].fetchpc->instAddr(), cpu->thread[0]->contextId());
    fragment.blockMask = 1U << group.current;
    fragment.request->taskId(cpu->taskId());
    group.fragments.push_back(std::move(fragment));
    ++groupLineRequestsCreated;
    ++fetchStats.twoFetchSupplements;
    threads[0].valid = false;
    setThreadStatus(0, WaitingCache);
    translateGroupFragment(group.fragments.size() - 1);
}

StallReason
Fetch::supplyGroupBytes(ThreadID tid, PCStateBase &pc)
{
    if (!threads[tid].valid)
        return StallReason::IcacheStall;
    const Addr addr = pc.instAddr();
    for (const auto &f : fetchGroup->fragments) {
        if (f.state == FragmentState::Failed) {
            const auto request = f.request;
            const Fault fault = f.fault;
            const auto tea = takeGroupTea(addr, 4);
            clearFetchGroup();
            threads[tid].valid = false;
            threads[tid].cacheReq.reset();
            threads[tid].cacheReq.addRequest(request);
            threads[tid].cacheReq.teaPsv[0] = tea;
            handleTranslationFault(tid, request, fault);
            return StallReason::TrapStall;
        }
    }
    uint8_t bytes[4]{};
    for (unsigned i = 0; i < 2; ++i) {
        if (!groupByte(addr + i, bytes[i])) {
            supplementGroup(addr + i, 2 - i);
            return StallReason::IcacheStall;
        }
    }
    const unsigned size = (bytes[0] & 3) == 3 ? 4 : 2;
    for (unsigned i = 2; i < size; ++i) {
        if (!groupByte(addr + i, bytes[i])) {
            const unsigned missing = std::min<unsigned>(size - i, 64 - ((addr + i) & 63));
            supplementGroup(addr + i, missing);
            return StallReason::IcacheStall;
        }
    }
    // Preserve start-PC FTQ ownership even when a 32b instruction straddles a
    // sequential boundary; lookupAndUpdateNextPC advances to the next block.
    if (fetchGroup->blocks[fetchGroup->current].taken && addr == fetchGroup->blocks[fetchGroup->current].controlPC &&
        size != fetchGroup->blocks[fetchGroup->current].controlSize)
        fetchGroup->noCross = true;
    memcpy(decoder[tid]->moreBytesPtr(), bytes, sizeof(bytes));
    decoder[tid]->moreBytes(pc, addr);
    return StallReason::NoStall;
}

uint16_t
Fetch::takeGroupTea(Addr pc, unsigned size)
{
    uint16_t events = 0;
    for (auto &f : fetchGroup->fragments) {
        if (pc < f.request->getVaddr() + f.request->getSize() && f.request->getVaddr() < pc + size) {
            events |= f.tea;
            f.tea = 0;
        }
    }
    return events;
}

}  // namespace gem5::o3
