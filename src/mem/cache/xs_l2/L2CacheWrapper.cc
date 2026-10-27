#include "mem/cache/xs_l2/L2CacheWrapper.hh"

#include <algorithm>
#include <cmath>

#include "base/trace.hh"
#include "debug/L2CacheWrapper.hh"
#include "sim/system.hh"

namespace gem5
{

L2CacheWrapper::WrapperStats::WrapperStats(
    statistics::Group *parent, unsigned num_slices)
    : statistics::Group(parent, "slicedPfq"),
      ADD_STAT(pfqDequeueWidth, statistics::units::Count::get(),
               "PFQ entries dequeued in one cycle"),
      ADD_STAT(pfqSendBlocked, statistics::units::Count::get(),
               "Prefetch sends rejected by each slice")
{
    using namespace statistics;
    pfqDequeueWidth.init(0, num_slices, 1);
    pfqSendBlocked.init(num_slices).flags(total | nozero);
    for (unsigned slice = 0; slice < num_slices; ++slice) {
        pfqSendBlocked.subname(slice, std::to_string(slice));
    }
}

L2CacheWrapper::L2CacheWrapper(const L2CacheWrapperParams &p)
    : ClockedObject(p),
      sendPrefetchEvent([this]{ processSendPrefetchEvent(); }, "SendPrefetchEvent"),
      cpu_side_port(p.name + ".cpu_side", *this),
      sliceMask(p.num_slices - 1),
      setMask((p.cache_size / (1 << p.block_bits) / p.cache_assoc / p.num_slices) - 1),
      block_bits(p.block_bits),
      pipe_dir_write_stage(p.pipe_dir_write_stage),
      dirReadBypass(p.dir_read_bypass),
      dataSramBanks(p.data_sram_banks),
      dirSramBanks(p.dir_sram_banks),
      sliced_cache_accessor(this),
      prefetcher(p.prefetcher),
      sliced_prefetcher(
          dynamic_cast<prefetch::SlicedPrefetcher *>(p.prefetcher)),
      system(p.system),
      stats(this, p.num_slices)
{
    if (p.num_slices == 0 || (p.num_slices & (p.num_slices - 1)) != 0) {
        fatal("L2CacheWrapper: num_slices must be a power of 2.");
    }

    if (p.data_sram_banks == 0 || (p.data_sram_banks & (p.data_sram_banks - 1)) != 0) {
        fatal("L2CacheWrapper: data_sram_banks must be a power of 2.");
    }

    if (p.dir_sram_banks == 0 || (p.dir_sram_banks & (p.dir_sram_banks - 1)) != 0) {
        fatal("L2CacheWrapper: dir_sram_banks must be a power of 2.");
    }

    for (int i = 0; i < p.num_slices; ++i) {
        slice_cpuside_ports.emplace_back(p.name + ".slice_cpuside_ports." + std::to_string(i), *this, i);
    }
    prefetch_blocked_by_slice.assign(p.num_slices, false);
    outstanding_prefetches.assign(p.num_slices, nullptr);
    if (prefetcher) {
        if (sliced_prefetcher) {
            sliced_prefetcher->configureSlices(
                p.num_slices, [this](Addr addr) {
                    return static_cast<unsigned>(getSliceId(addr));
                });
        }
        prefetcher->setParentInfo(system, getProbeManager(), &sliced_cache_accessor, 1 << block_bits);
        prefetcher->setPacketReadyCallback(
            [this](Tick ready) { scheduleSendPrefetch(ready); });
    }
}

bool
L2CacheWrapper::needPrefetch()
{
    if (!prefetcher) {
        return false;
    }

    if (sliced_prefetcher) {
        if (sliced_prefetcher->hasPendingIngress()) {
            return true;
        }
        for (unsigned slice = 0;
             slice < sliced_prefetcher->numSlices(); ++slice) {
            if (!prefetch_blocked_by_slice[slice] &&
                !demandWaitingForSlice(slice) &&
                (outstanding_prefetches[slice] ||
                 sliced_prefetcher->hasPendingPacket(slice))) {
                return true;
            }
        }
        return false;
    }

    return prefetcher && (prefetcher->nextPrefetchReadyTime() != MaxTick ||
                          outstanding_prefetch) && !prefetch_blocked;
}

bool
L2CacheWrapper::demandWaitingForSlice(PortID slice_id) const
{
    return upper_req_blocked && blocked_req_slice == slice_id;
}

void
L2CacheWrapper::scheduleSendPrefetch(Tick ready)
{
    if (!needPrefetch())
        return;

    Tick nextReady = MaxTick;
    if (sliced_prefetcher) {
        if (sliced_prefetcher->hasPendingIngress()) {
            nextReady = curTick();
        }
        for (unsigned slice = 0;
             slice < sliced_prefetcher->numSlices(); ++slice) {
            if (prefetch_blocked_by_slice[slice] ||
                demandWaitingForSlice(slice)) {
                continue;
            }
            const Tick slice_ready = outstanding_prefetches[slice] ?
                curTick() :
                sliced_prefetcher->nextPrefetchReadyTime(slice);
            nextReady = std::min(nextReady, slice_ready);
        }
    } else {
        nextReady = outstanding_prefetch ? curTick() :
            prefetcher->nextPrefetchReadyTime();
    }
    if (ready != MaxTick)
        nextReady = std::min(nextReady, ready);
    if (nextReady == MaxTick) {
        return;
    }
    const Tick sendTick = std::max(nextCycleLastTick(), nextReady);

    if (!sendPrefetchEvent.scheduled())
        schedule(sendPrefetchEvent, sendTick);
    else if (sendPrefetchEvent.when() != sendTick)
        reschedule(sendPrefetchEvent, sendTick);
}

void
L2CacheWrapper::processSendPrefetchEvent()
{
    if (sliced_prefetcher) {
        unsigned dequeue_width = 0;
        for (unsigned slice = 0;
             slice < sliced_prefetcher->numSlices(); ++slice) {
            if (prefetch_blocked_by_slice[slice] ||
                demandWaitingForSlice(slice)) {
                continue;
            }

            if (!outstanding_prefetches[slice] &&
                sliced_prefetcher->hasPendingPacket(slice) &&
                sliced_prefetcher->nextPrefetchReadyTime(slice) <=
                    curTick()) {
                outstanding_prefetches[slice] =
                    sliced_prefetcher->getPacket(slice);
                dequeue_width += outstanding_prefetches[slice] != nullptr;
            }

            PacketPtr pkt = outstanding_prefetches[slice];
            if (!pkt) {
                continue;
            }

            panic_if(getSliceId(pkt->getAddr()) != slice,
                     "L2 PFQ slice %u produced address %#x for slice %d",
                     slice, pkt->getAddr(), getSliceId(pkt->getAddr()));
            prefetch_blocked_by_slice[slice] =
                !slice_cpuside_ports[slice].sendTimingReq(pkt);
            if (prefetch_blocked_by_slice[slice]) {
                stats.pfqSendBlocked[slice]++;
            }
            if (!prefetch_blocked_by_slice[slice]) {
                outstanding_prefetches[slice] = nullptr;
            }
        }
        stats.pfqDequeueWidth.sample(dequeue_width);

        // The dequeue phase above is committed before input arbitration. A
        // full slice PFQ that dequeued this cycle therefore accepts a new
        // entry without dropping another queued request.
        sliced_prefetcher->processIngress();
        scheduleSendPrefetch();
        return;
    }

    if (prefetch_blocked) {
        return;
    }

    if (!outstanding_prefetch && prefetcher && prefetcher->hasPendingPacket() &&
        prefetcher->nextPrefetchReadyTime() <= curTick()) {
        outstanding_prefetch = prefetcher->getPacket();
    }

    if (!outstanding_prefetch) {
        scheduleSendPrefetch();
        return;
    }

    auto pkt = outstanding_prefetch;

    auto slice_id = getSliceId(pkt->getAddr());

    if (demandWaitingForSlice(slice_id)) {
        DPRINTF(L2CacheWrapper,
                "Demand is waiting for slice %d; deferring prefetch %#x\n",
                slice_id, pkt->getAddr());
        return;
    }

    prefetch_blocked = !slice_cpuside_ports[slice_id].sendTimingReq(pkt);
    if (prefetch_blocked) {
        return;
    }
    outstanding_prefetch = nullptr;
    scheduleSendPrefetch();
}

Port &
L2CacheWrapper::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_side") {
        return cpu_side_port;
    } else if (if_name == "slice_cpuside_ports") {
        if (idx >= slice_cpuside_ports.size()) {
            panic("L2CacheWrapper: %s: unknown index %d", if_name, idx);
        }
        return slice_cpuside_ports[idx];
    } else {
        return ClockedObject::getPort(if_name, idx);
    }
}

// --- CPUSidePort ---
L2CacheWrapper::CPUSidePort::CPUSidePort(const std::string &name, L2CacheWrapper &owner)
    : ResponsePort(name), owner(owner) {}

bool
L2CacheWrapper::CPUSidePort::recvTimingSnoopResp(PacketPtr pkt)
{
    DPRINTF(L2CacheWrapper, "Got timing snoop resp for addr %#x, forwarding to CPU\n", pkt->getAddr());
    auto slice_id = owner.getSliceId(pkt->getAddr());
    return owner.slice_cpuside_ports[slice_id].sendTimingSnoopResp(pkt);
}

Tick
L2CacheWrapper::CPUSidePort::recvAtomic(PacketPtr pkt)
{
    auto slice_id = owner.getSliceId(pkt->getAddr());
    DPRINTF(L2CacheWrapper, "Got atomic req for addr %#x, routing to slice %d\n", pkt->getAddr(), slice_id);
    return owner.slice_cpuside_ports[slice_id].sendAtomic(pkt);
}

void
L2CacheWrapper::CPUSidePort::recvFunctional(PacketPtr pkt)
{
    auto slice_id = owner.getSliceId(pkt->getAddr());
    DPRINTF(L2CacheWrapper, "Got functional req for addr %#x, routing to slice %d\n", pkt->getAddr(), slice_id);
    owner.slice_cpuside_ports[slice_id].sendFunctional(pkt);
}

bool
L2CacheWrapper::CPUSidePort::recvTimingReq(PacketPtr pkt)
{
    auto slice_id = owner.getSliceId(pkt->getAddr());
    DPRINTF(L2CacheWrapper, "Got timing req for addr %#x, routing to slice %d\n", pkt->getAddr(), slice_id);

    const bool prefetch_blocked = owner.sliced_prefetcher ?
        owner.prefetch_blocked_by_slice[slice_id] :
        owner.prefetch_blocked;
    if (prefetch_blocked ||
        !owner.slice_cpuside_ports[slice_id].sendTimingReq(pkt)) {
        DPRINTF(L2CacheWrapper, "Slice %d is busy, blocking CPU side\n", slice_id);
        owner.upper_req_blocked = true;
        owner.blocked_req_slice = slice_id;
        return false;
    }

    owner.scheduleSendPrefetch();
    return true;
}

void
L2CacheWrapper::CPUSidePort::recvRespRetry()
{
    // The component below the wrapper (a slice) is now ready.
    DPRINTF(L2CacheWrapper, "Got retry from CPU. This should ideally not happen frequently.\n");
    assert(owner.upper_resp_blocked);
    assert(owner.resp_waiting_slice.size() > 0);

    owner.upper_resp_blocked = false;
    auto slice_ids = owner.resp_waiting_slice;
    for (auto slice_id : slice_ids) {
        owner.slice_cpuside_ports[slice_id].sendRetryResp();
    }
}

AddrRangeList
L2CacheWrapper::CPUSidePort::getAddrRanges() const
{
    return owner.slice_cpuside_ports[0].getAddrRanges();
}


// --- SliceCPUSidePort ---
L2CacheWrapper::SliceCPUSidePort::SliceCPUSidePort(const std::string& name, L2CacheWrapper &owner, PortID id)
    : RequestPort(name), owner(owner), id(id) {}

void
L2CacheWrapper::SliceCPUSidePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(L2CacheWrapper, "Got timing snoop req for addr %#x from slice %d,"
                            " forwarding to CPU\n", pkt->getAddr(), id);
    owner.cpu_side_port.sendTimingSnoopReq(pkt);
}

void
L2CacheWrapper::SliceCPUSidePort::recvFunctionalSnoop(PacketPtr pkt)
{
    DPRINTF(L2CacheWrapper, "Got functional snoop req for addr %#x from slice %d,"
                            " forwarding to CPU\n", pkt->getAddr(), id);
    owner.cpu_side_port.sendFunctionalSnoop(pkt);
}

Tick
L2CacheWrapper::SliceCPUSidePort::recvAtomicSnoop(PacketPtr pkt)
{
    DPRINTF(L2CacheWrapper, "Got atomic snoop req for addr %#x from slice %d,"
                            " forwarding to CPU\n", pkt->getAddr(), id);
    return owner.cpu_side_port.sendAtomicSnoop(pkt);
}

bool
L2CacheWrapper::SliceCPUSidePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(L2CacheWrapper, "Got timing resp for addr %#x from slice %d, forwarding to CPU\n", pkt->getAddr(), id);
    if (owner.upper_resp_blocked) {
        DPRINTF(L2CacheWrapper, "Upper resp is blocked, dropping resp for addr %#x\n", pkt->getAddr());
        owner.resp_waiting_slice.insert(id);
        return false;
    }
    bool success = owner.cpu_side_port.sendTimingResp(pkt);
    if (!success) {
        owner.resp_waiting_slice.insert(id);
        owner.upper_resp_blocked = true;
    } else {
        owner.resp_waiting_slice.erase(id);
    }
    return success;
}

void
L2CacheWrapper::SliceCPUSidePort::recvReqRetry()
{
    const bool demand_waiting = owner.demandWaitingForSlice(id);
    const bool prefetch_waiting = owner.sliced_prefetcher ?
        owner.prefetch_blocked_by_slice[id] : owner.prefetch_blocked;
    assert(prefetch_waiting || demand_waiting);
    DPRINTF(L2CacheWrapper, "Got retry from slice %d\n", id);

    if (owner.sliced_prefetcher) {
        owner.prefetch_blocked_by_slice[id] = false;
    } else {
        owner.prefetch_blocked = false;
    }

    if (demand_waiting) {
        owner.upper_req_blocked = false;
        owner.blocked_req_slice = InvalidPortID;
        owner.cpu_side_port.sendRetryReq();
    }
    owner.scheduleSendPrefetch();
}

void
L2CacheWrapper::SliceCPUSidePort::recvRangeChange()
{
    DPRINTF(L2CacheWrapper, "Got range change from slice %d. Propagating to CPU.\n", id);
    owner.cpu_side_port.sendRangeChange();
}

} // namespace gem5
