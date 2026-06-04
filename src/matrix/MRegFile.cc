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
#include <optional>

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

void
MatrixRegResource::advanceCycle()
{
    ++cycle;
    for (auto &token : abBankTokens) {
        token = AbBankToken{};
    }
    cBankToken = CBankToken{};
}

bool
MatrixRegResource::currentCycleGrantBlocks(const Request &request,
                                           Grant &grant) const
{
    if (isAbBank(request.bank)) {
        const auto &token = abBankTokens[abBankIndex(request.bank)];
        if (!token.busy) {
            return false;
        }
        grant.reason = token.access == Access::Write &&
                       request.access == Access::Read ?
            StallReason::AbWritePriority : StallReason::BankConflict;
        return true;
    }

    if (request.bank != MatrixBankKind::C) {
        return false;
    }

    const uint32_t parity = request.entry & 1;
    if (request.access == Access::Read) {
        if (cBankToken.readBusy) {
            grant.reason = StallReason::BankConflict;
            return true;
        }
        if (cBankToken.writeBusy && cBankToken.writeParity == parity) {
            grant.reason = StallReason::CReadWriteConflict;
            return true;
        }
        return false;
    }

    if (cBankToken.writeBusy) {
        grant.reason = StallReason::BankConflict;
        return true;
    }
    if (cBankToken.readBusy && cBankToken.readParity == parity) {
        grant.reason = StallReason::CReadWriteConflict;
        return true;
    }
    return false;
}

void
MatrixRegResource::markCycleGrant(const Request &request)
{
    if (isAbBank(request.bank)) {
        auto &token = abBankTokens[abBankIndex(request.bank)];
        token.busy = true;
        token.access = request.access;
        return;
    }

    if (request.bank != MatrixBankKind::C) {
        return;
    }

    const uint32_t parity = request.entry & 1;
    if (request.access == Access::Read) {
        cBankToken.readBusy = true;
        cBankToken.readParity = parity;
    } else {
        cBankToken.writeBusy = true;
        cBankToken.writeParity = parity;
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
    std::vector<bool> eligible(requests.size(), true);

    for (size_t i = 0; i < requests.size(); ++i) {
        if (requests[i].bankMask != FullBankMask) {
            grants[i].reason = StallReason::PartialBankMask;
            eligible[i] = false;
        }
    }

    for (size_t i = 0; i < requests.size(); ++i) {
        if (!eligible[i]) {
            continue;
        }
        if (currentCycleGrantBlocks(requests[i], grants[i])) {
            eligible[i] = false;
        }
    }

    for (const auto bank : {MatrixBankKind::A, MatrixBankKind::B}) {
        std::vector<size_t> reads;
        std::vector<size_t> writes;
        for (size_t i = 0; i < requests.size(); ++i) {
            if (!eligible[i] || requests[i].bank != bank) {
                continue;
            }
            if (requests[i].access == Access::Read) {
                reads.push_back(i);
            } else {
                writes.push_back(i);
            }
        }

        if (!writes.empty()) {
            grants[writes.front()].granted = true;
            for (size_t i = 1; i < writes.size(); ++i) {
                grants[writes[i]].reason = StallReason::BankConflict;
            }
            for (const auto read_idx : reads) {
                grants[read_idx].reason = StallReason::AbWritePriority;
            }
        } else if (!reads.empty()) {
            grants[reads.front()].granted = true;
            for (size_t i = 1; i < reads.size(); ++i) {
                grants[reads[i]].reason = StallReason::BankConflict;
            }
        }
    }

    std::vector<size_t> c_reads;
    std::vector<size_t> c_writes;
    for (size_t i = 0; i < requests.size(); ++i) {
        if (!eligible[i] || requests[i].bank != MatrixBankKind::C) {
            continue;
        }
        if (requests[i].access == Access::Read) {
            c_reads.push_back(i);
        } else {
            c_writes.push_back(i);
        }
    }

    std::optional<size_t> c_write_grant;
    if (!c_writes.empty()) {
        c_write_grant = c_writes.front();
        grants[*c_write_grant].granted = true;
        for (size_t i = 1; i < c_writes.size(); ++i) {
            grants[c_writes[i]].reason = StallReason::BankConflict;
        }
    }

    if (!c_reads.empty()) {
        const bool same_parity_write =
            c_write_grant &&
            ((requests[c_reads.front()].entry & 1) ==
             (requests[*c_write_grant].entry & 1));
        if (same_parity_write) {
            grants[c_reads.front()].reason =
                StallReason::CReadWriteConflict;
            if (c_writes.size() == 1) {
                grants[*c_write_grant].granted = false;
                grants[*c_write_grant].reason =
                    StallReason::CReadWriteConflict;
            }
        } else if (c_writes.size() > 1) {
            grants[c_reads.front()].reason = StallReason::BankConflict;
        } else {
            grants[c_reads.front()].granted = true;
        }
        for (size_t i = 1; i < c_reads.size(); ++i) {
            grants[c_reads[i]].reason = StallReason::BankConflict;
        }
    }

    for (size_t i = 0; i < requests.size(); ++i) {
        if (!grants[i].granted) {
            continue;
        }
        markCycleGrant(requests[i]);
        if (requests[i].access == Access::Read) {
            enqueueReadResponse(requests[i]);
        }
    }

    return grants;
}

bool
MatrixRegResource::readResponseReady(MatrixBankKind bank, Client client) const
{
    return std::any_of(
        readResponses.begin(), readResponses.end(),
        [&](const ReadResponse &response) {
            return response.bank == bank &&
                   response.client == client &&
                   response.readyCycle <= cycle;
        });
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
