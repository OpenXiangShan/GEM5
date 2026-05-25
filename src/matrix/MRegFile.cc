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

#include "matrix/MRegFile.hh"

#include <cassert>

namespace gem5
{

namespace matrix
{

namespace
{

MatrixTensor
makeZeroTensor(uint32_t rows, uint32_t cols, MatrixElemType elem_type)
{
    MatrixTensor tensor;
    tensor.rows = rows;
    tensor.cols = cols;
    tensor.elemType = elem_type;
    tensor.elements.assign(static_cast<size_t>(rows) * cols, 0);
    return tensor;
}

} // anonymous namespace

MatrixRegFile::MatrixRegFile(size_t ab_reg_count, size_t c_reg_count)
    : _abRegCount(ab_reg_count), _cRegCount(c_reg_count),
      aRegs(ab_reg_count), bRegs(ab_reg_count), cRegs(c_reg_count)
{
}

size_t
MatrixRegFile::regCount(MatrixBankKind bank_kind) const
{
    return bank_kind == MatrixBankKind::C ? _cRegCount : _abRegCount;
}

std::vector<MatrixRegFile::Register> &
MatrixRegFile::bank(MatrixBankKind bank_kind)
{
    switch (bank_kind) {
      case MatrixBankKind::A:
        return aRegs;
      case MatrixBankKind::B:
        return bRegs;
      case MatrixBankKind::C:
        return cRegs;
    }

    return cRegs;
}

const std::vector<MatrixRegFile::Register> &
MatrixRegFile::bank(MatrixBankKind bank_kind) const
{
    switch (bank_kind) {
      case MatrixBankKind::A:
        return aRegs;
      case MatrixBankKind::B:
        return bRegs;
      case MatrixBankKind::C:
        return cRegs;
    }

    return cRegs;
}

bool
MatrixRegFile::hasRegister(MatrixBankKind bank_kind, size_t reg_idx) const
{
    assert(reg_idx < regCount(bank_kind));
    return bank(bank_kind)[reg_idx].meta.allocated;
}

const MatrixTensor &
MatrixRegFile::read(MatrixBankKind bank_kind, size_t reg_idx) const
{
    assert(reg_idx < regCount(bank_kind));
    const auto &reg = bank(bank_kind)[reg_idx];
    assert(reg.meta.allocated);
    return reg.tensor;
}

void
MatrixRegFile::write(MatrixBankKind bank_kind, size_t reg_idx,
                     const MatrixTensor &tensor)
{
    assert(reg_idx < regCount(bank_kind));
    auto &reg = bank(bank_kind)[reg_idx];
    reg.meta.allocated = true;
    reg.tensor = tensor;
}

void
MatrixRegFile::zero(MatrixBankKind bank_kind, size_t reg_idx, uint32_t rows,
                    uint32_t cols, MatrixElemType elem_type)
{
    write(bank_kind, reg_idx, makeZeroTensor(rows, cols, elem_type));
}

bool
MatrixRegFile::allocated(MatrixBankKind bank_kind, size_t reg_idx) const
{
    assert(reg_idx < regCount(bank_kind));
    return bank(bank_kind)[reg_idx].meta.allocated;
}

bool
MatrixRegFile::hasAllocatedState() const
{
    const auto has_allocated = [](const auto &regs) {
        for (const auto &reg : regs) {
            if (reg.meta.allocated) {
                return true;
            }
        }
        return false;
    };

    return has_allocated(aRegs) || has_allocated(bRegs) ||
           has_allocated(cRegs);
}

} // namespace matrix
} // namespace gem5
