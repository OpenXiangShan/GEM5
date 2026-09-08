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

#include "dev/serial/uart16550.hh"

#include <algorithm>
#include <cstdio>

#include "base/logging.hh"
#include "mem/packet_access.hh"

namespace gem5
{

Uart16550::Uart16550(const Params &p)
    : BasicPioDevice(p, p.pio_size), fifoSupported(p.enable_fifo),
      fifoDepth(p.fifo_depth),
      txEvent([this] { completeTransmit(); }, name() + ".tx")
{
    fatal_if(fifoSupported && fifoDepth == 0,
             "%s: FIFO depth must be non-zero", name().c_str());
}

bool
Uart16550::fifoActive() const
{
    return fifoSupported && (fcr & FcrEnableFifo);
}

bool
Uart16550::receiveReady() const
{
    return fifoActive() ? !rxFifo.empty() : rbrFull;
}

bool
Uart16550::transmitHoldingEmpty() const
{
    return txFifo.empty();
}

unsigned
Uart16550::receiveTriggerLevel() const
{
    unsigned trigger = 0;
    switch ((fcr & FcrTriggerMask) >> 6) {
      case 0:
        trigger = 1;
        break;
      case 1:
        trigger = 4;
        break;
      case 2:
        trigger = 8;
        break;
      case 3:
        trigger = 14;
        break;
      default:
        panic("Invalid UART16550 FIFO trigger level");
    }
    return std::min(trigger, static_cast<unsigned>(fifoDepth));
}

uint8_t
Uart16550::lineStatus() const
{
    uint8_t status = lineStatusErrors;
    if (receiveReady())
        status |= LsrDataReady;
    if (transmitHoldingEmpty())
        status |= LsrTransmitEmpty;
    if (transmitHoldingEmpty() && !txBusy)
        status |= LsrTransmitterEmpty;
    return status;
}

uint8_t
Uart16550::modemStatus() const
{
    if (!(mcr & McrLoopback))
        return MsrDcd | MsrDsr | MsrCts;

    uint8_t status = 0;
    if (mcr & 0x08)
        status |= MsrDcd;
    if (mcr & 0x01)
        status |= MsrDsr;
    if (mcr & 0x02)
        status |= MsrCts;
    if (mcr & 0x04)
        status |= 0x40;
    return status;
}

uint8_t
Uart16550::readRbr()
{
    uint8_t data = 0;
    if (fifoActive()) {
        if (!rxFifo.empty()) {
            data = rxFifo.front();
            rxFifo.pop_front();
        }
    } else if (rbrFull) {
        data = rbr;
        rbrFull = false;
    }
    return data;
}

uint8_t
Uart16550::readIir()
{
    uint8_t iir = fifoActive() ? IirFifoEnabled : 0;
    const uint8_t lsr = lineStatus();

    if ((ier & IerLineStatus) && (lsr & LsrErrorMask)) {
        iir |= IirLineStatus;
    } else if ((ier & IerReceiveData) && receiveReady() &&
               (!fifoActive() || rxFifo.size() >= receiveTriggerLevel())) {
        iir |= IirReceiveData;
    } else if ((ier & IerTransmitEmpty) && txInterruptPending) {
        iir |= IirTransmitEmpty;
        txInterruptPending = false;
    } else {
        iir |= IirNoInterrupt;
    }
    return iir;
}

uint8_t
Uart16550::readRegister(Addr offset)
{
    if (offset % RegisterStride != 0)
        return 0;

    switch (offset) {
      case RbrThrDll:
        return (lcr & LcrDlab) ? dll : readRbr();
      case IerDlm:
        return (lcr & LcrDlab) ? dlm : ier;
      case IirFcr:
        return readIir();
      case Lcr:
        return lcr;
      case Mcr:
        return mcr;
      case Lsr: {
        const uint8_t status = lineStatus();
        lineStatusErrors &= ~LsrErrorMask;
        return status;
      }
      case Msr:
        return modemStatus();
      case Scr:
        return scr;
      default:
        return 0;
    }
}

void
Uart16550::writeThr(uint8_t data)
{
    if (fifoActive()) {
        if (txFifo.size() < fifoDepth)
            txFifo.push_back(data);
    } else if (txFifo.empty()) {
        txFifo.push_back(data);
    } else {
        // The one-byte holding register exposes the last write.
        txFifo.back() = data;
    }

    txInterruptPending = false;
    startTransmit();
}

void
Uart16550::writeIer(uint8_t data)
{
    const bool thriWasEnabled = ier & IerTransmitEmpty;
    ier = data & 0x0f;
    if (!(ier & IerTransmitEmpty)) {
        txInterruptPending = false;
    } else if (!thriWasEnabled && transmitHoldingEmpty()) {
        txInterruptPending = true;
    }
}

void
Uart16550::writeFcr(uint8_t data)
{
    const bool wasActive = fifoActive();
    fcr = fifoSupported ? data & (FcrEnableFifo | FcrTriggerMask) : 0;
    const bool isActive = fifoActive();

    if (wasActive != isActive || (data & FcrResetReceive)) {
        rxFifo.clear();
        rbrFull = false;
        lineStatusErrors &= ~LsrErrorMask;
    }
    if (wasActive != isActive || (data & FcrResetTransmit))
        txFifo.clear();

    updateTxInterrupt();
}

void
Uart16550::writeRegister(Addr offset, uint8_t data)
{
    if (offset % RegisterStride != 0)
        return;

    switch (offset) {
      case RbrThrDll:
        if (lcr & LcrDlab)
            dll = data;
        else
            writeThr(data);
        break;
      case IerDlm:
        if (lcr & LcrDlab)
            dlm = data;
        else
            writeIer(data);
        break;
      case IirFcr:
        writeFcr(data);
        break;
      case Lcr:
        lcr = data;
        break;
      case Mcr:
        mcr = data;
        break;
      case Scr:
        scr = data;
        break;
      default:
        break;
    }
}

void
Uart16550::receive(uint8_t data)
{
    if (fifoActive()) {
        if (rxFifo.size() >= fifoDepth) {
            lineStatusErrors |= LsrOverrun;
            return;
        }
        rxFifo.push_back(data);
    } else if (rbrFull) {
        lineStatusErrors |= LsrOverrun;
    } else {
        rbr = data;
        rbrFull = true;
    }
}

Tick
Uart16550::transmitDelay() const
{
    return std::max<Tick>(pioDelay, 1);
}

void
Uart16550::startTransmit()
{
    if (txBusy || txFifo.empty())
        return;

    txShift = txFifo.front();
    txFifo.pop_front();
    txBusy = true;
    schedule(txEvent, curTick() + transmitDelay());
    updateTxInterrupt();
}

void
Uart16550::completeTransmit()
{
    if (!txBusy)
        return;

    if (mcr & McrLoopback)
        receive(txShift);
    else
        putc(txShift, stdout);

    txBusy = false;
    startTransmit();
    updateTxInterrupt();
}

void
Uart16550::updateTxInterrupt()
{
    if ((ier & IerTransmitEmpty) && transmitHoldingEmpty())
        txInterruptPending = true;
}

uint8_t
Uart16550::getWriteData(PacketPtr pkt) const
{
    switch (pkt->getSize()) {
      case sizeof(uint8_t):
        return pkt->getLE<uint8_t>();
      case sizeof(uint16_t):
        return pkt->getLE<uint16_t>();
      case sizeof(uint32_t):
        return pkt->getLE<uint32_t>();
      case sizeof(uint64_t):
        return pkt->getLE<uint64_t>();
      default:
        panic("Unsupported UART16550 write size %u", pkt->getSize());
    }
}

void
Uart16550::setReadData(PacketPtr pkt, uint8_t data) const
{
    switch (pkt->getSize()) {
      case sizeof(uint8_t):
        pkt->setLE<uint8_t>(data);
        break;
      case sizeof(uint16_t):
        pkt->setLE<uint16_t>(data);
        break;
      case sizeof(uint32_t):
        pkt->setLE<uint32_t>(data);
        break;
      case sizeof(uint64_t):
        pkt->setLE<uint64_t>(data);
        break;
      default:
        panic("Unsupported UART16550 read size %u", pkt->getSize());
    }
}

Tick
Uart16550::read(PacketPtr pkt)
{
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    const Addr offset = pkt->getAddr() - pioAddr;
    setReadData(pkt, readRegister(offset));
    pkt->makeAtomicResponse();
    return pioDelay;
}

Tick
Uart16550::write(PacketPtr pkt)
{
    assert(pkt->getAddr() >= pioAddr && pkt->getAddr() < pioAddr + pioSize);
    const Addr offset = pkt->getAddr() - pioAddr;
    writeRegister(offset, getWriteData(pkt));
    pkt->makeAtomicResponse();
    return pioDelay;
}

} // namespace gem5
