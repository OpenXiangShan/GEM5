/*
 * Copyright (c) 2022 PLCT Lab
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


#ifndef __ARCH_RISCV_REGS_VECTOR_HH__
#define __ARCH_RISCV_REGS_VECTOR_HH__

#include <cstdint>

#include "arch/generic/vec_pred_reg.hh"
#include "arch/generic/vec_reg.hh"
#include "arch/riscv/regs/renameable_misc.hh"
#include "arch/riscv/types.hh"
#include "base/bitunion.hh"
#include "cpu/reg_class.hh"
#include "debug/VecRegs.hh"

namespace gem5
{

namespace RiscvISA
{

using VecRegContainer =
    gem5::VecRegContainer<DPLENB>;
using vreg_t = VecRegContainer;

using VecBufRegContainer = gem5::VecRegContainer<VBUF_SIZE>;
// vlseg*ff needs slot_ring >= nf_max=8, and fof_slots == nf, so
// NumVecBufRegs >= 2 * nf_max = 16, independent of VregBanks.
constexpr int NumVecBufRegs = 16;

using VecPredReg =
    gem5::VecPredRegT<VecElem, NumArchVecElemPerReg, false, false>;
using ConstVecPredReg =
    gem5::VecPredRegT<VecElem, NumArchVecElemPerReg, false, true>;
using VecPredRegContainer = VecPredReg::Container;

const int NumVecStandardRegs = 32 * VregBanks;
const int NumVecRegs = NumVecStandardRegs;


static inline VecElemRegClassOps<RiscvISA::VecElem>
    vecRegElemClassOps(NumVecElemPerVecReg);
static inline TypedRegClassOps<RiscvISA::VecRegContainer> vecRegClassOps;

inline const auto VecCompressCntReg = RegId(VecBufRegClass, NumVecBufRegs - 1);

inline const auto VecRenamedVLReg = RegId(RMiscRegClass, rmisc_reg::_VlIdx);
inline const auto VecRenamedVSTARTReg = RegId(RMiscRegClass, rmisc_reg::_VstartIdx);
inline const auto VecRenamedVTYPEReg = RegId(RMiscRegClass, rmisc_reg::_VtypeIdx);

BitUnion64(VTYPE)
    Bitfield<63> vill;
    Bitfield<7, 0> vtype8;
    Bitfield<7> vma;
    Bitfield<6> vta;
    Bitfield<5, 3> vsew;
    Bitfield<2, 0> vlmul;
EndBitUnion(VTYPE)

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_REGS_VECTOR_HH__
