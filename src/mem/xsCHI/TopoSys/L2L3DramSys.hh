#pragma once

#include "mem/xsCHI/device/CHI_L3.hh"
#include "mem/xsCHI/device/DDRWrapper.hh"
#include "mem/xsCHI/device/CHI_L2.hh"
#include "params/L2L3DramSys.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace xsCHI
{

class L2L3DramSys : public ClockedObject
{
  private:
    CHI_L2 *l2wrap;
    CHI_L3 *l3;
    DDRWrapper *dram;

  public:
    using Params = L2L3DramSysParams;

    explicit L2L3DramSys(const Params &p);

    gem5::Port &getPort(const std::string &if_name,
                        PortID idx = InvalidPortID) override;
    void init() override;
};

} // namespace xsCHI
} // namespace gem5
