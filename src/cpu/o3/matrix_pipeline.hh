/*
 * Copyright (c) 2010, 2016, 2021 ARM Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

#ifndef __CPU_O3_MATRIX_PIPELINE_HH__
#define __CPU_O3_MATRIX_PIPELINE_HH__

#include <array>
#include <cstdint>
#include <list>
#include <optional>

#include "arch/riscv/matrix.hh"
#include "cpu/inst_seq.hh"
#include "matrix/MatrixTypes.hh"

namespace gem5
{

namespace o3
{

struct MatrixDynInstState
{
    using InstInfo = RiscvISA::MatrixStaticInfo;

    bool configFrozen = false;
    std::array<RegVal, 8> config = {};
    InstInfo matrixInst;
    std::optional<matrix::CuteRequest> request;
    std::optional<RegVal> stagedMatrixTokenReset;

    bool matrixNeedAmuCtrl() const
    {
        return matrixInst.valid &&
               matrixInst.needAmuCtrlCandidate;
    }
    void clearMatrixPayload()
    {
        request.reset();
        stagedMatrixTokenReset.reset();
    }
};

class MatrixAmuBuffer
{
  public:
    struct Entry
    {
        enum class State : uint8_t
        {
            Allocated, WrittenBack, NoRequest, Ready, Retired
        };
        State state = State::Allocated;
        InstSeqNum seqNum = 0;
        matrix::CuteRequest backendReq = {};
    };

    explicit MatrixAmuBuffer(unsigned capacity = 0);

    void reset(unsigned capacity);

    Entry *find(InstSeqNum seq_num);

    void allocate(InstSeqNum seq_num);
    void noteWriteback(InstSeqNum seq_num,
                       const matrix::CuteRequest *backend_req);
    void noteCommit(InstSeqNum seq_num);
    const Entry *peekReady();
    Entry popReady();
    void squash(InstSeqNum seq_num);
    unsigned numFreeEntries();

  private:
    void cleanupFront();

    unsigned capacity_;
    std::list<Entry> entries_;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MATRIX_PIPELINE_HH__
