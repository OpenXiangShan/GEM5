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

#include "matrix/LocalMMUModel.hh"

#include <algorithm>
#include <cassert>

namespace gem5
{

namespace matrix
{

LocalMmuModel::LocalMmuModel(Config config_)
    : sources(config_.maxOutstanding)
{
    assert(config_.maxOutstanding != 0);
}

size_t
LocalMmuModel::clientIndex(Client client)
{
    return static_cast<size_t>(client);
}

bool
LocalMmuModel::enqueue(const Request &request)
{
    if (request.byteSize == 0 || request.byteSize > 64) {
        return false;
    }
    const auto index = clientIndex(request.client);
    assert(index < ClientCount);
    pending[index].push_back(request);
    return true;
}

std::optional<size_t>
LocalMmuModel::nextRequestIndex() const
{
    for (size_t offset = 0; offset < ClientCount; ++offset) {
        const auto index = (firstRequestIndex + offset) % ClientCount;
        if (!pending[index].empty()) {
            return index;
        }
    }
    return std::nullopt;
}

bool
LocalMmuModel::issueRequest(IssuedRequest &issued_request)
{
    if (outstandingFull()) {
        return false;
    }

    const auto request_index = nextRequestIndex();
    if (!request_index.has_value()) {
        return false;
    }

    for (uint32_t i = sources.size(); i > 0; --i) {
        auto &slot = sources[i - 1];
        if (!slot) {
            slot = InFlight{pending[*request_index].front()};
            pending[*request_index].pop_front();
            issued_request.request = slot->request;
            issued_request.sourceId = i - 1;
            ++activeSources;
            return true;
        }
    }
    return false;
}

bool
LocalMmuModel::issueExternal(IssuedRequest &issued_request)
{
    const bool issued_request_valid = issueRequest(issued_request);
    firstRequestIndex = (firstRequestIndex + 1) % ClientCount;
    return issued_request_valid;
}

bool
LocalMmuModel::completeExternalResponse(uint32_t source_id)
{
    return completeExternalResponse(source_id, nullptr, 0);
}

bool
LocalMmuModel::completeExternalResponse(
    uint32_t source_id, const uint8_t *data, uint32_t size)
{
    if (source_id >= sources.size() || !sources[source_id] ||
        sources[source_id]->responseComplete) {
        return false;
    }
    auto &slot = *sources[source_id];
    const auto &request = slot.request;
    Response response;
    response.seq = request.seq;
    response.client = request.client;
    response.isStore = request.isStore;
    response.beatIndex = request.beatIndex;
    response.byteSize = request.byteSize;
    response.sourceId = source_id;
    response.destBank = request.destBank;
    if (data != nullptr && size != 0) {
        const auto copy_size = std::min<size_t>(size, response.data.size());
        std::copy(data, data + copy_size, response.data.begin());
        response.hasData = true;
        response.dataSize = copy_size;
    }
    readyResponses.push_back(response);
    slot.responseComplete = true;
    return true;
}

bool
LocalMmuModel::releaseExternalSource(uint32_t source_id)
{
    if (source_id >= sources.size() || !sources[source_id] ||
        !sources[source_id]->responseComplete) {
        return false;
    }

    sources[source_id].reset();
    --activeSources;
    return true;
}

std::vector<LocalMmuModel::Response>
LocalMmuModel::takeReadyResponses()
{
    std::vector<Response> responses(
        readyResponses.begin(), readyResponses.end());
    readyResponses.clear();
    return responses;
}

MatrixL2FillTable::MatrixL2FillTable(Config config_)
    : config(config_), entries(config.entryCount)
{
    assert(config.entryCount != 0);
    assert(config.bankFifoDepth != 0);
}

MatrixL2FillTable::Entry *
MatrixL2FillTable::findEntry(Handle handle)
{
    if (handle.slot >= entries.size()) {
        return nullptr;
    }
    auto &entry = entries[handle.slot];
    if (!entry.valid || entry.generation != handle.generation) {
        return nullptr;
    }
    return &entry;
}

const MatrixL2FillTable::Entry *
MatrixL2FillTable::findEntry(Handle handle) const
{
    if (handle.slot >= entries.size()) {
        return nullptr;
    }
    const auto &entry = entries[handle.slot];
    if (!entry.valid || entry.generation != handle.generation) {
        return nullptr;
    }
    return &entry;
}

bool
MatrixL2FillTable::canAccept(const Request &request) const
{
    if (request.fillChunks == 0) {
        return false;
    }

    return hasFreeEntry();
}

bool
MatrixL2FillTable::canAcceptResponse(const Request &request) const
{
    if (!canAccept(request) || request.targetBank >= bankFifos.size()) {
        return false;
    }

    return bankFifos[request.targetBank].size() < config.bankFifoDepth;
}

std::optional<MatrixL2FillTable::Handle>
MatrixL2FillTable::acceptResponseToBank(const Request &request)
{
    if (!canAcceptResponse(request)) {
        return std::nullopt;
    }

    for (uint32_t index = 0; index < entries.size(); ++index) {
        auto &entry = entries[index];
        if (entry.valid) {
            continue;
        }

        entry.valid = true;
        entry.client = request.client;
        entry.destBank = request.destBank;
        entry.remainingFillChunks = request.fillChunks;
        entry.targetBank = request.targetBank;
        pendingFillChunks[static_cast<size_t>(request.client)] +=
            request.fillChunks;
        const Handle handle{index, entry.generation};
        bankFifos[request.targetBank].push_back(handle);
        return handle;
    }

    return std::nullopt;
}

std::optional<MatrixL2FillTable::DrainCandidate>
MatrixL2FillTable::drainCandidate(unsigned bank) const
{
    if (bank >= bankFifos.size() || bankFifos[bank].empty()) {
        return std::nullopt;
    }

    const auto handle = bankFifos[bank].front();
    const auto *entry = findEntry(handle);
    if (entry == nullptr || entry->remainingFillChunks == 0) {
        return std::nullopt;
    }

    DrainCandidate candidate;
    candidate.handle = handle;
    candidate.client = entry->client;
    candidate.destBank = entry->destBank;
    candidate.targetBank = entry->targetBank;
    return candidate;
}

bool
MatrixL2FillTable::retireDrain(const DrainCandidate &candidate)
{
    if (candidate.targetBank >= bankFifos.size()) {
        return false;
    }
    auto &fifo = bankFifos[candidate.targetBank];
    if (fifo.empty() || fifo.front() != candidate.handle) {
        return false;
    }

    auto *entry = findEntry(candidate.handle);
    if (entry == nullptr || entry->remainingFillChunks == 0) {
        return false;
    }

    auto &pending = pendingFillChunks[static_cast<size_t>(entry->client)];
    assert(pending != 0);
    --pending;
    --entry->remainingFillChunks;
    if (entry->remainingFillChunks == 0) {
        fifo.pop_front();
        const auto generation = entry->generation + 1;
        *entry = Entry{};
        entry->generation = generation;
    }
    return true;
}

bool
MatrixL2FillTable::hasFreeEntry() const
{
    return std::any_of(
        entries.begin(), entries.end(),
        [](const Entry &entry) { return !entry.valid; });
}

size_t
MatrixL2FillTable::reservedCount() const
{
    return std::count_if(
        entries.begin(), entries.end(),
        [](const Entry &entry) { return entry.valid; });
}

bool
MatrixL2FillTable::hasPendingFillChunks(LocalMmuModel::Client client) const
{
    return pendingFillChunks[static_cast<size_t>(client)] != 0;
}

size_t
MatrixL2FillTable::bankFifoOccupancy(unsigned bank) const
{
    if (bank >= bankFifos.size()) {
        return 0;
    }
    return bankFifos[bank].size();
}

} // namespace matrix
} // namespace gem5
