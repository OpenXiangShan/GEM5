#include "mem/xsCHI/TopoSys/L2L3DramSys6x6.hh"

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

constexpr uint32_t MeshWidth = 6;
constexpr uint32_t MeshHeight = 6;

struct AttachTarget
{
    MeshNode *mesh = nullptr;
    CHIPort *port = nullptr;
    uint32_t localPort = 0;
    uint32_t meshIndex = 0;
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

bool
isPowerOfTwo(size_t value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

uint32_t
parseMeshIndexToken(const std::string &meshToken,
                    const std::string &rawAttachPoint,
                    const char *kind)
{
    panic_if(meshToken.size() <= 4 || meshToken.substr(0, 4) != "mesh",
             "Invalid %s mesh token '%s' in '%s'",
             kind, meshToken.c_str(), rawAttachPoint.c_str());

    uint32_t meshIndex = 0;
    for (size_t i = 4; i < meshToken.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(meshToken[i]);
        panic_if(!std::isdigit(c),
                 "Invalid %s mesh token '%s' in '%s'",
                 kind, meshToken.c_str(), rawAttachPoint.c_str());
        meshIndex = meshIndex * 10 + static_cast<uint32_t>(c - '0');
    }
    return meshIndex;
}

template <size_t N>
AttachTarget
parseAttachPoint(const std::string &rawAttachPoint,
                 const std::array<MeshNode*, N> &meshes,
                 const char *kind)
{
    const std::string trimmed = trimCopy(rawAttachPoint);
    panic_if(trimmed.empty(), "%s attach point is empty", kind);

    const auto dotPos = trimmed.find('.');
    panic_if(dotPos == std::string::npos,
             "Invalid %s attach point '%s', expected format meshX.localY",
             kind, trimmed.c_str());

    const std::string meshToken = toLowerCopy(trimmed.substr(0, dotPos));
    const std::string localToken = toLowerCopy(trimmed.substr(dotPos + 1));

    const uint32_t meshIndex =
        parseMeshIndexToken(meshToken, trimmed, kind);
    panic_if(meshIndex >= meshes.size(),
             "Invalid %s mesh token '%s' in '%s': index out of range [0,%u]",
             kind, meshToken.c_str(), trimmed.c_str(),
             static_cast<unsigned>(meshes.size() - 1));

    MeshNode *mesh = meshes[meshIndex];
    panic_if(mesh == nullptr,
             "%s attach point '%s' refers to null mesh%u",
             kind, trimmed.c_str(), meshIndex);

    uint32_t localPort = 0;
    if (localToken == "local0") {
        localPort = 0;
    } else if (localToken == "local1") {
        localPort = 1;
    } else {
        panic("Invalid %s local token '%s' in '%s'",
              kind, localToken.c_str(), trimmed.c_str());
    }

    CHIPort *port = (localPort == 0) ? mesh->getLocal0Port()
                                     : mesh->getLocal1Port();
    panic_if(port == nullptr,
             "%s attach point '%s' resolves to null port (mesh local%u)",
             kind, trimmed.c_str(), localPort);

    AttachTarget target;
    target.mesh = mesh;
    target.port = port;
    target.localPort = localPort;
    target.meshIndex = meshIndex;
    target.meshX = mesh->getNodeX();
    target.meshY = mesh->getNodeY();
    target.normalized = "mesh" + std::to_string(meshIndex) +
                        ".local" + std::to_string(localPort);
    return target;
}

void
reserveAttachPoint(std::set<std::string> &usedAttachPoints,
                   const AttachTarget &target, const char *kind)
{
    panic_if(usedAttachPoints.count(target.normalized) > 0,
             "Duplicate %s attach point %s",
             kind, target.normalized.c_str());
    panic_if(target.port->isConnected(),
             "%s attach point %s is already connected",
             kind, target.normalized.c_str());
    usedAttachPoints.insert(target.normalized);
}

void
connectPorts(CHIPort *a, CHIPort *b, const char *label)
{
    assert(a != nullptr && b != nullptr);
    panic_if(a->isConnected() || b->isConnected(),
             "L2L3DramSys6x6 mesh link %s is already connected", label);
    a->connect(b);
}

uint32_t
nodeIdOf(const AttachTarget &target)
{
    return NodeID(target.meshX, target.meshY, target.localPort).getNodeID();
}

} // namespace

L2L3DramSys6x6::L2L3DramSys6x6(const Params &p)
    : ClockedObject(p),
      l2wrap(p.L2Wrapper),
      hns(p.HNs.begin(), p.HNs.end()),
      drams(p.dramsim3s.begin(), p.dramsim3s.end()),
      meshes{p.MeshNode0, p.MeshNode1, p.MeshNode2, p.MeshNode3, p.MeshNode4,
             p.MeshNode5, p.MeshNode6, p.MeshNode7, p.MeshNode8, p.MeshNode9,
             p.MeshNode10, p.MeshNode11, p.MeshNode12, p.MeshNode13,
             p.MeshNode14, p.MeshNode15, p.MeshNode16, p.MeshNode17,
             p.MeshNode18, p.MeshNode19, p.MeshNode20, p.MeshNode21,
             p.MeshNode22, p.MeshNode23, p.MeshNode24, p.MeshNode25,
             p.MeshNode26, p.MeshNode27, p.MeshNode28, p.MeshNode29,
             p.MeshNode30, p.MeshNode31, p.MeshNode32, p.MeshNode33,
             p.MeshNode34, p.MeshNode35},
      rnAttachPoint(p.rn_attach_point),
      hnAttachPoints(p.hn_attach_points.begin(), p.hn_attach_points.end()),
      dramAttachPoints(p.dram_attach_points.begin(),
                       p.dram_attach_points.end()),
      shadowBridges(p.ShadowRNBridges.begin(), p.ShadowRNBridges.end()),
      shadowAttachPoints(p.shadow_attach_points.begin(),
                         p.shadow_attach_points.end())
{
    panic_if(l2wrap == nullptr, "L2L3DramSys6x6 requires L2Wrapper");
    for (size_t i = 0; i < meshes.size(); ++i) {
        panic_if(meshes[i] == nullptr,
                 "L2L3DramSys6x6 requires MeshNode0~35, MeshNode%u is null",
                 static_cast<unsigned>(i));
    }

    panic_if(hns.empty(), "L2L3DramSys6x6 requires at least one HN");
    panic_if(drams.empty(), "L2L3DramSys6x6 requires at least one DRAM");
    panic_if(hns.size() != hnAttachPoints.size(),
             "L2L3DramSys6x6 HN config length mismatch: hns=%u "
             "attach_points=%u",
             static_cast<unsigned>(hns.size()),
             static_cast<unsigned>(hnAttachPoints.size()));
    panic_if(drams.size() != dramAttachPoints.size(),
             "L2L3DramSys6x6 DRAM config length mismatch: drams=%u "
             "attach_points=%u",
             static_cast<unsigned>(drams.size()),
             static_cast<unsigned>(dramAttachPoints.size()));
    panic_if(shadowBridges.size() != shadowAttachPoints.size(),
             "L2L3DramSys6x6 shadow config length mismatch: bridges=%u "
             "attach_points=%u",
             static_cast<unsigned>(shadowBridges.size()),
             static_cast<unsigned>(shadowAttachPoints.size()));
    panic_if(shadowBridges.size() != l2wrap->getShadowBridges().size(),
             "L2L3DramSys6x6 shadow bridge mismatch with L2Wrapper: topo=%u "
             "wrapper=%u",
             static_cast<unsigned>(shadowBridges.size()),
             static_cast<unsigned>(l2wrap->getShadowBridges().size()));

    if (!isPowerOfTwo(hns.size())) {
        warn("L2L3DramSys6x6 HN count %u is not a power of two; current SAM "
             "hash may not select every HN",
             static_cast<unsigned>(hns.size()));
    }
    if (!isPowerOfTwo(drams.size())) {
        warn("L2L3DramSys6x6 DRAM count %u is not a power of two; current SAM "
             "hash may not select every DRAM",
             static_cast<unsigned>(drams.size()));
    }

    std::set<std::string> usedAttachPoints;
    const AttachTarget rnTarget = parseAttachPoint(rnAttachPoint, meshes, "RN");
    reserveAttachPoint(usedAttachPoints, rnTarget, "RN");

    std::vector<AttachTarget> hnTargets;
    std::vector<uint32_t> hnIds;
    for (size_t i = 0; i < hns.size(); ++i) {
        panic_if(hns[i] == nullptr,
                 "L2L3DramSys6x6 HN[%u] is null",
                 static_cast<unsigned>(i));
        const AttachTarget target =
            parseAttachPoint(hnAttachPoints[i], meshes, "HN");
        reserveAttachPoint(usedAttachPoints, target, "HN");
        hnTargets.push_back(target);
        hnIds.push_back(nodeIdOf(target));
    }

    std::vector<AttachTarget> dramTargets;
    std::vector<uint32_t> dramIds;
    for (size_t i = 0; i < drams.size(); ++i) {
        panic_if(drams[i] == nullptr,
                 "L2L3DramSys6x6 DRAM[%u] is null",
                 static_cast<unsigned>(i));
        const AttachTarget target =
            parseAttachPoint(dramAttachPoints[i], meshes, "DRAM");
        reserveAttachPoint(usedAttachPoints, target, "DRAM");
        dramTargets.push_back(target);
        dramIds.push_back(nodeIdOf(target));
    }

    const uint32_t l2Id = nodeIdOf(rnTarget);
    auto l2Sam = std::make_shared<SystemAddressMapRN>();
    for (const auto hnId : hnIds) {
        l2Sam->addNodeID(hnId);
    }
    l2wrap->setNodeID(l2Id);
    l2wrap->setSAM(l2Sam);

    for (size_t i = 0; i < drams.size(); ++i) {
        drams[i]->setNodeID(dramIds[i]);
    }

    for (size_t i = 0; i < hns.size(); ++i) {
        auto hnfSam = std::make_shared<SystemAddressMapHN>();
        for (const auto dramId : dramIds) {
            hnfSam->addNodeID(dramId);
        }
        hns[i]->setNodeID(hnIds[i]);
        hns[i]->setSAM(hnfSam);
    }

    assert(l2wrap->getCHIPort() != nullptr);
    panic_if(l2wrap->getCHIPort()->isConnected(),
             "L2L3DramSys6x6 RN network port already connected");
    l2wrap->getCHIPort()->connect(rnTarget.port);

    for (uint32_t y = 0; y < MeshHeight; ++y) {
        for (uint32_t x = 0; x + 1 < MeshWidth; ++x) {
            const uint32_t left = y * MeshWidth + x;
            const uint32_t right = left + 1;
            connectPorts(meshes[left]->getEastPort(), meshes[right]->getWestPort(),
                         ("M" + std::to_string(left) + ".east<->M" +
                          std::to_string(right) + ".west").c_str());
        }
    }
    for (uint32_t y = 0; y + 1 < MeshHeight; ++y) {
        for (uint32_t x = 0; x < MeshWidth; ++x) {
            const uint32_t south = y * MeshWidth + x;
            const uint32_t north = south + MeshWidth;
            connectPorts(meshes[south]->getNorthPort(),
                         meshes[north]->getSouthPort(),
                         ("M" + std::to_string(south) + ".north<->M" +
                          std::to_string(north) + ".south").c_str());
        }
    }

    for (size_t i = 0; i < hns.size(); ++i) {
        assert(hns[i]->getNetworkPort() != nullptr);
        panic_if(hns[i]->getNetworkPort()->isConnected(),
                 "L2L3DramSys6x6 HN[%u] network port already connected",
                 static_cast<unsigned>(i));
        hns[i]->getNetworkPort()->connect(hnTargets[i].port);
        inform("xsCHI HN[%u] placement: attach=%s node_id=%u",
               static_cast<unsigned>(i), hnTargets[i].normalized.c_str(),
               hnIds[i]);
    }

    for (size_t i = 0; i < drams.size(); ++i) {
        assert(drams[i]->getCHIPort() != nullptr);
        panic_if(drams[i]->getCHIPort()->isConnected(),
                 "L2L3DramSys6x6 DRAM[%u] network port already connected",
                 static_cast<unsigned>(i));
        drams[i]->getCHIPort()->connect(dramTargets[i].port);
        inform("xsCHI DRAM[%u] placement: attach=%s node_id=%u",
               static_cast<unsigned>(i), dramTargets[i].normalized.c_str(),
               dramIds[i]);
    }

    std::set<uint32_t> shadowNodeIds;
    for (size_t i = 0; i < shadowBridges.size(); ++i) {
        CHIBridge *shadowBridge = shadowBridges[i];
        panic_if(shadowBridge == nullptr,
                 "L2L3DramSys6x6 shadow bridge[%u] is null",
                 static_cast<unsigned>(i));
        panic_if(shadowBridge != l2wrap->getShadowBridges()[i],
                 "L2L3DramSys6x6 shadow bridge[%u] pointer mismatch with "
                 "L2Wrapper",
                 static_cast<unsigned>(i));

        const AttachTarget attachTarget =
            parseAttachPoint(shadowAttachPoints[i], meshes, "shadow");
        reserveAttachPoint(usedAttachPoints, attachTarget, "shadow");

        CHIPort *shadowPort = shadowBridge->getNetworkPort();
        panic_if(shadowPort == nullptr,
                 "L2L3DramSys6x6 shadow bridge[%u] has null network port",
                 static_cast<unsigned>(i));
        panic_if(shadowPort->isConnected(),
                 "L2L3DramSys6x6 shadow bridge[%u] network port already "
                 "connected",
                 static_cast<unsigned>(i));
        shadowPort->connect(attachTarget.port);

        const uint32_t shadowNodeId = nodeIdOf(attachTarget);
        panic_if(shadowNodeIds.count(shadowNodeId) > 0,
                 "L2L3DramSys6x6 duplicate shadow node_id=%u for shadow[%u]",
                 shadowNodeId, static_cast<unsigned>(i));
        shadowNodeIds.insert(shadowNodeId);

        auto shadowSam = std::make_shared<SystemAddressMapRN>();
        for (const auto hnId : hnIds) {
            shadowSam->addNodeID(hnId);
        }
        shadowBridge->setNodeID(shadowNodeId);
        shadowBridge->setSAM(shadowSam);

        inform("xsCHI shadow[%u] placement: attach=%s node_id=%u route_to_hns=%u",
               static_cast<unsigned>(i), attachTarget.normalized.c_str(),
               shadowNodeId, static_cast<unsigned>(hnIds.size()));
    }

    const auto isConnected = [](CHIPort *port) {
        return (port != nullptr) && port->isConnected();
    };

    std::string meshSummary = "xsCHI mesh summary: 6x6 nodes";
    for (size_t i = 0; i < meshes.size(); ++i) {
        meshSummary += " M" + std::to_string(i) + "=(" +
                       std::to_string(meshes[i]->getNodeX()) + "," +
                       std::to_string(meshes[i]->getNodeY()) + ")";
    }
    inform("%s", meshSummary.c_str());
    inform("xsCHI endpoint summary: RN@%s node_id=%u, HN_count=%u, "
           "DRAM_count=%u, shadow_count=%u",
           rnTarget.normalized.c_str(), l2Id, static_cast<unsigned>(hns.size()),
           static_cast<unsigned>(drams.size()),
           static_cast<unsigned>(shadowBridges.size()));

    for (size_t i = 0; i < meshes.size(); ++i) {
        MeshNode *mesh = meshes[i];
        inform("xsCHI node[M%u:%s] local0=%d local1=%d east=%d west=%d "
               "north=%d south=%d",
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
L2L3DramSys6x6::getPort(const std::string &if_name, PortID idx)
{
    return l2wrap->getPort(if_name, idx);
}

void
L2L3DramSys6x6::init()
{
    DPRINTF(Cache, "Init L2-CHI_L3-DRAM(6x6) system\n");
}

} // namespace xsCHI
} // namespace gem5
