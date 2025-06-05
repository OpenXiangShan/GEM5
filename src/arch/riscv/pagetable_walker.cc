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

#include <memory>
#include <numeric>

#include "arch/riscv/faults.hh"
#include "arch/riscv/page_size.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/tlb.hh"
#include "base/bitfield.hh"
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

namespace gem5
{

namespace RiscvISA {

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
            if (!newState->isTiming()) {
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
        if (!newState->isTiming()) {
            currStates.pop_front();
            delete newState;
        }
        return fault;
    }
}

void
Walker::doL2TLBHitSchedule(const RequestPtr &req, ThreadContext *tc, BaseMMU::Translation *translation,
                           BaseMMU::Mode mode, Addr Paddr, TlbEntry *entry, TlbEntry *entryVsstage,
                           TlbEntry *entryGstage, int delaytick)
{
    DPRINTF(PageTableWalker2, "schedule %d\n", curCycle());
    Tick hitdelay = curTick() + cyclesToTicks(Cycles(delaytick));
    if (!doL2TLBHitEvent.scheduled()) {
        schedule(doL2TLBHitEvent, hitdelay);
    }
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
    WalkerSenderState * senderState =
        dynamic_cast<WalkerSenderState *>(pkt->popSenderState());
    DPRINTF(PageTableWalker,
            "Received timing response for sender state: %#lx\n", senderState);
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
        delete senderWalk;
        // Since we block requests when another is outstanding, we
        // need to check if there is a waiting request to be serviced

    }
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
}

bool Walker::sendTiming(WalkerState* sendingState, PacketPtr pkt)
{
    WalkerSenderState* walker_state = new WalkerSenderState(sendingState);
    DPRINTF(PageTableWalker, "Sending packet %#x with sender state: %lx\n",
            pkt->getAddr(), walker_state);
    pkt->pushSenderState(walker_state);
    if (port.sendTimingReq(pkt)) {
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
    } else {
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
        assert(satp.mode == AddrXlateMode::SV39);
        fromPre = _from_forward_pre_req;
        fromBackPre = _from_back_pre_req;
        translateMode = defaultmode;
        hgatp = _tc->readMiscReg(MISCREG_HGATP);
        isHInst = false;
        isVsatp0Mode = false;
        GstageFault = false;
        tlbHit = false;
        assert(functional || !_req->get_h_inst());
    }
}


std::pair<bool, Fault>
Walker::WalkerState::tryCoalesce(ThreadContext *_tc, BaseMMU::Translation *translation, const RequestPtr &req,
                                 BaseMMU::Mode _mode, bool from_l2tlb, Addr asid, bool from_forward_pre_req,
                                 bool from_back_pre_req)
{

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
    while (!L2TLBrequestors.empty()) {
        L2TlbState dol2TLBHitrequestors = L2TLBrequestors.front();
        Fault l2tlbFault;
        PrivilegeMode pmodel2 = tlb->getMemPriv(dol2TLBHitrequestors.tc,
                                                dol2TLBHitrequestors.mode);
        dol2TLBHitrequestors.req->setPaddr(dol2TLBHitrequestors.Paddr);
        pma->check(dol2TLBHitrequestors.req);
        l2tlbFault =
            pmp->pmpCheck(dol2TLBHitrequestors.req, dol2TLBHitrequestors.mode,
                          pmodel2, dol2TLBHitrequestors.tc);
        //assert(l2tlbFault == NoFault);
        if (l2tlbFault == NoFault) {
            if (enableL1L2replace){
                if (dol2TLBHitrequestors.entry != nullptr)
                    tlb->insert(dol2TLBHitrequestors.entry->vaddr, *dol2TLBHitrequestors.entry, false, direct);
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
        L2TLBrequestors.pop_front();
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
        if (from_back_req) {
            setupWalk(ppn, mainReq->getBackPreVaddr(), f_level, from_l2tlb, open_nextline, auto_open_nextline,
                      from_forward_req, from_back_req);
        } else if (from_forward_req) {
            setupWalk(ppn, mainReq->getForwardPreVaddr(), f_level, from_l2tlb, open_nextline, auto_open_nextline,
                      from_forward_req, from_back_req);
        } else {
            setupWalk(ppn, mainReq->getVaddr(), f_level, from_l2tlb, open_nextline, auto_open_nextline,
                      from_forward_req, from_back_req);
        }
    }
    if (timing) {
        nextState = state;
        state = Waiting;
        mainFault = NoFault;
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
    PTESv39 pte;
    bool doEndWalk = false;
    bool doLLwalk = false;
    Addr PgBase;
    PTESv39 l2pte;
    unsigned oldSize = 64;
    Request::Flags flags = Request::PHYSICAL;

    vaddr_choose = (gPaddr >> (twoStageLevel * LEVEL_BITS + PageShift)) & VADDR_CHOOSE_MASK;
    PacketPtr oldRead = read;
    if (!tlbHit) {
        pte = read->getLE_l2tlb<uint64_t>(vaddr_choose);
        DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk pte %lx vaddr %lx gpaddr %lx\n",
                read->getLE_l2tlb<uint64_t>(vaddr_choose), entry.vaddr, gPaddr);

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

    if (fault == NoFault) {
        if (pte.v && !pte.r && !pte.w && !pte.x) {
            twoStageLevel--;
            if (twoStageLevel < 0) {
                endWalk();
                warn("pagefault in Gstage ptw twostagelevel <0\n");
                return endGstageWalk();
            } else {
                nextRead = (pte.ppn << PageShift) + (getGVPNi(gPaddr, twoStageLevel) * PTESIZE);
                nextcheck = nextRead;
                nextRead = (nextRead >> 6) << 6;
                nextState = Translate;
                if ((!isVsatp0Mode) && (!tlbHit)) {
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
                        if (l2_level == 2) {
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2L1, l2_i, false,
                                                     gstage);
                        } else if (l2_level == 1) {
                            inl2Entry.index =
                                (gPaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) & (walker->tlb->L2TLB_L2_MASK);
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2L2, l2_i, false,
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
            if (twoStageLevel > 0) {
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
            vaddr_choose_flag = (PgBase & 0x3f) / 8;
            nextcheck = PgBase;
            nextRead = (PgBase >> 6) << 6;
            gPaddr = nextRead;
            entry.paddr = gPaddr;
            if (finishGVA && (!isVsatp0Mode)) {
                walker->tlb->insert(entry.gpaddr, entry, false, gstage);
                int l2_level = twoStageLevel;
                inl2Entry.gpaddr = gPaddr;
                inl2Entry.pte = pte;
                inl2Entry.logBytes = PageShift + (l2_level * LEVEL_BITS);
                inl2Entry.level = l2_level;
                if (!tlbHit) {
                    for (int l2_i = 0; l2_i < l2tlbLineSize; l2_i++) {
                        inl2Entry.gpaddr =
                            ((gPaddr >> ((l2_level * LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) << L2TLB_BLK_OFFSET) +
                             l2_i)
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
                                (inl2Entry.gpaddr >> (L2TLB_BLK_OFFSET + PageShift)) & walker->tlb->L2TLB_L3_MASK;
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2L3, l2_i, false,
                                                     gstage);
                        }

                        else if (l2_level == 1) {
                            inl2Entry.index = (inl2Entry.gpaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                              (walker->tlb->L2TLB_L2_MASK);
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2sp2, l2_i, false,
                                                     gstage);
                        }  // hit level =1

                        else if (l2_level == 2) {
                            walker->tlb->L2TLBInsert(inl2Entry.gpaddr, inl2Entry, l2_level, L_L2sp1, l2_i, false,
                                                     gstage);
                        }
                    }
                }
            }

            if ((gPaddr & ~(((int64_t)1 << 41) - 1)) != 0) {
                // this is a excep
                panic("address fault\n");
            }
            DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx\n", gPaddr, entry.vaddr);
            gpaddrMode =1;
            mainReq->setgPaddr(gPaddr);
        }

        if (doLLwalk && finishGVA) {
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
                entry.paddr = gPaddr >> 12;
                entry.pte = pte;
                entry.logBytes = PageShift + (twoStageLevel * LEVEL_BITS);
                entry.level = twoStageLevel;
                entry.gpaddr = entry.vaddr;
                walker->tlb->insert(entry.vaddr, entry, false, gstage);
                endWalk();
                return NoFault;
            }
            if (nextRead == 0)
                panic("nextread can't be 0\n");

            TlbEntry *e[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
            int hit_level = 2;
            bool tlb_hit = false;

            if (walker->l2tlb == nullptr)
                panic("walker->l2tlb is none\n");
            
            if (!tlbHit) {
                delete oldRead;
                oldRead = nullptr;
                RequestPtr request = std::make_shared<Request>(nextRead, oldSize, flags, walker->requestorId);
                DPRINTF(PageTableWalkerTwoStage,
                        "twoStageStepWalk nextRead %lx vaddr %lx gpaddr %lx level %d twolevel %d\n", nextRead,
                        entry.vaddr, gPaddr, level, twoStageLevel);
                DPRINTF(PageTableWalker, "oldread size %d\n", oldSize);

                read = new Packet(request, MemCmd::ReadReq);
                read->allocate();
                DPRINTF(PageTableWalker, "Loading level%d PTE from %#x vaddr %#x\n", level, nextRead, entry.vaddr);
            }
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
    PTESv39 pte;

    PacketPtr oldRead = read;
    Request::Flags flags;

    PTESv39 l2pte;
    unsigned oldSize = 64;

    if (!tlbHit) {
        pte = read->getLE_l2tlb<uint64_t>(vaddr_choose_flag);
        flags = oldRead->req->getFlags();
        walker->pma->check(read->req);
        oldSize = oldRead->getSize();
        fault = walker->pmp->pmpCheck(read->req, BaseMMU::Read, RiscvISA::PrivilegeMode::PRV_S, requestors.front().tc,
                                      gPaddr);

    } else {
        pte = tlbHitPte;
        flags = tlbflags;
    }
    TlbEntry *e[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    int hit_level = 2;
    bool tlb_hit = false;
    Addr shift = 0;
    Addr idx_f = 0;
    Addr idx = 0;
    Addr nextcheck = 0;

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
                                if (l2_level == 0) {
                                    inl2Entry.index = (inl2Entry.vaddr >> (L2TLB_BLK_OFFSET + PageShift)) &
                                                      walker->tlb->L2TLB_L3_MASK;
                                    walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L3, l2_i, false,
                                                             vsstage);
                                } else if (l2_level == 1) {
                                    inl2Entry.index =
                                        (inl2Entry.vaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                        walker->tlb->L2TLB_L2_MASK;
                                    walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp2, l2_i,
                                                             false, vsstage);
                                } else if (l2_level == 2) {
                                    walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp1, l2_i,
                                                             false, vsstage);
                                }
                            }
                        }
                        if ((gPaddr & ~(((int64_t)1 << 41) - 1)) != 0) {
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
                        twoStageLevel = 2;
                        tlbHit = false;
                        nextcheck = 0;
                        shift = PageShift + LEVEL_BITS * twoStageLevel;
                        idx = ((gPaddr >> shift) & TWO_STAGE_L2_LEVEL_MASK);
                        nextcheck = gPaddr;

                        
                        if (!tlbHit) {
                            delete oldRead;
                            oldRead = nullptr;
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
                    entry.gpaddr = gPaddr;
                    entry.pte = pte;
                    entry.logBytes = PageShift + (level * LEVEL_BITS);
                    entry.level = level;

                    shift = (PageShift + LEVEL_BITS * level);
                    idx_f = (entry.vaddr >> shift) & LEVEL_MASK;
                    idx = (idx_f >> L2TLB_BLK_OFFSET) << L2TLB_BLK_OFFSET;
                    gPaddr = (pte.ppn << PageShift) + (idx_f * l2tlbLineSize);
                    entry.paddr = gPaddr;
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
                            if (l2_level == 2) {
                                walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L1, l2_i, false,
                                                         vsstage);
                            } else if (l2_level == 1) {
                                inl2Entry.index = (inl2Entry.vaddr>> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                                  (walker->tlb->L2TLB_L2_MASK);
                                walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L2, l2_i, false,
                                                         vsstage);
                            }
                        }
                    }

                    if ((gPaddr & ~(((int64_t)1 << 41) - 1)) != 0) {
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
                    twoStageLevel = 2;
                    tlbHit = false;


                    shift = PageShift + LEVEL_BITS * twoStageLevel;
                    idx = ((gPaddr >> shift) & TWO_STAGE_L2_LEVEL_MASK);
                    nextcheck = gPaddr;
                    if (!tlbHit) {
                        delete oldRead;
                        oldRead = nullptr;
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
    assert(state != Ready && state != Waiting);
    Fault fault = NoFault;
    write = NULL;
    uint64_t vaddr_choose;

    PTESv39 pte ;

    vaddr_choose = (entry.vaddr >> (level * LEVEL_BITS + PageShift)) & VADDR_CHOOSE_MASK;
    pte = read->getLE_l2tlb<uint64_t>(vaddr_choose);

    Addr nextRead = 0;
    bool doWrite = false;
    bool doTLBInsert = false;
    bool doEndWalk = false;
    int l2_i =0;
    PTESv39 l2pte;
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

    if ((fault == NoFault) && (!nextline)) {
        // step 3:
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
                    } else if (level == 2 && pte.ppn1 != 0) {
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
                        // step 8
                        entry.logBytes = PageShift + (level * LEVEL_BITS);
                        entry.paddr = pte.ppn;
                        entry.vaddr &= ~((1 << entry.logBytes) - 1);
                        entry.pte = pte;
                        entry.level = level;
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
                        if (l2_level == 2) {
                            walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L1, l2_i, false,
                                                     direct);
                        }
                        if (l2_level == 1) {
                            inl2Entry.index = (inl2Entry.vaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                              (walker->tlb->L2TLB_L2_MASK);
                            walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L2, l2_i, false,
                                                     direct);
                        }

                        if (l2_level == 0) {
                            panic("l2_level is 0,may be wrong\n");
                        }
                    }
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

        if (doTLBInsert) {
            if (!functional) {
                if (((!entry.fromForwardPreReq) && (!entry.fromBackPreReq)) || (preHitInPtw)) {
                    walker->tlb->insert(entry.vaddr, entry, false, direct);
                }
                finishDefaultTranslate = true;

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
                    if (l2_level == 0) {
                        inl2Entry.index = (entry.vaddr >> (L2TLB_BLK_OFFSET + PageShift)) & walker->tlb->L2TLB_L3_MASK;
                        walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2L3, l2_i, false, direct);
                    }

                    else if (l2_level == 1)  {
                        inl2Entry.index = (inl2Entry.vaddr >> (LEVEL_BITS + PageShift + L2TLB_BLK_OFFSET)) &
                                              (walker->tlb->L2TLB_L2_MASK);
                        walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp2, l2_i, false, direct);
                    }// hit level =1

                    else if (l2_level == 2)  //
                        walker->tlb->L2TLBInsert(inl2Entry.vaddr, inl2Entry, l2_level, L_L2sp1, l2_i, false, direct);
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
             else {
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
                if (nextlineEntry.level == 0) {
                    nextlineEntry.index =
                        (nextlineEntry.vaddr >> (PageShift + L2TLB_BLK_OFFSET)) & (walker->tlb->L2TLB_L3_MASK);
                    walker->tlb->L2TLBInsert(nextlineEntry.vaddr, nextlineEntry, nextlineLevel, L_L2L3, n_l2_i, false,
                                             direct);
                } else if (nextlineEntry.level == 1) {
                    panic("nextline level can't be 1\n");
                } else if (nextlineEntry.level == 2) {
                    panic("nextline level can't be 2\n");
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
        RequestPtr request = std::make_shared<Request>(
            nextRead, oldRead->getSize(), flags, walker->requestorId);
        if (nextRead == 0)
            panic("nextread can't be 0\n");
        DPRINTF(PageTableWalker, "oldread size %d\n", oldRead->getSize());

        delete oldRead;
        oldRead = nullptr;

        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();

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
    if ((gPaddr & ~(((int64_t)1 << 41) - 1)) != 0) {
        // this is a excep
        panic("address check wrong in from tlb ptw\n");
    }
    DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx\n", gPaddr, entry.vaddr);
    gpaddrMode = 1;
    mainReq->setgPaddr(gPaddr);

    if (nextRead == 0)
        panic("nextread can't be 0\n");
    Request::Flags flags = Request::PHYSICAL;
    RequestPtr request = std::make_shared<Request>(nextRead, 64, flags, walker->requestorId);
    DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk nextRead %lx vaddr %lx gpaddr %lx level %d twolevel %d\n",
            nextRead, entry.vaddr, gPaddr, level, twoStageLevel);
    read = new Packet(request, MemCmd::ReadReq);
    read->allocate();
    DPRINTF(PageTableWalker, "Loading level%d PTE from %#x vaddr %#x\n", level, nextRead, entry.vaddr);
    return NoFault;
}
Fault
Walker::WalkerState::startTwoStageWalkFromTLBInG(Addr ppn, Addr vaddr)
{
    // vaddr_choose = (gPaddr >> (twoStageLevel * LEVEL_BITS + PageShift)) & VADDR_CHOOSE_MASK;
    Addr nextRead = (ppn << PageShift) + (getGVPNi(gPaddr, twoStageLevel) * PTESIZE);
    Request::Flags flags = Request::PHYSICAL;
    nextRead = (nextRead >> 6) << 6;
    if (nextRead == 0)
        panic("nextread can't be 0\n");
    RequestPtr request = std::make_shared<Request>(nextRead, 64, flags, walker->requestorId);
    read = new Packet(request, MemCmd::ReadReq);
    read->allocate();
    return NoFault;
}

Fault
Walker::WalkerState::startTwoStageWalk(Addr ppn, Addr vaddr)
{
    Addr shift = PageShift + LEVEL_BITS * twoStageLevel;
    Addr idx;
    inGstage = true;

    idx = (((gPaddr >> shift) & TWO_STAGE_L2_LEVEL_MASK) >> 3) << 3;
    if (hgatp.mode == 8) {
        Addr TwoLevelTopAddr = 0;
        if ((ppn & ~(((int64_t)1 << 41) - 1)) != 0) {
            // this is a excep
            panic("address check wrong in start ptw\n");
        }
        TwoLevelTopAddr = (hgatp.ppn << PageShift) + (idx * sizeof(PTESv39));

        Request::Flags flags = Request::PHYSICAL;
        RequestPtr request = std::make_shared<Request>(TwoLevelTopAddr, 64, flags, walker->requestorId);
        DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk pte %lx vaddr %lx gpaddr %lx level %d twolevel %d\n",
                TwoLevelTopAddr, entry.vaddr, gPaddr, level, twoStageLevel);
        if (TwoLevelTopAddr == 0)
            panic("topAddr can't be 0\n");
        DPRINTF(PageTableWalker, " sv39 size is %d\n", sizeof(PTESv39));

        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();

    } else {
        panic("hgatp.mode != 8 \n");
    }
    return NoFault;
}

Fault
Walker::WalkerState::setupWalk(Addr ppn, Addr vaddr, int f_level, bool from_l2tlb, bool open_nextline,
                               bool auto_open_nextline, bool from_forward_pre_req, bool from_back_pre_req)
{
    Addr topAddr;
    if (from_l2tlb){
        level = f_level;
    }
    else {
        level = 2;
        if (mainReq && mainReq->get_level() != 2)
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
        topAddr = (vsatp.ppn << PageShift) + (idx * sizeof(PTESv39));
        gPaddr = (vsatp.ppn << PageShift) + (idx_f * sizeof(PTESv39));
        if ((mainReq->get_level() != 2) && (mainReq->getgPaddr() != 0)) {
            gPaddr = mainReq->getgPaddr();
        }
        if (isVsatp0Mode) {
            gPaddr = vaddr;
        }

        DPRINTF(PageTableWalkerTwoStage, "twoStageStepWalk gpaddr %lx vaddr %lx level %d\n", gPaddr, vaddr, level);
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
        if (finishGVA){
            entry.pteVS = mainReq->get_pte();
            inl2Entry.pteVS = mainReq->get_pte();
        }
        if ((!isVsatp0Mode) && (mainReq->get_h_gstage()) && (mainReq->get_two_stage_level() != 2)) {
            fault = startTwoStageWalkFromTLBInG(mainReq->get_ppn(), vaddr);
        } else if ((!isVsatp0Mode) && (!mainReq->get_h_gstage()) && (mainReq->get_level() != 2)) {
            fault = startTwoStageWalkFromTLBNotInG(mainReq->get_ppn(), vaddr);
        } else if ((mainReq->get_level() == 2) || (isVsatp0Mode)) {
            fault = startTwoStageWalk(gPaddr, vaddr);
        } else {
            fault = startTwoStageWalk(gPaddr, vaddr);
        }
        if (fault != NoFault) {
            endWalk();
            return fault;
        }
    } else {
        vaddr = Addr(sext<VADDR_BITS>(vaddr));
        twoStageLevel = 0;

        nextline = false;
        autoNextlineSign = auto_open_nextline;
        preHitInPtw = false;

        if (from_l2tlb) {
            topAddr = (ppn << PageShift) + (idx * sizeof(PTESv39));
            nextlineLevelMask = LEVEL_MASK;
            nextlineShift = shift;
            tlbVaddr = vaddr;
            tlbppn = ppn;
            nextlineRead = topAddr;
            nextlineLevel = level;
        } else {
            topAddr = (satp.ppn << PageShift) + (idx * sizeof(PTESv39));
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
        DPRINTF(PageTableWalker, " sv39 size is %d\n", sizeof(PTESv39));

        read = new Packet(request, MemCmd::ReadReq);
        read->allocate();
    }
    return NoFault;
}

bool
Walker::WalkerState::recvPacket(PacketPtr pkt)
{
    assert(pkt->isResponse());
    assert(inflight);
    assert(state == Waiting);
    inflight--;

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
        assert(!read);

        // @todo someone should pay for this
        pkt->headerDelay = pkt->payloadDelay = 0;

        state = nextState;
        nextState = Ready;
        PacketPtr write = NULL;
        read = pkt;
        if ((translateMode == twoStageMode) && (inGstage)) {
            mainFault = twoStageStepWalk(write);
        } else if ((translateMode == twoStageMode) && (!inGstage)) {
            mainFault = twoStageWalk(write);
        } else {
            mainFault = stepWalk(write);
        }
        state = Waiting;
        assert(mainFault == NoFault || read == NULL);
        if (write) {
            writes.push_back(write);
        }
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
                r.fault = pageFaultOnRequestor(r, GstageFault);
                r.translation->finish(r.fault, r.req, r.tc, mode);
                DPRINTF(PageTableWalkerTwoStage, "translate fault vaddr %lx\n", mainReq->getVaddr());
            }
        }
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
                    vaddr = Addr(sext<VADDR_BITS>(vaddr));

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
                    r.fault = pageFaultOnRequestor(r, false);
                    r.translation->finish(r.fault, r.req, r.tc, mode);
                }

            } else {
                DPRINTF(PageTableWalker,
                        "the req from pre Finished walk for %#lx (pc=%#lx)\n",
                        r.req->getVaddr(), r.req->getPC());
            }
        }
        DPRINTF(PageTableWalker, "finish all walk return true\n");
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
                    vaddr = Addr(sext<VADDR_BITS>(vaddr));
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
                    r.fault = pageFaultOnRequestor(r, false);
                    r.translation->finish(r.fault, r.req, r.tc, mode);
                }
            }
        }
    }
    return false;
}

void
Walker::WalkerState::sendPackets()
{
    //If we're already waiting for the port to become available, just return.
    if (retrying)
        return;

    //Reads always have priority
    if (read) {
        PacketPtr pkt = read;
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
    if (level == 2)
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
