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
#include "config/the_isa.hh"
#include "debug/TraceReader.hh"

namespace gem5
{
namespace o3
{

ChampSimTraceReader::ChampSimTraceReader(const std::string &trace_file,
                                         const std::string &name,
                                         const std::string &map_mode,
                                         uint64_t base_addr,
                                         uint64_t map_size,
                                         bool page_align)
    : TraceReader(trace_file, name), compressed(false), currentPos(0),
      instructionIndex(0), addrMapMode(map_mode), addrMapBase(base_addr),
      addrMapSize(map_size), addrPageAlign(page_align)
{
    compressed = isCompressed(trace_file);
    hasPendingInstr = false;

    // Address mapping configuration initialized
    // mode=%s, base=0x%lx, size=0x%lx, page_align=%d
    // (DPRINTF temporarily removed for compilation)
}

ChampSimTraceReader::~ChampSimTraceReader()
{
    if (compressed && gzTraceStream.is_open()) {
        gzTraceStream.close();
    } else if (!compressed && traceStream.is_open()) {
        traceStream.close();
    }
}

bool
ChampSimTraceReader::init()
{
    DPRINTF(TraceReader, "init: Initializing ChampSim trace reader, initialized=%d\n", initialized);

    if (initialized) {
        DPRINTF(TraceReader, "init: ChampSim trace reader already initialized\n");
        return true;
    }

    if (!validateTraceFile()) {
        DPRINTF(TraceReader, "init: ChampSim trace file validation failed\n");
        return false;
    }

    // Open the trace file based on compression
    if (compressed) {
        DPRINTF(TraceReader, "init: Opening compressed trace file: %s\n", traceFile.c_str());
        gzTraceStream.open(traceFile.c_str(), std::ios::binary);
        if (!gzTraceStream.is_open()) {
            DPRINTF(TraceReader, "init: Failed to open compressed ChampSim trace file\n");
            return false;
        }
        // For gzipped files, seeking to end for size is not reliable
        currentPos = 0;
        DPRINTF(TraceReader, "init: Compressed trace file opened successfully\n");
    } else {
        DPRINTF(TraceReader, "init: Opening regular trace file: %s\n", traceFile.c_str());
        traceStream.open(traceFile, std::ios::binary);
        if (!traceStream.is_open()) {
            DPRINTF(TraceReader, "init: Failed to open ChampSim trace file\n");
            return false;
        }
        // Get file size for progress tracking
        traceStream.seekg(0, std::ios::end);
        auto fileSize = traceStream.tellg();
        traceStream.seekg(0, std::ios::beg);
        currentPos = traceStream.tellg();
        DPRINTF(TraceReader, "init: Regular trace file opened, size=%ld bytes\n", fileSize);
    }

    DPRINTF(TraceReader, "init: ChampSim trace file opened successfully\n");

    initialized = true;
    eofReached = false;
    currentSeqNum = 0;

    return true;
}

bool
ChampSimTraceReader::reset()
{
    DPRINTF(TraceReader, "reset: Resetting trace reader (compressed=%d)\n", compressed);

    if (compressed) {
        // For gzipped files, we need to close and reopen since seeking doesn't work reliably
        if (gzTraceStream.is_open()) {
            gzTraceStream.close();
            DPRINTF(TraceReader, "reset: Closed compressed stream\n");
        }

        // Reopen the compressed file
        gzTraceStream.open(traceFile.c_str(), std::ios::binary);
        if (!gzTraceStream.is_open()) {
            DPRINTF(TraceReader, "reset: Failed to reopen compressed stream\n");
            return false;
        }
        currentPos = 0;
        DPRINTF(TraceReader, "reset: Reopened compressed stream successfully\n");
    } else {
        if (!traceStream.is_open()) {
            DPRINTF(TraceReader, "reset: Regular stream not open\n");
            return false;
        }
        traceStream.clear();
        traceStream.seekg(0, std::ios::beg);
        currentPos = traceStream.tellg();
        DPRINTF(TraceReader, "reset: Reset regular stream to beginning, pos=%ld\n", currentPos);
    }

    eofReached = false;
    currentSeqNum = 0;
    instructionIndex = 0;
    hasPendingInstr = false;

    // Clear instruction buffer and checkpoints
    while (!instrBuffer.empty()) {
        instrBuffer.pop();
    }
    checkpoints.clear();

    DPRINTF(TraceReader, "reset: Reset completed, eofReached=%d, buffer size=%lu\n",
            eofReached, instrBuffer.size());
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
        // Debug: ChampSim trace file too small
        return false;
    }

    return true;
}

size_t
ChampSimTraceReader::fillBuffer(size_t max_instructions)
{
    if (eofReached || (compressed && !gzTraceStream.is_open()) ||
        (!compressed && !traceStream.is_open())) {
        DPRINTF(TraceReader, "fillBuffer: Cannot read - eofReached=%d, compressed=%d, stream_open=%d\n",
                eofReached, compressed,
                compressed ? gzTraceStream.is_open() : traceStream.is_open());
        return 0;
    }

    DPRINTF(TraceReader, "fillBuffer: Starting to read (target push %lu) (compressed=%d)\n",
            max_instructions, compressed);

    size_t pushed = 0;

    while (pushed < max_instructions && !eofReached) {
        TraceInstruction current;
        if (parseInstruction(current)) {
            if (hasPendingInstr) {
                // If the pending instruction is a taken branch, set its target as nextPC
                if (pendingInstr.isAnyBranch() && pendingInstr.getBranchTaken()) {
                    pendingInstr.setBranchTarget(current.getPC());
                    DPRINTF(TraceReader, "fillBuffer: Set branch target from nextPC: 0x%lx\n",
                            current.getPC());
                }
                addToBuffer(pendingInstr);
                pushed++;
            }
            // Stage current as next pending
            pendingInstr = current;
            hasPendingInstr = true;
        } else {
            DPRINTF(TraceReader, "fillBuffer: parseInstruction failed, setting EOF\n");
            eofReached = true;
            break;
        }
    }

    // If we've reached EOF, flush the final pending instruction (no target derivable)
    if (eofReached && hasPendingInstr && pushed < max_instructions) {
        addToBuffer(pendingInstr);
        pushed++;
        hasPendingInstr = false;
        DPRINTF(TraceReader, "fillBuffer: Flushed final pending instruction at EOF\n");
    }

    DPRINTF(TraceReader, "fillBuffer: Completed, pushed %lu instructions\n", pushed);

    return pushed;
}

bool
ChampSimTraceReader::parseInstruction(TraceInstruction &instr)
{
    ChampSimInstr cs_instr;

    if (!readChampSimInstruction(cs_instr)) {
        DPRINTF(TraceReader, "parseInstruction: readChampSimInstruction failed\n");
        return false;
    }

    DPRINTF(TraceReader, "parseInstruction: Read ChampSim instruction, converting...\n");
    convertInstruction(cs_instr, instr);
    instructionIndex++;
    DPRINTF(TraceReader, "parseInstruction: Conversion completed, instructionIndex=%lu\n", instructionIndex);
    return true;
}

bool
ChampSimTraceReader::readChampSimInstruction(ChampSimInstr &cs_instr)
{
    if (eofReached || (compressed && !gzTraceStream.is_open()) ||
        (!compressed && !traceStream.is_open())) {
        DPRINTF(TraceReader, "readChampSimInstruction: Cannot read - eofReached=%d, compressed=%d, stream_open=%d\n",
                eofReached, compressed,
                compressed ? gzTraceStream.is_open() : traceStream.is_open());
        return false;
    }

    // Read the instruction structure from binary file
    std::streamsize bytes_read = 0;
    if (compressed) {
        DPRINTF(TraceReader, "readChampSimInstruction: Reading %lu bytes from compressed stream\n",
                sizeof(ChampSimInstr));
        gzTraceStream.read(reinterpret_cast<char*>(&cs_instr), sizeof(ChampSimInstr));
        bytes_read = gzTraceStream.gcount();

        DPRINTF(TraceReader, "readChampSimInstruction: Read %ld bytes (expected %lu)\n",
                bytes_read, sizeof(ChampSimInstr));

        if (bytes_read != sizeof(ChampSimInstr)) {
            if (gzTraceStream.eof()) {
                eofReached = true;
                // Debug output removed temporarily
            } else {
                // Debug: Error reading compressed ChampSim instruction
            }
            return false;
        }
    } else {
        traceStream.read(reinterpret_cast<char*>(&cs_instr), sizeof(ChampSimInstr));
        bytes_read = traceStream.gcount();

        if (bytes_read != sizeof(ChampSimInstr)) {
            if (traceStream.eof()) {
                eofReached = true;
                // Debug output removed temporarily
            } else {
                // Debug: Error reading ChampSim instruction
            }
            return false;
        }

        // Update position tracking for uncompressed files
        currentPos = traceStream.tellg();
    }

    return true;
}

void
ChampSimTraceReader::convertInstruction(const ChampSimInstr &cs_instr,
                                       TraceInstruction &trace_instr)
{
    // Reset the instruction
    trace_instr.reset();

    // Set basic fields
    // CRITICAL FIX: Apply address mapping to PC to avoid page table faults
    uint64_t mapped_pc = mapTraceAddressToVirtual(cs_instr.ip);
    DPRINTF(TraceReader, "convertInstruction: Mapping PC 0x%lx -> 0x%lx\n", cs_instr.ip, mapped_pc);
    trace_instr.setPC(mapped_pc);
    trace_instr.setSeqNum(getNextSeqNum());
    trace_instr.setValid(true);

    // Determine instruction type
    TraceInstruction::InstType inst_type = determineInstType(cs_instr);
    trace_instr.setInstType(inst_type);

    // Set branch information
    if (cs_instr.is_branch) {
        trace_instr.setBranchTaken(cs_instr.branch_taken != 0);
        // Do not estimate target; real target will be derived via look-ahead
        // when the next instruction's PC becomes available in fillBuffer.
    }

    // Extract memory operations
    extractMemoryOps(cs_instr, trace_instr);

    // Extract register dependencies
    extractRegisterDeps(cs_instr, trace_instr);

    // Generate simulated memory values for cache hierarchy integration
    // Since ChampSim traces don't contain values, we simulate them
    generateSimulatedMemoryValues(cs_instr, trace_instr);

    // Debug: Converted ChampSim instruction
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
    // Extract store addresses (destination memory) with address mapping for trace mode
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_memory[i] != 0) {
            // Map trace addresses to a safe memory region (e.g., starting at 0x10000000)
            uint64_t mapped_addr = mapTraceAddressToVirtual(cs_instr.destination_memory[i]);
            DPRINTF(TraceReader, "extractMemoryOps: Store addr 0x%lx -> 0x%lx for PC 0x%lx\n",
                    cs_instr.destination_memory[i], mapped_addr, cs_instr.ip);
            trace_instr.addStoreAddress(mapped_addr);
        }
    }

    // Extract load addresses (source memory) with address mapping
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_memory[i] != 0) {
            // Map trace addresses to a safe memory region
            uint64_t mapped_addr = mapTraceAddressToVirtual(cs_instr.source_memory[i]);
            DPRINTF(TraceReader, "extractMemoryOps: Load addr 0x%lx -> 0x%lx for PC 0x%lx\n",
                    cs_instr.source_memory[i], mapped_addr, cs_instr.ip);
            trace_instr.addLoadAddress(mapped_addr);
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

void
ChampSimTraceReader::generateSimulatedMemoryValues(const ChampSimInstr &cs_instr,
                                                   TraceInstruction &trace_instr)
{
    // Generate simulated load values for cache hierarchy integration
    // Since ChampSim traces don't contain actual memory values, we simulate them
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_memory[i] != 0) {
            // Generate a deterministic value based on address for consistent simulation
            uint64_t simulated_value = cs_instr.source_memory[i] ^ 0xDEADBEEF;
            trace_instr.addLoadValue(simulated_value);
        }
    }

    // Generate simulated store values
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_memory[i] != 0) {
            // Generate a deterministic value based on address and PC
            uint64_t simulated_value = (cs_instr.destination_memory[i] ^ cs_instr.ip) + 0x12345678;
            trace_instr.addStoreValue(simulated_value);
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

uint64_t
ChampSimTraceReader::mapTraceAddressToVirtual(uint64_t trace_addr)
{
    if (addrMapMode == "linear") {
        return mapAddressLinear(trace_addr);
    } else {
        return mapAddressHash(trace_addr);
    }
}

uint64_t
ChampSimTraceReader::mapAddressHash(uint64_t trace_addr)
{
    // Original hash-based mapping implementation (configurable)
    // Use a simple hash to map the trace address within the safe region
    uint64_t hash = (trace_addr ^ (trace_addr >> 16)) & 0x3FFFFFFFUL;
    uint64_t mapped_addr = addrMapBase + (hash % addrMapSize);

    if (addrPageAlign) {
        // Align to page boundaries to avoid TLB issues
        mapped_addr = (mapped_addr / PAGE_SIZE) * PAGE_SIZE + (trace_addr % PAGE_SIZE);
        // Ensure we stay within our region
        if ((mapped_addr - addrMapBase) >= addrMapSize) {
            mapped_addr = addrMapBase + (trace_addr % addrMapSize);
        }
    }

    // Ensure the mapped address is properly aligned (4-byte aligned for RISC-V)
    mapped_addr = mapped_addr & ~0x3UL;

    return mapped_addr;
}

uint64_t
ChampSimTraceReader::mapAddressLinear(uint64_t trace_addr)
{
    // Linear mapping preserves address relationships and locality
    // Better for cache/TLB research but may have more address conflicts

    uint64_t mapped_addr;

    if (addrPageAlign) {
        // For page-aligned mapping, preserve page offsets
        uint64_t page_offset = trace_addr % PAGE_SIZE;
        uint64_t trace_page = trace_addr / PAGE_SIZE;

        // Map the page linearly within our region
        uint64_t mapped_page = (trace_page % (addrMapSize / PAGE_SIZE));
        mapped_addr = addrMapBase + (mapped_page * PAGE_SIZE) + page_offset;
    } else {
        // Simple linear mapping within the region
        mapped_addr = addrMapBase + (trace_addr % addrMapSize);
    }

    // Ensure 4-byte alignment for RISC-V
    mapped_addr = mapped_addr & ~0x3UL;

    DPRINTF(TraceReader, "mapAddressLinear: 0x%lx -> 0x%lx (page_align=%d)\n",
            trace_addr, mapped_addr, addrPageAlign);

    return mapped_addr;
}

TraceReader::TraceCheckpoint
ChampSimTraceReader::createCheckpoint()
{
    TraceCheckpoint checkpoint;

    checkpoint.instructionIndex = instructionIndex;
    checkpoint.seqNum = currentSeqNum;
    checkpoint.eofState = eofReached;
    checkpoint.bufferSnapshot = instrBuffer;

    if (!compressed && traceStream.is_open()) {
        // For uncompressed files, we can save the file position
        checkpoint.filePosition = traceStream.tellg();
        checkpoint.valid = true;
        DPRINTF(TraceReader, "createCheckpoint: Created checkpoint at instrIndex=%lu, filePos=%ld\n",
                checkpoint.instructionIndex, checkpoint.filePosition);
    } else if (compressed) {
        // For compressed files, we can only checkpoint at current position
        checkpoint.filePosition = std::streampos(0);
        checkpoint.valid = true;
        DPRINTF(TraceReader, "createCheckpoint: Created checkpoint at instrIndex=%lu (compressed)\n",
                checkpoint.instructionIndex);
    } else {
        checkpoint.valid = false;
        DPRINTF(TraceReader, "createCheckpoint: Failed to create checkpoint - stream not open\n");
    }

    return checkpoint;
}

bool
ChampSimTraceReader::restoreCheckpoint(const TraceCheckpoint& checkpoint)
{
    if (!checkpoint.valid) {
        DPRINTF(TraceReader, "restoreCheckpoint: Invalid checkpoint\n");
        return false;
    }

    DPRINTF(TraceReader, "restoreCheckpoint: Restoring to instrIndex=%lu\n",
            checkpoint.instructionIndex);

    if (!compressed && traceStream.is_open()) {
        // For uncompressed files, seek to the saved position
        traceStream.clear();
        traceStream.seekg(checkpoint.filePosition);
        if (traceStream.fail()) {
            DPRINTF(TraceReader, "restoreCheckpoint: Failed to seek to position %ld\n",
                    checkpoint.filePosition);
            return false;
        }
        currentPos = traceStream.tellg();
    } else if (compressed) {
        // For compressed files, we need to reset and re-read to the checkpoint
        if (!reset()) {
            DPRINTF(TraceReader, "restoreCheckpoint: Failed to reset compressed stream\n");
            return false;
        }

        // Re-read to the checkpoint position
        uint64_t targetIndex = checkpoint.instructionIndex;
        while (instructionIndex < targetIndex && !eofReached) {
            TraceInstruction dummy;
            if (!parseInstruction(dummy)) {
                DPRINTF(TraceReader, "restoreCheckpoint: Failed to re-read to checkpoint\n");
                return false;
            }
        }
    } else {
        DPRINTF(TraceReader, "restoreCheckpoint: Stream not open\n");
        return false;
    }

    // Restore state
    instructionIndex = checkpoint.instructionIndex;
    currentSeqNum = checkpoint.seqNum;
    eofReached = checkpoint.eofState;

    // Restore buffer (clear first)
    while (!instrBuffer.empty()) {
        instrBuffer.pop();
    }
    instrBuffer = checkpoint.bufferSnapshot;

    DPRINTF(TraceReader, "restoreCheckpoint: Restored to instrIndex=%lu, seqNum=%lu, bufferSize=%lu\n",
            instructionIndex, currentSeqNum, instrBuffer.size());

    return true;
}

bool
ChampSimTraceReader::seekToInstruction(uint64_t instrIndex)
{
    DPRINTF(TraceReader, "seekToInstruction: Seeking to instruction %lu (current=%lu)\n",
            instrIndex, instructionIndex);

    // Find the closest checkpoint before or at the target
    TraceCheckpoint bestCheckpoint;
    bool foundCheckpoint = false;

    for (const auto& cp : checkpoints) {
        if (cp.valid && cp.instructionIndex <= instrIndex) {
            if (!foundCheckpoint || cp.instructionIndex > bestCheckpoint.instructionIndex) {
                bestCheckpoint = cp;
                foundCheckpoint = true;
            }
        }
    }

    if (foundCheckpoint) {
        DPRINTF(TraceReader, "seekToInstruction: Using checkpoint at instrIndex=%lu\n",
                bestCheckpoint.instructionIndex);
        if (!restoreCheckpoint(bestCheckpoint)) {
            return false;
        }
    } else {
        // No suitable checkpoint found, reset to beginning
        DPRINTF(TraceReader, "seekToInstruction: No checkpoint found, resetting to beginning\n");
        if (!reset()) {
            return false;
        }
    }

    // Read forward to the exact target if needed
    while (instructionIndex < instrIndex && !eofReached) {
        TraceInstruction dummy;
        if (!parseInstruction(dummy)) {
            DPRINTF(TraceReader, "seekToInstruction: Failed to read to target instruction\n");
            return false;
        }
    }

    DPRINTF(TraceReader, "seekToInstruction: Successfully sought to instruction %lu\n", instructionIndex);
    return instructionIndex == instrIndex;
}

uint64_t
ChampSimTraceReader::getCurrentInstructionIndex() const
{
    return instructionIndex;
}

uint64_t
ChampSimTraceReader::estimateBranchTarget(const ChampSimInstr &cs_instr)
{
    // Since ChampSim format doesn't provide branch targets, we estimate them
    // This is a simplified approach for interface completeness and BP training

    // For taken branches, estimate a reasonable target
    if (cs_instr.branch_taken != 0) {
        // Strategy 1: Simple forward offset for most branches
        // This assumes most branches are short forward jumps (loops, conditionals)
        uint64_t estimated_offset = 16; // Typical forward branch offset

        // Strategy 2: Use PC pattern analysis for better estimation
        // Look at PC alignment and estimate direction
        uint64_t pc_low_bits = cs_instr.ip & 0xFF;
        if (pc_low_bits > 0x80) {
            // High PC bits suggest forward branch
            estimated_offset = 32 + (pc_low_bits & 0x3F);
        } else {
            // Low PC bits suggest backward branch (loop)
            estimated_offset = -(64 + (pc_low_bits & 0x1F));
        }

        uint64_t target = cs_instr.ip + estimated_offset;

        DPRINTF(TraceReader, "estimateBranchTarget: PC=0x%lx taken=%d -> estimated target=0x%lx\n",
                cs_instr.ip, cs_instr.branch_taken, target);

        return target;
    }

    // For not-taken branches, target is typically fall-through (next instruction)
    // Assume 4-byte instruction size for RISC-V
    return cs_instr.ip + 4;
}

void
ChampSimTraceReader::setAddressMapping(uint64_t base, uint64_t size, const std::string &mode, bool pageAlign)
{
    addrMapBase = base;
    addrMapSize = size;
    addrMapMode = mode;
    addrPageAlign = pageAlign;
    
    DPRINTF(TraceReader, "Address mapping configured: base=0x%x, size=0x%x, mode=%s, pageAlign=%s\n",
            base, size, mode, pageAlign ? "true" : "false");
}

} // namespace o3
} // namespace gem5
