#include "DDRWrapper.hh"

namespace gem5
{
namespace xsCHI
{
    DDRWrapper::DDRWrapper(const Params &p) :
    AbstractMemory(p),
    port(AbstractMemory::name() + ".port", *this),
    read_cb(std::bind(&DDRWrapper::readComplete,
                      this, 0, std::placeholders::_1)),
    write_cb(std::bind(&DDRWrapper::writeComplete,
                       this, 0, std::placeholders::_1)),
    wrapper(p.configFile, p.filePath, read_cb, write_cb),
    retryReq(false), retryResp(false), startTick(0),
    nbrOutstandingReads(0), nbrOutstandingWrites(0),
    sendResponseEvent([this]{ sendResponse(); }, AbstractMemory::name()),
    tickEvent([this]{ tick(); }, AbstractMemory::name())
{
    DPRINTF(DDRWrapper,
            "Instantiated DDRWrapper with clock %d ns and queue size %d\n",
            wrapper.clockPeriod(), wrapper.queueSize());

    // Register a callback to compensate for the destructor not
    // being called. The callback prints the DDRWrapper stats.
    registerExitCallback([this]() { wrapper.printStats(); });
}

void
DDRWrapper::init()
{
    AbstractMemory::init();

    if (!port.isConnected()) {
        fatal("DDRWrapper %s is unconnected!\n", name());
    } else {
        port.sendRangeChange();
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
DDRWrapper::sendResponse()
{
    assert(!retryResp);
    assert(!responseQueue.empty());

    DPRINTF(DDRWrapper, "Attempting to send response\n");

    auto [pkt, time] = responseQueue.top();
    assert(time <= curTick());
    bool success = port.send(pkt);
    if (success) {
        responseQueue.pop();

        DPRINTF(DDRWrapper, "Have %d read, %d write, %d responses outstanding\n",
                nbrOutstandingReads, nbrOutstandingWrites,
                responseQueue.size());

        if (!responseQueue.empty() && !sendResponseEvent.scheduled()) {
            Tick nextReadyTime = responseQueue.top().second > clockEdge(Cycles(1)) ?
                responseQueue.top().second : clockEdge(Cycles(2)); // ddr to l2 bus 32-bytes width
            schedule(sendResponseEvent, nextReadyTime);
        }

        if (nbrOutstanding() == 0)
            signalDrainDone();
    } else {
        retryResp = true;

        DPRINTF(DDRWrapper, "Waiting for response retry\n");

        assert(!sendResponseEvent.scheduled());
    }
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

        // is the connected port waiting for a retry, if so check the
        // state and send a retry if conditions have changed
        if (retryReq && nbrOutstanding() < wrapper.queueSize()) {
            retryReq = false;
            port.sendRetryReq();
        }
    }

    schedule(tickEvent,
        curTick() + wrapper.clockPeriod() * sim_clock::as_int::ns);

    DPRINTF(DDRWrapper, "Scheduled Dramsim after %d ns, at tick %lu\n", wrapper.clockPeriod(),
            curTick() + wrapper.clockPeriod() * sim_clock::as_int::ns);
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
DDRWrapper::recvTimingReq(PacketPtr pkt)
{
    // if a cache is responding, sink the packet without further action
    if (pkt->cacheResponding()) {
        pendingDelete.reset(pkt);
        return true;
    }

    // we should not get a new request after committing to retry the
    // current one, but unfortunately the CPU violates this rule, so
    // simply ignore it for now
    if (retryReq) {
        DPRINTF(DDRWrapper, "Ignoring request while waiting for retry\n");
        return false;
    }

    // if we cannot accept we need to send a retry once progress can
    // be made
    bool outstanding_full = (nbrOutstanding() >= wrapper.queueSize());
    // bool can_accept = (nbrOutstanding() < wrapper.queueSize()) &&
    //                   wrapper.canAccept(pkt->getAddr(), pkt->isWrite());
    bool wrapper_can_acc = true;
    if (!outstanding_full) {
        wrapper_can_acc = wrapper.canAccept(pkt->getAddr(), pkt->isWrite());
    }
    bool can_accept = !outstanding_full && wrapper_can_acc;

    DPRINTF(DDRWrapper, "Can accept: %i, outstanding: %u, queue size: %u, wrapper can acc: %i, is write: %i\n",
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

            ++nbrOutstandingWrites;

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
        DPRINTF(DDRWrapper, "Enqueueing address %s for %s, pkt cmd %s\n",
                pkt->getAddrRange().to_string(),
                pkt->isWrite() ? "write" : "read", pkt->cmdString());

        // @todo what about the granularity here, implicit assumption that
        // a transaction matches the burst size of the memory (which we
        // cannot determine without parsing the ini file ourselves)
        wrapper.enqueue(pkt->getAddr(), pkt->isWrite());

        return true;
    } else {
        retryReq = true;
        return false;
    }
}

void
DDRWrapper::recvRespRetry()
{
    DPRINTF(DDRWrapper, "Retrying\n");

    assert(retryResp);
    retryResp = false;
    sendResponse();
}

void
DDRWrapper::accessAndRespond(PacketPtr pkt)
{
    DPRINTF(DDRWrapper, "Access for address %lx\n", pkt->getAddr());

    bool needsResponse = pkt->needsResponse();

    // do the actual memory access which also turns the packet into a
    // response
    access(pkt);

    // turn packet around to go back to requestor if response expected
    if (needsResponse) {
        // access already turned the packet into a response
        assert(pkt->isResponse());
        // Here we pay for xbar additional delay and to process the payload
        // of the packet.
        Tick time = curTick() + pkt->headerDelay + pkt->payloadDelay;
        // Reset the timings of the packet
        pkt->headerDelay = pkt->payloadDelay = 0;

        DPRINTF(DDRWrapper, "Queuing response for address %lld\n",
                pkt->getAddr());

        // queue it to be sent back
        responseQueue.push({pkt, time});

        // if we are not already waiting for a retry, or are scheduled
        // to send a response, schedule an event
        if (!retryResp && !sendResponseEvent.scheduled())
            schedule(sendResponseEvent, time);
    } else {
        // queue the packet for deletion
        pendingDelete.reset(pkt);
    }
}

void DDRWrapper::readComplete(unsigned id, uint64_t addr)
{

    DPRINTF(DDRWrapper, "Read to address %lx complete\n", addr);

    // get the outstanding reads for the address in question
    auto p = outstandingReads.find(addr);
    assert(p != outstandingReads.end());

    // first in first out, which is not necessarily true, but it is
    // the best we can do at this point
    PacketPtr pkt = p->second.front();
    p->second.pop();

    if (p->second.empty())
        outstandingReads.erase(p);

    // no need to check for drain here as the next call will add a
    // response to the response queue straight away
    assert(nbrOutstandingReads != 0);
    --nbrOutstandingReads;

    // perform the actual memory access
    accessAndRespond(pkt);
}

void DDRWrapper::writeComplete(unsigned id, uint64_t addr)
{

    DPRINTF(DDRWrapper, "Write to address %lld complete\n", addr);

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
    if (if_name != "port") {
        return ClockedObject::getPort(if_name, idx);
    } else {
        return port;
    }
}

DrainState
DDRWrapper::drain()
{
    // check our outstanding reads and writes and if any they need to
    // drain
    return nbrOutstanding() != 0 ? DrainState::Draining : DrainState::Drained;
}

// DDRWrapper::MemoryPort::MemoryPort(const std::string& _name,
//                                  DDRWrapper& _memory)
//     : ResponsePort(_name, &_memory), mem(_memory)
// { }

// AddrRangeList
// DDRWrapper::MemoryPort::getAddrRanges() const
// {
//     AddrRangeList ranges;
//     ranges.push_back(mem.getAddrRange());
//     return ranges;
// }

// Tick
// DDRWrapper::MemoryPort::recvAtomic(PacketPtr pkt)
// {
//     return mem.recvAtomic(pkt);
// }

// void
// DDRWrapper::MemoryPort::recvFunctional(PacketPtr pkt)
// {
//     mem.recvFunctional(pkt);
// }

// bool
// DDRWrapper::MemoryPort::recvTimingReq(PacketPtr pkt)
// {
//     // pass it to the memory controller
//     return mem.recvTimingReq(pkt);
// }

// void
// DDRWrapper::MemoryPort::recvRespRetry()
// {
//     mem.recvRespRetry();
// }
}
}