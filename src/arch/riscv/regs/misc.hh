/*
 * Copyright (c) 2013 ARM Limited
 * Copyright (c) 2014-2015 Sven Karlsson
 * Copyright (c) 2019 Yifei Liu
 * Copyright (c) 2020 Barkhausen Institut
 * Copyright (c) 2021 StreamComputing Corp
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2016 RISC-V Foundation
 * Copyright (c) 2016 The University of Virginia
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

#ifndef __ARCH_RISCV_REGS_MISC_HH__
#define __ARCH_RISCV_REGS_MISC_HH__

#include <map>
#include <string>

#include "arch/generic/vec_pred_reg.hh"
#include "arch/generic/vec_reg.hh"
#include "base/bitunion.hh"
#include "base/types.hh"

namespace gem5
{

namespace RiscvISA
{

enum MiscRegIndex
{
    MISCREG_PRV = 0,
    MISCREG_VIRMODE,
    MISCREG_ISA,
    MISCREG_VENDORID,
    MISCREG_ARCHID,
    MISCREG_IMPID,
    MISCREG_HARTID,
    MISCREG_STATUS,
    MISCREG_IP,
    MISCREG_IE,
    MISCREG_CYCLE,
    MISCREG_TIME,
    MISCREG_INSTRET,
    MISCREG_MHPMCOUNTER3,
    MISCREG_MHPMCOUNTER4,
    MISCREG_MHPMCOUNTER5,
    MISCREG_MHPMCOUNTER6,
    MISCREG_MHPMCOUNTER7,
    MISCREG_MHPMCOUNTER8,
    MISCREG_MHPMCOUNTER9,
    MISCREG_MHPMCOUNTER10,
    MISCREG_MHPMCOUNTER11,
    MISCREG_MHPMCOUNTER12,
    MISCREG_MHPMCOUNTER13,
    MISCREG_MHPMCOUNTER14,
    MISCREG_MHPMCOUNTER15,
    MISCREG_MHPMCOUNTER16,
    MISCREG_MHPMCOUNTER17,
    MISCREG_MHPMCOUNTER18,
    MISCREG_MHPMCOUNTER19,
    MISCREG_MHPMCOUNTER20,
    MISCREG_MHPMCOUNTER21,
    MISCREG_MHPMCOUNTER22,
    MISCREG_MHPMCOUNTER23,
    MISCREG_MHPMCOUNTER24,
    MISCREG_MHPMCOUNTER25,
    MISCREG_MHPMCOUNTER26,
    MISCREG_MHPMCOUNTER27,
    MISCREG_MHPMCOUNTER28,
    MISCREG_MHPMCOUNTER29,
    MISCREG_MHPMCOUNTER30,
    MISCREG_MHPMCOUNTER31,
    MISCREG_HPMCOUNTER03,
    MISCREG_HPMCOUNTER04,
    MISCREG_HPMCOUNTER05,
    MISCREG_HPMCOUNTER06,
    MISCREG_HPMCOUNTER07,
    MISCREG_HPMCOUNTER08,
    MISCREG_HPMCOUNTER09,
    MISCREG_HPMCOUNTER10,
    MISCREG_HPMCOUNTER11,
    MISCREG_HPMCOUNTER12,
    MISCREG_HPMCOUNTER13,
    MISCREG_HPMCOUNTER14,
    MISCREG_HPMCOUNTER15,
    MISCREG_HPMCOUNTER16,
    MISCREG_HPMCOUNTER17,
    MISCREG_HPMCOUNTER18,
    MISCREG_HPMCOUNTER19,
    MISCREG_HPMCOUNTER20,
    MISCREG_HPMCOUNTER21,
    MISCREG_HPMCOUNTER22,
    MISCREG_HPMCOUNTER23,
    MISCREG_HPMCOUNTER24,
    MISCREG_HPMCOUNTER25,
    MISCREG_HPMCOUNTER26,
    MISCREG_HPMCOUNTER27,
    MISCREG_HPMCOUNTER28,
    MISCREG_HPMCOUNTER29,
    MISCREG_HPMCOUNTER30,
    MISCREG_HPMCOUNTER31,
    MISCREG_MCOUNTINHIBIT,
    MISCREG_HPMEVENT03,
    MISCREG_HPMEVENT04,
    MISCREG_HPMEVENT05,
    MISCREG_HPMEVENT06,
    MISCREG_HPMEVENT07,
    MISCREG_HPMEVENT08,
    MISCREG_HPMEVENT09,
    MISCREG_HPMEVENT10,
    MISCREG_HPMEVENT11,
    MISCREG_HPMEVENT12,
    MISCREG_HPMEVENT13,
    MISCREG_HPMEVENT14,
    MISCREG_HPMEVENT15,
    MISCREG_HPMEVENT16,
    MISCREG_HPMEVENT17,
    MISCREG_HPMEVENT18,
    MISCREG_HPMEVENT19,
    MISCREG_HPMEVENT20,
    MISCREG_HPMEVENT21,
    MISCREG_HPMEVENT22,
    MISCREG_HPMEVENT23,
    MISCREG_HPMEVENT24,
    MISCREG_HPMEVENT25,
    MISCREG_HPMEVENT26,
    MISCREG_HPMEVENT27,
    MISCREG_HPMEVENT28,
    MISCREG_HPMEVENT29,
    MISCREG_HPMEVENT30,
    MISCREG_HPMEVENT31,


    MISCREG_TSELECT,
    MISCREG_TDATA1,
    MISCREG_TDATA2,
    MISCREG_TDATA3,
    MISCREG_DCSR,
    MISCREG_DPC,
    MISCREG_DSCRATCH,

    MISCREG_MEDELEG,
    MISCREG_MIDELEG,
    MISCREG_MTVEC,
    MISCREG_MCOUNTEREN,
    MISCREG_MENVCFG,
    MISCREG_MSCRATCH,
    MISCREG_MEPC,
    MISCREG_MCAUSE,
    MISCREG_MTVAL,
    MISCREG_PMPCFG0,
    // pmpcfg1 rv32 only
    MISCREG_PMPCFG2,
    // pmpcfg3 rv32 only
    MISCREG_PMPADDR00,
    MISCREG_PMPADDR01,
    MISCREG_PMPADDR02,
    MISCREG_PMPADDR03,
    MISCREG_PMPADDR04,
    MISCREG_PMPADDR05,
    MISCREG_PMPADDR06,
    MISCREG_PMPADDR07,
    MISCREG_PMPADDR08,
    MISCREG_PMPADDR09,
    MISCREG_PMPADDR10,
    MISCREG_PMPADDR11,
    MISCREG_PMPADDR12,
    MISCREG_PMPADDR13,
    MISCREG_PMPADDR14,
    MISCREG_PMPADDR15,

    MISCREG_SEDELEG,
    MISCREG_SIDELEG,
    MISCREG_STVEC,
    MISCREG_SCOUNTEREN,
    MISCREG_SSCRATCH,
    MISCREG_SEPC,
    MISCREG_SCAUSE,
    MISCREG_STVAL,
    MISCREG_SATP,

    MISCREG_UTVEC,
    MISCREG_USCRATCH,
    MISCREG_UEPC,
    MISCREG_UCAUSE,
    MISCREG_UTVAL,
    MISCREG_FFLAGS,
    MISCREG_FRM,

    MISCREG_VSTART,
    MISCREG_VXSAT,
    MISCREG_VXRM,
    MISCREG_VCSR,
    MISCREG_VL,
    MISCREG_VTYPE,
    MISCREG_VLENB,

    MISCREG_HSTATUS,
    MISCREG_HEDELEG,
    MISCREG_HIDELEG,
    MISCREG_HIE,
    MISCREG_HCOUNTEREN,
    MISCREG_HGEIE,
    MISCREG_HTVAL,
    MISCREG_HIP,
    MISCREG_HVIP,
    MISCREG_HTINST,
    MISCREG_HGEIP,
    MISCREG_HENVCFG,
    MISCREG_HGATP,
    MISCREG_HTIMEDELTA,
    MISCREG_VSSTATUS,
    MISCREG_VSIE,
    MISCREG_VSTVEC,
    MISCREG_VSSCRATCH,
    MISCREG_VSEPC,
    MISCREG_VSCAUSE,
    MISCREG_VSTVAL,
    MISCREG_VSIP,
    MISCREG_VSATP,
    MISCREG_MTINST,
    MISCREG_MTVAL2,

    // CSRData map. These are mainly used to provide a minimal implementation
    // for non-maskable-interrupt in our simple cpu.
    // non-maskable-interrupt-vector-base-address: NMI version of xTVEC
    MISCREG_NMIVEC,
    // non-maskable-interrupt-enable: NMI version of xIE
    MISCREG_NMIE,
    // non-maskable-interrupt-pending: NMI version of xIP
    MISCREG_NMIP,

    NUM_MISCREGS
};

enum CSRIndex
{
    CSR_USTATUS = 0x000,
    CSR_UIE = 0x004,
    CSR_UTVEC = 0x005,
    CSR_USCRATCH = 0x040,
    CSR_UEPC = 0x041,
    CSR_UCAUSE = 0x042,
    CSR_UTVAL = 0x043,
    CSR_UIP = 0x044,
    CSR_FFLAGS = 0x001,
    CSR_FRM = 0x002,
    CSR_FCSR = 0x003,
    CSR_CYCLE = 0xC00,
    CSR_TIME = 0xC01,
    CSR_INSTRET = 0xC02,
    CSR_HPMCOUNTER03 = 0xC03,
    CSR_HPMCOUNTER04 = 0xC04,
    CSR_HPMCOUNTER05 = 0xC05,
    CSR_HPMCOUNTER06 = 0xC06,
    CSR_HPMCOUNTER07 = 0xC07,
    CSR_HPMCOUNTER08 = 0xC08,
    CSR_HPMCOUNTER09 = 0xC09,
    CSR_HPMCOUNTER10 = 0xC0A,
    CSR_HPMCOUNTER11 = 0xC0B,
    CSR_HPMCOUNTER12 = 0xC0C,
    CSR_HPMCOUNTER13 = 0xC0D,
    CSR_HPMCOUNTER14 = 0xC0E,
    CSR_HPMCOUNTER15 = 0xC0F,
    CSR_HPMCOUNTER16 = 0xC10,
    CSR_HPMCOUNTER17 = 0xC11,
    CSR_HPMCOUNTER18 = 0xC12,
    CSR_HPMCOUNTER19 = 0xC13,
    CSR_HPMCOUNTER20 = 0xC14,
    CSR_HPMCOUNTER21 = 0xC15,
    CSR_HPMCOUNTER22 = 0xC16,
    CSR_HPMCOUNTER23 = 0xC17,
    CSR_HPMCOUNTER24 = 0xC18,
    CSR_HPMCOUNTER25 = 0xC19,
    CSR_HPMCOUNTER26 = 0xC1A,
    CSR_HPMCOUNTER27 = 0xC1B,
    CSR_HPMCOUNTER28 = 0xC1C,
    CSR_HPMCOUNTER29 = 0xC1D,
    CSR_HPMCOUNTER30 = 0xC1E,
    CSR_HPMCOUNTER31 = 0xC1F,
    // HPMCOUNTERH rv32 only

    CSR_SSTATUS = 0x100,
    CSR_SEDELEG = 0x102,
    CSR_SIDELEG = 0x103,
    CSR_SIE = 0x104,
    CSR_STVEC = 0x105,
    CSR_SCOUNTEREN = 0x106,
    CSR_SSCRATCH = 0x140,
    CSR_SEPC = 0x141,
    CSR_SCAUSE = 0x142,
    CSR_STVAL = 0x143,
    CSR_SIP = 0x144,
    CSR_SATP = 0x180,

    CSR_MVENDORID = 0xF11,
    CSR_MARCHID = 0xF12,
    CSR_MIMPID = 0xF13,
    CSR_MHARTID = 0xF14,
    CSR_MSTATUS = 0x300,
    CSR_MISA = 0x301,
    CSR_MEDELEG = 0x302,
    CSR_MIDELEG = 0x303,
    CSR_MIE = 0x304,
    CSR_MTVEC = 0x305,
    CSR_MCOUNTEREN = 0x306,
    CSR_MENVCFG = 0x30A,
    CSR_MSCRATCH = 0x340,
    CSR_MEPC = 0x341,
    CSR_MCAUSE = 0x342,
    CSR_MTVAL = 0x343,
    CSR_MIP = 0x344,
    CSR_PMPCFG0 = 0x3A0,
    // pmpcfg1 rv32 only
    CSR_PMPCFG2 = 0x3A2,
    // pmpcfg3 rv32 only
    CSR_PMPADDR00 = 0x3B0,
    CSR_PMPADDR01 = 0x3B1,
    CSR_PMPADDR02 = 0x3B2,
    CSR_PMPADDR03 = 0x3B3,
    CSR_PMPADDR04 = 0x3B4,
    CSR_PMPADDR05 = 0x3B5,
    CSR_PMPADDR06 = 0x3B6,
    CSR_PMPADDR07 = 0x3B7,
    CSR_PMPADDR08 = 0x3B8,
    CSR_PMPADDR09 = 0x3B9,
    CSR_PMPADDR10 = 0x3BA,
    CSR_PMPADDR11 = 0x3BB,
    CSR_PMPADDR12 = 0x3BC,
    CSR_PMPADDR13 = 0x3BD,
    CSR_PMPADDR14 = 0x3BE,
    CSR_PMPADDR15 = 0x3BF,
    CSR_MCYCLE = 0xB00,
    CSR_MINSTRET = 0xB02,
    CSR_MMHPMCOUNTER3 = 0xB03,
    CSR_MMHPMCOUNTER4 = 0xB04,
    CSR_MMHPMCOUNTER5 = 0xB05,
    CSR_MMHPMCOUNTER6 = 0xB06,
    CSR_MMHPMCOUNTER7 = 0xB07,
    CSR_MMHPMCOUNTER8 = 0xB08,
    CSR_MMHPMCOUNTER9 = 0xB09,
    CSR_MMHPMCOUNTER10 = 0xB0A,
    CSR_MMHPMCOUNTER11 = 0xB0B,
    CSR_MMHPMCOUNTER12 = 0xB0C,
    CSR_MMHPMCOUNTER13 = 0xB0D,
    CSR_MMHPMCOUNTER14 = 0xB0E,
    CSR_MMHPMCOUNTER15 = 0xB0F,
    CSR_MMHPMCOUNTER16 = 0xB10,
    CSR_MMHPMCOUNTER17 = 0xB11,
    CSR_MMHPMCOUNTER18 = 0xB12,
    CSR_MMHPMCOUNTER19 = 0xB13,
    CSR_MMHPMCOUNTER20 = 0xB14,
    CSR_MMHPMCOUNTER21 = 0xB15,
    CSR_MMHPMCOUNTER22 = 0xB16,
    CSR_MMHPMCOUNTER23 = 0xB17,
    CSR_MMHPMCOUNTER24 = 0xB18,
    CSR_MMHPMCOUNTER25 = 0xB19,
    CSR_MMHPMCOUNTER26 = 0xB1A,
    CSR_MMHPMCOUNTER27 = 0xB1B,
    CSR_MMHPMCOUNTER28 = 0xB1C,
    CSR_MMHPMCOUNTER29 = 0xB1D,
    CSR_MMHPMCOUNTER30 = 0xB1E,
    CSR_MMHPMCOUNTER31 = 0xB1F,
    CSR_MHPMCOUNTER03 = 0xC03,
    CSR_MHPMCOUNTER04 = 0xC04,
    CSR_MHPMCOUNTER05 = 0xC05,
    CSR_MHPMCOUNTER06 = 0xC06,
    CSR_MHPMCOUNTER07 = 0xC07,
    CSR_MHPMCOUNTER08 = 0xC08,
    CSR_MHPMCOUNTER09 = 0xC09,
    CSR_MHPMCOUNTER10 = 0xC0A,
    CSR_MHPMCOUNTER11 = 0xC0B,
    CSR_MHPMCOUNTER12 = 0xC0C,
    CSR_MHPMCOUNTER13 = 0xC0D,
    CSR_MHPMCOUNTER14 = 0xC0E,
    CSR_MHPMCOUNTER15 = 0xC0F,
    CSR_MHPMCOUNTER16 = 0xC10,
    CSR_MHPMCOUNTER17 = 0xC11,
    CSR_MHPMCOUNTER18 = 0xC12,
    CSR_MHPMCOUNTER19 = 0xC13,
    CSR_MHPMCOUNTER20 = 0xC14,
    CSR_MHPMCOUNTER21 = 0xC15,
    CSR_MHPMCOUNTER22 = 0xC16,
    CSR_MHPMCOUNTER23 = 0xC17,
    CSR_MHPMCOUNTER24 = 0xC18,
    CSR_MHPMCOUNTER25 = 0xC19,
    CSR_MHPMCOUNTER26 = 0xC1A,
    CSR_MHPMCOUNTER27 = 0xC1B,
    CSR_MHPMCOUNTER28 = 0xC1C,
    CSR_MHPMCOUNTER29 = 0xC1D,
    CSR_MHPMCOUNTER30 = 0xC1E,
    CSR_MHPMCOUNTER31 = 0xC1F,
    // MHPMCOUNTERH rv32 only
    CSR_MMCOUNTINHIBIT = 0x320,
    CSR_MHPMEVENT03 = 0x323,
    CSR_MHPMEVENT04 = 0x324,
    CSR_MHPMEVENT05 = 0x325,
    CSR_MHPMEVENT06 = 0x326,
    CSR_MHPMEVENT07 = 0x327,
    CSR_MHPMEVENT08 = 0x328,
    CSR_MHPMEVENT09 = 0x329,
    CSR_MHPMEVENT10 = 0x32A,
    CSR_MHPMEVENT11 = 0x32B,
    CSR_MHPMEVENT12 = 0x32C,
    CSR_MHPMEVENT13 = 0x32D,
    CSR_MHPMEVENT14 = 0x32E,
    CSR_MHPMEVENT15 = 0x32F,
    CSR_MHPMEVENT16 = 0x330,
    CSR_MHPMEVENT17 = 0x331,
    CSR_MHPMEVENT18 = 0x332,
    CSR_MHPMEVENT19 = 0x333,
    CSR_MHPMEVENT20 = 0x334,
    CSR_MHPMEVENT21 = 0x335,
    CSR_MHPMEVENT22 = 0x336,
    CSR_MHPMEVENT23 = 0x337,
    CSR_MHPMEVENT24 = 0x338,
    CSR_MHPMEVENT25 = 0x339,
    CSR_MHPMEVENT26 = 0x33A,
    CSR_MHPMEVENT27 = 0x33B,
    CSR_MHPMEVENT28 = 0x33C,
    CSR_MHPMEVENT29 = 0x33D,
    CSR_MHPMEVENT30 = 0x33E,
    CSR_MHPMEVENT31 = 0x33F,

    CSR_TSELECT = 0x7A0,
    CSR_TDATA1 = 0x7A1,
    CSR_TDATA2 = 0x7A2,
    CSR_TDATA3 = 0x7A3,
    CSR_DCSR = 0x7B0,
    CSR_DPC = 0x7B1,
    CSR_DSCRATCH = 0x7B2,

    CSR_VSTART       = 0x008,
    CSR_VXSAT        = 0x009,
    CSR_VXRM         = 0x00A,
    CSR_VCSR         = 0x00F,
    CSR_VL           = 0xC20,
    CSR_VTYPE        = 0xC21,
    CSR_VLENB        = 0xC22,

    CSR_HSTATUS     = 0x600,
    CSR_HEDELEG     = 0x602,
    CSR_HIDELEG     = 0x603,
    CSR_HIE         = 0x604,
    CSR_HCOUNTEREN  = 0x606,
    CSR_HGEIE       = 0x607,
    CSR_HTVAL       = 0x643,
    CSR_HIP         = 0x644,
    CSR_HVIP        = 0x645,
    CSR_HTINST      = 0x64A,
    CSR_HGEIP       = 0xE12,
    CSR_HENVCFG     = 0x60A,
    CSR_HGATP       = 0x680,
    CSR_HTIMEDELTA  = 0x605,
    CSR_VSSTATUS    = 0x200,
    CSR_VSIE        = 0x204,
    CSR_VSTVEC      = 0x205,
    CSR_VSSCRATCH   = 0x240,
    CSR_VSEPC       = 0x241,
    CSR_VSCAUSE     = 0x242,
    CSR_VSTVAL      = 0x243,
    CSR_VSIP        = 0x244,
    CSR_VSATP       = 0x280,
    CSR_MTINST      = 0x34A,
    CSR_MTVAL2      = 0x34B,
};

struct CSRMetadata
{
    const std::string name;
    const int physIndex;
};

const std::map<int, CSRMetadata> CSRData = {
    {CSR_USTATUS, {"ustatus", MISCREG_STATUS}},
    {CSR_UIE, {"uie", MISCREG_IE}},
    {CSR_UTVEC, {"utvec", MISCREG_UTVEC}},
    {CSR_USCRATCH, {"uscratch", MISCREG_USCRATCH}},
    {CSR_UEPC, {"uepc", MISCREG_UEPC}},
    {CSR_UCAUSE, {"ucause", MISCREG_UCAUSE}},
    {CSR_UTVAL, {"utval", MISCREG_UTVAL}},
    {CSR_UIP, {"uip", MISCREG_IP}},
    {CSR_FFLAGS, {"fflags", MISCREG_FFLAGS}},
    {CSR_FRM, {"frm", MISCREG_FRM}},
    {CSR_FCSR, {"fcsr", MISCREG_FFLAGS}}, // Actually FRM << 5 | FFLAGS
    {CSR_CYCLE, {"cycle", MISCREG_CYCLE}},
    {CSR_TIME, {"time", MISCREG_TIME}},
    {CSR_INSTRET, {"instret", MISCREG_INSTRET}},
    {CSR_MMHPMCOUNTER3, {"mmhpmcounter03", MISCREG_MHPMCOUNTER3}},
    {CSR_MMHPMCOUNTER4, {"mmhpmcounter04", MISCREG_MHPMCOUNTER4}},
    {CSR_MMHPMCOUNTER5, {"mmhpmcounter05", MISCREG_MHPMCOUNTER5}},
    {CSR_MMHPMCOUNTER6, {"mmhpmcounter06", MISCREG_MHPMCOUNTER6}},
    {CSR_MMHPMCOUNTER7, {"mmhpmcounter07", MISCREG_MHPMCOUNTER7}},
    {CSR_MMHPMCOUNTER8, {"mmhpmcounter08", MISCREG_MHPMCOUNTER8}},
    {CSR_MMHPMCOUNTER9, {"mmhpmcounter09", MISCREG_MHPMCOUNTER9}},
    {CSR_MMHPMCOUNTER10, {"mmhpmcounter10", MISCREG_MHPMCOUNTER10}},
    {CSR_MMHPMCOUNTER11, {"mmhpmcounter11", MISCREG_MHPMCOUNTER11}},
    {CSR_MMHPMCOUNTER12, {"mmhpmcounter12", MISCREG_MHPMCOUNTER12}},
    {CSR_MMHPMCOUNTER13, {"mmhpmcounter13", MISCREG_MHPMCOUNTER13}},
    {CSR_MMHPMCOUNTER14, {"mmhpmcounter14", MISCREG_MHPMCOUNTER14}},
    {CSR_MMHPMCOUNTER15, {"mmhpmcounter15", MISCREG_MHPMCOUNTER15}},
    {CSR_MMHPMCOUNTER16, {"mmhpmcounter16", MISCREG_MHPMCOUNTER16}},
    {CSR_MMHPMCOUNTER17, {"mmhpmcounter17", MISCREG_MHPMCOUNTER17}},
    {CSR_MMHPMCOUNTER18, {"mmhpmcounter18", MISCREG_MHPMCOUNTER18}},
    {CSR_MMHPMCOUNTER19, {"mmhpmcounter19", MISCREG_MHPMCOUNTER19}},
    {CSR_MMHPMCOUNTER20, {"mmhpmcounter20", MISCREG_MHPMCOUNTER20}},
    {CSR_MMHPMCOUNTER21, {"mmhpmcounter21", MISCREG_MHPMCOUNTER21}},
    {CSR_MMHPMCOUNTER22, {"mmhpmcounter22", MISCREG_MHPMCOUNTER22}},
    {CSR_MMHPMCOUNTER23, {"mmhpmcounter23", MISCREG_MHPMCOUNTER23}},
    {CSR_MMHPMCOUNTER24, {"mmhpmcounter24", MISCREG_MHPMCOUNTER24}},
    {CSR_MMHPMCOUNTER25, {"mmhpmcounter25", MISCREG_MHPMCOUNTER25}},
    {CSR_MMHPMCOUNTER26, {"mmhpmcounter26", MISCREG_MHPMCOUNTER26}},
    {CSR_MMHPMCOUNTER27, {"mmhpmcounter27", MISCREG_MHPMCOUNTER27}},
    {CSR_MMHPMCOUNTER28, {"mmhpmcounter28", MISCREG_MHPMCOUNTER28}},
    {CSR_MMHPMCOUNTER29, {"mmhpmcounter29", MISCREG_MHPMCOUNTER29}},
    {CSR_MMHPMCOUNTER30, {"mmhpmcounter30", MISCREG_MHPMCOUNTER30}},
    {CSR_MMHPMCOUNTER31, {"mmhpmcounter31", MISCREG_MHPMCOUNTER31}},
    {CSR_HPMCOUNTER03, {"hpmcounter03", MISCREG_HPMCOUNTER03}},
    {CSR_HPMCOUNTER04, {"hpmcounter04", MISCREG_HPMCOUNTER04}},
    {CSR_HPMCOUNTER05, {"hpmcounter05", MISCREG_HPMCOUNTER05}},
    {CSR_HPMCOUNTER06, {"hpmcounter06", MISCREG_HPMCOUNTER06}},
    {CSR_HPMCOUNTER07, {"hpmcounter07", MISCREG_HPMCOUNTER07}},
    {CSR_HPMCOUNTER08, {"hpmcounter08", MISCREG_HPMCOUNTER08}},
    {CSR_HPMCOUNTER09, {"hpmcounter09", MISCREG_HPMCOUNTER09}},
    {CSR_HPMCOUNTER10, {"hpmcounter10", MISCREG_HPMCOUNTER10}},
    {CSR_HPMCOUNTER11, {"hpmcounter11", MISCREG_HPMCOUNTER11}},
    {CSR_HPMCOUNTER12, {"hpmcounter12", MISCREG_HPMCOUNTER12}},
    {CSR_HPMCOUNTER13, {"hpmcounter13", MISCREG_HPMCOUNTER13}},
    {CSR_HPMCOUNTER14, {"hpmcounter14", MISCREG_HPMCOUNTER14}},
    {CSR_HPMCOUNTER15, {"hpmcounter15", MISCREG_HPMCOUNTER15}},
    {CSR_HPMCOUNTER16, {"hpmcounter16", MISCREG_HPMCOUNTER16}},
    {CSR_HPMCOUNTER17, {"hpmcounter17", MISCREG_HPMCOUNTER17}},
    {CSR_HPMCOUNTER18, {"hpmcounter18", MISCREG_HPMCOUNTER18}},
    {CSR_HPMCOUNTER19, {"hpmcounter19", MISCREG_HPMCOUNTER19}},
    {CSR_HPMCOUNTER20, {"hpmcounter20", MISCREG_HPMCOUNTER20}},
    {CSR_HPMCOUNTER21, {"hpmcounter21", MISCREG_HPMCOUNTER21}},
    {CSR_HPMCOUNTER22, {"hpmcounter22", MISCREG_HPMCOUNTER22}},
    {CSR_HPMCOUNTER23, {"hpmcounter23", MISCREG_HPMCOUNTER23}},
    {CSR_HPMCOUNTER24, {"hpmcounter24", MISCREG_HPMCOUNTER24}},
    {CSR_HPMCOUNTER25, {"hpmcounter25", MISCREG_HPMCOUNTER25}},
    {CSR_HPMCOUNTER26, {"hpmcounter26", MISCREG_HPMCOUNTER26}},
    {CSR_HPMCOUNTER27, {"hpmcounter27", MISCREG_HPMCOUNTER27}},
    {CSR_HPMCOUNTER28, {"hpmcounter28", MISCREG_HPMCOUNTER28}},
    {CSR_HPMCOUNTER29, {"hpmcounter29", MISCREG_HPMCOUNTER29}},
    {CSR_HPMCOUNTER30, {"hpmcounter30", MISCREG_HPMCOUNTER30}},
    {CSR_HPMCOUNTER31, {"hpmcounter31", MISCREG_HPMCOUNTER31}},

    {CSR_SSTATUS, {"sstatus", MISCREG_STATUS}},
    {CSR_SEDELEG, {"sedeleg", MISCREG_SEDELEG}},
    {CSR_SIDELEG, {"sideleg", MISCREG_SIDELEG}},
    {CSR_SIE, {"sie", MISCREG_IE}},
    {CSR_STVEC, {"stvec", MISCREG_STVEC}},
    {CSR_SCOUNTEREN, {"scounteren", MISCREG_SCOUNTEREN}},
    {CSR_SSCRATCH, {"sscratch", MISCREG_SSCRATCH}},
    {CSR_SEPC, {"sepc", MISCREG_SEPC}},
    {CSR_SCAUSE, {"scause", MISCREG_SCAUSE}},
    {CSR_STVAL, {"stval", MISCREG_STVAL}},
    {CSR_SIP, {"sip", MISCREG_IP}},
    {CSR_SATP, {"satp", MISCREG_SATP}},

    {CSR_MVENDORID, {"mvendorid", MISCREG_VENDORID}},
    {CSR_MARCHID, {"marchid", MISCREG_ARCHID}},
    {CSR_MIMPID, {"mimpid", MISCREG_IMPID}},
    {CSR_MHARTID, {"mhartid", MISCREG_HARTID}},
    {CSR_MSTATUS, {"mstatus", MISCREG_STATUS}},
    {CSR_MISA, {"misa", MISCREG_ISA}},
    {CSR_MEDELEG, {"medeleg", MISCREG_MEDELEG}},
    {CSR_MIDELEG, {"mideleg", MISCREG_MIDELEG}},
    {CSR_MIE, {"mie", MISCREG_IE}},
    {CSR_MTVEC, {"mtvec", MISCREG_MTVEC}},
    {CSR_MCOUNTEREN, {"mcounteren", MISCREG_MCOUNTEREN}},
    {CSR_MENVCFG, {"menvcfg", MISCREG_MENVCFG}},
    {CSR_MSCRATCH, {"mscratch", MISCREG_MSCRATCH}},
    {CSR_MEPC, {"mepc", MISCREG_MEPC}},
    {CSR_MCAUSE, {"mcause", MISCREG_MCAUSE}},
    {CSR_MTVAL, {"mtval", MISCREG_MTVAL}},
    {CSR_MIP, {"mip", MISCREG_IP}},
    {CSR_PMPCFG0, {"pmpcfg0", MISCREG_PMPCFG0}},
    // pmpcfg1 rv32 only
    {CSR_PMPCFG2, {"pmpcfg2", MISCREG_PMPCFG2}},
    // pmpcfg3 rv32 only
    {CSR_PMPADDR00, {"pmpaddr0", MISCREG_PMPADDR00}},
    {CSR_PMPADDR01, {"pmpaddr1", MISCREG_PMPADDR01}},
    {CSR_PMPADDR02, {"pmpaddr2", MISCREG_PMPADDR02}},
    {CSR_PMPADDR03, {"pmpaddr3", MISCREG_PMPADDR03}},
    {CSR_PMPADDR04, {"pmpaddr4", MISCREG_PMPADDR04}},
    {CSR_PMPADDR05, {"pmpaddr5", MISCREG_PMPADDR05}},
    {CSR_PMPADDR06, {"pmpaddr6", MISCREG_PMPADDR06}},
    {CSR_PMPADDR07, {"pmpaddr7", MISCREG_PMPADDR07}},
    {CSR_PMPADDR08, {"pmpaddr8", MISCREG_PMPADDR08}},
    {CSR_PMPADDR09, {"pmpaddr9", MISCREG_PMPADDR09}},
    {CSR_PMPADDR10, {"pmpaddr10", MISCREG_PMPADDR10}},
    {CSR_PMPADDR11, {"pmpaddr11", MISCREG_PMPADDR11}},
    {CSR_PMPADDR12, {"pmpaddr12", MISCREG_PMPADDR12}},
    {CSR_PMPADDR13, {"pmpaddr13", MISCREG_PMPADDR13}},
    {CSR_PMPADDR14, {"pmpaddr14", MISCREG_PMPADDR14}},
    {CSR_PMPADDR15, {"pmpaddr15", MISCREG_PMPADDR15}},
    {CSR_MCYCLE, {"mcycle", MISCREG_CYCLE}},
    {CSR_MINSTRET, {"minstret", MISCREG_INSTRET}},

    {CSR_MHPMCOUNTER03, {"mhpmcounter03", MISCREG_HPMCOUNTER03}},
    {CSR_MHPMCOUNTER04, {"mhpmcounter04", MISCREG_HPMCOUNTER04}},
    {CSR_MHPMCOUNTER05, {"mhpmcounter05", MISCREG_HPMCOUNTER05}},
    {CSR_MHPMCOUNTER06, {"mhpmcounter06", MISCREG_HPMCOUNTER06}},
    {CSR_MHPMCOUNTER07, {"mhpmcounter07", MISCREG_HPMCOUNTER07}},
    {CSR_MHPMCOUNTER08, {"mhpmcounter08", MISCREG_HPMCOUNTER08}},
    {CSR_MHPMCOUNTER09, {"mhpmcounter09", MISCREG_HPMCOUNTER09}},
    {CSR_MHPMCOUNTER10, {"mhpmcounter10", MISCREG_HPMCOUNTER10}},
    {CSR_MHPMCOUNTER11, {"mhpmcounter11", MISCREG_HPMCOUNTER11}},
    {CSR_MHPMCOUNTER12, {"mhpmcounter12", MISCREG_HPMCOUNTER12}},
    {CSR_MHPMCOUNTER13, {"mhpmcounter13", MISCREG_HPMCOUNTER13}},
    {CSR_MHPMCOUNTER14, {"mhpmcounter14", MISCREG_HPMCOUNTER14}},
    {CSR_MHPMCOUNTER15, {"mhpmcounter15", MISCREG_HPMCOUNTER15}},
    {CSR_MHPMCOUNTER16, {"mhpmcounter16", MISCREG_HPMCOUNTER16}},
    {CSR_MHPMCOUNTER17, {"mhpmcounter17", MISCREG_HPMCOUNTER17}},
    {CSR_MHPMCOUNTER18, {"mhpmcounter18", MISCREG_HPMCOUNTER18}},
    {CSR_MHPMCOUNTER19, {"mhpmcounter19", MISCREG_HPMCOUNTER19}},
    {CSR_MHPMCOUNTER20, {"mhpmcounter20", MISCREG_HPMCOUNTER20}},
    {CSR_MHPMCOUNTER21, {"mhpmcounter21", MISCREG_HPMCOUNTER21}},
    {CSR_MHPMCOUNTER22, {"mhpmcounter22", MISCREG_HPMCOUNTER22}},
    {CSR_MHPMCOUNTER23, {"mhpmcounter23", MISCREG_HPMCOUNTER23}},
    {CSR_MHPMCOUNTER24, {"mhpmcounter24", MISCREG_HPMCOUNTER24}},
    {CSR_MHPMCOUNTER25, {"mhpmcounter25", MISCREG_HPMCOUNTER25}},
    {CSR_MHPMCOUNTER26, {"mhpmcounter26", MISCREG_HPMCOUNTER26}},
    {CSR_MHPMCOUNTER27, {"mhpmcounter27", MISCREG_HPMCOUNTER27}},
    {CSR_MHPMCOUNTER28, {"mhpmcounter28", MISCREG_HPMCOUNTER28}},
    {CSR_MHPMCOUNTER29, {"mhpmcounter29", MISCREG_HPMCOUNTER29}},
    {CSR_MHPMCOUNTER30, {"mhpmcounter30", MISCREG_HPMCOUNTER30}},
    {CSR_MHPMCOUNTER31, {"mhpmcounter31", MISCREG_HPMCOUNTER31}},
    {CSR_MMCOUNTINHIBIT,{"mcountinhibit", MISCREG_MCOUNTINHIBIT}},
    {CSR_MHPMEVENT03, {"mhpmevent03", MISCREG_HPMEVENT03}},
    {CSR_MHPMEVENT04, {"mhpmevent04", MISCREG_HPMEVENT04}},
    {CSR_MHPMEVENT05, {"mhpmevent05", MISCREG_HPMEVENT05}},
    {CSR_MHPMEVENT06, {"mhpmevent06", MISCREG_HPMEVENT06}},
    {CSR_MHPMEVENT07, {"mhpmevent07", MISCREG_HPMEVENT07}},
    {CSR_MHPMEVENT08, {"mhpmevent08", MISCREG_HPMEVENT08}},
    {CSR_MHPMEVENT09, {"mhpmevent09", MISCREG_HPMEVENT09}},
    {CSR_MHPMEVENT10, {"mhpmevent10", MISCREG_HPMEVENT10}},
    {CSR_MHPMEVENT11, {"mhpmevent11", MISCREG_HPMEVENT11}},
    {CSR_MHPMEVENT12, {"mhpmevent12", MISCREG_HPMEVENT12}},
    {CSR_MHPMEVENT13, {"mhpmevent13", MISCREG_HPMEVENT13}},
    {CSR_MHPMEVENT14, {"mhpmevent14", MISCREG_HPMEVENT14}},
    {CSR_MHPMEVENT15, {"mhpmevent15", MISCREG_HPMEVENT15}},
    {CSR_MHPMEVENT16, {"mhpmevent16", MISCREG_HPMEVENT16}},
    {CSR_MHPMEVENT17, {"mhpmevent17", MISCREG_HPMEVENT17}},
    {CSR_MHPMEVENT18, {"mhpmevent18", MISCREG_HPMEVENT18}},
    {CSR_MHPMEVENT19, {"mhpmevent19", MISCREG_HPMEVENT19}},
    {CSR_MHPMEVENT20, {"mhpmevent20", MISCREG_HPMEVENT20}},
    {CSR_MHPMEVENT21, {"mhpmevent21", MISCREG_HPMEVENT21}},
    {CSR_MHPMEVENT22, {"mhpmevent22", MISCREG_HPMEVENT22}},
    {CSR_MHPMEVENT23, {"mhpmevent23", MISCREG_HPMEVENT23}},
    {CSR_MHPMEVENT24, {"mhpmevent24", MISCREG_HPMEVENT24}},
    {CSR_MHPMEVENT25, {"mhpmevent25", MISCREG_HPMEVENT25}},
    {CSR_MHPMEVENT26, {"mhpmevent26", MISCREG_HPMEVENT26}},
    {CSR_MHPMEVENT27, {"mhpmevent27", MISCREG_HPMEVENT27}},
    {CSR_MHPMEVENT28, {"mhpmevent28", MISCREG_HPMEVENT28}},
    {CSR_MHPMEVENT29, {"mhpmevent29", MISCREG_HPMEVENT29}},
    {CSR_MHPMEVENT30, {"mhpmevent30", MISCREG_HPMEVENT30}},
    {CSR_MHPMEVENT31, {"mhpmevent31", MISCREG_HPMEVENT31}},

    {CSR_TSELECT, {"tselect", MISCREG_TSELECT}},
    {CSR_TDATA1, {"tdata1", MISCREG_TDATA1}},
    {CSR_TDATA2, {"tdata2", MISCREG_TDATA2}},
    {CSR_TDATA3, {"tdata3", MISCREG_TDATA3}},
    {CSR_DCSR, {"dcsr", MISCREG_DCSR}},
    {CSR_DPC, {"dpc", MISCREG_DPC}},
    {CSR_DSCRATCH, {"dscratch", MISCREG_DSCRATCH}},

    {CSR_VSTART,       {"vstart", MISCREG_VSTART}},
    {CSR_VXSAT,        {"vxsat" , MISCREG_VXSAT}},
    {CSR_VXRM,         {"vxrm"  , MISCREG_VXRM}},
    {CSR_VCSR,         {"vcsr"  , MISCREG_VCSR}},
    {CSR_VL,           {"vl"    , MISCREG_VL}},
    {CSR_VTYPE,        {"vtype" , MISCREG_VTYPE}},
    {CSR_VLENB,        {"VLENB" , MISCREG_VLENB}},

    {CSR_HSTATUS, {"hstatus", MISCREG_HSTATUS}},
    {CSR_HEDELEG, {"hedeleg", MISCREG_HEDELEG}},
    {CSR_HIDELEG, {"hideleg", MISCREG_HIDELEG}},
    {CSR_HIE, {"hie", MISCREG_HIE}},
    {CSR_HCOUNTEREN, {"hcounteren", MISCREG_HCOUNTEREN}},
    {CSR_HGEIE, {"hgeie", MISCREG_HGEIE}},
    {CSR_HTVAL, {"htval", MISCREG_HTVAL}},
    {CSR_HIP, {"hip", MISCREG_HIP}},
    {CSR_HVIP, {"hvip", MISCREG_HVIP}},
    {CSR_HTINST, {"htinst", MISCREG_HTINST}},
    {CSR_HGEIP, {"hgeip", MISCREG_HGEIP}},
    {CSR_HENVCFG, {"henvcfg", MISCREG_HENVCFG}},
    {CSR_HGATP, {"hgatp", MISCREG_HGATP}},
    {CSR_HTIMEDELTA, {"htimedalta", MISCREG_HTIMEDELTA}},
    {CSR_VSSTATUS, {"vsstatus", MISCREG_VSSTATUS}},
    {CSR_VSIE, {"vsie", MISCREG_VSIE}},
    {CSR_VSTVEC, {"vstvec", MISCREG_VSTVEC}},
    {CSR_VSSCRATCH, {"vsscratch", MISCREG_VSSCRATCH}},
    {CSR_VSEPC, {"vsepc", MISCREG_VSEPC}},
    {CSR_VSCAUSE, {"vscause", MISCREG_VSCAUSE}},
    {CSR_VSTVAL, {"vstval", MISCREG_VSTVAL}},
    {CSR_VSIP, {"vsip", MISCREG_VSIP}},
    {CSR_VSATP, {"vsatp", MISCREG_VSATP}},
    {CSR_MTINST, {"mtinst", MISCREG_MTINST}},
    {CSR_MTVAL2, {"mtval2", MISCREG_MTVAL2}},
};

/**
 * These fields are specified in the RISC-V Instruction Set Manual, Volume II,
 * v1.10, accessible at www.riscv.org. in Figure 3.7. The main register that
 * uses these fields is the MSTATUS register, which is shadowed by two others
 * accessible at lower privilege levels (SSTATUS and USTATUS) that can't see
 * the fields for higher privileges.
 */
BitUnion64(STATUS)
    Bitfield<63> sd;
    Bitfield<62,40> wpri;
    Bitfield<39> mpv;
    Bitfield<38> gva;
    Bitfield<37> mbe;
    Bitfield<36> sbe;
    Bitfield<35, 34> sxl;
    Bitfield<33, 32> uxl;
    Bitfield<22> tsr;
    Bitfield<21> tw;
    Bitfield<20> tvm;
    Bitfield<19> mxr;
    Bitfield<18> sum;
    Bitfield<17> mprv;
    Bitfield<16, 15> xs;
    Bitfield<14, 13> fs;
    Bitfield<12, 11> mpp;
    Bitfield<10, 9> vs;
    Bitfield<8> spp;
    Bitfield<7> mpie;
    Bitfield<5> spie;
    Bitfield<4> upie;
    Bitfield<3> mie;
    Bitfield<1> sie;
    Bitfield<0> uie;
EndBitUnion(STATUS)

BitUnion64(HSTATUS)
    Bitfield<63,34> pad4;
    Bitfield<33,32> vsxl;
    Bitfield<31,23> pad3;
    Bitfield<22> vtsr;
    Bitfield<21> vtw;
    Bitfield<20> vtvm;
    Bitfield<19, 18> pad2;
    Bitfield<17,12> vgein;
    Bitfield<11,10> pad1;
    Bitfield<9> hu;
    Bitfield<8> spvp;
    Bitfield<7> spv;
    Bitfield<6> gva;
    Bitfield<5> vsbe;
    Bitfield<4,0> pad0;
EndBitUnion(HSTATUS)

BitUnion64(VSSTATUS)
    Bitfield<63> sd;
    Bitfield<62,34> pad6;
    Bitfield<33,32> uxl;
    Bitfield<31,20> pad5;
    Bitfield<19> mxr;
    Bitfield<18> sum;
    Bitfield<17> pad4;
    Bitfield<16,15> xs;
    Bitfield<14,13> fs;
    Bitfield<12,11> pad3;
    Bitfield<10,9> vs;
    Bitfield<8> spp;
    Bitfield<7> pad2;
    Bitfield<6> ube;
    Bitfield<5> spie;
    Bitfield<4,2> pad1;
    Bitfield<1> sie;
    Bitfield<0> pad0;
EndBitUnion(VSSTATUS)

BitUnion32(VSSTATUS32)
    Bitfield<31> sd;
    Bitfield<30,20> pad5;
    Bitfield<19> mxr;
    Bitfield<18> sum;
    Bitfield<17> pad4;
    Bitfield<16,15> xs;
    Bitfield<14,13> fs;
    Bitfield<12,11> pad3;
    Bitfield<10,9> vs;
    Bitfield<8> spp;
    Bitfield<7> pad2;
    Bitfield<6> ube;
    Bitfield<5> spie;
    Bitfield<4,2> pad1;
    Bitfield<1> sie;
    Bitfield<0> pad0;
EndBitUnion(VSSTATUS32)

BitUnion64(HGATP)
    Bitfield<63,60> mode;
    Bitfield<59,58> pad0;
    Bitfield<57,44> vmid;
    Bitfield<43,0> ppn;
EndBitUnion(HGATP)

/**
 * These fields are specified in the RISC-V Instruction Set Manual, Volume II,
 * v1.10 in Figures 3.11 and 3.12, accessible at www.riscv.org. Both the MIP
 * and MIE registers have the same fields, so accesses to either should use
 * this bit union.
 */
BitUnion64(INTERRUPT)
    Bitfield<11> mei;
    Bitfield<9> sei;
    Bitfield<8> uei;
    Bitfield<7> mti;
    Bitfield<5> sti;
    Bitfield<4> uti;
    Bitfield<3> msi;
    Bitfield<1> ssi;
    Bitfield<0> usi;
EndBitUnion(INTERRUPT)

const off_t MXL_OFFSET = (sizeof(uint64_t) * 8 - 2);
const off_t SXL_OFFSET = 34;
const off_t UXL_OFFSET = 32;
const off_t FS_OFFSET = 13;
const off_t VS_OFFSET = 9;
const off_t FRM_OFFSET = 5;

const RegVal ISA_MXL_MASK = 3ULL << MXL_OFFSET;
const RegVal ISA_EXT_MASK = mask(26);
const RegVal ISA_EXT_C_MASK = 1UL << ('c' - 'a');
const RegVal ISA_EXT_H_MASK = 1UL << ('h' - 'a');
const RegVal MISA_MASK = ISA_MXL_MASK | ISA_EXT_MASK;

const uint64_t NEMU_SV39_SIGN0 = 0;
const uint64_t NEMU_SV39_SIGN1 = 8;
const uint64_t NEMU_SATP_SV39_MASK = 0xf000000000000000;
const uint64_t NEMU_SATP_ASID_LEN = 16;
const uint64_t NEMU_SATP_PADDR_LEN = 24;
const uint64_t NEMU_SATP_PADDR_MAX_LEN = 44;
const uint64_t NEMU_SATP_RIGHT_OFFSET = 60;
const uint64_t NEMU_SATP_MODE_MASK = (uint64_t)8<<(NEMU_SATP_ASID_LEN+NEMU_SATP_PADDR_MAX_LEN);
const uint64_t NEMU_SATP_ASID_MASK =  ((((uint64_t)1 <<(NEMU_SATP_ASID_LEN))-1)<<NEMU_SATP_PADDR_MAX_LEN);
const uint64_t NEMU_SATP_PADDR_MASK =  (((uint64_t)1<<NEMU_SATP_PADDR_LEN)-1);
const uint64_t NEMU_SATP_MASK = NEMU_SATP_MODE_MASK |NEMU_SATP_ASID_MASK | NEMU_SATP_PADDR_MASK;
const uint64_t NEMU_MSTATUS_WMASK_BASE = (0x7e19aaUL);
const uint64_t NEMU_MSTATUS_WMASK_RVH = (3UL << 38);
const uint64_t NEMU_MSTATUS_WMASK_FS = (0x3UL << 13);
const uint64_t NEMU_MSTATUS_WMASK_RVV = (3UL << 9);
const uint64_t NEMU_MSTATUS_WMASK =
    (NEMU_MSTATUS_WMASK_BASE | NEMU_MSTATUS_WMASK_FS | NEMU_MSTATUS_WMASK_RVH | NEMU_MSTATUS_WMASK_RVV);
const uint64_t NEMU_SSTATUS_WMASK = NEMU_MSTATUS_WMASK & (0x80000003000de762UL);
// const uint64_t NEMU_COUNTER_MASK = 0xffffffffULL;
// const uint64_t NEMU_SSTATUS_WMASK = ((1 << 19) | (1 << 18) | (0x3 << 13) | (1 << 8) | (1 << 5) | (1 << 1) |(0x3
// <<9));
const uint64_t NEMU_SSTATUS_RMASK = 0x80000003000de762UL;
const uint64_t NEMU_COUNTER_MASK = 0xffffffffULL;
//const uint64_t NEMU_COUNTER_MASK = 0;

const uint64_t NEMU_VS_MASK = ((1 << 10) | (1 << 6) | (1 << 2));
const uint64_t NEMU_HS_MASK = ((1 << 12) | NEMU_VS_MASK);
const uint64_t NEMU_HIE_WMASK = NEMU_HS_MASK;
const uint64_t NEMU_HIE_RMASK = NEMU_HS_MASK;

const uint64_t NEMU_HIP_RMASK = NEMU_HS_MASK;
const uint64_t NEMU_HVIP_MASK = ((1 << 10) | (1 << 6) | (1 << 2));
const uint64_t NEMU_MIDELEG_FORCED_MASK = ((1 << 12) | (1 << 10) | (1 << 6) | (1 << 2));
const RegVal STATUS_SD_MASK = 1ULL << ((sizeof(uint64_t) * 8) - 1);
const RegVal STATUS_SXL_MASK = 3ULL << SXL_OFFSET;
const RegVal STATUS_UXL_MASK = 3ULL << UXL_OFFSET;
const RegVal STATUS_TSR_MASK = 1ULL << 22;
const RegVal STATUS_TW_MASK = 1ULL << 21;
const RegVal STATUS_TVM_MASK = 1ULL << 20;
const RegVal STATUS_MXR_MASK = 1ULL << 19;
const RegVal STATUS_SUM_MASK = 1ULL << 18;
const RegVal STATUS_MPRV_MASK = 1ULL << 17;
const RegVal STATUS_XS_MASK = 3ULL << 15;
const RegVal STATUS_FS_MASK = 3ULL << FS_OFFSET;
const RegVal STATUS_MPP_MASK = 3ULL << 11;
const RegVal STATUS_VS_MASK = 3ULL << VS_OFFSET;
const RegVal STATUS_SPP_MASK = 1ULL << 8;
const RegVal STATUS_MPIE_MASK = 1ULL << 7;
const RegVal STATUS_SPIE_MASK = 1ULL << 5;
const RegVal STATUS_UPIE_MASK = 1ULL << 4;
const RegVal STATUS_MIE_MASK = 1ULL << 3;
const RegVal STATUS_SIE_MASK = 1ULL << 1;
const RegVal STATUS_UIE_MASK = 1ULL << 0;
const RegVal MSTATUS_WMASK_FS = 0x3UL << 13;
const RegVal MSTATUS_WMASK_RVH = 3UL << 38;
const RegVal MSTATUS_WMASK_RVV = 3UL << 9;
const RegVal MSTATUS_MASK = (0x7e79aaUL) | MSTATUS_WMASK_FS | MSTATUS_WMASK_RVH | MSTATUS_WMASK_RVV;
const RegVal HSTATUS_MASK = ((1 << 22) | (1 << 21) | (1 << 20) | (1 << 18) | (0x3f << 12) | (1 << 9) | (1 << 8) |
                             (1 << 7) | (1 << 6) | (1 << 5));
const RegVal SSTATUS_MASK = STATUS_SD_MASK | STATUS_UXL_MASK |
                            STATUS_MXR_MASK | STATUS_SUM_MASK |
                            STATUS_XS_MASK | STATUS_FS_MASK |
                            STATUS_VS_MASK |
                            STATUS_SPP_MASK | STATUS_SPIE_MASK |
                            STATUS_UPIE_MASK | STATUS_SIE_MASK |
                            STATUS_UIE_MASK;
const RegVal USTATUS_MASK = STATUS_SD_MASK | STATUS_MXR_MASK |
                            STATUS_SUM_MASK | STATUS_XS_MASK |
                            STATUS_FS_MASK | STATUS_VS_MASK |
                            STATUS_UPIE_MASK | STATUS_UIE_MASK;

const RegVal MEI_MASK = 1ULL << 11;
const RegVal SEI_MASK = 1ULL << 9;
const RegVal UEI_MASK = 1ULL << 8;
const RegVal MTI_MASK = 1ULL << 7;
const RegVal STI_MASK = 1ULL << 5;
const RegVal UTI_MASK = 1ULL << 4;
const RegVal MSI_MASK = 1ULL << 3;
const RegVal SSI_MASK = 1ULL << 1;
const RegVal USI_MASK = 1ULL << 0;
const RegVal NEMU_MIP_MASK =  ((1 << 9) | (1 << 5) | (1 << 2) |(1 << 1));
const RegVal SI_MASK = SEI_MASK | STI_MASK | SSI_MASK;
const RegVal UI_MASK = UEI_MASK | UTI_MASK | USI_MASK;
const RegVal FFLAGS_MASK = (1 << FRM_OFFSET) - 1;
const RegVal FRM_MASK = 0x7;
const RegVal NEMU_MIE_MASK_BASE = 0xaaa;
const RegVal NEMU_MIE_MASK_H = (1 << 2) | (1 << 6) | (1 << 10) | (1 << 12);
const RegVal NEMU_LCOFI = 0;
const RegVal NEMU_MIE_MASK = NEMU_MIE_MASK_BASE | NEMU_MIE_MASK_H | NEMU_LCOFI;

const std::map<int, RegVal> CSRMasks = {
    {CSR_USTATUS, USTATUS_MASK},
    {CSR_UIE, UI_MASK},
    {CSR_UIP, UI_MASK},
    {CSR_FFLAGS, FFLAGS_MASK},
    {CSR_FRM, FRM_MASK},
    {CSR_FCSR, FFLAGS_MASK | (FRM_MASK << FRM_OFFSET)},
    {CSR_SSTATUS, SSTATUS_MASK},
    {CSR_SIP, SI_MASK},
    {CSR_MISA, MISA_MASK},
    {CSR_MIE,NEMU_MIE_MASK}
};

#define concat_temp(x, y) x ## y
#define concat(x, y) concat_temp(x, y)

#define CSR_STRUCT_START(name) \
  typedef union { \
    struct {

#define CSR_STRUCT_END(name) \
    }; \
    uint64_t val; \
  } concat(name, _t);



CSR_STRUCT_START(hstatus)
  uint64_t pad0  : 5;
  uint64_t vsbe  : 1;
  uint64_t gva   : 1;
  uint64_t spv   : 1;
  uint64_t spvp  : 1;
  uint64_t hu    : 1;
  uint64_t pad1  : 2;
  uint64_t vgein : 6;
  uint64_t pad2  : 2;
  uint64_t vtvm  : 1;
  uint64_t vtw   : 1;
  uint64_t vtsr  : 1;
  uint64_t pad3  : 9;
  uint64_t vsxl  : 2;
CSR_STRUCT_END(hstatus)

CSR_STRUCT_START(hie)
  uint64_t pad0  : 2;
  uint64_t vssie : 1;
  uint64_t pad1  : 3;
  uint64_t vstie : 1;
  uint64_t pad2  : 3;
  uint64_t vseie : 1;
  uint64_t pad3  : 1;
  uint64_t sgeie : 1;
CSR_STRUCT_END(hie)

CSR_STRUCT_START(hip)
  uint64_t pad0  : 2;
  uint64_t vssip : 1;
  uint64_t pad1  : 3;
  uint64_t vstip : 1;
  uint64_t pad2  : 3;
  uint64_t vseip : 1;
  uint64_t pad3  : 1;
  uint64_t sgeip : 1;
CSR_STRUCT_END(hip)

CSR_STRUCT_START(hvip)
  uint64_t pad0  : 2;
  uint64_t vssip : 1;
  uint64_t pad1  : 3;
  uint64_t vstip : 1;
  uint64_t pad2  : 3;
  uint64_t vseip : 1;
CSR_STRUCT_END(hvip)

CSR_STRUCT_START(henvcfg)
  uint64_t fiom   : 1;
  uint64_t pad0   : 3;
  uint64_t cbie   : 2;
  uint64_t cbcfe  : 1;
  uint64_t cbze   : 1;
  uint64_t pad1   :54;
  uint64_t pbmte  : 1;
  uint64_t vstce  : 1;
CSR_STRUCT_END(henvcfg)

CSR_STRUCT_START(hgatp)
  uint64_t ppn    : 44;
  uint64_t vmid   : 14;
  uint64_t pad0   : 2;
  uint64_t mode   : 4;
CSR_STRUCT_END(hgatp)

CSR_STRUCT_START(vsstatus)
  union{
    struct{
      uint64_t pad0: 1;
      uint64_t sie : 1;
      uint64_t pad1: 3;
      uint64_t spie: 1;
      uint64_t ube : 1;
      uint64_t pad2: 1;
      uint64_t spp : 1;
      uint64_t vs  : 2;
      uint64_t pad3: 2;
      uint64_t fs  : 2;
      uint64_t xs  : 2;
      uint64_t pad4: 1;
      uint64_t sum : 1;
      uint64_t mxr : 1;
      uint64_t pad5:11;
      uint64_t sd  : 1;
    }_32;
    struct{
      uint64_t pad0: 1;
      uint64_t sie : 1;
      uint64_t pad1: 3;
      uint64_t spie: 1;
      uint64_t ube : 1;
      uint64_t pad2: 1;
      uint64_t spp : 1;
      uint64_t vs  : 2;
      uint64_t pad3: 2;
      uint64_t fs  : 2;
      uint64_t xs  : 2;
      uint64_t pad4: 1;
      uint64_t sum : 1;
      uint64_t mxr : 1;
      uint64_t pad5:12;
      uint64_t uxl : 2;
      uint64_t pad6:29;
      uint64_t sd  : 1;
    }_64;
  };
CSR_STRUCT_END(vsstatus)

CSR_STRUCT_START(vsie)
  uint64_t pad0 : 1;
  uint64_t ssie : 1;
  uint64_t pad1 : 3;
  uint64_t stie : 1;
  uint64_t pad2 : 3;
  uint64_t seie : 1;
CSR_STRUCT_END(vsie)

CSR_STRUCT_START(vstvec)
  uint64_t mode  : 2;
  uint64_t base  :62;
CSR_STRUCT_END(vstvec)

CSR_STRUCT_START(vscause)
  union{
    struct{
      uint64_t code:31;
      uint64_t intr: 1;
    }_32;
    struct{
      uint64_t code:63;
      uint64_t intr: 1;
    }_64;
  };
CSR_STRUCT_END(vscause)

CSR_STRUCT_START(vsip)
  uint64_t pad0 : 1;
  uint64_t ssip : 1;
  uint64_t pad1 : 3;
  uint64_t stip : 1;
  uint64_t pad2 : 3;
  uint64_t seip : 1;
CSR_STRUCT_END(vsip)

CSR_STRUCT_START(vsatp)
  union{
    struct{
      uint64_t ppn  :22;
      uint64_t asid : 9;
      uint64_t mode : 1;
    }_32;
    struct{
      uint64_t ppn  :44;
      uint64_t asid :16;
      uint64_t mode : 4;
    }_64;
  };
CSR_STRUCT_END(vsatp)

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_REGS_MISC_HH__
