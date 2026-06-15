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

#ifndef __MATRIX_MATRIX_TE_HH__
#define __MATRIX_MATRIX_TE_HH__

namespace gem5
{

namespace matrix
{

struct MteTiming
{
    unsigned tensorMn = 0;
    unsigned tensorK = 0;
    unsigned matrixMn = 0;
    unsigned reduceWidthBytes = 0;
    unsigned resultWidthBytes = 0;
    unsigned aBytesPerBeat = 0;
    unsigned bBytesPerBeat = 0;
    unsigned cBytesPerBeat = 0;
    unsigned dBytesPerBeat = 0;
    unsigned acceptedInputBeats = 0;
    unsigned adcReadCycles = 0;
    unsigned bdcReadCycles = 0;
    unsigned cdcReadCycles = 0;
    unsigned mteAcceptedInputBeats = 0;
    unsigned fReduceTailCycles = 0;
    unsigned cdcWriteCycles = 0;
    unsigned terminalHandshakeCycles = 0;
    unsigned totalCompletionCycles = 0;
    bool supported = false;
};

} // namespace matrix
} // namespace gem5

#endif // __MATRIX_MATRIX_TE_HH__
