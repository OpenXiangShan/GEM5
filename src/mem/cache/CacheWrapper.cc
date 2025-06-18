#include "mem/cache/CacheWrapper.hh"

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
CacheWrapper::CPUSidePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got request from CPU side for addr: %#x\n", pkt->getAddr());

    if (!owner->inner_cpu_port.sendTimingReq(pkt)) {
        DPRINTF(CacheWrapper, "Inner cache busy, returning false to CPU side, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

bool
CacheWrapper::CPUSidePort::recvTimingSnoopResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop resp from CPU side for addr: %#x\n", pkt->getAddr());
    return owner->inner_cpu_port.sendTimingSnoopResp(pkt);
}

void
CacheWrapper::CPUSidePort::recvRespRetry()
{
    DPRINTF(CacheWrapper, "Got resp retry from CPU side\n");
    owner->inner_cpu_port.sendRetryResp();
}

AddrRangeList
CacheWrapper::CPUSidePort::getAddrRanges() const
{
    return owner->inner_cpu_port.getAddrRanges();
}

void
CacheWrapper::CPUSidePort::recvFunctional(PacketPtr pkt)
{
    owner->inner_cpu_port.sendFunctional(pkt);
}

Tick
CacheWrapper::CPUSidePort::recvAtomic(PacketPtr pkt)
{
    return owner->inner_cpu_port.sendAtomic(pkt);
}

// --- InnerCPUSidePort (Master) - Sends to inner cache CPU-side ---
CacheWrapper::InnerCPUSidePort::InnerCPUSidePort(const std::string& name,
                                            CacheWrapper* owner)
    : RequestPort(name, owner), owner(owner)
{
}

bool
CacheWrapper::InnerCPUSidePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got resp from inner cache (CPU side) for addr: %#x\n", pkt->getAddr());

    if (!owner->cpu_side_port.sendTimingResp(pkt)) {
         DPRINTF(CacheWrapper, "Response to CPU side was blocked!\n");
         return false;
    }
    return true;
}

void
CacheWrapper::InnerCPUSidePort::recvReqRetry()
{
    DPRINTF(CacheWrapper, "Got req retry from inner cache, forwarding to CPU side\n");
    owner->cpu_side_port.sendRetryReq();
}

void
CacheWrapper::InnerCPUSidePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop from inner cache for addr: %#x\n", pkt->getAddr());
    owner->cpu_side_port.sendTimingSnoopReq(pkt);
}

void
CacheWrapper::InnerCPUSidePort::recvRangeChange()
{
    DPRINTF(CacheWrapper, "Got range change from inner cache\n");
    owner->cpu_side_port.sendRangeChange();
}


// --- InnerMemSidePort (Slave) - Receives from inner cache Mem-side ---
CacheWrapper::InnerMemSidePort::InnerMemSidePort(const std::string& name,
                                            CacheWrapper* owner)
    : ResponsePort(name, owner), owner(owner)
{
}

bool
CacheWrapper::InnerMemSidePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got req from inner cache (Mem side) for addr: %#x\n", pkt->getAddr());

    if (!owner->mem_side_port.sendTimingReq(pkt)) {
        DPRINTF(CacheWrapper, "Memory side busy, returning false to inner cache, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

bool
CacheWrapper::InnerMemSidePort::recvTimingSnoopResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop resp from inner cache for addr: %#x\n", pkt->getAddr());
    return owner->mem_side_port.sendTimingSnoopResp(pkt);
}

AddrRangeList
CacheWrapper::InnerMemSidePort::getAddrRanges() const
{
    return owner->mem_side_port.getAddrRanges();
}

void
CacheWrapper::InnerMemSidePort::recvRespRetry()
{
    DPRINTF(CacheWrapper, "Got resp retry from inner cache (Mem side)\n");
    owner->mem_side_port.sendRetryResp();
}

void
CacheWrapper::InnerMemSidePort::recvFunctional(PacketPtr pkt)
{
    owner->mem_side_port.sendFunctional(pkt);
}

Tick
CacheWrapper::InnerMemSidePort::recvAtomic(PacketPtr pkt)
{
    return owner->mem_side_port.sendAtomic(pkt);
}


// --- MemSidePort (Master) - Sends to memory ---
CacheWrapper::MemSidePort::MemSidePort(const std::string& name,
                                        CacheWrapper* owner)
    : RequestPort(name, owner), owner(owner)
{
}

bool
CacheWrapper::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got resp from memory side for addr: %#x\n", pkt->getAddr());

    if (!owner->inner_mem_port.sendTimingResp(pkt)) {
        DPRINTF(CacheWrapper, "Response to inner cache was blocked!, Pkt addr: %#x\n", pkt->getAddr());
        return false;
    }
    return true;
}

void
CacheWrapper::MemSidePort::recvReqRetry()
{
    DPRINTF(CacheWrapper, "Got req retry from memory side\n");
    owner->inner_mem_port.sendRetryReq();
}

void
CacheWrapper::MemSidePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CacheWrapper, "Got snoop from memory side for addr: %#x\n", pkt->getAddr());
    owner->inner_mem_port.sendTimingSnoopReq(pkt);
}

void
CacheWrapper::MemSidePort::recvRangeChange()
{
    DPRINTF(CacheWrapper, "Got range change from memory side\n");
    owner->inner_mem_port.sendRangeChange();
}

} // namespace gem5
