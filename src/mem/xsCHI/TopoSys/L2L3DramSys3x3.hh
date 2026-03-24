#pragma once

#include <string>
#include <vector>

#include "mem/xsCHI/device/CHI_L2.hh"
#include "mem/xsCHI/device/CHI_L3.hh"
#include "mem/xsCHI/device/DDRWrapper.hh"
#include "mem/xsCHI/device/MeshNode.hh"
#include "params/L2L3DramSys3x3.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace xsCHI
{

class L2L3DramSys3x3 : public ClockedObject
{
  private:
    CHI_L2 *l2wrap;
    CHI_L3 *l3;
    DDRWrapper *dram;

    // 3x3 mesh (row-major):
    // M0(0,0) M1(1,0) M2(2,0)
    // M3(0,1) M4(1,1) M5(2,1)
    // M6(0,2) M7(1,2) M8(2,2)
    // Endpoints:
    // RN@M0.local0, HN@M4.local0, DRAM@M4.local1
    MeshNode *Mesh0;
    MeshNode *Mesh1;
    MeshNode *Mesh2;
    MeshNode *Mesh3;
    MeshNode *Mesh4;
    MeshNode *Mesh5;
    MeshNode *Mesh6;
    MeshNode *Mesh7;
    MeshNode *Mesh8;

    std::vector<CHIBridge*> shadowBridges;
    std::vector<std::string> shadowAttachPoints;
    std::string hnAttachPoint;
    std::string dramAttachPoint;

  public:
    using Params = L2L3DramSys3x3Params;

    explicit L2L3DramSys3x3(const Params &p);

    gem5::Port &getPort(const std::string &if_name,
                        PortID idx = InvalidPortID) override;
    void init() override;
};

} // namespace xsCHI
} // namespace gem5
