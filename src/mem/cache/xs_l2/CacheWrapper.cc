#include "mem/cache/xs_l2/CacheWrapper.hh"

#include "base/trace.hh"
#include "debug/CacheWrapper.hh"

namespace gem5
{

CacheWrapper::CacheWrapper(const CacheWrapperParams &p)
    : ClockedObject(p),
      cpu_side_port(p.name + ".cpu_side", this),
      mem_side_port(p.name + ".mem_side", this),
      inner_cpu_port(p.name + ".inner_cpu_port", this),
      inner_mem_port(p.name + ".inner_mem_port", this)
{
}

Port &
CacheWrapper::getPort(const std::string &if_name, PortID idx)
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
CacheWrapper::CPUSidePort::CPUSidePort(const std::string& name,
                                         CacheWrapper *owner)
    : ResponsePort(name, owner), owner(owner)
{
}

bool
CacheWrapper::cpuSidePortRecvTimingReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got request from CPU side for addr: %#x\n", pkt->getAddr());

    if (!inner_cpu_port.sendTimingReq(pkt)) {
        DPRINTF(CacheWrapper, "Inner cache busy, returning false to CPU side, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

bool
CacheWrapper::cpuSidePortRecvTimingSnoopResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop resp from CPU side for addr: %#x\n", pkt->getAddr());
    return inner_cpu_port.sendTimingSnoopResp(pkt);
}

void
CacheWrapper::cpuSidePortRecvRespRetry()
{
    DPRINTF(CacheWrapper, "Got resp retry from CPU side\n");
    inner_cpu_port.sendRetryResp();
}

AddrRangeList
CacheWrapper::cpuSidePortGetAddrRanges() const
{
    return inner_cpu_port.getAddrRanges();
}

void
CacheWrapper::cpuSidePortRecvFunctional(PacketPtr pkt)
{
    inner_cpu_port.sendFunctional(pkt);
}

Tick
CacheWrapper::cpuSidePortRecvAtomic(PacketPtr pkt)
{
    return inner_cpu_port.sendAtomic(pkt);
}

// --- InnerCPUSidePort (Master) - Sends to inner cache CPU-side ---
CacheWrapper::InnerCPUSidePort::InnerCPUSidePort(const std::string& name,
                                            CacheWrapper* owner)
    : RequestPort(name, owner), owner(owner)
{
}

bool
CacheWrapper::innerCpuPortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got resp from inner cache (CPU side) for addr: %#x\n", pkt->getAddr());

    if (!cpu_side_port.sendTimingResp(pkt)) {
         DPRINTF(CacheWrapper, "Response to CPU side was blocked!\n");
         return false;
    }
    return true;
}

void
CacheWrapper::innerCpuPortRecvReqRetry()
{
    DPRINTF(CacheWrapper, "Got req retry from inner cache, forwarding to CPU side\n");
    cpu_side_port.sendRetryReq();
}

void
CacheWrapper::innerCpuPortRecvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop from inner cache for addr: %#x\n", pkt->getAddr());
    cpu_side_port.sendTimingSnoopReq(pkt);
}

void
CacheWrapper::innerCpuPortRecvFunctionalSnoop(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got functional snoop from inner cache for addr: %#x\n", pkt->getAddr());
    cpu_side_port.sendFunctionalSnoop(pkt);
}

Tick
CacheWrapper::innerCpuPortRecvAtomicSnoop(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got atomic snoop from inner cache for addr: %#x\n", pkt->getAddr());
    return cpu_side_port.sendAtomicSnoop(pkt);
}

void
CacheWrapper::innerCpuPortRecvRangeChange()
{
    DPRINTF(CacheWrapper, "Got range change from inner cache\n");
    cpu_side_port.sendRangeChange();
}

// --- InnerMemSidePort (Slave) - Receives from inner cache Mem-side ---
CacheWrapper::InnerMemSidePort::InnerMemSidePort(const std::string& name,
                                            CacheWrapper* owner)
    : ResponsePort(name, owner), owner(owner)
{
}

bool
CacheWrapper::innerMemPortRecvTimingReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got req from inner cache (Mem side) for addr: %#x\n", pkt->getAddr());

    if (!mem_side_port.sendTimingReq(pkt)) {
        DPRINTF(CacheWrapper, "Memory side busy, returning false to inner cache, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

bool
CacheWrapper::innerMemPortRecvTimingSnoopResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop resp from inner cache for addr: %#x\n", pkt->getAddr());
    return mem_side_port.sendTimingSnoopResp(pkt);
}

AddrRangeList
CacheWrapper::innerMemPortGetAddrRanges() const
{
    return mem_side_port.getAddrRanges();
}

void
CacheWrapper::innerMemPortRecvRespRetry()
{
    DPRINTF(CacheWrapper, "Got resp retry from inner cache (Mem side)\n");
    mem_side_port.sendRetryResp();
}

void
CacheWrapper::innerMemPortRecvFunctional(PacketPtr pkt)
{
    mem_side_port.sendFunctional(pkt);
}

Tick
CacheWrapper::innerMemPortRecvAtomic(PacketPtr pkt)
{
    return mem_side_port.sendAtomic(pkt);
}

// --- MemSidePort (Master) - Sends to memory ---
CacheWrapper::MemSidePort::MemSidePort(const std::string& name,
                                        CacheWrapper* owner)
    : RequestPort(name, owner), owner(owner)
{
}

bool
CacheWrapper::memSidePortRecvTimingResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got resp from memory side for addr: %#x\n", pkt->getAddr());

    if (!inner_mem_port.sendTimingResp(pkt)) {
        DPRINTF(CacheWrapper, "Response to inner cache was blocked!, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

void
CacheWrapper::memSidePortRecvReqRetry()
{
    DPRINTF(CacheWrapper, "Got req retry from memory side\n");
    inner_mem_port.sendRetryReq();
}

void
CacheWrapper::memSidePortRecvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop from memory side for addr: %#x\n", pkt->getAddr());
    inner_mem_port.sendTimingSnoopReq(pkt);
}

void
CacheWrapper::memSidePortRecvRangeChange()
{
    DPRINTF(CacheWrapper, "Got range change from memory side\n");
    inner_mem_port.sendRangeChange();
}

void
CacheWrapper::memSidePortRecvFunctionalSnoop(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got functional snoop from memory side for addr: %#x\n", pkt->getAddr());
    inner_mem_port.sendFunctionalSnoop(pkt);
}

Tick
CacheWrapper::memSidePortRecvAtomicSnoop(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got atomic snoop from memory side for addr: %#x\n", pkt->getAddr());
    return inner_mem_port.sendAtomicSnoop(pkt);
}

} // namespace gem5
