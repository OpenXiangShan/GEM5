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

#ifndef __MATRIX_TIMING_MEMORY_ADAPTER_HH__
#define __MATRIX_TIMING_MEMORY_ADAPTER_HH__

#include <array>
#include <cstdint>

#include "base/types.hh"

namespace gem5
{

namespace matrix
{

enum class MatrixL2Key : uint8_t
{
    None = 0,
    Matrix = 0x1,
    MatrixModify = 0x3
};

inline uint8_t
matrixL2KeyBits(MatrixL2Key key)
{
    return static_cast<uint8_t>(key) & 0x3;
}

class MatrixTimingMemoryAdapter
{
  public:
    struct Request
    {
        bool isStore = false;
        Addr paddr = 0;
        uint32_t packetSize = 64;
        uint32_t sourceId = 0;
        ContextID contextId = InvalidContextID;
        std::array<uint8_t, 64> data = {};
        uint64_t byteMask = 0;
        uint8_t matrixKey = matrixL2KeyBits(MatrixL2Key::None);
    };

    virtual ~MatrixTimingMemoryAdapter() = default;

    virtual bool connected() const = 0;
    virtual bool sendTimingRequest(const Request &request) = 0;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_TIMING_MEMORY_ADAPTER_HH__
