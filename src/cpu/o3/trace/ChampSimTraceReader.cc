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

#include "cpu/o3/trace/ChampSimTraceReader.hh"

#include <iostream>
#include <algorithm>

#include "base/trace.hh"
#include "debug/TraceReader.hh"

namespace gem5
{
namespace o3
{

ChampSimTraceReader::ChampSimTraceReader(const std::string &trace_file, 
                                         const std::string &name)
    : TraceReader(trace_file, name), compressed(false), currentPos(0)
{
    compressed = isCompressed(trace_file);
    DPRINTF(TraceReader, "ChampSim trace reader created for file: %s (compressed: %s)\n",
            trace_file, compressed ? "yes" : "no");
}

ChampSimTraceReader::~ChampSimTraceReader()
{
    if (traceStream.is_open()) {
        traceStream.close();
    }
}

bool
ChampSimTraceReader::init()
{
    if (initialized) {
        DPRINTF(TraceReader, "ChampSim trace reader already initialized\n");
        return true;
    }
    
    if (!validateTraceFile()) {
        DPRINTF(TraceReader, "ChampSim trace file validation failed\n");
        return false;
    }
    
    // Open the trace file
    traceStream.open(traceFile, std::ios::binary);
    if (!traceStream.is_open()) {
        DPRINTF(TraceReader, "Failed to open ChampSim trace file: %s\n", traceFile);
        return false;
    }
    
    // Get file size for progress tracking
    traceStream.seekg(0, std::ios::end);
    auto fileSize = traceStream.tellg();
    traceStream.seekg(0, std::ios::beg);
    currentPos = traceStream.tellg();
    
    DPRINTF(TraceReader, "ChampSim trace file opened successfully, size: %lld bytes\n", 
            (long long)fileSize);
    
    initialized = true;
    eofReached = false;
    currentSeqNum = 0;
    
    return true;
}

bool
ChampSimTraceReader::reset()
{
    if (!traceStream.is_open()) {
        return false;
    }
    
    traceStream.clear();
    traceStream.seekg(0, std::ios::beg);
    currentPos = traceStream.tellg();
    eofReached = false;
    currentSeqNum = 0;
    
    // Clear instruction buffer
    while (!instrBuffer.empty()) {
        instrBuffer.pop();
    }
    
    DPRINTF(TraceReader, "ChampSim trace reader reset to beginning\n");
    return true;
}

bool
ChampSimTraceReader::validateTraceFile()
{
    std::ifstream testFile(traceFile, std::ios::binary);
    if (!testFile.is_open()) {
        return false;
    }
    
    // Check if file has reasonable size (at least one ChampSim instruction)
    testFile.seekg(0, std::ios::end);
    auto fileSize = testFile.tellg();
    testFile.close();
    
    if (fileSize < static_cast<std::streampos>(sizeof(ChampSimInstr))) {
        DPRINTF(TraceReader, "ChampSim trace file too small: %lld bytes\n", 
                (long long)fileSize);
        return false;
    }
    
    return true;
}

size_t
ChampSimTraceReader::fillBuffer(size_t max_instructions)
{
    if (eofReached || !traceStream.is_open()) {
        return 0;
    }
    
    size_t instructions_read = 0;
    
    while (instructions_read < max_instructions && !eofReached) {
        TraceInstruction trace_instr;
        if (parseInstruction(trace_instr)) {
            addToBuffer(trace_instr);
            instructions_read++;
        } else {
            eofReached = true;
            break;
        }
    }
    
    DPRINTF(TraceReader, "ChampSim fillBuffer: read %zu instructions\n", 
            instructions_read);
    
    return instructions_read;
}

bool
ChampSimTraceReader::parseInstruction(TraceInstruction &instr)
{
    ChampSimInstr cs_instr;
    
    if (!readChampSimInstruction(cs_instr)) {
        return false;
    }
    
    convertInstruction(cs_instr, instr);
    return true;
}

bool
ChampSimTraceReader::readChampSimInstruction(ChampSimInstr &cs_instr)
{
    if (!traceStream.is_open() || eofReached) {
        return false;
    }
    
    // Read the instruction structure from binary file
    traceStream.read(reinterpret_cast<char*>(&cs_instr), sizeof(ChampSimInstr));
    
    if (traceStream.gcount() != sizeof(ChampSimInstr)) {
        if (traceStream.eof()) {
            DPRINTF(TraceReader, "Reached end of ChampSim trace file\n");
        } else {
            DPRINTF(TraceReader, "Error reading ChampSim instruction: read %lld bytes, expected %zu\n",
                    (long long)traceStream.gcount(), sizeof(ChampSimInstr));
        }
        return false;
    }
    
    currentPos = traceStream.tellg();
    return true;
}

void
ChampSimTraceReader::convertInstruction(const ChampSimInstr &cs_instr, 
                                       TraceInstruction &trace_instr)
{
    // Reset the instruction
    trace_instr.reset();
    
    // Set basic fields
    trace_instr.setPC(cs_instr.ip);
    trace_instr.setSeqNum(getNextSeqNum());
    trace_instr.setValid(true);
    
    // Determine instruction type
    TraceInstruction::InstType inst_type = determineInstType(cs_instr);
    trace_instr.setInstType(inst_type);
    
    // Set branch information
    if (cs_instr.is_branch) {
        trace_instr.setBranchTaken(cs_instr.branch_taken != 0);
        // For now, we don't have branch target in ChampSim format
        // This would need to be computed or provided separately
    }
    
    // Extract memory operations
    extractMemoryOps(cs_instr, trace_instr);
    
    // Extract register dependencies
    extractRegisterDeps(cs_instr, trace_instr);
    
    DPRINTF(TraceReader, "Converted ChampSim instruction: PC=0x%llx, Type=%s, Branch=%s\n",
            trace_instr.getPC(), trace_instr.getInstTypeStr(),
            trace_instr.getBranch() ? (trace_instr.getBranchTaken() ? "taken" : "not_taken") : "no");
}

TraceInstruction::InstType
ChampSimTraceReader::determineInstType(const ChampSimInstr &cs_instr)
{
    // Check if it's a branch instruction first
    if (cs_instr.is_branch) {
        // For now, assume all branches in ChampSim traces are conditional
        // In a more sophisticated implementation, we could analyze the PC pattern
        // or use additional metadata to distinguish between branch types
        return TraceInstruction::InstType::COND_BRANCH;
    }
    
    // Check if it has memory operations
    bool hasLoad = false;
    bool hasStore = false;
    
    // Check for stores (destination memory addresses)
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_memory[i] != 0) {
            hasStore = true;
            break;
        }
    }
    
    // Check for loads (source memory addresses)
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_memory[i] != 0) {
            hasLoad = true;
            break;
        }
    }
    
    // Memory instruction type determination
    if (hasStore) {
        return TraceInstruction::InstType::STORE;
    } else if (hasLoad) {
        return TraceInstruction::InstType::LOAD;
    }
    
    // Check if it has floating-point characteristics
    // This is a heuristic - in a real implementation, you might have
    // additional metadata or analyze the instruction encoding
    
    // Default to ALU operation for all other instructions
    return TraceInstruction::InstType::ALU;
}

bool
ChampSimTraceReader::hasMemoryOps(const ChampSimInstr &cs_instr)
{
    // Check for any non-zero memory addresses
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_memory[i] != 0) {
            return true;
        }
    }
    
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_memory[i] != 0) {
            return true;
        }
    }
    
    return false;
}

void
ChampSimTraceReader::extractMemoryOps(const ChampSimInstr &cs_instr,
                                      TraceInstruction &trace_instr)
{
    // Extract store addresses (destination memory)
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_memory[i] != 0) {
            trace_instr.addStoreAddress(cs_instr.destination_memory[i]);
        }
    }
    
    // Extract load addresses (source memory)
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_memory[i] != 0) {
            trace_instr.addLoadAddress(cs_instr.source_memory[i]);
        }
    }
}

void
ChampSimTraceReader::extractRegisterDeps(const ChampSimInstr &cs_instr,
                                         TraceInstruction &trace_instr)
{
    // Extract source registers
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_registers[i] != 0) {
            trace_instr.addSrcReg(cs_instr.source_registers[i]);
        }
    }
    
    // Extract destination registers
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_registers[i] != 0) {
            trace_instr.addDstReg(cs_instr.destination_registers[i]);
        }
    }
}

bool
ChampSimTraceReader::isCompressed(const std::string &filename)
{
    // Simple check for .gz extension
    return filename.size() > 3 && 
           filename.substr(filename.size() - 3) == ".gz";
}

} // namespace o3
} // namespace gem5