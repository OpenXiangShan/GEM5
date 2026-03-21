#pragma once

#include "mem/xsCHI/device/MeshNode.hh"
#include "mem/xsCHI/test/CHIStressEndpoint.hh"
#include "params/TwoMeshStressSys.hh"
#include "sim/clocked_object.hh"

namespace gem5
{
namespace xsCHI
{

class TwoMeshStressSys : public ClockedObject
{
  public:
    using Params = TwoMeshStressSysParams;
    explicit TwoMeshStressSys(const Params &p);

    void init() override;

  private:
    CHIStressEndpoint *sender;
    CHIStressEndpoint *receiver;
    MeshNode *mesh0;
    MeshNode *mesh1;
};

} // namespace xsCHI
} // namespace gem5
