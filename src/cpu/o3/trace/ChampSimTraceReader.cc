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

#include <algorithm>
#include <iostream>

#include "arch/riscv/page_size.hh"
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
                                         bool page_align,
                                         statistics::Group *parent)
    : TraceReader(trace_file, name, parent), streamMode(TraceStream::Mode::Raw), currentPos(0),
      instructionIndex(0)
{
    if (isXz(trace_file)) {
        streamMode = TraceStream::Mode::Xz;
    } else if (isGzip(trace_file)) {
        streamMode = TraceStream::Mode::Gzip;
    } else {
        streamMode = TraceStream::Mode::Raw;
    }
    hasPendingInstr = false;
    setAddrMapConfig({base_addr, map_size, map_mode, page_align});

    // Address mapping configuration initialized
    // mode=%s, base=0x%lx, size=0x%lx, page_align=%d
    // (DPRINTF temporarily removed for compilation)
}

ChampSimTraceReader::~ChampSimTraceReader()
{
    traceStream.close();
}

TraceReader::AddrMapConfig
ChampSimTraceReader::addrCfgSnapshot() const
{
    return getAddrMapConfig();
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

    const bool compressedLike = streamMode != TraceStream::Mode::Raw;
    DPRINTF(TraceReader, "init: Opening trace file (%s): %s\n",
            compressedLike ? "compressed" : "raw", traceFile.c_str());
    if (!openTraceStream(traceStream, streamMode, &currentPos)) {
        DPRINTF(TraceReader, "init: Failed to open trace stream\n");
        return false;
    }
    DPRINTF(TraceReader, "init: ChampSim trace file opened successfully\n");

    resetBufferState();
    instructionIndex = 0;
    initialized = true;

    return true;
}

bool
ChampSimTraceReader::reset()
{
    DPRINTF(TraceReader, "reset: Resetting trace reader (mode=%d)\n",
            static_cast<int>(streamMode));

    // Dump buffer before reset clears it
    dumpInstrBuffer("before_reset");

    if (!traceStream.reopen()) {
        DPRINTF(TraceReader, "reset: Failed to reopen trace stream\n");
        return false;
    }
    if (!reopenTraceStream(traceStream, streamMode, &currentPos)) {
        DPRINTF(TraceReader, "reset: Failed to reopen trace stream\n");
        return false;
    }

    resetBufferState();
    instructionIndex = 0;
    checkpoints.clear();

    DPRINTF(TraceReader, "reset: Reset completed, eofReached=%d, buffer size=%lu\n",
            eofReached, instrBuffer.size());
    // Dump buffer after reset
    dumpInstrBuffer("after_reset");
    return true;
}

bool
ChampSimTraceReader::validateTraceFile()
{
    return validateTraceFileSize(traceFile,
                                 static_cast<std::streamsize>(sizeof(ChampSimInstr)));
}

size_t
ChampSimTraceReader::fillBuffer(size_t max_instructions)
{
    if (eofReached || !traceStream.isOpen()) {
        DPRINTF(TraceReader, "fillBuffer: Cannot read - eofReached=%d, stream_open=%d\n",
                eofReached, traceStream.isOpen());
        return 0;
    }

    const bool compressedLike = streamMode != TraceStream::Mode::Raw;
    DPRINTF(TraceReader, "fillBuffer: Starting to read (target push %lu) (compressed=%d)\n",
            max_instructions, (int)compressedLike);

    PendingResolveConfig resolveCfg;
    const size_t pushed = drainPendingToBuffer(max_instructions, resolveCfg,
                                               /*markLastInTrace=*/true);

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
    if (eofReached || !traceStream.isOpen()) {
        DPRINTF(TraceReader, "readChampSimInstruction: Cannot read - eofReached=%d, stream_open=%d\n",
                eofReached, traceStream.isOpen());
        return false;
    }

    const bool ok = traceStream.readExact(reinterpret_cast<char*>(&cs_instr),
                                          sizeof(ChampSimInstr));
    if (!ok) {
        eofReached = traceStream.eof();
        return false;
    }
    if (streamMode == TraceStream::Mode::Raw) {
        currentPos = traceStream.tell();
    }

    return true;
}

void
ChampSimTraceReader::convertInstruction(const ChampSimInstr &cs_instr,
                                       TraceInstruction &trace_instr)
{
    // Reset the instruction
    trace_instr.reset();
    const auto cfg = getAddrMapConfig();

    // Set basic fields
    // CRITICAL FIX: Apply address mapping to PC to avoid page table faults
    uint64_t mapped_pc = mapTracePcToVirtual(cs_instr.ip, cfg);
    DPRINTF(TraceReader, "convertInstruction: Mapping PC 0x%lx -> 0x%lx\n", cs_instr.ip, mapped_pc);
    trace_instr.setPC(mapped_pc);
    trace_instr.setSeqNum(getNextSeqNum());
    trace_instr.setInstSizeBytes(4);
    DPRINTF(TraceReader, "convertInstruction: Assigned SeqNum %lu\n", trace_instr.getSeqNum());
    trace_instr.setValid(true);

    // Determine instruction type
    TraceInstruction::InstType inst_type = determineInstType(cs_instr);
    trace_instr.setInstType(inst_type);

    // Set branch information
    bool eff_taken = false;
    switch (inst_type) {
      case TraceInstruction::InstType::COND_BRANCH:
        eff_taken = cs_instr.branch_taken != 0;
        break;
      case TraceInstruction::InstType::UNCOND_DIRECT_BRANCH:
      case TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH:
      case TraceInstruction::InstType::CALL_DIRECT:
      case TraceInstruction::InstType::CALL_INDIRECT:
      case TraceInstruction::InstType::RETURN:
        eff_taken = true;
        break;
      default:
        eff_taken = false;
        break;
    }
    if (cs_instr.is_branch || inst_type == TraceInstruction::InstType::RETURN ||
        inst_type == TraceInstruction::InstType::CALL_DIRECT ||
        inst_type == TraceInstruction::InstType::CALL_INDIRECT ||
        inst_type == TraceInstruction::InstType::UNCOND_DIRECT_BRANCH ||
        inst_type == TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH) {
        trace_instr.setBranchTaken(eff_taken);
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
    // Classify branches per ChampSim's instruction.h logic using special regs
    auto contains = [](const auto& arr, uint8_t v) {
        for (auto x : arr) {
            if (x == v) return true;
        }
        return false;
    };

    constexpr uint8_t REG_SP = 6;    // champsim::REG_STACK_POINTER
    constexpr uint8_t REG_FL = 25;   // champsim::REG_FLAGS
    constexpr uint8_t REG_IP = 26;   // champsim::REG_INSTRUCTION_POINTER
    constexpr uint8_t REG_RA = 1;    // RISC-V return-address (ABI), used in traces for jalr/ret

    bool writes_sp = contains(cs_instr.destination_registers, REG_SP);
    bool writes_ip = contains(cs_instr.destination_registers, REG_IP);
    bool reads_sp  = contains(cs_instr.source_registers, REG_SP);
    bool reads_fl  = contains(cs_instr.source_registers, REG_FL);
    bool reads_ip  = contains(cs_instr.source_registers, REG_IP);
    bool reads_other = false;
    // bool reads_ra = contains(cs_instr.source_registers, REG_RA);
    for (auto r : cs_instr.source_registers) {
        if (r != 0 && r != REG_SP && r != REG_FL && r != REG_IP) { reads_other = true; break; }
    }

    if (cs_instr.is_branch || writes_ip) {
        // // RISC-V ret pattern (jalr x0,x1,0) has only RA as meaningful source,
        // // no SP/flags traffic and still writes IP.
        // if (!reads_sp && !reads_fl && !writes_sp && writes_ip && reads_ra && !reads_other) {
        //     return TraceInstruction::InstType::RETURN;
        // }
        if (!reads_sp && !reads_fl && writes_ip && !reads_other) {
            return TraceInstruction::InstType::UNCOND_DIRECT_BRANCH; // direct jump
        } else if (!reads_sp && !reads_fl && writes_ip && reads_other) {
            return TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH; // indirect branch
        } else if (!reads_sp && reads_ip && !writes_sp && writes_ip && reads_fl && !reads_other) {
            return TraceInstruction::InstType::COND_BRANCH; // conditional branch
        } else if (reads_sp && reads_ip && writes_sp && writes_ip && !reads_fl && !reads_other) {
            return TraceInstruction::InstType::CALL_DIRECT; // direct call
        } else if (reads_sp && reads_ip && writes_sp && writes_ip && !reads_fl && reads_other) {
            return TraceInstruction::InstType::CALL_INDIRECT; // indirect call
        } else if (reads_sp && !reads_ip && writes_sp && writes_ip) {
            return TraceInstruction::InstType::RETURN; // return
        } else if (writes_ip) {
            return TraceInstruction::InstType::COND_BRANCH; // branch_other fallback
        }
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
    const auto cfg = getAddrMapConfig();

    // Extract store addresses (destination memory) with address mapping for trace mode
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_memory[i] != 0) {
            // Map trace addresses to a safe memory region (e.g., starting at 0x10000000)
            uint64_t mapped_addr = mapTraceMemToVirtual(cs_instr.destination_memory[i], cfg);
            DPRINTF(TraceReader, "extractMemoryOps: Store addr 0x%lx -> 0x%lx for PC 0x%lx\n",
                    cs_instr.destination_memory[i], mapped_addr, cs_instr.ip);
            trace_instr.addStoreAddress(mapped_addr);
        }
    }

    // Extract load addresses (source memory) with address mapping
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_memory[i] != 0) {
            // Map trace addresses to a safe memory region
            uint64_t mapped_addr = mapTraceMemToVirtual(cs_instr.source_memory[i], cfg);
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
    auto mapIntReg = [&](uint8_t r) -> uint8_t {
        // ChampSim: 6=SP, 25=FLAGS, 26=IP. No dedicated RA semantic.
        if (r == 0)  return 0;
        if (r == 25) return 0;   // FLAGS
        if (r == 26) return 0;   // IP is not a GPR
        if (r == 6)  return 2;   // SP
        if (r == 1)  return 3;   // avoid x1 (RA) semantics; map to gp (x3)
        if (r == 5)  return 3;   // avoid x5 as alt RA; steer to non-RA reg
        if (r < 32)  return r;   // generic GPRs keep number
        DPRINTF(TraceReader, "[TRACE-ENC] ChampSim reg %u unmapped -> x0\n", r);
        return 0;                // higher IDs -> x0 to avoid false deps
    };
    auto mapFpReg = [mapIntReg](uint8_t r) -> uint8_t {
        // ChampSim traces don't encode FP regs explicitly; fallback to int mapping.
        return mapIntReg(r);
    };

    // Extract source registers with normalization
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_registers[i] != 0) {
            auto mapped = mapIntReg(cs_instr.source_registers[i]);
            trace_instr.addSrcReg(mapped);
        }
    }

    // Extract destination registers with normalization
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_registers[i] != 0) {
            auto mapped = mapFpReg(cs_instr.destination_registers[i]);
            trace_instr.addDstReg(mapped);
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
ChampSimTraceReader::isGzip(const std::string &filename)
{
    // Simple check for .gz extension
    return filename.size() > 3 && filename.substr(filename.size() - 3) == ".gz";
}

bool
ChampSimTraceReader::isXz(const std::string &filename)
{
    // Simple check for .xz extension
    return filename.size() > 3 && filename.substr(filename.size() - 3) == ".xz";
}

TraceReader::TraceCheckpoint
ChampSimTraceReader::createCheckpoint()
{
    TraceCheckpoint checkpoint = buildCheckpoint(
        instructionIndex, currentSeqNum, eofReached, instrBuffer, hasPendingInstr,
        pendingInstr, traceStream, streamMode, /*allowCompressed=*/true);
    // Debug: also print pending status to verify snapshot coverage
    DPRINTF(TraceReader,
            "createCheckpoint: hasPendingInstr=%d, pending_sn=%llu, pending_pc=0x%llx\n",
            hasPendingInstr,
            (unsigned long long)(hasPendingInstr ? pendingInstr.getSeqNum() : 0ULL),
            (unsigned long long)(hasPendingInstr ? pendingInstr.getPC() : 0ULL));

    return checkpoint;
}

bool
ChampSimTraceReader::restoreCheckpoint(const TraceCheckpoint& checkpoint)
{
    auto ff = [this](uint64_t targetIndex) -> bool {
        resetBufferState();
        instructionIndex = 0;
        while (instructionIndex < targetIndex && !eofReached) {
            TraceInstruction dummy;
            if (!parseInstruction(dummy)) {
                return false;
            }
        }
        return true;
    };

    const bool ok = restoreCheckpointCommon(checkpoint, traceStream, streamMode,
                                            /*allowCompressedRewind=*/true, ff,
                                            instructionIndex);
    if (ok && streamMode == TraceStream::Mode::Raw && traceStream.isOpen()) {
        currentPos = traceStream.tell();
    }
    return ok;
}

bool
ChampSimTraceReader::seekToInstruction(uint64_t instrIndex)
{
    DPRINTF(TraceReader, "seekToInstruction: Seeking to instruction %lu (current=%lu)\n",
            instrIndex, instructionIndex);

    // Support a 0 index as "beginning of trace" sentinel to make 1-based
    // external indexing convenient (seek(0) -> before first instruction).
    if (instrIndex == 0) {
        if (!reset()) {
            return false;
        }
        // Ensure we are at start-of-trace state; instructionIndex remains 0 here.
        DPRINTF(TraceReader, "seekToInstruction: Reset to beginning (index=0)\n");
        return true;
    }

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

    // Debug: BEFORE fast-forward status
    DPRINTF(TraceReader,
            "seekToInstruction: BEFORE FF idx=%lu, seq=%lu, bufSize=%lu, "
            "hasPending=%d, pending_sn=%llu, pending_pc=0x%llx\n",
            instructionIndex, currentSeqNum, instrBuffer.size(),
            hasPendingInstr,
            (unsigned long long)(hasPendingInstr ? pendingInstr.getSeqNum() : 0ULL),
            (unsigned long long)(hasPendingInstr ? pendingInstr.getPC() : 0ULL));

    // Read forward to the exact target if needed (fast-forward parse only; NOT enqueued)
    while (instructionIndex < instrIndex && !eofReached) {
        TraceInstruction dummy;
        if (!parseInstruction(dummy)) {
            DPRINTF(TraceReader, "seekToInstruction: Failed to read to target instruction\n");
            return false;
        }
        DPRINTF(TraceReader,
                "seekToInstruction: FF parsed PC=0x%llx (sn:%llu) - NOT enqueued\n",
                (unsigned long long)dummy.getPC(),
                (unsigned long long)dummy.getSeqNum());
    }

    // Debug: AFTER fast-forward status
    DPRINTF(TraceReader,
            "seekToInstruction: AFTER FF idx=%lu, seq=%lu, bufSize=%lu, "
            "hasPending=%d, pending_sn=%llu, pending_pc=0x%llx\n",
            instructionIndex, currentSeqNum, instrBuffer.size(),
            hasPendingInstr,
            (unsigned long long)(hasPendingInstr ? pendingInstr.getSeqNum() : 0ULL),
            (unsigned long long)(hasPendingInstr ? pendingInstr.getPC() : 0ULL));

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
    setAddrMapConfig({base, size, mode, pageAlign});

    DPRINTF(TraceReader,
            "Address mapping configured: base=0x%llx, size=0x%llx, "
            "mode=%s, pageAlign=%s\n",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(size),
            mode.c_str(), pageAlign ? "true" : "false");
}

} // namespace o3
} // namespace gem5
