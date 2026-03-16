
#include "mem/xsCHI/TopoSys/L2todram.hh"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Cache.hh"
#include "mem/xsCHI/base/Network/NodeID.hh"
#include "params/ClockedObject.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace xsCHI {
namespace
{
struct ShadowAttachTarget
{
    MeshNode *mesh = nullptr;
    CHIPort *port = nullptr;
    // localPort 取值 0/1，对应 MeshNode.local0/local1。
    uint32_t localPort = 0;
    uint32_t meshX = 0;
    uint32_t meshY = 0;
    // 规范化后的挂点字符串（小写），用于日志和报错。
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
    // 输入容错：允许存在前后空白，但最终必须是 meshX.localY 格式。
    const std::string trimmed = trimCopy(rawAttachPoint);
    panic_if(trimmed.empty(),
             "Shadow attach point is empty");

    const auto dotPos = trimmed.find('.');
    panic_if(dotPos == std::string::npos,
             "Invalid shadow attach point '%s', expected format meshX.localY",
             trimmed.c_str());

    const std::string meshToken = toLowerCopy(trimmed.substr(0, dotPos));
    const std::string localToken = toLowerCopy(trimmed.substr(dotPos + 1));

    // 先解析 mesh 编号，再解析 local 口号，错误都在启动阶段直接失败。
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

    L2ToDramSys::L2ToDramSys(const Params &p)
        : ClockedObject(p),
        L2wrap(p.L2Wrapper),
        L3bridge(p.L3),
        Dram(p.dramsim3),
        Mesh0(p.MeshNode0),
        Mesh1(p.MeshNode1),
        Mesh2(p.MeshNode2),
        Mesh3(p.MeshNode3),
        shadowBridges(p.ShadowRNBridges.begin(), p.ShadowRNBridges.end()),
        shadowAttachPoints(p.shadow_attach_points.begin(), p.shadow_attach_points.end())
    {
        panic_if(Mesh0 == nullptr || Mesh1 == nullptr ||
                 Mesh2 == nullptr || Mesh3 == nullptr,
                 "L2ToDramSys requires MeshNode0/1/2/3");
        panic_if(shadowBridges.size() != shadowAttachPoints.size(),
                 "L2ToDramSys shadow config length mismatch: bridges=%zu attach_points=%zu",
                 shadowBridges.size(), shadowAttachPoints.size());
        panic_if(shadowBridges.size() != L2wrap->getShadowBridges().size(),
                 "L2ToDramSys shadow bridge mismatch with L2Wrapper: topo=%zu wrapper=%zu",
                 shadowBridges.size(), L2wrap->getShadowBridges().size());
        // 说明：
        // 这里强制拓扑层与 L2Wrapper 持有同一批 shadow bridge，
        // 防止“L2 镜像到 A 桥，拓扑却接了 B 桥”的隐蔽错配。

        // Derive endpoint IDs from MeshNode topology so NodeID encoding
        // always matches (x, y, local_port) placement.
        const uint32_t mesh0_x = Mesh0->getNodeX();
        const uint32_t mesh0_y = Mesh0->getNodeY();
        const uint32_t mesh1_x = Mesh1->getNodeX();
        const uint32_t mesh1_y = Mesh1->getNodeY();
        const uint32_t mesh2_x = Mesh2->getNodeX();
        const uint32_t mesh2_y = Mesh2->getNodeY();
        const uint32_t mesh3_x = Mesh3->getNodeX();
        const uint32_t mesh3_y = Mesh3->getNodeY();

        const uint32_t L2ID = NodeID(mesh0_x, mesh0_y, 0).getNodeID();
        const uint32_t L3ID = NodeID(mesh1_x, mesh1_y, 0).getNodeID();
        const uint32_t dramID = NodeID(mesh2_x, mesh2_y, 0).getNodeID();

        auto L2SAM = std::make_shared<SystemAddressMapRN>();
        L2SAM->addNodeID(L3ID);
        L2wrap->setNodeID(L2ID);
        L2wrap->setSAM(L2SAM);

        Dram->setNodeID(dramID);

        auto HNF_SAM = std::make_shared<SystemAddressMapHN>();
        HNF_SAM->addNodeID(dramID);
        L3bridge->setNodeID(L3ID);
        L3bridge->setSAM(HNF_SAM);

        // Link 2x2 mesh topology:
        // Mesh0(0,0) <-> Mesh1(1,0)
        //    ^              ^
        //    |              |
        // Mesh3(0,1) <-> Mesh2(1,1)
        //
        // Endpoint placement:
        // L2Wrapper(CHIBridge) <-> Mesh0(local0)
        // Mesh1(local0) <-> FakeL3(networkPort)
        // Mesh2(local0) <-> DDRWrapper(networkPort)
        assert(L2wrap->getCHIPort() != nullptr && Mesh0->getLocal0Port() != nullptr);
        L2wrap->getCHIPort()->connect(Mesh0->getLocal0Port());

        assert(Mesh0->getEastPort() != nullptr && Mesh1->getWestPort() != nullptr);
        Mesh0->getEastPort()->connect(Mesh1->getWestPort());

        assert(Mesh1->getNorthPort() != nullptr && Mesh2->getSouthPort() != nullptr);
        Mesh1->getNorthPort()->connect(Mesh2->getSouthPort());

        assert(Mesh2->getWestPort() != nullptr && Mesh3->getEastPort() != nullptr);
        Mesh2->getWestPort()->connect(Mesh3->getEastPort());

        assert(Mesh3->getSouthPort() != nullptr && Mesh0->getNorthPort() != nullptr);
        Mesh3->getSouthPort()->connect(Mesh0->getNorthPort());

        assert(Mesh1->getLocal0Port() != nullptr && L3bridge->getNetworkPort() != nullptr);
        Mesh1->getLocal0Port()->connect(L3bridge->getNetworkPort());

        assert(Mesh2->getLocal0Port() != nullptr && Dram->getCHIPort() != nullptr);
        Mesh2->getLocal0Port()->connect(Dram->getCHIPort());

        std::set<uint32_t> shadowNodeIds;
        for (size_t i = 0; i < shadowBridges.size(); ++i) {
            CHIBridge *shadowBridge = shadowBridges[i];
            panic_if(shadowBridge == nullptr,
                     "L2ToDramSys shadow bridge[%zu] is null", i);
            panic_if(shadowBridge != L2wrap->getShadowBridges()[i],
                     "L2ToDramSys shadow bridge[%zu] pointer mismatch with L2Wrapper", i);

            const ShadowAttachTarget attachTarget =
                parseShadowAttachPoint(shadowAttachPoints[i],
                                       Mesh0, Mesh1, Mesh2, Mesh3);
            // 同一 local 口只能连接一个 endpoint，冲突直接失败。
            panic_if(attachTarget.port->isConnected(),
                     "L2ToDramSys shadow[%zu] attach point %s is already connected",
                     i, attachTarget.normalized.c_str());

            CHIPort *shadowPort = shadowBridge->getNetworkPort();
            panic_if(shadowPort == nullptr,
                     "L2ToDramSys shadow bridge[%zu] has null network port", i);
            panic_if(shadowPort->isConnected(),
                     "L2ToDramSys shadow bridge[%zu] network port already connected", i);

            shadowPort->connect(attachTarget.port);

            // 影子 NodeID 按“物理挂点坐标 + local口”编码，确保在 Mesh 里被视作独立 RN。
            const uint32_t shadowNodeId = NodeID(
                attachTarget.meshX, attachTarget.meshY,
                attachTarget.localPort).getNodeID();
            panic_if(shadowNodeIds.count(shadowNodeId) > 0,
                     "L2ToDramSys duplicate shadow node_id=%u for shadow[%zu]",
                     shadowNodeId, i);
            shadowNodeIds.insert(shadowNodeId);

            // 每个影子桥单独配置 RN SAM，目前统一路由到 HN(L3ID)。
            auto shadowSam = std::make_shared<SystemAddressMapRN>();
            shadowSam->addNodeID(L3ID);
            shadowBridge->setNodeID(shadowNodeId);
            shadowBridge->setSAM(shadowSam);

            inform("xsCHI shadow[%zu] placement: attach=%s node_id=%u route_to_hn=%u",
                   i, attachTarget.normalized.c_str(), shadowNodeId, L3ID);
        }

        const auto isConnected = [](CHIPort *port) {
            return (port != nullptr) && port->isConnected();
        };

        // Keep topology summary always visible in runtime log so checkpoint
        // runs can verify actual mesh placement and links quickly.
        inform("xsCHI mesh summary: 2x2 nodes M0=(%u,%u) M1=(%u,%u) M2=(%u,%u) M3=(%u,%u)",
               mesh0_x, mesh0_y, mesh1_x, mesh1_y, mesh2_x, mesh2_y,
               mesh3_x, mesh3_y);
        inform("xsCHI endpoint placement: RN@M0.local0 node_id=%u, HN@M1.local0 node_id=%u, DRAM@M2.local0 node_id=%u",
               L2ID, L3ID, dramID);
        inform("xsCHI shadow summary: count=%zu", shadowBridges.size());
        inform("xsCHI mesh links: M0.east<->M1.west, M1.north<->M2.south, M2.west<->M3.east, M3.south<->M0.north");
        inform("xsCHI node[%s] local0=%d east=%d west=%d north=%d south=%d",
               Mesh0->name(), isConnected(Mesh0->getLocal0Port()),
               isConnected(Mesh0->getEastPort()),
               isConnected(Mesh0->getWestPort()),
               isConnected(Mesh0->getNorthPort()),
               isConnected(Mesh0->getSouthPort()));
        inform("xsCHI node[%s] local0=%d east=%d west=%d north=%d south=%d",
               Mesh1->name(), isConnected(Mesh1->getLocal0Port()),
               isConnected(Mesh1->getEastPort()),
               isConnected(Mesh1->getWestPort()),
               isConnected(Mesh1->getNorthPort()),
               isConnected(Mesh1->getSouthPort()));
        inform("xsCHI node[%s] local0=%d east=%d west=%d north=%d south=%d",
               Mesh2->name(), isConnected(Mesh2->getLocal0Port()),
               isConnected(Mesh2->getEastPort()),
               isConnected(Mesh2->getWestPort()),
               isConnected(Mesh2->getNorthPort()),
               isConnected(Mesh2->getSouthPort()));
        inform("xsCHI node[%s] local0=%d east=%d west=%d north=%d south=%d",
               Mesh3->name(), isConnected(Mesh3->getLocal0Port()),
               isConnected(Mesh3->getEastPort()),
               isConnected(Mesh3->getWestPort()),
               isConnected(Mesh3->getNorthPort()),
               isConnected(Mesh3->getSouthPort()));

        DPRINTF(Cache,
            "Init CHI topo with MeshNodes: L2ID=%u(%u,%u,p0) -> HNID=%u(%u,%u,p0), DramID=%u(%u,%u,p0)\n",
            L2ID, mesh0_x, mesh0_y,
            L3ID, mesh1_x, mesh1_y,
            dramID, mesh2_x, mesh2_y);
    }

    gem5::Port &
    L2ToDramSys::getPort(const std::string &if_name, PortID idx)
    {
        return this->L2wrap->getPort(if_name,idx);
    }
    void
    L2ToDramSys::init()
    {
        // make sure both sides are connected and have the same block size
        DPRINTF(Cache, "Init a L2-Dram system!");
    }

}
}
