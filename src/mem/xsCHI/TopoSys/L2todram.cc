
#include "mem/xsCHI/TopoSys/L2todram.hh"

#include <cassert>
#include <cstdint>
#include <memory>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Cache.hh"
#include "mem/xsCHI/base/Network/NodeID.hh"
#include "params/ClockedObject.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace xsCHI {
    L2ToDramSys::L2ToDramSys(const Params &p)
        : ClockedObject(p),
        L2wrap(p.L2Wrapper),
        L3bridge(p.L3),
        Dram(p.dramsim3),
        Mesh0(p.MeshNode0),
        Mesh1(p.MeshNode1),
        Mesh2(p.MeshNode2),
        Mesh3(p.MeshNode3)
    {
        panic_if(Mesh0 == nullptr || Mesh1 == nullptr ||
                 Mesh2 == nullptr || Mesh3 == nullptr,
                 "L2ToDramSys requires MeshNode0/1/2/3");

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
