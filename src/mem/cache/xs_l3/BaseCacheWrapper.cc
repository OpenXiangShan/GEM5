#include "mem/cache/xs_l3/BaseCacheWrapper.hh"

#include "base/trace.hh"
#include "debug/BaseCacheWrapper.hh"

namespace gem5
{

BaseCacheWrapper::BaseCacheWrapper(const BaseCacheWrapperParams &p)
    : ClockedObject(p),
      cpu_side_port(p.name + ".cpu_side", this),
      mem_side_port(p.name + ".mem_side", this),
      inner_cpu_port(p.name + ".inner_cpu_port", this),
      inner_mem_port(p.name + ".inner_mem_port", this)
{
}

Port &
BaseCacheWrapper::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "cpu_side") {
        return cpu_side_port;
    } else if (if_name == "mem_side") {
        return mem_side_port;
    } else if (if_name == "inner_cpu_port") {
        return inner_cpu_port;
    } else if (if_name == "inner_mem_port") {
        return inner_mem_port;
    } else {
        return ClockedObject::getPort(if_name, idx);
    }
}

// --- CPUSidePort (Slave) - Receives from L1/CPU ---
BaseCacheWrapper::CPUSidePort::CPUSidePort(const std::string& name,
                                         BaseCacheWrapper *owner)
    : ResponsePort(name, owner), owner(owner)
{
}

bool
BaseCacheWrapper::cpuSidePortRecvTimingReq(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got request from CPU side for addr: %#x\n", pkt->getAddr());

    if (!inner_cpu_port.sendTimingReq(pkt)) {
        DPRINTF(BaseCacheWrapper, "Inner cache busy, returning false to CPU side, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

bool
BaseCacheWrapper::cpuSidePortRecvTimingSnoopResp(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got snoop resp from CPU side for addr: %#x\n", pkt->getAddr());
    return inner_cpu_port.sendTimingSnoopResp(pkt);
}

void
BaseCacheWrapper::cpuSidePortRecvRespRetry()
{
    DPRINTF(BaseCacheWrapper, "Got resp retry from CPU side\n");
    inner_cpu_port.sendRetryResp();
}

AddrRangeList
BaseCacheWrapper::cpuSidePortGetAddrRanges() const
{
    return inner_cpu_port.getAddrRanges();
}

void
BaseCacheWrapper::cpuSidePortRecvFunctional(PacketPtr pkt)
{
    inner_cpu_port.sendFunctional(pkt);
}

Tick
BaseCacheWrapper::cpuSidePortRecvAtomic(PacketPtr pkt)
{
    return inner_cpu_port.sendAtomic(pkt);
}

// --- InnerCPUSidePort (Master) - Sends to inner cache CPU-side ---
BaseCacheWrapper::InnerCPUSidePort::InnerCPUSidePort(const std::string& name,
                                            BaseCacheWrapper* owner)
    : RequestPort(name, owner), owner(owner)
{
}

bool
BaseCacheWrapper::innerCpuPortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got resp from inner cache (CPU side) for addr: %#x\n", pkt->getAddr());

    if (!cpu_side_port.sendTimingResp(pkt)) {
         DPRINTF(BaseCacheWrapper, "Response to CPU side was blocked!\n");
         return false;
    }
    return true;
}

void
BaseCacheWrapper::innerCpuPortRecvReqRetry()
{
    DPRINTF(BaseCacheWrapper, "Got req retry from inner cache, forwarding to CPU side\n");
    cpu_side_port.sendRetryReq();
}

void
BaseCacheWrapper::innerCpuPortRecvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got snoop from inner cache for addr: %#x\n", pkt->getAddr());
    cpu_side_port.sendTimingSnoopReq(pkt);
}

void
BaseCacheWrapper::innerCpuPortRecvRangeChange()
{
    DPRINTF(BaseCacheWrapper, "Got range change from inner cache\n");
    cpu_side_port.sendRangeChange();
}

// --- InnerMemSidePort (Slave) - Receives from inner cache Mem-side ---
BaseCacheWrapper::InnerMemSidePort::InnerMemSidePort(const std::string& name,
                                            BaseCacheWrapper* owner)
    : ResponsePort(name, owner), owner(owner)
{
}

bool
BaseCacheWrapper::innerMemPortRecvTimingReq(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got req from inner cache (Mem side) for addr: %#x\n", pkt->getAddr());

    if (!mem_side_port.sendTimingReq(pkt)) {
        DPRINTF(BaseCacheWrapper, "Memory side busy, returning false to inner cache, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

bool
BaseCacheWrapper::innerMemPortRecvTimingSnoopResp(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got snoop resp from inner cache for addr: %#x\n", pkt->getAddr());
    return mem_side_port.sendTimingSnoopResp(pkt);
}

AddrRangeList
BaseCacheWrapper::innerMemPortGetAddrRanges() const
{
    return mem_side_port.getAddrRanges();
}

void
BaseCacheWrapper::innerMemPortRecvRespRetry()
{
    DPRINTF(BaseCacheWrapper, "Got resp retry from inner cache (Mem side)\n");
    mem_side_port.sendRetryResp();
}

void
BaseCacheWrapper::innerMemPortRecvFunctional(PacketPtr pkt)
{
    mem_side_port.sendFunctional(pkt);
}

Tick
BaseCacheWrapper::innerMemPortRecvAtomic(PacketPtr pkt)
{
    return mem_side_port.sendAtomic(pkt);
}

// --- MemSidePort (Master) - Sends to memory ---
BaseCacheWrapper::MemSidePort::MemSidePort(const std::string& name,
                                        BaseCacheWrapper* owner)
    : RequestPort(name, owner), owner(owner)
{
}

bool
BaseCacheWrapper::memSidePortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got resp from memory side for addr: %#x\n", pkt->getAddr());

    if (!inner_mem_port.sendTimingResp(pkt)) {
        DPRINTF(BaseCacheWrapper, "Response to inner cache was blocked!, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

void
BaseCacheWrapper::memSidePortRecvReqRetry()
{
    DPRINTF(BaseCacheWrapper, "Got req retry from memory side\n");
    inner_mem_port.sendRetryReq();
}

void
BaseCacheWrapper::memSidePortRecvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(BaseCacheWrapper, "Got snoop from memory side for addr: %#x\n", pkt->getAddr());
    inner_mem_port.sendTimingSnoopReq(pkt);
}

void
BaseCacheWrapper::memSidePortRecvRangeChange()
{
    DPRINTF(BaseCacheWrapper, "Got range change from memory side\n");
    inner_mem_port.sendRangeChange();
}

} // namespace gem5
