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

#ifndef __MATRIX_LOCAL_MMU_MODEL_HH__
#define __MATRIX_LOCAL_MMU_MODEL_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "matrix/CUTEParameters.hh"

namespace gem5
{

namespace matrix
{

class LocalMmuModel
{
  public:
    enum class Client : uint8_t
    {
        AML,
        BML,
        CML
    };

    struct Config
    {
        unsigned maxOutstanding = 64;
    };

    struct MatrixL2Metadata
    {
        MatrixBankKind destBank = MatrixBankKind::A;
    };

    struct Request
    {
        uint64_t seq = 0;
        Client client = Client::AML;
        bool isStore = false;
        uint32_t beatIndex = 0;
        uint32_t byteSize = 64;
        MatrixL2Metadata metadata = {};
    };

    struct Response
    {
        uint64_t seq = 0;
        Client client = Client::AML;
        bool isStore = false;
        uint32_t beatIndex = 0;
        uint32_t byteSize = 64;
        uint32_t sourceId = 0;
        MatrixL2Metadata metadata = {};
        bool hasData = false;
        uint32_t dataSize = 0;
        std::array<uint8_t, 64> data = {};
    };

    struct IssuedRequest
    {
        Request request = {};
        uint32_t sourceId = 0;
    };

    explicit LocalMmuModel(Config config);

    bool enqueue(const Request &request);
    bool issueExternal(IssuedRequest &issued_request);
    bool completeExternalResponse(uint32_t source_id);
    bool completeExternalResponse(
        uint32_t source_id, const uint8_t *data, uint32_t size);
    bool releaseExternalSource(uint32_t source_id);
    std::vector<Response> takeReadyResponses();

    bool outstandingFull() const
    {
        return outstanding.size() >= config.maxOutstanding;
    }
    size_t outstandingCount() const { return outstanding.size(); }

  private:
    struct InFlight
    {
        Request request = {};
        uint32_t sourceId = 0;
        bool responseComplete = false;
    };

    static constexpr size_t ClientCount = 3;

    static size_t clientIndex(Client client);
    std::optional<size_t> nextRequestIndex() const;
    bool issueRequest(IssuedRequest &issued_request);
    std::deque<InFlight>::iterator findOutstanding(uint32_t source_id);
    bool allocateSource(uint32_t &source_id);
    void freeSource(uint32_t source_id);
    void queueResponse(
        const InFlight &in_flight, const uint8_t *data = nullptr,
        uint32_t size = 0);

    Config config;
    std::array<std::deque<Request>, ClientCount> pending;
    std::deque<InFlight> outstanding;
    std::deque<Response> readyResponses;
    std::vector<bool> sourceBusy;
    size_t firstRequestIndex = 0;
};

class MatrixL2FillTable
{
  public:
    struct Config
    {
        size_t entryCount = 4;
        size_t bankFifoDepth = 2;
    };

    struct Handle
    {
        uint32_t slot = 0;
        uint32_t generation = 0;

        bool operator==(const Handle &other) const
        {
            return slot == other.slot && generation == other.generation;
        }
        bool operator!=(const Handle &other) const
        {
            return !(*this == other);
        }
    };

    struct Request
    {
        LocalMmuModel::Client client = LocalMmuModel::Client::AML;
        MatrixBankKind destBank = MatrixBankKind::A;
        unsigned fillChunks = 2;
        unsigned targetBank = 0;
    };

    struct Entry
    {
        Request request = {};
        unsigned remainingFillChunks = 0;
    };

    struct DrainCandidate
    {
        Handle handle = {};
        LocalMmuModel::Client client = LocalMmuModel::Client::AML;
        MatrixBankKind destBank = MatrixBankKind::A;
        unsigned targetBank = 0;
    };

    explicit MatrixL2FillTable(Config config);

    bool canAccept(const Request &request) const;
    bool canAcceptResponse(const Request &request) const;
    std::optional<Handle> acceptResponseToBank(const Request &request);
    std::optional<DrainCandidate> drainCandidate(unsigned bank) const;
    bool retireDrain(const DrainCandidate &candidate);
    bool hasPendingFillChunks(LocalMmuModel::Client client) const;

    bool hasFreeEntry() const;
    size_t reservedCount() const;
    size_t bankFifoOccupancy(unsigned bank) const;

  private:
    struct Slot
    {
        bool valid = false;
        uint32_t generation = 0;
        Entry entry = {};
    };

    Slot *findSlot(Handle handle);
    const Slot *findSlot(Handle handle) const;
    std::optional<Handle> acceptResponse(const Request &request);
    bool retireFillChunk(Handle handle);
    bool releaseEntry(Handle handle);
    bool entryReadyToRelease(Handle handle) const;

    Config config;
    std::vector<Slot> slots;
    std::array<std::deque<Handle>, 8> bankFifos;
    std::array<unsigned,
               static_cast<size_t>(LocalMmuModel::Client::CML) + 1>
        pendingFillChunks = {};
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_LOCAL_MMU_MODEL_HH__
