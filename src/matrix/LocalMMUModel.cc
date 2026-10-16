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
    : config(config_), sourceBusy(config.maxOutstanding, false)
{
    assert(config.maxOutstanding != 0);
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
LocalMmuModel::allocateSource(uint32_t &source_id)
{
    for (uint32_t i = sourceBusy.size(); i > 0; --i) {
        const uint32_t candidate = i - 1;
        if (!sourceBusy[candidate]) {
            sourceBusy[candidate] = true;
            source_id = candidate;
            return true;
        }
    }
    return false;
}

bool
LocalMmuModel::issueRequest(IssuedRequest &issued_request)
{
    if (outstanding.size() >= config.maxOutstanding) {
        return false;
    }

    const auto request_index = nextRequestIndex();
    if (!request_index.has_value()) {
        return false;
    }

    Request request = pending[*request_index].front();

    uint32_t source_id = 0;
    if (!allocateSource(source_id)) {
        return false;
    }

    pending[*request_index].pop_front();

    InFlight in_flight;
    in_flight.request = request;
    in_flight.sourceId = source_id;
    outstanding.push_back(in_flight);

    issued_request.request = request;
    issued_request.sourceId = source_id;
    return true;
}

std::deque<LocalMmuModel::InFlight>::iterator
LocalMmuModel::findOutstanding(uint32_t source_id)
{
    return std::find_if(
        outstanding.begin(), outstanding.end(),
        [source_id](const InFlight &in_flight) {
            return in_flight.sourceId == source_id;
        });
}

void
LocalMmuModel::freeSource(uint32_t source_id)
{
    assert(source_id < sourceBusy.size());
    assert(sourceBusy[source_id]);
    sourceBusy[source_id] = false;
}

void
LocalMmuModel::queueResponse(
    const InFlight &in_flight, const uint8_t *data, uint32_t size)
{
    Response response;
    response.seq = in_flight.request.seq;
    response.client = in_flight.request.client;
    response.isStore = in_flight.request.isStore;
    response.beatIndex = in_flight.request.beatIndex;
    response.byteSize = in_flight.request.byteSize;
    response.sourceId = in_flight.sourceId;
    response.metadata = in_flight.request.metadata;
    if (data != nullptr && size != 0) {
        const auto copy_size = std::min<size_t>(size, response.data.size());
        std::copy(data, data + copy_size, response.data.begin());
        response.hasData = true;
        response.dataSize = copy_size;
    }
    readyResponses.push_back(response);
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
    auto it = findOutstanding(source_id);
    if (it == outstanding.end()) {
        return false;
    }
    if (it->responseComplete) {
        return false;
    }

    const auto in_flight = *it;
    queueResponse(in_flight, data, size);
    it->responseComplete = true;
    return true;
}

bool
LocalMmuModel::releaseExternalSource(uint32_t source_id)
{
    auto it = findOutstanding(source_id);
    if (it == outstanding.end() || !it->responseComplete) {
        return false;
    }

    outstanding.erase(it);
    freeSource(source_id);
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
    : config(config_), slots(config.entryCount)
{
    assert(config.entryCount != 0);
    assert(config.bankFifoDepth != 0);
}

MatrixL2FillTable::Slot *
MatrixL2FillTable::findSlot(Handle handle)
{
    if (handle.slot >= slots.size()) {
        return nullptr;
    }
    auto &slot = slots[handle.slot];
    if (!slot.valid || slot.generation != handle.generation) {
        return nullptr;
    }
    return &slot;
}

const MatrixL2FillTable::Slot *
MatrixL2FillTable::findSlot(Handle handle) const
{
    if (handle.slot >= slots.size()) {
        return nullptr;
    }
    const auto &slot = slots[handle.slot];
    if (!slot.valid || slot.generation != handle.generation) {
        return nullptr;
    }
    return &slot;
}

bool
MatrixL2FillTable::canAccept(const Request &request) const
{
    if (request.fillChunks == 0) {
        return false;
    }

    return hasFreeEntry();
}

std::optional<MatrixL2FillTable::Handle>
MatrixL2FillTable::acceptResponse(const Request &request)
{
    if (!canAccept(request)) {
        return std::nullopt;
    }

    for (uint32_t index = 0; index < slots.size(); ++index) {
        auto &slot = slots[index];
        if (slot.valid) {
            continue;
        }

        slot.valid = true;
        slot.entry = Entry{};
        slot.entry.request = request;
        slot.entry.remainingFillChunks = request.fillChunks;
        pendingFillChunks[static_cast<size_t>(request.client)] +=
            request.fillChunks;
        return Handle{index, slot.generation};
    }

    return std::nullopt;
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

    const auto handle = acceptResponse(request);
    if (!handle.has_value()) {
        return std::nullopt;
    }

    bankFifos[request.targetBank].push_back(*handle);
    return handle;
}

std::optional<MatrixL2FillTable::DrainCandidate>
MatrixL2FillTable::drainCandidate(unsigned bank) const
{
    if (bank >= bankFifos.size() || bankFifos[bank].empty()) {
        return std::nullopt;
    }

    const auto handle = bankFifos[bank].front();
    const auto *slot = findSlot(handle);
    if (slot == nullptr || slot->entry.remainingFillChunks == 0) {
        return std::nullopt;
    }

    DrainCandidate candidate;
    candidate.handle = handle;
    candidate.client = slot->entry.request.client;
    candidate.destBank = slot->entry.request.destBank;
    candidate.targetBank = slot->entry.request.targetBank;
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

    if (!retireFillChunk(candidate.handle)) {
        return false;
    }

    if (!entryReadyToRelease(candidate.handle)) {
        return true;
    }

    fifo.pop_front();
    return releaseEntry(candidate.handle);
}

bool
MatrixL2FillTable::retireFillChunk(Handle handle)
{
    auto *slot = findSlot(handle);
    if (slot == nullptr || slot->entry.remainingFillChunks == 0) {
        return false;
    }

    auto &pending = pendingFillChunks[
        static_cast<size_t>(slot->entry.request.client)];
    assert(pending != 0);
    --pending;
    --slot->entry.remainingFillChunks;
    return true;
}

bool
MatrixL2FillTable::releaseEntry(Handle handle)
{
    auto *slot = findSlot(handle);
    if (slot == nullptr || !entryReadyToRelease(handle)) {
        return false;
    }

    slot->valid = false;
    slot->entry = Entry{};
    ++slot->generation;
    return true;
}

bool
MatrixL2FillTable::hasFreeEntry() const
{
    return std::any_of(
        slots.begin(), slots.end(),
        [](const Slot &slot) { return !slot.valid; });
}

bool
MatrixL2FillTable::entryReadyToRelease(Handle handle) const
{
    const auto *slot = findSlot(handle);
    return slot != nullptr && slot->entry.remainingFillChunks == 0;
}

size_t
MatrixL2FillTable::reservedCount() const
{
    return std::count_if(
        slots.begin(), slots.end(),
        [](const Slot &slot) { return slot.valid; });
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
