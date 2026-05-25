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

void
LocalMmuModel::freeSource(uint32_t source_id)
{
    assert(source_id < sourceBusy.size());
    assert(sourceBusy[source_id]);
    sourceBusy[source_id] = false;
}

void
LocalMmuModel::step(uint64_t cycle)
{
    currentCycle = cycle;

    if (pendingCount() != 0 && outstanding.size() < config.maxOutstanding) {
        uint32_t source_id = 0;
        if (allocateSource(source_id)) {
            Request request;
            const bool has_request = takeNextRequest(request);
            assert(has_request);
            InFlight in_flight;
            in_flight.request = request;
            in_flight.sourceId = source_id;
            in_flight.readyCycle = currentCycle + config.latencyCycles;
            outstanding.push_back(in_flight);
            ++issued;
        }
    }

    while (!outstanding.empty() &&
           outstanding.front().readyCycle <= currentCycle) {
        const auto in_flight = outstanding.front();
        outstanding.pop_front();

        Response response;
        response.seq = in_flight.request.seq;
        response.client = in_flight.request.client;
        response.isStore = in_flight.request.isStore;
        response.beatIndex = in_flight.request.beatIndex;
        response.byteSize = in_flight.request.byteSize;
        response.sourceId = in_flight.sourceId;
        readyResponses.push_back(response);
        freeSource(in_flight.sourceId);
    }

    firstRequestIndex = (firstRequestIndex + 1) % ClientCount;
}

std::vector<LocalMmuModel::Response>
LocalMmuModel::takeReadyResponses()
{
    std::vector<Response> responses(
        readyResponses.begin(), readyResponses.end());
    readyResponses.clear();
    return responses;
}

} // namespace matrix
} // namespace gem5
