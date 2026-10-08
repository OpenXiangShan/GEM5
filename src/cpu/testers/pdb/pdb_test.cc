// SPDX-License-Identifier: BSD-3-Clause
#include "cpu/testers/pdb/pdb_test.hh"

#include <algorithm>
#include <sstream>

#include "base/logging.hh"
#include "sim/sim_exit.hh"
#include "sim/system.hh"

namespace gem5
{

PdbTest::PdbTest(const PdbTestParams &p)
    : ClockedObject(p), cpuPort(name() + ".port", *this),
      lowerPort(name() + ".lower_port", *this),
      peerPort(name() + ".peer_port", *this),
      requestor(p.system->getRequestorId(this)),
      sendEvent([this] { send(); }, name() + ".send")
{
    for (auto text : p.actions) {
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream input(text);
        Action a;
        std::string address;
        panic_if(!(input >> a.port >> a.operation >> address >> a.bytes >>
                   a.value >> a.delay), "Invalid PDB test action: %s", text);
        a.address = std::stoull(address, nullptr, 0);
        panic_if(a.address % 64 || a.bytes > 64 || !a.bytes,
                 "PDB test requires an aligned line and 1..64 byte mask");
        actions.push_back(a);
    }
}

Port &
PdbTest::getPort(const std::string &if_name, PortID id)
{
    if (if_name == "port")
        return cpuPort;
    if (if_name == "lower_port")
        return lowerPort;
    if (if_name == "peer_port")
        return peerPort;
    return ClockedObject::getPort(if_name, id);
}

void
PdbTest::startup()
{
    schedule(sendEvent, clockEdge(Cycles(1)));
}

PdbTest::TestPort &
PdbTest::currentPort()
{
    const auto &port = actions.at(next).port;
    if (port == "cpu")
        return cpuPort;
    if (port == "lower")
        return lowerPort;
    panic_if(port != "peer", "Unknown PDB test port %s", port);
    return peerPort;
}

void
PdbTest::send()
{
    if (next == actions.size()) {
        exitSimLoop("PDB directed test passed");
        return;
    }
    const auto &a = actions[next];
    if (!pending) {
        auto req = std::make_shared<Request>(a.address, 64, 0, requestor);
        req->setPC(0x4000);
        MemCmd cmd = MemCmd::ReadReq;
        if (a.operation == "dirty" || a.operation == "dirty_branch") {
            cmd = MemCmd::WritebackDirty;
        } else if (a.operation == "shared_read") {
            cmd = MemCmd::ReadSharedReq;
        } else if (a.operation == "store") {
            cmd = MemCmd::WriteReq;
            std::vector<bool> mask(64, false);
            std::fill_n(mask.begin(), a.bytes, true);
            req->setByteEnable(mask);
        } else {
            panic_if(a.operation != "read", "Unknown PDB operation %s",
                     a.operation);
        }
        pending = new Packet(req, cmd);
        auto *data = new uint8_t[64]();
        if (pending->isWrite())
            std::fill_n(data, 64, a.value);
        pending->dataDynamic(data);
        if (a.operation == "dirty_branch" || a.operation == "shared_read")
            pending->setHasSharers();
        if (a.operation == "store" && a.port == "cpu")
            pending->setDcacheMainPipeSbufferReq();
    }
    const bool response_needed = pending->needsResponse();
    if (!currentPort().sendTimingReq(pending)) {
        waitingRetry = true;
        return;
    }
    waitingRetry = false;
    if (!response_needed) {
        // The receiver owns the writeback packet after acceptance.
        pending = nullptr;
        oracle[a.address].fill(a.value);
        finishAction();
    }
}

bool
PdbTest::response(PacketPtr pkt)
{
    panic_if(pkt != pending || pkt->isError(), "Unexpected PDB test response");
    const auto &a = actions[next];
    if (pkt->isSbufferReplay()) {
        panic_if(++replayCount > 100, "PDB Store replay failed to make progress");
        pkt->cmd = MemCmd::WriteReq;
        pkt->clearSbufferReplay();
        pkt->clearDcacheMainPipeSbufferHit();
        schedule(sendEvent, clockEdge(Cycles(16)));
        return true;
    }
    if (a.operation == "read" || a.operation == "shared_read") {
        const auto &expected = oracle[a.address];
        for (unsigned i = 0; i < 64; ++i) {
            panic_if(pkt->getConstPtr<uint8_t>()[i] != expected[i],
                     "PDB data mismatch action %u addr %#x byte %u: "
                     "got %u expected %u", next, a.address, i,
                     pkt->getConstPtr<uint8_t>()[i], expected[i]);
        }
    } else {
        std::fill_n(oracle[a.address].begin(), a.bytes, a.value);
    }
    delete pkt;
    pending = nullptr;
    replayCount = 0;
    finishAction();
    return true;
}

void
PdbTest::finishAction()
{
    const auto delay = actions[next++].delay;
    schedule(sendEvent, clockEdge(Cycles(std::max(1U, delay))));
}

bool
PdbTest::TestPort::recvTimingResp(PacketPtr pkt)
{
    return tester.response(pkt);
}

void
PdbTest::TestPort::recvReqRetry()
{
    panic_if(!tester.waitingRetry || !tester.pending,
             "Unexpected PDB test request retry");
    tester.send();
}

} // namespace gem5
