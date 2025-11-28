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

#ifndef __CPU_O3_TRACE_CHAMPSIM_TRACE_READER_HH__
#define __CPU_O3_TRACE_CHAMPSIM_TRACE_READER_HH__

#include <cstdio>
#include <fstream>
#include <string>

#include "cpu/o3/trace/TraceReader.hh"
#include "ext/iostream3/zfstream.h"

namespace gem5
{
namespace o3
{

/**
 * ChampSim trace reader implementation.
 *
 * This class reads ChampSim binary trace files and converts them
 * to TraceInstruction objects for use by the O3CPU. ChampSim traces
 * contain instruction PC, branch information, and register/memory
 * dependencies in a compact binary format.
 */
class ChampSimTraceReader : public TraceReader
{
  private:
    /** ChampSim instruction format (based on ChampSim's input_instr) */
    struct ChampSimInstr
    {
        /** Instruction pointer or PC */
        uint64_t ip;

        /** Branch information */
        uint8_t is_branch;
        uint8_t branch_taken;

        /** Register dependencies */
        static constexpr size_t NUM_INSTR_DESTINATIONS = 2;
        static constexpr size_t NUM_INSTR_SOURCES = 4;
        uint8_t destination_registers[NUM_INSTR_DESTINATIONS];
        uint8_t source_registers[NUM_INSTR_SOURCES];

        /** Memory operations */
        uint64_t destination_memory[NUM_INSTR_DESTINATIONS];
        uint64_t source_memory[NUM_INSTR_SOURCES];
    };

    /** Input file stream */
    std::ifstream traceStream;

    /** Gzipped input file stream */
    gzifstream gzTraceStream;

    /** Whether file is compressed */
    bool compressed;

    /** Whether file is xz-compressed */
    bool xzCompressed;

    /** Pipe handle for xz-decompressed stream (xz -dc) */
    FILE* xzPipe;

    /** Current position in file for debugging */
    std::streampos currentPos;

    /** Current instruction index in the trace */
    uint64_t instructionIndex;

    /** Checkpoints for rollback capability */
    std::vector<TraceCheckpoint> checkpoints;

    /** Address mapping configuration */
    std::string addrMapMode;        ///< Address mapping mode ("hash" or "linear")
    uint64_t addrMapBase;           ///< Base address for mapping
    uint64_t addrMapSize;           ///< Size of mapping region
    bool addrPageAlign;             ///< Whether to align to page boundaries
    static constexpr uint64_t PAGE_SIZE = 4096; ///< Page size for alignment

    /**
     * Look-ahead staging: hold the last parsed instruction until the next
     * one is available so we can derive branch target as nextPC for taken branches.
     */
    bool hasPendingInstr = false;
    TraceInstruction pendingInstr;

  public:
    /**
     * Constructor
     * @param trace_file Path to ChampSim trace file
     * @param name Name for statistics
     * @param map_mode Address mapping mode ("hash" or "linear")
     * @param base_addr Base address for mapping
     * @param map_size Size of mapping region
     * @param page_align Whether to align to page boundaries
     */
    ChampSimTraceReader(const std::string &trace_file, const std::string &name,
                       const std::string &map_mode = "hash",
                       uint64_t base_addr = 0x10000000UL,
                       uint64_t map_size = 0x40000000UL,
                       bool page_align = true);

    /** Destructor */
    ~ChampSimTraceReader();

    /**
     * Initialize the trace reader
     * @return true if initialization successful
     */
    bool init() override;

    /**
     * Reset the trace reader to the beginning
     * @return true if reset successful
     */
    bool reset() override;

    /**
     * Get trace format identifier
     * @return "champsim"
     */
    std::string getFormat() const override { return "champsim"; }

    /**
     * Create a checkpoint of the current trace reader state
     * @return TraceCheckpoint containing current state
     */
    TraceCheckpoint createCheckpoint() override;

    /**
     * Restore trace reader state from a checkpoint
     * @param checkpoint The checkpoint to restore from
     * @return true if restore successful, false otherwise
     */
    bool restoreCheckpoint(const TraceCheckpoint& checkpoint) override;

    /**
     * Seek to a specific instruction index in the trace
     * @param instrIndex The instruction index to seek to
     * @return true if seek successful, false otherwise
     */
    bool seekToInstruction(uint64_t instrIndex) override;

    /**
     * Get the current instruction index in the trace
     * @return Current instruction index
     */
    uint64_t getCurrentInstructionIndex() const override;

    /**
     * Configure address mapping parameters after construction
     * @param base Base address for mapping
     * @param size Size of mapping region  
     * @param mode Address mapping mode ("hash" or "linear")
     * @param pageAlign Whether to align to page boundaries
     */
    void setAddressMapping(uint64_t base, uint64_t size, const std::string &mode, bool pageAlign);

  protected:
    /**
     * Fill buffer with instructions from trace file
     * @param max_instructions Maximum number of instructions to read
     * @return Number of instructions actually read
     */
    size_t fillBuffer(size_t max_instructions) override;

    /**
     * Parse a single instruction from ChampSim format
     * @param instr Reference to TraceInstruction to populate
     * @return true if instruction parsed successfully
     */
    bool parseInstruction(TraceInstruction &instr) override;

    /**
     * Validate that the trace file exists and is readable
     * @return true if file is valid
     */
    bool validateTraceFile() override;

  private:
    /**
     * Read a ChampSim instruction from the binary file
     * @param cs_instr Reference to ChampSimInstr to populate
     * @return true if instruction read successfully
     */
    bool readChampSimInstruction(ChampSimInstr &cs_instr);

    /**
     * Convert ChampSim instruction to TraceInstruction
     * @param cs_instr ChampSim instruction
     * @param trace_instr Output TraceInstruction
     */
    void convertInstruction(const ChampSimInstr &cs_instr,
                           TraceInstruction &trace_instr);

    /**
     * Determine instruction type from ChampSim instruction
     * @param cs_instr ChampSim instruction
     * @return Instruction type
     */
    TraceInstruction::InstType determineInstType(const ChampSimInstr &cs_instr);

    /**
     * Check if instruction has memory operations
     * @param cs_instr ChampSim instruction
     * @return true if instruction accesses memory
     */
    bool hasMemoryOps(const ChampSimInstr &cs_instr);

    /**
     * Extract memory addresses from ChampSim instruction
     * @param cs_instr ChampSim instruction
     * @param trace_instr Output TraceInstruction to populate
     */
    void extractMemoryOps(const ChampSimInstr &cs_instr,
                         TraceInstruction &trace_instr);

    /**
     * Extract register dependencies from ChampSim instruction
     * @param cs_instr ChampSim instruction
     * @param trace_instr Output TraceInstruction to populate
     */
    void extractRegisterDeps(const ChampSimInstr &cs_instr,
                            TraceInstruction &trace_instr);

    /**
     * Generate simulated memory values for cache hierarchy integration
     * Since ChampSim traces don't contain memory values, we simulate them
     * @param cs_instr ChampSim instruction
     * @param trace_instr Output TraceInstruction
     */
    void generateSimulatedMemoryValues(const ChampSimInstr &cs_instr,
                                      TraceInstruction &trace_instr);

    /**
     * Check if file is compressed (has .gz extension)
     * @param filename File path to check
     * @return true if file appears to be compressed
     */
    bool isGzip(const std::string &filename);
    bool isXz(const std::string &filename);

    /**
     * Estimate branch target address for a taken branch instruction
     * Since ChampSim format doesn't provide targets directly, this method
     * estimates reasonable targets for branch prediction validation
     * @param cs_instr ChampSim instruction with branch information
     * @return Estimated branch target address (0 if cannot estimate)
     */
    uint64_t estimateBranchTarget(const ChampSimInstr &cs_instr);

    /**
     * Map trace address to a valid virtual address for the simulation
     * @param trace_addr Original address from trace
     * @return Mapped virtual address that won't cause page table faults
     */
    uint64_t mapTraceAddressToVirtual(uint64_t trace_addr);

    /**
     * Map trace PC to virtual address (keeps full mapping, no extra alignment).
     */
    uint64_t mapTracePcToVirtual(uint64_t trace_pc);

    /**
     * Map trace memory address to virtual address and enforce minimal alignment
     * so that scalar memory operations (e.g., sw) do not trigger RISC-V
     * misaligned address faults in trace mode.
     */
    uint64_t mapTraceMemToVirtual(uint64_t trace_addr);

    /**
     * Hash-based address mapping (original implementation)
     * @param trace_addr Original address from trace
     * @return Hashed and mapped virtual address
     */
    uint64_t mapAddressHash(uint64_t trace_addr);

    /**
     * Linear address mapping with optional page alignment
     * @param trace_addr Original address from trace
     * @return Linearly mapped virtual address
     */
    uint64_t mapAddressLinear(uint64_t trace_addr);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_CHAMPSIM_TRACE_READER_HH__
