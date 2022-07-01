#ifndef GEM5_LINT_HH
#define GEM5_LINT_HH


#include "dev/io_device.hh"
#include "dev/platform.hh"
#include "dev/riscv/lint.hh"
#include "params/Lint.hh"
#include "sim/system.hh"

namespace gem5
{

using namespace RiscvISA;

class Lint: public BasicPioDevice
{
  private:
    uint64_t timeStamp;

  public:
    typedef LintParams Params;
    Lint(const Params *p);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
};
}



#endif
