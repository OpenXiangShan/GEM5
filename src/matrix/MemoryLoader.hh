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

#ifndef __MATRIX_MEMORY_LOADER_HH__
#define __MATRIX_MEMORY_LOADER_HH__

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "base/types.hh"
#include "matrix/CUTEParameters.hh"
#include "matrix/LocalMMUModel.hh"
#include "matrix/MatrixTimingMemoryAdapter.hh"

namespace gem5
{

namespace matrix
{

struct TimingLoadPlan
{
    struct Beat
    {
        struct Target
        {
            uint8_t lineOffset;
            size_t tensorOffset;
        };

        Addr paddr = 0;
        uint32_t byteSize = 0;
        uint64_t byteMask = 0;
        std::vector<Target> tensorTargets;
    };

    size_t tensorBytes = 0;
    std::vector<Beat> beats;
};

struct TimingStorePlan
{
    struct Beat
    {
        Addr paddr = 0;
        uint32_t packetSize = 0;
        uint64_t byteMask = 0;
        std::array<uint8_t, 64> lineData = {};
    };

    size_t tensorBytes = 0;
    std::vector<Beat> beats;
};

using TimingAddressTranslator =
    std::function<bool(Addr vaddr, uint32_t size, Addr &paddr)>;

inline TimingAddressTranslator
frozenMatrixTranslator(FrozenMatrixPageMappings mappings)
{
    if (!mappings) {
        return {};
    }
    return [mappings = std::move(mappings)](
               Addr vaddr, uint32_t size, Addr &paddr) {
        const Addr page = vaddr & ~Addr(4095);
        const auto it = std::lower_bound(
            mappings->begin(), mappings->end(), page,
            [](const MatrixPageMapping &mapping, Addr value) {
                return mapping.vpage < value;
            });
        if (size == 0 || size > 4096 - (vaddr & 4095) ||
            it == mappings->end() || it->vpage != page) {
            return false;
        }
        paddr = it->ppage + (vaddr & 4095);
        return true;
    };
}

namespace timing_load_detail
{

inline constexpr Addr CacheLineBytes = 64;

inline Addr
elemAddr(const AmuLsuDesc &desc, uint32_t row, uint32_t col)
{
    const Addr col_bytes = static_cast<Addr>(col) * elemBytes(desc.elemType);
    if (!desc.transpose) {
        return desc.baseAddr + static_cast<Addr>(row) * desc.stride +
               col_bytes;
    }

    return desc.baseAddr + static_cast<Addr>(col) * desc.stride +
           static_cast<Addr>(row) * elemBytes(desc.elemType);
}

inline Addr
elemPhysAddr(const AmuLsuDesc &desc, uint32_t row, uint32_t col)
{
    return desc.physBaseAddr + (elemAddr(desc, row, col) - desc.baseAddr);
}

inline Addr
lineBase(Addr addr)
{
    return addr & ~(CacheLineBytes - 1);
}

template <class Visit>
inline void
forEachTensorByte(const AmuLsuDesc &desc,
                  const TimingAddressTranslator &translate,
                  Visit &&visit)
{
    const size_t bytes_per_elem = elemBytes(desc.elemType);
    for (uint32_t row = 0; row < desc.row; ++row) {
        for (uint32_t col = 0; col < desc.column; ++col) {
            const Addr elem_vaddr = elemAddr(desc, row, col);
            const Addr elem_paddr = translate ?
                elem_vaddr : elemPhysAddr(desc, row, col);
            const size_t elem_index =
                static_cast<size_t>(row) * desc.column + col;
            for (size_t byte = 0; byte < bytes_per_elem; ++byte) {
                const Addr byte_vaddr = elem_vaddr + byte;
                const Addr byte_paddr = elem_paddr + byte;
                const Addr line = translate ?
                    lineBase(byte_vaddr) : lineBase(byte_paddr);
                const size_t line_offset = translate ?
                    byte_vaddr - line : byte_paddr - line;
                visit(line, line_offset, elem_index, byte);
            }
        }
    }
}

} // namespace timing_load_detail

inline TimingLoadPlan buildTimingLoadPlan(
    const AmuLsuDesc &desc, const TimingAddressTranslator &translate);
inline TimingStorePlan buildTimingStorePlan(
    const AmuLsuDesc &desc, const MatrixTensor &tensor,
    const TimingAddressTranslator &translate);

inline TimingLoadPlan
buildTimingLoadPlan(const AmuLsuDesc &desc)
{
    return buildTimingLoadPlan(desc, {});
}

inline TimingLoadPlan
buildTimingLoadPlan(const AmuLsuDesc &desc,
                    const TimingAddressTranslator &translate)
{
    TimingLoadPlan plan;
    plan.tensorBytes = static_cast<size_t>(desc.row) * desc.column *
                       elemBytes(desc.elemType);

    std::map<Addr, TimingLoadPlan::Beat> beats_by_vline;
    timing_load_detail::forEachTensorByte(
        desc, translate,
        [&](Addr line, size_t line_offset, size_t elem_index, size_t byte) {
            auto &beat = beats_by_vline[line];
            if (beat.byteSize == 0) {
                beat.paddr = line;
                beat.byteSize = timing_load_detail::CacheLineBytes;
            }
            beat.byteMask |= uint64_t(1) << line_offset;
            beat.tensorTargets.push_back({
                static_cast<uint8_t>(line_offset),
                elem_index * elemBytes(desc.elemType) + byte});
        });

    plan.beats.reserve(beats_by_vline.size());
    for (auto &entry : beats_by_vline) {
        if (translate) {
            Addr paddr = 0;
            if (!translate(entry.first, entry.second.byteSize, paddr)) {
                plan = {};
                return plan;
            }
            entry.second.paddr = paddr;
        }
        plan.beats.push_back(std::move(entry.second));
    }

    return plan;
}

inline TimingStorePlan
buildTimingStorePlan(const AmuLsuDesc &desc, const MatrixTensor &tensor)
{
    return buildTimingStorePlan(desc, tensor, {});
}

inline uint64_t
timingStoreElementRaw(MatrixElemType elem_type, int64_t value)
{
    switch (elem_type) {
      case MatrixElemType::Int8:
        return static_cast<uint8_t>(value);
      case MatrixElemType::Int16:
      case MatrixElemType::Fp16:
      case MatrixElemType::Bf16:
        return static_cast<uint16_t>(value);
      case MatrixElemType::Int32:
      case MatrixElemType::Tf32:
        return static_cast<uint32_t>(value);
      case MatrixElemType::Int64:
        return static_cast<uint64_t>(value);
    }

    return 0;
}

inline TimingStorePlan
buildTimingStorePlan(const AmuLsuDesc &desc, const MatrixTensor &tensor,
                     const TimingAddressTranslator &translate)
{
    TimingStorePlan plan;
    const size_t bytes_per_elem = elemBytes(desc.elemType);
    const size_t element_count =
        static_cast<size_t>(desc.row) * desc.column;
    plan.tensorBytes = element_count * bytes_per_elem;
    if (bytes_per_elem == 0 ||
        tensor.rows != desc.row || tensor.cols != desc.column ||
        tensor.elemType != desc.elemType ||
        tensor.elements.size() != element_count) {
        plan = {};
        return plan;
    }

    std::map<Addr, TimingStorePlan::Beat> beats_by_vline;
    timing_load_detail::forEachTensorByte(
        desc, translate,
        [&](Addr line, size_t line_offset, size_t elem_index, size_t byte) {
            auto &beat = beats_by_vline[line];
            if (beat.packetSize == 0) {
                beat.paddr = line;
                beat.packetSize = timing_load_detail::CacheLineBytes;
            }
            const uint64_t raw = timingStoreElementRaw(
                desc.elemType, tensor.elements[elem_index]);
            assert(line_offset < beat.lineData.size());
            beat.lineData[line_offset] =
                static_cast<uint8_t>(raw >> (byte * 8));
            beat.byteMask |= uint64_t(1) << line_offset;
        });

    plan.beats.reserve(beats_by_vline.size());
    for (auto &entry : beats_by_vline) {
        if (translate) {
            Addr paddr = 0;
            if (!translate(entry.first, entry.second.packetSize, paddr)) {
                plan = {};
                return plan;
            }
            entry.second.paddr = paddr;
        }
        plan.beats.push_back(std::move(entry.second));
    }

    return plan;
}

inline bool
scatterTimingLoadResponse(const TimingLoadPlan &plan,
                          uint32_t beat_index,
                          const uint8_t *data,
                          uint32_t data_size,
                          std::vector<uint8_t> &tensor_bytes)
{
    if (beat_index >= plan.beats.size() || data == nullptr) {
        return false;
    }

    const auto &beat = plan.beats[beat_index];
    if (data_size < beat.byteSize ||
        tensor_bytes.size() != plan.tensorBytes ||
        beat.tensorTargets.empty()) {
        return false;
    }

    for (const auto &target : beat.tensorTargets) {
        if (target.lineOffset >= beat.byteSize ||
            target.tensorOffset >= plan.tensorBytes ||
            !(beat.byteMask & (uint64_t(1) << target.lineOffset))) {
            return false;
        }
        tensor_bytes[target.tensorOffset] = data[target.lineOffset];
    }

    return true;
}

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_MEMORY_LOADER_HH__
