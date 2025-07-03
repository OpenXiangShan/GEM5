/*
 * Copyright (c) 2007 The Hewlett-Packard Development Company
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

#ifndef __ARCH_RISCV_TABLE_WALKER_HH__
#define __ARCH_RISCV_TABLE_WALKER_HH__

#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/tlb.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/RiscvPagetableWalker.hh"
#include "sim/clocked_object.hh"
#include "sim/faults.hh"
#include "sim/system.hh"

namespace gem5
{

class ThreadContext;

namespace RiscvISA
{
    class Walker : public ClockedObject
    {
      protected:
        // Port for accessing memory
        class WalkerPort : public RequestPort
        {
          public:
            WalkerPort(const std::string &_name, Walker * _walker) :
                  RequestPort(_name, _walker), walker(_walker)
            {}

          protected:
            Walker *walker;

            bool recvTimingResp(PacketPtr pkt);
            void recvReqRetry();
        };

        friend class WalkerPort;
        WalkerPort port;

        // State to track each walk of the page table
        class WalkerState
        {
          friend class Walker;
          private:
            enum State
            {
                Ready,
                Waiting,
                Translate,
            };

            struct RequestorState
            {
                ThreadContext *tc;
                RequestPtr req;
                BaseMMU::Translation *translation;
                bool fromForwardPreReq;
                bool fromBackPreReq;
                Fault fault;
                bool squashed;

                RequestorState()
                {
                    tc = nullptr;
                    req = nullptr;
                    translation = nullptr;
                    fromForwardPreReq = false;
                    fromBackPreReq = false;
                    fault = NoFault;
                    squashed = false;
                }
                RequestorState(ThreadContext *tc, RequestPtr req, BaseMMU::Translation *translation)
                    : tc(tc),
                      req(req),
                      translation(translation),
                      fromForwardPreReq(false),
                      fromBackPreReq(false),
                      fault(NoFault),
                      squashed(false)
                {}

                void markSquash() { squashed = true; }
            };


            std::list<RequestorState> requestors;


          protected:
            Walker *walker;
            RequestPtr mainReq;  // req is renamed to main req
            State state;
            State nextState;
            int level;
            int twoStageLevel;
            unsigned inflight;
            TlbEntry entry;
            TlbEntry inl2Entry;
            PacketPtr read;
            std::vector<PacketPtr> writes;
            Fault mainFault;
            BaseMMU::Mode mode;
            bool isHInst;
            bool isVsatp0Mode;
            SATP satp;
            SATP vsatp;
            STATUS status;
            STATUS vsstatus;
            PrivilegeMode pmode;
            HGATP hgatp;
            bool functional;
            bool timing;
            bool retrying;
            bool started;
            bool squashed;
            bool nextline;
            Addr nextlineRead;
            int nextlineLevel;
            TlbEntry nextlineEntry;
            Addr nextlineVaddr;

            Addr nextlineLevelMask;
            Addr nextlineShift;
            Addr tlbVaddr;
            Addr tlbppn;
            Addr gPaddr;
            int vaddr_choose_flag;
            Addr tlbSizePte;
            bool openNextline;
            bool autoNextlineSign;
            bool finishDefaultTranslate;
            bool preHitInPtw;
            bool fromPre;
            bool fromBackPre;
            bool virt;
            int translateMode;
            bool inGstage;
            bool finishGVA;
            int gpaddrMode;
            bool finishGPA;
            bool GstageFault;
            bool tlbHit;
            PTESv39 tlbHitPte;
            Request::Flags tlbflags;


          public:
            WalkerState(Walker * _walker, BaseMMU::Translation *_translation,
                        const RequestPtr &_req, bool _isFunctional = false) :
                walker(_walker), mainReq(_req), state(Ready),
                nextState(Ready), level(0), twoStageLevel(2),inflight(0),
                functional(_isFunctional), timing(false),
                retrying(false), started(false), squashed(false), nextline(false),
                nextlineRead(0), nextlineLevel(0), nextlineVaddr(0),
                nextlineLevelMask(0), nextlineShift(0), tlbVaddr(0), tlbppn(0),
                gPaddr(0),vaddr_choose_flag(0),
                tlbSizePte(0), openNextline(false), autoNextlineSign(false),
                finishDefaultTranslate(false), preHitInPtw(false), fromPre(false),
                fromBackPre(false),virt(0),translateMode(0),inGstage(false),finishGVA(false),
                gpaddrMode(0),finishGPA(false),GstageFault(false),
                tlbHit(false),tlbHitPte(0),tlbflags(Request::PHYSICAL)
            {
                requestors.emplace_back(nullptr, _req, _translation);
            }
            void initState(ThreadContext *_tc, const RequestPtr &_req,BaseMMU::Mode _mode,
                           bool _isTiming = false, bool _from_forward_pre_req = false,
                           bool _from_back_pre_req = false);

            std::pair<bool, Fault> tryCoalesce(
                ThreadContext *_tc, BaseMMU::Translation *translation,
                const RequestPtr &req, BaseMMU::Mode mode, bool from_l2tlb,
                Addr asid, bool from_forward_pre_req, bool from_back_pre_req);

            Fault startWalk(Addr ppn, int f_level, bool from_l2tlb,
                            bool open_nextline, bool auto_openNextline,
                            bool from_forward_pre_req, bool from_back_req);
            Fault startFunctional(Addr &addr, unsigned &logBytes,
                                  bool open_nextline, bool auto_openNextline,
                                  bool from_forward_pre_req,
                                  bool from_back_pre_req);
            bool recvPacket(PacketPtr pkt);
            unsigned numInflight() const;
            bool isRetrying();
            bool wasStarted();
            bool isTiming();
            void retry();
            std::string name() const {return walker->name();}

            bool anyRequestorSquashed() const;
            bool allRequestorSquashed() const;
            Fault setupWalk(Addr ppn, Addr vaddr, int f_level, bool from_l2tlb,
                           bool open_nextline, bool auto_openNextline,
                           bool from_forward_pre_req, bool from_back_pre_req);

          private:
            Fault startTwoStageWalk(Addr ppn, Addr vaddr);
            Fault startTwoStageWalkFromTLBNotInG(Addr ppn, Addr vaddr);
            Fault startTwoStageWalkFromTLBInG(Addr ppn, Addr vaddr);

            Fault twoStageStepWalk(PacketPtr &write);
            Fault twoStageWalk(PacketPtr &write);
            Fault stepWalk(PacketPtr &write);
            void sendPackets();
            void endWalk();
            Fault endGstageWalk();
            Fault pageFault(bool present, bool G);
            Fault pageFaultOnRequestor(RequestorState &requestor, bool G);
            Addr getGVPNi(Addr vaddr, int level);
            Addr VpniShift(int level);
        };

        struct L2TlbState
        {
              RequestPtr req;
              ThreadContext *tc;

              BaseMMU::Translation *translation;
              BaseMMU::Mode mode;
              Addr Paddr;
              TlbEntry *entry;
              TlbEntry *entryVsstage;
              TlbEntry *entryGstage;
        };
        std::list<L2TlbState> L2TLBrequestors;

        friend class WalkerState;
        // State for timing and atomic accesses (need multiple per walker in
        // the case of multiple outstanding requests in timing mode)
        std::list<WalkerState *> currStates;
        // State for functional accesses (only need one of these per walker)
        WalkerState funcState;

        struct WalkerSenderState : public Packet::SenderState
        {
            WalkerState * senderWalk;
            WalkerSenderState(WalkerState * _senderWalk) :
                senderWalk(_senderWalk) {}
        };

      public:
        // Kick off the state machine.
        Fault start(Addr ppn, ThreadContext *_tc,
                    BaseMMU::Translation *translation, const RequestPtr &req,
                    BaseMMU::Mode mode, bool from_forward_pre_req = false,
                    bool frm_back_pre_req = false, int f_level = 2,
                    bool from_l2tlb = false, Addr asid = 0);

        void doL2TLBHitSchedule(const RequestPtr &req, ThreadContext *tc,
                                BaseMMU::Translation *translation,
                                BaseMMU::Mode mode, Addr Paddr,
                                TlbEntry *entry,TlbEntry *entryVsstage,
                                TlbEntry *entryGstage,int delaytick);



        std::pair<bool, Fault> tryCoalesce(ThreadContext *_tc,
                                           BaseMMU::Translation *translation,
                                           const RequestPtr &req,
                                           BaseMMU::Mode mode, bool from_l2tlb,
                                           Addr asid, bool from_forward_pre_req,
                                           bool from_back_pre_req);

        // Fault perm_check ();

        Fault startFunctional(RequestPtr req, ThreadContext * _tc, Addr &addr,
                unsigned &logBytes, BaseMMU::Mode mode);
        Port &getPort(const std::string &if_name,
                      PortID idx=InvalidPortID) override;
      protected:
        // The TLB we're supposed to load.
        TLB * tlb;
        TLB * l2tlb;
        System * sys;
        PMAChecker * pma;
        PMP * pmp;
        RequestorID requestorId;

        // The number of outstanding walks that can be squashed per cycle.
        unsigned numSquashable;
        bool enableL1L2replace;
        bool ptwSquash;
        bool openNextLine;
        bool autoOpenNextLine;
      public:
        bool openSv48;
        bool is_from_pre_req;

        Tick squashHandleTick;

        // Wrapper for checking for squashes before starting a translation.
        //void startWalkWrapper();

        // Checking for squashes
        //void handlePendingSquash();

        void dol2TLBHit();

        /**
         * Event used to call startWalkWrapper.
         **/


        EventFunctionWrapper doL2TLBHitEvent;

        // Functions for dealing with packets.
        bool recvTimingResp(PacketPtr pkt);
        void recvReqRetry();
        bool sendTiming(WalkerState * sendingState, PacketPtr pkt);
        //bool pre_ptw;

      public:

        void setTLB(TLB * _tlb)
        {
            tlb = _tlb;
        }
        void setL2TLB(TLB * _l2tlb)
        {
            l2tlb = _l2tlb;
        }

        using Params = RiscvPagetableWalkerParams;

        Walker(const Params &params) :
            ClockedObject(params), port(name() + ".port", this),
            funcState(this, NULL, NULL, true), tlb(NULL), sys(params.system),
            pma(params.pma_checker),
            pmp(params.pmp),
            requestorId(sys->getRequestorId(this)),
            numSquashable(params.num_squash_per_cycle),
            enableL1L2replace(params.enable_l1l2_replace),
            ptwSquash(params.ptw_squash),
            openNextLine(params.open_nextline),
            autoOpenNextLine(true),
            openSv48(false),
            doL2TLBHitEvent([this]{dol2TLBHit();},name())
        {
        }
    };

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_PAGE_TABLE_WALKER_HH__
