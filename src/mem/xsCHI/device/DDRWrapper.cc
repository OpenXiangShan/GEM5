#include <algorithm>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include "DDRWrapper.hh"
#include "base/flags.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/CHIDramsim.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "sim/system.hh"

namespace gem5
{
namespace xsCHI
{

    DDRWrapper::DDRWrapper(const Params &p) :
    AbstractMemory(p),
    _NodeID(0),
    useDMT(true),
    readResponsePaddingCycles(p.read_response_padding_cycles),
    port(p.networkPort),
    read_cb(std::bind(&DDRWrapper::readComplete,
                      this, 0, std::placeholders::_1)),
    write_cb(std::bind(&DDRWrapper::writeComplete,
                       this, 0, std::placeholders::_1)),
    wrapper(p.configFile, p.filePath, read_cb, write_cb),
    retryReq(false), retryResp(false), startTick(0),
    nbrOutstandingReads(0), nbrOutstandingWrites(0),
    sendResponseEvent([this]{ sendResponse(); }, AbstractMemory::name()),
    tickEvent([this]{ tick(); }, AbstractMemory::name()),
    TXN_Manager(1024)
    {
    DPRINTF(CHIDramsim,
            "Instantiated DDRWrapper with clock %d ns and queue size %d\n",
            wrapper.clockPeriod(), wrapper.queueSize());

    // Register a callback to compensate for the destructor not
    // being called. The callback prints the DDRWrapper stats.
    port->setReceiveCallback(
        [this](FlitPtr& flit) {
            // No-op callback, always return false
            return handlePortReceive(flit);
        });
    port->setCreditUnblockCallback(
        [this](Flit::CHI_CHN_TYPE channel) { handleCreditUnblock(channel); });
    port->setOwner(this);
    registerExitCallback([this]() { wrapper.printStats(); });
    }

//     DDRWrapper::DDRWrapper(const Params &p, NodeID nodeID, SystemAddressMap *sam) :
//     AbstractMemory(p),
//     _NodeID(nodeID),
//     sam(sam),
//     port(nullptr),
//     read_cb(std::bind(&DDRWrapper::readComplete,
//                       this, 0, std::placeholders::_1)),
//     write_cb(std::bind(&DDRWrapper::writeComplete,
//                        this, 0, std::placeholders::_1)),
//     wrapper(p.configFile, p.filePath, read_cb, write_cb),
//     retryReq(false), retryResp(false), startTick(0),
//     nbrOutstandingReads(0), nbrOutstandingWrites(0),
//     sendResponseEvent([this]{ sendResponse(); }, AbstractMemory::name()),
//     tickEvent([this]{ tick(); }, AbstractMemory::name()),
//     TXN_Manager(1024)
// {
//     DPRINTF(CHIDramsim,
//             "Instantiated DDRWrapper with clock %d ns and queue size %d\n",
//             wrapper.clockPeriod(), wrapper.queueSize());

//     // Register a callback to compensate for the destructor not
//     // being called. The callback prints the DDRWrapper stats.
//     port->setReceiveCallback(
//         [this](FlitPtr& flit) {
//             // No-op callback, always return false
//             return handlePortReceive(flit);
//         });
//     registerExitCallback([this]() { wrapper.printStats(); });
// }

void
DDRWrapper::init()
{
    AbstractMemory::init();

    if (!port->isConnected()) {
        fatal("DDRWrapper %s is unconnected!\n", name());
    } else {
        // port->sendRangeChange();
    }

    if (system()->cacheLineSize() != wrapper.burstSize())
        fatal("DDRWrapper burst size %d does not match cache line size %d\n",
              wrapper.burstSize(), system()->cacheLineSize());
}

void
DDRWrapper::startup()
{
    startTick = curTick();

    // kick off the clock ticks
    schedule(tickEvent, clockEdge());
}

void
DDRWrapper::resetStats() {
    wrapper.resetStats();
}

void
DDRWrapper::dumpOutstandingReadState(const char *reason, Addr focusAddr) const
{
    DPRINTF(CHIDramsim,
            "read_state_dump reason=%s focus=%#lx outstandingReads=%u "
            "transferMap=%u readTracks=%u responseQueue=%u tick=%llu\n",
            reason,
            focusAddr,
            static_cast<unsigned>(outstandingReads.size()),
            static_cast<unsigned>(outstandingReadTransferMap.size()),
            static_cast<unsigned>(readTracks.size()),
            static_cast<unsigned>(responseQueue.size()),
            static_cast<unsigned long long>(curTick()));
    for (const auto &[addr, queue] : outstandingReads) {
        DPRINTF(CHIDramsim,
                "read_state_dump outstandingReads addr=%#lx depth=%u\n",
                addr,
                static_cast<unsigned>(queue.size()));
    }
    for (const auto &[addr, req] : outstandingReadTransferMap) {
        DPRINTF(CHIDramsim,
                "read_state_dump transferMap addr=%#lx reqTxn=%u retTxn=%u src=%u tgt=%u req=%p\n",
                addr,
                req ? req->getTransactionId() : 0,
                req ? req->getReturnTxnid() : 0,
                req ? req->getSourceId() : 0,
                req ? req->getTargetId() : 0,
                req.get());
    }
    for (const auto &[addr, track] : readTracks) {
        DPRINTF(CHIDramsim,
                "read_state_dump readTrack addr=%#lx reqTxn=%u retTxn=%u "
                "src=%u tgt=%u insert=%llu readComplete=%llu "
                "sendResp=%llu agingWarned=%d\n",
                addr,
                track.reqTxnId,
                track.returnTxnId,
                track.srcId,
                track.tgtId,
                static_cast<unsigned long long>(track.insertTick),
                static_cast<unsigned long long>(track.readCompleteTick),
                static_cast<unsigned long long>(track.sendRespTick),
                track.agingWarned);
    }
}

void
DDRWrapper::scanAgedReadTracks(const char *where)
{
    const Tick warnThreshold = clockPeriod() * DiagnosticAgeWarnCycles;
    for (auto &[addr, track] : readTracks) {
        if (track.agingWarned || track.insertTick == 0) {
            continue;
        }
        const Tick age = curTick() - track.insertTick;
        if (age < warnThreshold) {
            continue;
        }
        track.agingWarned = true;
        DPRINTF(CHIDramsim,
                "read_track_aging_warn where=%s addr=%#lx reqTxn=%u "
                "retTxn=%u src=%u tgt=%u age_cycles=%llu age_ticks=%llu "
                "readComplete=%llu sendResp=%llu\n",
                where,
                addr,
                track.reqTxnId,
                track.returnTxnId,
                track.srcId,
                track.tgtId,
                static_cast<unsigned long long>(age / clockPeriod()),
                static_cast<unsigned long long>(age),
                static_cast<unsigned long long>(track.readCompleteTick),
                static_cast<unsigned long long>(track.sendRespTick));
        dumpOutstandingReadState("aging_warn", addr);
    }
}

void
DDRWrapper::sendResponse()
{
    // assert(!retryResp);
    assert(!responseQueue.empty());

    DPRINTF(CHIDramsim, "Attempting to send response\n");

    auto [pkt, time] = responseQueue.top();
    if (time > curTick()) {
        DPRINTF(CHIDramsim,
                "sendResponse stage=not_ready addr=%#lx readyTick=%llu "
                "tick=%llu\n",
                pkt->getAddr(),
                static_cast<unsigned long long>(time),
                static_cast<unsigned long long>(curTick()));
        scheduleSendResponseRetry();
        return;
    }
    auto reqIt = outstandingReadTransferMap.find(pkt->getAddr());
    assert(reqIt != outstandingReadTransferMap.end());
    assert(reqIt->second != nullptr);
    ReqPtr req = reqIt->second;
    if (!req->DataValid()) {
        // if the data is not set, we need to set it here
        // this is the case for read requests
        req->setData(pkt.get());
        assert(!req->dataTransferStarted());
    }
    uint32_t data_id = req->generateWriteDataID();
    FlitPtr data_flit = std::make_unique<Flit>();
    assert(data_flit && "Failed to create data Flit");
    data_flit->setOpcode(CHI_OP_TYPE::CHI_DAT_COMPDATA);
    data_flit->setDataId(data_id);
    data_flit->setCcid(0); // assuming CCID is always 0
    data_flit->setSize(req->getSize());
    data_flit->setAddr(req->getAddr());
    data_flit->setData(req);
    if (useDMT) {
        data_flit->setTgtId(req->getReturnNid());
        data_flit->setTxnId(req->getReturnTxnid());
    } else {
        data_flit->setTgtId(req->getSourceId());
        data_flit->setTxnId(req->getTransactionId());
    }
    data_flit->setSrcId(_NodeID);
    data_flit->setHomeNid(req->getSourceId());
    data_flit->setDbid(req->getTransactionId());

    auto trackIt = readTracks.find(pkt->getAddr());
    DPRINTF(CHIDramsim,
            "sendResponse stage=attempt addr=%#lx reqTxnId=%u "
            "returnTxnId=%u srcId=%u tgtId=%u dataId=%u dbid=%u "
            "queueDepth=%u responseQueueDepth=%u tick=%llu\n",
            pkt->getAddr(),
            req->getTransactionId(),
            req->getReturnTxnid(),
            req->getSourceId(),
            useDMT ? req->getReturnNid() : req->getSourceId(),
            data_id,
            req->getTransactionId(),
            outstandingReads.count(pkt->getAddr()) ?
                static_cast<unsigned>(outstandingReads.at(pkt->getAddr()).size()) : 0,
            static_cast<unsigned>(responseQueue.size()),
            static_cast<unsigned long long>(curTick()));

    if (port->send(data_flit)){
        //send success, we can save the request and txn_id
        req->finishTransferdata(data_id);
        if (trackIt != readTracks.end()) {
            trackIt->second.sendRespTick = curTick();
        }
        DPRINTF(CHIDramsim,
                "sendResponse stage=sent addr=%#lx reqTxnId=%u "
                "returnTxnId=%u srcId=%u tgtId=%u dataId=%u dbid=%u "
                "responseQueueDepth=%u tick=%llu\n",
                pkt->getAddr(),
                req->getTransactionId(),
                req->getReturnTxnid(),
                req->getSourceId(),
                useDMT ? req->getReturnNid() : req->getSourceId(),
                data_id,
                req->getTransactionId(),
                static_cast<unsigned>(responseQueue.size()),
                static_cast<unsigned long long>(curTick()));
    }else {
        //free the data_flit if send failed
        if (data_flit != nullptr) {
            data_flit.reset();
        }
        DPRINTF(CHIDramsim,
                "sendResponse stage=blocked addr=%#lx reqTxnId=%u "
                "returnTxnId=%u srcId=%u tgtId=%u dataId=%u dbid=%u "
                "responseQueueDepth=%u tick=%llu\n",
                pkt->getAddr(),
                req->getTransactionId(),
                req->getReturnTxnid(),
                req->getSourceId(),
                useDMT ? req->getReturnNid() : req->getSourceId(),
                data_id,
                req->getTransactionId(),
                static_cast<unsigned>(responseQueue.size()),
                static_cast<unsigned long long>(curTick()));
    }
    if (req->dataTransferFinished()){
        responseQueue.pop();
        outstandingReadTransferMap.erase(reqIt);
    }
    DPRINTF(CHIDramsim, "Have %d read, %d write, %d responses outstanding\n",
                    nbrOutstandingReads, nbrOutstandingWrites,
                    responseQueue.size());

    if (!responseQueue.empty()) {
        scheduleSendResponseRetry();
    }

    if (nbrOutstanding() == 0)
        signalDrainDone();


}

unsigned int
DDRWrapper::nbrOutstanding() const
{
    return nbrOutstandingReads + nbrOutstandingWrites + responseQueue.size();
}

void
DDRWrapper::tick()
{
    // Only tick when it's timing mode
    if (system()->isTimingMode()) {
        wrapper.tick();
        scanAgedReadTracks("tick");

        // is the connected port waiting for a retry, if so check the
        // state and send a retry if conditions have changed
        // if (retryReq && nbrOutstanding() < wrapper.queueSize()) {
        //     retryReq = false;
        //     port->sendRetryReq();
        // }
    }

    schedule(tickEvent,
        curTick() + wrapper.clockPeriod() * sim_clock::as_int::ns);

    // DPRINTF(CHIDramsim, "Scheduled Dramsim after %d ns, at tick %lu\n", wrapper.clockPeriod(),
    //         curTick() + wrapper.clockPeriod() * sim_clock::as_int::ns);
}

// Tick
// DDRWrapper::recvAtomic(PacketPtr pkt)
// {
//     access(pkt);

//     // 50 ns is just an arbitrary value at this point
//     return pkt->cacheResponding() ? 0 : 50000;
// }

// void
// DDRWrapper::recvFunctional(PacketPtr pkt)
// {
//     pkt->pushLabel(name());

//     functionalAccess(pkt);

//     // potentially update the packets in our response queue as well
//     for (auto i = responseQueue.begin(); i != responseQueue.end(); ++i)
//         pkt->trySatisfyFunctional((*i).first);

//     pkt->popLabel();
// }

bool
DDRWrapper::recvTimingReq(std::shared_ptr<Packet> pkt)
{
    // if a cache is responding, sink the packet without further action
    // if (pkt->cacheResponding()) {
    //     pendingDelete.reset(pkt);
    //     return true;
    // }

    // we should not get a new request after committing to retry the
    // current one, but unfortunately the CPU violates this rule, so
    // simply ignore it for now
    // if (retryReq) {
    //     // DPRINTF(CHIDramsim, "Ignoring request while waiting for retry\n");
    //     return false;
    // }

    // if we cannot accept we need to send a retry once progress can
    // be made
    bool outstanding_full = (nbrOutstanding() >= wrapper.queueSize());
    if (pkt->isWrite()){
        outstanding_full = false ;
    }
    // bool can_accept = (nbrOutstanding() < wrapper.queueSize()) &&
    //                   wrapper.canAccept(pkt->getAddr(), pkt->isWrite());
    bool wrapper_can_acc = true;
    if (!outstanding_full) {
        wrapper_can_acc = wrapper.canAccept(pkt->getAddr(), pkt->isWrite());
    }
    bool can_accept = !outstanding_full && wrapper_can_acc;

    // if (pkt->isWrite()){
    //     //since we intercept write transaction before here, if a write comes in, it should be acceptted.
    //     // assert(can_accept);
    //     if(!can_accept) 
    //         kill(getpid(), SIGTRAP);
    // }

    DPRINTF(CHIDramsim, "Can accept: %i, outstanding: %u, queue size: %u, wrapper can acc: %i, is write: %i\n",
            can_accept, nbrOutstanding(), wrapper.queueSize(), wrapper_can_acc, pkt->isWrite());

    // keep track of the transaction
    if (pkt->isRead()) {
        if (can_accept) {
            outstandingReads[pkt->getAddr()].push(pkt);

            // we count a transaction as outstanding until it has left the
            // queue in the controller, and the response has been sent
            // back, note that this will differ for reads and writes
            ++nbrOutstandingReads;
        }
    } else if (pkt->isWrite()) {
        if (can_accept) {
            outstandingWrites[pkt->getAddr()].push(pkt);

            //not do it here ,instead, add it when recv CHI_REQ_WRITENOSNPFULL!!
            // ++nbrOutstandingWrites;

            // perform the access for writes
            accessAndRespond(pkt);
        }
    } else {
        // keep it simple and just respond if necessary
        accessAndRespond(pkt);
        return true;
    }

    if (can_accept) {
        // we should never have a situation when we think there is space,
        // and there isn't
        // DPRINTF(CHIDramsim, "Enqueueing address %s for %s, pkt cmd %s\n",
        //         pkt->getAddrRange().to_string(),
        //         pkt->isWrite() ? "write" : "read", pkt->cmdString());

        // @todo what about the granularity here, implicit assumption that
        // a transaction matches the burst size of the memory (which we
        // cannot determine without parsing the ini file ourselves)
        wrapper.enqueue(pkt->getAddr(), pkt->isWrite());

        return true;
    } else {
        // retryReq = true;
        return false;
    }
}

void
DDRWrapper::recvRespRetry()
{
    // DPRINTF(CHIDramsim, "Retrying\n");

    assert(retryResp);
    retryResp = false;
    sendResponse();
}

void
DDRWrapper::accessAndRespond(std::shared_ptr<Packet> pkt)
{
    // DPRINTF(CHIDramsim, "Access for address %lx\n", pkt->getAddr());

    const bool wasRead = pkt->isRead();
    bool needsResponse = pkt->needsResponse();

    // do the actual memory access which also turns the packet into a
    // response
    // pkt->allocate();
    access(pkt.get());

    // turn packet around to go back to requestor if response expected
    if (needsResponse) {
        // access already turned the packet into a response
        assert(pkt->isResponse());
        // Here we pay for xbar additional delay and to process the payload
        // of the packet.
        const Tick time = curTick() +
            (wasRead ? cyclesToTicks(readResponsePaddingCycles) : 0);

        DPRINTF(CHIDramsim,
                "accessAndRespond queue response addr=%#lx wasRead=%d "
                "paddingCycles=%llu readyTick=%llu tick=%llu\n",
                pkt->getAddr(),
                wasRead,
                static_cast<unsigned long long>(readResponsePaddingCycles),
                static_cast<unsigned long long>(time),
                static_cast<unsigned long long>(curTick()));

        // queue it to be sent back
        responseQueue.push({pkt, time});

        // if we are not already waiting for a retry, or are scheduled
        // to send a response, schedule an event
        scheduleSendResponseRetry();
    }
    // else {
    //     // queue the packet for deletion
    //     pendingDelete.reset(pkt);
    // }
}

void
DDRWrapper::scheduleSendResponseRetry()
{
    if (responseQueue.empty() || sendResponseEvent.scheduled()) {
        return;
    }
    if (port->isChannelBlockedByCredit(Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA)) {
        return;
    }
    const Tick readyTick = responseQueue.top().second;
    const Tick nextTick = curTick() + clockPeriod();
    schedule(sendResponseEvent, std::max(readyTick, nextTick));
}

void
DDRWrapper::handleCreditUnblock(Flit::CHI_CHN_TYPE channel)
{
    if (channel == Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA &&
        !responseQueue.empty()) {
        scheduleSendResponseRetry();
    }
}

void DDRWrapper::readComplete(unsigned id, uint64_t addr)
{

    // DPRINTF(CHIDramsim, "Read to address %lx complete\n", addr);

    // get the outstanding reads for the address in question
    auto p = outstandingReads.find(addr);
    if (p == outstandingReads.end()) {
        dumpOutstandingReadState("read_complete_missing_outstanding", addr);
        panic("DDRWrapper readComplete missing outstanding read addr=%#lx", addr);
    }

    // first in first out, which is not necessarily true, but it is
    // the best we can do at this point
    auto pkt = p->second.front();
    p->second.pop();
    const unsigned remainingDepth = static_cast<unsigned>(p->second.size());

    if (p->second.empty())
        outstandingReads.erase(p);

    // no need to check for drain here as the next call will add a
    // response to the response queue straight away
    assert(nbrOutstandingReads != 0);
    --nbrOutstandingReads;

    auto trackIt = readTracks.find(addr);
    if (trackIt != readTracks.end()) {
        trackIt->second.readCompleteTick = curTick();
        DPRINTF(CHIDramsim,
                "readComplete stage=callback addr=%#lx reqTxnId=%u "
                "returnTxnId=%u srcId=%u tgtId=%u remainingDepth=%u "
                "latency_cycles=%llu tick=%llu\n",
                addr,
                trackIt->second.reqTxnId,
                trackIt->second.returnTxnId,
                trackIt->second.srcId,
                trackIt->second.tgtId,
                remainingDepth,
                static_cast<unsigned long long>(
                    (curTick() - trackIt->second.insertTick) / clockPeriod()),
                static_cast<unsigned long long>(curTick()));
    } else {
        DPRINTF(CHIDramsim,
                "readComplete stage=callback addr=%#lx no_readTrack "
                "remainingDepth=%u tick=%llu\n",
                addr,
                remainingDepth,
                static_cast<unsigned long long>(curTick()));
    }

    // perform the actual memory access
    accessAndRespond(pkt);
}

void DDRWrapper::writeComplete(unsigned id, uint64_t addr)
{

    // DPRINTF(CHIDramsim, "Write to address %lld complete\n", addr);

    // get the outstanding reads for the address in question
    auto p = outstandingWrites.find(addr);
    assert(p != outstandingWrites.end());

    // we have already responded, and this is only to keep track of
    // what is outstanding
    p->second.pop();
    if (p->second.empty())
        outstandingWrites.erase(p);

    assert(nbrOutstandingWrites != 0);
    --nbrOutstandingWrites;

    if (nbrOutstanding() == 0)
        signalDrainDone();
}

Port&
DDRWrapper::getPort(const std::string &if_name, PortID idx)
{

        return ClockedObject::getPort(if_name, idx);

}

DrainState
DDRWrapper::drain()
{
    // check our outstanding reads and writes and if any they need to
    // drain
    return nbrOutstanding() != 0 ? DrainState::Draining : DrainState::Drained;
}

bool
DDRWrapper::handlePortReceive(FlitPtr &flit)
{

    DPRINTF(CHIDramsim,"Recv Flit, op:%s, addr: %lx, size:%d\n",CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(flit->getOpcode()),flit->getAddr(),flit->getSize());
    switch (flit->getOpcode()) {
        case CHI_OP_TYPE::CHI_REQ_READNOSNP:
        // case CHI_OP_TYPE::CHI_REQ_READNOSNPSEP://not supported for the moment
        {
            //assume always use DMT!
            RequestPtr Req = std::make_shared<gem5::Request>(
                flit->getAddr(), flit->getSize(), gem5::Flags<uint64_t>(0),
                RequestorID(0));
            Req->setPaddr(flit->getAddr());
            auto pkt = (std::make_shared<Packet>(Req,gem5::MemCmd::ReadExReq,CACHEBLOCK_SIZE));
            pkt->allocate();
            if (recvTimingReq(pkt)) {
                ReqPtr req = std::make_shared<Request>(
                        flit->getOpcode(),flit->getAddr(),flit->getSize());
                req->setSourceId(flit->getSrcId());
                req->setTargetId(flit->getTgtId());
                req->setTransactionId(flit->getTxnId());
                if (useDMT) {
                    req->setReturnNid(flit->getReturnNid());
                    req->setReturnTxnid(flit->getReturnTxnid());
                } else {
                    req->setReturnNid(flit->getSrcId());
                    req->setReturnTxnid(flit->getTxnId());
                }
                req->setSize(flit->getSize());
                //here we do not have data,but need to fill it when start transfer
                outstandingReadTransferMap[pkt->getAddr()] = req;
                ReadTrack track;
                track.addr = pkt->getAddr();
                track.reqTxnId = flit->getTxnId();
                track.returnTxnId = req->getReturnTxnid();
                track.srcId = flit->getSrcId();
                track.tgtId = flit->getTgtId();
                track.insertTick = curTick();
                readTracks[pkt->getAddr()] = track;
                DPRINTF(CHIDramsim,
                        "handlePortReceive stage=READNOSNP_ACCEPT "
                        "addr=%#lx txnId=%u returnTxnId=%u srcId=%u "
                        "tgtId=%u queueDepth=%u outstandingReadsSize=%u "
                        "outstandingReadTransferMapSize=%u tick=%llu\n",
                        pkt->getAddr(),
                        flit->getTxnId(),
                        req->getReturnTxnid(),
                        flit->getSrcId(),
                        flit->getTgtId(),
                        outstandingReads.count(pkt->getAddr()) ?
                            static_cast<unsigned>(outstandingReads[pkt->getAddr()].size()) : 0,
                        static_cast<unsigned>(outstandingReads.size()),
                        static_cast<unsigned>(outstandingReadTransferMap.size()),
                        static_cast<unsigned long long>(curTick()));
                return true;

            }else{
                return false;
            }
            break;
        }
        case CHI_OP_TYPE::CHI_REQ_WRITENOSNPFULL:
        {
            // if (flit->getAddr()==0xf6499640) {
            //     kill(getpid(), SIGTRAP);
            // }
            bool outstanding_full = (nbrOutstanding() >= wrapper.queueSize());
            bool wrapper_can_acc = true;
            if (!outstanding_full) {
                wrapper_can_acc = wrapper.canAccept(flit->getAddr(), true);
            }
            bool can_accept = !outstanding_full && wrapper_can_acc;
            if (!can_accept) {
                return false;
            }else{
                //sendback a DBIDResp
                nbrOutstandingWrites++;
                FlitPtr resp = std::make_unique<Flit>();
                assert(resp && "Failed to create response Flit");
                resp->setOpcode(CHI_OP_TYPE::CHI_RSP_DBIDRESP);
                int dbid = TXN_Manager.getID();
                DPRINTF(CHIDramsim, "Get TxnID %d for DBIDResp Flit\n", dbid);
                assert(dbid >= 0 && "Failed to get TxnID, dramsim3 always get a TxnID");
                resp->setDbid(dbid);
                resp->setSrcId(_NodeID);
                resp->setTgtId(flit->getSrcId());
                resp->setTxnId(flit->getTxnId());
                if (port->send(resp)) {
                    //send success, we can save the request
                    ReqPtr req = std::make_shared<Request>(
                        flit->getOpcode(),flit->getAddr(),flit->getSize());
                    req->setTransactionId(dbid);
                    req->setSize(flit->getSize());
                    saveOutstandingRequest(req, dbid);

                    return true;
                } else {
                    nbrOutstandingWrites--;
                    TXN_Manager.releaseID(dbid);
                    // assert(false && "Failed to send DBIDResp Flit");
                    DPRINTF(CHIDramsim, "Failed to send DBIDResp Flit\n");
                    //free the resp flit
                    if (resp != nullptr) {
                        resp.reset();
                    }
                    return false;
                }
            }


        }
        case CHI_OP_TYPE::CHI_DAT_NCBWRDATACOMPACK:
        {
            // this is a data flit for a write, we need to find the
            // request that corresponds to this transaction ID
            int txn_id = flit->getTxnId();
            auto it = outstanding_requests.find(txn_id);
            if (it == outstanding_requests.end()) {
                panic("DDRWrapper received data flit with unknown TxnID %d", txn_id);
                return false;
            }
            ReqPtr req = it->second;

            // now we can process the data flit
            if(!req->dataTransferFinished()){
                req->gatherDataFlit(flit);
            }
            if (req->dataTransferFinished()) {
                // all data flits for this request have been received
                RequestPtr req0 = std::make_shared<gem5::Request>(
                flit->getAddr(), flit->getSize(), gem5::Flags<uint64_t>(0),
                RequestorID(0));
                auto pkt = std::make_shared<Packet>(req0,gem5::MemCmd::WritebackDirty,flit->getSize());
                pkt->allocate();
                pkt->setAddr(req->getAddr());
                pkt->setSize(flit->getSize());
                uint8_t* tmp = new uint8_t[flit->getSize()];
                flit->getData(tmp);
                pkt->setData(tmp);
                delete[] tmp; // 释放临时内存
                if (recvTimingReq(pkt)) {
                    //finish the req!
                    TXN_Manager.releaseID(flit->getTxnId());
                    DPRINTF(CHIDramsim, "Finish write request: txn_id=%d, outstanding_requests.size()=%d\n", flit->getTxnId(), outstanding_requests.size());
                    outstanding_requests.erase(it);
                    return true;

                }else{
                    //warn: may casue deadlock

                    // warn("DDRWrapper failed to send write request for TxnID %d, outstanding_requests.size()=%d",
                    //       flit->getTxnId(), outstanding_requests.size());
                    pkt.reset();

                    return false;
                }
            }
            return true;

        }
        default:
        {
            panic("DDRWrapper received unsupported opcode %s from port %s",
                  static_cast<int>(flit->getOpcode()), port->name());
            return false;
        }
    }
    return false;
}
void DDRWrapper::saveOutstandingRequest(ReqPtr &req, uint32_t txn_id)
{
    // save the request with the given transaction ID
    if (outstanding_requests.find(txn_id) == outstanding_requests.end()) {
        outstanding_requests[txn_id] = req;
    } else {
        panic("Request with transaction ID %u already exists", txn_id);
    }
}

void DDRWrapper::setNodeID(uint32_t _ID){
    this->_NodeID = _ID;
}
// void DDRWrapper::setSAM(SystemAddressMap *sam){
//     this->sam = sam;
// }
}
}
