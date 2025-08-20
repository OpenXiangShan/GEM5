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

#include <fstream>
#include <string>

#include "cpu/o3/trace/TraceReader.hh"

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
    
    /** Whether file is compressed */
    bool compressed;
    
    /** Current position in file for debugging */
    std::streampos currentPos;

  public:
    /**
     * Constructor
     * @param trace_file Path to ChampSim trace file
     * @param name Name for statistics
     */
    ChampSimTraceReader(const std::string &trace_file, const std::string &name);
    
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
     * Check if file is compressed (has .gz extension)
     * @param filename File path to check
     * @return true if file appears to be compressed
     */
    bool isCompressed(const std::string &filename);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_CHAMPSIM_TRACE_READER_HH__