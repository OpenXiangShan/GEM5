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

#ifndef __MATRIX_MATRIX_REGFILE_HH__
#define __MATRIX_MATRIX_REGFILE_HH__

#include <cstddef>
#include <vector>

#include "matrix/CUTEParameters.hh"

namespace gem5
{

namespace matrix
{

class MatrixRegFile
{
  public:
    struct RegMetadata
    {
        bool allocated = false;
    };

    struct Register
    {
        RegMetadata meta = {};
        MatrixTensor tensor = {};
    };

    static constexpr size_t DefaultAbRegCount = 8;
    static constexpr size_t DefaultCRegCount = 8;

    explicit MatrixRegFile(size_t ab_reg_count = DefaultAbRegCount,
                           size_t c_reg_count = DefaultCRegCount);

    size_t abRegCount() const { return _abRegCount; }
    size_t cRegCount() const { return _cRegCount; }
    size_t regCount(MatrixBankKind bank) const;

    bool hasRegister(MatrixBankKind bank, size_t reg_idx) const;
    const MatrixTensor &read(MatrixBankKind bank, size_t reg_idx) const;
    void write(MatrixBankKind bank, size_t reg_idx, const MatrixTensor &tensor);
    void zero(MatrixBankKind bank, size_t reg_idx, uint32_t rows,
              uint32_t cols, MatrixElemType elem_type);

    bool allocated(MatrixBankKind bank, size_t reg_idx) const;
    bool hasAllocatedState() const;

  private:
    std::vector<Register> &bank(MatrixBankKind bank);
    const std::vector<Register> &bank(MatrixBankKind bank) const;

    size_t _abRegCount;
    size_t _cRegCount;
    std::vector<Register> aRegs;
    std::vector<Register> bRegs;
    std::vector<Register> cRegs;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_MATRIX_REGFILE_HH__
