/*
 * Copyright (c) 2026
 *
 * CCHIAxiMemBridge: see the header for the design contract.
 */

#include "mem/cchi/cchi_axi_mem_bridge.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CCHI.hh"
#include "mem/cchi/cchi_fabric.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5
{

bool
CCHIAxiMemBridge::inWindow(uint64_t addr, uint32_t bytes) const
{
    const AddrRangeList &ranges = fabric.getAddrRanges();
    for (const auto &r : ranges) {
        if (addr >= r.start() && addr + bytes <= r.end())
            return true;
    }
    return false;
}

uint64_t
CCHIAxiMemBridge::nextBeatAddr(uint64_t addr, uint8_t size,
                               Cohestra::AXI::Burst burst) const
{
    switch (burst) {
      case Cohestra::AXI::Burst::FIXED:
        return addr;
      case Cohestra::AXI::Burst::INCR:
        return addr + beatBytes(size);
      default:
        warn_once("cchi_axi: WRAP burst treated as INCR (addr %#llx)\n", addr);
        return addr + beatBytes(size);
    }
}

void
CCHIAxiMemBridge::preTick(uint64_t time)
{
    // Drive previously queued B/R onto the pins (idempotent per timestamp)
    busIf->TickPreHandshake(time);
}

void
CCHIAxiMemBridge::postTick(uint64_t time)
{
    // Sample the settled AW/W/AR channels and retire the driven B/R beats
    busIf->TickPostHandshake(time);

    if (ports.empty())
        ports.resize(busIf->GetPortMaxIndex() + 1);

    for (size_t index : busIf->GetPortIndices()) {
        if (busIf->GetPortDataBits(index) == 0)
            continue;
        serviceWrite(index, ports[index]);
        serviceRead(index, ports[index]);
    }
}

void
CCHIAxiMemBridge::serviceWrite(size_t index, PortState &ps)
{
    const size_t portBytes = busIf->GetPortDataBits(index) / 8;

    // Admit a new burst (one outstanding per port; backpressure via Pop)
    if (!ps.write && busIf->HasAW(index)) {
        auto aw = busIf->PopAW(index);
        WriteTxn txn;
        txn.aw = *aw;
        txn.nextAddr = aw->Addr;
        txn.beatsLeft = aw->Len + 1;
        txn.error = aw->Burst == Cohestra::AXI::Burst::WRAP ||
                    beatBytes(aw->Size) > portBytes ||
                    !inWindow(aw->Addr, beatBytes(aw->Size));
        if (txn.error)
            warn("cchi_axi: AW denied: addr %#llx size %u burst %u\n",
                 aw->Addr, aw->Size, static_cast<unsigned>(aw->Burst));
        DPRINTF(CCHI, "cchi_axi: AW port %zu addr %#llx len %u size %u%s\n",
                index, aw->Addr, aw->Len, aw->Size,
                txn.error ? " (DECERR)" : "");
        ps.write = txn;
    }

    // Consume W beats. NOTE the V3 AXISlaveInterface contract: the
    // channel mirror refreshes only on TickPostHandshake and Pop does not
    // clear the mirrored valid, so at most ONE W beat may be popped per
    // port per tick (a second Pop would re-read the same mirrored beat)
    if (ps.write && busIf->HasW(index)) {
        WriteTxn &w = *ps.write;
        auto beat = busIf->PopW(index);
        DPRINTF(CCHI, "cchi_axi: W port %zu addr %#llx left %u Last %d\n",
                index, w.nextAddr, w.beatsLeft, int(beat->Last));
        const uint32_t bytes = beatBytes(w.aw.Size);
        const uint64_t lineBase = w.nextAddr & ~uint64_t(63);
        const unsigned offset = w.nextAddr & 63;
        panic_if(offset + bytes > 64,
                 "cchi_axi: write beat straddles a line (%#llx + %u)\n",
                 w.nextAddr, bytes);

        // Line read-modify-write with the strobe mask (zero-time
        // atomic into gem5 memory, same contract as the Earth backend)
        RequestPtr req = std::make_shared<Request>(
            lineBase, 64, Request::PHYSICAL, Request::funcRequestorId);
        Packet pkt(req, MemCmd::ReadReq);
        pkt.allocate();
        fabric.sendAtomicOnMemSide(&pkt);

        uint8_t *line = pkt.getPtr<uint8_t>();
        for (uint32_t j = 0; j < bytes; ++j) {
            if ((beat->Strb[j >> 6] >> (j & 63)) & 1)
                line[offset + j] =
                    (beat->Data[j >> 3] >> ((j & 7) * 8)) & 0xff;
        }
        pkt.cmd = MemCmd::WriteReq;
        fabric.sendAtomicOnMemSide(&pkt);

        w.nextAddr = nextBeatAddr(w.nextAddr, w.aw.Size, w.aw.Burst);
        panic_if(w.beatsLeft == 1 && !beat->Last,
                 "cchi_axi: write burst %#llx missing WLAST\n", w.aw.Addr);
        if (--w.beatsLeft == 0) {
            Cohestra::AXI::BundleChannelB b;
            b.Id = w.aw.Id;
            b.Resp = w.error ? Cohestra::AXI::Resp::DECERR
                             : Cohestra::AXI::Resp::OKAY;
            ps.bQueue.push_back(b);
            ps.write.reset();
        }
    }

    // Drive B responses
    while (!ps.bQueue.empty()) {
        const auto &b = ps.bQueue.front();
        if (!busIf->PushB(index, b)) {
            DPRINTF(CCHI, "cchi_axi: B port %zu id %u BACKPRESSURED\n",
                    index, b.Id);
            break;
        }
        DPRINTF(CCHI, "cchi_axi: B port %zu id %u resp %d\n",
                index, b.Id, static_cast<int>(b.Resp));
        ps.bQueue.pop_front();
    }
}

void
CCHIAxiMemBridge::serviceRead(size_t index, PortState &ps)
{
    const size_t portBytes = busIf->GetPortDataBits(index) / 8;

    // Admit a new burst
    if (!ps.read && busIf->HasAR(index)) {
        auto ar = busIf->PopAR(index);
        ReadTxn txn;
        txn.ar = *ar;
        txn.nextAddr = ar->Addr;
        txn.beatsLeft = ar->Len + 1;
        txn.error = ar->Burst == Cohestra::AXI::Burst::WRAP ||
                    beatBytes(ar->Size) > portBytes ||
                    !inWindow(ar->Addr, beatBytes(ar->Size));
        if (txn.error)
            warn("cchi_axi: AR denied: addr %#llx size %u burst %u\n",
                 ar->Addr, ar->Size, static_cast<unsigned>(ar->Burst));
        DPRINTF(CCHI, "cchi_axi: AR port %zu addr %#llx len %u size %u%s\n",
                index, ar->Addr, ar->Len, ar->Size,
                txn.error ? " (DECERR)" : "");
        ps.read = txn;
    }

    // Produce R beats (bounded production; PushR backpressure drains)
    if (ps.read) {
        ReadTxn &r = *ps.read;
        while (r.beatsLeft && ps.rQueue.size() < 2) {
            const uint32_t bytes = beatBytes(r.ar.Size);
            const uint64_t lineBase = r.nextAddr & ~uint64_t(63);
            const unsigned offset = r.nextAddr & 63;
            panic_if(offset + bytes > 64,
                     "cchi_axi: read beat straddles a line (%#llx + %u)\n",
                     r.nextAddr, bytes);

            Cohestra::AXI::BundleChannelR resp;
            resp.Id = r.ar.Id;
            resp.Resp = r.error ? Cohestra::AXI::Resp::DECERR
                                : Cohestra::AXI::Resp::OKAY;
            resp.Data.assign(portBytes / 8, 0);
            resp.Last = (r.beatsLeft == 1);

            if (!r.error) {
                RequestPtr req = std::make_shared<Request>(
                    lineBase, 64, Request::PHYSICAL, Request::funcRequestorId);
                Packet pkt(req, MemCmd::ReadReq);
                pkt.allocate();
                fabric.sendAtomicOnMemSide(&pkt);
                const uint8_t *line = pkt.getConstPtr<uint8_t>();
                for (uint32_t j = 0; j < bytes; ++j)
                    resp.Data[j >> 3] |=
                        uint64_t(line[offset + j]) << ((j & 7) * 8);
            }

            ps.rQueue.push_back(resp);
            r.nextAddr = nextBeatAddr(r.nextAddr, r.ar.Size, r.ar.Burst);
            if (--r.beatsLeft == 0)
                ps.read.reset();
        }
    }

    // Drive R beats
    while (!ps.rQueue.empty()) {
        const auto &r = ps.rQueue.front();
        if (!busIf->PushR(index, r)) {
            DPRINTF(CCHI, "cchi_axi: R port %zu id %u BACKPRESSURED\n",
                    index, r.Id);
            break;
        }
        DPRINTF(CCHI, "cchi_axi: R port %zu id %u last %d\n",
                index, r.Id, int(r.Last));
        ps.rQueue.pop_front();
    }
}

} // namespace gem5
