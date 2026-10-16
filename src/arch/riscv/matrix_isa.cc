/*
 * Copyright (c) 2016 RISC-V Foundation
 * Copyright (c) 2016 The University of Virginia
 * Copyright (c) 2020 Barkhausen Institut
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

#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/misc.hh"
#include "base/trace.hh"
#include "debug/RiscvMisc.hh"

namespace gem5
{

namespace RiscvISA
{

void
ISA::useNonMatrixRegClasses()
{
    // This is an O3 construction hook, before any register files exist.
    // Keep xs-dev's reserved RMisc slots for ordinary scalar/vector configs.
    RegClasses classes;
    for (const auto &reg_class : _regClasses) {
        if (reg_class.type() == RMiscRegClass) {
            classes.emplace_back(RMiscRegClass, 10, reg_class.debug(),
                                 reg_class.regBytes());
        } else {
            classes.push_back(reg_class);
        }
    }
    _regClasses.swap(classes);
}

namespace
{

constexpr RegVal MatrixXmisa = 0x2e6;
constexpr RegVal MatrixXTLenB = 8192;
constexpr RegVal MatrixXTRLenB = 64;
constexpr RegVal MatrixXALenB = 65536;
constexpr RegVal MatrixTokenCount = 32;
constexpr RegVal MatrixRowNum = MatrixXTLenB / MatrixXTRLenB;
constexpr RegVal MatrixArLenBPerRow = MatrixXALenB / MatrixRowNum;
constexpr RegVal MatrixSewE8 = 0;
constexpr RegVal MatrixSewE32 = 2;

inline RegVal
packXmcsr(RegVal xmxrm, RegVal xmsat, RegVal xmfflags, RegVal xmfrm,
          RegVal xmsaten)
{
    return (xmxrm & 0x3) |
           ((xmsat & 0x1) << 2) |
           ((xmfflags & 0x1f) << 3) |
           ((xmfrm & 0x7) << 8) |
           ((xmsaten & 0x1) << 11);
}

} // namespace

void
ISA::resetMatrixState()
{
    matrixXmxrm = 0;
    matrixXmsat = 0;
    matrixXmfflags = 0;
    matrixXmfrm = 0;
    matrixXmsaten = 0;
    matrixTileM = 0;
    matrixTileK = 0;
    matrixTileN = 0;
    matrixTokens.assign(MatrixTokenCount, 0);
}

void
ISA::resetMatrixToken(RegVal token_idx)
{
    assert(token_idx < matrixTokens.size());
    DPRINTF(RiscvMisc, "Reset matrix token %llu.\n", token_idx);
    matrixTokens[token_idx] = 0;
}

void
ISA::releaseMatrixToken(RegVal token_idx)
{
    assert(token_idx < matrixTokens.size());
    matrixTokens[token_idx]++;
    DPRINTF(RiscvMisc, "Release matrix token %llu -> %#x.\n",
            token_idx, matrixTokens[token_idx]);
}

bool
ISA::matrixTokenReady(RegVal token_idx, RegVal threshold) const
{
    assert(token_idx < matrixTokens.size());
    return matrixTokens[token_idx] >= threshold;
}

RegVal
ISA::matrixTokenValue(RegVal token_idx) const
{
    assert(token_idx < matrixTokens.size());
    return matrixTokens[token_idx];
}

bool
ISA::readMatrixMiscReg(int misc_reg, RegVal &val) const
{
    switch (misc_reg) {
      case MISCREG_XMCSR:
        val = packXmcsr(matrixXmxrm, matrixXmsat, matrixXmfflags,
                        matrixXmfrm, matrixXmsaten);
        return true;
      case MISCREG_XMXRM:
        val = matrixXmxrm;
        return true;
      case MISCREG_XMSAT:
        val = matrixXmsat;
        return true;
      case MISCREG_XMFFLAGS:
        val = matrixXmfflags;
        return true;
      case MISCREG_XMFRM:
        val = matrixXmfrm;
        return true;
      case MISCREG_XMSATEN:
        val = matrixXmsaten;
        return true;
      case MISCREG_XMISA:
        val = MatrixXmisa;
        return true;
      case MISCREG_XTLENB:
        val = MatrixXTLenB;
        return true;
      case MISCREG_XTRLENB:
        val = MatrixXTRLenB;
        return true;
      case MISCREG_XALENB:
        val = MatrixXALenB;
        return true;
      case MISCREG_MTOK:
        val = MatrixTokenCount;
        return true;
      case MISCREG_MTILEM:
        val = matrixTileM;
        return true;
      case MISCREG_MTILEN:
        val = matrixTileN;
        return true;
      case MISCREG_MTILEK:
        val = matrixTileK;
        return true;
      default:
        return false;
    }
}

} // namespace RiscvISA
} // namespace gem5
