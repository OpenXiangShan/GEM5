#include "mem/xsCHI/TopoSys/L2L3DramSys.hh"

#include <cassert>
#include <memory>

#include "base/trace.hh"
#include "debug/Cache.hh"

namespace gem5
{
namespace xsCHI
{

L2L3DramSys::L2L3DramSys(const Params &p)
    : ClockedObject(p), l2wrap(p.L2Wrapper), l3(p.L3), dram(p.dramsim3)
{
    const uint32_t l2Id = 1;
    const uint32_t l3Id = 2;
    const uint32_t dramId = 3;

    auto l2Sam = std::make_shared<SystemAddressMapRN>();
    l2Sam->addNodeID(l3Id);
    l2wrap->setNodeID(l2Id);
    l2wrap->setSAM(l2Sam);

    dram->setNodeID(dramId);

    auto hnfSam = std::make_shared<SystemAddressMapHN>();
    hnfSam->addNodeID(dramId);
    l3->setNodeID(l3Id);
    l3->setSAM(hnfSam);

    assert(l2wrap->getCHIPort() != nullptr && l3->getCpuSidePort() != nullptr);
    l2wrap->getCHIPort()->connect(l3->getCpuSidePort());

    assert(dram->getCHIPort() != nullptr && l3->getMemSidePort() != nullptr);
    dram->getCHIPort()->connect(l3->getMemSidePort());
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
