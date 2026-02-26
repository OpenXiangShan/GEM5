#include "mem/xsCHI/device/L2Wrapper.hh"

#include <sys/types.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>

#include "base/addr_range.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "debug/CHIL2Wrapper.hh"
#include "mem/packet.hh"
#include "mem/xsCHI/base/FlitOpType.hh"
#include "params/Bridge.hh"
#include "params/ClockedObject.hh"
#include "params/SimObject.hh"
#include "sim/cur_tick.hh"

namespace gem5
{
namespace xsCHI
{
    L2Wrapper::L2Wrapper(const Params &p):
    ClockedObject(p),
    cpuSidePort(p.name + ".cpu_side_port", this, "CpuSidePort"),
    memSidePort(p.name + ".mem_side_port", this, "MemSidePort"),
    bridge(p.RNBridge)
    {
        bridge->set_recvReadResp_callback([this](ReqPtr& req) { this->recvReadResp(req); });
        DPRINTF(CHIL2Wrapper,"L2Wrapper Construct,without id\n");

    }

    void
    L2Wrapper::init()
    {
        ClockedObject::init();
        // Propagate address ranges so upstream crossbars have valid routing
        // before the first packet arrives.
        cpuSidePort.sendRangeChange();
    }
    // L2Wrapper::L2Wrapper(const Params &p,NodeID id,SystemAddressMap* sam):
    // ClockedObject(p),
    // cpuSidePort(p.name + ".cpu_side_port", this, "CpuSidePort"),
    // bridge(p,id,sam)
    // {
    //     bridge->set_recvReadResp_callback([this](ReqPtr& req) { this->recvReadResp(req); });
    //     DPRINTF(CHIL2Wrapper,"L2Wrapper Construct,id:%d",id.getNodeID());

    // }
    bool
    L2Wrapper::CpuSidePort::recvTimingSnoopResp(PacketPtr pkt)
    {
        //todo:handle snoop situation！
        return true;
    }


    bool
    L2Wrapper::CpuSidePort::tryTiming(PacketPtr pkt)
    {
        //no need to do it
        return true;
    }

    bool
    L2Wrapper::CpuSidePort::recvTimingReq(PacketPtr pkt)
    {
        // if pkt is a Uncached request, we should redirect it to MemSidePort
        if (pkt->req->isUncacheable()) {
            DPRINTF(CHIL2Wrapper,"Recv Uncached request, redirect to MemSidePort, cmd:%s, addr: %lx\n",
                    pkt->cmdString(), pkt->getAddr());
            // redirect to MemSidePort
            return wrapper->memSidePort.sendTimingReq(pkt);
        }
        
        assert(pkt->isRequest());
        DPRINTF(CHIL2Wrapper,"RecvReq, cmd:%s, addr: %lx\n",pkt->cmdString(),pkt->getAddr());
        ReqPtr req = wrapper->CreateRequest(pkt);

        wrapper->bridge->ReceiveReq(req, false);
        assert(wrapper->outstanding_pkts.count(pkt->getAddr())==0);
        if (pkt->needsResponse()) {
            assert(!pkt->isWrite());
            wrapper->outstanding_pkts[pkt->getAddr()] = pkt;
        }
        //always true
        return true;
    }



    // AddrRangeList
    // L2Wrapper::CpuSidePort::getAddrRanges() const
    // {
    //     return cache->getAddrRanges();
    // }


    L2Wrapper::
    CpuSidePort::CpuSidePort(const std::string &_name, L2Wrapper *wrapper,
                            const std::string &_label)
        : CacheResponsePort(_name, wrapper, _label),wrapper(wrapper)
    {
    }

    L2Wrapper::MemSidePort::MemSidePort(const std::string &_name,
                                        L2Wrapper *wrapper,
                                        const std::string &_label)
        : CacheRequestPort(_name, wrapper, _reqQueue, _snoopRespQueue),
        _reqQueue(*wrapper, *this, _label),
        _snoopRespQueue(*wrapper, *this, true, _label), wrapper(wrapper)
    {
    }

    L2Wrapper::CacheResponsePort::CacheResponsePort(const std::string &_name,
                                            L2Wrapper *wrapper,
                                            const std::string &_label)
        : QueuedResponsePort(_name, wrapper, queue),
        queue(*wrapper, *this, true, _label),
        blocked(false), mustSendRetry(false),
        sendRetryEvent([this]{ processSendRetry(); }, _name)
    {
    }

    void
    L2Wrapper::CacheResponsePort::setBlocked()
    {
        assert(!blocked);
        // DPRINTF(CHIL2Wrapper, "Port is blocking new requests\n");
        blocked = true;
        // if we already scheduled a retry in this cycle, but it has not yet
        // happened, cancel it
        if (sendRetryEvent.scheduled()) {
            owner.deschedule(sendRetryEvent);
            // DPRINTF(CHIL2Wrapper, "Port descheduled retry\n");
            mustSendRetry = true;
        }
    }

    void
    L2Wrapper::CacheResponsePort::clearBlocked()
    {
        assert(blocked);
        // DPRINTF(CHIL2Wrapper, "Port is accepting new requests\n");
        blocked = false;
        if (mustSendRetry) {
            // @TODO: need to find a better time (next cycle?)
            owner.schedule(sendRetryEvent, curTick() + 1);
        }
    }

    void
    L2Wrapper::CacheResponsePort::processSendRetry()
    {
        DPRINTF(CHIL2Wrapper, "Port is sending retry\n");

        // reset the flag and call retry
        mustSendRetry = false;
        sendRetryReq();
    }

    Tick
    L2Wrapper::CpuSidePort::recvAtomic(PacketPtr pkt)
    {
        panic("not supported");
        return curTick();
    }

    void
    L2Wrapper::CpuSidePort::recvFunctional(PacketPtr pkt)
    {
        panic("not supported");
    }

    AddrRangeList
    L2Wrapper::CpuSidePort::getAddrRanges() const
    {
        AddrRangeList ranges;
        // Advertise a catch-all range so upstream crossbars know this port can
        // service any address that reaches the L2 wrapper.
        ranges.push_back(RangeSize(0, MaxAddr));
        return ranges;
    }

    ReqPtr
    L2Wrapper::CreateRequest(PacketPtr pkt)
    {
        //phrase pkt
        Addr addr = pkt->getAddr();
        uint32_t size = pkt->getSize();
        CHI_OP_TYPE op = CHI_OP_TYPE::CHI_REQ_OP_START;
        bool pktHasData = false;
        if (pkt->cmd==MemCmd(MemCmd::ReadExReq)){
            op = CHI_OP_TYPE::CHI_REQ_READUNIQUE;
        }else if (pkt->cmd==MemCmd(MemCmd::ReadSharedReq)){
            op = CHI_OP_TYPE::CHI_REQ_READSHARED;
        }else if (pkt->cmd==MemCmd(MemCmd::ReadCleanReq)) {
            op = CHI_OP_TYPE::CHI_REQ_READCLEAN;
        }else if (pkt->cmd==MemCmd(MemCmd::CleanEvict)) {
            op = CHI_OP_TYPE::CHI_REQ_EVICT;
        }else if (pkt->cmd==MemCmd(MemCmd::WritebackDirty)) {
            op = CHI_OP_TYPE::CHI_REQ_WRITEBACKFULL;
            pktHasData = true;
        }else if (pkt->cmd==MemCmd(MemCmd::WritebackClean)){
            op = CHI_OP_TYPE::CHI_REQ_WRITECLEANFULL;
            pktHasData = true;
        }else if (pkt->cmd==MemCmd(MemCmd::HardPFReq)) {
            op = CHI_OP_TYPE::CHI_REQ_READUNIQUE;
        }else if (pkt->cmd==MemCmd(MemCmd::UpgradeReq)){
            op = CHI_OP_TYPE::CHI_REQ_CLEANUNIQUE;
        }else {
            assert(false && "unsupported Req!");
        }
        DPRINTF(CHIL2Wrapper,"Create Req, op:%s, addr: %lx, size:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(op),addr,size);
        ReqPtr req = std::make_shared<Request>(op,addr,size);
        if (pktHasData) {
            req->setData(pkt);
        }
        return req;
    }

    void
    L2Wrapper::recvReadResp(ReqPtr &req){
        DPRINTF(CHIL2Wrapper,"Recv Read Resp, op:%s, addr: %lx, size:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(req->getOpcode()),req->getAddr(),req->getSize());
        assert(outstanding_pkts.count(req->getAddr())>0);
        PacketPtr pkt = outstanding_pkts[req->getAddr()];
        assert(pkt->needsResponse());
        // todo: properly set delay!
        // decide to ignore the delay, leave it to l2xbar
        // assert(pkt->headerDelay == 0);
        // assert(pkt->payloadDelay == 0);
        pkt->makeTimingResponse();
        uint8_t *tmp = new uint8_t[req->getSize()];
        assert(req->getSize()==pkt->getSize());
        req->getData(tmp);
        pkt->setData(tmp);
        delete[] tmp; // 释放临时内存
        cpuSidePort.schedTimingResp(pkt, curTick());

        outstanding_pkts.erase(req->getAddr());

    }
    gem5::Port &
    L2Wrapper::getPort(const std::string &if_name, PortID idx)
    {

        if (if_name == "mem_side_port")
            return memSidePort;
        else if (if_name == "cpu_side_port")
            return cpuSidePort;
        else
            // pass it along to our super class
            return ClockedObject::getPort(if_name, idx);
    }
    CHIPort*
    L2Wrapper::getCHIPort(){
        return bridge->getNetworkPort();
    }
    CHIBridge* L2Wrapper::getBridge(){
        return bridge;
    }

    ///////////////
//
// MemSidePort
//
///////////////
bool
L2Wrapper::MemSidePort::recvTimingResp(PacketPtr pkt)
{
    wrapper->cpuSidePort.schedTimingResp(pkt, curTick());
    // cache->recvTimingResp(pkt);
    return true;
}

void
L2Wrapper::MemSidePort::recvFunctionalCustomSignal(PacketPtr pkt, int sig)
{
    assert(false && "recvFunctionalCustomSignal not implemented in L2Wrapper::MemSidePort");
}

// Express snooping requests to memside port
void
L2Wrapper::MemSidePort::recvTimingSnoopReq(PacketPtr pkt)
{
    // Snoops shouldn't happen when bypassing caches
    assert(false && "Snoops should not happen inL2Wrapper ");

}

Tick
L2Wrapper::MemSidePort::recvAtomicSnoop(PacketPtr pkt)
{
    panic("not supported");
    return curTick();
}

void
L2Wrapper::MemSidePort::recvFunctionalSnoop(PacketPtr pkt)
{
    panic("not supported");
}
}
}
