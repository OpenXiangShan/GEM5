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
#include <functional>
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
        unsigned latencyCycles = 1;
        unsigned maxOutstanding = 64;
    };

    struct MatrixL2Metadata
    {
        bool valid = false;
        uint64_t seq = 0;
        Client client = Client::AML;
        bool isStore = false;
        bool isRMW = false;
        uint32_t ameIndex = 0;
        uint32_t beatIndex = 0;
        MatrixBankKind destBank = MatrixBankKind::A;
        uint32_t destReg = 0;
        uint32_t byteSize = 64;
        uint64_t byteMask = ~uint64_t(0);
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
        MatrixL2Metadata metadata = {};
    };

    using IssueAdmission = std::function<bool(const Request &request)>;

    LocalMmuModel();
    explicit LocalMmuModel(Config config);

    bool enqueue(const Request &request);
    void step(uint64_t cycle);
    bool issueExternal(uint64_t cycle, IssuedRequest &issued_request);
    bool issueExternal(uint64_t cycle, IssuedRequest &issued_request,
                       const IssueAdmission &admission);
    bool completeExternalResponse(uint32_t source_id);
    bool completeExternalResponse(
        uint32_t source_id, const uint8_t *data, uint32_t size);
    bool releaseExternalSource(uint32_t source_id);
    std::vector<Response> takeReadyResponses();

    size_t pendingCount() const;
    size_t outstandingCount() const { return outstanding.size(); }
    size_t readyCount() const { return readyResponses.size(); }
    uint64_t issuedCount() const { return issued; }

  private:
    struct InFlight
    {
        Request request = {};
        uint32_t sourceId = 0;
        uint64_t readyCycle = 0;
        bool responseComplete = false;
    };

    static constexpr size_t ClientCount = 3;

    static size_t clientIndex(Client client);
    static uint64_t byteMaskForSize(uint32_t byte_size);
    static MatrixL2Metadata normalizedMetadata(const Request &request);
    bool peekNextRequest(Request &request) const;
    bool takeNextRequest(Request &request);
    bool issueRequest(uint64_t ready_cycle, IssuedRequest &issued_request,
                      const IssueAdmission *admission = nullptr);
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
    uint64_t currentCycle = 0;
    uint64_t issued = 0;
    size_t firstRequestIndex = 0;
};

class MatrixL2FillTable
{
  public:
    struct Config
    {
        size_t entryCount = 4;
        unsigned fillChunksPerBeat = 2;
    };

    struct Request
    {
        uint32_t sourceId = 0;
        uint64_t seq = 0;
        LocalMmuModel::Client client = LocalMmuModel::Client::AML;
        uint32_t beatIndex = 0;
        MatrixBankKind destBank = MatrixBankKind::A;
        uint32_t destReg = 0;
        uint32_t byteSize = 64;
    };

    struct Entry
    {
        Request request = {};
        bool hasData = false;
        uint32_t dataSize = 0;
        std::array<uint8_t, 64> data = {};
        unsigned remainingFillChunks = 0;
    };

    explicit MatrixL2FillTable(Config config);

    bool reserveForIssue(const Request &request);
    bool acceptResponse(uint32_t source_id, const uint8_t *data,
                        uint32_t size);
    bool retireFillChunk(uint32_t source_id);
    bool releaseSource(uint32_t source_id);

    std::optional<Entry> lookup(uint32_t source_id) const;
    bool hasFreeEntry() const;
    bool sourceHeld(uint32_t source_id) const;
    bool sourceReadyToRelease(uint32_t source_id) const;
    size_t reservedCount() const;
    unsigned pendingFillChunks(uint32_t source_id) const;

  private:
    struct Slot
    {
        bool valid = false;
        Entry entry = {};
    };

    Slot *findSlot(uint32_t source_id);
    const Slot *findSlot(uint32_t source_id) const;

    Config config;
    std::vector<Slot> slots;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_LOCAL_MMU_MODEL_HH__
