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

MatrixRegResource::Request
MatrixRegResource::makeRead(MatrixBankKind bank, Client client,
                            uint32_t entry)
{
    Request request;
    request.bank = bank;
    request.client = client;
    request.access = Access::Read;
    request.entry = entry;
    return request;
}

MatrixRegResource::Request
MatrixRegResource::makeWrite(MatrixBankKind bank, Client client,
                             uint32_t entry)
{
    Request request;
    request.bank = bank;
    request.client = client;
    request.access = Access::Write;
    request.entry = entry;
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

bool
MatrixRegResource::bankMaskIncludes(uint16_t bank_mask, unsigned bank)
{
    return (bank_mask & (1U << bank)) != 0;
}

void
MatrixRegResource::advanceCycle()
{
    ++cycle;
    for (auto &bank_tokens : abBankTokens) {
        for (auto &token : bank_tokens) {
            token = AbBankToken{};
        }
    }
}

bool
MatrixRegResource::currentCycleGrantBlocks(const Request &request,
                                           Grant &grant) const
{
    if (!isAbBank(request.bank)) {
        return false;
    }

    const auto &bank_tokens = abBankTokens[abBankIndex(request.bank)];
    bool blocked = false;
    for (unsigned bank = 0; bank < NumBanks; ++bank) {
        if (!bankMaskIncludes(request.bankMask, bank)) {
            continue;
        }
        const auto &token = bank_tokens[bank];
        if (!token.busy) {
            continue;
        }
        if (token.access == Access::Write &&
            request.access == Access::Read) {
            grant.reason = StallReason::AbWritePriority;
            return true;
        }
        blocked = true;
    }

    if (blocked) {
        grant.reason = StallReason::BankConflict;
        return true;
    }
    return false;
}

void
MatrixRegResource::markCycleGrant(const Request &request)
{
    if (!isAbBank(request.bank)) {
        return;
    }

    auto &bank_tokens = abBankTokens[abBankIndex(request.bank)];
    for (unsigned bank = 0; bank < NumBanks; ++bank) {
        if (!bankMaskIncludes(request.bankMask, bank)) {
            continue;
        }
        auto &token = bank_tokens[bank];
        token.busy = true;
        token.access = request.access;
    }
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
            grants[i].reason = StallReason::PartialBankMask;
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
                grants[i].reason = StallReason::InvalidOwner;
                continue;
            }
        } else if (request.bank == MatrixBankKind::C) {
            const bool valid_owner =
                (request.client == Client::DataController &&
                 fullBankMask(request.bankMask)) ||
                (request.client == Client::MemoryLoader &&
                 singleBankMask(request.bankMask));
            if (!valid_owner) {
                grants[i].reason = StallReason::PartialBankMask;
                continue;
            }
        }

        eligible[i] = true;
    }

    const auto try_grant = [&](size_t i) {
        const auto &request = requests[i];
        if (!eligible[i] || grants[i].granted ||
            grants[i].reason != StallReason::None) {
            return;
        }
        if (currentCycleGrantBlocks(request, grants[i])) {
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
