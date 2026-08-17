#include <cstdio>

#include "base/logging.hh"
#include "mem/packet_access.hh"
#include "uartlite.hh"

namespace gem5
{

// Xilinx UART Lite TXEMPTY bit. OpenSBI polls TXFULL before putc (cleared when
// status==0 also works); Linux/other firmwares may wait on TXEMPTY. Always
// report idle TX so both polling styles make progress.
static constexpr uint8_t UARTLITE_STATUS_TXEMPTY = 0x04;

Tick UartLite::read(PacketPtr pkt)
{
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    auto offset = pkt->getAddr() - pioAddr;
    assert(pkt->getSize() == 1);

    switch (offset) {
        case UARTLITE_STAT_REG:
            // Instant TX drain model: always ready for the next byte.
            pkt->setRaw((uint8_t)(UARTLITE_STATUS_TXEMPTY));
            break;
        case UARTLITE_RX_FIFO:
            // No RX data modeled; RXVALID stays clear.
            pkt->setRaw((uint8_t)0);
            break;
        default:
            warn("Read to other uartlite addr %i is not implemented\n",
                 offset);
            pkt->setRaw((uint8_t)0);
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
        case UARTLITE_TX_FIFO: {
            // Flush on newline only: per-byte fflush on the PIO path slows
            // console-heavy boots (OpenSBI/Linux) without buying correctness.
            const uint8_t ch = pkt->getRaw<uint8_t>();
            putc(ch, stdout);
            if (ch == '\n')
                fflush(stdout);
            break;
        }
        case UARTLITE_CTRL_REG:
            // Soft reset / IE bits: accept and ignore for bare-metal boot.
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
