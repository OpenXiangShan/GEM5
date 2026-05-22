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

namespace gem5
{

namespace matrix
{

bool
NullMatrixMemoryAdapter::loadTile(const AmuLsuDesc &desc, MatrixTensor &out_tensor)
{
    (void)desc;
    (void)out_tensor;
    return false;
}

bool
NullMatrixMemoryAdapter::storeTile(const AmuLsuDesc &desc,
                                   const MatrixTensor &tensor)
{
    (void)desc;
    (void)tensor;
    return false;
}

bool
SparseMatrixMemoryAdapter::loadTile(const AmuLsuDesc &desc,
                                    MatrixTensor &out_tensor)
{
    out_tensor.rows = desc.row;
    out_tensor.cols = desc.column;
    out_tensor.elemType = desc.elemType;
    out_tensor.elements.clear();
    out_tensor.elements.reserve(lsuElementCount(desc));

    for (uint32_t r = 0; r < desc.row; ++r) {
        for (uint32_t c = 0; c < desc.column; ++c) {
            int64_t value = 0;
            auto it = elements.find(lsuElementAddr(desc, r, c));
            if (it != elements.end()) {
                value = it->second;
            }
            out_tensor.elements.push_back(value);
        }
    }

    return true;
}

bool
SparseMatrixMemoryAdapter::storeTile(const AmuLsuDesc &desc,
                                     const MatrixTensor &tensor)
{
    if (!lsuTensorShapeMatches(desc, tensor)) {
        return false;
    }

    for (uint32_t r = 0; r < desc.row; ++r) {
        for (uint32_t c = 0; c < desc.column; ++c) {
            elements[lsuElementAddr(desc, r, c)] =
                tensor.elements[static_cast<size_t>(r) * tensor.cols + c];
        }
    }

    return true;
}

void
SparseMatrixMemoryAdapter::writeElement(Addr addr, int64_t value)
{
    elements[addr] = value;
}

bool
SparseMatrixMemoryAdapter::readElement(Addr addr, int64_t &value) const
{
    auto it = elements.find(addr);
    if (it == elements.end()) {
        return false;
    }

    value = it->second;
    return true;
}

} // namespace matrix
} // namespace gem5
