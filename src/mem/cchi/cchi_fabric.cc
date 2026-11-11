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

// spdlog-tainted CHIron harness headers: included FIRST in this TU,
// before any gem5 header defines the warn/panic logging macros (see the
// note in cchi_fabric.hh)
#include "cohestra/cohestra_instance.hpp"
#include "cohestra/cohestra_monitor.hpp"
#include "cohestra/cohestra_cchi_flit_logger.hpp"

#ifdef CCHI_RTL_ENABLED
// Verilator backend headers (likewise spdlog-tainted via cohestra_axi.hpp)
#include "cohestra/cohestra_v3/interface.hpp"
#include "cohestra/cohestra_v3/interface_axi.hpp"
#include "mem/cchi/cchi_axi_mem_bridge.hh"
#include "mem/cchi/cchi_rtl_top.hh"
#include "verilated_vcd_c.h"
#endif

#include "mem/cchi/cchi_fabric.hh"

#include <array>
#include <cstring>

#include "base/logging.hh"
#include "mem/cchi/cchi_l1_agent.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
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

#ifdef CCHI_RTL_ENABLED
// Verilator backend stack (pimpl; see the declaration in cchi_fabric.hh)
struct CCHIFabric::RtlStack {
    std::unique_ptr<CchiRtlModule> top;
    std::shared_ptr<Cohestra::V3CCHIInterface<CchiRtlModule>> interface;
    std::shared_ptr<Cohestra::V3AXISlaveInterface<CchiRtlModule>> axiIf;
    std::unique_ptr<CCHIAxiMemBridge> axiBridge;

    // Null-snoop-responder delay lines (see the tick hook): a response
    // must mature a few cycles so the home's slot has registered the
    // snoop as issued before any answer can arrive (a same-cycle answer
    // trips its unknown-snoop assertion)
    struct DelayedSnoopResp {
        uint64_t readyCycle;
        CCHI::Flits::UpRSP<Cohestra::CommonFlitConfigurationType1> rsp;
    };
    std::map<size_t, std::deque<DelayedSnoopResp>> nullSnoopQueue;

    // Optional FST waveform dump (CCHI_RTL_TRACE env var: path to write)
    std::unique_ptr<VerilatedVcdC> tfp;
    uint64_t traceTime = 0;
    // Deferred trace start (CCHI_RTL_TRACE_START env var, in gem5 ticks):
    // keeps long runs from producing multi-GB dumps before the region of
    // interest; the dump opens on the first fabric tick at/after this tick
    Tick traceStartTick = 0;
    std::string tracePendingPath;

    void traceDump()
    {
        if (tfp) {
            tfp->dump(traceTime++);
            // Abort paths never close the file; flush periodically
            if ((traceTime & 0x3FF) == 0)
                tfp->flush();
        }
    }
    void traceClose()
    {
        if (tfp) {
            tfp->close();
            tfp.reset();
        }
    }
};
#endif

CCHIFabric::CCHIFabric(const CCHIFabricParams &p)
    : ClockedObject(p),
      memSidePort(p.name + ".mem_side", *this),
      reqQueue(*this, memSidePort),
      snoopRespQueue(*this, memSidePort),
      upstreamNodeCount(p.upstream_node_count),
      downstream(p.downstream),
      memoryStart(p.memory_start),
      memoryEnd(p.memory_end),
      earthLatencyRSP(p.earth_latency_rsp),
      earthLatencyDAT(p.earth_latency_dat),
      monitorEnable(p.monitor_enable),
      monitorFailOnMismatch(p.monitor_fail_on_mismatch),
      monitorTrace(p.monitor_trace),
      flitTrace(p.flit_trace),
      tickEvent([this] { tick(); }, p.name + ".tick"),
      stats(this)
{
    fatal_if(memoryEnd <= memoryStart,
             "%s: memory_end (%#llx) must be greater than memory_start "
             "(%#llx)\n", name(), memoryEnd, memoryStart);
    fatal_if(upstreamNodeCount == 0,
             "%s: upstream_node_count must be at least 1\n", name());
#ifndef CCHI_RTL_ENABLED
    fatal_if(downstream == "rtl",
             "%s: downstream='rtl' requires a WITH_CCHI_RTL build "
             "(this binary has none)\n", name());
#endif
    fatal_if(downstream != "earth" && downstream != "rtl",
             "%s: unknown downstream endpoint '%s' (expected earth|rtl)\n",
             name(), downstream);
    addrRanges.emplace_back(memoryStart, memoryEnd - 1);
}

void
CCHIFabric::init()
{
    if (!memSidePort.isConnected())
        fatal("%s: mem_side port is not connected\n", name());
}

void
CCHIFabric::startup()
{
    ClockedObject::startup();

    // All CCHIL1Agent::init() registrations are complete by now.
    ensureInstance();

    schedule(tickEvent, curTick() + clockPeriod());
}

Port &
CCHIFabric::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "mem_side") {
        return memSidePort;
    }
    return ClockedObject::getPort(if_name, idx);
}

void
CCHIFabric::addUpstreamNode(uint32_t nodeId, CCHIL1Agent *agent)
{
    fatal_if(instanceBuilt,
             "%s: agent %s registered after the Cohestra instance was "
             "built\n", name(), agent->name());
    fatal_if(!agents.emplace(nodeId, agent).second,
             "%s: duplicate CCHI node ID %u (agents %s and %s)\n",
             name(), nodeId, agents[nodeId]->name(), agent->name());
}

CCHIFabric::~CCHIFabric() = default;

CCHIFabric::TaurusNode *
CCHIFabric::getUpstreamNode(uint32_t nodeId)
{
    ensureInstance();

    // The index assignment is the node-ID sort order built in
    // ensureInstance(); locate this node ID's index.
    size_t index = 0;
    for (const auto &[id, agent] : agents) {
        if (id == nodeId)
            return instance->GetType1Upstream(index);
        ++index;
    }
    return nullptr;
}

void
CCHIFabric::ensureInstance()
{
    if (instanceBuilt)
        return;

    fatal_if(agents.size() != upstreamNodeCount,
             "%s: %llu agent(s) registered but upstream_node_count=%llu\n",
             name(), (unsigned long long)agents.size(),
             (unsigned long long)upstreamNodeCount);

    // Deterministic assignment: upstream index i is the agent with the
    // i-th smallest CCHI node ID (std::map iteration order). Documented
    // simplification: node IDs are expected to be dense from 0.
    std::vector<uint32_t> nodeIds;
    for (const auto &[id, agent] : agents) {
        agentsByIndex.push_back(agent);
        nodeIds.push_back(id);
    }

    // Instance upstream config. The stock Cohestra::Instance applies one
    // uniform set of xaction limits to every node it constructs, so the
    // values are taken from the first agent; a mismatch across agents is
    // worth a warning since it cannot be represented.
    const CCHIL1Agent *first = agentsByIndex.front();
    for (const auto *agent : agentsByIndex)
        warn_if(agent->getXactionLimitREQ() != first->getXactionLimitREQ() ||
                agent->getXactionLimitEVT() != first->getXactionLimitEVT() ||
                agent->getXactionLimitSNP() != first->getXactionLimitSNP(),
                "%s: agents disagree on xaction limits; the Cohestra "
                "instance applies the first agent's values uniformly\n",
                name());

    std::string nodeIdList;
    for (size_t i = 0; i < nodeIds.size(); ++i) {
        if (i)
            nodeIdList += ",";
        nodeIdList += std::to_string(nodeIds[i]);
    }
    configLoader.set("root", "upstream.type1.nodeid", nodeIdList);
    configLoader.set("root", "upstream.type1.inflight.evt",
                     std::to_string(first->getXactionLimitEVT()));
    configLoader.set("root", "upstream.type1.inflight.snp",
                     std::to_string(first->getXactionLimitSNP()));
    configLoader.set("root", "upstream.type1.inflight.req",
                     std::to_string(first->getXactionLimitREQ()));
    // 'upstream.type1.inflight' (total TxnID space) keeps its CHIron
    // default (32); the default already covers the per-class limits above.

    // Monitor knobs
    configLoader.set("monitor", "enable", monitorEnable ? "1" : "0");
    configLoader.set("monitor", "fail.on.mismatch",
                     monitorFailOnMismatch ? "1" : "0");
    configLoader.set("monitor", "trace", monitorTrace ? "1" : "0");
    // Flit-level transaction logger knob (debug)
    configLoader.set("cchi", "verbose", flitTrace ? "1" : "0");

    // Downstream endpoint: either the vendored Earth behavioral home
    // (backed by gem5 memory through the MemoryBackend hook) or the
    // Verilator backend (V3CCHIInterface over the verilated top, its AXI
    // memory ports served by CCHIAxiMemBridge into gem5 memory)

    if (downstream == "earth") {
        earthInterface =
            std::make_shared<Cohestra::EarthCCHIInterface>(upstreamNodeCount);
        auto &model = earthInterface->GetModel();
        memoryBackend = std::make_shared<Gem5MemoryBackend>(*this);
        model.SetMemoryBackend(memoryBackend);
        model.SetMemoryWindow(memoryStart, memoryEnd);
        model.SetLatencyRSP(earthLatencyRSP);
        model.SetLatencyDAT(earthLatencyDAT);
        model.SetUpstreamNodeIDs(nodeIds);
        // Earth's verbose switches keep their (on) defaults: with
        // EARTH_HAVE_GEM5_DEBUG the verbose logs route to the Earth debug
        // flag and stay silent unless --debug-flags=Earth is given.
        downstreamInterface = earthInterface;
    } else {
#ifdef CCHI_RTL_ENABLED
        // Verilator backend: verilated top + CHIron's pin bindings + the
        // AXI memory bridge, then the reset sequence
        rtl = std::make_unique<RtlStack>();
        rtl->top = std::make_unique<CchiRtlModule>();
        rtl->interface =
            std::make_shared<Cohestra::V3CCHIInterface<CchiRtlModule>>(
                rtl->top.get());
        rtl->axiIf =
            std::make_shared<Cohestra::V3AXISlaveInterface<CchiRtlModule>>(
                rtl->top.get());
        // The AXI response FIFOs default to size 0 (Push never accepted);
        // size them like the cohestra_v3 harness does
        for (size_t i : rtl->axiIf->GetPortIndices()) {
            rtl->axiIf->SetPortBFIFOSize(i, 2);
            rtl->axiIf->SetPortRFIFOSize(i, 2);
        }
        rtl->axiBridge =
            std::make_unique<CCHIAxiMemBridge>(*this, rtl->axiIf);
        // The Type-1 upstream->downstream buffers default to size 0: a
        // combinational-bypass mode that DROPS flits whenever the RTL is
        // not ready at the next posedge (its rx FIFOs backpressure).
        // Size them so backpressure propagates to the pump instead
        for (size_t i : rtl->interface->GetType1Indices()) {
            rtl->interface->SetType1EVTBufferSize(i, 4);
            rtl->interface->SetType1REQBufferSize(i, 4);
            rtl->interface->SetType1UpRSPBufferSize(i, 4);
            rtl->interface->SetType1UpDATBufferSize(i, 4);
        }
        rtlReset();

        fatal_if(rtl->interface->GetType1Count() < upstreamNodeCount,
                 "%s: verilated top has %llu CCHI Type-1 port(s), fewer "
                 "than upstream_node_count %llu\n", name(),
                 (unsigned long long)rtl->interface->GetType1Count(),
                 (unsigned long long)upstreamNodeCount);
        inform("%s: Verilator downstream endpoint up (%s)\n", name(),
               CCHI_RTL_TOP_NAME);
        downstreamInterface = rtl->interface;
#else
        fatal("%s: downstream='rtl' without a WITH_CCHI_RTL build\n",
              name());
#endif
    }

    // SNP gate decorator between the instance and the endpoint
    gatedDownstream =
        std::make_shared<SnoopGateCCHIInterface>(downstreamInterface, *this);
    snoopGates.assign(upstreamNodeCount, SnoopGate{});
    for (size_t i = 0; i < agentsByIndex.size(); ++i)
        snoopGates[i].mergeOn = agentsByIndex[i]->getSnoopMerge();

    // The embedded CHIron instance: constructs the Taurus upstream nodes
    // from the loader-supplied config (node IDs + xaction limits)
    instance = std::make_unique<Cohestra::Instance>();
    fatal_if(!instance->LoadPreInitConfiguration(configLoader),
             "%s: failed to load Cohestra pre-init configuration\n", name());
    instance->Initialize(gatedDownstream, nullptr /* no stimulators */);
    fatal_if(!instance->IsAlive(),
             "%s: Cohestra instance failed to initialize\n", name());

    // Data-integrity monitor, per the cohestra_v3 executable attach
    // pattern: construct with the instance, load knobs, attach if enabled
    monitor = std::make_unique<Cohestra::CacheLineDataMonitor>(*instance);
    fatal_if(!monitor->LoadConfiguration(configLoader),
             "%s: failed to load monitor configuration\n", name());
    if (monitor->IsEnabled())
        monitor->Attach();

    // Flit-level transaction logger (debug runs): same attach pattern as
    // the monitor; enabled via [cchi] verbose (the flit_trace param). Its
    // records emit at spdlog debug level, so lower the instance logger's
    // floor while it is on.
    if (flitTrace && instance->GetLogger())
        instance->GetLogger()->set_level(spdlog::level::debug);
    flitLogger = std::make_unique<Cohestra::CCHIFlitLogger>(*instance);
    fatal_if(!flitLogger->LoadConfiguration(configLoader),
             "%s: failed to load flit logger configuration\n", name());
    if (flitLogger->IsEnabled())
        flitLogger->Attach();

    // Home-snoop reflection hook: whenever the home's snoop is accepted
    // by a Taurus node, reflect it into that agent's L1s. snoop_merge=on
    // agents reflect earlier (at gate interception, pre-acceptance) and
    // are skipped here. Registration follows CHIron's EventBus listener
    // API; the listener must not call back into the same node (CHIron
    // ERRATA N3) - reflectSnoopToL1 only talks to gem5 ports.
    for (size_t i = 0; i < agentsByIndex.size(); ++i) {
        auto *node = instance->GetType1Upstream(i);
        fatal_if(!node, "%s: no Taurus node at upstream index %llu\n",
                 name(), (unsigned long long)i);
        CCHIL1Agent *agent = agentsByIndex[i];
        agent->setUpstreamIndex(i);
        node->events->OnAcceptedSNP.Register(Gravity::MakeListener(
            "gem5.CCHIFabric.reflectSnoop", 0,
            std::function<void(AcceptedSNPEvent &)>(
                [this, agent](AcceptedSNPEvent &ev) {
                    onAcceptedSnoop(agent, ev);
                })));
    }

    instanceBuilt = true;

    inform_once(
        "%s: Cohestra instance up with %llu Taurus node(s), %s endpoint, "
        "memory window [%#llx, %#llx), monitor %s\n", name(),
        (unsigned long long)upstreamNodeCount, downstream.c_str(),
        (unsigned long long)memoryStart, (unsigned long long)memoryEnd,
        monitor->IsAttached() ? "attached" : "off");
}

#ifdef CCHI_RTL_ENABLED
void
CCHIFabric::rtlReset()
{
    // High-active reset (mirrors the cohestra_v3 harness): 100 full clock
    // cycles held, then released with all pins quiesced by the interface
    static constexpr unsigned RESET_CYCLES = 100;

    if (const char *tracePath = getenv("CCHI_RTL_TRACE")) {
        if (const char *startEnv = getenv("CCHI_RTL_TRACE_START")) {
            rtl->traceStartTick = std::strtoull(startEnv, nullptr, 0);
        }
        if (rtl->traceStartTick > 0) {
            // Defer the open to the first fabric tick at/after the start
            rtl->tracePendingPath = tracePath;
        } else {
            Verilated::traceEverOn(true);
            rtl->tfp = std::make_unique<VerilatedVcdC>();
            rtl->top->trace(rtl->tfp.get(), 99);
            rtl->tfp->open(tracePath);
            inform("%s: FST waveform dump to %s\n", name(), tracePath);
        }
    }

    rtl->top->reset = 1;
    for (unsigned i = 0; i < RESET_CYCLES; ++i) {
        rtl->top->clock = 0;
        rtl->top->eval();
        rtl->traceDump();
        rtl->top->clock = 1;
        rtl->top->eval();
        rtl->traceDump();
    }
    rtl->top->reset = 0;
}

void
CCHIFabric::rtlClockEvalLow()
{
    rtl->top->clock = 0;
    rtl->top->eval();
    rtl->traceDump();
}

void
CCHIFabric::rtlClockEvalHigh()
{
    rtl->top->clock = 1;
    rtl->top->eval();
    rtl->traceDump();
}
#endif

void
CCHIFabric::tick()
{
    ++stats.ticks;

    // CHIron's own pump: upstream Ticks, downstream Tick, then
    // fixed-priority flit pumping in both directions. SNP interception
    // for snoop_merge=on agents happens lazily inside the gate
    // interface's Peek/PopType1SNP during this call. Taurus futures fire
    // here as well; their bound agent lambdas only post to the agents'
    // completion queues, which the tick hooks below drain.
    const bool alive = instance->Tick(cycleCount);

#ifdef CCHI_RTL_ENABLED
    // Null snoop responder for RTL Type-1 ports beyond the configured
    // upstreams: Venus broadcasts snoops on an SF-maybe to EVERY port
    // (by design, assuming all NUM_T1 ports are live), but the Instance
    // only pumps the ports that have agents (see the gate's capping).
    // Snoops to unused ports would otherwise wait forever and wedge the
    // home's tracker slots in PH_SNOOP - answer them locally with a
    // plain SnpResp(I), which is exactly what a port without the line
    // returns. Responses mature a few cycles in a delay line first: the
    // home's slot must have registered the snoop as issued before any
    // answer arrives, or its unknown-snoop assertion trips. NOTE: this
    // runs BEFORE the clock eval below, because a Pop asserts 'ready'
    // only until the next interface TickPreHandshake - the posedge must
    // land in between or the snoop never leaves the RTL's FIFO.
    if (rtl) {
        static constexpr uint64_t NULL_SNOOP_DELAY = 4;

        for (size_t i = upstreamNodeCount;
             i < rtl->interface->GetType1Count(); ++i) {
            auto snp = rtl->interface->PeekType1SNP(i);
            if (snp) {
                CCHI::Flits::UpRSP<Cohestra::CommonFlitConfigurationType1>
                    rsp;
                rsp.TxnID   = snp->TxnID;
                rsp.SrcID   = snp->TgtID;   // answered by the snoop target
                rsp.TgtID   = snp->SrcID;
                rsp.Opcode  = CCHI::Opcodes::UpRSP::Type1::SnpResp;
                rsp.RespErr = 0;
                rsp.Resp    = CCHI::Resps::I;
                rsp.TraceTag = snp->TraceTag;

                rtl->nullSnoopQueue[i].push_back(
                    {cycleCount + NULL_SNOOP_DELAY, rsp});
                DPRINTF(CCHI, "[%s] null snoop pop: port %d addr %#llx "
                        "id %u\n", name(), int(i),
                        uint64_t(snp->Addr) << 3, unsigned(snp->TxnID));
                rtl->interface->PopType1SNP(i);
            }

            auto &queue = rtl->nullSnoopQueue[i];
            while (!queue.empty() &&
                   queue.front().readyCycle <= cycleCount) {
                if (!rtl->interface->PushType1UpRSP(i, queue.front().rsp))
                    break;
                ++stats.nullSnoopResponses;
                DPRINTF(CCHI, "[%s] null snoop responder: port %d id %u "
                        "(SnpResp I)\n", name(), int(i),
                        unsigned(queue.front().rsp.TxnID));
                queue.pop_front();
            }
        }
    }

    // Verilator backend: serve the endpoint's AXI memory ports, then one
    // full RTL clock cycle in CHIron's two-phase cadence, mirroring the
    // cohestra_v3 harness: TickPreHandshake (drive) -> Eval(clock=0) ->
    // TickPostHandshake (sample + pop) -> Eval(clock=1). The Instance's own
    // pre-handshake drive already ran inside instance->Tick above.
    if (rtl && rtl->axiBridge) {
        // Deferred waveform start (CCHI_RTL_TRACE_START)
        if (!rtl->tracePendingPath.empty() &&
            curTick() >= rtl->traceStartTick) {
            Verilated::traceEverOn(true);
            rtl->tfp = std::make_unique<VerilatedVcdC>();
            rtl->top->trace(rtl->tfp.get(), 99);
            rtl->tfp->open(rtl->tracePendingPath.c_str());
            inform("%s: FST waveform dump to %s (deferred to tick %llu)\n",
                   name(), rtl->tracePendingPath.c_str(),
                   (unsigned long long)curTick());
            rtl->tracePendingPath.clear();
        }
        rtl->axiBridge->preTick(cycleCount);
        rtlClockEvalLow();
        instance->TickPostHandshake(cycleCount);
        rtl->axiBridge->postTick(cycleCount);
        rtlClockEvalHigh();
    }
#endif
    ++cycleCount;

    for (auto *agent : agentsByIndex)
        agent->tickHook();

    // Monitor mismatch accounting + failure check
    if (monitor && monitor->IsAttached()) {
        const uint64_t mismatches = monitor->GetMismatchCount();
        if (mismatches > lastMonitorMismatchCount) {
            stats.monitorMismatches += mismatches - lastMonitorMismatchCount;
            lastMonitorMismatchCount = mismatches;
        }
    }

    if (!alive) {
        fatal_if(instance->IsFailed(),
                 "%s: Cohestra instance FAILED at cycle %llu (%llu monitor "
                 "mismatch(es))\n", name(),
                 (unsigned long long)cycleCount,
                 (unsigned long long)lastMonitorMismatchCount);
        // FINISHED: quiesce (nothing left to pump)
        return;
    }

    schedule(tickEvent, curTick() + clockPeriod());
}

std::optional<CCHIFabric::FlitSNP>
CCHIFabric::gatePeekSNP(size_t index)
{
    SnoopGate &gate = snoopGates[index];

    if (!gate.mergeOn)
        return downstreamInterface->PeekType1SNP(index);

    if (gate.intercepted)
        return gate.ready ? std::optional<FlitSNP>(gate.heldFlit)
                          : std::nullopt;

    // First sight of this home snoop: intercept it so the agent can
    // reflect it into the L1 and merge dirty data before Taurus answers.
    // Called from inside Instance::Tick (the pump peeks before pushing).
    auto flit = downstreamInterface->PeekType1SNP(index);
    if (!flit)
        return std::nullopt;

    gate.heldFlit = *downstreamInterface->PopType1SNP(index);
    gate.intercepted = true;
    gate.ready = false;
    ++stats.snoopGateInterceptions;

    agentsByIndex[index]->handleSnoopFromHome(gate.heldFlit);

    // The agent releases synchronously when no L1 snoop response is
    // expected, making the flit visible to the pump in the same cycle.
    return gate.ready ? std::optional<FlitSNP>(gate.heldFlit)
                      : std::nullopt;
}

std::optional<CCHIFabric::FlitSNP>
CCHIFabric::gatePopSNP(size_t index)
{
    SnoopGate &gate = snoopGates[index];

    if (!gate.mergeOn)
        return downstreamInterface->PopType1SNP(index);

    panic_if(!gate.intercepted || !gate.ready,
             "%s: SNP gate pop for upstream %llu without a released flit\n",
             name(), (unsigned long long)index);

    gate.intercepted = false;
    gate.ready = false;
    return gate.heldFlit;
}

void
CCHIFabric::releaseSnoop(size_t index)
{
    panic_if(index >= snoopGates.size() ||
             !snoopGates[index].mergeOn ||
             !snoopGates[index].intercepted,
             "%s: spurious snoop release for upstream %llu\n",
             name(), (unsigned long long)index);

    snoopGates[index].ready = true;
    ++stats.snoopGateReleases;
}

void
CCHIFabric::onAcceptedSnoop(CCHIL1Agent *agent, AcceptedSNPEvent &ev)
{
    // snoop_merge=on agents reflected the snoop when the gate intercepted
    // it (before PushRXSNP); reflecting again would double-snoop the L1
    if (agent->getSnoopMerge())
        return;

    const auto &flit = ev.GetSNPFlit();
    ++stats.snoopReflections;
    // SNP Addr carries PA >> 3 (bits [2:0] are implicit zeros); the
    // reflected snoop covers the whole 64B line
    agent->reflectSnoopToL1(uint64_t(flit.Addr) << 3,
                            static_cast<uint64_t>(flit.Opcode));
}

bool
CCHIFabric::SnoopGateCCHIInterface::HasType1SNP(size_t index)
    const noexcept
{
    // Informational only (the Instance pump uses Peek/Pop): mirror what
    // PeekType1SNP would return
    const SnoopGate &gate = fabric.snoopGates[index];
    if (gate.mergeOn && gate.intercepted)
        return gate.ready;
    return real->HasType1SNP(index);
}

std::optional<CCHIFabric::FlitSNP>
CCHIFabric::SnoopGateCCHIInterface::PeekType1SNP(size_t index)
    const noexcept
{
    // const interface, mutating gate: the gate state lives in the fabric,
    // so this stays const here while the fabric method does the work
    return fabric.gatePeekSNP(index);
}

std::optional<CCHIFabric::FlitSNP>
CCHIFabric::SnoopGateCCHIInterface::PopType1SNP(size_t index) noexcept
{
    return fabric.gatePopSNP(index);
}

void
CCHIFabric::Gem5MemoryBackend::readLine(uint64_t lineAddr,
                                        std::array<uint64_t, 8> &data)
{
    const Addr pa = lineAddr << 6;
    // Phase 1: zero-time atomic read; the endpoint's latencyRSP/latencyDAT
    // carry the modeled latency. See the TODO on the class declaration.
    RequestPtr req = std::make_shared<Request>(pa, 64, Request::PHYSICAL,
                                               Request::funcRequestorId);
    Packet pkt(req, MemCmd::ReadReq);
    pkt.allocate();
    fabric.sendAtomicOnMemSide(&pkt);
    std::memcpy(data.data(), pkt.getConstPtr<uint8_t>(), 64);
    ++fabric.stats.backendReads;
}

void
CCHIFabric::Gem5MemoryBackend::writeLine(uint64_t lineAddr,
                                         const std::array<uint64_t, 8> &data)
{
    const Addr pa = lineAddr << 6;
    RequestPtr req = std::make_shared<Request>(pa, 64, Request::PHYSICAL,
                                               Request::funcRequestorId);
    Packet pkt(req, MemCmd::WriteReq);
    pkt.allocate();
    std::memcpy(pkt.getPtr<uint8_t>(), data.data(), 64);
    fabric.sendAtomicOnMemSide(&pkt);
    ++fabric.stats.backendWrites;
}

CCHIFabric::MemSideRequestPort::MemSideRequestPort(
    const std::string &_name, CCHIFabric &_parent)
    : QueuedRequestPort(_name, &_parent, _parent.reqQueue,
                        _parent.snoopRespQueue),
      parent(_parent)
{
}

bool
CCHIFabric::MemSideRequestPort::recvTimingResp(PacketPtr pkt)
{
    // Phase 1 issues no timing requests on mem_side (the endpoint's
    // memory backend uses atomic accesses), so a timing response here
    // means the wiring is wrong
    panic("%s: unexpected timing response %s\n", name(), pkt->print());
}

CCHIFabric::CCHIFabricStats::CCHIFabricStats(statistics::Group *parent)
    : statistics::Group(parent),
      ADD_STAT(ticks, statistics::units::Count::get(),
               "Fabric cycles pumped through the Cohestra instance"),
      ADD_STAT(backendReads, statistics::units::Count::get(),
               "Endpoint memory-backend line reads served from gem5 memory"),
      ADD_STAT(backendWrites, statistics::units::Count::get(),
               "Endpoint memory-backend line writes into gem5 memory"),
      ADD_STAT(snoopGateInterceptions, statistics::units::Count::get(),
               "Home snoops intercepted for the snoop_merge flow"),
      ADD_STAT(snoopGateReleases, statistics::units::Count::get(),
               "Held home snoops released to Taurus after merging"),
      ADD_STAT(snoopReflections, statistics::units::Count::get(),
               "Home snoops reflected into L1s (merge-off path)"),
      ADD_STAT(nullSnoopResponses, statistics::units::Count::get(),
               "SnpResp(I) synthesized for unused RTL ports"),
      ADD_STAT(monitorMismatches, statistics::units::Count::get(),
               "CacheLineDataMonitor data mismatches observed")
{
}

} // namespace gem5
