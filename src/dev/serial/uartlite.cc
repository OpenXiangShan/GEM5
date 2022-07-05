#include "mem/packet_access.hh"
#include "uartlite.hh"

namespace gem5
{
Tick UartLite::read(PacketPtr pkt)
{
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    auto offset = pkt->getAddr() - pioAddr;
    assert(pkt->getSize() == 1);

    switch (offset) {
        case UARTLITE_STAT_REG:
            pkt->setRaw((uint8_t)0);
            break;
        default:
            warn("Read to other uartlite addr %i is not implemented\n",
                 offset);
    }
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick UartLite::write(PacketPtr pkt)
{
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    auto offset = pkt->getAddr() - pioAddr;
    assert(pkt->getSize() == 1);

    switch (offset) {
        case UARTLITE_TX_FIFO:
            putc(pkt->getRaw<uint8_t>(), stdout);
            break;
        default:
            warn("Write to other uartlite addr %i is not implemented\n",
                 offset);
    }

    pkt->makeAtomicResponse();
    return pioDelay;
}

UartLite::UartLite(const UartLiteParams *params)
    : BasicPioDevice(*params, params->pio_size)
{
}

gem5::UartLite *UartLiteParams::create() const { return new UartLite(this); }

}  // namespace gem5
