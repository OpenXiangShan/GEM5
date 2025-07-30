
#include "mem/xsCHI/TopoSys/L2todram.hh"

#include <cassert>
#include <memory>

#include "base/trace.hh"
#include "debug/Cache.hh"
#include "params/ClockedObject.hh"
#include "sim/sim_object.hh"

namespace gem5 {
namespace xsCHI {
    L2ToDramSys::L2ToDramSys(const Params &p)
        : ClockedObject(p),
        L2wrap(p.L2Wrapper),
        L3bridge(p.L3),
        Dram(p.dramsim3)
    {
        // NodeID* L2ID = new NodeID(0,0,0);
        auto L2ID= std::make_shared<NodeID>(0,0,0);
        auto L3ID = std::make_shared<NodeID>(1,1,0);
        auto dramID = std::make_shared<NodeID>(2,2,0);
        // auto HNs = std::make_shared<std::list<uint32_t>>();
        // HNs->push_back(L3ID->getNodeID());
        auto L2SAM = std::make_shared<SystemAddressMapRN>();
        L2SAM->addNodeID(L3ID->getNodeID());
        // L2wrap = new L2Wrapper(p,L2ID,&L2SAM);
        L2wrap->setNodeID(*L2ID);
        L2wrap->setSAM(L2SAM);
        // Dram = new DDRWrapper(params,dramID,&L2SAM);
        // auto list = std::make_shared<std::list<uint32_t>>();
        // list->push_back(dramID->getNodeID());
        // auto dramSAM = std::make_shared<SystemAddressMapRN>(*list);
        Dram->setNodeID(*dramID);
        // Dram->setSAM(dramSAM.get());
        // auto SNs =std::make_shared<std::list<uint32_t>>();
        // SNs->push_back(dramID->getNodeID());
        auto HNF_SAM = std::make_shared<SystemAddressMapHN>();
        // L3bridge = new FakeL3(p,L3ID,&HNF_SAM);
        HNF_SAM->addNodeID(dramID->getNodeID());
        L3bridge->setNodeID(*L3ID);
        L3bridge->setSAM(HNF_SAM);
        //todo:connect port! set buffersize by param!
        assert(L2wrap->getCHIPort()!=nullptr && L3bridge->getCHIPort_CPUSIDE() != nullptr);
        L2wrap->getCHIPort()->connect(L3bridge->getCHIPort_CPUSIDE());
        assert(Dram->getCHIPort()!=nullptr && L3bridge->getCHIPort_MEMSIDE() != nullptr);
        Dram->getCHIPort()->connect(L3bridge->getCHIPort_MEMSIDE());
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
