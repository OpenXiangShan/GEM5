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
#include "cpu/base.hh"
#include "cpu/thread_context.hh"
#include "debug/PageTableWalker.hh"
#include "mem/packet_access.hh"
#include "mem/request.hh"

namespace gem5
{

namespace RiscvISA {

std::pair<bool, Fault>
Walker::tryCoalesce(ThreadContext *_tc, BaseMMU::Translation *translation,
                    const RequestPtr &req, BaseMMU::Mode mode)
{
    assert(currStates.size());
    for (auto it: currStates) {
        auto &ws = *it;
        auto [coalesced, fault] = ws.tryCoalesce(_tc, translation, req, mode);
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
              const RequestPtr &_req, BaseMMU::Mode _mode, bool pre,
              int f_level, bool from_l2tlb)
{
    // TODO: in timing mode, instead of blocking when there are other
    // outstanding requests, see if this request can be coalesced with
    // another one (i.e. either coalesce or start walk)
    DPRINTF(PageTableWalker, "Starting page table walk for %#lx\n",
            _req->getVaddr());
    DPRINTF(PageTableWalker, "pre %d f_level %d from_l2tlb %d\n", pre, f_level,
            from_l2tlb);
    pre_ptw = pre;
    if (currStates.size()) {
        auto [coalesced, fault] = tryCoalesce(_tc, _translation, _req, _mode);
        if (!coalesced) {
            // create state
            WalkerState * newState = new WalkerState(this, _translation, _req);
            newState->initState(_tc, _mode, sys->isTimingMode());
            assert(newState->isTiming());
            // TODO: add to requestors
            DPRINTF(PageTableWalker,
                    "Walks in progress: %d, push req pc: %#lx, addr: %#lx "
                    "into currStates\n",
                    currStates.size(), _req->getPC(), _req->getVaddr());
            currStates.push_back(newState);
            Fault fault = newState->startWalk(ppn,f_level,from_l2tlb);
            return NoFault;
        } else {
            DPRINTF(PageTableWalker,
                    "Walks in progress: %d. Coalesce req pc: %#lx, addr: %#lx "
                    "into currStates\n",
                    currStates.size(), _req->getPC(), _req->getVaddr());
            return fault;
        }
    } else {
        WalkerState *newState = new WalkerState(this, _translation, _req);
        newState->initState(_tc, _mode, sys->isTimingMode());
        currStates.push_back(newState);
        Fault fault = newState->startWalk(ppn,f_level,from_l2tlb);
        if (!newState->isTiming()) {
            currStates.pop_front();
            delete newState;
        }
        return fault;
    }
}

Fault
Walker::startFunctional(ThreadContext * _tc, Addr &addr, unsigned &logBytes,
              BaseMMU::Mode _mode)
{
    funcState.initState(_tc, _mode);
    return funcState.startFunctional(addr, logBytes);
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
        if (currStates.size() && !startWalkWrapperEvent.scheduled())
            // delay sending any new requests until we are finished
            // with the responses
            schedule(startWalkWrapperEvent, clockEdge());
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
Walker::WalkerState::initState(ThreadContext * _tc,
        BaseMMU::Mode _mode, bool _isTiming)
{
    assert(state == Ready);
    started = false;
    assert(requestors.back().tc == nullptr);
    requestors.back().tc = _tc;
    mode = _mode;
    timing = _isTiming;
    // fetch these now in case they change during the walk
    status = _tc->readMiscReg(MISCREG_STATUS);
    pmode = walker->tlb->getMemPriv(_tc, mode);
    satp = _tc->readMiscReg(MISCREG_SATP);
    assert(satp.mode == AddrXlateMode::SV39);
    //next_line = false;

}

std::pair<bool, Fault>
Walker::WalkerState::tryCoalesce(ThreadContext *_tc,
                                 BaseMMU::Translation *translation,
                                 const RequestPtr &req, BaseMMU::Mode _mode)
{
    SATP _satp = _tc->readMiscReg(MISCREG_SATP);
    assert(_satp.mode == AddrXlateMode::SV39);
    bool priv_match = mode == _mode && satp == _satp &&
                      pmode == walker->tlb->getMemPriv(_tc, _mode) &&
                      status == _tc->readMiscReg(MISCREG_STATUS);
    bool addr_match = ((req->getVaddr() >> PageShift) << PageShift) ==
                      ((mainReq->getVaddr() >> PageShift) << PageShift);
    if (priv_match && addr_match) {
        // coalesce
        DPRINTF(PageTableWalker,
                "Coalescing walk for %#lx(pc=%#lx) into %#lx(pc=%#lx)\n",
                req->getVaddr(), req->getPC(), mainReq->getVaddr(),
                mainReq->getPC());
        // add to list of requestors
        requestors.emplace_back(_tc, req, translation);
        auto &r = requestors.back();
        Fault new_fault = NoFault;
        if (mainFault != NoFault) {
            // recreate fault for this txn, we don't have pmp yet
            // TODO: also consider pmp's addr fault
            new_fault = pageFaultOnRequestor(r);
        }
        if (requestors.size() == 1) {  // previous requestors are squashed
            DPRINTF(
                PageTableWalker,
                "Replace %#lx(pc=%#lx) with %#lx(pc=%#lx) bc main is squashed",
                mainReq->getVaddr(), mainReq->getPC(), r.req->getVaddr(),
                r.req->getPC());
            mainFault = new_fault;
            mainReq = r.req;
        }
        return std::make_pair(true, new_fault);
    }
    return std::make_pair(false, NoFault);
}

void
Walker::startWalkWrapper()
{
    handlePendingSquash();
    WalkerState *currState = currStates.front();
    if (currState && !currState->wasStarted() && !currState->allRequestorSquashed())
        currState->startWalk(0,2,false);
}

void
Walker::handlePendingSquash()
{
    if (squashHandleTick == curCycle()) {  // ensure only handle only once per cycle
        DPRINTF(PageTableWalker, "Handled squash this cycle, skip\n");
        return;
    }
    unsigned num_squashed = 0;
    auto it = currStates.begin();
    while (it != currStates.end()) {
        auto &ws = *it;  // walker state
        DPRINTF(PageTableWalker,
                "Try to squash table walk for pc (of main req): %#x, addr %#x\n",
                ws->mainReq->getPC(), ws->mainReq->getVaddr());

        auto r_it = ws->requestors.begin();
        bool main_erased = false;
        while (r_it != ws->requestors.end()) {
            auto &r = *r_it;
            if (r.translation->squashed()) {
                DPRINTF(PageTableWalker,
                        "Squashing table walk for addr %#lx(pc=%#lx)\n",
                        r.req->getVaddr(), r.req->getPC());
                // finish it before real ``finish''
                r.translation->finish(
                    std::make_shared<UnimpFault>("Squashed Inst"), r.req,
                    r.tc, ws->mode);
                if (r_it == ws->requestors.begin()) {
                    main_erased = true;
                }
                r_it = ws->requestors.erase(r_it);
            } else {
                r_it++;
            }
        }
        // choose the first as main
        if (main_erased && ws->requestors.size() > 0) {
            ws->mainReq = ws->requestors.front().req;
            ws->mainFault = ws->requestors.front().fault;
        }

        // After this loop, if one requestor is not squashed, it must further
        // be finished in other cycles or on complte

        if (ws->numInflight() == 0 &&
            ws->requestors.size() == 0) {  // all requestors have been squashed
            DPRINTF(PageTableWalker,
                    "Erase walk of addr %#lx(pc=%#lx) bc no in-flight & no "
                    "requestors\n",
                    ws->mainReq->getVaddr(), ws->mainReq->getPC());
            it = currStates.erase(it);
            delete ws;
            num_squashed++;
            if (num_squashed >= numSquashable) {
                break;
            }
        }
        ++it;
    }

    if (it != currStates.end() && (*it)->anyRequestorSquashed() &&
        !handlePendingSquashEvent.scheduled()) {
        // we have a translation squashed, but we cannot remove it this cycle
        DPRINTF(PageTableWalker,
                "Scheduling squash at next cycle %lu, because squash bandwidth "
                "used up. Last handle tick=%lu\n", nextCycle(), squashHandleTick);
        schedule(handlePendingSquashEvent, nextCycle());
    }
    squashHandleTick = curCycle();
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
Walker::WalkerState::startWalk(Addr ppn, int f_level,bool from_l2tlb )
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    setupWalk(ppn,mainReq->getVaddr(),f_level,from_l2tlb);
    if (timing) {
        nextState = state;
        state = Waiting;
        mainFault = NoFault;
        sendPackets();
    } else {
        do {
            walker->port.sendAtomic(read);
            PacketPtr write = NULL;
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
Walker::WalkerState::startFunctional(Addr &addr, unsigned &logBytes)
{
    Fault fault = NoFault;
    assert(!started);
    started = true;
    setupWalk(0,addr,2,false);

    do {
        walker->port.sendFunctional(read);
        // On a functional access (page table lookup), writes should
        // not happen so this pointer is ignored after stepWalk
        PacketPtr write = NULL;
        fault = stepWalk(write);
        assert(fault == NoFault || read == NULL);
        state = nextState;
        nextState = Ready;
    } while (read);
    logBytes = entry.logBytes;
    addr = entry.paddr << PageShift;

    return fault;
}

Fault
Walker::WalkerState::stepWalk(PacketPtr &write)
{
    assert(state != Ready && state != Waiting);
    Fault fault = NoFault;
    write = NULL;
    //PTESv39 pte = read->getLE<uint64_t>();
    uint64_t vaddr_choose;

    PTESv39 pte ;
    //b= read->getLE_l2tlb<uint64_t>(vaddr_choose);

   // if (next_line){
     //   vaddr_choose = (entry.vaddr >> (level*9+12)) &(0x7);

//    }
  //  else{
        vaddr_choose = (entry.vaddr >> (level*9+12)) &(0x7);
        pte = read->getLE_l2tlb<uint64_t>(vaddr_choose);
    //}


    Addr nextRead = 0;
    bool doWrite = false;
    bool doTLBInsert = false;
    bool doEndWalk = false;
    //bool next_line = false;
    int l2_i =0;
    PTESv39 l2pte;
    int l2_level;

    DPRINTF(PageTableWalker,
            "Got level%d PTE: %#x PPN: %#x choose %d vaddr %#x next_line %d "
            "next_vaddr %#x\n",
            level, pte, pte.ppn, vaddr_choose, entry.vaddr, next_line,
            nextline_entry.vaddr);
    // step 2:
    // Performing PMA/PMP checks on physical address of PTE

    if (!next_line){
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
            requestors.front().tc, nextline_entry.vaddr);
    }

    if ((fault == NoFault) && (!next_line)) {
    //if (fault == NoFault) {
        // step 3:
        if (!pte.v || (!pte.r && pte.w)) {
            doEndWalk = true;
            DPRINTF(PageTableWalker, "PTE invalid, raising PF\n");
            fault = pageFault(pte.v);
        }
        else {
            // step 4:
            if (pte.r || pte.x) {
                // step 5: leaf PTE
                doEndWalk = true;
                fault = walker->tlb->checkPermissions(status, pmode,
                                                    entry.vaddr, mode, pte);

                // step 6
                if (fault == NoFault) {
                    if (level >= 1 && pte.ppn0 != 0) {
                        DPRINTF(PageTableWalker,
                                "PTE has misaligned PPN, raising PF\n");
                        fault = pageFault(true);
                    }
                    else if (level == 2 && pte.ppn1 != 0) {
                        DPRINTF(PageTableWalker,
                                "PTE has misaligned PPN, raising PF\n");
                        fault = pageFault(true);
                    }
                }

                if (fault == NoFault) {
                    // step 7
                    if (!pte.a) {
                        DPRINTF(PageTableWalker,
                            "PTE needs to write pte.a,raising PF\n");
                        fault = pageFault(true);
                    }
                    if (!pte.d && mode == BaseMMU::Write) {
                        DPRINTF(PageTableWalker,
                            "PTE needs to write pte.d,raising PF\n");
                        fault = pageFault(true);
                    }
                    // Performing PMA/PMP checks

                    if (doWrite) {

                        // this read will eventually become write
                        // if doWrite is True

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
                        // put it non-writable into the TLB to detect
                        // writes and redo the page table walk in order
                        // to update the dirty flag.
                        if (!pte.d && mode != BaseMMU::Write)
                            entry.pte.w = 0;
                        doTLBInsert = true;
                        DPRINTF(
                            PageTableWalker,
                            "tlb read paddr %#x vaddr %#x pte %#x level %d\n",
                            entry.paddr, entry.vaddr, entry.pte, level);
                    }
                }
            } else {
                level--;
                if (level < 0) {
                    DPRINTF(PageTableWalker, "No leaf PTE found,"
                                                  "raising PF\n");
                    doEndWalk = true;
                    fault = pageFault(true);
                } else {
                    inl2_entry.logBytes =
                        PageShift + ((level + 1) * LEVEL_BITS);
                    l2_level = level + 1;

                    for (l2_i = 0; l2_i < 8; l2_i++) {
                        inl2_entry.vaddr =
                            (((entry.vaddr >> ((l2_level * 9 + 12 + 3)))
                              << 3) +
                             l2_i)
                            << ((l2_level * 9 + 12));
                        DPRINTF(PageTableWalker,
                                "inl2_entry.vaddr %#x entry.vaddr %#x\n",
                                inl2_entry.vaddr, entry.vaddr);

                        l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                        inl2_entry.paddr = l2pte.ppn;
                        inl2_entry.pte = l2pte;
                        if (l2_level == 2) {
                            walker->tlb->L2TLB_insert(inl2_entry.vaddr,
                                                      inl2_entry, l2_level, 1);
                        }
                        if (l2_level == 1) {
                            inl2_entry.index = (entry.vaddr >> 24) & (0x1f);
                            walker->tlb->L2TLB_insert(inl2_entry.vaddr,
                                                      inl2_entry, l2_level, 2);
                        }

                        if (l2_level == 0) {
                            DPRINTF(PageTableWalker,
                                    "l2_level is 0,may be wrong\n");
                            assert(0);
                        }
                    }
                    Addr shift = (PageShift + LEVEL_BITS * level);
                    Addr idx_f = (entry.vaddr >> shift) & LEVEL_MASK;
                    Addr idx = (idx_f >> 3) << 3;

                    nextline_level_mask = LEVEL_MASK;
                    nextline_shift = shift;

                    tlb_vaddr = entry.vaddr;
                    tlb_ppn = pte.ppn;
                    tlb_size_pte = sizeof(pte);

                    nextRead = (pte.ppn << PageShift) + (idx * 8);
                    nextState = Translate;
                    nextline_Read = nextRead;
                    nextline_level = level;
                    DPRINTF(PageTableWalker,
                            "next read of pte pte.ppx %#x,idx %#x,sizeof(pte) "
                            "%#x,nextread %#x\n",
                            pte.ppn, idx, sizeof(pte), nextRead);
                    DPRINTF(PageTableWalker, "tlb_ppn %#x vaddr %#x\n",
                            tlb_ppn, entry.vaddr);
                }
            }
        }
    } else if (next_line) {
        nextline_entry.logBytes =PageShift + (nextline_level * LEVEL_BITS);
        nextline_entry.vaddr &= ~((1 << nextline_entry.logBytes) - 1);
        nextline_entry.level = nextline_level;
    }
    else {
        doEndWalk = true;
    }
    PacketPtr oldRead = read;
    Request::Flags flags = oldRead->req->getFlags();

    if ((doEndWalk)&&(!next_line)) {
    //if (doEndWalk) {
        // If we need to write, adjust the read packet to write the modified
        // value back to memory.
        if (!functional && doWrite) {
            DPRINTF(PageTableWalker, "Writing level%d PTE to %#x: %#x\n",
                level, oldRead->getAddr(), pte);
            write = oldRead;
            write->setLE<uint64_t>(pte);
            write->cmd = MemCmd::WriteReq;
            read = NULL;
            assert(0);
        } else {
            write = NULL;
        }

        if (doTLBInsert) {
            if (!functional) {
                walker->tlb->insert(entry.vaddr, entry);
                DPRINTF(PageTableWalker,"l1tlb vaddr %#x \n",entry.vaddr);
                inl2_entry.logBytes = PageShift + (level * LEVEL_BITS);
                l2_level = level;
                inl2_entry.level = level;
                for (l2_i = 0; l2_i < 8; l2_i++) {
                    inl2_entry.vaddr =
                        ((entry.vaddr >> ((l2_level * 9 + 12 + 3)) << 3) +
                         l2_i)
                        << ((l2_level * 9 + 12));
                    l2pte = read->getLE_l2tlb<uint64_t>(l2_i);
                    DPRINTF(PageTableWalker,
                            "final insert vaddr %#x ppn %#x\n",
                            inl2_entry.vaddr, l2pte.ppn);
                    DPRINTF(PageTableWalker,"level %d l2_level %d\n",
                            level,l2_level);
                    inl2_entry.paddr = l2pte.ppn;
                    inl2_entry.pte = l2pte;
                    if (l2_level == 0) {
                        inl2_entry.index = (entry.vaddr >> 15) & (0x7f);
                        walker->tlb->L2TLB_insert(inl2_entry.vaddr, inl2_entry,
                                                  l2_level, 3);
                    }

                    else if (l2_level == 1)  // hit level =1
                        walker->tlb->L2TLB_insert(inl2_entry.vaddr, inl2_entry,
                                                  l2_level, 5);
                    else if (l2_level == 2)  //
                        walker->tlb->L2TLB_insert(inl2_entry.vaddr, inl2_entry,
                                                  l2_level, 4);
                }
                if (!doWrite) {
                    nextline_vaddr =
                        entry.vaddr + (0x8 << (nextline_level * 9 + 12));
                    Addr read_num_pre =
                        (nextline_vaddr >> ((nextline_level + 1) * 9 + 12));
                    Addr read_num =
                        (entry.vaddr >> ((nextline_level + 1) * 9 + 12));
                    Addr nextline_idx;
                    Addr nextline_idx_f;
                    nextline_idx_f = (nextline_vaddr >> nextline_shift) &
                                     nextline_level_mask;
                    nextline_idx = (nextline_idx_f >> 3) << 3;

                    // nextRead = (tlb_ppn << PageShift) + (nextline_idx *
                    // tlb_size_pte);
                    nextRead = (tlb_ppn << PageShift) + (nextline_idx * 8);

                    DPRINTF(PageTableWalker,
                            "nextline basis is vaddr %#x pre vaddr is %#x "
                            "read_num is %#x\n",
                            entry.vaddr, nextline_vaddr, read_num);
                    DPRINTF(PageTableWalker,
                            "nextline tlb_vaddr %#x entry.vaddr is %#x \n",
                            tlb_vaddr, entry.vaddr);
                    DPRINTF(PageTableWalker,
                            "nextline nextread %#x tlb_ppn %#x "
                            "read_num_pre%#x read_num %#x\n",
                            nextRead, tlb_ppn, read_num_pre, read_num);

                    //if (read_num != 0) {
                    if ((read_num_pre == read_num) && (nextline_level == 0)) {
                        walker->tlb->lookup(entry.vaddr, entry.asid,
                                            BaseMMU::Read, false);
                        next_line = true;
                        nextState = Translate;
                        // nextline_Read =
                        // nextline_vaddr = entry.vaddr +
                        // (0x1<<(nextline_level*9+12));
                        nextline_entry.vaddr =
                            entry.vaddr + (0x8 << (nextline_level * 9 + 12));

                        RequestPtr request = std::make_shared<Request>(
                            nextRead, oldRead->getSize(), flags,
                            walker->requestorId);
                        delete oldRead;
                        oldRead = nullptr;
                        read = new Packet(request, MemCmd::ReadReq);
                        read->allocate();

                        DPRINTF(PageTableWalker,
                                "nextline nextline_vaddr %#x "
                                "nextline_entry.vaddr is %#x\n",
                                nextline_vaddr, nextline_entry.vaddr);
                        DPRINTF(PageTableWalker,
                                "nextline level %d pte from %#x vaddr %#x "
                                "nextline_vaddr %#x\n",
                                nextline_level, nextRead, entry.vaddr,
                                nextline_vaddr);
                        if (nextRead ==0) assert(0);
                        //assert(nextRead ==0);
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
    } else if (next_line) {
        if (fault == NoFault){
            Addr nextline_basic_vaddr = nextline_entry.vaddr;
            for (int n_l2_i = 0; n_l2_i < 8; n_l2_i++) {
                nextline_entry.vaddr =
                    ((nextline_basic_vaddr >>
                      ((nextline_entry.level * 9 + 12 + 3)) << 3) +
                     n_l2_i)
                    << ((nextline_entry.level * 9 + 12));
                l2pte = read->getLE_l2tlb<uint64_t>(n_l2_i);
                nextline_entry.paddr = l2pte.ppn;
                nextline_entry.pte = l2pte;
                if (nextline_entry.level == 0) {
                    nextline_entry.index =
                        (nextline_entry.vaddr >> 15) & (0x7f);
                    walker->tlb->L2TLB_insert(nextline_entry.vaddr,
                                              nextline_entry, nextline_level,
                                              3);
                } else if (nextline_entry.level == 1) {
                    assert(0);
                } else if (nextline_entry.level == 2) {
                    assert(0);
                }
                DPRINTF(PageTableWalker,
                        "nextline vaddr %#x paddr %#x pte %#x\n",
                        nextline_entry.vaddr, nextline_entry.paddr,
                        nextline_entry.pte);
            }
        } else {
            next_line = false;
            endWalk();
            return NoFault;
        }

        next_line = false;
        endWalk();
    } else {
        //If we didn't return, we're setting up another read.
        RequestPtr request = std::make_shared<Request>(
            nextRead, oldRead->getSize(), flags, walker->requestorId);
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

void
Walker::WalkerState::setupWalk(Addr ppn, Addr vaddr, int f_level,
                               bool from_l2tlb)
{
    vaddr = Addr(sext<VADDR_BITS>(vaddr));
    Addr topAddr;
    //level = 2;

    if (from_l2tlb){
        level = f_level;
        //level = 2;
    }
    else {
        level = 2;
    }
    next_line = false;

    //Addr shift = PageShift + LEVEL_BITS * 2;
    //Addr idx = (vaddr >> shift) & LEVEL_MASK;
    //Addr topAddr = (satp.ppn << PageShift) + (idx * sizeof(PTESv39));

   // Addr shift = PageShift + LEVEL_BITS * level +3;
    //Addr idx = (vaddr >> shift) & LEVEL_MASK;
   // Addr idx = (vaddr >> shift) & (LEVEL_MASK >> 3);
    //Addr topAddr = (satp.ppn << PageShift) + (idx << shift);
    Addr shift = PageShift + LEVEL_BITS * level;
    Addr idx_f = (vaddr >> shift) & LEVEL_MASK;
    Addr idx = (idx_f>>3)<<3;

    if (from_l2tlb){
        topAddr = (ppn << PageShift) + (idx * sizeof(PTESv39));
        nextline_level_mask = LEVEL_MASK;
        nextline_shift = shift;
        tlb_vaddr = vaddr;
        tlb_ppn = ppn;
        nextline_Read = topAddr;
        nextline_level = level;


    }
    else{
        topAddr = (satp.ppn << PageShift) + (idx * sizeof(PTESv39));
        nextline_level_mask = LEVEL_MASK;
        nextline_shift = shift;
        tlb_vaddr = vaddr;
        tlb_ppn = satp.ppn;
        nextline_Read = topAddr;
        nextline_level = level;

    }


    //Addr topAddr = (l2_entry.pte.ppn << PageShift) + (idx << shift);

    DPRINTF(PageTableWalker,
            "Performing table walk for address %#x shift %d idx_f %#x ppn %#x "
            "satp.ppn %#x\n",
            vaddr, shift, idx_f, ppn, satp.ppn);
    DPRINTF(PageTableWalker,
            "Loading level%d PTE from %#x idx %#x idx_shift %#x vaddr %#x\n",
            level, topAddr, idx, idx << shift, vaddr);

    state = Translate;
    nextState = Ready;
    entry.vaddr = vaddr;
    entry.asid = satp.asid;
    nextline_entry.vaddr = vaddr;
    nextline_entry.asid = satp.asid;

    Request::Flags flags = Request::PHYSICAL;
    //RequestPtr request = std::make_shared<Request>(
    //    topAddr, sizeof(PTESv39), flags, walker->requestorId);
    RequestPtr request = std::make_shared<Request>(
        topAddr, 64, flags, walker->requestorId);
    DPRINTF(PageTableWalker," sv39 size is %d\n",sizeof(PTESv39));

    //Addr
    read = new Packet(request, MemCmd::ReadReq);
    read->allocate();
}

bool
Walker::WalkerState::recvPacket(PacketPtr pkt)
{
    assert(pkt->isResponse());
    assert(inflight);
    assert(state == Waiting);
    inflight--;
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
        mainFault = stepWalk(write);
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

        delete pkt;

        sendPackets();
    }
    if (inflight == 0 && read == NULL && writes.size() == 0) {
        state = Ready;
        nextState = Waiting;
        DPRINTF(PageTableWalker,
                "All ops finished for table walk of %#lx (pc=%#lx), requestor "
                "size: %lu\n",
                mainReq->getVaddr(), mainReq->getPC(), requestors.size());
        for (auto &r : requestors) {
            if (mainFault == NoFault) {
                /*
                 * Finish the translation. Now that we know the right entry is
                 * in the TLB, this should work with no memory accesses.
                 * There could be new faults unrelated to the table walk like
                 * permissions violations, so we'll need the return value as
                 * well.
                 */
                Addr vaddr = r.req->getVaddr();
                vaddr = Addr(sext<VADDR_BITS>(vaddr));
                Addr paddr =
                    walker->tlb->translateWithTLB(vaddr, satp.asid, mode);
                r.req->setPaddr(paddr);
                walker->pma->check(r.req);

                // do pmp check if any checking condition is met.
                // mainFault will be NoFault if pmp checks are
                // passed, otherwise an address fault will be returned.
                mainFault = walker->pmp->pmpCheck(r.req, mode, pmode, r.tc);
                assert(mainFault == NoFault);

                // Let the CPU continue.
                DPRINTF(PageTableWalker, "Finished walk for %#lx (pc=%#lx)\n",
                        r.req->getVaddr(), r.req->getPC());
                r.translation->finish(mainFault, r.req, r.tc, mode);
            } else {
                // There was a fault during the walk. Let the CPU know.
                DPRINTF(PageTableWalker, "Finished fault walk for %#lx (pc=%#lx)\n",
                        r.req->getVaddr(), r.req->getPC());
                // recreate the fault to ensure that the faulting address matches
                r.fault = pageFaultOnRequestor(r);
                r.translation->finish(r.fault, r.req, r.tc, mode);
            }
        }
        return true;
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
Walker::WalkerState::pageFaultOnRequestor(RequestorState &r)
{
    if (r.req->isInstFetch()) {
        Addr page_start = (entry.vaddr >> PageShift) << PageShift;
        if (r.req->getPC() < page_start) {
            // expected: instruction crosses the page boundary
            if (!r.req->getPC() + 4 >= entry.vaddr) {
                warn("Unexepected fetch page fault: PC: %#x, Page: %#x\n",
                     r.req->getPC(), entry.vaddr);
            }
            return walker->tlb->createPagefault(page_start, mode);
        } else {
            return walker->tlb->createPagefault(r.req->getPC(), mode);
        }
    } else {
        return walker->tlb->createPagefault(r.req->getVaddr(), mode);
    }
}

Fault
Walker::WalkerState::pageFault(bool present)
{
    bool found_main = false;
    for (auto &r: requestors) {
        DPRINTF(PageTableWalker, "Mark page fault for req %#lx (pc=%#lx).\n",
                r.req->getVaddr(), r.req->getPC());
        auto _fault = pageFaultOnRequestor(r);
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
