#include "dev/riscv/lint.hh"

#include "lint.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5
{
using namespace RiscvISA;



Tick Lint::read(PacketPtr pkt) {

  pkt->setLE(timeStamp);
  timeStamp += 800;
  pkt->makeAtomicResponse();
  return pioDelay;
}

Tick Lint::write(PacketPtr pkt) {
  warn("Lint device doesn't support writes\n");

  pkt->makeAtomicResponse();
  return pioDelay;
}
Lint::Lint(const LintParams *p) :
    BasicPioDevice(*p, p->pio_size)
{

}


gem5::Lint *
LintParams::create() const
{
  return new Lint(this);
}
}
