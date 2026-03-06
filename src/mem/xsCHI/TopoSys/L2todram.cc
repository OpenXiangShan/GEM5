
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
        Mesh1(p.MeshNode1)
    {
        panic_if(Mesh0 == nullptr || Mesh1 == nullptr,
                 "L2ToDramSys requires MeshNode0 and MeshNode1");

        // Derive endpoint IDs from MeshNode topology so NodeID encoding
        // always matches (x, y, local_port) placement.
        const uint32_t mesh0_x = Mesh0->getNodeX();
        const uint32_t mesh0_y = Mesh0->getNodeY();
        const uint32_t mesh1_x = Mesh1->getNodeX();
        const uint32_t mesh1_y = Mesh1->getNodeY();

        const uint32_t L2ID = NodeID(mesh0_x, mesh0_y, 0).getNodeID();
        const uint32_t L3ID = NodeID(mesh1_x, mesh1_y, 0).getNodeID();
        const uint32_t dramID = NodeID(mesh1_x, mesh1_y, 1).getNodeID();

        auto L2SAM = std::make_shared<SystemAddressMapRN>();
        L2SAM->addNodeID(L3ID);
        L2wrap->setNodeID(L2ID);
        L2wrap->setSAM(L2SAM);

        Dram->setNodeID(dramID);

        auto HNF_SAM = std::make_shared<SystemAddressMapHN>();
        HNF_SAM->addNodeID(dramID);
        L3bridge->setNodeID(L3ID);
        L3bridge->setSAM(HNF_SAM);

        // Link chain:
        // L2Wrapper(CHIBridge) <-> Mesh0(local0)
        // Mesh0(east) <-> Mesh1(west)
        // Mesh1(local0) <-> FakeL3(CPUSIDE)
        assert(L2wrap->getCHIPort() != nullptr && Mesh0->getLocal0Port() != nullptr);
        L2wrap->getCHIPort()->connect(Mesh0->getLocal0Port());

        assert(Mesh0->getEastPort() != nullptr && Mesh1->getWestPort() != nullptr);
        Mesh0->getEastPort()->connect(Mesh1->getWestPort());

        assert(Mesh1->getLocal0Port() != nullptr && L3bridge->getCHIPort_CPUSIDE() != nullptr);
        Mesh1->getLocal0Port()->connect(L3bridge->getCHIPort_CPUSIDE());

        // Keep HN -> DRAM side direct for the minimal 2-node mesh bring-up.
        assert(Dram->getCHIPort()!=nullptr && L3bridge->getCHIPort_MEMSIDE() != nullptr);
        Dram->getCHIPort()->connect(L3bridge->getCHIPort_MEMSIDE());

        DPRINTF(Cache,
            "Init CHI topo with MeshNodes: L2ID=%u(%u,%u,p0) -> HNID=%u(%u,%u,p0), DramID=%u(%u,%u,p1)\n",
            L2ID, mesh0_x, mesh0_y,
            L3ID, mesh1_x, mesh1_y,
            dramID, mesh1_x, mesh1_y);
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
