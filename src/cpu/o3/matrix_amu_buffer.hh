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

#ifndef __CPU_O3_MATRIX_AMU_BUFFER_HH__
#define __CPU_O3_MATRIX_AMU_BUFFER_HH__

#include <list>

#include "cpu/inst_seq.hh"
#include "matrix/CUTEParameters.hh"

namespace gem5
{

namespace o3
{

class MatrixAmuBuffer
{
  public:
    struct Entry
    {
        bool needAMU = false;
        bool writebacked = false;
        bool committed = false;
        bool canDeq = false;
        InstSeqNum seqNum = 0;
        matrix::CuteRequest backendReq = {};

        bool
        amuReqValid() const
        {
            return needAMU && writebacked && committed && !canDeq;
        }
    };

    explicit MatrixAmuBuffer(unsigned capacity = 0, unsigned fire_width = 1);

    void reset(unsigned capacity, unsigned fire_width);

    Entry *find(InstSeqNum seq_num);
    const Entry *find(InstSeqNum seq_num) const;

    void allocate(InstSeqNum seq_num);
    void noteWriteback(InstSeqNum seq_num, bool faulted,
                       bool req_valid,
                       const matrix::CuteRequest &backend_req);
    void noteCommit(InstSeqNum seq_num);
    bool peekReady(Entry &entry_out);
    bool popReady(Entry &entry_out);
    void squash(InstSeqNum seq_num);
    unsigned numFreeEntries();

  private:
    void cleanupFront();
    std::list<Entry>::iterator findReadyToFire();

    unsigned capacity_;
    unsigned fireWidth_;
    std::list<Entry> entries_;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_MATRIX_AMU_BUFFER_HH__
