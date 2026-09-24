/*
 * Copyright (c) 2026
 * All rights reserved
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
 * SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * @file
 * CCHIL1Agent: per-core bridge between a gem5 classic L1 cache hierarchy
 * (L1I + L1D + MMU walkers, untouched) and one CCHI Taurus UpstreamNode
 * owned by the CCHIFabric. See the approved plan's "Bridge semantics"
 * section; the mapping is:
 *
 *   ReadReq            -> DoLoad, fill via CacheLine::Load* on grant
 *   ReadExReq          -> DoStore (fill + ownership), same readback
 *   UpgradeReq/SCUpgradeReq (and StoreCondReq) -> DoStore (S->U upgrade)
 *   WriteReq/WriteLineReq -> DoStore/DoStoreLine + CacheLine::Store*
 *   HardPFReq          -> DoLoad fill, always respond
 *   LockedRMWReadReq   -> DoStore + pin the line
 *   LockedRMWWriteReq  -> CacheLine::Store(span) + unpin
 *   WritebackDirty     -> CacheLine::Store(span) + respond + async DoEvict
 *   WritebackClean/CleanEvict -> respond + async DoEvict
 *   WriteClean         -> CacheLine::Store(span) + respond (no eviction)
 *   MemFenceReq/MemSyncReq -> drain agent futures, then respond
 *   CleanSharedReq     -> DoCBOClean, CleanInvalidReq -> DoCBOInval
 *   uncacheable/MMIO/out-of-window -> mem_side bypass port (no CCHI)
 *
 * All data movement goes through the Taurus CacheLine checked accessors
 * (the Load and Store families) - never through the raw members.
 *
 * Futures: Do* calls return FutureNow handles; the bridge always Bind()s a
 * lambda that only posts the line address to the agent's completion queue
 * (now-futures run the lambda synchronously, pending futures run it inside
 * the fabric's Instance::Tick). The fabric's per-cycle tick hook then
 * turns completions into gem5 responses via the queued cpu_side ports.
 *
 * Snoops are home-originated only: with snoop_merge=off (default) the
 * fabric's OnAcceptedSNP hook calls reflectSnoopToL1() and Taurus answers
 * the home itself; the L1's data-supplying snoop response is sunk (drop +
 * delete + stat). With snoop_merge=on the fabric's SNP gate intercepts the
 * flit first, calls handleSnoopFromHome(), and only after the merge
 * completes releases it for PushRXSNP.
 */

#ifndef __MEM_CCHI_CCHI_L1_AGENT_HH__
#define __MEM_CCHI_CCHI_L1_AGENT_HH__

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/addr_range.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/cache/cache_probe_arg.hh"
#include "mem/packet.hh"
#include "sim/probe/probe.hh"
#include "mem/packet_queue.hh"
#include "mem/qport.hh"
#include "mem/request.hh"
#include "params/CCHIL1Agent.hh"
#include "sim/clocked_object.hh"

// CHIron (read-only; resolved via -I$(CHIRON_DIR)/cchi)
#include "cohestra/cohestra_interface.hpp"
#include "icn/taurus/cchi_taurus_component.hpp"

#include "mem/cchi/cchi_fabric.hh"

namespace gem5
{

namespace prefetch { class Base; }

class CCHIL1Agent : public ClockedObject
{
  public:
    using TaurusNode = CCHIFabric::TaurusNode;
    using TaurusCacheLine = TaurusNode::CacheLine;
    using FlitSNP = CCHIFabric::FlitSNP;
    using DenialEnum = CCHI::Taurus::DenialEnum;

    CCHIL1Agent(const CCHIL1AgentParams &params);

    void init() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    // --- fabric-facing interface -------------------------------------

    /** Per-cycle hook, called by the fabric after Instance::Tick. */
    void tickHook();

    /**
     * Home-snoop entry point for the snoop_merge=on gate flow: reflect
     * the flit into the L1s and start (or synchronously finish) the
     * merge. The fabric releases the held flit once no L1 snoop response
     * is outstanding.
     */
    void handleSnoopFromHome(const FlitSNP &flit);

    /**
     * Reflect a home-originated snoop into every snooping cpu_side port
     * (1:1 transliteration; the bridge never originates snoops itself).
     * SnpToInvalid/SnpMakeInvalid map to ReadExReq (invalidate),
     * SnpToShared/SnpToClean to ReadSharedReq.
     * Returns the number of ports that committed to a data response.
     */
    int reflectSnoopToL1(Addr linePA, uint64_t snpOpcode);

    bool getSnoopMerge() const { return snoopMerge; }
    uint32_t getNodeId() const { return nodeId; }
    uint32_t getXactionLimitREQ() const { return xactionLimitREQ; }
    uint32_t getXactionLimitEVT() const { return xactionLimitEVT; }
    uint32_t getXactionLimitSNP() const { return xactionLimitSNP; }

    void setUpstreamIndex(size_t index) { upstreamIndex = index; }

  protected: // Port interface
    class AgentResponsePort : public QueuedResponsePort
    {
      public:
        AgentResponsePort(const std::string &_name, CCHIL1Agent &_parent,
                          PortID _id)
            : QueuedResponsePort(_name, &_parent, queue, _id),
              queue(_parent, *this, false, _name + ".queue"),
              parent(_parent)
        { }

      protected:
        bool recvTimingReq(PacketPtr pkt) override;
        bool recvTimingSnoopResp(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;

        AddrRangeList getAddrRanges() const override;
        bool tryTiming(PacketPtr) override { return true; }

      private:
        // Own response queue, constructed right after the base stores its
        // reference (the gem5 CacheResponsePort idiom)
        RespPacketQueue queue;
        CCHIL1Agent &parent;
    };

    class BypassRequestPort : public QueuedRequestPort
    {
      public:
        BypassRequestPort(const std::string &_name, CCHIL1Agent &_parent);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;

      private:
        CCHIL1Agent &parent;
    };

    // cpu_side vector (grown on demand by getPort); each port owns one
    // response queue so that responses return on the port the request
    // arrived on
    std::vector<std::unique_ptr<AgentResponsePort>> cpuSidePorts;

    BypassRequestPort memSidePort;
    ReqPacketQueue reqQueue;
    SnoopRespPacketQueue snoopRespQueue;

  protected: // Packet handlers (called from the ports)
    bool recvTimingReq(PacketPtr pkt, PortID portId);
    void recvTimingSnoopResp(PacketPtr pkt, PortID portId);
    Tick recvAtomic(PacketPtr pkt, PortID portId);
    void recvFunctional(PacketPtr pkt, PortID portId);
    void recvBypassResp(PacketPtr pkt);
    AddrRangeList getAddrRanges() const;

  protected: // Pending-transaction types (declared before use below)
    enum XactAction : uint8_t {
        ACT_READ = 0,   // ReadReq / LoadLockedReq: DoLoad + fill
        ACT_READ_EX,    // ReadExReq: DoStore + fill (ownership)
        ACT_UPGRADE,    // UpgradeReq/SCUpgradeReq/StoreCondReq: DoStore
        ACT_WRITE,      // WriteReq/WriteLineReq: DoStore(/Line) + Store*
        ACT_HARD_PF,    // HardPFReq: DoLoad + fill, always respond
        ACT_RMW_READ,   // LockedRMWReadReq: DoStore + pin
        ACT_RMW_WRITE,  // LockedRMWWriteReq: Store* + unpin
        ACT_WRITEBACK,  // WritebackDirty (deferred store path)
        ACT_WRITE_CLEAN,// WriteClean (deferred store path)
        ACT_CMO,        // CleanShared/CleanInvalid: DoCBO*
        ACT_INV_WRITE,  // InvalidateReq (XS fast-writeline): ownership only
        ACT_SWAP        // SwapReq (AMO): RMW on the granted line
    };

    struct PendingXaction {
        Addr linePA;
        PacketPtr pkt;
        PortID portId;
        XactAction action;
        bool grantedHit;                // FutureNow was IsNow() at issue
        Tick grantedTick;               // set when the future fires
        std::shared_ptr<TaurusCacheLine> line;  // set when the future fires
        // Grant-time fill snapshot for read actions: the granted line can
        // be invalidated (and reaped) by a racing home snoop before the
        // completion drains, making a later Load() fail forever. The data
        // as-of the grant is exactly what the home sent and what the L1
        // must fill with, so capture it while the line is guaranteed
        // consistent (the grant just completed).
        std::optional<std::array<uint64_t, 8>> fillData = std::nullopt;
        unsigned retries = 0;           // completion retry counter
    };

    // After this many failed store attempts on a granted line, ownership
    // is re-acquired (DoStore) instead of retrying forever. Kept small:
    // a failed checked store means the line is not (or no longer)
    // Unique, which only an ownership upgrade can fix - spinning longer
    // just stalls the completion for no benefit (the tick hook and the
    // escalation rejection path keep this bounded and livelock-free).
    static constexpr unsigned STORE_RETRY_ESCALATE = 4;

  protected: // Bridge semantics
    /** MemCmd -> Taurus Do* mapping for cacheable, in-window requests. */
    bool handleCchiRequest(PacketPtr pkt, PortID portId);

    bool issueLoad(PacketPtr pkt, PortID portId, XactAction action);
    bool issueStore(PacketPtr pkt, PortID portId, XactAction action,
                    bool wholeLine);
    bool issueCMO(PacketPtr pkt, PortID portId, bool invalidate);
    bool handleRMWWrite(PacketPtr pkt, PortID portId);
    bool handleWriteback(PacketPtr pkt, PortID portId, bool dirty);
    bool handleWriteClean(PacketPtr pkt, PortID portId);
    bool handleFence(PacketPtr pkt, PortID portId);

    /** Backpressure for a denied Do* call: block + retry next fabric tick. */
    bool handleDenial(DenialEnum denial, PortID portId);

    /**
     * Intra-node (local) snoop: with several L1s fanning into one Taurus
     * node (L1I/L1D/walker caches), a sibling's dirty data is invisible
     * to home-level coherence, so a reader from another port could fill
     * the node's stale copy (observed: PTW walker reading a PTE page that
     * was dirty in the L1D -> spurious fetch fault). Before a data or
     * ownership request on a node-resident line, snoop the other fan-in
     * L1s (share for reads, invalidate for write-class), merge any dirty
     * data into the node line, then issue the original request.
     */
    int reflectLocalSnoop(PacketPtr pkt, PortID portId);
    /** Merge a data-carrying snoop response into the node line. */
    void mergeSnoopData(PacketPtr pkt, Addr linePA);
    /** Command dispatch after admission/local-snoop completion. */
    bool dispatchCchiRequest(PacketPtr pkt, PortID portId);
    /** The home-snoop flow, split so it can be deferred by a local snoop. */
    void processHomeSnoop(const FlitSNP &flit);

    /** FutureNow completion: only posts to the completion queue. */
    void onGrantFired(Addr linePA,
                      const TaurusNode::GrantedEvent &ev);
    void onCMOFired(Addr linePA,
                    const TaurusNode::CMOCompleteEvent &ev);

    /** Turn a posted completion into a response; false = data not ready. */
    bool processCompletion(PendingXaction &entry);

    /** Checked data movement through the Taurus CacheLine accessors. */
    bool fillFromLine(PacketPtr pkt, TaurusCacheLine &line);
    /** Fill the packet from the grant-time snapshot when present (racing
     *  home snoops can invalidate the granted line before the completion
     *  drains); falls back to the live line otherwise. */
    bool fillFromEntry(PendingXaction &entry);
    bool storeToLine(PacketPtr pkt, TaurusCacheLine &line);
    /** AMO read-modify-write on the granted line (returns old value). */
    bool performAtomicOp(PacketPtr pkt, TaurusCacheLine &line);

    /** A line store/RMW attempt under the checked accessors. */
    using StoreOp = std::function<bool(PacketPtr, TaurusCacheLine &)>;
    /** Bounded retry + ownership re-acquisition for a StoreOp. */
    bool storeWithEscalation(PendingXaction &entry, const StoreOp &op);
    bool storeWithEscalation(PendingXaction &entry)
    {
        return storeWithEscalation(
            entry, [this](PacketPtr pkt, TaurusCacheLine &line) {
                return storeToLine(pkt, line);
            });
    }

    void respondTo(PacketPtr pkt, PortID portId, Tick when);
    void issueEvict(Addr linePA);
    void drainPrefetcher();

    /** Lazy Taurus node lookup (built by the fabric after init()). */
    TaurusNode *taurus();

  protected: // Pending-transaction bookkeeping
    std::unordered_map<Addr, PendingXaction> pending;
    std::deque<Addr> completionQueue;

    // Parked fence/sync requests, answered once the agent has drained
    std::deque<std::pair<PacketPtr, PortID>> fenceQueue;

    // cpu_side ports that saw a false from recvTimingReq and need a retry
    std::set<PortID> blockedPorts;

    // Evictions whose DoEvict was denied; retried from the tick hook
    std::deque<Addr> evictRetryQueue;

    // Tick counter for the debug heartbeat dump in tickHook
    uint64_t heartbeat = 0;

    // Lines pinned by an in-flight LockedRMW pair (never evicted by the
    // bridge; SNP hold for pinned lines is subsumed by the merge flow -
    // see the merge state below)
    std::unordered_set<Addr> pinnedLines;

    // snoop_merge state: at most one merge in flight per agent (the
    // fabric's SNP gate withholds the port's SNP channel head-of-line)
    struct MergeState {
        Addr linePA;
        int awaitingResponses;
        Cycles startCycle;
    };
    std::optional<MergeState> merge;

    // Local (intra-node) snoop state: one parked request per line, waiting
    // for the sibling L1s' snoop responses before its Do* may issue.
    struct LocalSnoop {
        PacketPtr pkt;
        PortID portId;
        int awaitingResponses;
        Cycles startCycle;
    };
    std::unordered_map<Addr, LocalSnoop> localSnoops;

    // A home snoop that arrived while a local snoop for the same line was
    // in flight: the gate stays held (not released) until the local snoop
    // completes, then this deferred flit goes through processHomeSnoop.
    std::optional<FlitSNP> deferredHomeSnoop;

    // Cross-transaction snoops released early (our own bridge transaction
    // for the same line was in flight at the home): the L1's deferred
    // snoop response merges into the Taurus line when it eventually
    // arrives (see handleSnoopFromHome / recvTimingSnoopResp)
    std::unordered_set<Addr> lateMergeLines;

    // Safety valve for the merge flow: if the L1 snoop response never
    // arrives (e.g. a cross-transaction wait at the endpoint), release
    // the held snoop anyway after this many cycles and accept bounded
    // staleness (counted; see the plan's multicore risk note)
    // TODO(cchi): revisit with the Phase 2 shared-line stress results
    static constexpr uint64_t MERGE_TIMEOUT_CYCLES = 4096;

  protected: // L2 prefetch engine hosting (wire-up only)
    /**
     * CacheAccessor backed by the Taurus node, letting the hosted L2
     * prefetcher query "the L2" (which in CCHI mode is the Taurus node +
     * endpoint, not a gem5 cache).
     */
    class TaurusCacheAccessor : public CacheAccessor
    {
      public:
        explicit TaurusCacheAccessor(CCHIL1Agent *_agent) : agent(_agent) { }

        bool inCache(Addr addr, bool is_secure) const override;
        unsigned level() const override { return 2; }
        bool hasBeenPrefetched(Addr, bool) const override
        { return false; } // TODO(cchi): no prefetch-use tracking in Taurus
        bool hasBeenPrefetched(Addr, bool, RequestorID) const override
        { return false; } // TODO(cchi): as above
        bool hasEverBeenPrefetched(Addr, bool) const override
        { return false; } // TODO(cchi): as above
        Request::XsMetadata getHitBlkXsMetadata(PacketPtr) override
        { return Request::XsMetadata(); } // TODO(cchi): no per-block XS metadata
        bool inMissQueue(Addr addr, bool is_secure) const override;
        bool coalesce() const override { return false; }
        const uint8_t *findBlock(Addr addr, bool is_secure) const override;

      private:
        CCHIL1Agent *agent;
    };

    TaurusCacheAccessor cacheAccessor;
    prefetch::Base *prefetcher;

    // Probe points feeding the hosted L2 prefetcher (created when a
    // prefetcher is configured; fired by firePfProbe / fill completions)
    ProbePointArg<PacketPtr> *ppMiss = nullptr;
    ProbePointArg<PacketPtr> *ppHit = nullptr;
    ProbePointArg<PacketPtr> *ppFill = nullptr;
    ProbePointArg<PacketPtr> *ppStorePFTrain = nullptr;

    /** Approximate-L2-lookup probe feed for the hosted prefetcher. */
    void firePfProbe(PacketPtr pkt);

  protected: // Params and node binding
    CCHIFabric *fabric;
    const uint32_t nodeId;
    const Cycles hitLatency;
    const bool snoopMerge;
    const uint32_t xactionLimitREQ;
    const uint32_t xactionLimitEVT;
    const uint32_t xactionLimitSNP;

    TaurusNode *node = nullptr;
    size_t upstreamIndex = 0;

  protected: // Bypass routing state
    /** Remembers which cpu_side port a bypassed request arrived on. */
    class BypassSenderState : public Packet::SenderState
    {
      public:
        const PortID portId;
        explicit BypassSenderState(PortID _portId) : portId(_portId) { }
    };

  public:
    struct CCHIL1AgentStats : public statistics::Group
    {
        CCHIL1AgentStats(statistics::Group *parent);

        statistics::Scalar reqHits;
        statistics::Scalar reqMisses;
        statistics::Scalar reads;
        statistics::Scalar readExes;
        statistics::Scalar upgrades;
        statistics::Scalar writes;
        statistics::Scalar hardPrefetches;
        statistics::Scalar rmwReads;
        statistics::Scalar rmwWrites;
        statistics::Scalar writebacks;
        statistics::Scalar cleanEvicts;
        statistics::Scalar writeCleans;
        statistics::Scalar fences;
        statistics::Scalar cmos;
        statistics::Scalar deniedReqLimit;
        statistics::Scalar deniedTotalLimit;
        statistics::Scalar deniedPABusy;
        statistics::Scalar deniedCMOLimit;
        statistics::Scalar blockedSameLine;
        statistics::Scalar storeEscalations;
        statistics::Scalar lateGrants;
        statistics::Scalar completionRetries;
        statistics::Scalar evictions;
        statistics::Scalar evictRetries;
        statistics::Scalar evictMisses;
        statistics::Scalar evictForRefetch;
        statistics::Scalar evictForRefetchDenied;
        statistics::Scalar evictCancelled;
        statistics::Scalar writebacksDropped;
        statistics::Scalar bypassReqs;
        statistics::Scalar atomicBypasses;
        statistics::Scalar functionalBypasses;
        statistics::Scalar snoopReflected;
        statistics::Scalar snoopRespDropped;
        statistics::Scalar snoopMerged;
        statistics::Scalar snoopMergeLineGone;
        statistics::Scalar storeFlightBackoff;
        statistics::Scalar snoopMergeStoreFailed;
        statistics::Scalar snoopMergeTimeouts;
        statistics::Scalar snoopLateMerges;
        statistics::Scalar snoopLateMerged;
        statistics::Scalar snoopLateMergeDropped;
        statistics::Scalar localSnoopIssued;
        statistics::Scalar localSnoopMerged;
        statistics::Scalar homeSnoopDeferred;
        statistics::Scalar mergeBlockedReqs;
        statistics::Scalar pfStashIssued;
        statistics::Scalar pfStashDropped;
        statistics::Scalar pfStoreTrains;
    } stats;
};

} // namespace gem5

#endif // __MEM_CCHI_CCHI_L1_AGENT_HH__
