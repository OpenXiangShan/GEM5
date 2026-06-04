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

LocalMmuModel::LocalMmuModel()
    : LocalMmuModel(Config())
{
}

LocalMmuModel::LocalMmuModel(Config config_)
    : config(config_), sourceBusy(config.maxOutstanding, false)
{
    assert(config.latencyCycles != 0);
    assert(config.maxOutstanding != 0);
}

size_t
LocalMmuModel::clientIndex(Client client)
{
    return static_cast<size_t>(client);
}

uint64_t
LocalMmuModel::byteMaskForSize(uint32_t byte_size)
{
    if (byte_size == 0) {
        return 0;
    }
    if (byte_size >= 64) {
        return ~uint64_t(0);
    }
    return (uint64_t(1) << byte_size) - 1;
}

LocalMmuModel::MatrixL2Metadata
LocalMmuModel::normalizedMetadata(const Request &request)
{
    MatrixL2Metadata metadata = request.metadata;
    if (!metadata.valid) {
        metadata.byteMask = byteMaskForSize(request.byteSize);
    }

    metadata.valid = true;
    metadata.seq = request.seq;
    metadata.client = request.client;
    metadata.isStore = request.isStore;
    metadata.beatIndex = request.beatIndex;
    metadata.byteSize = request.byteSize;
    return metadata;
}

bool
LocalMmuModel::enqueue(const Request &request)
{
    if (request.byteSize == 0 || request.byteSize > 64) {
        return false;
    }
    const auto index = clientIndex(request.client);
    assert(index < ClientCount);
    Request queued_request = request;
    queued_request.metadata = normalizedMetadata(request);
    pending[index].push_back(queued_request);
    return true;
}

size_t
LocalMmuModel::pendingCount() const
{
    size_t count = 0;
    for (const auto &queue : pending) {
        count += queue.size();
    }
    return count;
}

bool
LocalMmuModel::peekNextRequest(Request &request) const
{
    for (size_t offset = 0; offset < ClientCount; ++offset) {
        const auto index = (firstRequestIndex + offset) % ClientCount;
        if (!pending[index].empty()) {
            request = pending[index].front();
            return true;
        }
    }
    return false;
}

bool
LocalMmuModel::takeNextRequest(Request &request)
{
    for (size_t offset = 0; offset < ClientCount; ++offset) {
        const auto index = (firstRequestIndex + offset) % ClientCount;
        if (!pending[index].empty()) {
            request = pending[index].front();
            pending[index].pop_front();
            return true;
        }
    }
    return false;
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
LocalMmuModel::issueRequest(uint64_t ready_cycle, IssuedRequest &issued_request,
                            const IssueAdmission *admission)
{
    if (pendingCount() == 0 || outstanding.size() >= config.maxOutstanding) {
        return false;
    }

    Request request;
    const bool can_peek = peekNextRequest(request);
    assert(can_peek);
    if (admission != nullptr && !(*admission)(request)) {
        return false;
    }

    uint32_t source_id = 0;
    if (!allocateSource(source_id)) {
        return false;
    }

    const bool has_request = takeNextRequest(request);
    assert(has_request);

    InFlight in_flight;
    in_flight.request = request;
    in_flight.sourceId = source_id;
    in_flight.readyCycle = ready_cycle;
    outstanding.push_back(in_flight);

    issued_request.request = request;
    issued_request.sourceId = source_id;
    issued_request.metadata = request.metadata;
    ++issued;
    return true;
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

void
LocalMmuModel::step(uint64_t cycle)
{
    currentCycle = cycle;

    IssuedRequest unused;
    issueRequest(currentCycle + config.latencyCycles, unused);

    while (!outstanding.empty() &&
           outstanding.front().readyCycle <= currentCycle) {
        const auto in_flight = outstanding.front();
        outstanding.pop_front();

        queueResponse(in_flight);
        freeSource(in_flight.sourceId);
    }

    firstRequestIndex = (firstRequestIndex + 1) % ClientCount;
}

bool
LocalMmuModel::issueExternal(uint64_t cycle, IssuedRequest &issued_request)
{
    currentCycle = cycle;
    const bool issued_request_valid =
        issueRequest(UINT64_MAX, issued_request);
    firstRequestIndex = (firstRequestIndex + 1) % ClientCount;
    return issued_request_valid;
}

bool
LocalMmuModel::issueExternal(uint64_t cycle, IssuedRequest &issued_request,
                             const IssueAdmission &admission)
{
    currentCycle = cycle;
    const bool issued_request_valid =
        issueRequest(UINT64_MAX, issued_request, &admission);
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
    auto it = std::find_if(
        outstanding.begin(), outstanding.end(),
        [source_id](const InFlight &in_flight) {
            return in_flight.sourceId == source_id;
        });
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
    auto it = std::find_if(
        outstanding.begin(), outstanding.end(),
        [source_id](const InFlight &in_flight) {
            return in_flight.sourceId == source_id;
        });
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
    assert(config.fillChunksPerBeat != 0);
}

MatrixL2FillTable::Slot *
MatrixL2FillTable::findSlot(uint32_t source_id)
{
    for (auto &slot : slots) {
        if (slot.valid && slot.entry.request.sourceId == source_id) {
            return &slot;
        }
    }
    return nullptr;
}

const MatrixL2FillTable::Slot *
MatrixL2FillTable::findSlot(uint32_t source_id) const
{
    for (const auto &slot : slots) {
        if (slot.valid && slot.entry.request.sourceId == source_id) {
            return &slot;
        }
    }
    return nullptr;
}

bool
MatrixL2FillTable::reserveForIssue(const Request &request)
{
    if (request.byteSize == 0 || request.byteSize > 64 ||
        findSlot(request.sourceId) != nullptr) {
        return false;
    }

    for (auto &slot : slots) {
        if (slot.valid) {
            continue;
        }
        slot.valid = true;
        slot.entry = Entry{};
        slot.entry.request = request;
        slot.entry.remainingFillChunks = config.fillChunksPerBeat;
        return true;
    }
    return false;
}

bool
MatrixL2FillTable::acceptResponse(uint32_t source_id, const uint8_t *data,
                                  uint32_t size)
{
    auto *slot = findSlot(source_id);
    if (slot == nullptr || slot->entry.hasData || data == nullptr ||
        size == 0) {
        return false;
    }

    const auto copy_size = std::min<size_t>(size, slot->entry.data.size());
    std::copy(data, data + copy_size, slot->entry.data.begin());
    slot->entry.hasData = true;
    slot->entry.dataSize = copy_size;
    slot->entry.remainingFillChunks = config.fillChunksPerBeat;
    return true;
}

bool
MatrixL2FillTable::retireFillChunk(uint32_t source_id)
{
    auto *slot = findSlot(source_id);
    if (slot == nullptr || !slot->entry.hasData ||
        slot->entry.remainingFillChunks == 0) {
        return false;
    }

    --slot->entry.remainingFillChunks;
    return true;
}

bool
MatrixL2FillTable::releaseSource(uint32_t source_id)
{
    auto *slot = findSlot(source_id);
    if (slot == nullptr || !sourceReadyToRelease(source_id)) {
        return false;
    }

    slot->valid = false;
    slot->entry = Entry{};
    return true;
}

std::optional<MatrixL2FillTable::Entry>
MatrixL2FillTable::lookup(uint32_t source_id) const
{
    const auto *slot = findSlot(source_id);
    if (slot == nullptr) {
        return std::nullopt;
    }
    return slot->entry;
}

bool
MatrixL2FillTable::hasFreeEntry() const
{
    return std::any_of(
        slots.begin(), slots.end(),
        [](const Slot &slot) { return !slot.valid; });
}

bool
MatrixL2FillTable::sourceHeld(uint32_t source_id) const
{
    return findSlot(source_id) != nullptr;
}

bool
MatrixL2FillTable::sourceReadyToRelease(uint32_t source_id) const
{
    const auto *slot = findSlot(source_id);
    return slot != nullptr && slot->entry.hasData &&
           slot->entry.remainingFillChunks == 0;
}

size_t
MatrixL2FillTable::reservedCount() const
{
    return std::count_if(
        slots.begin(), slots.end(),
        [](const Slot &slot) { return slot.valid; });
}

unsigned
MatrixL2FillTable::pendingFillChunks(uint32_t source_id) const
{
    const auto *slot = findSlot(source_id);
    return slot == nullptr ? 0 : slot->entry.remainingFillChunks;
}

} // namespace matrix
} // namespace gem5
