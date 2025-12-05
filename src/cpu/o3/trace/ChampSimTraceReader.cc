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
    : TraceReader(trace_file, name, parent), compressed(false), xzCompressed(false), xzPipe(nullptr), currentPos(0),
      instructionIndex(0), addrMapMode(map_mode), addrMapBase(base_addr),
      addrMapSize(map_size), addrPageAlign(page_align)
{
    compressed = isGzip(trace_file);
    xzCompressed = isXz(trace_file);
    hasPendingInstr = false;

    // Address mapping configuration initialized
    // mode=%s, base=0x%lx, size=0x%lx, page_align=%d
    // (DPRINTF temporarily removed for compilation)
}

ChampSimTraceReader::~ChampSimTraceReader()
{
    if (compressed && gzTraceStream.is_open()) {
        gzTraceStream.close();
    } else if (!compressed && !xzCompressed && traceStream.is_open()) {
        traceStream.close();
    }
    if (xzCompressed && xzPipe) {
        pclose(xzPipe);
        xzPipe = nullptr;
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
    } else if (xzCompressed) {
        DPRINTF(TraceReader, "init: Opening xz-compressed trace via pipe: %s\n", traceFile.c_str());
        // Build a shell-safe command with quoting and `--` to stop option parsing
        std::string safe = traceFile;
        size_t pos = 0;
        while ((pos = safe.find("'", pos)) != std::string::npos) {
            safe.replace(pos, 1, "'\\''");
            pos += 4;
        }
        std::string cmd = std::string("xz -dc -- '") + safe + "'";
        xzPipe = popen(cmd.c_str(), "r");
        if (!xzPipe) {
            DPRINTF(TraceReader, "init: Failed to open xz pipe for trace file\n");
            return false;
        }
        currentPos = 0;
        DPRINTF(TraceReader, "init: xz pipe opened successfully\n");
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
    DPRINTF(TraceReader, "reset: Resetting trace reader (gzip=%d, xz=%d)\n", compressed, xzCompressed);

    // Dump buffer before reset clears it
    dumpInstrBuffer("before_reset");

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
    } else if (xzCompressed) {
        if (xzPipe) {
            pclose(xzPipe);
            DPRINTF(TraceReader, "reset: Closed xz pipe\n");
        }
        std::string safe = traceFile;
        size_t pos = 0;
        while ((pos = safe.find("'", pos)) != std::string::npos) {
            safe.replace(pos, 1, "'\\''");
            pos += 4;
        }
        std::string cmd = std::string("xz -dc -- '") + safe + "'";
        xzPipe = popen(cmd.c_str(), "r");
        if (!xzPipe) {
            DPRINTF(TraceReader, "reset: Failed to reopen xz pipe\n");
            return false;
        }
        currentPos = 0;
        DPRINTF(TraceReader, "reset: Reopened xz pipe successfully\n");
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
    // Dump buffer after reset
    dumpInstrBuffer("after_reset");
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
        (xzCompressed && xzPipe == nullptr) ||
        (!compressed && !xzCompressed && !traceStream.is_open())) {
        DPRINTF(TraceReader, "fillBuffer: Cannot read - eofReached=%d, compressed=%d, stream_open=%d\n",
                eofReached, (int)(compressed || xzCompressed),
                compressed ? gzTraceStream.is_open() : (!xzCompressed && traceStream.is_open()));
        return 0;
    }

    DPRINTF(TraceReader, "fillBuffer: Starting to read (target push %lu) (compressed=%d)\n",
            max_instructions, (int)(compressed || xzCompressed));

    size_t pushed = 0;

    auto isApproxFallthrough = [](Addr pc, Addr next_pc) {
        return next_pc == pc + 2 || next_pc == pc + 4;
    };

    while (pushed < max_instructions && !eofReached) {
        TraceInstruction current;
        if (parseInstruction(current)) {
            if (hasPendingInstr) {
                Addr next_pc = current.getPC();
                const Addr curr_pc = pendingInstr.getPC();
                const bool pending_taken_branch =
                    pendingInstr.isAnyBranch() && pendingInstr.getBranchTaken();

                // 健壮性兜底：非分支指令且下一条 PC 非 2 字节对齐时，强制认为顺序流 pc+4。
                // 避免 trace 中异常 PC 破坏后续长度推断 / fallthrough。
                if (!pendingInstr.isAnyBranch() && (next_pc & 0x1)) {
                    Addr corrected_pc = curr_pc + 4;
                    current.setPC(corrected_pc);
                    next_pc = corrected_pc;
                }
                if (!pending_taken_branch && next_pc >= curr_pc) {
                    const uint64_t delta = next_pc - curr_pc;
                    // Trace may contain 2-byte (compressed) and 4-byte instructions.
                    // Capture the observed delta when it's a small forward step so
                    // Fetch can emit the proper encoding.
                    if (delta > 0 && delta <= 8) {
                        pendingInstr.setInstSizeBytes(static_cast<uint8_t>(delta));
                    }
                }

                if (pendingInstr.isAnyBranch()) {
                    const bool taken = pendingInstr.getBranchTaken();
                    if (taken) {
                        // 正常 taken 分支：next_pc 作为分支目标
                        pendingInstr.setBranchTarget(next_pc);
                        DPRINTF(TraceReader,
                                "fillBuffer: Set branch target from nextPC: 0x%lx\n",
                                next_pc);
                    } else {
                        // not-taken 分支，但 next_pc 不是近似顺序流（pc+2/pc+4）：视为 cond-trap
                        if (!isApproxFallthrough(pendingInstr.getPC(), next_pc)) {
                            pendingInstr.setCtrlFlowChange(true);
                            pendingInstr.setCtrlFlowTarget(next_pc);
                            DPRINTF(TraceReader,
                                    "fillBuffer: Mark branch-not-taken ctrl-flow change: "
                                    "pc=0x%lx -> nextPC=0x%lx\n",
                                    pendingInstr.getPC(), next_pc);
                        }
                    }
                } else {
                    // 非分支 + next_pc 不是近似顺序流：视为非分支 trap/异常控制流改变
                    if (!isApproxFallthrough(pendingInstr.getPC(), next_pc)) {
                        pendingInstr.setCtrlFlowChange(true);
                        pendingInstr.setCtrlFlowTarget(next_pc);
                        DPRINTF(TraceReader,
                                "fillBuffer: Mark non-branch ctrl-flow change: "
                                "pc=0x%lx -> nextPC=0x%lx\n",
                                pendingInstr.getPC(), next_pc);
                    }
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
        // Mark this instruction as the last one in the trace stream so
        // downstream components (e.g., O3 commit stage) can terminate
        // cleanly once it commits.
        pendingInstr.setLastInTrace(true);
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
        (xzCompressed && xzPipe == nullptr) ||
        (!compressed && !xzCompressed && !traceStream.is_open())) {
        DPRINTF(TraceReader, "readChampSimInstruction: Cannot read - eofReached=%d, compressed=%d, stream_open=%d\n",
                eofReached, (int)(compressed || xzCompressed),
                compressed ? gzTraceStream.is_open() : (!xzCompressed && traceStream.is_open()));
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
    } else if (xzCompressed) {
        DPRINTF(TraceReader, "readChampSimInstruction: Reading %lu bytes from xz pipe\n",
                sizeof(ChampSimInstr));
        bytes_read = std::fread(reinterpret_cast<char*>(&cs_instr), 1, sizeof(ChampSimInstr), xzPipe);
        if (bytes_read != (std::streamsize)sizeof(ChampSimInstr)) {
            if (feof(xzPipe)) {
                eofReached = true;
            } else {
                // Debug: Error reading from xz pipe
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
    uint64_t mapped_pc = mapTracePcToVirtual(cs_instr.ip);
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
    // Extract store addresses (destination memory) with address mapping for trace mode
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_DESTINATIONS; i++) {
        if (cs_instr.destination_memory[i] != 0) {
            // Map trace addresses to a safe memory region (e.g., starting at 0x10000000)
            uint64_t mapped_addr = mapTraceMemToVirtual(cs_instr.destination_memory[i]);
            DPRINTF(TraceReader, "extractMemoryOps: Store addr 0x%lx -> 0x%lx for PC 0x%lx\n",
                    cs_instr.destination_memory[i], mapped_addr, cs_instr.ip);
            trace_instr.addStoreAddress(mapped_addr);
        }
    }

    // Extract load addresses (source memory) with address mapping
    for (size_t i = 0; i < ChampSimInstr::NUM_INSTR_SOURCES; i++) {
        if (cs_instr.source_memory[i] != 0) {
            // Map trace addresses to a safe memory region
            uint64_t mapped_addr = mapTraceMemToVirtual(cs_instr.source_memory[i]);
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
ChampSimTraceReader::mapTracePcToVirtual(uint64_t trace_pc)
{
    // PC uses the full mapping (page alignment etc.) so that external
    // tools and difftest can correlate PCs 1:1 with trace addresses.
    return mapTraceAddressToVirtual(trace_pc);
}

uint64_t
ChampSimTraceReader::mapTraceMemToVirtual(uint64_t trace_addr)
{
    // Memory addresses reuse the same mapping but we additionally enforce
    // a minimal alignment so that scalar memory operations (e.g., sw) do
    // not systematically trigger RISC-V misaligned-address faults.
    uint64_t mapped = mapTraceAddressToVirtual(trace_addr);
    // For now we conservatively align to 4 bytes, which is enough for
    // 32-bit accesses and significantly reduces misaligned faults coming
    // from arbitrary ChampSim addresses.
    mapped &= ~static_cast<uint64_t>(0x3);
    return mapped;
}

uint64_t
ChampSimTraceReader::mapAddressHash(uint64_t trace_addr)
{
    // Original hash-based mapping implementation (configurable)
    // Use a simple hash to map the trace address within the safe region
    uint64_t hash = (trace_addr ^ (trace_addr >> 16)) & 0x3FFFFFFFUL;
    uint64_t mapped_addr = addrMapBase + (hash % addrMapSize);

    if (addrPageAlign) {
        // Align to ISA page boundaries to avoid TLB issues and keep offsets
        const uint64_t PageSz = TheISA::PageBytes;
        mapped_addr = (mapped_addr / PageSz) * PageSz + (trace_addr % PageSz);
        // Ensure we stay within our region
        if ((mapped_addr - addrMapBase) >= addrMapSize) {
            mapped_addr = addrMapBase + (trace_addr % addrMapSize);
        }
    }

    return mapped_addr;
}

uint64_t
ChampSimTraceReader::mapAddressLinear(uint64_t trace_addr)
{
    // Linear mapping preserves address relationships and locality
    // Better for cache/TLB research but may have more address conflicts

    uint64_t mapped_addr;

    if (addrPageAlign) {
        // For page-aligned mapping, preserve page offsets with ISA-defined page size
        const uint64_t PageSz = TheISA::PageBytes;
        const uint64_t page_offset = trace_addr % PageSz;
        const uint64_t trace_page = trace_addr / PageSz;

        // Ensure the mapping region is page-aligned in size to avoid discontinuities
        const uint64_t pages_in_region = (addrMapSize / PageSz);
        const uint64_t mapped_page = (pages_in_region ? (trace_page % pages_in_region) : 0);
        mapped_addr = addrMapBase + (mapped_page * PageSz) + page_offset;
    } else {
        // Simple linear mapping within the region based on full-byte offset
        mapped_addr = addrMapBase + (trace_addr % addrMapSize);
    }

    DPRINTF(TraceReader, "mapAddressLinear: 0x%lx -> 0x%lx (page_align=%d, PageSz=%llu)\n",
            trace_addr, mapped_addr, addrPageAlign, (unsigned long long)TheISA::PageBytes);

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
    // Save pending instruction state (if any)
    checkpoint.hasPending = hasPendingInstr;
    if (hasPendingInstr) {
        checkpoint.pending = pendingInstr;
    }

    if (!compressed && !xzCompressed && traceStream.is_open()) {
        // For uncompressed files, we can save the file position
        checkpoint.filePosition = traceStream.tellg();
        checkpoint.valid = true;
        DPRINTF(TraceReader, "createCheckpoint: Created checkpoint at instrIndex=%lu, filePos=%ld\n",
                checkpoint.instructionIndex, checkpoint.filePosition);
    } else if (compressed || xzCompressed) {
        // For compressed files, we can only checkpoint at current position
        checkpoint.filePosition = std::streampos(0);
        checkpoint.valid = true;
        DPRINTF(TraceReader, "createCheckpoint: Created checkpoint at instrIndex=%lu (compressed)\n",
                checkpoint.instructionIndex);
    } else {
        checkpoint.valid = false;
        DPRINTF(TraceReader, "createCheckpoint: Failed to create checkpoint - stream not open\n");
    }

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
    if (!checkpoint.valid) {
        DPRINTF(TraceReader, "restoreCheckpoint: Invalid checkpoint\n");
        return false;
    }

    DPRINTF(TraceReader, "restoreCheckpoint: Restoring to instrIndex=%lu\n",
            checkpoint.instructionIndex);

    // Debug: print pending status BEFORE restoring
    DPRINTF(TraceReader,
            "restoreCheckpoint: BEFORE restore hasPendingInstr=%d, pending_sn=%llu, pending_pc=0x%llx\n",
            hasPendingInstr,
            (unsigned long long)(hasPendingInstr ? pendingInstr.getSeqNum() : 0ULL),
            (unsigned long long)(hasPendingInstr ? pendingInstr.getPC() : 0ULL));

    // Dump buffer before restore overwrites it
    dumpInstrBuffer("before_restore");

    if (!compressed && !xzCompressed && traceStream.is_open()) {
        // For uncompressed files, seek to the saved position
        traceStream.clear();
        traceStream.seekg(checkpoint.filePosition);
        if (traceStream.fail()) {
            DPRINTF(TraceReader, "restoreCheckpoint: Failed to seek to position %ld\n",
                    checkpoint.filePosition);
            return false;
        }
        currentPos = traceStream.tellg();
    } else if (compressed || xzCompressed) {
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

    // Restore pending instruction state so that the next fillBuffer() will
    // first flush this pending into instrBuffer, preserving sequence continuity.
    hasPendingInstr = checkpoint.hasPending;
    if (hasPendingInstr) {
        pendingInstr = checkpoint.pending;
    } else {
        // Make sure no stale pending remains
        pendingInstr.reset();
    }

    DPRINTF(TraceReader, "restoreCheckpoint: Restored to instrIndex=%lu, seqNum=%lu, bufferSize=%lu\n",
            instructionIndex, currentSeqNum, instrBuffer.size());

    // Debug: print pending status AFTER restoring
    DPRINTF(TraceReader,
            "restoreCheckpoint: AFTER restore hasPendingInstr=%d, pending_sn=%llu, pending_pc=0x%llx\n",
            hasPendingInstr,
            (unsigned long long)(hasPendingInstr ? pendingInstr.getSeqNum() : 0ULL),
            (unsigned long long)(hasPendingInstr ? pendingInstr.getPC() : 0ULL));

    // Dump buffer after restore
    dumpInstrBuffer("after_restore");

    return true;
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
    addrMapBase = base;
    addrMapSize = size;
    addrMapMode = mode;
    addrPageAlign = pageAlign;
    
    DPRINTF(TraceReader, "Address mapping configured: base=0x%x, size=0x%x, mode=%s, pageAlign=%s\n",
            base, size, mode, pageAlign ? "true" : "false");
}

} // namespace o3
} // namespace gem5
