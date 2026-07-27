#pragma once

#include <array>
#include <string>
#include <vector>

#include "mem/xsCHI/device/CHI_L2.hh"
#include "mem/xsCHI/device/CHI_L3.hh"
#include "mem/xsCHI/device/DDRWrapper.hh"
#include "mem/xsCHI/device/MeshNode.hh"
#include "params/L2L3DramSys6x6.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace xsCHI
{

class L2L3DramSys6x6 : public ClockedObject
{
  private:
    static constexpr size_t NumMeshes = 36;

    CHI_L2 *l2wrap;
    std::vector<CHI_L3*> hns;
    std::vector<DDRWrapper*> drams;
    std::array<MeshNode*, NumMeshes> meshes;

    std::string rnAttachPoint;
    std::vector<std::string> hnAttachPoints;
    std::vector<std::string> dramAttachPoints;
    std::vector<CHIBridge*> shadowBridges;
    std::vector<std::string> shadowAttachPoints;

  public:
    using Params = L2L3DramSys6x6Params;

    explicit L2L3DramSys6x6(const Params &p);

    gem5::Port &getPort(const std::string &if_name,
                        PortID idx = InvalidPortID) override;
    void init() override;
};

} // namespace xsCHI
} // namespace gem5
