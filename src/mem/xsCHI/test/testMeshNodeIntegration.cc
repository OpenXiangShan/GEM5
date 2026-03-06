#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "base/debug.hh"
#include "mem/xsCHI/base/CHIPort.hh"
#include "mem/xsCHI/base/FlitOpType.hh"
#include "mem/xsCHI/base/Network/NodeID.hh"
#include "mem/xsCHI/base/flit.hh"
#include "mem/xsCHI/device/MeshNode.hh"
#include "params/CHIPort.hh"
#include "params/MeshNode.hh"
#include "params/SimObject.hh"
#include "params/SrcClockDomain.hh"
#include "params/VoltageDomain.hh"
#include "sim/clock_domain.hh"
#include "sim/eventq.hh"
#include "sim/power/power_model.hh"
#include "sim/root.hh"
#include "sim/sim_object.hh"
#include "sim/voltage_domain.hh"

using namespace gem5;
using namespace gem5::xsCHI;

namespace gem5
{

Root *Root::_root = nullptr;

void
PowerModel::setClockedObject(ClockedObject *clkobj)
{
    clocked_object = clkobj;
    for (auto &pms: states_pm) {
        pms->setClockedObject(clkobj);
    }
}

} // namespace gem5

namespace
{

class EndpointOwner : public SimObject
{
  public:
    explicit EndpointOwner(const SimObjectParams &p) : SimObject(p) {}
};

struct FlitMeta
{
    CHI_OP_TYPE opcode = CHI_OP_TYPE::CHI_REQ_OP_START;
    Flit::CHI_CHN_TYPE channel = Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ;
    uint32_t src = 0;
    uint32_t tgt = 0;
    uint64_t txn = 0;
    uint64_t dbid = 0;
    uint64_t addr = 0;
    uint32_t size = 0;
    uint16_t dataId = 0;
    bool hasData = false;
    uint8_t firstDataByte = 0;
};

struct DecodedNode
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t port = 0;
};

const char*
routeDecisionToString(MeshNode::RouteDecision decision)
{
    switch (decision) {
      case MeshNode::RouteDecision::Local0:
        return "Local0";
      case MeshNode::RouteDecision::Local1:
        return "Local1";
      case MeshNode::RouteDecision::East:
        return "East";
      case MeshNode::RouteDecision::West:
        return "West";
      case MeshNode::RouteDecision::North:
        return "North";
      case MeshNode::RouteDecision::South:
        return "South";
    }
    return "Invalid";
}

const char*
channelToString(Flit::CHI_CHN_TYPE channel)
{
    switch (channel) {
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ:
        return "REQ";
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_SNP:
        return "SNP";
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP:
        return "RSP";
      case Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA:
        return "DAT";
      default:
        return "UNKNOWN";
    }
}

DecodedNode
decodeNodeId(uint32_t nodeId)
{
    NodeID parser(0, 0, 0);
    const NodeID decoded = parser.createFromNodeID(nodeId);
    return DecodedNode{decoded.getXCoord(), decoded.getYCoord(),
                       decoded.getPort()};
}

std::string
nodeIdToString(uint32_t nodeId)
{
    const auto decoded = decodeNodeId(nodeId);
    std::ostringstream os;
    os << "(x=" << decoded.x << ", y=" << decoded.y << ", local"
       << decoded.port << ", raw=" << nodeId << ")";
    return os.str();
}

FlitMeta
captureFlitMeta(Flit &flit)
{
    FlitMeta meta;
    meta.opcode = flit.getOpcode();
    meta.channel = flit.get_Flit_Channel_Type();
    meta.src = flit.getSrcId();
    meta.tgt = flit.getTgtId();
    meta.txn = flit.getTxnId();
    meta.dbid = flit.getDbid();
    meta.addr = flit.getAddr();
    meta.size = flit.getSize();
    meta.dataId = flit.getDataId();
    meta.hasData = flit.DataValid();
    if (meta.hasData && meta.size > 0) {
        std::vector<uint8_t> payload(meta.size, 0);
        flit.getData(payload.data());
        meta.firstDataByte = payload[0];
    }
    return meta;
}

std::string
flitMetaToString(const FlitMeta &meta)
{
    std::ostringstream os;
    os << "op=" << CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(meta.opcode)
       << ", chn=" << channelToString(meta.channel)
       << ", src=" << nodeIdToString(meta.src)
       << ", tgt=" << nodeIdToString(meta.tgt)
       << ", txn=" << meta.txn
       << ", dbid=" << meta.dbid
       << ", addr=0x" << std::hex << meta.addr << std::dec
       << ", size=" << meta.size
       << ", data_id=" << meta.dataId
       << ", payload=";
    if (meta.hasData) {
        os << "yes(first=0x" << std::hex
           << static_cast<unsigned>(meta.firstDataByte) << std::dec << ")";
    } else {
        os << "no";
    }
    return os.str();
}

class MeshNodeIntegrationTest : public ::testing::Test
{
  protected:
    static constexpr size_t kMaxEventSteps = 256;

    EventQueue *eventQueue = nullptr;

    std::unique_ptr<VoltageDomain> voltageDomain;
    std::unique_ptr<SrcClockDomain> clockDomain;

    std::unique_ptr<EndpointOwner> rnOwner;
    std::unique_ptr<EndpointOwner> hnOwner;

    std::unique_ptr<CHIPort> rnPort;
    std::unique_ptr<CHIPort> hnPort;
    std::unique_ptr<CHIPort> mesh0Local0;
    std::unique_ptr<CHIPort> mesh0East;
    std::unique_ptr<CHIPort> mesh1West;
    std::unique_ptr<CHIPort> mesh1Local0;

    std::unique_ptr<MeshNode> mesh0;
    std::unique_ptr<MeshNode> mesh1;

    std::vector<FlitMeta> rnRx;
    std::vector<FlitMeta> hnRx;

    size_t expectedRnRx = 0;
    size_t expectedHnRx = 0;

    uint32_t rnNodeId = 0;
    uint32_t hnNodeId = 0;

    void SetUp() override
    {
        eventQueue = getEventQueue(0);
        curEventQueue(eventQueue);
        // Keep initial cycle > 0 so the first flit send is not blocked by
        // per-channel "one send per cycle" last-send initialization.
        eventQueue->setCurTick(1);

        rnNodeId = NodeID(0, 0, 0).getNodeID();
        hnNodeId = NodeID(1, 0, 0).getNodeID();

        buildRuntimeObjects();
        buildTopologyAndCallbacks();

        std::cout << "\n[MeshIntegration][SetUp] RN=" << nodeIdToString(rnNodeId)
                  << ", HN=" << nodeIdToString(hnNodeId) << std::endl;
        std::cout << "[MeshIntegration][SetUp] Topology: RN_EP <-> Mesh0(local0)"
                  << " <-> Mesh0(east)-Mesh1(west) <-> Mesh1(local0) <-> HN_EP"
                  << std::endl;
    }

    void TearDown() override
    {
        // Drain any residual scheduled events before checking leakage.
        size_t drainSteps = 0;
        while (!eventQueue->empty() && drainSteps < kMaxEventSteps) {
            eventQueue->serviceOne();
            ++drainSteps;
        }
        EXPECT_TRUE(eventQueue->empty())
            << "Event queue is not empty after test teardown, drained steps="
            << drainSteps;

        // Fixture-level "NoUnexpectedFlitLeak" check.
        EXPECT_EQ(rnRx.size(), expectedRnRx)
            << "Unexpected flits received at RN endpoint";
        EXPECT_EQ(hnRx.size(), expectedHnRx)
            << "Unexpected flits received at HN endpoint";

        std::cout << "[MeshIntegration][TearDown] rn_rx=" << rnRx.size()
                  << ", hn_rx=" << hnRx.size()
                  << ", drain_steps=" << drainSteps << std::endl;
    }

    void buildRuntimeObjects()
    {
        VoltageDomainParams voltageParams;
        voltageParams.name = "mesh_it_voltage";
        voltageParams.eventq_index = 0;
        voltageParams.voltage = {1.0};
        voltageDomain = std::make_unique<VoltageDomain>(voltageParams);

        SrcClockDomainParams clockParams;
        clockParams.name = "mesh_it_clock";
        clockParams.eventq_index = 0;
        // 1ns period at 1THz tick base.
        clockParams.clock = {1000};
        clockParams.domain_id = 0;
        clockParams.init_perf_level = 0;
        clockParams.voltage_domain = voltageDomain.get();
        clockDomain = std::make_unique<SrcClockDomain>(clockParams);

        SimObjectParams rnOwnerParams;
        rnOwnerParams.name = "mesh_it_rn_owner";
        rnOwnerParams.eventq_index = 0;
        rnOwner = std::make_unique<EndpointOwner>(rnOwnerParams);

        SimObjectParams hnOwnerParams;
        hnOwnerParams.name = "mesh_it_hn_owner";
        hnOwnerParams.eventq_index = 0;
        hnOwner = std::make_unique<EndpointOwner>(hnOwnerParams);

        rnPort = createPort("mesh_it_rn_port");
        hnPort = createPort("mesh_it_hn_port");
        mesh0Local0 = createPort("mesh_it_mesh0_local0");
        mesh0East = createPort("mesh_it_mesh0_east");
        mesh1West = createPort("mesh_it_mesh1_west");
        mesh1Local0 = createPort("mesh_it_mesh1_local0");

        mesh0 = createMeshNode("mesh_it_mesh0", 0, 0,
                               mesh0Local0.get(), mesh0East.get(),
                               nullptr, nullptr, nullptr, nullptr, 8);
        mesh1 = createMeshNode("mesh_it_mesh1", 1, 0,
                               mesh1Local0.get(), nullptr,
                               mesh1West.get(), nullptr, nullptr, nullptr, 8);
    }

    void buildTopologyAndCallbacks()
    {
        rnPort->setOwner(rnOwner.get());
        hnPort->setOwner(hnOwner.get());

        rnPort->setReceiveCallback([this](FlitPtr &flit) {
            const auto meta = captureFlitMeta(*flit);
            std::cout << "[EndpointRx][RN ] tick=" << eventQueue->getCurTick()
                      << " " << flitMetaToString(meta) << std::endl;
            rnRx.push_back(meta);
            return true;
        });
        hnPort->setReceiveCallback([this](FlitPtr &flit) {
            const auto meta = captureFlitMeta(*flit);
            std::cout << "[EndpointRx][HN ] tick=" << eventQueue->getCurTick()
                      << " " << flitMetaToString(meta) << std::endl;
            hnRx.push_back(meta);
            return true;
        });

        rnPort->connect(mesh0Local0.get());
        mesh0East->connect(mesh1West.get());
        mesh1Local0->connect(hnPort.get());

        mesh0->init();
        mesh1->init();
    }

    std::unique_ptr<CHIPort> createPort(const std::string &name)
    {
        CHIPortParams params;
        params.name = name;
        params.eventq_index = 0;
        params.clk_domain = clockDomain.get();
        params.power_state = nullptr;
        params.power_model = {};
        params.recv_buffer_size = 4;
        return std::make_unique<CHIPort>(params);
    }

    std::unique_ptr<MeshNode> createMeshNode(
        const std::string &name, uint32_t x, uint32_t y,
        CHIPort *local0, CHIPort *east, CHIPort *west,
        CHIPort *north, CHIPort *south, CHIPort *local1,
        uint32_t voqDepth)
    {
        MeshNodeParams params;
        params.name = name;
        params.eventq_index = 0;
        params.clk_domain = clockDomain.get();
        params.power_state = nullptr;
        params.power_model = {};
        params.node_x = x;
        params.node_y = y;
        params.port_local0 = local0;
        params.port_local1 = local1;
        params.port_east = east;
        params.port_west = west;
        params.port_north = north;
        params.port_south = south;
        params.voq_depth = voqDepth;
        return std::make_unique<MeshNode>(params);
    }

    bool sendReqFromRn(uint64_t addr, uint64_t txnId, uint32_t size)
    {
        FlitPtr req = std::make_unique<Flit>(
            CHI_OP_TYPE::CHI_REQ_READNOSNP, addr, size);
        req->setOpcode(CHI_OP_TYPE::CHI_REQ_READNOSNP);
        req->setSrcId(rnNodeId);
        req->setTgtId(hnNodeId);
        req->setTxnId(txnId);
        const auto meta = captureFlitMeta(*req);
        std::cout << "[EndpointTx][RN ] tick=" << eventQueue->getCurTick()
                  << " " << flitMetaToString(meta) << std::endl;
        const bool ok = rnPort->send(req);
        std::cout << "[EndpointTx][RN ] send_result="
                  << (ok ? "SUCCESS" : "BLOCKED") << std::endl;
        return ok;
    }

    bool sendRspFromHn(uint64_t txnId)
    {
        FlitPtr rsp = std::make_unique<Flit>();
        rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMP);
        rsp->setSrcId(hnNodeId);
        rsp->setTgtId(rnNodeId);
        rsp->setTxnId(txnId);
        const auto meta = captureFlitMeta(*rsp);
        std::cout << "[EndpointTx][HN ] tick=" << eventQueue->getCurTick()
                  << " " << flitMetaToString(meta) << std::endl;
        const bool ok = hnPort->send(rsp);
        std::cout << "[EndpointTx][HN ] send_result="
                  << (ok ? "SUCCESS" : "BLOCKED") << std::endl;
        return ok;
    }

    bool sendDatCompFromHn(uint64_t addr, uint64_t txnId, uint32_t size,
                           uint64_t dbid, uint16_t dataId, uint8_t fillByte)
    {
        FlitPtr dat = std::make_unique<Flit>(
            CHI_OP_TYPE::CHI_DAT_COMPDATA, addr, size);
        dat->setOpcode(CHI_OP_TYPE::CHI_DAT_COMPDATA);
        dat->setSrcId(hnNodeId);
        dat->setTgtId(rnNodeId);
        dat->setTxnId(txnId);
        dat->setDbid(dbid);
        dat->setDataId(dataId);

        std::vector<uint8_t> payload(size, fillByte);
        dat->setData(payload.data());

        const auto meta = captureFlitMeta(*dat);
        std::cout << "[EndpointTx][HN ] tick=" << eventQueue->getCurTick()
                  << " " << flitMetaToString(meta) << std::endl;
        const bool ok = hnPort->send(dat);
        std::cout << "[EndpointTx][HN ] send_result="
                  << (ok ? "SUCCESS" : "BLOCKED") << std::endl;
        return ok;
    }

    bool sendDbidRespFromHn(uint64_t txnId, uint64_t dbid)
    {
        FlitPtr rsp = std::make_unique<Flit>();
        rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_DBIDRESP);
        rsp->setSrcId(hnNodeId);
        rsp->setTgtId(rnNodeId);
        rsp->setTxnId(txnId);
        rsp->setDbid(dbid);

        const auto meta = captureFlitMeta(*rsp);
        std::cout << "[EndpointTx][HN ] tick=" << eventQueue->getCurTick()
                  << " " << flitMetaToString(meta) << std::endl;
        const bool ok = hnPort->send(rsp);
        std::cout << "[EndpointTx][HN ] send_result="
                  << (ok ? "SUCCESS" : "BLOCKED") << std::endl;
        return ok;
    }

    bool sendCopybackDataFromRn(uint64_t addr, uint64_t txnId, uint32_t size,
                                uint64_t dbid, uint16_t dataId,
                                uint8_t fillByte)
    {
        FlitPtr dat = std::make_unique<Flit>(
            CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA, addr, size);
        dat->setOpcode(CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA);
        dat->setSrcId(rnNodeId);
        dat->setTgtId(hnNodeId);
        dat->setTxnId(txnId);
        dat->setDbid(dbid);
        dat->setDataId(dataId);

        std::vector<uint8_t> payload(size, fillByte);
        dat->setData(payload.data());

        const auto meta = captureFlitMeta(*dat);
        std::cout << "[EndpointTx][RN ] tick=" << eventQueue->getCurTick()
                  << " " << flitMetaToString(meta) << std::endl;
        const bool ok = rnPort->send(dat);
        std::cout << "[EndpointTx][RN ] send_result="
                  << (ok ? "SUCCESS" : "BLOCKED") << std::endl;
        return ok;
    }

    bool sendCompDbidRespFromHn(uint64_t txnId, uint64_t dbid)
    {
        FlitPtr rsp = std::make_unique<Flit>();
        rsp->setOpcode(CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP);
        rsp->setSrcId(hnNodeId);
        rsp->setTgtId(rnNodeId);
        rsp->setTxnId(txnId);
        rsp->setDbid(dbid);

        const auto meta = captureFlitMeta(*rsp);
        std::cout << "[EndpointTx][HN ] tick=" << eventQueue->getCurTick()
                  << " " << flitMetaToString(meta) << std::endl;
        const bool ok = hnPort->send(rsp);
        std::cout << "[EndpointTx][HN ] send_result="
                  << (ok ? "SUCCESS" : "BLOCKED") << std::endl;
        return ok;
    }

    void traceExpectedPath(uint32_t srcNodeId, uint32_t tgtNodeId,
                           const std::string &tag) const
    {
        const auto src = decodeNodeId(srcNodeId);
        const auto tgt = decodeNodeId(tgtNodeId);
        uint32_t curX = src.x;
        uint32_t curY = src.y;
        size_t hop = 0;

        std::cout << "[FlitTrace][" << tag << "] src="
                  << nodeIdToString(srcNodeId)
                  << ", tgt=" << nodeIdToString(tgtNodeId) << std::endl;

        while (hop < 32) {
            const auto route = MeshNode::TestApi::routeDecisionXY(
                curX, curY, tgtNodeId);
            std::cout << "  [hop " << hop << "] node=(" << curX << ", " << curY
                      << "), route=" << routeDecisionToString(route);
            switch (route) {
              case MeshNode::RouteDecision::East:
                ++curX;
                std::cout << " -> egress=east -> next=(" << curX << ", "
                          << curY << ")";
                break;
              case MeshNode::RouteDecision::West:
                --curX;
                std::cout << " -> egress=west -> next=(" << curX << ", "
                          << curY << ")";
                break;
              case MeshNode::RouteDecision::North:
                ++curY;
                std::cout << " -> egress=north -> next=(" << curX << ", "
                          << curY << ")";
                break;
              case MeshNode::RouteDecision::South:
                --curY;
                std::cout << " -> egress=south -> next=(" << curX << ", "
                          << curY << ")";
                break;
              case MeshNode::RouteDecision::Local0:
                std::cout << " -> deliver@local0";
                break;
              case MeshNode::RouteDecision::Local1:
                std::cout << " -> deliver@local1";
                break;
            }
            std::cout << std::endl;

            if (route == MeshNode::RouteDecision::Local0 ||
                route == MeshNode::RouteDecision::Local1) {
                break;
            }
            ++hop;
        }

        EXPECT_EQ(curX, tgt.x);
        EXPECT_EQ(curY, tgt.y);
    }

    size_t runEventsUntil(const std::function<bool()> &done, size_t maxSteps,
                          const std::string &phase)
    {
        std::cout << "[EventLoop][" << phase << "] start_tick="
                  << eventQueue->getCurTick() << ", max_steps=" << maxSteps
                  << std::endl;
        size_t steps = 0;
        while (steps < maxSteps && !done()) {
            if (eventQueue->empty()) {
                std::cout << "[EventLoop][" << phase
                          << "] queue empty before condition met" << std::endl;
                break;
            }
            const Tick tickBefore = eventQueue->getCurTick();
            eventQueue->serviceOne();
            const Tick tickAfter = eventQueue->getCurTick();
            ++steps;

            std::cout << "  [step " << steps << "] tick " << tickBefore
                      << " -> " << tickAfter
                      << ", rn_rx=" << rnRx.size()
                      << ", hn_rx=" << hnRx.size()
                      << ", pending=" << (eventQueue->empty() ? "0" : "1+")
                      << std::endl;
        }

        std::cout << "[EventLoop][" << phase << "] done=" << (done() ? "yes" : "no")
                  << ", steps=" << steps
                  << ", end_tick=" << eventQueue->getCurTick() << std::endl;
        return steps;
    }

    void printPhaseSummary(const std::string &phase, const FlitMeta &meta,
                           size_t steps, const std::string &note = "") const
    {
        std::cout << "[PhaseSummary][" << phase << "] PASS"
                  << " | steps=" << steps
                  << ", chn=" << channelToString(meta.channel)
                  << ", op=" << CHI_OP_HELPER::CHI_OP_TYPE_TO_STR(meta.opcode)
                  << ", src=" << nodeIdToString(meta.src)
                  << ", tgt=" << nodeIdToString(meta.tgt)
                  << ", txn=" << meta.txn
                  << ", dbid=" << meta.dbid
                  << ", size=" << meta.size
                  << ", data_id=" << meta.dataId
                  << ", payload=";
        if (meta.hasData) {
            std::cout << "yes(first=0x" << std::hex
                      << static_cast<unsigned>(meta.firstDataByte)
                      << std::dec << ")";
        } else {
            std::cout << "no";
        }

        std::cout << ", rn_rx=" << rnRx.size()
                  << ", hn_rx=" << hnRx.size();
        if (!note.empty()) {
            std::cout << ", note=" << note;
        }
        std::cout << std::endl;
    }
};

TEST_F(MeshNodeIntegrationTest, ReqCrossNodeDelivery)
{
    constexpr uint64_t txnId = 11;
    constexpr uint64_t addr = 0x80000000ULL;
    constexpr uint32_t size = 64;

    expectedHnRx = 1;
    expectedRnRx = 0;

    std::cout << "\n[TestCase] ReqCrossNodeDelivery" << std::endl;
    traceExpectedPath(rnNodeId, hnNodeId, "REQ");

    ASSERT_TRUE(sendReqFromRn(addr, txnId, size));
    const size_t steps = runEventsUntil(
        [this] { return hnRx.size() == 1; }, kMaxEventSteps, "REQ->HN");

    ASSERT_EQ(hnRx.size(), 1u) << "REQ should be delivered to HN endpoint";
    EXPECT_LE(steps, kMaxEventSteps);

    const auto &recv = hnRx.front();
    EXPECT_EQ(recv.channel, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
    EXPECT_EQ(recv.opcode, CHI_OP_TYPE::CHI_REQ_READNOSNP);
    EXPECT_EQ(recv.src, rnNodeId);
    EXPECT_EQ(recv.tgt, hnNodeId);
    EXPECT_EQ(recv.txn, txnId);
    EXPECT_EQ(recv.addr, addr);
    EXPECT_EQ(recv.size, size);

    printPhaseSummary("REQ->HN", recv, steps);
}

TEST_F(MeshNodeIntegrationTest, ReqRspRoundTripAcrossTwoNodes)
{
    constexpr uint64_t txnId = 22;
    constexpr uint64_t addr = 0x81000000ULL;
    constexpr uint32_t size = 64;

    expectedHnRx = 1;
    expectedRnRx = 1;

    std::cout << "\n[TestCase] ReqRspRoundTripAcrossTwoNodes" << std::endl;
    traceExpectedPath(rnNodeId, hnNodeId, "REQ");

    ASSERT_TRUE(sendReqFromRn(addr, txnId, size));
    const size_t reqSteps = runEventsUntil(
        [this] { return hnRx.size() == 1; }, kMaxEventSteps, "REQ->HN");
    ASSERT_EQ(hnRx.size(), 1u) << "REQ should first arrive at HN endpoint";
    EXPECT_LE(reqSteps, kMaxEventSteps);

    traceExpectedPath(hnNodeId, rnNodeId, "RSP");
    ASSERT_TRUE(sendRspFromHn(txnId));
    const size_t rspSteps = runEventsUntil(
        [this] { return rnRx.size() == 1; }, kMaxEventSteps, "RSP->RN");
    ASSERT_EQ(rnRx.size(), 1u) << "RSP should return to RN endpoint";
    EXPECT_LE(rspSteps, kMaxEventSteps);

    const auto &reqAtHn = hnRx.front();
    EXPECT_EQ(reqAtHn.channel, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_REQ);
    EXPECT_EQ(reqAtHn.opcode, CHI_OP_TYPE::CHI_REQ_READNOSNP);
    EXPECT_EQ(reqAtHn.src, rnNodeId);
    EXPECT_EQ(reqAtHn.tgt, hnNodeId);
    EXPECT_EQ(reqAtHn.txn, txnId);

    const auto &rspAtRn = rnRx.front();
    EXPECT_EQ(rspAtRn.channel, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
    EXPECT_EQ(rspAtRn.opcode, CHI_OP_TYPE::CHI_RSP_COMP);
    EXPECT_EQ(rspAtRn.src, hnNodeId);
    EXPECT_EQ(rspAtRn.tgt, rnNodeId);
    EXPECT_EQ(rspAtRn.txn, txnId);

    printPhaseSummary("REQ->HN", reqAtHn, reqSteps);
    printPhaseSummary("RSP->RN", rspAtRn, rspSteps);
}

TEST_F(MeshNodeIntegrationTest, DatCompCrossNodeDelivery)
{
    constexpr uint64_t txnId = 33;
    constexpr uint64_t dbid = 9;
    constexpr uint64_t addr = 0x82000000ULL;
    constexpr uint32_t size = 64;
    constexpr uint16_t dataId = 0;
    constexpr uint8_t firstByte = 0xAB;

    expectedHnRx = 0;
    expectedRnRx = 1;

    std::cout << "\n[TestCase] DatCompCrossNodeDelivery" << std::endl;
    traceExpectedPath(hnNodeId, rnNodeId, "DAT_COMPDATA");

    ASSERT_TRUE(sendDatCompFromHn(addr, txnId, size, dbid, dataId, firstByte));
    const size_t steps = runEventsUntil(
        [this] { return rnRx.size() == 1; }, kMaxEventSteps, "DAT->RN");

    ASSERT_EQ(rnRx.size(), 1u) << "DAT should be delivered to RN endpoint";
    EXPECT_LE(steps, kMaxEventSteps);

    const auto &recv = rnRx.front();
    EXPECT_EQ(recv.channel, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
    EXPECT_EQ(recv.opcode, CHI_OP_TYPE::CHI_DAT_COMPDATA);
    EXPECT_EQ(recv.src, hnNodeId);
    EXPECT_EQ(recv.tgt, rnNodeId);
    EXPECT_EQ(recv.txn, txnId);
    EXPECT_EQ(recv.dbid, dbid);
    EXPECT_EQ(recv.addr, addr);
    EXPECT_EQ(recv.size, size);
    EXPECT_EQ(recv.dataId, dataId);
    EXPECT_TRUE(recv.hasData);
    EXPECT_EQ(recv.firstDataByte, firstByte);

    printPhaseSummary("DAT->RN", recv, steps);
}

TEST_F(MeshNodeIntegrationTest, CopybackDbidRespDataFlowAcrossTwoNodes)
{
    constexpr uint64_t txnId = 44;
    constexpr uint64_t dbid = 12;
    constexpr uint64_t addr = 0x83000000ULL;
    constexpr uint32_t size = 64;
    constexpr uint16_t dataId = 0;
    constexpr uint8_t firstByte = 0x5A;

    // RN should receive DBIDRESP + COMPDBIDRESP, HN should receive one
    // COPYBACKWRDATA.
    expectedRnRx = 2;
    expectedHnRx = 1;

    std::cout << "\n[TestCase] CopybackDbidRespDataFlowAcrossTwoNodes"
              << std::endl;

    traceExpectedPath(hnNodeId, rnNodeId, "DBIDRESP");
    ASSERT_TRUE(sendDbidRespFromHn(txnId, dbid));
    const size_t dbidRspSteps = runEventsUntil(
        [this] { return rnRx.size() == 1; }, kMaxEventSteps, "DBIDRESP->RN");
    ASSERT_EQ(rnRx.size(), 1u) << "DBIDRESP should arrive at RN endpoint";
    EXPECT_LE(dbidRspSteps, kMaxEventSteps);

    const auto &dbidRspAtRn = rnRx[0];
    EXPECT_EQ(dbidRspAtRn.channel, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
    EXPECT_EQ(dbidRspAtRn.opcode, CHI_OP_TYPE::CHI_RSP_DBIDRESP);
    EXPECT_EQ(dbidRspAtRn.src, hnNodeId);
    EXPECT_EQ(dbidRspAtRn.tgt, rnNodeId);
    EXPECT_EQ(dbidRspAtRn.txn, txnId);
    EXPECT_EQ(dbidRspAtRn.dbid, dbid);
    printPhaseSummary("DBIDRESP->RN", dbidRspAtRn, dbidRspSteps);

    traceExpectedPath(rnNodeId, hnNodeId, "COPYBACKWRDATA");
    ASSERT_TRUE(sendCopybackDataFromRn(addr, txnId, size, dbid, dataId,
                                       firstByte));
    const size_t copybackSteps = runEventsUntil(
        [this] { return hnRx.size() == 1; }, kMaxEventSteps, "COPYBACK->HN");
    ASSERT_EQ(hnRx.size(), 1u) << "COPYBACKWRDATA should arrive at HN endpoint";
    EXPECT_LE(copybackSteps, kMaxEventSteps);

    const auto &copybackAtHn = hnRx[0];
    EXPECT_EQ(copybackAtHn.channel, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_DATA);
    EXPECT_EQ(copybackAtHn.opcode, CHI_OP_TYPE::CHI_DAT_COPYBACKWRDATA);
    EXPECT_EQ(copybackAtHn.src, rnNodeId);
    EXPECT_EQ(copybackAtHn.tgt, hnNodeId);
    EXPECT_EQ(copybackAtHn.txn, txnId);
    EXPECT_EQ(copybackAtHn.dbid, dbid);
    EXPECT_EQ(copybackAtHn.addr, addr);
    EXPECT_EQ(copybackAtHn.size, size);
    EXPECT_EQ(copybackAtHn.dataId, dataId);
    EXPECT_TRUE(copybackAtHn.hasData);
    EXPECT_EQ(copybackAtHn.firstDataByte, firstByte);
    printPhaseSummary("COPYBACK->HN", copybackAtHn, copybackSteps);

    traceExpectedPath(hnNodeId, rnNodeId, "COMPDBIDRESP");
    ASSERT_TRUE(sendCompDbidRespFromHn(txnId, dbid));
    const size_t compSteps = runEventsUntil(
        [this] { return rnRx.size() == 2; }, kMaxEventSteps,
        "COMPDBIDRESP->RN");
    ASSERT_EQ(rnRx.size(), 2u) << "COMPDBIDRESP should arrive at RN endpoint";
    EXPECT_LE(compSteps, kMaxEventSteps);

    const auto &compAtRn = rnRx[1];
    EXPECT_EQ(compAtRn.channel, Flit::CHI_CHN_TYPE::CHI_CHN_TYPE_RSP);
    EXPECT_EQ(compAtRn.opcode, CHI_OP_TYPE::CHI_RSP_COMPDBIDRESP);
    EXPECT_EQ(compAtRn.src, hnNodeId);
    EXPECT_EQ(compAtRn.tgt, rnNodeId);
    EXPECT_EQ(compAtRn.txn, txnId);
    EXPECT_EQ(compAtRn.dbid, dbid);
    printPhaseSummary("COMPDBIDRESP->RN", compAtRn, compSteps);
}

} // namespace

int
main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
