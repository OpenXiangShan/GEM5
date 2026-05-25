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

#ifndef __MATRIX_CUTE_PARAMETERS_HH__
#define __MATRIX_CUTE_PARAMETERS_HH__

#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{

class ThreadContext;

namespace matrix
{

enum class MatrixBankKind : uint8_t
{
    A,
    B,
    C
};

enum class MatrixElemType : uint8_t
{
    Int8,
    Int16,
    Int32,
    Int64,
    Fp16,
    Bf16,
    Tf32
};

struct MatrixTensor
{
    uint32_t rows = 0;
    uint32_t cols = 0;
    MatrixElemType elemType = MatrixElemType::Int8;
    std::vector<int64_t> elements;

    bool valid() const { return rows != 0 || cols != 0 || !elements.empty(); }
};

enum class MatrixArithOpcode : uint8_t
{
    Zero
};

enum class CuteRequestKind : uint8_t
{
    Lsu,
    Mma,
    Arith,
    Release
};

enum class CuteCompletionStatus : uint8_t
{
    Success,
    Unsupported
};

struct AmuLsuDesc
{
    uint8_t ms = 0;
    uint8_t widths = 0;
    bool isStore = false;
    bool transpose = false;
    bool isAcc = false;
    bool isA = false;
    bool isB = false;
    Addr baseAddr = 0;
    Addr physBaseAddr = 0;
    Addr stride = 0;
    uint32_t row = 0;
    uint32_t column = 0;
    MatrixElemType elemType = MatrixElemType::Int8;
    ThreadContext *tc = nullptr;
};

struct AmuMmaDesc
{
    uint8_t md = 0;
    uint8_t ms1 = 0;
    uint8_t ms2 = 0;
    uint32_t mtilem = 0;
    uint32_t mtilen = 0;
    uint32_t mtilek = 0;
    uint8_t rm = 0;
    uint8_t frm = 0;
    uint8_t types1 = 0;
    uint8_t types2 = 0;
    uint8_t typed = 0;
    MatrixElemType lhsElemType = MatrixElemType::Int8;
    MatrixElemType rhsElemType = MatrixElemType::Int8;
    MatrixElemType dstElemType = MatrixElemType::Int32;
    bool isFp = false;
    bool sat = false;
};

struct AmuArithDesc
{
    MatrixArithOpcode op = MatrixArithOpcode::Zero;
    MatrixBankKind bank = MatrixBankKind::C;
    uint8_t reg = 0;
    uint32_t rows = 0;
    uint32_t cols = 0;
    MatrixElemType elemType = MatrixElemType::Int32;
};

struct AmuReleaseDesc
{
    uint32_t tokenIndex = 0;
};

struct CuteRequest
{
    uint64_t seq = 0;
    CuteRequestKind kind = CuteRequestKind::Arith;
    RegVal op = 0;
    AmuLsuDesc lsu = {};
    AmuMmaDesc mma = {};
    AmuArithDesc arith = {};
    AmuReleaseDesc release = {};

    static CuteRequest
    makeArithZero(uint64_t seq, MatrixBankKind bank, uint8_t reg,
                  uint32_t rows, uint32_t cols, MatrixElemType elem_type)
    {
        CuteRequest req;
        req.seq = seq;
        req.kind = CuteRequestKind::Arith;
        req.op = 0x06;
        req.arith.op = MatrixArithOpcode::Zero;
        req.arith.bank = bank;
        req.arith.reg = reg;
        req.arith.rows = rows;
        req.arith.cols = cols;
        req.arith.elemType = elem_type;
        return req;
    }

    static CuteRequest
    makeRelease(uint64_t seq, uint32_t token_index)
    {
        CuteRequest req;
        req.seq = seq;
        req.kind = CuteRequestKind::Release;
        req.op = 0x48;
        req.release.tokenIndex = token_index;
        return req;
    }

    static CuteRequest
    makeMma(uint64_t seq, uint8_t md, uint8_t ms1, uint8_t ms2,
            uint32_t mtilem, uint32_t mtilen, uint32_t mtilek,
            MatrixElemType lhs_elem_type = MatrixElemType::Int8,
            MatrixElemType rhs_elem_type = MatrixElemType::Int8,
            MatrixElemType dst_elem_type = MatrixElemType::Int32)
    {
        CuteRequest req;
        req.seq = seq;
        req.kind = CuteRequestKind::Mma;
        req.op = 0x0c;
        req.mma.md = md;
        req.mma.ms1 = ms1;
        req.mma.ms2 = ms2;
        req.mma.mtilem = mtilem;
        req.mma.mtilen = mtilen;
        req.mma.mtilek = mtilek;
        req.mma.lhsElemType = lhs_elem_type;
        req.mma.rhsElemType = rhs_elem_type;
        req.mma.dstElemType = dst_elem_type;
        return req;
    }
};

inline bool
operator==(const AmuLsuDesc &lhs, const AmuLsuDesc &rhs)
{
    return lhs.ms == rhs.ms &&
           lhs.widths == rhs.widths &&
           lhs.isStore == rhs.isStore &&
           lhs.transpose == rhs.transpose &&
           lhs.isAcc == rhs.isAcc &&
           lhs.isA == rhs.isA &&
           lhs.isB == rhs.isB &&
           lhs.baseAddr == rhs.baseAddr &&
           lhs.physBaseAddr == rhs.physBaseAddr &&
           lhs.stride == rhs.stride &&
           lhs.row == rhs.row &&
           lhs.column == rhs.column &&
           lhs.elemType == rhs.elemType &&
           lhs.tc == rhs.tc;
}

inline bool
operator==(const AmuMmaDesc &lhs, const AmuMmaDesc &rhs)
{
    return lhs.md == rhs.md &&
           lhs.ms1 == rhs.ms1 &&
           lhs.ms2 == rhs.ms2 &&
           lhs.mtilem == rhs.mtilem &&
           lhs.mtilen == rhs.mtilen &&
           lhs.mtilek == rhs.mtilek &&
           lhs.rm == rhs.rm &&
           lhs.frm == rhs.frm &&
           lhs.types1 == rhs.types1 &&
           lhs.types2 == rhs.types2 &&
           lhs.typed == rhs.typed &&
           lhs.lhsElemType == rhs.lhsElemType &&
           lhs.rhsElemType == rhs.rhsElemType &&
           lhs.dstElemType == rhs.dstElemType &&
           lhs.isFp == rhs.isFp &&
           lhs.sat == rhs.sat;
}

inline bool
operator==(const AmuArithDesc &lhs, const AmuArithDesc &rhs)
{
    return lhs.op == rhs.op &&
           lhs.bank == rhs.bank &&
           lhs.reg == rhs.reg &&
           lhs.rows == rhs.rows &&
           lhs.cols == rhs.cols &&
           lhs.elemType == rhs.elemType;
}

inline bool
operator==(const AmuReleaseDesc &lhs, const AmuReleaseDesc &rhs)
{
    return lhs.tokenIndex == rhs.tokenIndex;
}

inline bool
operator==(const CuteRequest &lhs, const CuteRequest &rhs)
{
    return lhs.seq == rhs.seq &&
           lhs.kind == rhs.kind &&
           lhs.op == rhs.op &&
           lhs.lsu == rhs.lsu &&
           lhs.mma == rhs.mma &&
           lhs.arith == rhs.arith &&
           lhs.release == rhs.release;
}

struct CuteCompletion
{
    uint64_t seq = 0;
    CuteRequestKind kind = CuteRequestKind::Arith;
    CuteCompletionStatus status = CuteCompletionStatus::Success;
    bool hasTokenRelease = false;
    uint32_t tokenIdx = 0;
};

inline constexpr size_t
elemBytes(MatrixElemType elem_type)
{
    switch (elem_type) {
      case MatrixElemType::Int8:
        return 1;
      case MatrixElemType::Int16:
        return 2;
      case MatrixElemType::Int32:
        return 4;
      case MatrixElemType::Int64:
        return 8;
      case MatrixElemType::Fp16:
      case MatrixElemType::Bf16:
        return 2;
      case MatrixElemType::Tf32:
        return 4;
    }

    return 0;
}

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_CUTE_PARAMETERS_HH__
