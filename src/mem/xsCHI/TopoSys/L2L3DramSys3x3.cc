#include "mem/xsCHI/TopoSys/L2L3DramSys3x3.hh"

#include <algorithm>
#include <array>
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

uint32_t
parseMeshIndexToken(const std::string &meshToken, const std::string &rawAttachPoint)
{
    panic_if(meshToken.size() <= 4 || meshToken.substr(0, 4) != "mesh",
             "Invalid shadow attach mesh token '%s' in '%s'",
             meshToken.c_str(), rawAttachPoint.c_str());

    uint32_t meshIndex = 0;
    for (size_t i = 4; i < meshToken.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(meshToken[i]);
        panic_if(!std::isdigit(c),
                 "Invalid shadow attach mesh token '%s' in '%s'",
                 meshToken.c_str(), rawAttachPoint.c_str());
        meshIndex = meshIndex * 10 + static_cast<uint32_t>(c - '0');
    }
    return meshIndex;
}

ShadowAttachTarget
parseShadowAttachPoint(const std::string &rawAttachPoint,
                       const std::array<MeshNode*, 9> &meshes)
{
    const std::string trimmed = trimCopy(rawAttachPoint);
    panic_if(trimmed.empty(), "Shadow attach point is empty");

    const auto dotPos = trimmed.find('.');
    panic_if(dotPos == std::string::npos,
             "Invalid shadow attach point '%s', expected format meshX.localY",
             trimmed.c_str());

    const std::string meshToken = toLowerCopy(trimmed.substr(0, dotPos));
    const std::string localToken = toLowerCopy(trimmed.substr(dotPos + 1));

    const uint32_t meshIndex = parseMeshIndexToken(meshToken, trimmed);
    panic_if(meshIndex >= meshes.size(),
             "Invalid shadow attach mesh token '%s' in '%s': index out of range [0,%u]",
             meshToken.c_str(), trimmed.c_str(),
             static_cast<unsigned>(meshes.size() - 1));

    MeshNode *mesh = meshes[meshIndex];
    panic_if(mesh == nullptr,
             "Shadow attach point '%s' refers to null mesh%u",
             trimmed.c_str(), meshIndex);

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
    target.normalized = "mesh" + std::to_string(meshIndex) +
                        ".local" + std::to_string(localPort);
    return target;
}

} // namespace

L2L3DramSys3x3::L2L3DramSys3x3(const Params &p)
    : ClockedObject(p),
      l2wrap(p.L2Wrapper),
      l3(p.L3),
      dram(p.dramsim3),
      Mesh0(p.MeshNode0),
      Mesh1(p.MeshNode1),
      Mesh2(p.MeshNode2),
      Mesh3(p.MeshNode3),
      Mesh4(p.MeshNode4),
      Mesh5(p.MeshNode5),
      Mesh6(p.MeshNode6),
      Mesh7(p.MeshNode7),
      Mesh8(p.MeshNode8),
      shadowBridges(p.ShadowRNBridges.begin(), p.ShadowRNBridges.end()),
      shadowAttachPoints(p.shadow_attach_points.begin(),
                         p.shadow_attach_points.end())
{
    panic_if(Mesh0 == nullptr || Mesh1 == nullptr || Mesh2 == nullptr ||
                 Mesh3 == nullptr || Mesh4 == nullptr || Mesh5 == nullptr ||
                 Mesh6 == nullptr || Mesh7 == nullptr || Mesh8 == nullptr,
             "L2L3DramSys3x3 requires MeshNode0~8");
    panic_if(shadowBridges.size() != shadowAttachPoints.size(),
             "L2L3DramSys3x3 shadow config length mismatch: bridges=%u "
             "attach_points=%u",
             static_cast<unsigned>(shadowBridges.size()),
             static_cast<unsigned>(shadowAttachPoints.size()));
    panic_if(shadowBridges.size() != l2wrap->getShadowBridges().size(),
             "L2L3DramSys3x3 shadow bridge mismatch with L2Wrapper: topo=%u "
             "wrapper=%u",
             static_cast<unsigned>(shadowBridges.size()),
             static_cast<unsigned>(l2wrap->getShadowBridges().size()));

    const std::array<MeshNode*, 9> meshes = {
        Mesh0, Mesh1, Mesh2, Mesh3, Mesh4, Mesh5, Mesh6, Mesh7, Mesh8};

    const uint32_t l2Id = NodeID(Mesh0->getNodeX(), Mesh0->getNodeY(), 0).getNodeID();
    const uint32_t l3Id = NodeID(Mesh4->getNodeX(), Mesh4->getNodeY(), 0).getNodeID();
    const uint32_t dramId = NodeID(Mesh4->getNodeX(), Mesh4->getNodeY(), 1).getNodeID();

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
             "L2L3DramSys3x3 RN attach point Mesh0.local0 is already connected");
    l2wrap->getCHIPort()->connect(Mesh0->getLocal0Port());

    assert(Mesh0->getEastPort() != nullptr && Mesh1->getWestPort() != nullptr);
    Mesh0->getEastPort()->connect(Mesh1->getWestPort());
    assert(Mesh1->getEastPort() != nullptr && Mesh2->getWestPort() != nullptr);
    Mesh1->getEastPort()->connect(Mesh2->getWestPort());

    assert(Mesh3->getEastPort() != nullptr && Mesh4->getWestPort() != nullptr);
    Mesh3->getEastPort()->connect(Mesh4->getWestPort());
    assert(Mesh4->getEastPort() != nullptr && Mesh5->getWestPort() != nullptr);
    Mesh4->getEastPort()->connect(Mesh5->getWestPort());

    assert(Mesh6->getEastPort() != nullptr && Mesh7->getWestPort() != nullptr);
    Mesh6->getEastPort()->connect(Mesh7->getWestPort());
    assert(Mesh7->getEastPort() != nullptr && Mesh8->getWestPort() != nullptr);
    Mesh7->getEastPort()->connect(Mesh8->getWestPort());

    assert(Mesh0->getNorthPort() != nullptr && Mesh3->getSouthPort() != nullptr);
    Mesh0->getNorthPort()->connect(Mesh3->getSouthPort());
    assert(Mesh3->getNorthPort() != nullptr && Mesh6->getSouthPort() != nullptr);
    Mesh3->getNorthPort()->connect(Mesh6->getSouthPort());

    assert(Mesh1->getNorthPort() != nullptr && Mesh4->getSouthPort() != nullptr);
    Mesh1->getNorthPort()->connect(Mesh4->getSouthPort());
    assert(Mesh4->getNorthPort() != nullptr && Mesh7->getSouthPort() != nullptr);
    Mesh4->getNorthPort()->connect(Mesh7->getSouthPort());

    assert(Mesh2->getNorthPort() != nullptr && Mesh5->getSouthPort() != nullptr);
    Mesh2->getNorthPort()->connect(Mesh5->getSouthPort());
    assert(Mesh5->getNorthPort() != nullptr && Mesh8->getSouthPort() != nullptr);
    Mesh5->getNorthPort()->connect(Mesh8->getSouthPort());

    assert(Mesh4->getLocal0Port() != nullptr && l3->getNetworkPort() != nullptr);
    panic_if(Mesh4->getLocal0Port()->isConnected(),
             "L2L3DramSys3x3 HN attach point Mesh4.local0 is already connected");
    Mesh4->getLocal0Port()->connect(l3->getNetworkPort());

    assert(Mesh4->getLocal1Port() != nullptr && dram->getCHIPort() != nullptr);
    panic_if(Mesh4->getLocal1Port()->isConnected(),
             "L2L3DramSys3x3 DRAM attach point Mesh4.local1 is already connected");
    Mesh4->getLocal1Port()->connect(dram->getCHIPort());

    std::set<uint32_t> shadowNodeIds;
    for (size_t i = 0; i < shadowBridges.size(); ++i) {
        CHIBridge *shadowBridge = shadowBridges[i];
        panic_if(shadowBridge == nullptr,
                 "L2L3DramSys3x3 shadow bridge[%u] is null",
                 static_cast<unsigned>(i));
        panic_if(shadowBridge != l2wrap->getShadowBridges()[i],
                 "L2L3DramSys3x3 shadow bridge[%u] pointer mismatch with L2Wrapper",
                 static_cast<unsigned>(i));

        const ShadowAttachTarget attachTarget =
            parseShadowAttachPoint(shadowAttachPoints[i], meshes);
        panic_if(attachTarget.port->isConnected(),
                 "L2L3DramSys3x3 shadow[%u] attach point %s is already connected",
                 static_cast<unsigned>(i), attachTarget.normalized.c_str());

        CHIPort *shadowPort = shadowBridge->getNetworkPort();
        panic_if(shadowPort == nullptr,
                 "L2L3DramSys3x3 shadow bridge[%u] has null network port",
                 static_cast<unsigned>(i));
        panic_if(shadowPort->isConnected(),
                 "L2L3DramSys3x3 shadow bridge[%u] network port already connected",
                 static_cast<unsigned>(i));
        shadowPort->connect(attachTarget.port);

        const uint32_t shadowNodeId = NodeID(
            attachTarget.meshX, attachTarget.meshY,
            attachTarget.localPort).getNodeID();
        panic_if(shadowNodeIds.count(shadowNodeId) > 0,
                 "L2L3DramSys3x3 duplicate shadow node_id=%u for shadow[%u]",
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

    inform("xsCHI mesh summary: 3x3 nodes "
           "M0=(%u,%u) M1=(%u,%u) M2=(%u,%u) "
           "M3=(%u,%u) M4=(%u,%u) M5=(%u,%u) "
           "M6=(%u,%u) M7=(%u,%u) M8=(%u,%u)",
           Mesh0->getNodeX(), Mesh0->getNodeY(),
           Mesh1->getNodeX(), Mesh1->getNodeY(),
           Mesh2->getNodeX(), Mesh2->getNodeY(),
           Mesh3->getNodeX(), Mesh3->getNodeY(),
           Mesh4->getNodeX(), Mesh4->getNodeY(),
           Mesh5->getNodeX(), Mesh5->getNodeY(),
           Mesh6->getNodeX(), Mesh6->getNodeY(),
           Mesh7->getNodeX(), Mesh7->getNodeY(),
           Mesh8->getNodeX(), Mesh8->getNodeY());
    inform("xsCHI endpoint placement: RN@Mesh0.local0 node_id=%u, "
           "HN@Mesh4.local0 node_id=%u, DRAM@Mesh4.local1 node_id=%u",
           l2Id, l3Id, dramId);
    inform("xsCHI shadow summary: count=%u",
           static_cast<unsigned>(shadowBridges.size()));
    inform("xsCHI mesh links: "
           "M0.east<->M1.west, M1.east<->M2.west, "
           "M3.east<->M4.west, M4.east<->M5.west, "
           "M6.east<->M7.west, M7.east<->M8.west, "
           "M0.north<->M3.south, M3.north<->M6.south, "
           "M1.north<->M4.south, M4.north<->M7.south, "
           "M2.north<->M5.south, M5.north<->M8.south");

    for (size_t i = 0; i < meshes.size(); ++i) {
        MeshNode *mesh = meshes[i];
        inform("xsCHI node[M%u:%s] local0=%d local1=%d east=%d west=%d north=%d south=%d",
               static_cast<unsigned>(i), mesh->name(),
               isConnected(mesh->getLocal0Port()),
               isConnected(mesh->getLocal1Port()),
               isConnected(mesh->getEastPort()),
               isConnected(mesh->getWestPort()),
               isConnected(mesh->getNorthPort()),
               isConnected(mesh->getSouthPort()));
    }
}

gem5::Port &
L2L3DramSys3x3::getPort(const std::string &if_name, PortID idx)
{
    return l2wrap->getPort(if_name, idx);
}

void
L2L3DramSys3x3::init()
{
    DPRINTF(Cache, "Init L2-CHI_L3-DRAM(3x3) system\n");
}

} // namespace xsCHI
} // namespace gem5
