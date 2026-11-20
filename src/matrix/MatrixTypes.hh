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

#ifndef __MATRIX_TYPES_HH__
#define __MATRIX_TYPES_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "base/types.hh"

namespace gem5
{

class ThreadContext;

namespace matrix
{

struct MatrixPageMapping
{
    Addr vpage;
    Addr ppage;
};

using MatrixPageMappings = std::vector<MatrixPageMapping>;
using FrozenMatrixPageMappings = std::shared_ptr<const MatrixPageMappings>;

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

inline const char *
requestKindName(CuteRequestKind kind)
{
    switch (kind) {
      case CuteRequestKind::Lsu: return "lsu";
      case CuteRequestKind::Mma: return "mma";
      case CuteRequestKind::Arith: return "arith";
      case CuteRequestKind::Release: return "release";
    }
    return "unknown";
}

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
    FrozenMatrixPageMappings pageMappings;
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
    ThreadID tid = 0;
    uint64_t seq = 0;
    Addr pc = 0;
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

struct CuteCompletion
{
    uint64_t seq = 0;
    CuteRequestKind kind = CuteRequestKind::Arith;
    CuteCompletionStatus status = CuteCompletionStatus::Success;
    uint32_t tokenIdx = 0;
    ThreadID tid = 0;
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

// Decoded FIFO normalizes request fields for issue gating and tracing.
struct DecodedFifoEntry
{
    CuteRequest request = {};

    std::array<uint8_t, 3> readRegs = {0, 0, 0};
    std::array<bool, 3> readValid = {false, false, false};
    std::array<uint8_t, 1> writeRegs = {0};
    std::array<bool, 1> writeValid = {false};

    bool isLoad = false;
    bool isStore = false;
    bool isMma = false;
    bool isZeroAcc = false;
    bool isRelease = false;
};

inline MatrixBankKind
destBank(const DecodedFifoEntry &entry)
{
    if (entry.isMma || entry.isZeroAcc) {
        return MatrixBankKind::C;
    }
    if (entry.isLoad) {
        if (entry.request.lsu.isAcc) {
            return MatrixBankKind::C;
        }
        return entry.request.lsu.isB ? MatrixBankKind::B : MatrixBankKind::A;
    }
    return MatrixBankKind::C;
}

inline DecodedFifoEntry
decodeCuteRequest(const CuteRequest &req)
{
    DecodedFifoEntry entry;
    entry.request = req;

    switch (req.kind) {
      case CuteRequestKind::Lsu:
        entry.isLoad = !req.lsu.isStore;
        entry.isStore = req.lsu.isStore;
        if (entry.isLoad) {
            entry.writeRegs[0] = req.lsu.ms;
            entry.writeValid[0] = true;
        } else {
            entry.readRegs[0] = req.lsu.ms;
            entry.readValid[0] = true;
        }
        break;
      case CuteRequestKind::Mma:
        entry.isMma = true;
        entry.readRegs = {req.mma.ms1, req.mma.ms2, req.mma.md};
        entry.readValid = {true, true, true};
        entry.writeRegs[0] = req.mma.md;
        entry.writeValid[0] = true;
        break;
      case CuteRequestKind::Arith:
        entry.isZeroAcc = true;
        entry.readRegs[0] = req.arith.reg;
        entry.readValid[0] = true;
        entry.writeRegs[0] = req.arith.reg;
        entry.writeValid[0] = true;
        break;
      case CuteRequestKind::Release:
        entry.isRelease = true;
        break;
    }

    return entry;
}

enum class MatrixRequestError : uint8_t
{
    None,
    UnsupportedMma,
    UnsupportedLsu,
    UnsupportedArith,
    UnsupportedRelease,
};

inline bool
supportedInt8MmaShape(uint32_t m, uint32_t n, uint32_t k)
{
    return m >= 8 && m <= 128 && m % 8 == 0 &&
           n == 128 && (k == 32 || k == 64);
}

inline MatrixRequestError
validateMatrixRequest(const CuteRequest &req)
{
    switch (req.kind) {
      case CuteRequestKind::Mma:
      {
        const auto &mma = req.mma;
        if (!supportedInt8MmaShape(mma.mtilem, mma.mtilen, mma.mtilek) ||
            mma.isFp || mma.sat || mma.lhsElemType != MatrixElemType::Int8 ||
            mma.rhsElemType != MatrixElemType::Int8 ||
            mma.dstElemType != MatrixElemType::Int32 ||
            (mma.types1 != 0 && mma.types1 != 0x4) ||
            (mma.types2 != 0 && mma.types2 != 0x4) ||
            (mma.typed != 0 && mma.typed != 0x2)) {
            return MatrixRequestError::UnsupportedMma;
        }
        return MatrixRequestError::None;
      }
      case CuteRequestKind::Lsu:
      {
        const auto &lsu = req.lsu;
        const bool ab = lsu.isA != lsu.isB && !lsu.isAcc;
        const bool acc = lsu.isAcc && !lsu.isA && !lsu.isB;
        const bool ab_width =
            (lsu.widths == 0 && lsu.elemType == MatrixElemType::Int8) ||
            (lsu.widths == 1 && lsu.elemType == MatrixElemType::Fp16);
        const bool acc_width = lsu.widths == 2 &&
            lsu.elemType == MatrixElemType::Int32;
        if (lsu.row == 0 || lsu.column == 0 ||
            lsu.row > 128 || lsu.column > 128 ||
            (!(ab && ab_width) && !(acc && acc_width)) ||
            (lsu.isStore && !acc)) {
            return MatrixRequestError::UnsupportedLsu;
        }
        return MatrixRequestError::None;
      }
      case CuteRequestKind::Arith:
        return req.op == 0x06 &&
               req.arith.op == MatrixArithOpcode::Zero &&
               req.arith.reg < 8 &&
               req.arith.bank == MatrixBankKind::C &&
               req.arith.rows <= 128 && req.arith.cols <= 128 &&
               req.arith.elemType == MatrixElemType::Int32 ?
            MatrixRequestError::None : MatrixRequestError::UnsupportedArith;
      case CuteRequestKind::Release:
        return req.release.tokenIndex < 32 ?
            MatrixRequestError::None :
            MatrixRequestError::UnsupportedRelease;
    }
    return MatrixRequestError::UnsupportedMma;
}

class MatrixTimingMemoryAdapter
{
  public:
    struct Request
    {
        uint64_t seq = 0;
        ThreadID tid = 0;
        bool isStore = false;
        Addr paddr = 0;
        uint32_t packetSize = 64;
        uint32_t sourceId = 0;
        ContextID contextId = InvalidContextID;
        std::array<uint8_t, 64> data = {};
        uint64_t byteMask = 0;
    };

    virtual ~MatrixTimingMemoryAdapter() = default;

    virtual bool connected() const = 0;
    virtual bool sendTimingRequest(const Request &request) = 0;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_TYPES_HH__
