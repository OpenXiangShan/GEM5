/*
 * Copyright (c) 2026
 *
 * CCHIAxiMemBridge: serves the AXI4 memory (slave) ports of a verilated
 * CCHI downstream endpoint (e.g. Venus) from the gem5 memory system.
 * Written against CHIron's Cohestra::AXISlaveInterface: the endpoint's AW/W/
 * AR channels are drained (Pop) and B/R responses driven (Push); data
 * movement itself goes through the fabric's zero-time atomic path into
 * gem5's membus/DRAM (same modelling contract as the Earth endpoint's
 * MemoryBackend - endpoint/burst latencies are carried by the RTL itself).
 */

#ifndef __MEM_CCHI_CCHI_AXI_MEM_BRIDGE_HH__
#define __MEM_CCHI_CCHI_AXI_MEM_BRIDGE_HH__

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "base/types.hh"
#include "cohestra_axi.hpp"

namespace gem5
{

class CCHIFabric;

class CCHIAxiMemBridge
{
  public:
    CCHIAxiMemBridge(CCHIFabric &_fabric,
                     std::shared_ptr<Cohestra::AXISlaveInterface> _busIf)
        : fabric(_fabric), busIf(std::move(_busIf)) { }

    /** Two-phase per-cycle service (CHIron's split tick contract):
        preTick drives the queued B/R beats onto the pins (before the
        clock=0 eval); postTick samples/retires the settled channels, then
        drains AW/W/AR and queues the next B/R (after the clock=0 eval,
        before the posedge eval). */
    void preTick(uint64_t time);
    void postTick(uint64_t time);

  protected:
    /** One outstanding write burst per AXI port (backpressured by Pop). */
    struct WriteTxn {
        Cohestra::AXI::BundleChannelAW aw;
        uint64_t nextAddr = 0;
        uint32_t beatsLeft = 0;
        bool error = false;
    };

    /** One outstanding read burst per AXI port. */
    struct ReadTxn {
        Cohestra::AXI::BundleChannelAR ar;
        uint64_t nextAddr = 0;
        uint32_t beatsLeft = 0;
        bool error = false;
    };

    struct PortState {
        std::optional<WriteTxn> write;
        std::optional<ReadTxn> read;
        std::deque<Cohestra::AXI::BundleChannelB> bQueue;
        std::deque<Cohestra::AXI::BundleChannelR> rQueue;
    };

    void serviceWrite(size_t index, PortState &ps);
    void serviceRead(size_t index, PortState &ps);

    /** Burst beat semantics: bytes per beat, next address (INCR/FIXED;
        WRAP degrades to INCR with a one-time warning). */
    static uint32_t beatBytes(uint8_t size) { return 1u << size; }
    uint64_t nextBeatAddr(uint64_t addr, uint8_t size,
                          Cohestra::AXI::Burst burst) const;

    bool inWindow(uint64_t addr, uint32_t bytes) const;

    CCHIFabric &fabric;
    std::shared_ptr<Cohestra::AXISlaveInterface> busIf;
    std::vector<PortState> ports;
};

} // namespace gem5

#endif // __MEM_CCHI_CCHI_AXI_MEM_BRIDGE_HH__
