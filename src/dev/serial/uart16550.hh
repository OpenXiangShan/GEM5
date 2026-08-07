/*
 * Copyright (c) 2026
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __DEV_SERIAL_UART16550_HH__
#define __DEV_SERIAL_UART16550_HH__

#include <cstddef>
#include <cstdint>
#include <deque>

#include "dev/io_device.hh"
#include "params/Uart16550.hh"
#include "sim/eventq.hh"

namespace gem5
{

/**
 * A register-shifted, direct-stdout UART16550 for the XiangShan platform.
 *
 * Standard UART registers are eight bits wide but occupy four-byte MMIO
 * slots, matching the `reg-shift = <2>` hardware interface.  The device has
 * no external interrupt pin because the XiangShan NemuPlic model does not
 * expose one; IER/IIR state is still modeled for software compatibility.
 */
class Uart16550 : public BasicPioDevice
{
  public:
    using Params = Uart16550Params;

    explicit Uart16550(const Params &p);

    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;

    /** Deliver a character to the receive side, for loopback or future I/O. */
    void receive(uint8_t data);

  private:
    static constexpr Addr RegisterStride = 4;

    // Standard 16550 registers at register index << 2.
    static constexpr Addr RbrThrDll = 0x00;
    static constexpr Addr IerDlm = 0x04;
    static constexpr Addr IirFcr = 0x08;
    static constexpr Addr Lcr = 0x0c;
    static constexpr Addr Mcr = 0x10;
    static constexpr Addr Lsr = 0x14;
    static constexpr Addr Msr = 0x18;
    static constexpr Addr Scr = 0x1c;

    static constexpr uint8_t LcrDlab = 0x80;
    static constexpr uint8_t McrLoopback = 0x10;

    static constexpr uint8_t IerReceiveData = 0x01;
    static constexpr uint8_t IerTransmitEmpty = 0x02;
    static constexpr uint8_t IerLineStatus = 0x04;

    static constexpr uint8_t IirNoInterrupt = 0x01;
    static constexpr uint8_t IirTransmitEmpty = 0x02;
    static constexpr uint8_t IirReceiveData = 0x04;
    static constexpr uint8_t IirLineStatus = 0x06;
    static constexpr uint8_t IirFifoEnabled = 0xc0;

    static constexpr uint8_t FcrEnableFifo = 0x01;
    static constexpr uint8_t FcrResetReceive = 0x02;
    static constexpr uint8_t FcrResetTransmit = 0x04;
    static constexpr uint8_t FcrTriggerMask = 0xc0;

    static constexpr uint8_t LsrDataReady = 0x01;
    static constexpr uint8_t LsrOverrun = 0x02;
    static constexpr uint8_t LsrErrorMask = 0x1e;
    static constexpr uint8_t LsrTransmitEmpty = 0x20;
    static constexpr uint8_t LsrTransmitterEmpty = 0x40;

    static constexpr uint8_t MsrDcd = 0x80;
    static constexpr uint8_t MsrDsr = 0x20;
    static constexpr uint8_t MsrCts = 0x10;

    const bool fifoSupported;
    const std::size_t fifoDepth;

    uint8_t rbr = 0;
    bool rbrFull = false;
    uint8_t ier = 0;
    uint8_t lcr = 0;
    uint8_t mcr = 0;
    uint8_t scr = 0;
    uint8_t dll = 12;
    uint8_t dlm = 0;
    uint8_t fcr = 0;
    uint8_t lineStatusErrors = 0;

    std::deque<uint8_t> rxFifo;
    std::deque<uint8_t> txFifo;
    bool txBusy = false;
    uint8_t txShift = 0;
    bool txInterruptPending = false;
    EventFunctionWrapper txEvent;

    bool fifoActive() const;
    bool receiveReady() const;
    bool transmitHoldingEmpty() const;
    unsigned receiveTriggerLevel() const;

    uint8_t lineStatus() const;
    uint8_t modemStatus() const;
    uint8_t readRbr();
    uint8_t readIir();
    uint8_t readRegister(Addr offset);
    void writeThr(uint8_t data);
    void writeIer(uint8_t data);
    void writeFcr(uint8_t data);
    void writeRegister(Addr offset, uint8_t data);

    void startTransmit();
    void completeTransmit();
    void updateTxInterrupt();
    Tick transmitDelay() const;

    uint8_t getWriteData(PacketPtr pkt) const;
    void setReadData(PacketPtr pkt, uint8_t data) const;
};

} // namespace gem5

#endif // __DEV_SERIAL_UART16550_HH__
