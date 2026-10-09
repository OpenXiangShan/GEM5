// SPDX-License-Identifier: BSD-3-Clause
#ifndef __CPU_TESTERS_PDB_TEST_HH__
#define __CPU_TESTERS_PDB_TEST_HH__

#include <array>
#include <map>
#include <string>
#include <vector>

#include "mem/cache/prefetch/queued.hh"
#include "mem/port.hh"
#include "params/PdbTest.hh"
#include "params/PdbTestPrefetcher.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5
{

/** One fixed prefetch through the production queue and MSHR path. */
class PdbTestPrefetcher : public prefetch::Queued
{
  public:
    explicit PdbTestPrefetcher(const PdbTestPrefetcherParams &p)
        : prefetch::Queued(p) {}
    void calculatePrefetch(const PrefetchInfo &pfi,
                          std::vector<AddrPriority> &addresses) override
    {
        if (blockAddress(pfi.getAddr()) == 0x1000) {
            addresses.emplace_back(0x1040, 0,
                                   PrefetchSourceType::SStream);
        }
    }
};

/** Deterministic real-cache coherence test with an independent byte oracle. */
class PdbTest : public ClockedObject
{
  private:
    class TestPort : public RequestPort
    {
      private:
        PdbTest &tester;
      public:
        TestPort(const std::string &name, PdbTest &tester)
            : RequestPort(name), tester(tester) {}
        bool recvTimingResp(PacketPtr pkt) override;
        void recvReqRetry() override;
    };
    struct Action
    {
        std::string port;
        std::string operation;
        Addr address;
        unsigned bytes;
        unsigned value;
        unsigned delay;
    };
    TestPort cpuPort;
    TestPort lowerPort;
    TestPort peerPort;
    const RequestorID requestor;
    std::vector<Action> actions;
    size_t next = 0;
    std::map<Addr, std::array<uint8_t, 64>> oracle;
    PacketPtr pending = nullptr;
    bool waitingRetry = false;
    EventFunctionWrapper sendEvent;

    TestPort &currentPort();
    void send();
    bool response(PacketPtr pkt);
    void finishAction();

  public:
    explicit PdbTest(const PdbTestParams &p);
    Port &getPort(const std::string &name,
                  PortID id = InvalidPortID) override;
    void startup() override;
};

} // namespace gem5
#endif
