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
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "mem/cchi/cchi_l1_agent.hh"

#include <cstring>

#include "base/logging.hh"
#include "mem/cache/prefetch/base.hh"
#include "sim/system.hh"

// Debug-logging plumbing: the gem5 build defines CCHI_HAVE_GEM5_DEBUG (see
// src/mem/cchi/SConscript) and the messages go through the CCHI debug flag.
// Without it (e.g. standalone syntax checks, where the generated
// debug/CCHI.hh does not exist) DPRINTF* are forced to no-ops; the #undef
// is needed because base/trace.hh (which defines the real macro and would
// require the generated gem5::debug::CCHI flag) may already have been
// pulled in transitively.
#ifdef CCHI_HAVE_GEM5_DEBUG
#include "base/trace.hh"
#include "debug/CCHI.hh"
#else
#undef DPRINTF
#define DPRINTF(...) do { } while (0)
#undef DPRINTFR
#define DPRINTFR(...) do { } while (0)
#endif

namespace gem5
{

namespace TaurusDenial = CCHI::Taurus::Denial;
namespace TaurusState = CCHI::Taurus::CacheState;

CCHIL1Agent::CCHIL1Agent(const CCHIL1AgentParams &p)
    : ClockedObject(p),
      memSidePort(p.name + ".mem_side", *this),
      reqQueue(*this, memSidePort),
      snoopRespQueue(*this, memSidePort),
      cacheAccessor(this),
      prefetcher(p.l2_prefetcher),
      fabric(p.fabric),
      nodeId(p.node_id),
      hitLatency(p.hit_latency),
      snoopMerge(p.snoop_merge),
      xactionLimitREQ(p.xaction_limit_req),
      xactionLimitEVT(p.xaction_limit_evt),
      xactionLimitSNP(p.xaction_limit_snp),
      stats(this)
{
    // L2CacheWrapper-style prefetch hosting: the accessor is backed by the
    // Taurus node (resolved lazily), the block size is 64 (CCHI line)
    if (prefetcher) {
        prefetcher->setParentInfo(p.system, getProbeManager(),
                                  &cacheAccessor, 64);
        // The hosted prefetcher subscribes to the classic cache probe
        // points (see prefetch::Base::setParentInfo); with no gem5 L2 in
        // the path the agent fires them itself (see firePfProbe)
        ppMiss = new ProbePointArg<PacketPtr>(getProbeManager(), "Miss");
        ppHit = new ProbePointArg<PacketPtr>(getProbeManager(), "Hit");
        ppFill = new ProbePointArg<PacketPtr>(getProbeManager(), "Fill");
        ppStorePFTrain =
            new ProbePointArg<PacketPtr>(getProbeManager(), "StorePFtrain");
    }
}

// Probe feed for the hosted L2 prefetcher, approximating an L2 lookup:
// a Taurus-resident line counts as an L2 hit, anything else as a miss.
// Evictions, fences and bypassed traffic do not train prefetchers (they
// do not in the gem5 L2 either).
void
CCHIL1Agent::firePfProbe(PacketPtr pkt)
{
    if (!prefetcher)
        return;

    if (taurus()->IsValid(pkt->getAddr() & ~Addr(63)))
        ppHit->notify(pkt);
    else
        ppMiss->notify(pkt);
}

void
CCHIL1Agent::init()
{
    fatal_if(cpuSidePorts.empty(),
             "%s: no cpu_side port connected\n", name());
    for (auto &port : cpuSidePorts)
        fatal_if(!port->isConnected(),
                 "%s: unconnected cpu_side port %s\n",
                 name(), port->name());
    fatal_if(!memSidePort.isConnected(),
             "%s: mem_side port is not connected\n", name());

    fabric->addUpstreamNode(nodeId, this);
}

Port &
CCHIL1Agent::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_side") {
        panic_if(idx == InvalidPortID,
                 "%s: cpu_side is a vector port and needs an index\n",
                 name());
        while (cpuSidePorts.size() <= static_cast<size_t>(idx)) {
            const PortID id = cpuSidePorts.size();
            cpuSidePorts.emplace_back(std::make_unique<AgentResponsePort>(
                name() + ".cpu_side[" + std::to_string(id) + "]",
                *this, id));
        }
        return *cpuSidePorts[idx];
    } else if (if_name == "mem_side") {
        return memSidePort;
    }
    return ClockedObject::getPort(if_name, idx);
}

CCHIL1Agent::TaurusNode *
CCHIL1Agent::taurus()
{
    if (!node) {
        node = fabric->getUpstreamNode(nodeId);
        panic_if(!node, "%s: no Taurus node for node_id %u\n",
                 name(), nodeId);
    }
    return node;
}

// ---------------------------------------------------------------------------
// Request path (cpu_side -> Taurus / bypass)
// ---------------------------------------------------------------------------

bool
CCHIL1Agent::recvTimingReq(PacketPtr pkt, PortID portId)
{
    const Addr addr = pkt->getAddr();
    const Addr linePA = addr & ~Addr(63);

    // Store-prefetch training hints from the core/LSU: forward to the
    // hosted prefetcher's StorePFtrain probe point and drop (gem5's L2
    // semantics: no response, no memory traffic)
    if (pkt->cmd == MemCmd::StorePFTrain) {
        ++stats.pfStoreTrains;
        if (ppStorePFTrain)
            ppStorePFTrain->notify(pkt);
        delete pkt;
        return true;
    }

    // Uncached/MMIO and everything outside the fabric window bypasses CCHI
    // entirely (the Taurus DoNonCacheable* entry points are nullptr stubs
    // in CHIron; if they land, uncached ops can optionally go through
    // Taurus instead - config knob, see the plan)
    if (pkt->req->isUncacheable() || !fabric->isCacheable(addr)) {
        ++stats.bypassReqs;
        DPRINTF(CCHI, "[%s] bypass %s\n", name(), pkt->print());
        pkt->pushSenderState(new BypassSenderState(portId));
        const Tick receiveDelay = pkt->headerDelay + pkt->payloadDelay;
        pkt->headerDelay = pkt->payloadDelay = 0;
        memSidePort.schedTimingReq(pkt, curTick() + receiveDelay);
        return true;
    }

    // A held home snoop blocks new L1 requests to the same line for the
    // duration of the merge (snoop_merge flow; the bridge hazard rule)
    if (merge && merge->linePA == linePA) {
        ++stats.mergeBlockedReqs;
        blockedPorts.insert(portId);
        return false;
    }

    return handleCchiRequest(pkt, portId);
}

bool
CCHIL1Agent::handleCchiRequest(PacketPtr pkt, PortID portId)
{
    const Addr linePA = pkt->getAddr() & ~Addr(63);

    // Same-line serialization: never issue a second bridge transaction
    // for a line with an undrained pending entry. Taurus's own
    // PA_REQ_BUSY rejection covers the in-flight window, but NOT the
    // post-completion/pre-drain window (the Taurus tracker is already
    // freed, so a duplicate Do* would succeed and collide in the pending
    // table). Backpressure instead: the L1 retries after the pending
    // entry drains and the tick hook releases the port.
    if (pending.count(linePA) || localSnoops.count(linePA)) {
        ++stats.blockedSameLine;
        DPRINTF(CCHI, "[%s] NACK %s (line %#llx pending%s)\n", name(),
                pkt->print(), linePA,
                pending.count(linePA) ? " xact" : " local-snoop");
        blockedPorts.insert(portId);
        return false;
    }

    // Admission of a new bridge transaction re-establishes tracking of
    // this line, so any eviction queued from the PREVIOUS ownership
    // epoch (a denied DoEvict parked in the retry queue) is now stale:
    // letting it fire later would drop the line out from under the L1's
    // fresh copy (observed: deferred writeback's queued evict reaping
    // the line right after the L1 re-acquired it with a ReadEx).
    if (!evictRetryQueue.empty()) {
        const auto erased = std::erase(evictRetryQueue, linePA);
        if (erased) {
            stats.evictCancelled += erased;
            DPRINTF(CCHI, "[%s] cancelled %llu stale queued evict(s) for "
                    "%#llx\n", name(), (unsigned long long)erased, linePA);
        }
    }

    switch (pkt->cmd.toInt()) {
      case MemCmd::WritebackDirty:
        return handleWriteback(pkt, portId, true);

      case MemCmd::WritebackClean:
      case MemCmd::CleanEvict:
        return handleWriteback(pkt, portId, false);

      case MemCmd::MemFenceReq:
      case MemCmd::MemSyncReq:
        return handleFence(pkt, portId);

      default:
        break;
    }

    // Local snoop: a node-resident line may have fresher data in a
    // sibling L1 (the node only learns of L1 writes at writeback/merge
    // time), and a write-class request must also invalidate sibling
    // copies before the writer proceeds (xbar-equivalent semantics).
    // Node-absent lines skip this: sibling-resident implies
    // node-resident by construction (every fill goes through the node).
    if (taurus()->IsValid(linePA)) {
        const int expected = reflectLocalSnoop(pkt, portId);
        if (expected > 0) {
            ++stats.localSnoopIssued;
            DPRINTF(CCHI, "[%s] local snoop for %#llx (expects %d)\n",
                    name(), linePA, expected);
            localSnoops.emplace(linePA,
                                LocalSnoop{pkt, portId, expected,
                                           curCycle()});
            return true;
        }
        // No sibling holds the line: nothing to merge, issue inline
    }

    return dispatchCchiRequest(pkt, portId);
}

bool
CCHIL1Agent::dispatchCchiRequest(PacketPtr pkt, PortID portId)
{
    switch (pkt->cmd.toInt()) {
      case MemCmd::WritebackDirty:
        return handleWriteback(pkt, portId, true);

      case MemCmd::WritebackClean:
      case MemCmd::CleanEvict:
        return handleWriteback(pkt, portId, false);

      case MemCmd::MemFenceReq:
      case MemCmd::MemSyncReq:
        return handleFence(pkt, portId);

      default:
        // Demand/store/prefetch traffic: feed the hosted L2 prefetcher's
        // training stream (approximate L2-lookup semantics)
        firePfProbe(pkt);
        break;
    }

    switch (pkt->cmd.toInt()) {
      case MemCmd::ReadReq:
      case MemCmd::ReadSharedReq:
      case MemCmd::ReadCleanReq:
      case MemCmd::LoadLockedReq:
        // Read variants a gem5 cache may emit on its mem side (the XS L1D
        // uses ReadSharedReq for plain data loads): all map to DoLoad
        // (ReadShared grant). LoadLockedReq degrades to a plain load here:
        // the LL/SC reservation machinery lives in the L1/ISA (its
        // reservation clearing is fed by our snoop reflection path)
        ++stats.reads;
        return issueLoad(pkt, portId, ACT_READ);

      case MemCmd::ReadExReq:
        // store miss: fill + ownership (ReadUnique with ExpCompData=1)
        ++stats.readExes;
        return issueStore(pkt, portId, ACT_READ_EX, false);

      case MemCmd::UpgradeReq:
      case MemCmd::SCUpgradeReq:
        // S->U upgrade (ReadUnique with ExpCompData=0, no data - designed
        // CHIron behavior). The SC success/fail decision lives in the
        // L1/ISA; failing SCs arrive as SCUpgradeFailReq/StoreCondFailReq
        // and are handled like ReadEx below
        ++stats.upgrades;
        return issueStore(pkt, portId, ACT_UPGRADE, false);

      case MemCmd::StoreCondReq:
        // A data-carrying SC that reached the memory side: acquire
        // ownership and commit the data like a store; the L1/ISA owns
        // the reservation check, so downstream success means SC success
        ++stats.upgrades;
        return issueStore(pkt, portId, ACT_WRITE, false);

      case MemCmd::WriteReq:
        ++stats.writes;
        return issueStore(pkt, portId, ACT_WRITE, false);

      case MemCmd::WriteLineReq:
        ++stats.writes;
        return issueStore(pkt, portId, ACT_WRITE, true);

      case MemCmd::InvalidateReq:
        // XS fast-writeline conversion of a whole-line write miss
        // (cache.cc createMissPacket): "give me ownership, I will
        // overwrite the whole line" - the miss packet carries no data,
        // so this is ownership-only like an upgrade, but always a
        // full-line acquisition (DoStoreLine, dataless by design)
        ++stats.writes;
        return issueStore(pkt, portId, ACT_INV_WRITE, true);

      case MemCmd::SCUpgradeFailReq:
      case MemCmd::StoreCondFailReq:
        // A failing SC still fetches ownership + data (to supply
        // snoopers); the SC failure itself is decided in the L1/ISA
        ++stats.readExes;
        return issueStore(pkt, portId, ACT_READ_EX, false);

      case MemCmd::HardPFReq:
      case MemCmd::SoftPFReq:
      case MemCmd::SoftPFExReq:
        // L1-originated prefetch: gem5's HardPFReq is NeedsWritable|
        // IsInvalidate, so ownership (DoStore/ReadUnique) is required -
        // a Shared DoLoad fill would let the L1 store-hit later without
        // an UpgradeReq, and the writeback would then deadlock on a
        // non-Unique Taurus line. Always respond, the L1's prefetch
        // MSHRs wedge otherwise.
        ++stats.hardPrefetches;
        return issueStore(pkt, portId, ACT_HARD_PF, false);

      case MemCmd::LockedRMWReadReq:
        // AMO read phase: acquire Unique + data, pin the line (never
        // evicted by the bridge while pinned)
        ++stats.rmwReads;
        return issueStore(pkt, portId, ACT_RMW_READ, false);

      case MemCmd::LockedRMWWriteReq:
        return handleRMWWrite(pkt, portId);

      case MemCmd::SwapReq:
        // AMO forwarded by the L1 on a miss: RMW under Unique ownership
        // on the node line; the old value goes back as the response data
        ++stats.rmwReads;
        return issueStore(pkt, portId, ACT_SWAP, false);

      case MemCmd::WritebackDirty:
        return handleWriteback(pkt, portId, true);

      case MemCmd::WritebackClean:
      case MemCmd::CleanEvict:
        return handleWriteback(pkt, portId, false);

      case MemCmd::WriteClean:
        return handleWriteClean(pkt, portId);

      case MemCmd::MemFenceReq:
      case MemCmd::MemSyncReq:
        return handleFence(pkt, portId);

      case MemCmd::CleanSharedReq:
        return issueCMO(pkt, portId, false);

      case MemCmd::CleanInvalidReq:
        return issueCMO(pkt, portId, true);

      default:
        fatal("%s: unexpected command from L1: %s\n",
              name(), pkt->cmd.toString());
    }
}

bool
CCHIL1Agent::issueLoad(PacketPtr pkt, PortID portId, XactAction action)
{
    auto future = taurus()->DoLoad(pkt->getAddr());

    if (future->IsRejected())
        return handleDenial(future->GetDenial(), portId);

    const Addr linePA = pkt->getAddr() & ~Addr(63);
    const bool hit = future->IsNow();
    if (hit)
        ++stats.reqHits;
    else
        ++stats.reqMisses;

    DPRINTF(CCHI, "[%s] DoLoad %#llx -> %s\n", name(), linePA,
            hit ? "hit" : "miss");

    PendingXaction entry{linePA, pkt, portId, action, hit, 0, nullptr};
    panic_if(!pending.emplace(linePA, entry).second,
             "%s: duplicate pending entry for line %#llx\n", name(), linePA);

    // The lambda only posts to the completion queue; for a now-future it
    // runs synchronously here, for a pending future it runs inside the
    // fabric's Instance::Tick. Responses are produced by the tick hook,
    // never directly from the callback.
    future->Bind([this, linePA](const TaurusNode::GrantedEvent &ev) {
        onGrantFired(linePA, ev);
    });
    return true;
}

bool
CCHIL1Agent::issueStore(PacketPtr pkt, PortID portId, XactAction action,
                        bool wholeLine)
{
    // Dataless-upgrade hazard (documented Taurus behavior): a DoStore on a
    // resident Shared line latches ExpCompData=0 and never refetches, so
    // if a racing home snoop invalidates the line in flight, the grant
    // completes Unique with the PRE-SNOOP (stale) data - and any later
    // eviction would even write the stale copy back over fresher home
    // data. DoStoreLine is exempt by construction (the L1 overwrites the
    // whole line, so stale content never survives). For partial stores,
    // evict the Shared line first (dataless EVT, the home keeps its
    // copy): the ReadUnique below then always goes the with-data path.
    if (!wholeLine) {
        auto line = taurus()->GetCacheLine(pkt->getAddr());
        if (line && line->GetState() == CCHI::Taurus::CacheState::Shared) {
            auto evFuture = taurus()->DoEvict(pkt->getAddr());
            if (evFuture->IsRejected()) {
                const DenialEnum denial = evFuture->GetDenial();
                if (denial != TaurusDenial::REJECTED_TAURUS_EVICT_MISS) {
                    // Busy/limits: back off and retry the whole store on a
                    // later tick (the L1 re-issues after sendRetryReq)
                    ++stats.evictForRefetchDenied;
                    DPRINTF(CCHI, "[%s] evict-for-refetch %#llx DENIED "
                            "(%s)\n", name(),
                            pkt->getAddr() & ~Addr(63), denial->name);
                    blockedPorts.insert(portId);
                    return false;
                }
                // EVICT_MISS: already gone - DoStore below fetches data
            } else {
                ++stats.evictForRefetch;
                DPRINTF(CCHI, "[%s] evict-for-refetch %#llx\n", name(),
                        pkt->getAddr() & ~Addr(63));
            }
        }
    }

    // A resident line with a snoop or eviction in flight is about to be
    // demoted/reaped: granting a store hit on it would write the new data
    // into a dying line - the home only ever learns the pre-store content
    // (via the in-flight answer or CopyBack), and the post-store content
    // would sit in a detached line object until it vanishes (observed in
    // the XSCache linux boot: a back-invalidation answered in the same
    // window as a store hit, reaping the line before the next merge). Back
    // off and let the L1 retry: by then the line is either gone (a proper
    // miss re-acquires through the home) or the flight has resolved.
    if (auto line = taurus()->GetCacheLine(pkt->getAddr());
        line && (line->IsSNPInFlight(taurus()->glbl) ||
                 line->IsEVTInFlight(taurus()->glbl))) {
        ++stats.storeFlightBackoff;
        DPRINTF(CCHI, "[%s] DoStore %#llx backed off (snoop/evict in "
                "flight)\n", name(), pkt->getAddr() & ~Addr(63));
        blockedPorts.insert(portId);
        return false;
    }

    auto future = wholeLine ? taurus()->DoStoreLine(pkt->getAddr())
                            : taurus()->DoStore(pkt->getAddr());

    if (future->IsRejected()) {
        DPRINTF(CCHI, "[%s] DoStore %#llx (a%d) DENIED\n", name(),
                pkt->getAddr() & ~Addr(63), static_cast<int>(action));
        return handleDenial(future->GetDenial(), portId);
    }

    const Addr linePA = pkt->getAddr() & ~Addr(63);
    const bool hit = future->IsNow();
    if (hit)
        ++stats.reqHits;
    else
        ++stats.reqMisses;

    DPRINTF(CCHI, "[%s] DoStore %#llx (a%d) -> %s\n", name(), linePA,
            static_cast<int>(action), hit ? "hit" : "miss");

    PendingXaction entry{linePA, pkt, portId, action, hit, 0, nullptr};
    panic_if(!pending.emplace(linePA, entry).second,
             "%s: duplicate pending entry for line %#llx\n", name(), linePA);

    future->Bind([this, linePA](const TaurusNode::GrantedEvent &ev) {
        onGrantFired(linePA, ev);
    });
    return true;
}

bool
CCHIL1Agent::issueCMO(PacketPtr pkt, PortID portId, bool invalidate)
{
    ++stats.cmos;

    auto future = invalidate ? taurus()->DoCBOInval(pkt->getAddr())
                             : taurus()->DoCBOClean(pkt->getAddr());

    if (future->IsRejected())
        return handleDenial(future->GetDenial(), portId);

    const Addr linePA = pkt->getAddr() & ~Addr(63);

    PendingXaction entry{linePA, pkt, portId, ACT_CMO,
                         future->IsNow(), 0, nullptr};
    panic_if(!pending.emplace(linePA, entry).second,
             "%s: duplicate pending entry for line %#llx\n", name(), linePA);

    // TODO(cchi): endpoint-side CMO servicing is still pending in CHIron
    // (Earth does not answer CMOs with CompCMO today), so with the Earth
    // endpoint a CMO request currently never completes. The completion
    // path is live for endpoints that service CMOs.
    future->Bind([this, linePA](const TaurusNode::CMOCompleteEvent &ev) {
        onCMOFired(linePA, ev);
    });
    return true;
}

bool
CCHIL1Agent::handleRMWWrite(PacketPtr pkt, PortID portId)
{
    ++stats.rmwWrites;

    const Addr linePA = pkt->getAddr() & ~Addr(63);
    auto line = taurus()->GetCacheLine(linePA);

    if (!line || !pinnedLines.count(linePA)) {
        // With snoop_merge=off a home snoop can drop the pinned line from
        // Taurus between the RMW read and write (the off mode's
        // bounded-staleness stance): re-acquire ownership and commit the
        // modified bytes at the grant instead of failing. The write
        // packet carries the final (post-AMO) bytes, the grant fills the
        // rest of the line with the current content.
        warn_once("%s: RMW write re-acquiring line %#llx (pin lost)\n",
                  name(), linePA);
        return issueStore(pkt, portId, ACT_RMW_WRITE, false);
    }

    // The line is already Unique and held: go through the completion
    // machinery with a pre-granted entry (store + unpin + respond)
    PendingXaction entry{linePA, pkt, portId, ACT_RMW_WRITE,
                         true, curTick(), std::move(line)};
    panic_if(!pending.emplace(linePA, entry).second,
             "%s: duplicate pending entry for line %#llx\n", name(), linePA);
    completionQueue.push_back(linePA);
    return true;
}

bool
CCHIL1Agent::handleWriteback(PacketPtr pkt, PortID portId, bool dirty)
{
    const Addr linePA = pkt->getAddr() & ~Addr(63);

    panic_if(pinnedLines.count(linePA),
             "%s: eviction of RMW-pinned line %#llx\n", name(), linePA);

    // Evictions need no response (InvalidCmd in gem5's commandInfo): the
    // bridge is the final destination and consumes the packet
    if (dirty) {
        ++stats.writebacks;

        auto line = taurus()->GetCacheLine(linePA);
        if (!line) {
            // The node no longer tracks this line. By the bridge
            // invariant an M-dirty L1 line always implies a resident
            // Unique node line, so this is only reachable for an
            // O-state (shared-dirty) eviction whose data was already
            // merged into the home when the line was downgraded - the
            // writeback is redundant and can be dropped. The stale
            // queued-evict race that could re-dirty the L1 copy after
            // the node reaped it is closed by the cancel-on-admit rule
            // in handleCchiRequest.
            ++stats.writebacksDropped;
            warn_once("%s: dropping WritebackDirty for non-resident line "
                      "%#llx (data already at the home)\n", name(), linePA);
            delete pkt;
            return true;
        }

        if (storeToLine(pkt, *line)) {
            // Fresh data committed to the Taurus line; the eviction then
            // propagates asynchronously (EVT WriteBackFull carries the
            // data to the home)
            DPRINTF(CCHI, "[%s] WritebackDirty %#llx stored + evict\n",
                    name(), linePA);
            delete pkt;
            issueEvict(linePA);
        } else {
            // Defensive: the checked store should not fail on a Unique,
            // resident line; defer through the completion machinery
            DPRINTF(CCHI, "[%s] WritebackDirty %#llx DEFERRED (state %s)\n",
                    name(), linePA, line->GetState()->name);
            warn_once("%s: deferred store for WritebackDirty %#llx\n",
                      name(), linePA);
            PendingXaction entry{linePA, pkt, portId, ACT_WRITEBACK,
                                 true, curTick(), std::move(line)};
            panic_if(!pending.emplace(linePA, entry).second,
                     "%s: duplicate pending entry for line %#llx\n",
                     name(), linePA);
            completionQueue.push_back(linePA);
        }
    } else {
        ++stats.cleanEvicts;
        // Clean data: the Taurus copy is at least as fresh; drop the line
        // from the node (EVT Evict)
        delete pkt;
        issueEvict(linePA);
    }
    return true;
}

bool
CCHIL1Agent::handleWriteClean(PacketPtr pkt, PortID portId)
{
    ++stats.writeCleans;

    const Addr linePA = pkt->getAddr() & ~Addr(63);
    auto line = taurus()->GetCacheLine(linePA);
    if (!line) {
        // Same non-resident situation as handleWriteback: the WriteClean
        // data is either clean or already merged into the home, so the
        // packet can be dropped (see the epoch argument there)
        ++stats.writebacksDropped;
        warn_once("%s: dropping WriteClean for non-resident line %#llx\n",
                  name(), linePA);
        delete pkt;
        return true;
    }

    if (storeToLine(pkt, *line)) {
        // No response needed: consume the packet, the line stays resident
        delete pkt;
    } else {
        warn_once("%s: deferred store for WriteClean %#llx\n",
                  name(), linePA);
        PendingXaction entry{linePA, pkt, portId, ACT_WRITE_CLEAN,
                             true, curTick(), std::move(line)};
        panic_if(!pending.emplace(linePA, entry).second,
                 "%s: duplicate pending entry for line %#llx\n",
                 name(), linePA);
        completionQueue.push_back(linePA);
    }
    return true;
}

bool
CCHIL1Agent::handleFence(PacketPtr pkt, PortID portId)
{
    ++stats.fences;

    // Drain agent futures, then respond. In-flight asynchronous evictions
    // play the role of a write buffer here and are not awaited: their
    // data was committed to Taurus before the writeback was answered.
    if (pending.empty() && completionQueue.empty()) {
        respondTo(pkt, portId, curTick() + clockPeriod());
    } else {
        fenceQueue.emplace_back(pkt, portId);
    }
    return true;
}

bool
CCHIL1Agent::handleDenial(DenialEnum denial, PortID portId)
{
    if (denial == TaurusDenial::REJECTED_TAURUS_REQ_LIMIT_EXCEEDED) {
        ++stats.deniedReqLimit;
    } else if (denial == TaurusDenial::REJECTED_TAURUS_TOTAL_LIMIT_EXCEEDED) {
        ++stats.deniedTotalLimit;
    } else if (denial == TaurusDenial::REJECTED_TAURUS_PA_REQ_BUSY) {
        // Legitimate with L1I+L1D fan-in (two independent MSHR pools may
        // target the same line); retried on a later fabric tick
        ++stats.deniedPABusy;
    } else if (denial == TaurusDenial::REJECTED_TAURUS_CMO_LIMIT_EXCEEDED) {
        ++stats.deniedCMOLimit;
    } else {
        fatal("%s: unexpected Taurus denial %s\n", name(), denial->name);
    }

    blockedPorts.insert(portId);
    return false;
}

// ---------------------------------------------------------------------------
// Completion path
// ---------------------------------------------------------------------------

void
CCHIL1Agent::onGrantFired(Addr linePA,
                          [[maybe_unused]] const TaurusNode::GrantedEvent &ev)
{
    auto it = pending.find(linePA);
    if (it == pending.end()) {
        // Late/duplicate grant: FutureNow supports multiple fires and an
        // escalation grant can race with the entry completing through
        // another path. The entry is gone, the grant is stale - benign.
        ++stats.lateGrants;
        DPRINTF(CCHI, "[%s] late grant for %#llx (no pending entry)\n",
                name(), linePA);
        return;
    }

    // The const event only hands out shared_ptr<const CacheLine>;
    // re-fetch the writable handle from the node (the line was just
    // granted, so it is necessarily resident)
    it->second.line = taurus()->GetCacheLine(linePA);
    panic_if(!it->second.line,
             "%s: granted line %#llx not resident\n", name(), linePA);

    // Snapshot the fill data for read actions (see PendingXaction::fillData):
    // taken while the grant has just completed, so a later home snoop
    // invalidating the line cannot starve the fill
    switch (it->second.action) {
      case ACT_READ:
      case ACT_READ_EX:
      case ACT_HARD_PF:
      case ACT_RMW_READ: {
        const auto span = it->second.line->Load();
        if (span) {
            std::array<uint64_t, 8> buf;
            std::memcpy(buf.data(), span->data(), 64);
            it->second.fillData = buf;
        }
        break;
      }
      default:
        break;
    }

    it->second.grantedTick = curTick();
    DPRINTF(CCHI, "[%s] grant fired for %#llx\n", name(), linePA);
    completionQueue.push_back(linePA);
}

void
CCHIL1Agent::onCMOFired(Addr linePA,
                        [[maybe_unused]] const TaurusNode::CMOCompleteEvent &ev)
{
    auto it = pending.find(linePA);
    if (it == pending.end()) {
        // Same late/duplicate-fire rationale as onGrantFired
        ++stats.lateGrants;
        return;
    }

    it->second.grantedTick = curTick();
    completionQueue.push_back(linePA);
}

bool
CCHIL1Agent::processCompletion(PendingXaction &entry)
{
    const Tick delay = entry.grantedHit ? cyclesToTicks(hitLatency)
                                        : clockPeriod();

    switch (entry.action) {
      case ACT_READ:
      case ACT_READ_EX:
      case ACT_HARD_PF:
        // Fill data readback: grant-time snapshot when present (immune to
        // a racing home snoop invalidating the granted line), else the
        // line's checked accessors; an empty optional means the line is
        // not consistent yet -> retry next fabric tick
        if (!fillFromEntry(entry))
            return false;
        // Feed the hosted prefetcher's Fill stream (L2-refill semantics)
        if (ppFill)
            ppFill->notify(entry.pkt);
        // Mirror the Taurus grant state into the response: a DoLoad grant
        // is Shared - if the L1 filled it as writable (gem5 marks
        // no-sharers fills Exclusive), it would store-hit without an
        // UpgradeReq and a later writeback would deadlock on the Shared
        // Taurus line. DoStore/DoStoreLine grants are Unique (writable),
        // so no sharers flag there.
        if (entry.action == ACT_READ)
            entry.pkt->setHasSharers();
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      case ACT_RMW_READ:
        if (!fillFromEntry(entry))
            return false;
        // Pin the line: the bridge never evicts pinned lines; with
        // snoop_merge=on, SNPs to this line are held by the merge flow
        pinnedLines.insert(entry.linePA);
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      case ACT_UPGRADE:
        // No data: S->U upgrade (ExpCompData=0) by design
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      case ACT_INV_WRITE:
        // Ownership only (full-line overwrite follows inside the L1):
        // the miss packet carried no data, so nothing to store - the
        // L1's own write makes the line fresh, and the next merge or
        // writeback syncs it into the node
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      case ACT_WRITE:
        if (!storeWithEscalation(entry))
            return false;
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      case ACT_RMW_WRITE:
        if (!storeWithEscalation(entry))
            return false;
        pinnedLines.erase(entry.linePA);
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      case ACT_SWAP:
        if (!storeWithEscalation(entry,
                                 [this](PacketPtr p, TaurusCacheLine &l) {
                                     return performAtomicOp(p, l);
                                 }))
            return false;
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      case ACT_WRITEBACK:
        if (!storeWithEscalation(entry))
            return false;
        // Evictions need no response; consume the packet
        delete entry.pkt;
        issueEvict(entry.linePA);
        return true;

      case ACT_WRITE_CLEAN:
        if (!storeWithEscalation(entry))
            return false;
        delete entry.pkt;
        return true;

      case ACT_CMO:
        respondTo(entry.pkt, entry.portId, entry.grantedTick + delay);
        return true;

      default:
        fatal("%s: bad pending action %u\n", name(), entry.action);
    }
}

bool
CCHIL1Agent::fillFromEntry(PendingXaction &entry)
{
    if (entry.fillData) {
        const Addr offset = entry.pkt->getAddr() & Addr(63);
        const unsigned size = entry.pkt->getSize();
        uint8_t *data = entry.pkt->getPtr<uint8_t>();
        std::memcpy(data,
                    reinterpret_cast<const uint8_t *>(entry.fillData->data()) +
                        offset,
                    size);
        return true;
    }
    return fillFromLine(entry.pkt, *entry.line);
}

bool
CCHIL1Agent::fillFromLine(PacketPtr pkt, TaurusCacheLine &line)
{
    const Addr offset = pkt->getAddr() & Addr(63);
    const unsigned size = pkt->getSize();
    uint8_t *data = pkt->getPtr<uint8_t>();

    if (offset == 0 && size == 64) {
        const auto span = line.Load();
        if (!span)
            return false;
        std::memcpy(data, span->data(), 64);
        return true;
    }

    for (unsigned i = 0; i < size; ++i) {
        const auto byte = line.Load8(offset + i);
        if (!byte)
            return false;
        data[i] = *byte;
    }
    return true;
}

bool
CCHIL1Agent::storeToLine(PacketPtr pkt, TaurusCacheLine &line)
{
    const Addr offset = pkt->getAddr() & Addr(63);
    const unsigned size = pkt->getSize();
    const uint8_t *data = pkt->getConstPtr<uint8_t>();

    if (offset == 0 && size == 64) {
        std::array<uint64_t, 8> buf;
        std::memcpy(buf.data(), data, 64);
        return line.Store(std::span<const uint64_t, 8>(buf));
    }

    for (unsigned i = 0; i < size; ++i)
        if (!line.Store8(offset + i, data[i]))
            return false;
    return true;
}

bool
CCHIL1Agent::performAtomicOp(PacketPtr pkt, TaurusCacheLine &line)
{
    const Addr offset = pkt->getAddr() & Addr(63);

    // Read-modify-write on the granted line: copy the old value into the
    // packet (the AMO's return value), apply the functor in place, and
    // commit. Runs inside one processCompletion call, so no gem5-side
    // event (snoop merge, ...) can interleave between Load and Store;
    // a failed store retries the whole op on the freshest line content.
    const auto span = line.Load();
    if (!span)
        return false;

    std::array<uint64_t, 8> buf;
    std::memcpy(buf.data(), span->data(), 64);
    uint8_t *lane = reinterpret_cast<uint8_t *>(buf.data()) + offset;
    pkt->setData(lane);
    (*pkt->getAtomicOp())(lane);
    return line.Store(std::span<const uint64_t, 8>(buf));
}

bool
CCHIL1Agent::storeWithEscalation(PendingXaction &entry,
                                 const StoreOp &op)
{
    if (op(entry.pkt, *entry.line))
        return true;

    // Bounded retry: a persistently failing store means the line is not
    // (or no longer) Unique - e.g. it was downgraded after the grant.
    // Escalate by re-acquiring ownership (DoStore upgrades S->U with
    // ExpCompData=0, or re-fills an absent line which the store then
    // overwrites), never spinning forever.
    if (++entry.retries >= STORE_RETRY_ESCALATE) {
        entry.retries = 0;
        ++stats.storeEscalations;
        DPRINTF(CCHI, "[%s] escalating store for %#llx (DoStore)\n",
                name(), entry.linePA);
        auto future = taurus()->DoStore(entry.linePA);
        if (future->IsRejected()) {
            DPRINTF(CCHI, "[%s] escalation for %#llx REJECTED (%s)\n",
                    name(), entry.linePA, future->GetDenial()->name);
        } else {
            future->Bind([this, linePA = entry.linePA]
                         (const TaurusNode::GrantedEvent &ev) {
                onGrantFired(linePA, ev);
            });
        }
    }
    return false;
}

void
CCHIL1Agent::respondTo(PacketPtr pkt, PortID portId, Tick when)
{
    panic_if(portId == InvalidPortID ||
             static_cast<size_t>(portId) >= cpuSidePorts.size(),
             "%s: response for unknown cpu_side port %d\n", name(), portId);
    pkt->makeTimingResponse();
    cpuSidePorts[portId]->schedTimingResp(pkt, when);
}

void
CCHIL1Agent::issueEvict(Addr linePA)
{
    auto future = taurus()->DoEvict(linePA);

    if (future->IsRejected()) {
        const DenialEnum denial = future->GetDenial();
        if (denial == TaurusDenial::REJECTED_TAURUS_EVICT_MISS) {
            // The line is already gone (e.g. a home snoop dropped it);
            // nothing left to do
            ++stats.evictMisses;
            DPRINTF(CCHI, "[%s] evict %#llx -> miss (already gone)\n",
                    name(), linePA);
            return;
        }
        // PA_EVT_BUSY / EVT_LIMIT_EXCEEDED / TOTAL_LIMIT_EXCEEDED: park
        // and retry from the tick hook; the L1 was already answered, so
        // this is purely internal bookkeeping
        ++stats.evictRetries;
        DPRINTF(CCHI, "[%s] evict %#llx DENIED (%s), queued\n",
                name(), linePA, denial->name);
        evictRetryQueue.push_back(linePA);
        return;
    }

    ++stats.evictions;
    DPRINTF(CCHI, "[%s] evict %#llx issued\n", name(), linePA);
    // Completion is not awaited: the node reaps the line record itself
}

// ---------------------------------------------------------------------------
// Per-cycle hook (called by the fabric after Instance::Tick)
// ---------------------------------------------------------------------------

void
CCHIL1Agent::tickHook()
{
    // Debug heartbeat: periodic state dump while anything is in flight,
    // so a silent wedge shows what was held (cheap, CCHI flag gated)
    if (DTRACE(CCHI) && (++heartbeat & 0xFFFF) == 0 &&
        (!pending.empty() || merge || !blockedPorts.empty() ||
         !completionQueue.empty() || !evictRetryQueue.empty() ||
         !localSnoops.empty())) {
        std::string pend;
        for (const auto &[line, e] : pending)
            pend += csprintf(" %#llx(a%d,r%u,%s)", line,
                             static_cast<int>(e.action), e.retries,
                             e.line ? "g" : "-");
        DPRINTF(CCHI, "[%s] heartbeat pend={%s }%s blocked=%d evictQ=%d "
                "compQ=%d\n", name(), pend,
                merge ? csprintf(" merge=%#llx(await=%d)", merge->linePA,
                                 merge->awaitingResponses).c_str() : "",
                blockedPorts.size(), evictRetryQueue.size(),
                completionQueue.size());
    }

    // 1. Completions posted by the FutureNow callbacks (bounded pass:
    //    entries whose data is not consistent yet are re-posted once)
    size_t remaining = completionQueue.size();
    while (remaining--) {
        const Addr linePA = completionQueue.front();
        completionQueue.pop_front();

        auto it = pending.find(linePA);
        if (it == pending.end()) {
            // Stale queue entry: Taurus futures can fire a second time
            // (at dealloc), so a line can be posted twice - once while
            // the entry is still queued for completion (duplicate push
            // in onGrantFired) and once after it drained (late grant).
            // Completions are idempotent hints; skip the duplicate.
            ++stats.lateGrants;
            continue;
        }

        if (processCompletion(it->second)) {
            DPRINTF(CCHI, "[%s] completed %#llx (a%d, r%u)\n", name(), linePA,
                    static_cast<int>(it->second.action), it->second.retries);
            pending.erase(it);
        } else {
            ++stats.completionRetries;
            ++it->second.retries;
            completionQueue.push_back(linePA);
            // Rate-limited visibility for persistently stuck entries
            if (it->second.retries == 100 || it->second.retries == 1000) {
                auto line = taurus()->GetCacheLine(linePA);
                const bool loadable = line && line->Load().has_value();
                warn("%s: completion for %#llx (action %d) stuck after %llu "
                     "retries; line %s, state %s, Load()=%d\n", name(), linePA,
                     static_cast<int>(it->second.action),
                     (unsigned long long)it->second.retries,
                     line ? "resident" : "ABSENT",
                     line ? line->GetState()->name : "n/a", int(loadable));
            }
        }
    }

    // 2. Fences: answer once the agent has fully drained
    if (!fenceQueue.empty() && pending.empty() && completionQueue.empty()) {
        for (auto &[pkt, portId] : fenceQueue)
            respondTo(pkt, portId, curTick() + clockPeriod());
        fenceQueue.clear();
    }

    // 3. Denied evictions (bounded pass, may re-enqueue)
    size_t evictions = evictRetryQueue.size();
    while (evictions--) {
        const Addr linePA = evictRetryQueue.front();
        evictRetryQueue.pop_front();
        issueEvict(linePA);
    }

    // 4. snoop_merge timeout safety valve (bounded staleness, see the
    //    plan's multicore risk note)
    if (merge && (uint64_t)(curCycle() - merge->startCycle)
                 > MERGE_TIMEOUT_CYCLES) {
        warn("%s: snoop merge for %#llx timed out after %llu cycles; "
             "releasing the held snoop (bounded staleness)\n",
             name(), merge->linePA,
             (unsigned long long)MERGE_TIMEOUT_CYCLES);
        ++stats.snoopMergeTimeouts;
        fabric->releaseSnoop(upstreamIndex);
        merge.reset();
    }

    // 5. Hosted L2 prefetch engine drain -> CCHI stash
    drainPrefetcher();

    // 5b. Local-snoop dispatches denied earlier (Taurus busy): bounded
    //     pass; entries that still deny stay parked for the next tick
    for (auto it = localSnoops.begin(); it != localSnoops.end();) {
        if (it->second.awaitingResponses != 0) {
            ++it;
            continue;
        }
        PacketPtr parked = it->second.pkt;
        const PortID parkedPort = it->second.portId;
        it = localSnoops.erase(it);
        if (!dispatchCchiRequest(parked, parkedPort)) {
            localSnoops.emplace(parked->getAddr() & ~Addr(63),
                                LocalSnoop{parked, parkedPort, 0,
                                           curCycle()});
            break;  // still busy: leave the rest for the next tick
        }
    }

    // 6. Backpressure release: TxnIDs freed during Instance::Tick (or a
    //    merge completed above) make progress possible again; spurious
    //    retries are harmless, the L1 simply re-attempts. Swap the set
    //    out BEFORE sending: sendRetryReq is synchronous, so the L1's
    //    immediate re-attempt can NACK and re-arm blockedPorts
    //    re-entrantly right here - clearing AFTER the sends would wipe
    //    that fresh state and deadlock the port.
    if (!blockedPorts.empty()) {
        std::set<PortID> toRetry;
        toRetry.swap(blockedPorts);
        for (const PortID portId : toRetry) {
            DPRINTF(CCHI, "[%s] sendRetryReq port %d\n", name(), portId);
            cpuSidePorts[portId]->sendRetryReq();
        }
    }
}

void
CCHIL1Agent::drainPrefetcher()
{
    if (!prefetcher)
        return;

    while (prefetcher->hasPendingPacket()) {
        PacketPtr pf = prefetcher->getPacket();
        if (!pf)
            break;

        // CCHI stash: prefetch the line into the endpoint-side L2
        // (StashShared/StashUnique); no allocation in Taurus and no gem5
        // response is expected, so the drained packet is dropped after
        // emission. Caveat (per plan): endpoint-side stash processing is
        // still pending in CHIron - Earth only decodes these opcodes.
        auto future = pf->needsWritable()
            ? taurus()->DoPrefetchStore(pf->getAddr())
            : taurus()->DoPrefetchLoad(pf->getAddr());

        if (future->IsRejected())
            ++stats.pfStashDropped;
        else
            ++stats.pfStashIssued;

        delete pf;
    }
}

// ---------------------------------------------------------------------------
// Snoop paths (home-originated only)
// ---------------------------------------------------------------------------

int
CCHIL1Agent::reflectSnoopToL1(Addr linePA, uint64_t snpOpcode)
{
    const bool invalidating =
        snpOpcode == static_cast<uint64_t>(CCHI::Opcodes::SNP::SnpToInvalid) ||
        snpOpcode == static_cast<uint64_t>(CCHI::Opcodes::SNP::SnpMakeInvalid);
    // Matches pkt->isInvalidate()/needsWritable() semantics in the L1's
    // handleSnoop (also feeds the ISA's LL/SC reservation clearing)
    const MemCmd cmd = invalidating ? MemCmd::ReadExReq
                                    : MemCmd::ReadSharedReq;

    int willRespond = 0;

    for (auto &port : cpuSidePorts) {
        // Only snoop coherent masters (L1I/L1D); MMU walker ports are not
        // snooping, mirroring the coherent-xbar behavior
        if (!port->isSnooping())
            continue;

        // The one place the bridge creates a Request: a home-originated
        // snoop has no gem5 request of its own (64B, physical)
        RequestPtr req = std::make_shared<Request>(linePA, 64,
                                                   Request::PHYSICAL,
                                                   Request::funcRequestorId);
        PacketPtr pkt = new Packet(req, cmd);
        pkt->allocate();

        // Unrefusable by protocol; cacheResponding() is set synchronously
        // if this L1 will supply dirty data (including the MSHR-deferred
        // will-respond case)
        port->sendTimingSnoopReq(pkt);
        if (pkt->cacheResponding())
            ++willRespond;

        DPRINTF(CCHI, "[%s] reflected snoop %s %#llx -> %s (responds: %d)\n",
                name(), invalidating ? "inval" : "shared", linePA,
                port->name(), pkt->cacheResponding());

        // The L1 only borrows the snoop packet synchronously (a deferred
        // MSHR snoop is replayed from an internal copy, see mshr.cc), and
        // any snoop response is a fresh packet owned and deleted by us
        delete pkt;

        ++stats.snoopReflected;
    }

    return willRespond;
}

int
CCHIL1Agent::reflectLocalSnoop(PacketPtr pkt, PortID portId)
{
    const Addr linePA = pkt->getAddr() & ~Addr(63);
    // Write-class (ownership) requests invalidate sibling copies, reads
    // just ask for the freshest data - mirroring the home's
    // SnpToInvalid/SnpToShared and the L1's handleSnoop semantics
    const MemCmd cmd = pkt->needsWritable() ? MemCmd::ReadExReq
                                            : MemCmd::ReadSharedReq;

    int willRespond = 0;

    for (size_t i = 0; i < cpuSidePorts.size(); ++i) {
        auto &port = cpuSidePorts[i];
        if (static_cast<PortID>(i) == portId || !port->isSnooping())
            continue;

        RequestPtr req = std::make_shared<Request>(linePA, 64,
                                                   Request::PHYSICAL,
                                                   Request::funcRequestorId);
        PacketPtr snp = new Packet(req, cmd);
        snp->allocate();

        port->sendTimingSnoopReq(snp);
        if (snp->cacheResponding())
            ++willRespond;

        DPRINTF(CCHI, "[%s] local snoop %s %#llx -> %s (responds: %d)\n",
                name(), pkt->needsWritable() ? "inval" : "shared", linePA,
                port->name(), snp->cacheResponding());

        // Borrowed synchronously by the L1; any response is a fresh
        // packet owned and deleted by us (same contract as the home flow)
        delete snp;
    }

    return willRespond;
}

void
CCHIL1Agent::mergeSnoopData(PacketPtr pkt, Addr linePA)
{
    auto line = taurus()->GetCacheLine(linePA);
    if (!line) {
        DPRINTF(CCHI, "[%s] snoop data for %#llx dropped (line gone)\n",
                name(), linePA);
        return;
    }

    std::array<uint64_t, 8> buf;
    std::memcpy(buf.data(), pkt->getConstPtr<uint8_t>(), 64);
    if (line->Store(std::span<const uint64_t, 8>(buf))) {
        ++stats.snoopMerged;
    } else {
        // The line should be Unique here (an L1-dirty line implies node
        // ownership); a rejected store means the content already matches
        // (e.g. an O-state repeat snoop) - bounded staleness, counted
        ++stats.snoopMergeStoreFailed;
        warn("%s: snoop merge store rejected for %#llx\n", name(), linePA);
    }
}

void
CCHIL1Agent::processHomeSnoop(const FlitSNP &flit)
{
    // SNP Addr carries PA >> 3 (bits [2:0] are implicit zeros)
    const Addr linePA = uint64_t(flit.Addr) << 3;

    panic_if(merge.has_value(),
             "%s: overlapping snoop merges (%#llx in flight, %#llx new)\n",
             name(), merge->linePA, linePA);

    const int willRespond =
        reflectSnoopToL1(linePA, static_cast<uint64_t>(flit.Opcode));

    if (willRespond == 0) {
        // Nothing to merge: release the held flit right away (the fabric
        // gate can then push it into Taurus in the same pump cycle)
        fabric->releaseSnoop(upstreamIndex);
    } else if (pending.count(linePA) && !pending.at(linePA).grantedHit) {
        // Cross-transaction case: our own bridge transaction for this
        // line is in flight at the home BEHIND this snoop's transaction
        // (per-line serialization at the home), and the L1 deferred the
        // snoop into the matching MSHR - its response therefore waits on
        // OUR transaction, which waits on THIS snoop. Holding the gate
        // would deadlock (the timeout valve would then trade the deadlock
        // for silent staleness). The Taurus copy is as fresh as the
        // L1's here: dirty L1 data only ever exists after a completed
        // bridge transaction, and anything earlier was merged into (or
        // written back through) the node already. So let the home be
        // answered from the Taurus line right away and merge the L1's
        // response when it eventually arrives (best effort; identical
        // content in the cases above).
        ++stats.snoopLateMerges;
        lateMergeLines.insert(linePA);
        fabric->releaseSnoop(upstreamIndex);
    } else {
        // No pending entry, or the pending entry is a LOCAL hit
        // (grantedHit: IsNow at issue, so nothing is in flight at the
        // home). The freshness premise above is false for a local hit:
        // its dirty data lives only in the L1 until the completion merges
        // it into the node line, so an immediate answer would go out
        // clean and silently lose it (observed with the XSCache L2, which
        // back-invalidates in the same cycle as a store grant: the answer
        // went out SnpResp_I and the store's data was lost). A hit
        // completes locally in hit_latency without home involvement, so
        // holding the gate here cannot deadlock.
        merge = MergeState{linePA, willRespond, curCycle()};
    }
}

void
CCHIL1Agent::handleSnoopFromHome(const FlitSNP &flit)
{
    // SNP Addr carries PA >> 3 (bits [2:0] are implicit zeros)
    const Addr linePA = uint64_t(flit.Addr) << 3;

    if (localSnoops.count(linePA)) {
        // A local snoop for this line is in flight: the L1s owe their
        // responses to it first (FIFO response accounting at
        // recvTimingSnoopResp). Hold the gate and run the home snoop
        // once the local one completes (the local snoop finishes within
        // a few L1 snoop latencies, never blocking the home for long).
        panic_if(deferredHomeSnoop.has_value(),
                 "%s: overlapping deferred home snoops (%#llx)\n",
                 name(), linePA);
        ++stats.homeSnoopDeferred;
        DPRINTF(CCHI, "[%s] deferring home snoop for %#llx (local snoop "
                "in flight)\n", name(), linePA);
        deferredHomeSnoop = flit;
        return;
    }

    processHomeSnoop(flit);
}

void
CCHIL1Agent::recvTimingSnoopResp(PacketPtr pkt,
                                 [[maybe_unused]] PortID portId)
{
    const Addr linePA = pkt->getAddr() & ~Addr(63);

    auto lsIt = localSnoops.find(linePA);
    if (lsIt != localSnoops.end()) {
        // Response to a local (intra-node) snoop: merge any dirty sibling
        // data into the node line before the parked request issues
        if (pkt->isRead() && pkt->hasData()) {
            ++stats.localSnoopMerged;
            mergeSnoopData(pkt, linePA);
        }

        panic_if(lsIt->second.awaitingResponses <= 0,
                 "%s: unexpected local snoop response for %#llx\n",
                 name(), linePA);
        if (--lsIt->second.awaitingResponses == 0) {
            // All siblings answered: the node line is now the freshest
            // local copy; issue the parked request. A denial (Taurus
            // busy) re-parks it for the tick hook's dispatch pass.
            PacketPtr parked = lsIt->second.pkt;
            const PortID parkedPort = lsIt->second.portId;
            localSnoops.erase(lsIt);
            DPRINTF(CCHI, "[%s] local snoop for %#llx complete, "
                    "dispatching\n", name(), linePA);
            if (!dispatchCchiRequest(parked, parkedPort)) {
                localSnoops.emplace(
                    linePA, LocalSnoop{parked, parkedPort, 0, curCycle()});
            }

            // A home snoop deferred behind this local one can now run
            if (deferredHomeSnoop) {
                const FlitSNP flit = *deferredHomeSnoop;
                deferredHomeSnoop.reset();
                processHomeSnoop(flit);
            }
        }
    } else if (snoopMerge && lateMergeLines.count(linePA)) {
        // Late response to a cross-transaction snoop (released early in
        // handleSnoopFromHome): merge the L1's data into the Taurus line
        // best-effort; by the invariant above the content matches, so a
        // rejected/absent store is benign (counted, never fatal)
        lateMergeLines.erase(linePA);
        if (pkt->isRead() && pkt->hasData()) {
            auto line = taurus()->GetCacheLine(linePA);
            if (line) {
                std::array<uint64_t, 8> buf;
                std::memcpy(buf.data(), pkt->getConstPtr<uint8_t>(), 64);
                if (line->Store(std::span<const uint64_t, 8>(buf)))
                    ++stats.snoopLateMerged;
                else
                    ++stats.snoopLateMergeDropped;
            } else {
                ++stats.snoopLateMergeDropped;
            }
        }
    } else if (snoopMerge && merge && merge->linePA == linePA) {
        // Merge any dirty L1 data into the Taurus line BEFORE the home's
        // snoop is answered (the fabric gate releases it when the last
        // expected response arrives). The line can legitimately be gone
        // here: an L1 writeback issued BEFORE the merge started already
        // stored this data into the node line (handleWriteback stores,
        // then DoEvicts), and the eviction's completion reaped the line
        // while the L1's snoop response was still in flight - the EVT
        // carried the same data home. The merge hazard rule blocks new
        // L1 requests for the line for the merge's duration, so the
        // response cannot be fresher: drop it, counted.
        if (pkt->isRead() && pkt->hasData()) {
            if (taurus()->GetCacheLine(linePA)) {
                mergeSnoopData(pkt, linePA);
            } else {
                ++stats.snoopMergeLineGone;
                DPRINTF(CCHI, "[%s] merge response for reaped line %#llx "
                        "dropped (data already at the home)\n",
                        name(), linePA);
            }
        }

        panic_if(merge->awaitingResponses <= 0,
                 "%s: unexpected snoop response for %#llx\n", name(), linePA);
        if (--merge->awaitingResponses == 0) {
            fabric->releaseSnoop(upstreamIndex);
            merge.reset();
        }
    } else {
        // snoop_merge=off (or stale response): the L1's data-supplying
        // snoop response is sunk here - never merged, never awaited
        ++stats.snoopRespDropped;
    }

    // The snoop response packet was created by the L1 for us; we own it
    delete pkt;
}

// ---------------------------------------------------------------------------
// Atomic / functional paths (boot, fast-forward, debugger)
// ---------------------------------------------------------------------------

Tick
CCHIL1Agent::recvAtomic(PacketPtr pkt, [[maybe_unused]] PortID portId)
{
    const Addr addr = pkt->getAddr();

    // Phase 1 assumption: atomic traffic (boot, fast-forward) does not
    // overlap in-flight CCHI transactions on the same lines, so serving
    // the resident Taurus line here cannot race the endpoint
    if (!pkt->req->isUncacheable() && fabric->isCacheable(addr)) {
        auto line = taurus()->GetCacheLine(addr);
        if (line) {
            if (pkt->isRead()) {
                if (fillFromLine(pkt, *line)) {
                    pkt->makeAtomicResponse();
                    return 0;
                }
                // Line not readable: fall through to the bypass read
            } else if (pkt->isWrite()) {
                // Keep the resident line in sync with the writeback to
                // memory below (best effort through the checked accessor)
                storeToLine(pkt, *line);
            }
        }
    }

    ++stats.atomicBypasses;
    return memSidePort.sendAtomic(pkt);
}

void
CCHIL1Agent::recvFunctional(PacketPtr pkt, [[maybe_unused]] PortID portId)
{
    // Same assumptions as recvAtomic; functional writes also update a
    // resident Taurus line so later checked loads stay consistent
    if (!pkt->req->isUncacheable() && fabric->isCacheable(pkt->getAddr()) &&
        pkt->isWrite()) {
        auto line = taurus()->GetCacheLine(pkt->getAddr());
        if (line)
            storeToLine(pkt, *line);
    }

    ++stats.functionalBypasses;
    memSidePort.sendFunctional(pkt);
}

void
CCHIL1Agent::recvBypassResp(PacketPtr pkt)
{
    auto *state = dynamic_cast<BypassSenderState *>(pkt->popSenderState());
    panic_if(!state, "%s: bypass response without sender state\n", name());

    const PortID portId = state->portId;
    delete state;

    const Tick receiveDelay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;
    cpuSidePorts[portId]->schedTimingResp(pkt, curTick() + receiveDelay);
}

AddrRangeList
CCHIL1Agent::getAddrRanges() const
{
    return fabric->getAddrRanges();
}

// ---------------------------------------------------------------------------
// Port plumbing
// ---------------------------------------------------------------------------

bool
CCHIL1Agent::AgentResponsePort::recvTimingReq(PacketPtr pkt)
{
    return parent.recvTimingReq(pkt, id);
}

bool
CCHIL1Agent::AgentResponsePort::recvTimingSnoopResp(PacketPtr pkt)
{
    parent.recvTimingSnoopResp(pkt, id);
    return true;
}

Tick
CCHIL1Agent::AgentResponsePort::recvAtomic(PacketPtr pkt)
{
    return parent.recvAtomic(pkt, id);
}

void
CCHIL1Agent::AgentResponsePort::recvFunctional(PacketPtr pkt)
{
    parent.recvFunctional(pkt, id);
}

AddrRangeList
CCHIL1Agent::AgentResponsePort::getAddrRanges() const
{
    return parent.getAddrRanges();
}

CCHIL1Agent::BypassRequestPort::BypassRequestPort(const std::string &_name,
                                                  CCHIL1Agent &_parent)
    : QueuedRequestPort(_name, &_parent, _parent.reqQueue,
                        _parent.snoopRespQueue),
      parent(_parent)
{
}

bool
CCHIL1Agent::BypassRequestPort::recvTimingResp(PacketPtr pkt)
{
    parent.recvBypassResp(pkt);
    return true;
}

// ---------------------------------------------------------------------------
// Taurus-backed CacheAccessor for the hosted L2 prefetch engine
// ---------------------------------------------------------------------------

bool
CCHIL1Agent::TaurusCacheAccessor::inCache(Addr addr, bool) const
{
    auto *node = agent->taurus();
    return node->IsValid(addr) &&
           node->GetState(addr) != TaurusState::Invalid;
}

bool
CCHIL1Agent::TaurusCacheAccessor::inMissQueue(Addr addr, bool) const
{
    return agent->pending.count(addr & ~Addr(63)) != 0;
}

const uint8_t *
CCHIL1Agent::TaurusCacheAccessor::findBlock(Addr addr, bool) const
{
    auto *node = agent->taurus();
    if (!node->IsValid(addr) || node->GetState(addr) == TaurusState::Invalid)
        return nullptr;

    auto line = node->GetCacheLine(addr);
    if (!line)
        return nullptr;

    // Raw unchecked view (no Invalid/fill guards): consumers use it
    // transiently, synchronously, and the line cannot be reaped mid-call
    // (the simulation is single-threaded)
    const auto data = line->GetData();
    return reinterpret_cast<const uint8_t *>(data.data());
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

CCHIL1Agent::CCHIL1AgentStats::CCHIL1AgentStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(reqHits, statistics::units::Count::get(),
               "Requests granted immediately (Taurus hits)"),
      ADD_STAT(reqMisses, statistics::units::Count::get(),
               "Requests going to the CCHI fabric"),
      ADD_STAT(reads, statistics::units::Count::get(),
               "ReadReq/LoadLockedReq mapped to DoLoad"),
      ADD_STAT(readExes, statistics::units::Count::get(),
               "ReadExReq mapped to DoStore (fill + ownership)"),
      ADD_STAT(upgrades, statistics::units::Count::get(),
               "Upgrade/SCUpgrade/StoreCond mapped to DoStore (S->U)"),
      ADD_STAT(writes, statistics::units::Count::get(),
               "WriteReq/WriteLineReq mapped to DoStore/DoStoreLine"),
      ADD_STAT(hardPrefetches, statistics::units::Count::get(),
               "L1 prefetch requests filled via DoLoad"),
      ADD_STAT(rmwReads, statistics::units::Count::get(),
               "LockedRMWReadReq mapped to DoStore + pin"),
      ADD_STAT(rmwWrites, statistics::units::Count::get(),
               "LockedRMWWriteReq (store + unpin)"),
      ADD_STAT(writebacks, statistics::units::Count::get(),
               "Dirty writebacks committed to Taurus + async DoEvict"),
      ADD_STAT(cleanEvicts, statistics::units::Count::get(),
               "Clean evictions answered + async DoEvict"),
      ADD_STAT(writeCleans, statistics::units::Count::get(),
               "WriteClean stores committed to Taurus (no eviction)"),
      ADD_STAT(fences, statistics::units::Count::get(),
               "Fence/sync requests drained and answered"),
      ADD_STAT(cmos, statistics::units::Count::get(),
               "Cache-maintenance requests mapped to DoCBO*"),
      ADD_STAT(deniedReqLimit, statistics::units::Count::get(),
               "Taurus REQ_LIMIT_EXCEEDED backpressure events"),
      ADD_STAT(deniedTotalLimit, statistics::units::Count::get(),
               "Taurus TOTAL_LIMIT_EXCEEDED (TxnID space) backpressure"),
      ADD_STAT(deniedPABusy, statistics::units::Count::get(),
               "Taurus PA_REQ_BUSY denials (same-line REQ in flight)"),
      ADD_STAT(deniedCMOLimit, statistics::units::Count::get(),
               "Taurus CMO_LIMIT_EXCEEDED backpressure events"),
      ADD_STAT(blockedSameLine, statistics::units::Count::get(),
               "Requests backpressured by same-line pending bridge "
               "transactions (retried by the L1 after drain)"),
      ADD_STAT(storeEscalations, statistics::units::Count::get(),
               "Ownership re-acquisitions after bounded store retries"),
      ADD_STAT(lateGrants, statistics::units::Count::get(),
               "Grant/CMO completions arriving for already-drained lines"),
      ADD_STAT(completionRetries, statistics::units::Count::get(),
               "Completions retried because the line was not readable yet"),
      ADD_STAT(evictions, statistics::units::Count::get(),
               "DoEvict issued towards the fabric"),
      ADD_STAT(evictRetries, statistics::units::Count::get(),
               "Denied DoEvict calls parked for retry"),
      ADD_STAT(evictMisses, statistics::units::Count::get(),
               "DoEvict on already-gone lines (benign)"),
      ADD_STAT(evictForRefetch, statistics::units::Count::get(),
               "Shared lines evicted so the store refetches with data"),
      ADD_STAT(evictForRefetchDenied, statistics::units::Count::get(),
               "Evict-for-refetch denials (store retried later)"),
      ADD_STAT(evictCancelled, statistics::units::Count::get(),
               "Stale queued evictions cancelled on re-acquisition"),
      ADD_STAT(writebacksDropped, statistics::units::Count::get(),
               "Writebacks dropped for non-resident lines (data already "
               "at the home)"),
      ADD_STAT(bypassReqs, statistics::units::Count::get(),
               "Requests bypassed to the gem5 memory system (no CCHI)"),
      ADD_STAT(atomicBypasses, statistics::units::Count::get(),
               "Atomic accesses forwarded to the bypass port"),
      ADD_STAT(functionalBypasses, statistics::units::Count::get(),
               "Functional accesses forwarded to the bypass port"),
      ADD_STAT(snoopReflected, statistics::units::Count::get(),
               "Home snoops reflected into the L1s"),
      ADD_STAT(snoopRespDropped, statistics::units::Count::get(),
               "L1 snoop responses sunk (snoop_merge=off)"),
      ADD_STAT(snoopMerged, statistics::units::Count::get(),
               "L1 dirty snoop data merged into the Taurus line"),
      ADD_STAT(snoopMergeLineGone, statistics::units::Count::get(),
               "Merge responses for a line reaped by an in-flight "
               "eviction (data already at the home)"),
      ADD_STAT(storeFlightBackoff, statistics::units::Count::get(),
               "Store issue backed off (snoop/eviction in flight on the "
               "line)"),
      ADD_STAT(snoopMergeStoreFailed, statistics::units::Count::get(),
               "Snoop merge stores rejected by the checked accessor"),
      ADD_STAT(snoopMergeTimeouts, statistics::units::Count::get(),
               "Snoop merges force-released by the timeout safety valve"),
      ADD_STAT(snoopLateMerges, statistics::units::Count::get(),
               "Cross-transaction snoops released early (late-merge path)"),
      ADD_STAT(snoopLateMerged, statistics::units::Count::get(),
               "Late snoop responses merged into the Taurus line"),
      ADD_STAT(snoopLateMergeDropped, statistics::units::Count::get(),
               "Late snoop response data dropped (benign, line moved on)"),
      ADD_STAT(localSnoopIssued, statistics::units::Count::get(),
               "Intra-node snoops issued to sibling L1s"),
      ADD_STAT(localSnoopMerged, statistics::units::Count::get(),
               "Data-carrying responses to intra-node snoops"),
      ADD_STAT(homeSnoopDeferred, statistics::units::Count::get(),
               "Home snoops deferred behind an in-flight local snoop"),
      ADD_STAT(mergeBlockedReqs, statistics::units::Count::get(),
               "L1 requests blocked by an in-flight snoop merge"),
      ADD_STAT(pfStashIssued, statistics::units::Count::get(),
               "Hosted-prefetcher emissions stashed via DoPrefetch*"),
      ADD_STAT(pfStashDropped, statistics::units::Count::get(),
               "Prefetch stash emissions dropped (queue limits)"),
      ADD_STAT(pfStoreTrains, statistics::units::Count::get(),
               "StorePFTrain hints forwarded to the hosted prefetcher")
{
}

} // namespace gem5
