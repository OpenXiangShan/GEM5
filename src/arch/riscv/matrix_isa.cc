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

#include <algorithm>
#include <cassert>
#include <iterator>

#include "arch/riscv/isa.hh"
#include "arch/riscv/matrix.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/regs/renameable_misc.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "debug/RiscvMisc.hh"
#include "sim/serialize.hh"

namespace gem5
{

namespace RiscvISA
{

namespace
{

constexpr RegVal MatrixXmisa = 0x2e6;
constexpr RegVal MatrixXTLenB = 8192;
constexpr RegVal MatrixXTRLenB = 64;
constexpr RegVal MatrixXALenB = 65536;
constexpr unsigned MatrixCheckpointVersion = 2;

} // namespace

void
ISA::setMatrixEnabled(bool enable)
{
    matrixEnabled = enable;
    if (enable) {
        return;
    }
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

void
ISA::resetMatrixState()
{
    miscRegFile[MISCREG_XMXRM] = 0;
    miscRegFile[MISCREG_XMSAT] = 0;
    miscRegFile[MISCREG_XMFFLAGS] = 0;
    miscRegFile[MISCREG_XMFRM] = 0;
    miscRegFile[MISCREG_XMSATEN] = 0;
    miscRegFile[MISCREG_MTILEM] = 0;
    miscRegFile[MISCREG_MTILEK] = 0;
    miscRegFile[MISCREG_MTILEN] = 0;
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

bool
ISA::readMatrixMiscReg(int misc_reg, RegVal &val) const
{
    switch (misc_reg) {
      case MISCREG_XMCSR:
        val = (miscRegFile[MISCREG_XMXRM] & 0x3) |
              ((miscRegFile[MISCREG_XMSAT] & 0x1) << 2) |
              ((miscRegFile[MISCREG_XMFFLAGS] & 0x1f) << 3) |
              ((miscRegFile[MISCREG_XMFRM] & 0x7) << 8) |
              ((miscRegFile[MISCREG_XMSATEN] & 0x1) << 11);
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
      case MISCREG_MSYNC:
        val = MatrixTokenCount;
        return true;
      default:
        return false;
    }
}

bool
ISA::writeMatrixMiscReg(int misc_reg, RegVal val)
{
    switch (misc_reg) {
      case MISCREG_STATUS:
      case MISCREG_VSSTATUS: {
        STATUS status = val;
        if (!matrixEnabled || status.ms != 3)
            return false;
        // CSR writes and O3 commit both update SD through this entry point.
        const bool rv32_guest = misc_reg == MISCREG_VSSTATUS &&
            HSTATUS(miscRegFile[MISCREG_HSTATUS]).vsxl == 1;
        miscRegFile[misc_reg] = val | (RegVal(1) << (rv32_guest ? 31 : 63));
        return true;
      }
      case MISCREG_XMCSR:
        miscRegFile[MISCREG_XMXRM] = val & 0x3;
        miscRegFile[MISCREG_XMSAT] = (val >> 2) & 0x1;
        miscRegFile[MISCREG_XMFFLAGS] = (val >> 3) & 0x1f;
        miscRegFile[MISCREG_XMFRM] = (val >> 8) & 0x7;
        miscRegFile[MISCREG_XMSATEN] = (val >> 11) & 0x1;
        return true;
      case MISCREG_XMXRM:
        miscRegFile[MISCREG_XMXRM] = val & 0x3;
        return true;
      case MISCREG_XMSAT:
        miscRegFile[MISCREG_XMSAT] = val & 0x1;
        return true;
      case MISCREG_XMFFLAGS:
        miscRegFile[MISCREG_XMFFLAGS] = val & 0x1f;
        return true;
      case MISCREG_XMFRM:
        miscRegFile[MISCREG_XMFRM] = val & 0x7;
        return true;
      case MISCREG_XMSATEN:
        miscRegFile[MISCREG_XMSATEN] = val & 0x1;
        return true;
      case MISCREG_XMISA:
      case MISCREG_XTLENB:
      case MISCREG_XTRLENB:
      case MISCREG_XALENB:
      case MISCREG_MSYNC:
        return true;
      default:
        return false;
    }
}

void
ISA::serializeMatrixState(CheckpointOut &cp) const
{
    // Older checkpoints used dummy slots here and authoritative named fields.
    // Serialize architectural values, including the XMCSR alias and constants.
    auto registers = miscRegFile;
    for (int reg = MISCREG_XMCSR; reg < MISCREG_HSTATUS; ++reg) {
        registers[reg] = readMiscRegNoEffect(reg);
    }
    arrayParamOut(cp, "miscRegFile", registers);
    paramOut(cp, "matrix_checkpoint_version", MatrixCheckpointVersion);
    const RegVal matrixXmxrm = miscRegFile[MISCREG_XMXRM];
    const RegVal matrixXmsat = miscRegFile[MISCREG_XMSAT];
    const RegVal matrixXmfflags = miscRegFile[MISCREG_XMFFLAGS];
    const RegVal matrixXmfrm = miscRegFile[MISCREG_XMFRM];
    const RegVal matrixXmsaten = miscRegFile[MISCREG_XMSATEN];
    const RegVal matrixTileM = miscRegFile[MISCREG_MTILEM];
    const RegVal matrixTileK = miscRegFile[MISCREG_MTILEK];
    const RegVal matrixTileN = miscRegFile[MISCREG_MTILEN];

    SERIALIZE_SCALAR(matrixXmxrm);
    SERIALIZE_SCALAR(matrixXmsat);
    SERIALIZE_SCALAR(matrixXmfflags);
    SERIALIZE_SCALAR(matrixXmfrm);
    SERIALIZE_SCALAR(matrixXmsaten);
    SERIALIZE_SCALAR(matrixTileM);
    SERIALIZE_SCALAR(matrixTileK);
    SERIALIZE_SCALAR(matrixTileN);
    SERIALIZE_CONTAINER(matrixTokens);
}

void
ISA::unserializeMatrixState(CheckpointIn &cp)
{
    RegVal matrixXmxrm = 0;
    RegVal matrixXmsat = 0;
    RegVal matrixXmfflags = 0;
    RegVal matrixXmfrm = 0;
    RegVal matrixXmsaten = 0;
    RegVal matrixTileM = 0;
    RegVal matrixTileK = 0;
    RegVal matrixTileN = 0;
    const auto &section = Serializable::currentSection();
    const auto hasField = [&](const char *field) {
        return cp.entryExists(section, field);
    };
    constexpr size_t csrSlots = MISCREG_HSTATUS - MISCREG_XMCSR;
    constexpr size_t configSlots = MISCREG_MCFG7 - MISCREG_MCFG0 + 1;
    constexpr size_t ordinaryRegCount = NUM_MISCREGS - csrSlots - configSlots;
    constexpr size_t oldOrdinaryRegCount = ordinaryRegCount - 2;
    constexpr size_t oldMatrixRegCount = MISCREG_STIMECMP;
    const size_t savedRegs = miscRegFile.size();
    const bool legacy = savedRegs == oldOrdinaryRegCount ||
                        savedRegs == ordinaryRegCount;
    fatal_if(!legacy && savedRegs != oldMatrixRegCount &&
             savedRegs != NUM_MISCREGS,
             "Unsupported RISC-V checkpoint register layout: %zu slots.",
             savedRegs);

    if (hasField("matrix_checkpoint_version")) {
        unsigned version;
        paramIn(cp, "matrix_checkpoint_version", version);
        fatal_if((version != 1 || savedRegs != oldMatrixRegCount) &&
                     (version != MatrixCheckpointVersion ||
                      savedRegs != NUM_MISCREGS),
                 "Unsupported Matrix checkpoint version %u for %zu slots.",
                 version, savedRegs);
    } else {
        fatal_if(savedRegs == NUM_MISCREGS,
                 "Matrix checkpoint version missing for %zu slots.", savedRegs);
    }

    if (legacy) {
        // Insert the Matrix slots before HSTATUS and before the timer pair.
        miscRegFile.insert(miscRegFile.begin() + MISCREG_XMCSR, csrSlots, 0);
        miscRegFile.insert(miscRegFile.begin() + MISCREG_MCFG0, configSlots, 0);
    }
    // Only the two timer slots were appended to the old Matrix layout.
    miscRegFile.resize(NUM_MISCREGS, UINT64_MAX);

    if (legacy) {
        constexpr const char *controlFields[] = {
            "matrixXmxrm", "matrixXmsat", "matrixXmfflags", "matrixXmfrm",
            "matrixXmsaten"
        };
        fatal_if(std::any_of(std::begin(controlFields), std::end(controlFields),
                             hasField),
                 "Matrix CSR state conflicts with pre-Matrix register layout.");
        validateLegacyMatrixCheckpoint(cp);
        resetMatrixState();
    } else {
        fatal_if(hasField("matrixTileA") || hasField("matrixTileB") ||
                 hasField("matrixAcc"),
                 "Legacy Matrix tile buffers conflict with Matrix CSR layout.");
        UNSERIALIZE_SCALAR(matrixXmxrm);
        UNSERIALIZE_SCALAR(matrixXmsat);
        UNSERIALIZE_SCALAR(matrixXmfflags);
        UNSERIALIZE_SCALAR(matrixXmfrm);
        UNSERIALIZE_SCALAR(matrixXmsaten);
        UNSERIALIZE_SCALAR(matrixTileM);
        UNSERIALIZE_SCALAR(matrixTileK);
        UNSERIALIZE_SCALAR(matrixTileN);
        UNSERIALIZE_CONTAINER(matrixTokens);
    }
    miscRegFile[MISCREG_XMXRM] = matrixXmxrm;
    miscRegFile[MISCREG_XMSAT] = matrixXmsat;
    miscRegFile[MISCREG_XMFFLAGS] = matrixXmfflags;
    miscRegFile[MISCREG_XMFRM] = matrixXmfrm;
    miscRegFile[MISCREG_XMSATEN] = matrixXmsaten;
    miscRegFile[MISCREG_MTILEM] = matrixTileM;
    miscRegFile[MISCREG_MTILEK] = matrixTileK;
    miscRegFile[MISCREG_MTILEN] = matrixTileN;

    fatal_if(matrixTokens.size() != MatrixTokenCount,
             "Invalid matrixTokens size: %u (expected %llu).",
             matrixTokens.size(), MatrixTokenCount);
    for (int reg = MISCREG_XMCSR; reg < MISCREG_HSTATUS; ++reg) {
        RegVal value;
        if (readMatrixMiscReg(reg, value)) {
            miscRegFile[reg] = value;
        }
    }
}

} // namespace RiscvISA
} // namespace gem5
