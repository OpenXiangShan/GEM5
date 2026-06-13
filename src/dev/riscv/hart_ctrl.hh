//
// Created for Xiangshan bare-metal hart control MMIO.
//

#ifndef GEM5_HART_CTRL_HH
#define GEM5_HART_CTRL_HH

#include <vector>

#include "dev/io_device.hh"
#include "params/HartCtrl.hh"

namespace gem5
{

class HartCtrl : public BasicPioDevice
{
  public:
    typedef HartCtrlParams Params;
    explicit HartCtrl(const Params &p);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

  private:
    void tryWakeHart(ThreadID tid);

    std::vector<uint64_t> hartResetState;
};

} // namespace gem5

#endif // GEM5_HART_CTRL_HH
