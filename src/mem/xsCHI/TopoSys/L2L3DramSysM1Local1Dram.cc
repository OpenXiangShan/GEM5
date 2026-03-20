#include "mem/xsCHI/TopoSys/L2L3DramSysM1Local1Dram.hh"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <memory>
#include <set>
#include <string>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Cache.hh"
#include "mem/xsCHI/base/Network/NodeID.hh"

namespace gem5
{
namespace xsCHI
{
namespace
{

struct ShadowAttachTarget
{
    MeshNode *mesh = nullptr;
    CHIPort *port = nullptr;
    uint32_t localPort = 0;
    uint32_t meshX = 0;
    uint32_t meshY = 0;
    std::string normalized;
};

std::string
trimCopy(const std::string &input)
{
    const auto begin = input.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = input.find_last_not_of(" \t\r\n");
    return input.substr(begin, end - begin + 1);
}

std::string
toLowerCopy(std::string input)
{
    std::transform(input.begin(), input.end(), input.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return input;
}

ShadowAttachTarget
parseShadowAttachPoint(const std::string &rawAttachPoint,
                       MeshNode *mesh0, MeshNode *mesh1,
                       MeshNode *mesh2, MeshNode *mesh3)
{
    const std::string trimmed = trimCopy(rawAttachPoint);
    panic_if(trimmed.empty(), "Shadow attach point is empty");

    const auto dotPos = trimmed.find('.');
    panic_if(dotPos == std::string::npos,
             "Invalid shadow attach point '%s', expected format meshX.localY",
             trimmed.c_str());

    const std::string meshToken = toLowerCopy(trimmed.substr(0, dotPos));
    const std::string localToken = toLowerCopy(trimmed.substr(dotPos + 1));

    MeshNode *mesh = nullptr;
    if (meshToken == "mesh0") {
        mesh = mesh0;
    } else if (meshToken == "mesh1") {
        mesh = mesh1;
    } else if (meshToken == "mesh2") {
        mesh = mesh2;
    } else if (meshToken == "mesh3") {
        mesh = mesh3;
    } else {
        panic("Invalid shadow attach mesh token '%s' in '%s'",
              meshToken.c_str(), trimmed.c_str());
    }

    uint32_t localPort = 0;
    if (localToken == "local0") {
        localPort = 0;
    } else if (localToken == "local1") {
        localPort = 1;
    } else {
        panic("Invalid shadow attach local token '%s' in '%s'",
              localToken.c_str(), trimmed.c_str());
    }

    CHIPort *port = (localPort == 0) ? mesh->getLocal0Port()
                                     : mesh->getLocal1Port();
    panic_if(port == nullptr,
             "Shadow attach point '%s' resolves to null port (mesh local%u)",
             trimmed.c_str(), localPort);

    ShadowAttachTarget target;
    target.mesh = mesh;
    target.port = port;
    target.localPort = localPort;
    target.meshX = mesh->getNodeX();
    target.meshY = mesh->getNodeY();
    target.normalized = meshToken + ".local" + std::to_string(localPort);
    return target;
}

} // namespace

L2L3DramSysM1Local1Dram::L2L3DramSysM1Local1Dram(const Params &p)
    : ClockedObject(p),
      l2wrap(p.L2Wrapper),
      l3(p.L3),
      dram(p.dramsim3),
      Mesh0(p.MeshNode0),
      Mesh1(p.MeshNode1),
      Mesh2(p.MeshNode2),
      Mesh3(p.MeshNode3),
      shadowBridges(p.ShadowRNBridges.begin(), p.ShadowRNBridges.end()),
      shadowAttachPoints(p.shadow_attach_points.begin(),
                         p.shadow_attach_points.end())
{
    panic_if(Mesh0 == nullptr || Mesh1 == nullptr ||
                 Mesh2 == nullptr || Mesh3 == nullptr,
             "L2L3DramSysM1Local1Dram requires MeshNode0/1/2/3");
    panic_if(shadowBridges.size() != shadowAttachPoints.size(),
             "L2L3DramSysM1Local1Dram shadow config length mismatch: bridges=%u "
             "attach_points=%u",
             static_cast<unsigned>(shadowBridges.size()),
             static_cast<unsigned>(shadowAttachPoints.size()));
    panic_if(shadowBridges.size() != l2wrap->getShadowBridges().size(),
             "L2L3DramSysM1Local1Dram shadow bridge mismatch with L2Wrapper: "
             "topo=%u wrapper=%u",
             static_cast<unsigned>(shadowBridges.size()),
             static_cast<unsigned>(l2wrap->getShadowBridges().size()));

    const uint32_t mesh0_x = Mesh0->getNodeX();
    const uint32_t mesh0_y = Mesh0->getNodeY();
    const uint32_t mesh1_x = Mesh1->getNodeX();
    const uint32_t mesh1_y = Mesh1->getNodeY();
    const uint32_t mesh2_x = Mesh2->getNodeX();
    const uint32_t mesh2_y = Mesh2->getNodeY();
    const uint32_t mesh3_x = Mesh3->getNodeX();
    const uint32_t mesh3_y = Mesh3->getNodeY();

    const uint32_t l2Id = NodeID(mesh0_x, mesh0_y, 0).getNodeID();
    const uint32_t l3Id = NodeID(mesh1_x, mesh1_y, 0).getNodeID();
    const uint32_t dramId = NodeID(mesh1_x, mesh1_y, 1).getNodeID();

    auto l2Sam = std::make_shared<SystemAddressMapRN>();
    l2Sam->addNodeID(l3Id);
    l2wrap->setNodeID(l2Id);
    l2wrap->setSAM(l2Sam);

    dram->setNodeID(dramId);

    auto hnfSam = std::make_shared<SystemAddressMapHN>();
    hnfSam->addNodeID(dramId);
    l3->setNodeID(l3Id);
    l3->setSAM(hnfSam);

    assert(l2wrap->getCHIPort() != nullptr && Mesh0->getLocal0Port() != nullptr);
    panic_if(Mesh0->getLocal0Port()->isConnected(),
             "L2L3DramSysM1Local1Dram RN attach point Mesh0.local0 is already "
             "connected");
    l2wrap->getCHIPort()->connect(Mesh0->getLocal0Port());

    assert(Mesh0->getEastPort() != nullptr && Mesh1->getWestPort() != nullptr);
    Mesh0->getEastPort()->connect(Mesh1->getWestPort());

    assert(Mesh1->getNorthPort() != nullptr && Mesh2->getSouthPort() != nullptr);
    Mesh1->getNorthPort()->connect(Mesh2->getSouthPort());

    assert(Mesh2->getWestPort() != nullptr && Mesh3->getEastPort() != nullptr);
    Mesh2->getWestPort()->connect(Mesh3->getEastPort());

    assert(Mesh3->getSouthPort() != nullptr && Mesh0->getNorthPort() != nullptr);
    Mesh3->getSouthPort()->connect(Mesh0->getNorthPort());

    assert(Mesh1->getLocal0Port() != nullptr && l3->getNetworkPort() != nullptr);
    panic_if(Mesh1->getLocal0Port()->isConnected(),
             "L2L3DramSysM1Local1Dram HN attach point Mesh1.local0 is already "
             "connected");
    Mesh1->getLocal0Port()->connect(l3->getNetworkPort());

    assert(Mesh1->getLocal1Port() != nullptr && dram->getCHIPort() != nullptr);
    panic_if(Mesh1->getLocal1Port()->isConnected(),
             "L2L3DramSysM1Local1Dram DRAM attach point Mesh1.local1 is already "
             "connected");
    Mesh1->getLocal1Port()->connect(dram->getCHIPort());

    std::set<uint32_t> shadowNodeIds;
    for (size_t i = 0; i < shadowBridges.size(); ++i) {
        CHIBridge *shadowBridge = shadowBridges[i];
        panic_if(shadowBridge == nullptr,
                 "L2L3DramSysM1Local1Dram shadow bridge[%u] is null",
                 static_cast<unsigned>(i));
        panic_if(shadowBridge != l2wrap->getShadowBridges()[i],
                 "L2L3DramSysM1Local1Dram shadow bridge[%u] pointer mismatch "
                 "with L2Wrapper",
                 static_cast<unsigned>(i));

        const ShadowAttachTarget attachTarget =
            parseShadowAttachPoint(shadowAttachPoints[i],
                                   Mesh0, Mesh1, Mesh2, Mesh3);
        panic_if(attachTarget.port->isConnected(),
                 "L2L3DramSysM1Local1Dram shadow[%u] attach point %s is already "
                 "connected",
                 static_cast<unsigned>(i), attachTarget.normalized.c_str());

        CHIPort *shadowPort = shadowBridge->getNetworkPort();
        panic_if(shadowPort == nullptr,
                 "L2L3DramSysM1Local1Dram shadow bridge[%u] has null network "
                 "port",
                 static_cast<unsigned>(i));
        panic_if(shadowPort->isConnected(),
                 "L2L3DramSysM1Local1Dram shadow bridge[%u] network port already "
                 "connected",
                 static_cast<unsigned>(i));
        shadowPort->connect(attachTarget.port);

        const uint32_t shadowNodeId = NodeID(
            attachTarget.meshX, attachTarget.meshY,
            attachTarget.localPort).getNodeID();
        panic_if(shadowNodeIds.count(shadowNodeId) > 0,
                 "L2L3DramSysM1Local1Dram duplicate shadow node_id=%u for "
                 "shadow[%u]",
                 shadowNodeId, static_cast<unsigned>(i));
        shadowNodeIds.insert(shadowNodeId);

        auto shadowSam = std::make_shared<SystemAddressMapRN>();
        shadowSam->addNodeID(l3Id);
        shadowBridge->setNodeID(shadowNodeId);
        shadowBridge->setSAM(shadowSam);

        inform("xsCHI shadow[%u] placement: attach=%s node_id=%u route_to_hn=%u",
               static_cast<unsigned>(i), attachTarget.normalized.c_str(),
               shadowNodeId, l3Id);
    }

    const auto isConnected = [](CHIPort *port) {
        return (port != nullptr) && port->isConnected();
    };

    inform("xsCHI mesh summary: 2x2 nodes M0=(%u,%u) M1=(%u,%u) M2=(%u,%u) "
           "M3=(%u,%u)",
           mesh0_x, mesh0_y, mesh1_x, mesh1_y, mesh2_x, mesh2_y,
           mesh3_x, mesh3_y);
    inform("xsCHI endpoint placement: RN@Mesh0.local0 node_id=%u, "
           "HN@Mesh1.local0 node_id=%u, DRAM@Mesh1.local1 node_id=%u",
           l2Id, l3Id, dramId);
    inform("xsCHI shadow summary: count=%u",
           static_cast<unsigned>(shadowBridges.size()));
    inform("xsCHI mesh links: M0.east<->M1.west, M1.north<->M2.south, "
           "M2.west<->M3.east, M3.south<->M0.north");
    inform("xsCHI node[%s] local0=%d local1=%d east=%d west=%d north=%d south=%d",
           Mesh0->name(), isConnected(Mesh0->getLocal0Port()),
           isConnected(Mesh0->getLocal1Port()),
           isConnected(Mesh0->getEastPort()),
           isConnected(Mesh0->getWestPort()),
           isConnected(Mesh0->getNorthPort()),
           isConnected(Mesh0->getSouthPort()));
    inform("xsCHI node[%s] local0=%d local1=%d east=%d west=%d north=%d south=%d",
           Mesh1->name(), isConnected(Mesh1->getLocal0Port()),
           isConnected(Mesh1->getLocal1Port()),
           isConnected(Mesh1->getEastPort()),
           isConnected(Mesh1->getWestPort()),
           isConnected(Mesh1->getNorthPort()),
           isConnected(Mesh1->getSouthPort()));
    inform("xsCHI node[%s] local0=%d local1=%d east=%d west=%d north=%d south=%d",
           Mesh2->name(), isConnected(Mesh2->getLocal0Port()),
           isConnected(Mesh2->getLocal1Port()),
           isConnected(Mesh2->getEastPort()),
           isConnected(Mesh2->getWestPort()),
           isConnected(Mesh2->getNorthPort()),
           isConnected(Mesh2->getSouthPort()));
    inform("xsCHI node[%s] local0=%d local1=%d east=%d west=%d north=%d south=%d",
           Mesh3->name(), isConnected(Mesh3->getLocal0Port()),
           isConnected(Mesh3->getLocal1Port()),
           isConnected(Mesh3->getEastPort()),
           isConnected(Mesh3->getWestPort()),
           isConnected(Mesh3->getNorthPort()),
           isConnected(Mesh3->getSouthPort()));
}

gem5::Port &
L2L3DramSysM1Local1Dram::getPort(const std::string &if_name, PortID idx)
{
    return l2wrap->getPort(if_name, idx);
}

void
L2L3DramSysM1Local1Dram::init()
{
    DPRINTF(Cache, "Init L2-CHI_L3-DRAM(M1.local1) system\n");
}

} // namespace xsCHI
} // namespace gem5
