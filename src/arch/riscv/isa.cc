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

#include <ctime>
#include <set>
#include <sstream>

#include "arch/riscv/interrupts.hh"
#include "arch/riscv/mmu.hh"
#include "arch/riscv/pagetable.hh"
#include "arch/riscv/pmp.hh"
#include "arch/riscv/regs/float.hh"
#include "arch/riscv/regs/int.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/regs/renameable_misc.hh"
#include "arch/riscv/regs/vector.hh"
#include "base/bitfield.hh"
#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "base/types.hh"
#include "cpu/base.hh"
#include "debug/Checkpoint.hh"
#include "debug/FloatRegs.hh"
#include "debug/IntRegs.hh"
#include "debug/LLSC.hh"
#include "debug/MiscRegs.hh"
#include "debug/RiscvMisc.hh"
#include "debug/VecRegs.hh"
#include "mem/packet.hh"
#include "mem/request.hh"
#include "mem/se_translating_port_proxy.hh"
#include "mem/translating_port_proxy.hh"
#include "params/RiscvISA.hh"
#include "sim/core.hh"
#include "sim/faults.hh"
#include "sim/full_system.hh"
#include "sim/pseudo_inst.hh"

namespace gem5
{

namespace RiscvISA
{

namespace
{

Fault
matrixReadBlob(ThreadContext *tc, Addr addr, void *dst, size_t size)
{
    bool ok = false;
    if (FullSystem) {
        TranslatingPortProxy proxy(tc);
        ok = proxy.tryReadBlob(addr, dst, size);
    } else {
        SETranslatingPortProxy proxy(tc);
        ok = proxy.tryReadBlob(addr, dst, size);
    }

    if (!ok) {
        return std::make_shared<GenericPageTableFault>(addr);
    }
    return NoFault;
}

Fault
matrixWriteBlob(ThreadContext *tc, Addr addr, const void *src, size_t size)
{
    bool ok = false;
    if (FullSystem) {
        TranslatingPortProxy proxy(tc);
        ok = proxy.tryWriteBlob(addr, src, size);
    } else {
        SETranslatingPortProxy proxy(tc);
        ok = proxy.tryWriteBlob(addr, src, size);
    }

    if (!ok) {
        return std::make_shared<GenericPageTableFault>(addr);
    }
    return NoFault;
}

} // namespace

[[maybe_unused]] const std::array<const char *, NUM_MISC_AND_HELPER_REGS> MiscRegNames = {{
    [MISCREG_PRV]           = "PRV",
    [MISCREG_VIRMODE]         = "VIRTUALIZATIONMODE",
    [MISCREG_ISA]           = "ISA",
    [MISCREG_VENDORID]      = "VENDORID",
    [MISCREG_ARCHID]        = "ARCHID",
    [MISCREG_IMPID]         = "IMPID",
    [MISCREG_HARTID]        = "HARTID",
    [MISCREG_STATUS]        = "STATUS",
    [MISCREG_IP]            = "IP",
    [MISCREG_IE]            = "IE",
    [MISCREG_CYCLE]         = "CYCLE",
    [MISCREG_TIME]          = "TIME",
    [MISCREG_INSTRET]       = "INSTRET",
    [MISCREG_MHPMCOUNTER3]  = "MHPMCOUNTER3",
    [MISCREG_MHPMCOUNTER4]  = "MHPMCOUNTER4",
    [MISCREG_MHPMCOUNTER5]  = "MHPMCOUNTER5",
    [MISCREG_MHPMCOUNTER6]  = "MHPMCOUNTER6",
    [MISCREG_MHPMCOUNTER7]  = "MHPMCOUNTER7",
    [MISCREG_MHPMCOUNTER8]  = "MHPMCOUNTER8",
    [MISCREG_MHPMCOUNTER9]  = "MHPMCOUNTER9",
    [MISCREG_MHPMCOUNTER10]  = "MHPMCOUNTER10",
    [MISCREG_MHPMCOUNTER11]  = "MHPMCOUNTER11",
    [MISCREG_MHPMCOUNTER12]  = "MHPMCOUNTER12",
    [MISCREG_MHPMCOUNTER13]  = "MHPMCOUNTER13",
    [MISCREG_MHPMCOUNTER14]  = "MHPMCOUNTER14",
    [MISCREG_MHPMCOUNTER15]  = "MHPMCOUNTER15",
    [MISCREG_MHPMCOUNTER16]  = "MHPMCOUNTER16",
    [MISCREG_MHPMCOUNTER17]  = "MHPMCOUNTER17",
    [MISCREG_MHPMCOUNTER18]  = "MHPMCOUNTER18",
    [MISCREG_MHPMCOUNTER19]  = "MHPMCOUNTER19",
    [MISCREG_MHPMCOUNTER20]  = "MHPMCOUNTER20",
    [MISCREG_MHPMCOUNTER21]  = "MHPMCOUNTER21",
    [MISCREG_MHPMCOUNTER22]  = "MHPMCOUNTER22",
    [MISCREG_MHPMCOUNTER23]  = "MHPMCOUNTER23",
    [MISCREG_MHPMCOUNTER24]  = "MHPMCOUNTER24",
    [MISCREG_MHPMCOUNTER25]  = "MHPMCOUNTER25",
    [MISCREG_MHPMCOUNTER26]  = "MHPMCOUNTER26",
    [MISCREG_MHPMCOUNTER27]  = "MHPMCOUNTER27",
    [MISCREG_MHPMCOUNTER28]  = "MHPMCOUNTER28",
    [MISCREG_MHPMCOUNTER29]  = "MHPMCOUNTER29",
    [MISCREG_MHPMCOUNTER30]  = "MHPMCOUNTER30",
    [MISCREG_MHPMCOUNTER31]  = "MHPMCOUNTER31",
    [MISCREG_HPMCOUNTER03]  = "HPMCOUNTER03",
    [MISCREG_HPMCOUNTER04]  = "HPMCOUNTER04",
    [MISCREG_HPMCOUNTER05]  = "HPMCOUNTER05",
    [MISCREG_HPMCOUNTER06]  = "HPMCOUNTER06",
    [MISCREG_HPMCOUNTER07]  = "HPMCOUNTER07",
    [MISCREG_HPMCOUNTER08]  = "HPMCOUNTER08",
    [MISCREG_HPMCOUNTER09]  = "HPMCOUNTER09",
    [MISCREG_HPMCOUNTER10]  = "HPMCOUNTER10",
    [MISCREG_HPMCOUNTER11]  = "HPMCOUNTER11",
    [MISCREG_HPMCOUNTER12]  = "HPMCOUNTER12",
    [MISCREG_HPMCOUNTER13]  = "HPMCOUNTER13",
    [MISCREG_HPMCOUNTER14]  = "HPMCOUNTER14",
    [MISCREG_HPMCOUNTER15]  = "HPMCOUNTER15",
    [MISCREG_HPMCOUNTER16]  = "HPMCOUNTER16",
    [MISCREG_HPMCOUNTER17]  = "HPMCOUNTER17",
    [MISCREG_HPMCOUNTER18]  = "HPMCOUNTER18",
    [MISCREG_HPMCOUNTER19]  = "HPMCOUNTER19",
    [MISCREG_HPMCOUNTER20]  = "HPMCOUNTER20",
    [MISCREG_HPMCOUNTER21]  = "HPMCOUNTER21",
    [MISCREG_HPMCOUNTER22]  = "HPMCOUNTER22",
    [MISCREG_HPMCOUNTER23]  = "HPMCOUNTER23",
    [MISCREG_HPMCOUNTER24]  = "HPMCOUNTER24",
    [MISCREG_HPMCOUNTER25]  = "HPMCOUNTER25",
    [MISCREG_HPMCOUNTER26]  = "HPMCOUNTER26",
    [MISCREG_HPMCOUNTER27]  = "HPMCOUNTER27",
    [MISCREG_HPMCOUNTER28]  = "HPMCOUNTER28",
    [MISCREG_HPMCOUNTER29]  = "HPMCOUNTER29",
    [MISCREG_HPMCOUNTER30]  = "HPMCOUNTER30",
    [MISCREG_HPMCOUNTER31]  = "HPMCOUNTER31",
    [MISCREG_MCOUNTINHIBIT]  = "MCOUNTINHIBIT",
    [MISCREG_HPMEVENT03]    = "HPMEVENT03",
    [MISCREG_HPMEVENT04]    = "HPMEVENT04",
    [MISCREG_HPMEVENT05]    = "HPMEVENT05",
    [MISCREG_HPMEVENT06]    = "HPMEVENT06",
    [MISCREG_HPMEVENT07]    = "HPMEVENT07",
    [MISCREG_HPMEVENT08]    = "HPMEVENT08",
    [MISCREG_HPMEVENT09]    = "HPMEVENT09",
    [MISCREG_HPMEVENT10]    = "HPMEVENT10",
    [MISCREG_HPMEVENT11]    = "HPMEVENT11",
    [MISCREG_HPMEVENT12]    = "HPMEVENT12",
    [MISCREG_HPMEVENT13]    = "HPMEVENT13",
    [MISCREG_HPMEVENT14]    = "HPMEVENT14",
    [MISCREG_HPMEVENT15]    = "HPMEVENT15",
    [MISCREG_HPMEVENT16]    = "HPMEVENT16",
    [MISCREG_HPMEVENT17]    = "HPMEVENT17",
    [MISCREG_HPMEVENT18]    = "HPMEVENT18",
    [MISCREG_HPMEVENT19]    = "HPMEVENT19",
    [MISCREG_HPMEVENT20]    = "HPMEVENT20",
    [MISCREG_HPMEVENT21]    = "HPMEVENT21",
    [MISCREG_HPMEVENT22]    = "HPMEVENT22",
    [MISCREG_HPMEVENT23]    = "HPMEVENT23",
    [MISCREG_HPMEVENT24]    = "HPMEVENT24",
    [MISCREG_HPMEVENT25]    = "HPMEVENT25",
    [MISCREG_HPMEVENT26]    = "HPMEVENT26",
    [MISCREG_HPMEVENT27]    = "HPMEVENT27",
    [MISCREG_HPMEVENT28]    = "HPMEVENT28",
    [MISCREG_HPMEVENT29]    = "HPMEVENT29",
    [MISCREG_HPMEVENT30]    = "HPMEVENT30",
    [MISCREG_HPMEVENT31]    = "HPMEVENT31",


    [MISCREG_TSELECT]       = "TSELECT",
    [MISCREG_TDATA1]        = "TDATA1",
    [MISCREG_TDATA2]        = "TDATA2",
    [MISCREG_TDATA3]        = "TDATA3",
    [MISCREG_DCSR]          = "DCSR",
    [MISCREG_DPC]           = "DPC",
    [MISCREG_DSCRATCH]      = "DSCRATCH",

    [MISCREG_MEDELEG]       = "MEDELEG",
    [MISCREG_MIDELEG]       = "MIDELEG",
    [MISCREG_MTVEC]         = "MTVEC",
    [MISCREG_MCOUNTEREN]    = "MCOUNTEREN",
    [MISCREG_MENVCFG]       = "MENVCFG",
    [MISCREG_MSCRATCH]      = "MSCRATCH",
    [MISCREG_MEPC]          = "MEPC",
    [MISCREG_MCAUSE]        = "MCAUSE",
    [MISCREG_MTVAL]         = "MTVAL",
    [MISCREG_PMPCFG0]       = "PMPCFG0",
    // pmpcfg1 rv32 only
    [MISCREG_PMPCFG2]       = "PMPCFG2",
    // pmpcfg3 rv32 only
    [MISCREG_PMPADDR00]     = "PMPADDR00",
    [MISCREG_PMPADDR01]     = "PMPADDR01",
    [MISCREG_PMPADDR02]     = "PMPADDR02",
    [MISCREG_PMPADDR03]     = "PMPADDR03",
    [MISCREG_PMPADDR04]     = "PMPADDR04",
    [MISCREG_PMPADDR05]     = "PMPADDR05",
    [MISCREG_PMPADDR06]     = "PMPADDR06",
    [MISCREG_PMPADDR07]     = "PMPADDR07",
    [MISCREG_PMPADDR08]     = "PMPADDR08",
    [MISCREG_PMPADDR09]     = "PMPADDR09",
    [MISCREG_PMPADDR10]     = "PMPADDR10",
    [MISCREG_PMPADDR11]     = "PMPADDR11",
    [MISCREG_PMPADDR12]     = "PMPADDR12",
    [MISCREG_PMPADDR13]     = "PMPADDR13",
    [MISCREG_PMPADDR14]     = "PMPADDR14",
    [MISCREG_PMPADDR15]     = "PMPADDR15",

    [MISCREG_RESERVED01]    = "",
    [MISCREG_RESERVED02]    = "",
    [MISCREG_STVEC]         = "STVEC",
    [MISCREG_SCOUNTEREN]    = "SCOUNTEREN",
    [MISCREG_SSCRATCH]      = "SSCRATCH",
    [MISCREG_SEPC]          = "SEPC",
    [MISCREG_SCAUSE]        = "SCAUSE",
    [MISCREG_STVAL]         = "STVAL",
    [MISCREG_SATP]          = "SATP",

    [MISCREG_RESERVED03]    = "",
    [MISCREG_RESERVED04]    = "",
    [MISCREG_RESERVED05]    = "",
    [MISCREG_RESERVED06]    = "",
    [MISCREG_RESERVED07]    = "",
    [MISCREG_FFLAGS]        = "FFLAGS",
    [MISCREG_FRM]           = "FRM",

    [MISCREG_VSTART]        = "VSTART",
    [MISCREG_VXSAT]         = "VXSAT",
    [MISCREG_VXRM]          = "VXRM",
    [MISCREG_VCSR]          = "VCSR",
    [MISCREG_VL]            = "VL",
    [MISCREG_VTYPE]         = "VTYPE",
    [MISCREG_VLENB]         = "VLENB",

    [MISCREG_HSTATUS]       = "HSTATUS",
    [MISCREG_HEDELEG]       = "HEDELEG",
    [MISCREG_HIDELEG]       = "HIDELEG",
    [MISCREG_HIE]           = "HIE",
    [MISCREG_HCOUNTEREN]    = "HCOUNTEREN",
    [MISCREG_HGEIE]         = "HGEIE",
    [MISCREG_HTVAL]         = "HTVAL",
    [MISCREG_HIP]           = "HIP",
    [MISCREG_HVIP]          = "HVIP",
    [MISCREG_HTINST]        = "HTINST",
    [MISCREG_HGEIP]         = "HGEIP",
    [MISCREG_HENVCFG]       = "HENVCFG",
    [MISCREG_HGATP]         = "HGATP",
    [MISCREG_HTIMEDELTA]    = "HTIMEDELTA",
    [MISCREG_VSSTATUS]      = "VSSTATUS",
    [MISCREG_VSIE]          = "VSIE",
    [MISCREG_VSTVEC]        = "VSTVEC",
    [MISCREG_VSSCRATCH]     = "VSSCRATCH",
    [MISCREG_VSEPC]         = "VSEPC",
    [MISCREG_VSCAUSE]       = "VSCAUSE",
    [MISCREG_VSTVAL]        = "VSTVAL",
    [MISCREG_VSIP]          = "VSIP",
    [MISCREG_VSATP]         = "VSATP",
    [MISCREG_MTINST]        = "MTINST",
    [MISCREG_MTVAL2]        = "MTVAL2",


    [MISCREG_NMIVEC]        = "NMIVEC",
    [MISCREG_NMIE]          = "NMIE",
    [MISCREG_NMIP]          = "NMIP",
    [MISCREG_MSTATEEN0]     = "MSTATEEN0",
    [MISCREG_MSTATEEN1]     = "MSTATEEN1",
    [MISCREG_MSTATEEN2]     = "MSTATEEN2",
    [MISCREG_MSTATEEN3]     = "MSTATEEN3",

    [MISCREG_SENVCFG]       = "SENVCFG",
    [MISCREG_SSTATEEN0]     = "SSTATEEN0",
    [MISCREG_SSTATEEN1]     = "SSTATEEN1",
    [MISCREG_SSTATEEN2]     = "SSTATEEN2",
    [MISCREG_SSTATEEN3]     = "SSTATEEN3",

    [MISCREG_HSTATEEN0]     = "HSTATEEN0",
    [MISCREG_HSTATEEN1]     = "HSTATEEN1",
    [MISCREG_HSTATEEN2]     = "HSTATEEN2",
    [MISCREG_HSTATEEN3]     = "HSTATEEN3",
    [MISCREG_FFLAGS_EXE]    = "FFLAGS_EXE",
}};



ISA::ISA(const Params &p) : BaseISA(p)
{
    _regClasses.emplace_back(IntRegClass, int_reg::NumRegs, debug::IntRegs, sizeof(RegVal));
    _regClasses.emplace_back(FloatRegClass, float_reg::NumRegs, debug::FloatRegs, sizeof(RegVal));

    _regClasses.emplace_back(VecRegClass, NumVecRegs, debug::VecRegs, RiscvISA::VLENB);
    _regClasses.emplace_back(VecElemClass, NumVecElemPerVecReg * NumVecRegs, debug::VecRegs, sizeof(RegVal));
    _regClasses.emplace_back(VecPredRegClass, 1, debug::VecRegs, RiscvISA::VLENB);
    _regClasses.emplace_back(CCRegClass, 0, debug::IntRegs, sizeof(RegVal));
    _regClasses.emplace_back(RMiscRegClass,
                rmisc_reg::NumRegs, debug::MiscRegs, sizeof(RegVal));

    _regClasses.emplace_back(MiscRegClass, NUM_MISCREGS, debug::MiscRegs, sizeof(RegVal));

    miscRegFile.resize(NUM_MISCREGS);
    resetMatrixState();
    clear();
}

bool ISA::inUserMode() const
{
    return miscRegFile[MISCREG_PRV] == PRV_U;
}

void
ISA::copyRegsFrom(ThreadContext *src)
{
    // First loop through the integer registers.
    for (int i = 0; i < int_reg::NumRegs; ++i) {
        RegId reg(IntRegClass, i);
        tc->setReg(reg, src->getReg(reg));
    }

    // Second loop through the float registers.
    for (int i = 0; i < float_reg::NumRegs; ++i) {
        RegId reg(FloatRegClass, i);
        tc->setReg(reg, src->getReg(reg));
    }

    // TODO: Copy vector regs.

    // Lastly copy PC/NPC
    tc->pcState(src->pcState());
}

void ISA::clear()
{
    std::fill(miscRegFile.begin(), miscRegFile.end(), 0);
    resetMatrixState();

    miscRegFile[MISCREG_PRV] = PRV_M;
    miscRegFile[MISCREG_ISA] = 0x80000000003411af;
    miscRegFile[MISCREG_IMPID] = 0;
    miscRegFile[MISCREG_MIDELEG] = ((1 << 12) | (1 << 10) | (1 << 6) | (1 << 2));
    if (FullSystem) {
        // Xiangshan assume machine boots with FS off
        miscRegFile[MISCREG_STATUS] = (2ULL << UXL_OFFSET) | (2ULL << SXL_OFFSET);
    } else {
        // SE assumes process starts with FS on
        miscRegFile[MISCREG_STATUS] = (2ULL << UXL_OFFSET) | (2ULL << SXL_OFFSET) |
                                    (1ULL << FS_OFFSET);
    }
    if (FullSystem) {
        miscRegFile[MISCREG_MCOUNTEREN] = 0;
        miscRegFile[MISCREG_SCOUNTEREN] = 0;
    } else {
        // SE runs user-mode code without firmware or an OS to enable counters.
        miscRegFile[MISCREG_MCOUNTEREN] = 0x7;
        miscRegFile[MISCREG_SCOUNTEREN] = 0x7;
    }
    // don't set it to zero; software may try to determine the supported
    // triggers, starting at zero. simply set a different value here.
    miscRegFile[MISCREG_TSELECT] = 1;
    // NMI is always enabled.
    miscRegFile[MISCREG_NMIE] = 1;
    // sync with NEMU
    miscRegFile[MISCREG_VTYPE] = (1lu<<63);
    miscRegFile[MISCREG_HSTATUS] = (uint64_t)2<<32;
    miscRegFile[MISCREG_VSSTATUS] = miscRegFile[MISCREG_STATUS] & NEMU_SSTATUS_RMASK;
    miscRegFile[MISCREG_ARCHID] = 0x19;
    miscRegFile[MISCREG_VENDORID] = (16ULL << 7) | 0x6FULL;
}

void
ISA::resetMatrixState()
{
    matrixTileM = 0;
    matrixTileK = 0;
    matrixTileN = 0;
    matrixTileA.assign(MatrixTileABytes, 0);
    matrixTileB.assign(MatrixTileBBytes, 0);
    matrixAcc.assign(MatrixAccElems, 0);
    matrixTokens.assign(32, 0);
}

void
ISA::matrixSyncReset(uint64_t token_idx)
{
    matrixToken(token_idx) = 0;
}

void
ISA::matrixRelease(uint64_t token_idx)
{
    ++matrixToken(token_idx);
}

void
ISA::matrixAcquire(uint64_t token_idx, uint64_t target)
{
    panic_if(matrixToken(token_idx) < target,
        "macquire tok%u target=%llu observed=%llu",
        token_idx, target, matrixToken(token_idx));
}

void
ISA::setMatrixTileM(uint64_t value)
{
    matrixTileM = clampMatrixTileM(value);
}

void
ISA::setMatrixTileK(uint64_t value)
{
    matrixTileK = clampMatrixTileK(value);
}

void
ISA::setMatrixTileN(uint64_t value)
{
    matrixTileN = clampMatrixTileN(value);
}

Fault
ISA::matrixLoadA8(ExecContext *xc, Addr base, Addr stride)
{
    ThreadContext *tc = xc->tcBase();
    for (uint32_t row = 0; row < matrixTileM; ++row) {
        auto *dst = reinterpret_cast<uint8_t *>(&matrixTileA[row * MatrixMaxK]);
        Fault fault = matrixReadBlob(tc, base + row * stride, dst, matrixTileK);
        if (fault != NoFault) {
            return fault;
        }
    }
    return NoFault;
}

Fault
ISA::matrixLoadB8(ExecContext *xc, Addr base, Addr stride)
{
    ThreadContext *tc = xc->tcBase();
    for (uint32_t row = 0; row < matrixTileN; ++row) {
        auto *dst = reinterpret_cast<uint8_t *>(&matrixTileB[row * MatrixMaxK]);
        Fault fault = matrixReadBlob(tc, base + row * stride, dst, matrixTileK);
        if (fault != NoFault) {
            return fault;
        }
    }
    return NoFault;
}

Fault
ISA::matrixLoadC32(ExecContext *xc, Addr base, Addr stride)
{
    ThreadContext *tc = xc->tcBase();
    for (uint32_t row = 0; row < matrixTileM; ++row) {
        auto *dst = reinterpret_cast<uint8_t *>(&matrixAcc[row * MatrixMaxN]);
        Fault fault = matrixReadBlob(
            tc, base + row * stride, dst, matrixTileN * sizeof(int32_t));
        if (fault != NoFault) {
            return fault;
        }
    }
    return NoFault;
}

Fault
ISA::matrixStoreC32(ExecContext *xc, Addr base, Addr stride)
{
    ThreadContext *tc = xc->tcBase();
    for (uint32_t row = 0; row < matrixTileM; ++row) {
        auto *src = reinterpret_cast<uint8_t *>(&matrixAcc[row * MatrixMaxN]);
        Fault fault = matrixWriteBlob(
            tc, base + row * stride, src, matrixTileN * sizeof(int32_t));
        if (fault != NoFault) {
            return fault;
        }
    }
    return NoFault;
}

void
ISA::matrixZeroAcc()
{
    std::fill(matrixAcc.begin(), matrixAcc.end(), 0);
}

void
ISA::matrixMMAccWB()
{
    for (uint32_t m = 0; m < matrixTileM; ++m) {
        for (uint32_t n = 0; n < matrixTileN; ++n) {
            int32_t acc = matrixAcc[m * MatrixMaxN + n];
            for (uint32_t k = 0; k < matrixTileK; ++k) {
                int8_t a = matrixTileA[m * MatrixMaxK + k];
                int8_t b = matrixTileB[n * MatrixMaxK + k];
                acc += static_cast<int32_t>(a) * static_cast<int32_t>(b);
            }
            matrixAcc[m * MatrixMaxN + n] = acc;
        }
    }
}

bool
ISA::hpmCounterEnabled(int misc_reg) const
{
    int hpmcounter = misc_reg - MISCREG_CYCLE;
    if (hpmcounter < 0 || hpmcounter > 31)
        panic("Illegal HPM counter %d\n", hpmcounter);
    RegVal counteren;
    bool v = readMiscRegNoEffect(MISCREG_VIRMODE) == 1;
    switch (readMiscRegNoEffect(MISCREG_PRV)) {
      case PRV_M:
        return true;
      case PRV_S:
        counteren = miscRegFile[MISCREG_MCOUNTEREN];
        if (v) {
            counteren &= miscRegFile[MISCREG_HCOUNTEREN];
        }
        break;
      case PRV_U:
        counteren = miscRegFile[MISCREG_SCOUNTEREN] & miscRegFile[MISCREG_MCOUNTEREN];
        break;
      default:
        panic("Unknown privilege level %d\n", miscRegFile[MISCREG_PRV]);
        return false;
    }
    return (counteren & (1ULL << (hpmcounter))) > 0;
}

RegVal
ISA::readMiscRegNoEffect(int misc_reg) const
{
    if ((misc_reg >= RiscvISA::MiscRegIndex::MISCREG_PMPADDR00) &&
        (misc_reg < RiscvISA::MiscRegIndex::MISCREG_PMPADDR00 + 16)) {
        auto mmu = dynamic_cast<RiscvISA::MMU *>(tc->getMMUPtr());
        uint32_t pmp_index = misc_reg - MISCREG_PMPADDR00;
        uint64_t csr_num = mmu->getPMP()->pmpcfg_from_index(pmp_index);
        if (mmu->getPMP()->pmp_read_config(csr_num)) {
            return miscRegFile[misc_reg] | (~mmu->getPMP()->pmpTorMask() >> 1);
        } else {
            return miscRegFile[misc_reg] & (mmu->getPMP()->pmpTorMask());
        }
        return 0;
    } else if (misc_reg >= NUM_MISCREGS || misc_reg < 0) {
        // Illegal CSR
        panic("Illegal CSR index %#x\n", misc_reg);
        return -1;
    }
    DPRINTF(RiscvMisc, "Reading MiscReg %s (%d): %#x.\n",
            MiscRegNames[misc_reg], misc_reg, miscRegFile[misc_reg]);
    return miscRegFile[misc_reg];
}

RegVal
ISA::readMiscReg(int misc_reg)
{
    // VIRMODE is a plain (non-PMP, in-range) CSR, so its no-effect read is
    // just a register-file load; avoid the out-of-line call on this hot path.
    int v = miscRegFile[MISCREG_VIRMODE];
    if ((v == 1) && (misc_reg == MISCREG_SSCRATCH)) {
        return readMiscRegNoEffect(MISCREG_VSSCRATCH);
    }
    if ((v == 1) && (misc_reg == MISCREG_SATP)) {
        return readMiscRegNoEffect(MISCREG_VSATP);
    }
    if ((v == 1) && (misc_reg == MISCREG_SEPC)) {
        return readMiscRegNoEffect(MISCREG_VSEPC);
    }
    if ((v == 1) && (misc_reg == MISCREG_STVAL)) {
        return readMiscRegNoEffect(MISCREG_VSTVAL);
    }
    if ((v == 1) && (misc_reg == MISCREG_SCAUSE)) {
        return readMiscRegNoEffect(MISCREG_VSCAUSE);
    }
    if ((v == 1) && (misc_reg == MISCREG_STVEC)) {
        return readMiscRegNoEffect(MISCREG_VSTVEC);
    }
    if (misc_reg == MISCREG_HIE) {
        auto ic = dynamic_cast<RiscvISA::Interrupts *>(tc->getCpuPtr()->getInterruptController(tc->threadId()));
        DPRINTF(RiscvMisc, "Read IE value: %#lx.\n", ic->readIE());
        return ic->readIE() & NEMU_HIE_RMASK & (readMiscRegNoEffect(MISCREG_MIDELEG) | NEMU_MIDELEG_FORCED_MASK);
    }
    if (misc_reg == MISCREG_HIP) {
        auto ic = dynamic_cast<RiscvISA::Interrupts *>(tc->getCpuPtr()->getInterruptController(tc->threadId()));
        DPRINTF(RiscvMisc, "Read IE value: %#lx.\n", ic->readIE());
        return (ic->readIP() & NEMU_HIP_RMASK & (readMiscRegNoEffect(MISCREG_MIDELEG) | NEMU_MIDELEG_FORCED_MASK));
    }
    if (misc_reg == MISCREG_HVIP) {
        auto ic = dynamic_cast<RiscvISA::Interrupts *>(tc->getCpuPtr()->getInterruptController(tc->threadId()));
        return (ic->readIP() & NEMU_HVIP_MASK);
    }
    if (misc_reg == MISCREG_HIDELEG) {
        return readMiscRegNoEffect(MISCREG_HIDELEG) & NEMU_HIDELEG_MASK &
               readMiscRegNoEffect(MISCREG_MIDELEG);
    }
    if (misc_reg == MISCREG_VSIE) {
        auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                tc->getCpuPtr()->getInterruptController(tc->threadId()));
        RegVal mask = readMiscReg(MISCREG_HIDELEG) &
                      (readMiscRegNoEffect(MISCREG_MIDELEG) |
                       NEMU_MIDELEG_FORCED_MASK);
        return (ic->readIE() & mask & NEMU_VS_MASK) >> 1;
    }
    if (misc_reg == MISCREG_VSIP) {
        auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                tc->getCpuPtr()->getInterruptController(tc->threadId()));
        RegVal mask = readMiscReg(MISCREG_HIDELEG) &
                      (readMiscRegNoEffect(MISCREG_MIDELEG) |
                       NEMU_MIDELEG_FORCED_MASK);
        return (ic->readIP() & mask & NEMU_VS_MASK) >> 1;
    }
    switch (misc_reg) {
      case MISCREG_HARTID:
        return tc->contextId();
      case MISCREG_CYCLE:
        if (hpmCounterEnabled(MISCREG_CYCLE)) {
            DPRINTF(RiscvMisc, "Cycle counter at: %llu.\n",
                    tc->getCpuPtr()->curCycle());
            return tc->getCpuPtr()->curCycle();
        } else {
            return 0;
        }
      case MISCREG_TIME:
        if (hpmCounterEnabled(MISCREG_TIME)) {
            if (!FullSystem) {
                const uint64_t seTimebaseHz = 1000000;
                RegVal time = curTick() / (sim_clock::Frequency / seTimebaseHz);
                DPRINTF(RiscvMisc, "SE time counter at: %llu.\n", time);
                return time;
            } else {
                DPRINTF(RiscvMisc, "Wall-clock counter at: %llu.\n",
                        std::time(nullptr));
                return readMiscRegNoEffect(MISCREG_TIME);
            }
        } else {
            return 0;
        }
      case MISCREG_INSTRET:
        if (hpmCounterEnabled(MISCREG_INSTRET)) {
            DPRINTF(RiscvMisc, "Instruction counter at: %llu.\n",
                    tc->getCpuPtr()->totalInsts());
            return tc->getCpuPtr()->totalInsts();
        } else {
            return 0;
        }
      case MISCREG_IP:
        {
            auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
            return ic->readIP();
        }
      case MISCREG_IE:
        {
            auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
            DPRINTF(RiscvMisc, "Read IE value: %#lx.\n", ic->readIE());
            return ic->readIE();
        }
      case MISCREG_SEPC:
      case MISCREG_MEPC:
        {
            auto misa = readMiscRegNoEffect(MISCREG_ISA);
            auto val = readMiscRegNoEffect(misc_reg);
            // if compressed instructions are disabled, epc[1] is set to 0
            if ((misa & ISA_EXT_C_MASK) == 0)
                return mbits(val, 63, 2);
            // epc[0] is always 0
            else
                return mbits(val, 63, 1);
        }
      case MISCREG_VLENB:
        {
            return VLENB;
        }
        break;
      case MISCREG_VCSR:
        {
            return readMiscRegNoEffect(MISCREG_VXSAT) |
                  (readMiscRegNoEffect(MISCREG_VXRM) << 1);
        }
        break;
      case MISCREG_FFLAGS_EXE:
        {
            return readMiscRegNoEffect(MISCREG_FFLAGS) & FFLAGS_MASK;
        }
        break;
        case MISCREG_PMPADDR00 ... MISCREG_PMPADDR15:
        {
            return readMiscRegNoEffect(misc_reg);
        } break;
      default:
        // Try reading HPM counters
        // As a placeholder, all HPM counters are just cycle counters
        if (misc_reg >= MISCREG_HPMCOUNTER03 &&
                misc_reg <= MISCREG_HPMCOUNTER31) {
            if (hpmCounterEnabled(misc_reg)) {
                DPRINTF(RiscvMisc, "HPM counter %d: %llu.\n",
                        misc_reg - MISCREG_CYCLE, tc->getCpuPtr()->curCycle());
                return tc->getCpuPtr()->curCycle();
            } else {
                warn("HPM counter %d disabled.\n", misc_reg - MISCREG_CYCLE);
                return 0;
            }
        }
        // PMP address registers are handled by their own switch case above, so
        // any register reaching here is a plain in-range CSR whose no-effect
        // read is just a register-file load. Index directly to skip the
        // out-of-line call on this hot path.
        return miscRegFile[misc_reg];
    }
}

void
ISA::setMiscRegNoEffect(int misc_reg, RegVal val)
{
    if (misc_reg >= NUM_MISCREGS || misc_reg < 0) {
        // Illegal CSR
        panic("Illegal CSR index %#x\n", misc_reg);
    }
    DPRINTF(RiscvMisc, "Setting MiscReg %s (%d) to %#x.\n",
            MiscRegNames[misc_reg], misc_reg, val);
    miscRegFile[misc_reg] = val;
}

void
ISA::setMiscReg(int misc_reg, RegVal val)
{
    int v = readMiscReg(MISCREG_VIRMODE);
    if (misc_reg == MISCREG_STATUS) {
        DPRINTF(RiscvMisc, "setMiscReg: setting status with %#lx\n", val);
    }
    if (misc_reg == MISCREG_HSTATUS) {
        DPRINTF(RiscvMisc, "setMiscReg: setting hstatus with %#lx\n", val);
    }
    if (misc_reg == MISCREG_IE) {
        DPRINTF(RiscvMisc, "setMiscReg: setting mstatus with %#lx\n", val);
    }
    if (misc_reg >= MISCREG_CYCLE && misc_reg <= MISCREG_HPMCOUNTER31) {
        // Ignore writes to HPM counters for now
        if (misc_reg >= MISCREG_MHPMCOUNTER3 && misc_reg <= MISCREG_MHPMCOUNTER31) {
            warn("write to misc_reg %x val %lx but now write 0\n", misc_reg, val);
            setMiscRegNoEffect(misc_reg, 0);
        } else {
            warn("Ignoring write to %x\n", misc_reg);
        }
    } else if ((v == 1) && ((misc_reg == MISCREG_SSCRATCH))) {
        if (misc_reg == MISCREG_SSCRATCH) {
            setMiscRegNoEffect(MISCREG_VSSCRATCH, val);
        }

    } else if ((v == 1) && ((misc_reg == MISCREG_STATUS)) && (readMiscRegNoEffect(MISCREG_PRV) == PRV_S)) {
        auto vsstatus = readMiscRegNoEffect(MISCREG_VSSTATUS);
        STATUS write_val = ((vsstatus & ~(NEMU_SSTATUS_WMASK)) | (val & NEMU_SSTATUS_WMASK));
        bool fs_dirty = (write_val.fs == 0x3);
        bool vs_dirty = (write_val.vs == 0x3);
        uint64_t write_val2 = ((uint64_t)(fs_dirty || vs_dirty) << 63);
        write_val = write_val | write_val2;
        setMiscRegNoEffect(MISCREG_VSSTATUS, write_val);
    } else if ((v == 1) && (misc_reg == MISCREG_VSSTATUS)) {
        auto vsstatus = readMiscRegNoEffect(MISCREG_VSSTATUS);
        STATUS write_val = ((vsstatus & ~(NEMU_SSTATUS_WMASK)) | (val & NEMU_SSTATUS_WMASK));
        // if enable h
        bool fs_dirty = (write_val.fs == 0x3);
        bool vs_dirty = (write_val.vs == 0x3);
        uint64_t write_val2 = ((uint64_t)(fs_dirty || vs_dirty) << 63);
        write_val = write_val | write_val2;
        setMiscRegNoEffect(MISCREG_VSSTATUS, write_val);
    } else if ((v == 1) && ((misc_reg == MISCREG_SATP))) {
        auto satp_mode = (val & SATP_MODE_MASK) >> NEMU_SATP_RIGHT_OFFSET;
        if (satp_mode == NEMU_SATP_BARE) {
            setMiscRegNoEffect(MISCREG_VSATP, val & NEMU_SATP_MASK);
            warn("enable SATP BARE\n");
        } else if (satp_mode == NEMU_SATP_SV39) {
            setMiscRegNoEffect(MISCREG_VSATP, val & NEMU_SATP_MASK);
            warn("enable SV39\n");
        } else if (satp_mode == NEMU_SATP_SV48) {
            setMiscRegNoEffect(MISCREG_VSATP, val & NEMU_SATP_MASK);
            warn("enable SV48\n");
        }
    } else if ((v == 1) && (misc_reg == MISCREG_SEPC)) {
        setMiscRegNoEffect(MISCREG_VSEPC, val);
    } else if ((v == 1) && ((misc_reg == MISCREG_STVEC))) {
        setMiscRegNoEffect(MISCREG_VSTVEC, val & ~(0x2UL));
    } else if (misc_reg == MISCREG_VSSTATUS) {
        auto vsstatus = readMiscRegNoEffect(MISCREG_VSSTATUS);
        STATUS write_val = ((vsstatus & ~(NEMU_SSTATUS_WMASK)) | (val & NEMU_SSTATUS_WMASK));
        // if enable h
        bool fs_dirty = (write_val.fs == 0x3);
        bool vs_dirty = (write_val.vs == 0x3);
        uint64_t write_val2 = ((uint64_t)(fs_dirty || vs_dirty) << 63);
        write_val = write_val | write_val2;
        setMiscRegNoEffect(MISCREG_VSSTATUS, write_val);
    } else {
        switch (misc_reg) {

          // From section 3.7.1 of RISCV priv. specs
          // V1.12, the odd-numbered configuration
          // registers are illegal for RV64 and
          // each 64 bit CFG register hold configurations
          // for 8 PMP entries.

          case MISCREG_PMPCFG0:
          case MISCREG_PMPCFG2:
            {
                // PMP registers should only be modified in M mode
                assert(readMiscRegNoEffect(MISCREG_PRV) == PRV_M);

                // Specs do not seem to mention what should be
                // configured first, cfg or address regs!
                // qemu seems to update the tables when
                // pmp addr regs are written (with the assumption
                // that cfg regs are already written)

                for (int i=0; i < sizeof(val); i++) {

                    uint8_t cfg_val = (val >> (8*i)) & 0xff;
                    auto mmu = dynamic_cast<RiscvISA::MMU *>
                                (tc->getMMUPtr());

                    // Form pmp_index using the index i and
                    // PMPCFG register number
                    // Note: MISCREG_PMPCFG2 - MISCREG_PMPCFG0 = 1
                    // 8*(misc_reg-MISCREG_PMPCFG0) will be useful
                    // if a system contains more than 16 PMP entries
                    uint32_t pmp_index = i+(8*(misc_reg-MISCREG_PMPCFG0));
                    mmu->getPMP()->pmpUpdateCfg(pmp_index,cfg_val);
                }

                setMiscRegNoEffect(misc_reg, val);
            }
            break;
          case MISCREG_MCOUNTEREN:
          case MISCREG_SCOUNTEREN:
          case MISCREG_HCOUNTEREN:
            {
                auto xcounter = readMiscRegNoEffect(misc_reg);
                // The lower 32 bits are writable
                RegVal write_val = ((xcounter & ~(NEMU_COUNTER_MASK)) | (val & NEMU_COUNTER_MASK));
                setMiscRegNoEffect(misc_reg, write_val);
            }
            break;
          case MISCREG_PMPADDR00 ... MISCREG_PMPADDR15:
            {
                // PMP registers should only be modified in M mode
                assert(readMiscRegNoEffect(MISCREG_PRV) == PRV_M);

                auto mmu = dynamic_cast<RiscvISA::MMU *>
                              (tc->getMMUPtr());
                uint32_t pmp_index = misc_reg-MISCREG_PMPADDR00;
                RegVal write_val = val & (((uint64_t)1 << (36 - 2)) - 1);
                uint64_t csr_num = mmu->getPMP()->pmpcfg_from_index(pmp_index);
                uint64_t cfg = miscRegFile[csr_num];

                mmu->getPMP()->pmpUpdateAddr(pmp_index, val);

                setMiscRegNoEffect(misc_reg, write_val);
            }
            break;
            case MISCREG_HVIP: {
                auto ic =
                    dynamic_cast<RiscvISA::Interrupts *>(tc->getCpuPtr()->getInterruptController(tc->threadId()));
                auto old = readMiscReg(MISCREG_IP);
                RegVal writeVal = ((old & ~(NEMU_HVIP_MASK)) | (val & NEMU_HVIP_MASK));
                ic->setIP(writeVal);
            } break;
            case MISCREG_VSIP: {
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                RegVal old = readMiscReg(MISCREG_IP);
                RegVal mask = NEMU_VSIP_WMASK & readMiscReg(MISCREG_HIDELEG) &
                              (readMiscReg(MISCREG_MIDELEG) |
                               NEMU_MIDELEG_FORCED_MASK);
                RegVal writeVal = (old & ~mask) | ((val << 1) & mask);
                ic->setIP(writeVal);
            } break;

          case MISCREG_IP:
            {
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                DPRINTF(RiscvMisc, "Setting IP to %#lx.\n", val);
                auto old = readMiscReg(MISCREG_IP);
                RegVal writeVal = ((old & ~(NEMU_MIP_MASK)) | (val & NEMU_MIP_MASK));
                ic->setIP(writeVal);
            }
            break;
          case MISCREG_HIE:
            {
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                RegVal write_val =0;
                RegVal old = readMiscReg(MISCREG_IE);
                RegVal hip_Mask = NEMU_HIE_WMASK & (readMiscReg(MISCREG_MIDELEG) | NEMU_MIDELEG_FORCED_MASK);
                write_val = ((old & ~(hip_Mask)) |(val & hip_Mask));
                ic->setIE(write_val);
            }
            break;
          case MISCREG_VSIE:
            {
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                RegVal old = readMiscReg(MISCREG_IE);
                RegVal mask = NEMU_VS_MASK & readMiscReg(MISCREG_HIDELEG) &
                              (readMiscReg(MISCREG_MIDELEG) |
                               NEMU_MIDELEG_FORCED_MASK);
                RegVal writeVal = (old & ~mask) | ((val << 1) & mask);
                ic->setIE(writeVal);
            }
            break;
          case MISCREG_IE:
            {
                auto ic = dynamic_cast<RiscvISA::Interrupts *>(
                    tc->getCpuPtr()->getInterruptController(tc->threadId()));
                DPRINTF(RiscvMisc, "Setting IE to %#lx.\n", val);
                ic->setIE(val);
            }
            break;
          case MISCREG_SATP:
            {
                // we only support bare and Sv39 mode; setting a different mode
                // shall have no effect (see 4.1.12 in priv ISA manual)
                SATP cur_val = readMiscRegNoEffect(misc_reg);
                SATP new_val = val;
                if (cur_val != new_val) {
                    tc->getCpuPtr()->flushTLBs();
                }
                auto satp_mode = (val & SATP_MODE_MASK) >> NEMU_SATP_RIGHT_OFFSET;
                RegVal writeVal = val & NEMU_SATP_MASK;
                if (satp_mode == NEMU_SATP_BARE) {
                    setMiscRegNoEffect(misc_reg, writeVal);
                    warn("enable SATP BARE\n");
                } else if (satp_mode == NEMU_SATP_SV39) {
                    setMiscRegNoEffect(misc_reg, writeVal);
                    warn("enable SV39\n");
                } else if (satp_mode == NEMU_SATP_SV48) {
                    setMiscRegNoEffect(misc_reg, writeVal);
                    warn("enable SV48\n");
                }

            }
            break;
          case MISCREG_TSELECT:
            {
                // we don't support debugging, so always set a different value
                // than written
                setMiscRegNoEffect(misc_reg, val + 1);
            }
            break;
          case MISCREG_ISA:
            break;
          case MISCREG_STATUS:
            {
                // Match NEMU CSR semantics: only writable MSTATUS fields update.
                auto cur = readMiscRegNoEffect(misc_reg);
                STATUS mstatus =
                    ((cur & ~(NEMU_MSTATUS_WMASK)) | (val & NEMU_MSTATUS_WMASK));
                mstatus.sd = mstatus.fs == 0x3 || mstatus.vs == 0x3;
                setMiscRegNoEffect(misc_reg, mstatus);
            }
            break;
          case MISCREG_FFLAGS_EXE:
            {
                DPRINTF(RiscvMisc, "Will set fs\n");
                STATUS mstatus = readMiscRegNoEffect(MISCREG_STATUS);
                mstatus.fs = 3;
                mstatus.sd = 1;
                setMiscRegNoEffect(MISCREG_STATUS, mstatus);

                RegVal fflags = readMiscRegNoEffect(MISCREG_FFLAGS);
                fflags |= (val & FFLAGS_MASK);
                setMiscRegNoEffect(MISCREG_FFLAGS, fflags);
            }
            break;
            case MISCREG_FFLAGS:
            case MISCREG_FRM:
            {
                DPRINTF(RiscvMisc, "Will set fs\n");
                STATUS mstatus = readMiscRegNoEffect(MISCREG_STATUS);
                mstatus.fs = 3;
                mstatus.sd = 1;
                setMiscRegNoEffect(MISCREG_STATUS, mstatus);
                setMiscRegNoEffect(misc_reg, val);
            }
            break;
          case MISCREG_VXSAT:
            {
                DPRINTF(RiscvMisc, "Will set vs\n");
                STATUS mstatus = readMiscRegNoEffect(MISCREG_STATUS);
                mstatus.vs = 3;
                mstatus.sd = 1;
                setMiscRegNoEffect(MISCREG_STATUS, mstatus);

                setMiscRegNoEffect(misc_reg, val & 0x1);
            }
            break;
          case MISCREG_VXRM:
            {
                DPRINTF(RiscvMisc, "Will set vs\n");
                STATUS mstatus = readMiscRegNoEffect(MISCREG_STATUS);
                mstatus.vs = 3;
                mstatus.sd = 1;
                setMiscRegNoEffect(MISCREG_STATUS, mstatus);

                setMiscRegNoEffect(misc_reg, val & 0x3);
            }
            break;
          case MISCREG_VCSR:
            {
                DPRINTF(RiscvMisc, "Will set vs\n");
                STATUS mstatus = readMiscRegNoEffect(MISCREG_STATUS);
                mstatus.vs = 3;
                mstatus.sd = 1;
                setMiscRegNoEffect(MISCREG_STATUS, mstatus);

                setMiscRegNoEffect(MISCREG_VXSAT, val & 0x1);
                setMiscRegNoEffect(MISCREG_VXRM, (val & 0x6) >> 1);
            }
            break;
          case MISCREG_VTYPE:
            {
                DPRINTF(RiscvMisc, "Will set vs\n");
                STATUS mstatus = readMiscRegNoEffect(MISCREG_STATUS);
                mstatus.vs = 3;
                mstatus.sd = 1;
                setMiscRegNoEffect(MISCREG_STATUS, mstatus);
                setMiscRegNoEffect(misc_reg, val);
            }
            break;
          case MISCREG_MIDELEG:
            {
               RegVal writeVal = val|((1 << 12) | (1 << 10) | (1 << 6) | (1 << 2));
               setMiscRegNoEffect(misc_reg, writeVal);
            }
            break;
          case MISCREG_HIDELEG:
            {
                RegVal writeVal = val & NEMU_HIDELEG_MASK;
                setMiscRegNoEffect(misc_reg, writeVal);
            }
            break;
          case MISCREG_SENVCFG:
            setMiscRegNoEffect(misc_reg, val & NEMU_SENVCFG_WMASK);
            break;
          case MISCREG_HSTATUS:
            {
                RegVal oldVal = readMiscRegNoEffect(MISCREG_HSTATUS);
                RegVal writeVal = (oldVal & ~HSTATUS_MASK)|(val & HSTATUS_MASK);
                setMiscRegNoEffect(misc_reg, writeVal);
            }
            break;
          default:
            setMiscRegNoEffect(misc_reg, val);
        }
    }
}

void
ISA::serialize(CheckpointOut &cp) const
{
    DPRINTF(Checkpoint, "Serializing Riscv Misc Registers\n");
    SERIALIZE_CONTAINER(miscRegFile);
    SERIALIZE_SCALAR(matrixTileM);
    SERIALIZE_SCALAR(matrixTileK);
    SERIALIZE_SCALAR(matrixTileN);
    SERIALIZE_CONTAINER(matrixTileA);
    SERIALIZE_CONTAINER(matrixTileB);
    SERIALIZE_CONTAINER(matrixAcc);
    SERIALIZE_CONTAINER(matrixTokens);
}

void
ISA::unserialize(CheckpointIn &cp)
{
    DPRINTF(Checkpoint, "Unserializing Riscv Misc Registers\n");
    UNSERIALIZE_CONTAINER(miscRegFile);
    if (miscRegFile.size() < NUM_MISCREGS)
        miscRegFile.resize(NUM_MISCREGS, 0);
    UNSERIALIZE_SCALAR(matrixTileM);
    UNSERIALIZE_SCALAR(matrixTileK);
    UNSERIALIZE_SCALAR(matrixTileN);
    UNSERIALIZE_CONTAINER(matrixTileA);
    UNSERIALIZE_CONTAINER(matrixTileB);
    UNSERIALIZE_CONTAINER(matrixAcc);
    UNSERIALIZE_CONTAINER(matrixTokens);
}

RegVal &
ISA::matrixToken(size_t idx)
{
    panic_if(idx >= matrixTokens.size(), "matrix token index %u out of range",
        idx);
    return matrixTokens[idx];
}

const RegVal &
ISA::matrixToken(size_t idx) const
{
    panic_if(idx >= matrixTokens.size(), "matrix token index %u out of range",
        idx);
    return matrixTokens[idx];
}

const int WARN_FAILURE = 10000;

const Addr INVALID_RESERVATION_ADDR = (Addr) -1;
std::unordered_map<int, Addr> load_reservation_addrs;

void
ISA::handleLockedSnoop(PacketPtr pkt, Addr cacheBlockMask)
{
    Addr& load_reservation_addr = load_reservation_addrs[tc->contextId()];

    if (load_reservation_addr == INVALID_RESERVATION_ADDR)
        return;
    Addr snoop_addr = pkt->getAddr() & cacheBlockMask;
    DPRINTF(LLSC, "Locked snoop on address %x.\n", snoop_addr);
    if ((load_reservation_addr & cacheBlockMask) == snoop_addr)
        load_reservation_addr = INVALID_RESERVATION_ADDR;
}


void
ISA::handleLockedRead(const RequestPtr &req)
{
    Addr& load_reservation_addr = load_reservation_addrs[tc->contextId()];

    load_reservation_addr = req->getPaddr() & ~0xF;
    DPRINTF(LLSC, "[cid:%d]: Reserved address %x.\n",
            req->contextId(), req->getPaddr() & ~0xF);
}

bool
ISA::handleLockedWrite(const RequestPtr &req, Addr cacheBlockMask)
{
    Addr& load_reservation_addr = load_reservation_addrs[tc->contextId()];
    bool lr_addr_empty = (load_reservation_addr == INVALID_RESERVATION_ADDR);

    // Normally RISC-V uses zero to indicate success and nonzero to indicate
    // failure (right now only 1 is reserved), but in gem5 zero indicates
    // failure and one indicates success, so here we conform to that (it should
    // be switched in the instruction's implementation)

    DPRINTF(LLSC, "[cid:%d]: load_reservation_addrs empty? %s.\n",
            req->contextId(),
            lr_addr_empty ? "yes" : "no");
    if (!lr_addr_empty) {
        DPRINTF(LLSC, "[cid:%d]: addr = %x.\n", req->contextId(),
                req->getPaddr() & ~0xF);
        DPRINTF(LLSC, "[cid:%d]: last locked addr = %x.\n", req->contextId(),
                load_reservation_addr);
    }
    if (lr_addr_empty
            || load_reservation_addr != ((req->getPaddr() & ~0xF))) {
        req->setExtraData(0);
        int stCondFailures = tc->readStCondFailures();
        tc->setStCondFailures(++stCondFailures);
        if (stCondFailures % WARN_FAILURE == 0) {
            warn("%i: context %d: %d consecutive SC failures.\n",
                    curTick(), tc->contextId(), stCondFailures);
        }
        return false;
    }
    if (req->isUncacheable()) {
        req->setExtraData(2);
    }

    return true;
}

void
ISA::globalClearExclusive()
{
    tc->getCpuPtr()->wakeup(tc->threadId());
}

} // namespace RiscvISA
} // namespace gem5

std::ostream &
operator<<(std::ostream &os, gem5::RiscvISA::PrivilegeMode pm)
{
    switch (pm) {
    case gem5::RiscvISA::PRV_U:
        return os << "PRV_U";
    case gem5::RiscvISA::PRV_S:
        return os << "PRV_S";
    case gem5::RiscvISA::PRV_HS:
        return os << "PRV_HS";
    case gem5::RiscvISA::PRV_M:
        return os << "PRV_M";
    }
    return os << "PRV_<invalid>";
}
