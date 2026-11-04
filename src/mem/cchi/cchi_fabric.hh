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

/**
 * @file
 * CCHIFabric: one-per-system SimObject that embeds CHIron's
 * Cohestra::Instance. The instance owns the per-core Taurus upstream
 * nodes (one per CCHIL1Agent) and a single downstream CCHIInterface
 * endpoint. The fabric's per-cycle event is Instance::Tick (CHIron's own
 * flit pump) followed by each agent's tick hook.
 *
 * The downstream endpoint sits behind CHIron's CCHIInterface abstraction:
 * Phase 1 uses the vendored Earth behavioral home (src/mem/cchi/earth),
 * whose MemoryBackend is implemented here on top of the fabric's
 * mem_side port, so gem5's memory system remains the only data store.
 *
 * A Cohestra::CacheLineDataMonitor is attached to the instance as the
 * built-in end-to-end data checker (per-line scoreboard); a mismatch
 * marks the instance FAILED and is raised as a gem5 fatal at the next
 * fabric tick when monitor_fail_on_mismatch is set.
 */

#ifndef __MEM_CCHI_CCHI_FABRIC_HH__
#define __MEM_CCHI_CCHI_FABRIC_HH__

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/addr_range.hh"
#include "base/statistics.hh"
#include "base/types.hh"
#include "mem/packet_queue.hh"
#include "mem/qport.hh"
#include "params/CCHIFabric.hh"
#include "sim/clocked_object.hh"

// CHIron Cohestra harness (read-only; resolved via -I$(CHIRON_DIR)/cchi).
// NOTE: only spdlog-free CHIron headers may be included from this header:
// the generated SimObject pybind TU includes gem5 headers (with the
// warn/panic logging macros) before this one, and those macros collide
// with spdlog's member functions. Cohestra::Instance and
// Cohestra::CacheLineDataMonitor (whose headers pull in spdlog) are
// therefore forward-declared and only used from the .cc file.
#include "cohestra/cohestra_config.hpp"
#include "cohestra/cohestra_interface.hpp"
#include "icn/taurus/cchi_taurus_component.hpp"

// Vendored Earth endpoint (MemoryBackend hook, plain setters, no spdlog)
#include "mem/cchi/earth/earth_interface.hpp"

namespace Cohestra
{
    class Instance;
    class CacheLineDataMonitor;
    class CCHIFlitLogger;
}

namespace gem5
{

class CCHIL1Agent;

class CCHIFabric : public ClockedObject
{
  public:
    using TaurusNode =
        CCHI::Taurus::UpstreamNode<Cohestra::CommonFlitConfigurationType1>;
    using FlitSNP = CCHI::Flits::SNP<Cohestra::CommonFlitConfigurationType1>;
    using AcceptedSNPEvent =
        CCHI::Taurus::UpstreamNodeXactAcceptedSNPEvent<
            Cohestra::CommonFlitConfigurationType1>;

    CCHIFabric(const CCHIFabricParams &params);
    ~CCHIFabric(); // out-of-line: unique_ptrs to forward-declared CHIron types

    void init() override;
    void startup() override;

    Port &getPort(const std::string &if_name,
                  PortID idx = InvalidPortID) override;

    /** Agent registration, called from CCHIL1Agent::init(). */
    void addUpstreamNode(uint32_t nodeId, CCHIL1Agent *agent);

    /**
     * Taurus node lookup for agents. Builds the embedded instance lazily
     * on first use (guaranteed to run after all init() registrations).
     */
    TaurusNode *getUpstreamNode(uint32_t nodeId);

    /** True for addresses served through CCHI (the endpoint window). */
    bool isCacheable(Addr addr) const
    { return addr >= memoryStart && addr < memoryEnd; }

    /** Address window of the CCHI-managed memory. */
    AddrRangeList getAddrRanges() const { return addrRanges; }

    /**
     * Release a home snoop held by the SNP gate of upstream 'index'
     * (snoop_merge flow): the next Instance::Tick is allowed to push the
     * held flit into the Taurus node, whose answer then carries the data
     * merged from the L1.
     */
    void releaseSnoop(size_t index);

    /** Atomic access to gem5 memory on behalf of the endpoint backend. */
    Tick sendAtomicOnMemSide(PacketPtr pkt) { return memSidePort.sendAtomic(pkt); }

  protected: // Port interface
    class MemSideRequestPort : public QueuedRequestPort
    {
      public:
        MemSideRequestPort(const std::string &_name, CCHIFabric &_parent);

      protected:
        bool recvTimingResp(PacketPtr pkt) override;

      private:
        CCHIFabric &parent;
    };

    MemSideRequestPort memSidePort;
    ReqPacketQueue reqQueue;
    SnoopRespPacketQueue snoopRespQueue;

  protected:
    /**
     * gem5-memory-backed implementation of the vendored Earth's
     * MemoryBackend: whole-line reads/writes through mem_side. Phase 1
     * uses zero-time atomic accesses; the endpoint's own
     * latencyRSP/latencyDAT knobs carry the modeled latency.
     * TODO(cchi): serve endpoint memory traffic with timing-mode
     * transactions so DRAM latency is modeled end-to-end.
     */
    class Gem5MemoryBackend : public Cohestra::MemoryBackend
    {
      public:
        explicit Gem5MemoryBackend(CCHIFabric &_fabric) : fabric(_fabric) { }

        void readLine(uint64_t lineAddr,
                      std::array<uint64_t, 8> &data) override;
        void writeLine(uint64_t lineAddr,
                       const std::array<uint64_t, 8> &data) override;

      private:
        CCHIFabric &fabric;
    };

    /**
     * gem5-Params-backed StringLoaderBase: answers CHIron's
     * (section, key) configuration queries from values filled in by the
     * fabric (instance upstream config + monitor knobs). Unset keys fall
     * back to the CHIron defaults.
     */
    class Gem5ConfigLoader : public Cohestra::StringLoaderBase
    {
      public:
        void set(const std::string &section, const std::string &key,
                 const std::string &value)
        { values[std::make_pair(section, key)] = value; }

        std::optional<std::string> GetRawString(
            Cohestra::ConfigurationEntryBase entry,
            const std::string &defaultSection = "global")
            const noexcept override
        {
            const std::string section =
                entry->section ? entry->section : defaultSection;
            auto it = values.find(std::make_pair(section, entry->key));
            if (it == values.end())
                return std::nullopt;
            return it->second;
        }

      private:
        std::map<std::pair<std::string, std::string>, std::string> values;
    };

    /**
     * Per-upstream snoop gate state (snoop_merge=on agents). While a
     * merge is in flight the port's SNP channel is withheld from the
     * Instance pump (head-of-line, bounded); once the agent merged the
     * L1 data the held flit is offered again.
     */
    struct SnoopGate {
        bool mergeOn = false;     // agent runs with snoop_merge=on
        bool intercepted = false; // a flit was taken out of the endpoint
        bool ready = false;       // merge done, offer the held flit
        FlitSNP heldFlit = {};
    };

    /**
     * CCHIInterface decorator between the embedded Instance and the real
     * endpoint: forwards every channel verbatim except Type-1 SNP, which
     * is routed through the fabric's per-port gates so that
     * snoop_merge=on agents can merge L1 data before Taurus answers.
     */
    class SnoopGateCCHIInterface : public Cohestra::CCHIInterface
    {
      public:
        SnoopGateCCHIInterface(
            std::shared_ptr<Cohestra::CCHIInterface> _real,
            CCHIFabric &_fabric) noexcept
            : real(std::move(_real)), fabric(_fabric) { }

        void TickPreHandshake(uint64_t time) noexcept override
        { real->TickPreHandshake(time); }
        void TickPostHandshake(uint64_t time) noexcept override
        { real->TickPostHandshake(time); }

        // Cap the reported port set at the configured upstream count:
        // RTL endpoints may expose more Type-1 ports than this system has
        // agents (e.g. a 4-port Venus with a single-core config), and
        // Instance::Initialize rejects a downstream that is larger than
        // the configured upstream set. Earth reports exactly the
        // configured count already, so this is a no-op for it.
        size_t GetType1Count() const noexcept override
        { return fabric.upstreamNodeCount; }
        std::set<size_t> GetType1Indices() const noexcept override
        {
            std::set<size_t> capped;
            for (size_t i : real->GetType1Indices())
                if (i < fabric.upstreamNodeCount)
                    capped.insert(i);
            return capped;
        }
        size_t GetType1MaxIndex() const noexcept override
        { return fabric.upstreamNodeCount - 1; }

        bool HasType1SNP(size_t index) const noexcept override;
        std::optional<FlitSNP> PeekType1SNP(size_t index)
            const noexcept override;
        std::optional<FlitSNP> PopType1SNP(size_t index) noexcept override;

        bool HasType1DnRSP(size_t index) const noexcept override
        { return real->HasType1DnRSP(index); }
        std::optional<CCHI::Flits::DnRSP<Cohestra::CommonFlitConfigurationType1>>
        PeekType1DnRSP(size_t index) const noexcept override
        { return real->PeekType1DnRSP(index); }
        std::optional<CCHI::Flits::DnRSP<Cohestra::CommonFlitConfigurationType1>>
        PopType1DnRSP(size_t index) noexcept override
        { return real->PopType1DnRSP(index); }

        bool HasType1DnDAT(size_t index) const noexcept override
        { return real->HasType1DnDAT(index); }
        std::optional<CCHI::Flits::DnDAT<Cohestra::CommonFlitConfigurationType1>>
        PeekType1DnDAT(size_t index) const noexcept override
        { return real->PeekType1DnDAT(index); }
        std::optional<CCHI::Flits::DnDAT<Cohestra::CommonFlitConfigurationType1>>
        PopType1DnDAT(size_t index) noexcept override
        { return real->PopType1DnDAT(index); }

        bool PushType1EVT(size_t index,
            const CCHI::Flits::EVT<Cohestra::CommonFlitConfigurationType1> &flit)
            noexcept override
        { return real->PushType1EVT(index, flit); }
        bool PushType1REQ(size_t index,
            const CCHI::Flits::REQ<Cohestra::CommonFlitConfigurationType1> &flit)
            noexcept override
        { return real->PushType1REQ(index, flit); }
        bool PushType1UpRSP(size_t index,
            const CCHI::Flits::UpRSP<Cohestra::CommonFlitConfigurationType1> &flit)
            noexcept override
        { return real->PushType1UpRSP(index, flit); }
        bool PushType1UpDAT(size_t index,
            const CCHI::Flits::UpDAT<Cohestra::CommonFlitConfigurationType1> &flit)
            noexcept override
        { return real->PushType1UpDAT(index, flit); }

      private:
        std::shared_ptr<Cohestra::CCHIInterface> real;
        CCHIFabric &fabric;
    };

    /** Build the endpoint, instance, monitor and event listeners. */
    void ensureInstance();

    /** Per-cycle event: pump the CHIron stack, then the agent hooks. */
    void tick();

    /** SNP-gate front side, called from the gate interface. */
    std::optional<FlitSNP> gatePeekSNP(size_t index);
    std::optional<FlitSNP> gatePopSNP(size_t index);

    /** OnAcceptedSNP listener: default (merge-off) L1 reflection hook. */
    void onAcceptedSnoop(CCHIL1Agent *agent, AcceptedSNPEvent &ev);

  protected:
    // Params
    const size_t upstreamNodeCount;
    const std::string downstream;
    const Addr memoryStart;
    const Addr memoryEnd;
    const uint32_t earthLatencyRSP;
    const uint32_t earthLatencyDAT;
    const bool monitorEnable;
    const bool monitorFailOnMismatch;
    const bool monitorTrace;
    const bool flitTrace;

    AddrRangeList addrRanges;

    // Agent registrations by CCHI node ID (filled during init()), and the
    // deterministic node-ID-sorted index assignment used by the instance
    std::map<uint32_t, CCHIL1Agent *> agents;
    std::vector<CCHIL1Agent *> agentsByIndex;

    // CHIron stack (built lazily by ensureInstance())
    bool instanceBuilt = false;
    std::unique_ptr<Cohestra::Instance> instance;
    std::shared_ptr<Cohestra::EarthCCHIInterface> earthInterface;
    // The real downstream endpoint behind the gate decorator (Earth or RTL)
    std::shared_ptr<Cohestra::CCHIInterface> downstreamInterface;
    std::shared_ptr<SnoopGateCCHIInterface> gatedDownstream;
    std::shared_ptr<Gem5MemoryBackend> memoryBackend;
    std::unique_ptr<Cohestra::CacheLineDataMonitor> monitor;
    std::unique_ptr<Cohestra::CCHIFlitLogger> flitLogger;
    Gem5ConfigLoader configLoader;
    std::vector<SnoopGate> snoopGates;

#ifdef CCHI_RTL_ENABLED
    // Verilator backend (downstream="rtl", WITH_CCHI_RTL build only).
    // Pimpl: the verilated top and CHIron's pin bindings for it pull in
    // spdlog-tainted CHIron headers, which this header must not include
    // (see the note above); the full definition lives in cchi_fabric.cc
    struct RtlStack;
    std::unique_ptr<RtlStack> rtl;

    /** High-active reset sequence, then the per-tick clock drivers:
        low/high evals bracket the two-phase handshake ticks (see tick()). */
    void rtlReset();
    void rtlClockEvalLow();
    void rtlClockEvalHigh();
#endif

    EventFunctionWrapper tickEvent;
    uint64_t cycleCount = 0;
    uint64_t lastMonitorMismatchCount = 0;

  public:
    struct CCHIFabricStats : public statistics::Group
    {
        CCHIFabricStats(statistics::Group *parent);

        statistics::Scalar ticks;
        statistics::Scalar backendReads;
        statistics::Scalar backendWrites;
        statistics::Scalar snoopGateInterceptions;
        statistics::Scalar snoopGateReleases;
        statistics::Scalar snoopReflections;
        statistics::Scalar nullSnoopResponses;
        statistics::Scalar monitorMismatches;
    } stats;
};

} // namespace gem5

#endif // __MEM_CCHI_CCHI_FABRIC_HH__
