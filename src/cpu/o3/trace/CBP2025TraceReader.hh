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

#include <string>
#include <vector>

#include "cpu/o3/trace/TraceReader.hh"

namespace gem5
{
namespace o3
{

/**
 * CBP2025 trace reader implementation.
 *
 * This reader consumes CBP2025 binary traces (see cbp2025/lib/trace_reader.h)
 * and converts them into TraceInstruction objects understood by O3CPU.
 *
 * Design goals (KISS/YAGNI):
 * - Only one TraceInstruction per CBP macro-op (no further cracking yet).
 * - Support uncompressed and .gz traces; map trace PCs/VAs into the simulated
 *   address space using the same hash/linear strategy as ChampSim reader.
 * - Preserve last-in-trace marker so commit can exit cleanly.
 */
class CBP2025TraceReader : public TraceReader
{
  public:
    CBP2025TraceReader(const std::string &trace_file, const std::string &name,
                      const std::string &map_mode = "hash",
                      uint64_t base_addr = 0x80000000ULL,
                      uint64_t map_size = 0x40000000ULL,
                      bool page_align = true,
                      statistics::Group *parent = nullptr);

    ~CBP2025TraceReader() override;

    bool init() override;
    bool reset() override;
    std::string getFormat() const override { return "cbp2025"; }
    TraceCheckpoint createCheckpoint() override;
    bool restoreCheckpoint(const TraceCheckpoint& checkpoint) override;
    bool seekToInstruction(uint64_t instrIndex) override;
    uint64_t getCurrentInstructionIndex() const override { return instructionIndex; }

  protected:
    size_t fillBuffer(size_t max_instructions) override;
    bool parseInstruction(TraceInstruction &instr) override;
    bool validateTraceFile() override;

  private:
    // CBP trace encoded instruction (macro-op)
    struct CBPInstr
    {
        uint64_t pc = 0;
        uint64_t nextPc = 0;
        uint64_t effAddr = 0;
        uint8_t memSize = 0;
        uint8_t baseUpd = 0;
        uint8_t hasRegOffset = 0;
        uint8_t type = 0;
        bool taken = false;
        uint8_t numInRegs = 0;
        std::vector<uint8_t> inRegs;
        uint8_t numOutRegs = 0;
        std::vector<uint8_t> outRegs;
    };

    enum class CBPInstClass : uint8_t
    {
        ALU = 0,
        LOAD = 1,
        STORE = 2,
        COND_BR = 3,
        UNCOND_DIR_BR = 4,
        UNCOND_IND_BR = 5,
        FP = 6,
        SLOW_ALU = 7,
        UNDEF = 8,
        CALL_DIR = 9,
        CALL_IND = 10,
        RET = 11,
    };

    // Stream helpers
    bool isGzip(const std::string &filename) const;
    bool readBytes(void *dst, size_t size);
    bool readCBPInstruction(CBPInstr &out);

    // Type helpers
    static bool isLoad(CBPInstClass t);
    static bool isStore(CBPInstClass t);
    static bool isMem(CBPInstClass t);
    static bool isBranch(CBPInstClass t);
    static bool isCondBranch(CBPInstClass t);
    static bool regIsInt(uint8_t reg);
    TraceInstruction::InstType mapInstType(CBPInstClass t) const;

    void extractRegisterDeps(const CBPInstr &raw, CBPInstClass cls,
                             TraceInstruction &instr);

    TraceReader::AddrMapConfig addrCfgSnapshot() const;

  private:
    TraceStream traceStream;
    TraceStream::Mode streamMode;
    uint64_t instructionIndex;

    std::string addrMapMode;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_CBP2025_TRACE_READER_HH__
