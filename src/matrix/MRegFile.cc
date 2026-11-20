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

#include <algorithm>
#include <cassert>

namespace gem5
{

namespace matrix
{

namespace
{

constexpr unsigned RegisterRows = 128;

size_t
rowBytes(MatrixBankKind bank)
{
    return bank == MatrixBankKind::C ? 512 : 64;
}

size_t
registerOffset(MatrixBankKind bank, size_t row, size_t col, size_t width)
{
    // A is M x K and B is K x N. Both address the same physical N/M x K
    // tile storage, while A and B retain separate timing resource ports.
    return bank == MatrixBankKind::B ? col * rowBytes(bank) + row * width :
                                       row * rowBytes(bank) + col * width;
}

} // anonymous namespace

MatrixRegResource::Request
MatrixRegResource::makeRead(MatrixBankKind bank, Client client)
{
    Request request;
    request.bank = bank;
    request.client = client;
    request.access = Access::Read;
    return request;
}

MatrixRegResource::Request
MatrixRegResource::makeWrite(MatrixBankKind bank, Client client)
{
    Request request;
    request.bank = bank;
    request.client = client;
    request.access = Access::Write;
    return request;
}

bool
MatrixRegResource::isAbBank(MatrixBankKind bank)
{
    return bank == MatrixBankKind::A || bank == MatrixBankKind::B;
}

size_t
MatrixRegResource::abBankIndex(MatrixBankKind bank)
{
    return bank == MatrixBankKind::A ? 0 : 1;
}

bool
MatrixRegResource::validBankMask(uint16_t bank_mask)
{
    return bank_mask != 0 && (bank_mask & ~FullBankMask) == 0;
}

bool
MatrixRegResource::fullBankMask(uint16_t bank_mask)
{
    return bank_mask == FullBankMask;
}

bool
MatrixRegResource::singleBankMask(uint16_t bank_mask)
{
    return validBankMask(bank_mask) &&
           (bank_mask & (bank_mask - 1)) == 0;
}

void
MatrixRegResource::advanceCycle()
{
    ++cycle;
    abBusyBanks.fill(0);
}

bool
MatrixRegResource::currentCycleGrantBlocks(const Request &request) const
{
    if (!isAbBank(request.bank)) {
        return false;
    }

    return (abBusyBanks[abBankIndex(request.bank)] & request.bankMask) != 0;
}

void
MatrixRegResource::markCycleGrant(const Request &request)
{
    if (!isAbBank(request.bank)) {
        return;
    }

    abBusyBanks[abBankIndex(request.bank)] |= request.bankMask;
}

void
MatrixRegResource::enqueueReadResponse(const Request &request)
{
    ReadResponse response;
    response.bank = request.bank;
    response.client = request.client;
    response.readyCycle = cycle + ReadLatencyCycles;
    readResponses.push_back(response);
}

std::vector<MatrixRegResource::Grant>
MatrixRegResource::arbitrate(const std::vector<Request> &requests)
{
    std::vector<Grant> grants(requests.size());
    std::vector<bool> eligible(requests.size(), false);

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto &request = requests[i];
        if (!validBankMask(request.bankMask)) {
            continue;
        }

        if (isAbBank(request.bank)) {
            const bool valid_owner =
                (request.client == Client::DataController &&
                 request.access == Access::Read &&
                 fullBankMask(request.bankMask)) ||
                (request.client == Client::MemoryLoader &&
                 request.access == Access::Write);
            if (!valid_owner) {
                continue;
            }
        } else if (request.bank == MatrixBankKind::C) {
            const bool valid_owner =
                (request.client == Client::DataController &&
                 fullBankMask(request.bankMask)) ||
                (request.client == Client::MemoryLoader &&
                 singleBankMask(request.bankMask));
            if (!valid_owner) {
                continue;
            }
        }

        eligible[i] = true;
    }

    const auto try_grant = [&](size_t i) {
        const auto &request = requests[i];
        if (!eligible[i] || grants[i].granted) {
            return;
        }
        if (currentCycleGrantBlocks(request)) {
            return;
        }

        grants[i].granted = true;
        markCycleGrant(request);
        if (request.access == Access::Read) {
            enqueueReadResponse(request);
        }
    };

    for (size_t i = 0; i < requests.size(); ++i) {
        if (isAbBank(requests[i].bank) &&
            requests[i].access == Access::Write) {
            try_grant(i);
        }
    }

    for (size_t i = 0; i < requests.size(); ++i) {
        try_grant(i);
    }

    return grants;
}

bool
MatrixRegResource::consumeReadResponse(MatrixBankKind bank, Client client)
{
    auto it = std::find_if(
        readResponses.begin(), readResponses.end(),
        [&](const ReadResponse &response) {
            return response.bank == bank &&
                   response.client == client &&
                   response.readyCycle <= cycle;
        });
    if (it == readResponses.end()) {
        return false;
    }

    readResponses.erase(it);
    return true;
}

MatrixRegFile::MatrixRegFile(size_t ab_reg_count, size_t c_reg_count)
    : tileRegs(ab_reg_count), accRegs(c_reg_count)
{
}

bool
MatrixRegFile::read(MatrixBankKind bank, size_t reg_idx, uint32_t rows,
                    uint32_t cols, MatrixElemType type,
                    MatrixTensor &tensor) const
{
    const auto &reg = (bank == MatrixBankKind::C ? accRegs : tileRegs).at(reg_idx);
    const size_t width = elemBytes(type);
    const size_t physical_rows = bank == MatrixBankKind::B ? cols : rows;
    const size_t used_bytes = (bank == MatrixBankKind::B ? rows : cols) * width;
    if (physical_rows > RegisterRows || used_bytes > rowBytes(bank))
        return false;

    // Unwritten accumulator bytes read as zero, including after a partial load.
    if (bank != MatrixBankKind::C) {
        if (physical_rows > reg.validRowBytes.size() ||
            std::any_of(reg.validRowBytes.begin(),
                        reg.validRowBytes.begin() + physical_rows,
                        [used_bytes](size_t n) { return n < used_bytes; })) {
            return false;
        }
    }

    tensor = {rows, cols, type, {}};
    if (reg.bytes.empty()) {
        tensor.elements.resize(size_t(rows) * cols);
        return true;
    }
    tensor.elements.reserve(size_t(rows) * cols);
    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned col = 0; col < cols; ++col) {
            const size_t offset = registerOffset(bank, row, col, width);
            uint64_t raw = 0;
            for (size_t byte = 0; byte < width; ++byte) {
                raw |= uint64_t(reg.bytes[offset + byte]) << (8 * byte);
            }
            tensor.elements.push_back(raw);
        }
    }
    return true;
}

void
MatrixRegFile::write(MatrixBankKind bank, size_t reg_idx,
                     const MatrixTensor &tensor)
{
    auto &reg = (bank == MatrixBankKind::C ? accRegs : tileRegs).at(reg_idx);
    const size_t width = elemBytes(tensor.elemType);
    const unsigned physical_rows = bank == MatrixBankKind::B ?
        tensor.cols : tensor.rows;
    const size_t used_bytes = (bank == MatrixBankKind::B ?
        tensor.rows : tensor.cols) * width;
    assert(physical_rows <= RegisterRows && used_bytes <= rowBytes(bank));
    assert(tensor.elements.size() == size_t(tensor.rows) * tensor.cols);
    if (reg.bytes.empty()) {
        reg.bytes.resize(RegisterRows * rowBytes(bank));
        reg.validRowBytes.resize(RegisterRows);
    }
    for (unsigned row = 0; row < physical_rows; ++row) {
        reg.validRowBytes[row] = std::max(reg.validRowBytes[row],
                                          uint16_t(used_bytes));
    }
    for (unsigned row = 0; row < tensor.rows; ++row) {
        for (unsigned col = 0; col < tensor.cols; ++col) {
            const size_t offset = registerOffset(bank, row, col, width);
            const uint64_t value = tensor.elements[size_t(row) * tensor.cols + col];
            for (size_t byte = 0; byte < width; ++byte) {
                reg.bytes[offset + byte] = value >> (8 * byte);
            }
        }
    }
}

bool
MatrixRegFile::hasAllocatedState() const
{
    const auto allocated = [](const Register &reg) { return !reg.bytes.empty(); };
    return std::any_of(tileRegs.begin(), tileRegs.end(), allocated) ||
           std::any_of(accRegs.begin(), accRegs.end(), allocated);
}

} // namespace matrix
} // namespace gem5
