//
//created by zhenh on 2024/04/19
//

#include "base/trace.hh"
#include "cpu/base.hh"
#include "mem/packet_access.hh"
#include "nemu_plic.hh"
#include "sim/system.hh"

namespace gem5
{
NemuPlic::NemuPlic(const NemuPlicParams &p) : BasicPioDevice(p, p.pio_size) {}

Tick
NemuPlic::read(PacketPtr pkt)
{
    pkt->setLE(0);
    warn("now plic read always is 0\n");
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
NemuPlic::write(PacketPtr pkt)
{
    pkt->makeAtomicResponse();
    return pioDelay;
}

}
