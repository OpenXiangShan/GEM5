#pragma once

#include <string>
#include <vector>

#include "mem/xsCHI/device/CHI_L2.hh"
#include "mem/xsCHI/device/CHI_L3.hh"
#include "mem/xsCHI/device/DDRWrapper.hh"
#include "mem/xsCHI/device/MeshNode.hh"
#include "params/L2L3DramSysM1Local1Dram.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace xsCHI
{

class L2L3DramSysM1Local1Dram : public ClockedObject
{
  private:
    CHI_L2 *l2wrap;
    CHI_L3 *l3;
    DDRWrapper *dram;
    // 2x2 mesh:
    // Mesh0(0,0) <-> Mesh1(1,0)
    //   ^               ^
    //   |               |
    // Mesh3(0,1) <-> Mesh2(1,1)
    // Endpoints:
    // RN@Mesh0.local0, HN@Mesh1.local0, DRAM@Mesh1.local1
    MeshNode *Mesh0;
    MeshNode *Mesh1;
    MeshNode *Mesh2;
    MeshNode *Mesh3;
    std::vector<CHIBridge*> shadowBridges;
    std::vector<std::string> shadowAttachPoints;

  public:
    using Params = L2L3DramSysM1Local1DramParams;

    explicit L2L3DramSysM1Local1Dram(const Params &p);

    gem5::Port &getPort(const std::string &if_name,
                        PortID idx = InvalidPortID) override;
    void init() override;
};

} // namespace xsCHI
} // namespace gem5
