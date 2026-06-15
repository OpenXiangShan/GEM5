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

#include "matrix/MatrixTE.hh"

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

#include "matrix/CUTETOP.hh"

namespace gem5
{

namespace matrix
{

namespace
{

CuteCompletion
makeCompletion(uint64_t seq, CuteRequestKind kind, CuteCompletionStatus status)
{
    CuteCompletion completion;
    completion.seq = seq;
    completion.kind = kind;
    completion.status = status;
    return completion;
}

constexpr unsigned RtlTensorMn = 128;
constexpr unsigned RtlTensorK = 64;
constexpr unsigned RtlMatrixMn = 8;
constexpr unsigned RtlReduceWidthBytes = 32;
constexpr unsigned RtlResultWidthBytes = 4;
constexpr unsigned MatrixRegReadResponseCycles = 1;
constexpr unsigned FReducePePipelineTailCycles = 6;
constexpr unsigned MicroTaskEndHandshakeCycles = 1;

bool
isFloatingElemType(MatrixElemType elem_type)
{
    return elem_type == MatrixElemType::Fp16 ||
           elem_type == MatrixElemType::Bf16 ||
           elem_type == MatrixElemType::Tf32;
}

std::optional<MatrixElemType>
fpElemTypeForMmaEncoding(uint8_t type_encoding)
{
    switch (type_encoding & 0x7) {
      case 0x1:
        return MatrixElemType::Fp16;
      case 0x5:
        return MatrixElemType::Bf16;
      case 0x6:
        return MatrixElemType::Tf32;
      default:
        return std::nullopt;
    }
}

bool
isFpMmaRequest(const AmuMmaDesc &desc)
{
    return desc.isFp || isFloatingElemType(desc.lhsElemType) ||
           isFloatingElemType(desc.rhsElemType) ||
           isFloatingElemType(desc.dstElemType);
}

float
bitsToFloat(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

uint32_t
floatToBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float
fp16ToFloat(uint16_t bits)
{
    const uint32_t sign = static_cast<uint32_t>(bits & 0x8000) << 16;
    uint32_t exp = (bits >> 10) & 0x1f;
    uint32_t mant = bits & 0x3ff;
    uint32_t fp32_bits = 0;

    if (exp == 0) {
        if (mant == 0) {
            fp32_bits = sign;
        } else {
            exp = 1;
            while ((mant & 0x400) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ff;
            exp = exp + (127 - 15);
            fp32_bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1f) {
        fp32_bits = sign | 0x7f800000 | (mant << 13);
    } else {
        exp = exp + (127 - 15);
        fp32_bits = sign | (exp << 23) | (mant << 13);
    }

    return bitsToFloat(fp32_bits);
}

uint32_t
fp16Mac(const MatrixTensor &a, const MatrixTensor &b, const MatrixTensor &c,
        uint32_t m, uint32_t n, uint32_t k_count)
{
    float acc = bitsToFloat(static_cast<uint32_t>(
        c.elements[static_cast<size_t>(m) * c.cols + n]));
    for (uint32_t k = 0; k < k_count; ++k) {
        const auto lhs_bits = static_cast<uint16_t>(
            a.elements[static_cast<size_t>(m) * a.cols + k]);
        const auto rhs_bits = static_cast<uint16_t>(
            b.elements[static_cast<size_t>(k) * b.cols + n]);
        acc = static_cast<float>(acc + fp16ToFloat(lhs_bits) *
                                       fp16ToFloat(rhs_bits));
    }
    return floatToBits(acc);
}

std::optional<MatrixElemType>
intElemTypeForMmaEncoding(uint8_t type_encoding)
{
    switch (type_encoding & 0x3) {
      case 0x0:
        return MatrixElemType::Int8;
      case 0x1:
        return MatrixElemType::Int16;
      case 0x2:
        return MatrixElemType::Int32;
      case 0x3:
        return MatrixElemType::Int64;
    }

    return std::nullopt;
}

std::pair<unsigned, unsigned>
kScaleForMmaEncoding(uint8_t type_encoding)
{
    switch (type_encoding & 0x3) {
      case 0x0:
        return {1, 1};
      case 0x1:
        return {2, 1};
      case 0x2:
        return {4, 1};
      default:
        return {1, 2};
    }
}

std::pair<unsigned, unsigned>
kScaleForElemType(MatrixElemType elem_type)
{
    switch (elem_type) {
      case MatrixElemType::Int8:
        return {1, 1};
      case MatrixElemType::Int16:
      case MatrixElemType::Fp16:
      case MatrixElemType::Bf16:
        return {2, 1};
      case MatrixElemType::Int32:
      case MatrixElemType::Tf32:
        return {4, 1};
      case MatrixElemType::Int64:
        return {8, 1};
    }

    return {1, 1};
}

unsigned
ceilDiv(unsigned lhs, unsigned rhs)
{
    return (lhs + rhs - 1) / rhs;
}

std::pair<unsigned, unsigned>
kScaleForMmaDesc(const AmuMmaDesc &desc)
{
    if (desc.types1 != 0) {
        return kScaleForMmaEncoding(desc.types1);
    }

    return kScaleForElemType(desc.lhsElemType);
}

} // anonymous namespace

CuteCompletion
DetailedCuteBackend::executeMma(uint64_t seq, const AmuMmaDesc &desc,
                                MatrixRegFile &state)
{
    if (!computeDatatypeSupported(desc)) {
        return makeCompletion(seq, CuteRequestKind::Mma,
                              CuteCompletionStatus::Unsupported);
    }

    auto lhs_type = desc.lhsElemType;
    auto rhs_type = desc.rhsElemType;
    auto dst_type = desc.dstElemType;
    const bool fp_request = isFpMmaRequest(desc);
    if (fp_request) {
        if (!isFloatingElemType(lhs_type) && desc.types1 != 0) {
            lhs_type = fpElemTypeForMmaEncoding(desc.types1).value();
        }
        if (!isFloatingElemType(rhs_type) && desc.types2 != 0) {
            rhs_type = fpElemTypeForMmaEncoding(desc.types2).value();
        }
    } else {
        if (!isFloatingElemType(lhs_type) && desc.types1 != 0) {
            lhs_type = intElemTypeForMmaEncoding(desc.types1).value();
        }
        if (!isFloatingElemType(rhs_type) && desc.types2 != 0) {
            rhs_type = intElemTypeForMmaEncoding(desc.types2).value();
        }
        if (!isFloatingElemType(dst_type) && desc.typed != 0) {
            dst_type = intElemTypeForMmaEncoding(desc.typed).value();
        }
    }

    if (!state.hasRegister(MatrixBankKind::A, desc.ms1) ||
        !state.hasRegister(MatrixBankKind::B, desc.ms2)) {
        return makeCompletion(seq, CuteRequestKind::Mma,
                              CuteCompletionStatus::Unsupported);
    }

    const auto &a = state.read(MatrixBankKind::A, desc.ms1);
    const auto &b = state.read(MatrixBankKind::B, desc.ms2);
    if (a.elemType != lhs_type || b.elemType != rhs_type ||
        a.rows != desc.mtilem || a.cols != desc.mtilek ||
        b.rows != desc.mtilek || b.cols != desc.mtilen) {
        return makeCompletion(seq, CuteRequestKind::Mma,
                              CuteCompletionStatus::Unsupported);
    }

    if (!state.hasRegister(MatrixBankKind::C, desc.md)) {
        state.zero(MatrixBankKind::C, desc.md, desc.mtilem, desc.mtilen,
                   dst_type);
    }

    auto c = state.read(MatrixBankKind::C, desc.md);
    if (c.elemType != dst_type ||
        c.rows != desc.mtilem || c.cols != desc.mtilen) {
        return makeCompletion(seq, CuteRequestKind::Mma,
                              CuteCompletionStatus::Unsupported);
    }

    for (uint32_t m = 0; m < desc.mtilem; ++m) {
        for (uint32_t n = 0; n < desc.mtilen; ++n) {
            if (fp_request) {
                c.elements[static_cast<size_t>(m) * c.cols + n] = fp16Mac(
                    a, b, c, m, n, desc.mtilek);
                continue;
            }

            int64_t acc = c.elements[static_cast<size_t>(m) * c.cols + n];
            for (uint32_t k = 0; k < desc.mtilek; ++k) {
                const int64_t lhs = a.elements[static_cast<size_t>(m) *
                                               a.cols + k];
                const int64_t rhs = b.elements[static_cast<size_t>(k) *
                                               b.cols + n];
                acc += lhs * rhs;
            }
            c.elements[static_cast<size_t>(m) * c.cols + n] = acc;
        }
    }

    state.write(MatrixBankKind::C, desc.md, c);
    return makeCompletion(seq, CuteRequestKind::Mma, CuteCompletionStatus::Success);
}

// Active compute path: datatype gating and MMA latency progression.
bool
DetailedCuteBackend::computeDatatypeSupported(const AmuMmaDesc &desc) const
{
    if (!computeMteTiming(desc).supported) {
        return false;
    }

    if (isFpMmaRequest(desc)) {
        const auto lhs_type =
            isFloatingElemType(desc.lhsElemType) ? desc.lhsElemType :
            fpElemTypeForMmaEncoding(desc.types1).value_or(desc.lhsElemType);
        const auto rhs_type =
            isFloatingElemType(desc.rhsElemType) ? desc.rhsElemType :
            fpElemTypeForMmaEncoding(desc.types2).value_or(desc.rhsElemType);

        return lhs_type == MatrixElemType::Fp16 &&
               rhs_type == MatrixElemType::Fp16 &&
               desc.dstElemType == MatrixElemType::Int32 &&
               (desc.types1 == 0 || desc.types1 == 0x1) &&
               (desc.types2 == 0 || desc.types2 == 0x1) &&
               (desc.typed == 0 || desc.typed == 0x2);
    }

    if (desc.types1 == 0 && desc.types2 == 0 && desc.typed == 0) {
        return true;
    }

    return desc.typed == 0x2 &&
           ((desc.types1 & 0x3) == (desc.types2 & 0x3));
}

MteTiming
DetailedCuteBackend::computeMteTiming(const AmuMmaDesc &desc) const
{
    MteTiming timing;
    timing.tensorMn = RtlTensorMn;
    timing.tensorK = RtlTensorK;
    timing.matrixMn = RtlMatrixMn;
    timing.reduceWidthBytes = RtlReduceWidthBytes;
    timing.resultWidthBytes = RtlResultWidthBytes;
    timing.aBytesPerBeat = RtlReduceWidthBytes * RtlMatrixMn;
    timing.bBytesPerBeat = RtlReduceWidthBytes * RtlMatrixMn;
    timing.cBytesPerBeat = RtlResultWidthBytes * RtlMatrixMn * RtlMatrixMn;
    timing.dBytesPerBeat = timing.cBytesPerBeat;

    if (desc.mtilem == 0 || desc.mtilen == 0 || desc.mtilek == 0 ||
        desc.mtilem > RtlTensorMn ||
        desc.mtilen > RtlTensorMn ||
        desc.mtilek > RtlTensorK ||
        desc.mtilen != RtlTensorMn) {
        return timing;
    }

    const auto [k_scale_num, k_scale_den] = kScaleForMmaDesc(desc);
    const unsigned scaled_k_bytes =
        (desc.mtilek * k_scale_num) / k_scale_den;
    const unsigned m_iters = ceilDiv(desc.mtilem, RtlMatrixMn);
    const unsigned n_iters = desc.mtilen / RtlMatrixMn;
    const unsigned k_iters = scaled_k_bytes / RtlReduceWidthBytes;
    if (k_iters == 0) {
        return timing;
    }

    timing.acceptedInputBeats = m_iters * n_iters * k_iters;
    timing.adcReadCycles = MatrixRegReadResponseCycles;
    timing.bdcReadCycles = MatrixRegReadResponseCycles;
    timing.cdcReadCycles = MatrixRegReadResponseCycles;
    timing.mteAcceptedInputBeats = timing.acceptedInputBeats;
    timing.fReduceTailCycles = FReducePePipelineTailCycles;
    timing.cdcWriteCycles = timing.acceptedInputBeats;
    timing.terminalHandshakeCycles = MicroTaskEndHandshakeCycles;
    timing.totalCompletionCycles =
        std::max({timing.adcReadCycles, timing.bdcReadCycles,
                  timing.cdcReadCycles}) +
        timing.mteAcceptedInputBeats + timing.fReduceTailCycles +
        timing.cdcWriteCycles + timing.terminalHandshakeCycles;
    timing.supported = true;
    return timing;
}

unsigned
DetailedCuteBackend::computeExecuteLatency(const DecodedFifoEntry &entry) const
{
    assert(entry.isMma);

    const auto timing = computeMteTiming(entry.request.mma);
    if (!timing.supported) {
        return 1;
    }

    return std::max(1U, timing.acceptedInputBeats +
                        FReducePePipelineTailCycles);
}

} // namespace matrix
} // namespace gem5
