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

#include <array>
#include <cstdio>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arch/generic/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pma_checker.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/tlb.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "params/RiscvPagetableWalker.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/faults.hh"
#include "sim/system.hh"

namespace gem5
{

class ThreadContext;

namespace RiscvISA
{
#ifndef MPT_ENABLED
#define MPT_ENABLED 1
#endif
//#include "sim/stat_control.hh"
//#else
//#define MPT_ENABLED 0
//#endif

// Whether MPT Cache is enabled.
//#if MPT_ENABLED && !defined(__ARCH_RISCV_MMU_MPT_CACHE_HH__)
#define MPT_CACHE_ENABLED 1
#if MPT_ENABLED
struct MPT;
#endif


class MPTCache52;


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
        public:
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


          public:
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
            PacketPtr MptReadReq;
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
            bool waitingForPtwLevel;
            int reservedPtwLevel;
            int blockedPtwLevel;
            Addr blockedPtwRead;
            unsigned blockedPtwReadSize;
            Request::Flags blockedPtwFlags;
            Addr PaddrUT;
            Addr mptCheckPaddr;
            BaseMMU::Mode mptCheckMode;
            BaseMMU::Mode mptFaultMode;
            bool mptCheckingPteRead=false;
            bool pteReadMptResult=false;
            bool pteReadMptChecked=false;
            bool pteReadMptIsNextline=false;
            bool pteReadMptFaultPending=false;
            Addr pteReadMptCheckedPaddr=0;
            bool isMPTing=false;
            bool finishMPTing=false;
            bool MPTresult=false;
            int mpt_level=4;
            MPTInfoInTLB mptInfo = MPTInfoInTLB();
            bool mptGranularityClipped=false;
            EventFunctionWrapper *mptCacheHitEvent = nullptr;
            bool mptCacheHitPending = false;
            bool mptCacheHitRetry = false;
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
                tlbHit(false),tlbHitPte(0),tlbflags(Request::PHYSICAL),
                waitingForPtwLevel(false), reservedPtwLevel(-1),
                blockedPtwLevel(-1), blockedPtwRead(0), blockedPtwReadSize(0),
                blockedPtwFlags(Request::PHYSICAL),
                mptCheckPaddr(0),mptCheckMode(BaseMMU::Read),
                mptFaultMode(BaseMMU::Read),mptGranularityClipped(false)
            {
                requestors.emplace_back(nullptr, _req, _translation);
            }
            ~WalkerState();

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

            bool recvPacketSplit();
            unsigned numInflight() const;
            bool isRetrying();
            bool wasStarted();
            bool isTiming();
            void retry();
            bool startMPTwalk();
            bool LastMPTwalk();
            bool completeMPTWalk();
            void scheduleMptCacheHit();
            bool stepMPTwalk();
            bool stepMPTwalkFromMPTE(uint64_t raw, Addr mptePaddr);
            Fault startPteReadMPTCheck();
            bool finishPteReadMPTFault();
            std::string name() const {return walker->name();}

            bool anyRequestorSquashed() const;
            bool allRequestorSquashed() const;
            Fault setupWalk(Addr ppn, Addr vaddr, int f_level, bool from_l2tlb,
                           bool open_nextline, bool auto_openNextline,
                           bool from_forward_pre_req, bool from_back_pre_req);

          private:
            /** setup the walk state started by a Gstage translation .
                (hgatp + idx as the base address) */
            Fault startTwoStageWalk(Addr ppn, Addr vaddr);
            Fault startTwoStageWalkFromTLBNotInG(Addr ppn, Addr vaddr);
            Fault startTwoStageWalkFromTLBInG(Addr ppn, Addr vaddr);

            Fault twoStageStepWalk(PacketPtr &write);
            Fault twoStageWalk(PacketPtr &write);
            Fault stepWalk(PacketPtr &write);
            void sendPackets();
            void sendMptPacket();
            void sendPacketsMPT();
            void endWalk();
            bool usePtwLevelLimit() const;
            int currentPtwResourceLevel() const;
            bool waitForPtwLevel(int target_level, Addr next_read,
                                 unsigned read_size, Request::Flags flags);
            bool deferPtwLevelRead(int target_level, Addr next_read,
                                   unsigned read_size, Request::Flags flags,
                                   PacketPtr old_read);
            bool retryBlockedPtwLevel();
            Fault endGstageWalk();
            Fault pageFault(bool present, bool G);
            Fault pageFaultOnRequestor(RequestorState &requestor, bool G);
            Addr getGVPNi(Addr vaddr, int level);
            Addr getGVPNi(uint8_t addrXlateMode, Addr vaddr, int level);
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

        struct MissQueueEntry
        {
            ThreadContext *tc;
            BaseMMU::Translation *translation;
            RequestPtr req;
            BaseMMU::Mode mode;
        };

        friend class WalkerState;
        // State for timing and atomic accesses (need multiple per walker in
        // the case of multiple outstanding requests in timing mode)
        std::list<WalkerState *> currStates;
        // State for functional accesses (only need one of these per walker)
        WalkerState funcState;

        /**
         * PTW memory-side statistics. These counters intentionally track only
         * the requests that leave the walker and the time window during which
         * at least one walker memory request is still outstanding.
         */
        struct WalkerStats : public statistics::Group
        {
            WalkerStats(statistics::Group *parent);

            statistics::Scalar ptwMemCount;
            statistics::Scalar ptwMemCycle;
            statistics::Formula ptwAvgMemLatency;
        } stats;

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
                                TlbEntry *entryGstage);



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
      public:

#if MPT_ENABLED
          inline int getLevelForPageSizeLog2(uint8_t logBytes);
          Fault createMPTPagefault(Addr vaddr, Addr paForMPTCheck, BaseMMU::Mode mode);

#endif


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
        bool enablePtwLevelLimit;
        std::array<unsigned, 4> ptwLevelLimit;
        std::array<unsigned, 4> ptwLevelActive;
        unsigned ptwMissQueueSize;
        std::deque<MissQueueEntry> ptwMissQueue;
        std::deque<MissQueueEntry> ptwMissQueueWaiters;
        bool retryingPtwMissQueue;
        bool processingPtwMissQueueHint;
        bool ptwMissQueueHeadRequeued;
        /** Last tick at which PTW in-flight time accounting was updated. */
        Tick lastPtwMemCycleTick;
        /** Number of PTW memory requests currently in flight. */
        unsigned outstandingPtwMemReqs;

        void updatePtwMemCycleStats();
        bool ptwLevelAvailable(WalkerState *state, int level) const;
        bool reservePtwLevel(WalkerState *state, int level);
        void releasePtwLevel(WalkerState *state);
        void retryPtwLevelBlockedStates();
        bool ptwMissQueueHintMatch(const MissQueueEntry &entry,
                                   const TlbEntry &refill_entry,
                                   uint8_t translateMode) const;
      public:
        bool is_from_pre_req;

        Tick squashHandleTick;

        // Wrapper for checking for squashes before starting a translation.
        //void startWalkWrapper();

        // Checking for squashes
        //void handlePendingSquash();

        void dol2TLBHit();
        void preDumpStats() override;
        void resetStats() override;

        /**
         * Event used to call startWalkWrapper.
         **/


        EventFunctionWrapper doL2TLBHitEvent;

        // Functions for dealing with packets.
        bool recvTimingResp(PacketPtr pkt);
        void deletestate(WalkerState * senderWalk);
        void recvReqRetry();
        bool sendTiming(WalkerState * sendingState, PacketPtr pkt);
        bool usePtwLevelLimitForStart(bool from_forward_pre_req,
                                      bool from_back_pre_req,
                                      bool is_prefetch) const;
        bool canStartPtwLevel(int level, bool from_forward_pre_req,
                              bool from_back_pre_req,
                              bool is_prefetch);
        bool enqueuePtwMiss(ThreadContext *tc, BaseMMU::Translation *translation,
                            const RequestPtr &req, BaseMMU::Mode mode, bool front = false);
        bool hasPendingPtwMiss() const
        {
            return !ptwMissQueue.empty() || !ptwMissQueueWaiters.empty();
        }
        void notifyTlbRefillHint(const TlbEntry &entry, uint8_t translateMode);
        void retryPtwMissQueue();
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
            funcState(this, NULL, NULL, true), stats(this), tlb(NULL),
            sys(params.system),
            pma(params.pma_checker),
            pmp(params.pmp),
            requestorId(sys->getRequestorId(this)),
            numSquashable(params.num_squash_per_cycle),
            enableL1L2replace(params.enable_l1l2_replace),
            ptwSquash(params.ptw_squash),
            openNextLine(params.open_nextline),
            autoOpenNextLine(true),
            enablePtwLevelLimit(params.enable_ptw_level_limit),
            ptwLevelLimit({{
                params.ptw_level0_limit,
                params.ptw_level1_limit,
                params.ptw_level2_limit,
                params.ptw_level3_limit
            }}),
            ptwLevelActive({{0, 0, 0, 0}}),
            ptwMissQueueSize(params.ptw_miss_queue_size),
            retryingPtwMissQueue(false),
            processingPtwMissQueueHint(false),
            ptwMissQueueHeadRequeued(false),
            lastPtwMemCycleTick(0),
            outstandingPtwMemReqs(0),
            doL2TLBHitEvent([this]{dol2TLBHit();},name())
        {
        }
    };


class PLRUTreeN
{
  private:
    size_t numWays;               // N: cache entry 个数，必须是2的幂
    std::vector<bool> bits;       // 满二叉树内部方向位，大小为 N - 1

  public:
    // 构造函数
    explicit PLRUTreeN(size_t ways);

    // 获取应被替换的 victim entry 索引
    size_t getVictim() const;

    // 表示访问了某个 entry，更新树路径
    void access(size_t way);

    // 重置所有方向位为0
    void reset();

    // 返回树支持的容量
    size_t size() const { return numWays; }
};

/* depracated implementation
struct MPTSenderState : public Packet::SenderState
{
    ThreadContext *tc;
    BaseMMU::Translation *translation;

    MPTSenderState(ThreadContext *tc_, BaseMMU::Translation *tr_)
        : tc(tc_), translation(tr_) {}
};
*/
//new implementation
extern std::unordered_map<const Request*, std::pair<ThreadContext*, BaseMMU::Translation*>> mptContextMap;

struct MptPendingRead
{
    Addr mptePaddr = 0;
    PacketPtr pkt = nullptr;
    std::vector<Walker::WalkerState*> waiters;
};

struct MPT
{
    Addr rootPPN; // Root page table physical page number (in page number units).
    //override:
    Addr nextPPN;
    MPT(); // Default constructor.
    bool readMPTE(Addr paddr, ThreadContext *tc, PMAChecker *pma, PMP *pmp,
                  Walker::WalkerState *senderState);
    // Smmpt52 multi-level traversal returning an MPTE52 or invalid entry.
    bool walk(int hit_level, Addr base, Addr paddrUT, ThreadContext *tc,
              PMAChecker *pma, PMP *pmp,
              Walker::WalkerState *senderState);

    //all miss, 127*4; L3 hit , else miss,  127*3.   L3 L2 hit , l1 l0 miss, 127*2.   L3 L2 L1 hit , l0 miss 127

    //Asynchronous delayed walk interface, triggers the callback to return the result after 127 cycles.
    // void walkDelayed(Addr vaddr,
    //                 int hit_level,
    //                 Addr base,
    //                  ThreadContext *tc,
    //                  PMAChecker *pma, PMP *pmp,
    //                  std::function<void(MPTCacheEntry)> callback); //const;
    PacketPtr MptReadReq;
    MptPendingRead activeMptRead;
    bool hasActiveMptRead;
    bool processingMptResp;
    std::deque<MptPendingRead> pendingMptReads;
    bool enqueueMptPacket(PacketPtr pkt, Walker::WalkerState* senderState);
    bool issueMptPacket();
    bool sendMptPacket();
    PacketPtr CreateMptReqPacket(Addr paddr,Walker::WalkerState* senderState) ;
    bool MptRecvTimingResp(PacketPtr pkt) ;
    void completeMptWaiter(Walker::WalkerState *senderWalk);
    void flushPendingMptReads();
    bool ensureSimulatedMPTTree(System *sys, ThreadContext *tc);
    Addr buildSimulatedMPTTree(System *sys);
    Walker *walker;
    int mptinflight;
    bool mptretry;
    MMPT mmpt;
    bool simulatedTreeBuilt;
    Addr simulatedRootPaddr;

};




class MPTCache52
{
  private:
    size_t capacity;
    size_t capacityL0;
    size_t capacityL1;
    size_t capacityL2;
    size_t capacityL3;
    size_t capacitySP;

    static int configuredSize;

    //std::unordered_map<Addr, MPTCacheEntry> table;

    std::unordered_map<Addr, MPTCacheEntry> tableL0;
    std::unordered_map<Addr, MPTCacheEntry> tableL1;
    std::unordered_map<Addr, MPTCacheEntry> tableL2;
    std::unordered_map<Addr, MPTCacheEntry> tableL3;
    std::unordered_map<Addr, MPTCacheEntry> tableSP;

// -------- PLRU Replacement Support --------
// Tag order and replacement path for each level
// Tag order table for each level's cache + corresponding PLRU tree
    std::vector<Addr> tagListL0;
    std::vector<Addr> tagListL1;
    std::vector<Addr> tagListL2;
    std::vector<Addr> tagListL3;
    std::vector<Addr> tagListSP;

    PLRUTreeN plruL0 = PLRUTreeN(1); // Default constructor, will resize later.
    PLRUTreeN plruL1 = PLRUTreeN(1);
    PLRUTreeN plruL2 = PLRUTreeN(1);
    PLRUTreeN plruL3 = PLRUTreeN(1);
    PLRUTreeN plruSP = PLRUTreeN(1);
    // -------------------------------
  public:
    Addr regionAlign(Addr pa, int level) const;

    // MPTCache Hit
    mutable statistics::Scalar mptCacheL0Hits;
    mutable statistics::Scalar mptCacheL1Hits;
    mutable statistics::Scalar mptCacheL2Hits;
    mutable statistics::Scalar mptCacheL3Hits;
    mutable statistics::Scalar mptCacheSPHits;

    // MPTCache Miss
    mutable statistics::Scalar mptCacheL0Misses;
    mutable statistics::Scalar mptCacheL1Misses;
    mutable statistics::Scalar mptCacheL2Misses;
    mutable statistics::Scalar mptCacheL3Misses;
    mutable statistics::Scalar mptCacheSPMisses;



    MPTCache52(size_t capL0, size_t capL1, size_t capL2, size_t capL3, size_t capSP);
    MPTCache52();

    static int configuredSizeL0;
    static int configuredSizeL1;
    static int configuredSizeL2;
    static int configuredSizeL3;
    static int configuredSizeSP;

    static void configureSize(int sL0, int sL1, int sL2, int sL3, int sSP);

    void initMPTCacheFromParams(const RiscvTLBParams *params );
    void
    sayhimpt()
    {
    }
    // -------- PLRU Replacement Support --------
    std::vector<Addr>& getTagListByLevel(int level);
    PLRUTreeN& getPLRUByLevel(int level);
    const std::vector<Addr>& getTagListByLevel(int level) const;
    const PLRUTreeN& getPLRUByLevel(int level) const;      //const overload
    // -------------------------------


    // non-const
    std::unordered_map<Addr, MPTCacheEntry>& getTableByLevel(int level);

    // const
    const std::unordered_map<Addr, MPTCacheEntry>& getTableByLevel(int level) const;

    // non-const
    size_t& getCapacityByLevel(int level);

    // Read-only version (if the capacity needs to be read in a `const` function).
    size_t getCapacityByLevel(int level) const;

    // Insert a new refill entry, or refresh an existing tag without changing
    // replacement state as a new allocation. Returns true only for a new tag.
    bool insertOrRefreshEntry(int level, Addr aligned,
                              const MPTCacheEntry &entry);

    void mfence_all(); //just fence all entries in the cache.

    std::pair<bool /*hit*/, MPTCacheEntry> fetchDelayed(
        Addr pa, ThreadContext *tc, PMAChecker *pma, PMP *pmp,
        int& mptlevel, gem5::RiscvISA::Walker::WalkerState* senderState);
};




extern gem5::RiscvISA::MPT globalMPT;

//extern MPTCache52 globalMPTCache;
extern gem5::RiscvISA::MPTCache52* globalMPTCache;



} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_PAGE_TABLE_WALKER_HH__
