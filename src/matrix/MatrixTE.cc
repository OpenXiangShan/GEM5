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
#include <utility>

#include "matrix/CUTETOP.hh"

namespace gem5
{

namespace matrix
{

namespace
{

constexpr unsigned RtlTensorMn = 128;
constexpr unsigned RtlTensorK = 64;
constexpr unsigned RtlMatrixMn = 8;
constexpr unsigned RtlReduceWidthBytes = 32;
constexpr unsigned FReducePePipelineTailCycles = 6;

bool
isSupportedInt8Mma(const AmuMmaDesc &desc)
{
    return !desc.isFp &&
           desc.lhsElemType == MatrixElemType::Int8 &&
           desc.rhsElemType == MatrixElemType::Int8 &&
           desc.dstElemType == MatrixElemType::Int32 &&
           (desc.types1 == 0 || desc.types1 == 0x4) &&
           (desc.types2 == 0 || desc.types2 == 0x4) &&
           (desc.typed == 0 || desc.typed == 0x2);
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

// Active compute path: datatype gating and MMA latency progression.
bool
DetailedCuteBackend::computeDatatypeSupported(const AmuMmaDesc &desc) const
{
    return computeMteTiming(desc).supported && isSupportedInt8Mma(desc);
}

MteTiming
DetailedCuteBackend::computeMteTiming(const AmuMmaDesc &desc) const
{
    MteTiming timing;

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
    timing.cdcWriteCycles = timing.acceptedInputBeats;
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
