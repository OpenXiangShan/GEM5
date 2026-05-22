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

#include "matrix/MemoryAdapter.hh"
#include "mem/port_proxy.hh"
#include "mem/se_translating_port_proxy.hh"
#include "mem/translating_port_proxy.hh"
#include "sim/byteswap.hh"
#include "sim/full_system.hh"

namespace gem5
{

namespace matrix
{

namespace
{

template <typename T>
T
readGuestElem(PortProxy &proxy, Addr addr)
{
    return proxy.read<T>(addr, ByteOrder::little);
}

template <typename T>
void
writeGuestElem(PortProxy &proxy, Addr addr, T value)
{
    proxy.write<T>(addr, value, ByteOrder::little);
}

template <typename T>
T
readMatrixElem(const AmuLsuDesc &desc, Addr addr)
{
    if (!desc.tc) {
        panic("Matrix access missing thread context");
    }

    if (!FullSystem) {
        SETranslatingPortProxy proxy(desc.tc, SETranslatingPortProxy::Never);
        return readGuestElem<T>(proxy, addr);
    }

    TranslatingPortProxy proxy(desc.tc);
    return readGuestElem<T>(proxy, addr);
}

template <typename T>
void
writeMatrixElem(const AmuLsuDesc &desc, Addr addr, T value)
{
    if (!desc.tc) {
        panic("Matrix access missing thread context");
    }

    if (!FullSystem) {
        SETranslatingPortProxy proxy(desc.tc, SETranslatingPortProxy::Never);
        writeGuestElem<T>(proxy, addr, value);
        return;
    }

    TranslatingPortProxy proxy(desc.tc);
    writeGuestElem<T>(proxy, addr, value);
}

int64_t
loadMatrixElement(const AmuLsuDesc &desc, Addr addr)
{
    switch (desc.elemType) {
      case MatrixElemType::Int8:
        return static_cast<int8_t>(
            readMatrixElem<uint8_t>(desc, addr));
      case MatrixElemType::Int16:
        return static_cast<int16_t>(
            readMatrixElem<uint16_t>(desc, addr));
      case MatrixElemType::Int32:
        return static_cast<int32_t>(
            readMatrixElem<uint32_t>(desc, addr));
      case MatrixElemType::Int64:
        return static_cast<int64_t>(
            readMatrixElem<uint64_t>(desc, addr));
      case MatrixElemType::Fp16:
      case MatrixElemType::Bf16:
        return static_cast<int64_t>(
            readMatrixElem<uint16_t>(desc, addr));
      case MatrixElemType::Tf32:
        return static_cast<int64_t>(
            readMatrixElem<uint32_t>(desc, addr));
    }

    return 0;
}

void
storeMatrixElement(const AmuLsuDesc &desc, Addr addr, int64_t value)
{
    switch (desc.elemType) {
      case MatrixElemType::Int8:
        writeMatrixElem<uint8_t>(
            desc, addr, static_cast<uint8_t>(value));
        return;
      case MatrixElemType::Int16:
        writeMatrixElem<uint16_t>(
            desc, addr, static_cast<uint16_t>(value));
        return;
      case MatrixElemType::Int32:
        writeMatrixElem<uint32_t>(
            desc, addr, static_cast<uint32_t>(value));
        return;
      case MatrixElemType::Int64:
        writeMatrixElem<uint64_t>(
            desc, addr, static_cast<uint64_t>(value));
        return;
      case MatrixElemType::Fp16:
      case MatrixElemType::Bf16:
        writeMatrixElem<uint16_t>(
            desc, addr, static_cast<uint16_t>(value));
        return;
      case MatrixElemType::Tf32:
        writeMatrixElem<uint32_t>(
            desc, addr, static_cast<uint32_t>(value));
        return;
    }
}

} // anonymous namespace

Gem5MatrixMemoryAdapter::Gem5MatrixMemoryAdapter(PortProxy &proxy)
  : portProxy(&proxy)
{
}

bool
Gem5MatrixMemoryAdapter::loadTile(const AmuLsuDesc &desc,
                                  MatrixTensor &out_tensor)
{
    if (!portProxy) {
        return false;
    }

    out_tensor.rows = desc.row;
    out_tensor.cols = desc.column;
    out_tensor.elemType = desc.elemType;
    out_tensor.elements.clear();
    out_tensor.elements.reserve(lsuElementCount(desc));

    for (uint32_t r = 0; r < desc.row; ++r) {
        for (uint32_t c = 0; c < desc.column; ++c) {
            out_tensor.elements.push_back(
                loadMatrixElement(desc, lsuElementAddr(desc, r, c)));
        }
    }

    return true;
}

bool
Gem5MatrixMemoryAdapter::storeTile(const AmuLsuDesc &desc,
                                   const MatrixTensor &tensor)
{
    if (!portProxy || !lsuTensorShapeMatches(desc, tensor)) {
        return false;
    }

    for (uint32_t r = 0; r < desc.row; ++r) {
        for (uint32_t c = 0; c < desc.column; ++c) {
            const int64_t value =
                tensor.elements[static_cast<size_t>(r) * tensor.cols + c];
            storeMatrixElement(desc, lsuElementAddr(desc, r, c), value);
        }
    }

    return true;
}

} // namespace matrix
} // namespace gem5
