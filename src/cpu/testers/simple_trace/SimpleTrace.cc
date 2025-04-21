/*
 * Copyright (c) 2016 Georgia Institute of Technology
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "cpu/testers/simple_trace/SimpleTrace.hh"

#include <cmath>
#include <iomanip>
#include <vector>

#include "base/logging.hh"
#include "base/random.hh"
#include "base/statistics.hh"
#include "debug/SimpleTrace.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "mem/request.hh"
#include "sim/sim_events.hh"
#include "sim/stats.hh"
#include "sim/system.hh"

namespace gem5
{

std::map<int, bool> completionMap;

bool
SimpleTrace::CpuPort::recvTimingResp(PacketPtr pkt)
{
    tester->completeRequest(pkt);
    return true;
}

void
SimpleTrace::CpuPort::recvReqRetry()
{
    tester->doRetry();
}

void
SimpleTrace::sendPkt(PacketPtr pkt)
{
    if (!cachePort.sendTimingReq(pkt)) {
        DPRINTF(SimpleTrace, "Failed to send packet %llx to cache port\n", pkt->req->getPaddr());
        retryPkt = pkt;  // RubyPort will retry sending
    }
    numPacketsSent++;
}

SimpleTrace::SimpleTrace(const Params &p)
    : ClockedObject(p),
      tickEvent([this] { tick(); }, "SimpleTrace tick", false, Event::CPU_Tick_Pri),
      cachePort("SimpleTrace", this),
      retryPkt(NULL),
      numPacketsSent(0),
      responseLimit(p.response_limit),
      requestorId(p.system->getRequestorId(this)),
      contextId(p.id),
      traceFile(p.trace_file)
{

    DPRINTF(SimpleTrace, "Config Created: Name = %s , RequestorID = %d, ContextID = %d\n", name(), requestorId,
            contextId);

    if (traceFile == "") {
        printf("No trace file for %s\n", name().c_str());
        enable = false;
    } else {
        traceStream.open(traceFile);
        traceStream.clear();
        enable = true;
        printf("Trace file %s for %s\n", traceFile.c_str(), name().c_str());
        // Find the line that trace starts
        std::string line;
        while (std::getline(traceStream, line)) {
            if (line.find("!!!!", 0) == 0) {
                break;
            }
        }
        if (traceStream.eof() || traceStream.fail()) {
            fatal("Error finding trace in trace file! %s",
                traceStream.eof() ? "EOF" : "fail");
        }
    }
    // set up counters

    noResponseCycles = 0;

    if (completionMap.find(requestorId) == completionMap.end()) {
        completionMap[requestorId] = !enable;
    } else {
        fatal("Reuse of requestorID %d !!\n", requestorId);
    }
    if (enable) {
        printf("Scheduling for %s\n", name().c_str());
        schedule(tickEvent, 0);
    }
}

Port &
SimpleTrace::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "test")
        return cachePort;
    else
        return ClockedObject::getPort(if_name, idx);
}

void
SimpleTrace::init()
{
    numPacketsSent = 0;
    numTransCompleted = 0;
}


void
SimpleTrace::completeRequest(PacketPtr pkt)
{
    DPRINTF(SimpleTrace, "Completed %s packet for address %llx\n", pkt->isWrite() ? "write" : "read",
            pkt->req->getPaddr());

    assert(pkt->isResponse());
    noResponseCycles = 0;
    delete pkt;
    numTransCompleted++;
    std::cout << "completed " << numTransCompleted << " sent " << numPacketsSent << " enable " << enable <<std::endl;
    if (!enable && numTransCompleted == numPacketsSent) {
        checkCompletion();
    }
}


SimpleTrace::RequestInfo *
SimpleTrace::getRequestInfo()
{
    // Read Trace file
    std::string line;
    std::getline(traceStream, line);
    if (line.find("!!!!", 0) == 0) {
        printf("End of trace file for %s\n", name().c_str());
        return NULL;
    }
    TraceCmd cmd;
    if (line[0] == 'r') {
        cmd = TraceCmd::LOAD;
    } else if (line[0] == 'w') {
        cmd = TraceCmd::STORE;
    } else {
        panic("Invalid trace command %c\n", line[0]);
    }
    uint64_t addr;
    addr = std::stoull(line.substr(2), nullptr, 16);
    return new RequestInfo{addr, cmd};
}

void
SimpleTrace::tick()
{
    if (++noResponseCycles >= responseLimit) {
        fatal("%s deadlocked at tick %d\n", name().c_str(), curTick());
    }
    // printf("Tick @ %s\n", name().c_str());
    // Always generate packet on each cycle
    if (!retryPkt) {
        generatePkt(); // TODO generate more packets per cycle
    }

    if (!enable) {
        return;
    }
    // Schedule wakeup
    if (curTick() >= simCycles)
        exitSimLoop("Simple Trace completed simCycles");
    else {
        if (!tickEvent.scheduled())
            schedule(tickEvent, clockEdge(Cycles(1)));
    }
}

void
SimpleTrace::generatePkt()
{
    MemCmd::Command requestType;

    RequestPtr req = nullptr;
    Request::Flags flags;

    RequestInfo *info = getRequestInfo();
    // Last request
    if (!info) {
        enable = false;
        return;
    }
    switch (info->cmd) {
        case TraceCmd::LOAD:
            requestType = MemCmd::ReadReq;
            break;
        case TraceCmd::STORE:
            requestType = MemCmd::WriteReq;
            break;
        default:
            panic("Invalid commd type");
            break;
    }
    req = std::make_shared<Request>(info->addr, 1, flags, requestorId);
    // Set context id using the id of this generator
    req->setContext(id);

    // No need to do functional simulation
    // We just do timing simulation of the network

    DPRINTF(SimpleTrace, "Generated packet %llx\n", req->getPaddr());

    PacketPtr pkt = new Packet(req, requestType);
    pkt->dataDynamic(new uint8_t[req->getSize()]{0x11});
    pkt->senderState = NULL;

    sendPkt(pkt);
    delete info;
}

void
SimpleTrace::checkCompletion()
{
    completionMap[requestorId] = true;
    DPRINTF(SimpleTrace, "%s completed all requests\n", name());
    // Check if all the requests are completed
    bool all_completed = true;
    for (auto it = completionMap.begin(); it != completionMap.end(); it++) {
        if (!it->second) {
            all_completed = false;
        }
    }
    if (all_completed) {
        printf("All requests completed, last requestor RequestorID is %d ContextID is %d\n", requestorId, contextId);
        exitSimLoop("Simple Trace completed all requests");
    }
}

void
SimpleTrace::doRetry()
{
    DPRINTF(SimpleTrace, "Retrying packet addr %llx to cache port\n", retryPkt->req->getPaddr());
    if (cachePort.sendTimingReq(retryPkt)) {
        retryPkt = NULL;
    }
}

void
SimpleTrace::printAddr(Addr a)
{
    cachePort.printAddr(a);
}

}  // namespace gem5
