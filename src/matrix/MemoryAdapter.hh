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

#ifndef __MATRIX_MEMORYADAPTER_HH__
#define __MATRIX_MEMORYADAPTER_HH__

#include <unordered_map>

#include "matrix/CUTEParameters.hh"

namespace gem5
{

class PortProxy;

namespace matrix
{

class MatrixMemoryAdapter
{
  public:
    virtual ~MatrixMemoryAdapter() = default;

    virtual bool loadTile(const AmuLsuDesc &desc, MatrixTensor &out_tensor) = 0;
    virtual bool storeTile(const AmuLsuDesc &desc,
                           const MatrixTensor &tensor) = 0;
};

class NullMatrixMemoryAdapter : public MatrixMemoryAdapter
{
  public:
    bool loadTile(const AmuLsuDesc &desc, MatrixTensor &out_tensor) override;
    bool storeTile(const AmuLsuDesc &desc,
                   const MatrixTensor &tensor) override;
};

class SparseMatrixMemoryAdapter : public MatrixMemoryAdapter
{
  public:
    bool loadTile(const AmuLsuDesc &desc, MatrixTensor &out_tensor) override;
    bool storeTile(const AmuLsuDesc &desc,
                   const MatrixTensor &tensor) override;

    void writeElement(Addr addr, int64_t value);
    bool readElement(Addr addr, int64_t &value) const;

  private:
    std::unordered_map<Addr, int64_t> elements;
};

class Gem5MatrixMemoryAdapter : public MatrixMemoryAdapter
{
  public:
    explicit Gem5MatrixMemoryAdapter(PortProxy &proxy);

    bool loadTile(const AmuLsuDesc &desc, MatrixTensor &out_tensor) override;
    bool storeTile(const AmuLsuDesc &desc,
                   const MatrixTensor &tensor) override;

  private:
    PortProxy *portProxy = nullptr;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_MEMORYADAPTER_HH__
