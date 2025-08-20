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

#ifndef __CPU_SIMPLE_TRACE_HH__
#define __CPU_SIMPLE_TRACE_HH__

#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "base/statistics.hh"
#include "mem/port.hh"
#include "mem/ruby/slicc_interface/AbstractController.hh"
#include "mem/ruby/structures/SharingManager.hh"
#include "mem/ruby/structures/SharingManagerProxy.hh"
#include "mem/ruby/system/RubyPort.hh"
#include "params/SimpleTrace.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/sim_exit.hh"
#include "sim/sim_object.hh"
#include "sim/stats.hh"

namespace gem5
{

enum TraceCmd
{
    LOAD = 0,
                  STORE,
                  NUM_TRACE_CMDS};

class Packet;
class SimpleTrace : public ClockedObject
{
  public:
    typedef SimpleTraceParams Params;
    SimpleTrace(const Params &p);

    void init() override;

    // main simulation loop (one cycle)
    void tick();

    Port &getPort(const std::string &if_name,
                  PortID idx=InvalidPortID) override;

    /**
     * Print state of address in memory system via PrintReq (for
     * debugging).
     */
    void printAddr(Addr a);

    struct RequestInfo
    {
      Addr addr;
      TraceCmd cmd;
    };

  protected:
    EventFunctionWrapper tickEvent;

    class CpuPort : public RequestPort
    {
        SimpleTrace *tester;

      public:

        CpuPort(const std::string &_name, SimpleTrace *_tester)
            : RequestPort(_name, _tester), tester(_tester)
        { }

      protected:

        virtual bool recvTimingResp(PacketPtr pkt);

        virtual void recvReqRetry();
    };

    CpuPort cachePort;

    class SimpleTraceSenderState : public Packet::SenderState
    {
      public:
        /** Constructor. */
        SimpleTraceSenderState(uint8_t *_data)
            : data(_data)
        { }

        // Hold onto data pointer
        uint8_t *data;
    };

    PacketPtr retryPkt;
    unsigned size;
    int id;

    unsigned blockSizeBits;

    Tick noResponseCycles;

    int numDestinations;
    Tick simCycles;
    int numPacketsMax;
    int numPacketsSent;
    int numTransCompleted;

    const Cycles responseLimit;

    RequestorID requestorId;
    ContextID contextId;

    bool enable;
    Addr defaultAddr;
    bool useTraceFile;
    std::string traceFile;
    std::ifstream traceStream;
    int maxRequests; // Maximum number of requests to send (-1 for unlimited)
    int requestsSent; // Counter for the number of requests sent

    void completeRequest(PacketPtr pkt);

    RequestInfo* getRequestInfo();
    RequestInfo* getRequestInfoTrace();

    void setupSharingManager();

    void generatePkt();
    void sendPkt(PacketPtr pkt);

    void checkCompletion();

    void doRetry();

    friend class MemCompleteEvent;
};

} // namespace gem5

#endif // __CPU_SIMPLE_TRACE_HH__
