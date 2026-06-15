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

#ifndef __MATRIX_SCOREBOARD_HH__
#define __MATRIX_SCOREBOARD_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "matrix/TaskController.hh"

namespace gem5
{

namespace matrix
{

// Issue-time dependence tracking and staged release bookkeeping.
class DetailedCuteScoreboard
{
  public:
    enum class FuKind : uint8_t
    {
        None = 0,
        AML = 1,
        BML = 2,
        CML = 3,
        Compute = 4,
        Count = 5
    };

    struct SrcState
    {
        bool valid = false;
        MatrixBankKind bank = MatrixBankKind::A;
        uint8_t reg = 0;
        bool ready = false;
        FuKind waitFu = FuKind::None;
        bool readPending = false;
    };

    struct FuState
    {
        bool busy = false;
        bool destValid = false;
        MatrixBankKind destBank = MatrixBankKind::A;
        uint8_t destReg = 0;
        std::array<SrcState, 3> srcs = {};
    };

    enum class BlockReason : uint8_t
    {
        None = 0,
        FuBusy,
        DestBusy,
        DestPendingReaders,
        SrcNotReady,
        SrcPendingReaders,
        Count
    };

    explicit DetailedCuteScoreboard(size_t ab_reg_count, size_t c_reg_count)
        : abRegs(ab_reg_count), cRegs(c_reg_count)
    {
    }

    bool canIssue(const DecodedFifoEntry &entry) const;
    BlockReason blockReason(const DecodedFifoEntry &entry) const;
    void onLoadIssue(const DecodedFifoEntry &entry);
    void onLoadFinish(const DecodedFifoEntry &entry);
    void onStoreIssue(const DecodedFifoEntry &entry);
    void onStoreReadFinish(const DecodedFifoEntry &entry);
    void onStoreWriteFinish(const DecodedFifoEntry &entry);
    void onComputeIssue(const DecodedFifoEntry &entry);
    void onComputeReadFinishA(const DecodedFifoEntry &entry);
    void onComputeReadFinishB(const DecodedFifoEntry &entry);
    void onComputeReadFinishC(const DecodedFifoEntry &entry);
    void onComputeWriteFinishC(const DecodedFifoEntry &entry);
    void onArithIssue(const DecodedFifoEntry &entry);
    void onArithFinish(const DecodedFifoEntry &entry);
    void onIssue(const DecodedFifoEntry &entry);
    bool fuBusyForTest(FuKind fu) const;
    bool regBusyForTest(uint8_t reg, MatrixBankKind bank) const;
    unsigned pendingReadersForTest(uint8_t reg, MatrixBankKind bank) const;

  private:
    struct RegState
    {
        bool busy = false;
        FuKind writer = FuKind::None;
        unsigned pendingReaders = 0;
    };

    std::vector<RegState> abRegs;
    std::vector<RegState> cRegs;
    std::array<FuState, static_cast<size_t>(FuKind::Count)> fuStates = {};

    static constexpr size_t SrcAIdx = 0;
    static constexpr size_t SrcBIdx = 1;
    static constexpr size_t SrcCIdx = 2;

    FuState &fuState(FuKind fu);
    const FuState &fuState(FuKind fu) const;
    bool isFuBusy(FuKind fu) const;
    RegState &regStatus(uint8_t reg, MatrixBankKind bank);
    const RegState &regStatus(uint8_t reg, MatrixBankKind bank) const;
    MatrixBankKind bankForSource(const DecodedFifoEntry &entry, size_t src_idx) const;
    MatrixBankKind destBank(const DecodedFifoEntry &entry) const;
    FuKind loadFu(const DecodedFifoEntry &entry) const;
    bool sourceReady(const DecodedFifoEntry &entry, size_t src_idx) const;
    bool destBusy(const DecodedFifoEntry &entry) const;
    bool sourceHasPendingReaders(const DecodedFifoEntry &entry, size_t src_idx) const;
    bool destHasPendingReaders(const DecodedFifoEntry &entry) const;
    void resetSrc(SrcState &src);
    void resetFu(FuState &fu);
    void setupSrc(SrcState &src, MatrixBankKind bank, uint8_t reg);
    void reserveDest(const DecodedFifoEntry &entry, FuKind writer);
    void releaseDest(const DecodedFifoEntry &entry, FuKind writer);
    void incrementPendingReader(uint8_t reg, MatrixBankKind bank);
    void decrementPendingReader(uint8_t reg, MatrixBankKind bank);
    void wakeupConsumers(uint8_t reg, MatrixBankKind bank, FuKind producer);
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_SCOREBOARD_HH__
