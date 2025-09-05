/*
 * Copyright (c) 2024 The Regents of The University of Michigan
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

#ifndef __CPU_O3_TRACE_CBP2025_TRACE_READER_HH__
#define __CPU_O3_TRACE_CBP2025_TRACE_READER_HH__

#include "cpu/o3/trace/TraceReader.hh"

namespace gem5
{
namespace o3
{

/**
 * CBP2025 trace reader implementation (stub).
 * 
 * This is a placeholder implementation for CBP2025 traces.
 * To be fully implemented in future iterations.
 */
class CBP2025TraceReader : public TraceReader
{
  public:
    CBP2025TraceReader(const std::string &trace_file, const std::string &name)
        : TraceReader(trace_file, name) {}
    
    bool init() override { return false; }
    bool reset() override { return false; }
    std::string getFormat() const override { return "cbp2025"; }
    TraceCheckpoint createCheckpoint() override { return {}; }
    bool restoreCheckpoint(const TraceCheckpoint& checkpoint) override { return false; }
    bool seekToInstruction(uint64_t instrIndex) override { return false; }
    uint64_t getCurrentInstructionIndex() const override { return 0; }

  protected:
    size_t fillBuffer(size_t max_instructions) override { return 0; }
    bool parseInstruction(TraceInstruction &instr) override { return false; }
    bool validateTraceFile() override { return false; }
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_CBP2025_TRACE_READER_HH__