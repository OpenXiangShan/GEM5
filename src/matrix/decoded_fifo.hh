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

#ifndef __MATRIX_DECODED_FIFO_HH__
#define __MATRIX_DECODED_FIFO_HH__

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

#include "matrix/CUTEParameters.hh"

namespace gem5
{

namespace matrix
{

// Decoded FIFO normalizes request fields for issue gating and tracing.
struct DecodedFifoEntry
{
    CuteRequest request = {};

    std::array<uint8_t, 3> readRegs = {0, 0, 0};
    std::array<bool, 3> readValid = {false, false, false};
    std::array<uint8_t, 1> writeRegs = {0};
    std::array<bool, 1> writeValid = {false};

    bool isLoad = false;
    bool isStore = false;
    bool isMma = false;
    bool isZeroAcc = false;
    bool isZeroTr = false;
    bool isRelease = false;
};

inline DecodedFifoEntry
decodeCuteRequest(const CuteRequest &req)
{
    DecodedFifoEntry entry;
    entry.request = req;

    switch (req.kind) {
      case CuteRequestKind::Lsu:
        entry.isLoad = !req.lsu.isStore;
        entry.isStore = req.lsu.isStore;
        if (entry.isLoad) {
            entry.writeRegs[0] = req.lsu.ms;
            entry.writeValid[0] = true;
        } else {
            entry.readRegs[0] = req.lsu.ms;
            entry.readValid[0] = true;
        }
        break;
      case CuteRequestKind::Mma:
        entry.isMma = true;
        entry.readRegs = {req.mma.ms1, req.mma.ms2, req.mma.md};
        entry.readValid = {true, true, true};
        entry.writeRegs[0] = req.mma.md;
        entry.writeValid[0] = true;
        break;
      case CuteRequestKind::Arith:
        entry.isZeroAcc = req.arith.bank == MatrixBankKind::C;
        entry.isZeroTr = !entry.isZeroAcc;
        entry.readRegs[0] = req.arith.reg;
        entry.readValid[0] = true;
        entry.writeRegs[0] = req.arith.reg;
        entry.writeValid[0] = true;
        break;
      case CuteRequestKind::Release:
        entry.isRelease = true;
        break;
    }

    return entry;
}

class DecodedFifo
{
  public:
    explicit DecodedFifo(size_t depth = 8) : _depth(depth) {}

    bool canAccept() const { return _depth == 0 || entries.size() < _depth; }
    void enqueue(const DecodedFifoEntry &entry) { entries.push_back(entry); }
    bool empty() const { return entries.empty(); }
    const DecodedFifoEntry &head() const { return entries.front(); }
    void dequeue() { entries.pop_front(); }
    size_t size() const { return entries.size(); }
    size_t depth() const { return _depth; }

  private:
    size_t _depth;
    std::deque<DecodedFifoEntry> entries;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_DECODED_FIFO_HH__
