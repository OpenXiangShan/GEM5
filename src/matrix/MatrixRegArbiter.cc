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

#include "matrix/MatrixRegArbiter.hh"

#include <algorithm>
#include <optional>

namespace gem5
{

namespace matrix
{

MatrixRegResource::Request
MatrixRegResource::makeRead(MatrixBankKind bank, Client client,
                            uint32_t entry)
{
    return {bank, client, Access::Read, entry};
}

MatrixRegResource::Request
MatrixRegResource::makeWrite(MatrixBankKind bank, Client client,
                             uint32_t entry)
{
    return {bank, client, Access::Write, entry};
}

void
MatrixRegResource::enqueueReadResponse(const Request &request)
{
    readResponses.push_back(
        {request.bank, request.client, cycle + ReadLatencyCycles});
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
        if (grants[i].granted && requests[i].access == Access::Read) {
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

} // namespace matrix
} // namespace gem5
