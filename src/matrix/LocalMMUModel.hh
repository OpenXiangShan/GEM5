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
#include <cstdint>
#include <deque>
#include <vector>

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

    struct Request
    {
        uint64_t seq = 0;
        Client client = Client::AML;
        bool isStore = false;
        uint32_t beatIndex = 0;
        uint32_t byteSize = 64;
    };

    struct Response
    {
        uint64_t seq = 0;
        Client client = Client::AML;
        bool isStore = false;
        uint32_t beatIndex = 0;
        uint32_t byteSize = 64;
        uint32_t sourceId = 0;
    };

    LocalMmuModel();
    explicit LocalMmuModel(Config config);

    bool enqueue(const Request &request);
    void step(uint64_t cycle);
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
    };

    static constexpr size_t ClientCount = 3;

    static size_t clientIndex(Client client);
    bool takeNextRequest(Request &request);
    bool allocateSource(uint32_t &source_id);
    void freeSource(uint32_t source_id);

    Config config;
    std::array<std::deque<Request>, ClientCount> pending;
    std::deque<InFlight> outstanding;
    std::deque<Response> readyResponses;
    std::vector<bool> sourceBusy;
    uint64_t currentCycle = 0;
    uint64_t issued = 0;
    size_t firstRequestIndex = 0;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_LOCAL_MMU_MODEL_HH__
