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

#include "matrix/matrix_memory_adapter.hh"
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

Addr
elemAddr(const AmuLsuDesc &desc, uint32_t row, uint32_t col)
{
    const Addr col_bytes = static_cast<Addr>(col) * elemBytes(desc.elemType);
    if (!desc.transpose) {
        return desc.baseAddr + static_cast<Addr>(row) * desc.stride + col_bytes;
    }

    return desc.baseAddr + static_cast<Addr>(col) * desc.stride +
           static_cast<Addr>(row) * elemBytes(desc.elemType);
}

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
readMatrixElem(const AmuLsuDesc &desc, PortProxy &fallback, Addr addr)
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
writeMatrixElem(const AmuLsuDesc &desc, PortProxy &fallback, Addr addr, T value)
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
    out_tensor.elements.reserve(static_cast<size_t>(desc.row) * desc.column);

    for (uint32_t r = 0; r < desc.row; ++r) {
        for (uint32_t c = 0; c < desc.column; ++c) {
            const Addr addr = elemAddr(desc, r, c);
            int64_t value = 0;
            switch (desc.elemType) {
              case MatrixElemType::Int8:
                value = static_cast<int8_t>(
                    readMatrixElem<uint8_t>(desc, *portProxy, addr));
                break;
              case MatrixElemType::Int16:
                value = static_cast<int16_t>(
                    readMatrixElem<uint16_t>(desc, *portProxy, addr));
                break;
              case MatrixElemType::Int32:
                value = static_cast<int32_t>(
                    readMatrixElem<uint32_t>(desc, *portProxy, addr));
                break;
              case MatrixElemType::Int64:
                value = static_cast<int64_t>(
                    readMatrixElem<uint64_t>(desc, *portProxy, addr));
                break;
              case MatrixElemType::Fp16:
              case MatrixElemType::Bf16:
                value = static_cast<int64_t>(
                    readMatrixElem<uint16_t>(desc, *portProxy, addr));
                break;
              case MatrixElemType::Tf32:
                value = static_cast<int64_t>(
                    readMatrixElem<uint32_t>(desc, *portProxy, addr));
                break;
            }
            out_tensor.elements.push_back(value);
        }
    }

    return true;
}

bool
Gem5MatrixMemoryAdapter::storeTile(const AmuLsuDesc &desc,
                                   const MatrixTensor &tensor)
{
    if (!portProxy || tensor.rows != desc.row || tensor.cols != desc.column ||
        tensor.elemType != desc.elemType) {
        return false;
    }

    for (uint32_t r = 0; r < desc.row; ++r) {
        for (uint32_t c = 0; c < desc.column; ++c) {
            const Addr addr = elemAddr(desc, r, c);
            const int64_t value =
                tensor.elements[static_cast<size_t>(r) * tensor.cols + c];
            switch (desc.elemType) {
              case MatrixElemType::Int8:
                writeMatrixElem<uint8_t>(
                    desc, *portProxy, addr, static_cast<uint8_t>(value));
                break;
              case MatrixElemType::Int16:
                writeMatrixElem<uint16_t>(
                    desc, *portProxy, addr, static_cast<uint16_t>(value));
                break;
              case MatrixElemType::Int32:
                writeMatrixElem<uint32_t>(
                    desc, *portProxy, addr, static_cast<uint32_t>(value));
                break;
              case MatrixElemType::Int64:
                writeMatrixElem<uint64_t>(
                    desc, *portProxy, addr, static_cast<uint64_t>(value));
                break;
              case MatrixElemType::Fp16:
              case MatrixElemType::Bf16:
                writeMatrixElem<uint16_t>(
                    desc, *portProxy, addr, static_cast<uint16_t>(value));
                break;
              case MatrixElemType::Tf32:
                writeMatrixElem<uint32_t>(
                    desc, *portProxy, addr, static_cast<uint32_t>(value));
                break;
            }
        }
    }

    return true;
}

} // namespace matrix
} // namespace gem5
