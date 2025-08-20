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

#include "cpu/o3/trace/TraceReader.hh"
#include "cpu/o3/trace/ChampSimTraceReader.hh"
#include "cpu/o3/trace/CBP2025TraceReader.hh"

#include "base/trace.hh"
#include "debug/TraceReader.hh"

namespace gem5
{
namespace o3
{

TraceReader::TraceReaderStats::TraceReaderStats(statistics::Group *parent, 
                                                 const std::string &name)
    : statistics::Group(parent, name.c_str()),
      ADD_STAT(instrRead, statistics::units::Count::get(),
               "Number of instructions read from trace"),
      ADD_STAT(branchInstr, statistics::units::Count::get(),
               "Number of branch instructions encountered"),
      ADD_STAT(loadInstr, statistics::units::Count::get(),
               "Number of load instructions encountered"),
      ADD_STAT(storeInstr, statistics::units::Count::get(),
               "Number of store instructions encountered"),
      ADD_STAT(bufferUnderruns, statistics::units::Count::get(),
               "Number of times buffer was empty when instruction requested")
{
}

TraceReader::TraceReader(const std::string &trace_file, const std::string &name)
    : traceFile(trace_file), eofReached(false), initialized(false),
      currentSeqNum(0), stats(nullptr, name)
{
    DPRINTF(TraceReader, "Creating TraceReader for file: %s\n", trace_file);
}

TraceInstruction
TraceReader::getNextInstruction()
{
    // Fill buffer if it's getting low
    if (instrBuffer.size() < MAX_BUFFER_SIZE / 4 && !eofReached) {
        fillBuffer(MAX_BUFFER_SIZE / 2);
    }
    
    // Check if we have any instructions available
    if (instrBuffer.empty()) {
        if (!eofReached) {
            // Try to fill buffer one more time
            fillBuffer(1);
        }
        
        if (instrBuffer.empty()) {
            stats.bufferUnderruns++;
            DPRINTF(TraceReader, "Buffer underrun - no instructions available\n");
            // Return invalid instruction
            TraceInstruction invalid_instr;
            invalid_instr.setValid(false);
            return invalid_instr;
        }
    }
    
    // Get instruction from buffer
    TraceInstruction instr = instrBuffer.front();
    instrBuffer.pop();
    
    // Update statistics
    updateStats(instr);
    
    DPRINTF(TraceReader, "Returning instruction PC: 0x%llx, Type: %s, SeqNum: %llu\n",
            instr.getPC(), instr.getInstTypeStr(), instr.getSeqNum());
    
    return instr;
}

void
TraceReader::addToBuffer(const TraceInstruction &instr)
{
    if (instrBuffer.size() >= MAX_BUFFER_SIZE) {
        DPRINTF(TraceReader, "Warning: Instruction buffer full, dropping instruction\n");
        return;
    }
    
    instrBuffer.push(instr);
    DPRINTF(TraceReader, "Added instruction to buffer, size now: %zu\n", 
            instrBuffer.size());
}

void
TraceReader::updateStats(const TraceInstruction &instr)
{
    stats.instrRead++;
    
    if (instr.isAnyBranch()) {
        stats.branchInstr++;
    }
    
    if (instr.getLoad()) {
        stats.loadInstr++;
    }
    
    if (instr.getStore()) {
        stats.storeInstr++;
    }
}

std::unique_ptr<TraceReader>
createTraceReader(const std::string &format, const std::string &trace_file,
                  const std::string &name)
{
    DPRINTF(TraceReader, "Creating trace reader for format: %s, file: %s\n",
            format, trace_file);
    
    if (format == "champsim") {
        return std::make_unique<ChampSimTraceReader>(trace_file, name);
    } else if (format == "cbp2025") {
        return std::make_unique<CBP2025TraceReader>(trace_file, name);
    } else {
        DPRINTF(TraceReader, "Unsupported trace format: %s\n", format);
        return nullptr;
    }
}

} // namespace o3
} // namespace gem5