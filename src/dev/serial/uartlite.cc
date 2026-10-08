#include "mem/packet_access.hh"
#include "uartlite.hh"

namespace gem5
{
namespace
{

/**
 * A UARTLite transaction must lie entirely inside the PIO range. The
 * remaining-bytes form avoids the overflow an addr + size comparison
 * could run into.
 */
bool
accessInRange(Addr pio_addr, Addr pio_size, PacketPtr pkt)
{
    const Addr addr = pkt->getAddr();
    const Addr size = pkt->getSize();
    const Addr end = pio_addr + pio_size;
    return size != 0 && addr >= pio_addr && addr < end &&
           size <= end - addr;
}

void
setReadData(PacketPtr pkt, uint32_t data)
{
    switch (pkt->getSize()) {
      case sizeof(uint8_t):
        pkt->setLE<uint8_t>(data);
        break;
      case sizeof(uint32_t):
        pkt->setLE<uint32_t>(data);
        break;
      default:
        panic("Unsupported UARTLite read size %u", pkt->getSize());
    }
}

uint32_t
getWriteData(PacketPtr pkt)
{
    switch (pkt->getSize()) {
      case sizeof(uint8_t):
        return pkt->getLE<uint8_t>();
      case sizeof(uint32_t):
        return pkt->getLE<uint32_t>();
      default:
        panic("Unsupported UARTLite write size %u", pkt->getSize());
    }
}

} // anonymous namespace

Tick UartLite::read(PacketPtr pkt)
{
    assert(accessInRange(pioAddr, pioSize, pkt));
    auto offset = pkt->getAddr() - pioAddr;
    assert(pkt->getSize() == sizeof(uint8_t) ||
           pkt->getSize() == sizeof(uint32_t));

    switch (offset) {
        case UARTLITE_STAT_REG:
            setReadData(pkt, 0);
            break;
        default:
            warn("Read to other uartlite addr %i is not implemented\n",
                 offset);
            setReadData(pkt, 0);
    }
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick UartLite::write(PacketPtr pkt)
{
    assert(accessInRange(pioAddr, pioSize, pkt));
    auto offset = pkt->getAddr() - pioAddr;
    assert(pkt->getSize() == sizeof(uint8_t) ||
           pkt->getSize() == sizeof(uint32_t));

    switch (offset) {
        case UARTLITE_TX_FIFO:
            putc(getWriteData(pkt) & 0xff, stdout);
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
