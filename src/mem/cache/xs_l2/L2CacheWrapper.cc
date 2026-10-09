#include "mem/cache/xs_l2/L2CacheWrapper.hh"

#include <cmath>

#include "base/trace.hh"
#include "debug/L2CacheWrapper.hh"
#include "mem/cache/prefetch/l2_composite_with_worker.hh"
#include "sim/system.hh"

namespace gem5
{

L2CacheWrapper::L2CacheWrapper(const L2CacheWrapperParams &p)
    : ClockedObject(p),
      sendPrefetchEvent([this]{ processSendPrefetchEvent(); }, "SendPrefetchEvent"),
      bankPrefetchBlocked(p.num_slices, false),
      bankOutstandingPrefetch(p.num_slices, nullptr),
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
      system(p.system)
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
    if (prefetcher) {
        prefetcher->setParentInfo(system, getProbeManager(), &sliced_cache_accessor, 1 << block_bits);
        auto *composite = dynamic_cast<
            prefetch::L2CompositeWithWorkerPrefetcher *>(prefetcher);
        if (composite && composite->usesBankedQueue()) {
            bankedPrefetcher = composite;
            composite->configureBanks(
                p.num_slices,
                [this](Addr addr) { return getSliceId(addr); },
                [this]() { scheduleSendPrefetch(); });
        }
    }
}

bool
L2CacheWrapper::needPrefetch()
{
    if (bankedPrefetcher) {
        if (bankedPrefetcher->hasBankInputs())
            return true;
        for (unsigned bank = 0; bank < bankOutstandingPrefetch.size(); ++bank) {
            if (!bankPrefetchBlocked[bank] &&
                !(upper_req_blocked && upperReqBlockedSlice == bank) &&
                (bankOutstandingPrefetch[bank] ||
                 bankedPrefetcher->hasBankPacket(bank))) {
                return true;
            }
        }
        return false;
    }
    return prefetcher && (prefetcher->hasPendingPacket() || outstanding_prefetch) && !prefetch_blocked;
}

void
L2CacheWrapper::scheduleSendPrefetch()
{
    if (needPrefetch() && !sendPrefetchEvent.scheduled()) {
        if (bankedPrefetcher) {
            // Collect inputs during the cycle; arbitrate once at its end.
            Tick when = clockEdge(Cycles(1)) - 1;
            if (when <= curTick())
                when += clockPeriod();
            schedule(sendPrefetchEvent, when);
            return;
        }
        // schedule at the last tick of next cycle
        // because prefetch has lower priority than other requests
        schedule(sendPrefetchEvent, nextCycleLastTick());
    }
}

void
L2CacheWrapper::processSendPrefetchEvent()
{
    if (bankedPrefetcher) {
        processBankedPrefetch();
        return;
    }
    if (prefetch_blocked) {
        return;
    }

    if (!outstanding_prefetch && prefetcher && prefetcher->hasPendingPacket()) {
        outstanding_prefetch = prefetcher->getPacket();
    }

    if (!outstanding_prefetch) {
        return;
    }

    auto pkt = outstanding_prefetch;

    auto slice_id = getSliceId(pkt->getAddr());

    if (upper_req_blocked) {
        DPRINTF(L2CacheWrapper, "Upper req is blocked, dropping prefetch for addr %#x\n", pkt->getAddr());
        return;
    }

    prefetch_blocked = !slice_cpuside_ports[slice_id].sendTimingReq(pkt);
    if (prefetch_blocked) {
        return;
    }
    outstanding_prefetch = nullptr;
    scheduleSendPrefetch();
}

void
L2CacheWrapper::processBankedPrefetch()
{
    // Dequeue into free output slots before enqueue. A full FIFO with a
    // concurrent dequeue must not overwrite another entry.
    for (unsigned bank = 0; bank < bankOutstandingPrefetch.size(); ++bank) {
        if (!bankOutstandingPrefetch[bank])
            bankOutstandingPrefetch[bank] =
                bankedPrefetcher->getBankPacket(bank);
    }
    bankedPrefetcher->enqueueBankInputs();
    for (unsigned bank = 0; bank < bankOutstandingPrefetch.size(); ++bank) {
        auto &pkt = bankOutstandingPrefetch[bank];
        // Empty FIFOs flow through to the output slot.
        if (!pkt)
            pkt = bankedPrefetcher->getBankPacket(bank);
        if (!pkt)
            continue;
        if (bankPrefetchBlocked[bank] ||
            (upper_req_blocked && upperReqBlockedSlice == bank)) {
            bankedPrefetcher->recordBankBlocked(bank);
            continue;
        }
        if (!slice_cpuside_ports[bank].sendTimingReq(pkt)) {
            bankPrefetchBlocked[bank] = true;
            bankedPrefetcher->recordBankBlocked(bank);
        } else {
            pkt = nullptr;
        }
    }
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

    if (owner.bankedPrefetcher) {
        if (owner.bankPrefetchBlocked[slice_id] ||
            !owner.slice_cpuside_ports[slice_id].sendTimingReq(pkt)) {
            owner.upper_req_blocked = true;
            owner.upperReqBlockedSlice = slice_id;
            return false;
        }
        owner.scheduleSendPrefetch();
        return true;
    }

    if (owner.prefetch_blocked || !owner.slice_cpuside_ports[slice_id].sendTimingReq(pkt)) {
        DPRINTF(L2CacheWrapper, "Slice %d is busy, blocking CPU side\n", slice_id);
        owner.upper_req_blocked = true;
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
    if (owner.bankedPrefetcher) {
        const bool retry_upper = owner.upper_req_blocked &&
            owner.upperReqBlockedSlice == id;
        assert(owner.bankPrefetchBlocked[id] || retry_upper);
        owner.bankPrefetchBlocked[id] = false;
        if (retry_upper) {
            owner.upper_req_blocked = false;
            owner.upperReqBlockedSlice = InvalidPortID;
            owner.cpu_side_port.sendRetryReq();
        }
        owner.scheduleSendPrefetch();
        return;
    }
    assert(owner.prefetch_blocked || owner.upper_req_blocked);
    DPRINTF(L2CacheWrapper, "Got retry from slice %d\n", id);
    if (owner.prefetch_blocked) {
        owner.prefetch_blocked = false;
        owner.scheduleSendPrefetch();
    }
    if (owner.upper_req_blocked) {
        owner.upper_req_blocked = false;
        owner.cpu_side_port.sendRetryReq();
    }
}

void
L2CacheWrapper::SliceCPUSidePort::recvRangeChange()
{
    DPRINTF(L2CacheWrapper, "Got range change from slice %d. Propagating to CPU.\n", id);
    owner.cpu_side_port.sendRangeChange();
}

} // namespace gem5
