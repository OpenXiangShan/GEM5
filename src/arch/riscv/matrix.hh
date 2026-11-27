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

#ifndef __ARCH_RISCV_MATRIX_HH__
#define __ARCH_RISCV_MATRIX_HH__

#include <cstdint>

#include "arch/riscv/insts/standard.hh"
#include "arch/riscv/regs/misc.hh"
#include "arch/riscv/regs/renameable_misc.hh"
#include "arch/riscv/types.hh"
#include "cpu/reg_class.hh"

namespace gem5
{

namespace RiscvISA
{

inline constexpr RegVal MatrixTokenCount = 32;
inline constexpr RegVal MatrixRowNum = 128;
inline constexpr RegVal MatrixTrLenE8Max = 64;
inline constexpr RegVal MatrixTrLenE16Max = 32;
inline constexpr RegVal MatrixAccE32Max = 128;
inline constexpr RegVal MatrixSewE8 = 0;
inline constexpr RegVal MatrixSewE16 = 1;
inline constexpr RegVal MatrixSewE32 = 2;

enum class MatrixElemKind : uint8_t
{
    None,
    Int8,
    Fp16,
    Int32,
};

struct MatrixStaticInfo
{
    bool valid = false;
    uint8_t funct7 = 0;
    uint8_t rd = 0;
    bool loadLike = false;
    bool storeLike = false;
    bool needAmuCtrlCandidate = false;

    bool lsuIsA = false;
    bool lsuIsB = false;
    bool lsuIsAcc = false;
    bool lsuTranspose = false;
};

class MatrixStaticInst : public RiscvStaticInst
{
  protected:
    using RiscvStaticInst::RiscvStaticInst;
    MatrixStaticInfo matrixStaticInfo_;

  public:
    const MatrixStaticInfo &matrixStaticInfo() const { return matrixStaticInfo_; }
};

class MatrixCSROp : public CSROp
{
  protected:
    using CSROp::CSROp;
    MatrixStaticInfo matrixStaticInfo_;

  public:
    const MatrixStaticInfo &matrixStaticInfo() const { return matrixStaticInfo_; }
};

namespace matrix_static_info
{

inline bool
lsuEncodingLegal(const MatrixStaticInfo &info, uint8_t widths,
                 MatrixElemKind elem_kind)
{
    return (info.rd & 0x18) == 0 &&
           (info.lsuIsAcc ? (info.rd & 4) != 0 : (info.rd & 4) == 0) &&
           elem_kind != MatrixElemKind::None &&
           (info.lsuIsAcc ? widths == MatrixSewE32 :
            (widths == MatrixSewE8 || widths == MatrixSewE16));
}

} // namespace matrix_static_info

inline const auto MatrixRenamedTileMReg =
    RegId(RMiscRegClass, rmisc_reg::_MtilemIdx);
inline const auto MatrixRenamedTileNReg =
    RegId(RMiscRegClass, rmisc_reg::_MtilenIdx);
inline const auto MatrixRenamedTileKReg =
    RegId(RMiscRegClass, rmisc_reg::_MtilekIdx);

} // namespace RiscvISA
} // namespace gem5

#endif // __ARCH_RISCV_MATRIX_HH__
