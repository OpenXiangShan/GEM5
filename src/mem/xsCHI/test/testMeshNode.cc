#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "mem/xsCHI/base/Network/NodeID.hh"
#include "mem/xsCHI/device/MeshNode.hh"

using namespace gem5::xsCHI;

namespace
{

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
ingressNameFromIndex(size_t idx)
{
    switch (idx) {
      case 0:
        return "local0";
      case 1:
        return "local1";
      case 2:
        return "east";
      case 3:
        return "west";
      case 4:
        return "north";
      case 5:
        return "south";
      default:
        return "invalid";
    }
}

struct DecodedNode
{
    uint32_t x;
    uint32_t y;
    uint32_t port;
};

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

std::string
pendingToString(const std::array<size_t, 6>& pending)
{
    std::ostringstream os;
    os << "{";
    for (size_t i = 0; i < pending.size(); ++i) {
        if (i != 0) {
            os << ", ";
        }
        os << ingressNameFromIndex(i) << ":" << pending[i];
    }
    os << "}";
    return os.str();
}

void
traceFlitPath(uint32_t srcX, uint32_t srcY, uint32_t tgtId,
              const std::vector<MeshNode::RouteDecision>& expectedPath)
{
    ASSERT_FALSE(expectedPath.empty());

    const auto target = decodeNodeId(tgtId);
    uint32_t curX = srcX;
    uint32_t curY = srcY;

    std::cout << "[FlitTrace] src=(" << srcX << ", " << srcY
              << "), tgt=" << nodeIdToString(tgtId) << std::endl;

    for (size_t cycle = 0; cycle < expectedPath.size(); ++cycle) {
        const auto route = MeshNode::TestApi::routeDecisionXY(curX, curY, tgtId);
        std::cout << "  [cycle " << cycle << "] node=(" << curX << ", " << curY
                  << "), route=" << routeDecisionToString(route);

        EXPECT_EQ(route, expectedPath[cycle]);
        switch (route) {
          case MeshNode::RouteDecision::East:
            ++curX;
            std::cout << " -> egress=east -> next=(" << curX << ", " << curY
                      << ")";
            break;
          case MeshNode::RouteDecision::West:
            --curX;
            std::cout << " -> egress=west -> next=(" << curX << ", " << curY
                      << ")";
            break;
          case MeshNode::RouteDecision::North:
            ++curY;
            std::cout << " -> egress=north -> next=(" << curX << ", " << curY
                      << ")";
            break;
          case MeshNode::RouteDecision::South:
            --curY;
            std::cout << " -> egress=south -> next=(" << curX << ", " << curY
                      << ")";
            break;
          case MeshNode::RouteDecision::Local0:
            std::cout << " -> deliver@local0";
            break;
          case MeshNode::RouteDecision::Local1:
            std::cout << " -> deliver@local1";
            break;
        }
        std::cout << std::endl;
    }

    EXPECT_EQ(curX, target.x);
    EXPECT_EQ(curY, target.y);
    const auto expectedLast = target.port == 0 ? MeshNode::RouteDecision::Local0
                                                : MeshNode::RouteDecision::Local1;
    EXPECT_EQ(expectedPath.back(), expectedLast);
}

} // namespace

TEST(MeshNodeTest, RouteDecisionXY)
{
    const uint32_t n00_p0 = NodeID(0, 0, 0).getNodeID();
    const uint32_t n00_p1 = NodeID(0, 0, 1).getNodeID();
    const uint32_t n10_p0 = NodeID(1, 0, 0).getNodeID();
    const uint32_t n01_p0 = NodeID(0, 1, 0).getNodeID();

    auto expectRoute = [](uint32_t srcX, uint32_t srcY, uint32_t tgtId,
                          MeshNode::RouteDecision expected) {
        const auto got = MeshNode::TestApi::routeDecisionXY(srcX, srcY, tgtId);
        std::cout << "[RouteDecisionXY] src=(" << srcX << ", " << srcY
                  << "), tgt=" << nodeIdToString(tgtId)
                  << " => route=" << routeDecisionToString(got) << std::endl;
        EXPECT_EQ(got, expected);
    };

    expectRoute(0, 0, n10_p0, MeshNode::RouteDecision::East);
    expectRoute(1, 0, n00_p0, MeshNode::RouteDecision::West);
    expectRoute(0, 0, n01_p0, MeshNode::RouteDecision::North);
    expectRoute(0, 1, n00_p0, MeshNode::RouteDecision::South);
    expectRoute(0, 0, n00_p0, MeshNode::RouteDecision::Local0);
    expectRoute(0, 0, n00_p1, MeshNode::RouteDecision::Local1);
}

TEST(MeshNodeTest, BackpressureThreshold)
{
    auto expectBackpressure = [](size_t queueDepth, size_t configuredDepth,
                                 bool expected) {
        const size_t effectiveDepth =
            configuredDepth == 0 ? 1 : configuredDepth;
        const bool got = MeshNode::TestApi::shouldBackpressure(queueDepth,
                                                               configuredDepth);
        std::cout << "[Backpressure] depth=" << queueDepth
                  << ", configured_depth=" << configuredDepth
                  << ", effective_depth=" << effectiveDepth
                  << " => " << (got ? "BLOCK upstream" : "PASS")
                  << std::endl;
        EXPECT_EQ(got, expected);
    };

    // configuredDepth=0 is treated as effective depth 1.
    expectBackpressure(0, 0, false);
    expectBackpressure(1, 0, true);
    expectBackpressure(7, 8, false);
    expectBackpressure(8, 8, true);
}

TEST(MeshNodeTest, RoundRobinSelect)
{
    std::array<size_t, 6> pending = {1, 0, 0, 2, 0, 0};

    auto expectPick = [](const std::array<size_t, 6>& pendingPerIngress,
                         size_t cursor, int expectedPick) {
        const int got = MeshNode::TestApi::selectIngressRR(pendingPerIngress,
                                                            cursor);
        std::cout << "[RoundRobin] cursor=" << cursor
                  << ", pending=" << pendingToString(pendingPerIngress)
                  << " => pick=" << got;
        if (got >= 0) {
            std::cout << " (" << ingressNameFromIndex(static_cast<size_t>(got))
                      << ")";
        }
        std::cout << std::endl;
        EXPECT_EQ(got, expectedPick);
    };

    expectPick(pending, 0, 0);
    // Emulate runtime cursor update after a successful send.
    expectPick(pending, 1, 3);
    expectPick(pending, 4, 0);

    std::array<size_t, 6> empty = {0, 0, 0, 0, 0, 0};
    expectPick(empty, 0, -1);
}

TEST(MeshNodeTest, FlitTransferTraceXY)
{
    std::cout << "[FlitTrace] Case-1: (0,0) -> (2,1,local1)" << std::endl;
    traceFlitPath(0, 0, NodeID(2, 1, 1).getNodeID(),
                  {MeshNode::RouteDecision::East,
                   MeshNode::RouteDecision::East,
                   MeshNode::RouteDecision::North,
                   MeshNode::RouteDecision::Local1});

    std::cout << "[FlitTrace] Case-2: (3,2) -> (1,0,local0)" << std::endl;
    traceFlitPath(3, 2, NodeID(1, 0, 0).getNodeID(),
                  {MeshNode::RouteDecision::West,
                   MeshNode::RouteDecision::West,
                   MeshNode::RouteDecision::South,
                   MeshNode::RouteDecision::South,
                   MeshNode::RouteDecision::Local0});
}

int
main(int argc, char **argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}