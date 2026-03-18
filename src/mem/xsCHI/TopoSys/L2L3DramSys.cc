#include "mem/xsCHI/TopoSys/L2L3DramSys.hh"

#include <cassert>
#include <cstdint>
#include <memory>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Cache.hh"
#include "mem/xsCHI/base/Network/NodeID.hh"

namespace gem5
{
namespace xsCHI
{

L2L3DramSys::L2L3DramSys(const Params &p)
    : ClockedObject(p), l2wrap(p.L2Wrapper), l3(p.L3), dram(p.dramsim3),
      Mesh0(p.MeshNode0), Mesh1(p.MeshNode1), Mesh2(p.MeshNode2),
      Mesh3(p.MeshNode3)
{
    panic_if(Mesh0 == nullptr || Mesh1 == nullptr ||
             Mesh2 == nullptr || Mesh3 == nullptr,
             "L2L3DramSys requires MeshNode0/1/2/3");

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
    const uint32_t dramId = NodeID(mesh2_x, mesh2_y, 0).getNodeID();

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
        Mesh1->getLocal0Port()->connect(l3->getNetworkPort());

        assert(Mesh2->getLocal0Port() != nullptr && dram->getCHIPort() != nullptr);
        Mesh2->getLocal0Port()->connect(dram->getCHIPort());

        const auto isConnected = [](CHIPort *port) {
         return (port != nullptr) && port->isConnected();
        };

        inform("xsCHI mesh summary: 2x2 nodes M0=(%u,%u) M1=(%u,%u) M2=(%u,%u) M3=(%u,%u)",
            mesh0_x, mesh0_y, mesh1_x, mesh1_y, mesh2_x, mesh2_y,
            mesh3_x, mesh3_y);
        inform("xsCHI endpoint placement: RN@M0.local0 node_id=%u, HN@M1.local0 node_id=%u, DRAM@M2.local0 node_id=%u",
            l2Id, l3Id, dramId);
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
}

gem5::Port &
L2L3DramSys::getPort(const std::string &if_name, PortID idx)
{
    return l2wrap->getPort(if_name, idx);
}

void
L2L3DramSys::init()
{
    DPRINTF(Cache, "Init L2-CHI_L3-DRAM system\n");
}

} // namespace xsCHI
} // namespace gem5
