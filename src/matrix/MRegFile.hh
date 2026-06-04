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

#ifndef __MATRIX_MREG_FILE_HH__
#define __MATRIX_MREG_FILE_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "matrix/CUTEParameters.hh"

namespace gem5
{

namespace matrix
{

class MatrixRegResource
{
  public:
    static constexpr unsigned NumBanks = 8;
    static constexpr unsigned EntryBytes = 32;
    static constexpr unsigned ReadLatencyCycles = 1;
    static constexpr uint16_t FullBankMask = (1U << NumBanks) - 1;

    enum class Client : uint8_t
    {
        DataController,
        MemoryLoader
    };

    enum class Access : uint8_t
    {
        Read,
        Write
    };

    enum class StallReason : uint8_t
    {
        None,
        PartialBankMask,
        AbWritePriority,
        BankConflict,
        CReadWriteConflict
    };

    struct Request
    {
        MatrixBankKind bank = MatrixBankKind::A;
        Client client = Client::DataController;
        Access access = Access::Read;
        uint32_t entry = 0;
        uint16_t bankMask = FullBankMask;
    };

    struct Grant
    {
        bool granted = false;
        StallReason reason = StallReason::None;
    };

    static Request makeRead(MatrixBankKind bank, Client client,
                            uint32_t entry);
    static Request makeWrite(MatrixBankKind bank, Client client,
                             uint32_t entry);

    std::vector<Grant> arbitrate(const std::vector<Request> &requests);
    void advanceCycle();

    bool readResponseReady(MatrixBankKind bank, Client client) const;
    bool consumeReadResponse(MatrixBankKind bank, Client client);

    uint64_t currentCycle() const { return cycle; }

  private:
    struct ReadResponse
    {
        MatrixBankKind bank = MatrixBankKind::A;
        Client client = Client::DataController;
        uint64_t readyCycle = 0;
    };

    static bool isAbBank(MatrixBankKind bank);
    static size_t abBankIndex(MatrixBankKind bank);
    bool currentCycleGrantBlocks(const Request &request,
                                 Grant &grant) const;
    void markCycleGrant(const Request &request);
    void enqueueReadResponse(const Request &request);

    struct AbBankToken
    {
        bool busy = false;
        Access access = Access::Read;
    };

    struct CBankToken
    {
        bool readBusy = false;
        uint32_t readParity = 0;
        bool writeBusy = false;
        uint32_t writeParity = 0;
    };

    uint64_t cycle = 0;
    std::array<AbBankToken, 2> abBankTokens;
    CBankToken cBankToken;
    std::deque<ReadResponse> readResponses;
};

class MatrixRegFile
{
  public:
    struct RegMetadata
    {
        bool allocated = false;
    };

    struct Register
    {
        RegMetadata meta = {};
        MatrixTensor tensor = {};
    };

    static constexpr size_t DefaultAbRegCount = 8;
    static constexpr size_t DefaultCRegCount = 8;

    explicit MatrixRegFile(size_t ab_reg_count = DefaultAbRegCount,
                           size_t c_reg_count = DefaultCRegCount);

    size_t abRegCount() const { return _abRegCount; }
    size_t cRegCount() const { return _cRegCount; }
    size_t regCount(MatrixBankKind bank) const;

    bool hasRegister(MatrixBankKind bank, size_t reg_idx) const;
    const MatrixTensor &read(MatrixBankKind bank, size_t reg_idx) const;
    void write(MatrixBankKind bank, size_t reg_idx, const MatrixTensor &tensor);
    void zero(MatrixBankKind bank, size_t reg_idx, uint32_t rows,
              uint32_t cols, MatrixElemType elem_type);

    bool allocated(MatrixBankKind bank, size_t reg_idx) const;
    bool hasAllocatedState() const;

  private:
    std::vector<Register> &bank(MatrixBankKind bank);
    const std::vector<Register> &bank(MatrixBankKind bank) const;

    size_t _abRegCount;
    size_t _cRegCount;
    std::vector<Register> aRegs;
    std::vector<Register> bRegs;
    std::vector<Register> cRegs;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_MREG_FILE_HH__
