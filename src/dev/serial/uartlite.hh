#ifndef __DEV_UARTLITE_HH__
#define __DEV_UARTLITE_HH__

#include "dev/io_device.hh"
#include "dev/serial/serial.hh"
#include "params/UartLite.hh"

#define UARTLITE_RX_FIFO 0x0
#define UARTLITE_TX_FIFO 0x4
#define UARTLITE_STAT_REG 0x8
#define UARTLITE_CTRL_REG 0xc

namespace gem5
{
class UartLite : public BasicPioDevice
{
  public:
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

    explicit UartLite(const UartLiteParams *params);
};
}  // namespace gem5

#endif
