#include "dev/riscv/hart_ctrl.hh"

#include "cpu/thread_context.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5
{

HartCtrl::HartCtrl(const Params &p)
    : BasicPioDevice(p, p.pio_size),
      hartResetState(p.num_threads, 1)
{
    if (!hartResetState.empty()) {
        // Hart 0 is the boot hart and is considered released by default.
        hartResetState[0] = 0;
    }
}

Tick
HartCtrl::read(PacketPtr pkt)
{
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    assert(pkt->getSize() > 0 && pkt->getSize() <= sizeof(uint64_t));

    const Addr offset = pkt->getAddr() - pioAddr;
    panic_if(offset % sizeof(uint64_t) != 0,
             "HartCtrl only supports 64-bit aligned accesses: addr=%#lx",
             pkt->getAddr());

    const ThreadID tid = offset / sizeof(uint64_t);
    panic_if(tid >= hartResetState.size(),
             "HartCtrl access out of range: tid=%u addr=%#lx",
             tid, pkt->getAddr());

    pkt->setLE(hartResetState[tid]);
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
HartCtrl::write(PacketPtr pkt)
{
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    assert(pkt->getSize() > 0 && pkt->getSize() <= sizeof(uint64_t));

    const Addr offset = pkt->getAddr() - pioAddr;
    panic_if(offset % sizeof(uint64_t) != 0,
             "HartCtrl only supports 64-bit aligned accesses: addr=%#lx",
             pkt->getAddr());

    const ThreadID tid = offset / sizeof(uint64_t);
    panic_if(tid >= hartResetState.size(),
             "HartCtrl access out of range: tid=%u addr=%#lx",
             tid, pkt->getAddr());

    uint64_t value = 0;
    switch (pkt->getSize()) {
      case sizeof(uint8_t):
        value = pkt->getLE<uint8_t>();
        break;
      case sizeof(uint16_t):
        value = pkt->getLE<uint16_t>();
        break;
      case sizeof(uint32_t):
        value = pkt->getLE<uint32_t>();
        break;
      case sizeof(uint64_t):
        value = pkt->getLE<uint64_t>();
        break;
      default:
        panic("Unsupported HartCtrl write size %u\n", pkt->getSize());
    }

    hartResetState[tid] = value;

    if (value == 0) {
        tryWakeHart(tid);
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}

void
HartCtrl::tryWakeHart(ThreadID tid)
{
    panic_if(tid >= sys->threads.size(),
             "HartCtrl wake target %u out of system thread range %zu",
             tid, sys->threads.size());

    auto *tc = sys->threads[tid];
    panic_if(!tc, "HartCtrl target %u has no thread context", tid);

    tc->activate();
}

} // namespace gem5
