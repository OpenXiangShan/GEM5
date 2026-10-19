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

#ifndef __CPU_O3_TRACE_TRACERTL_TRACE_READER_HH__
#define __CPU_O3_TRACE_TRACERTL_TRACE_READER_HH__

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu/o3/trace/TraceReader.hh"

namespace gem5
{
namespace o3
{

/**
 * TRACERTL/NEMU trace reader ("tracertl" format).
 *
 * Record layout: 48 bytes, little-endian, no file header. The layout was
 * verified byte-by-byte against the gwt-v2-nemu-format-simpoint-interval
 * corpus (49 intervals, 12 Google workloads). Unlike the ChampSim/CBP
 * formats, each record carries the REAL RV64 instruction encoding, which
 * this reader publishes via TraceInstruction::setInstBits() so that
 * TraceFetch feeds the true opcode to the decoder (preserving the
 * instruction mix, functional units and latencies).
 *
 * Field caveats measured on that corpus (do NOT rely on the excluded
 * fields; see docs/tools/trace/trace_tools.md for details):
 *  - instr_pc_pa is always 0 (the transform never initializes it);
 *  - the memory physical address is stale garbage — only the va fields
 *    are consumed;
 *  - branch/jump/call immediates are intentionally zeroed ("consistent
 *    imm"): every control-flow target lives in the target field and is
 *    applied through the trace branch-truth override in IEW/decode;
 *  - exception != 0 marks a forced control-flow change to target
 *    (pc-discontinuity markers, usually carrying a NOP 0x00000033
 *    encoding, occasionally the real static encoding of that PC);
 *  - memory_size is a size CODE: 0=1B, 1=2B, 2=4B, 3=8B.
 */
class TraceRTLTraceReader : public TraceReader
{
  private:
    /** On-disk record: 48 bytes, little-endian. */
    struct TraceRTLInstr
    {
        uint64_t instr_pc_va;   ///< instruction virtual address (4-aligned)
        uint64_t instr_pc_pa;   ///< unused: always 0 in current dumps
        uint64_t mem_va;        ///< access vaddr (valid when memory_type != 0)
        uint64_t mem_pa;        ///< unused: stale garbage, never consumed
        uint64_t target;        ///< branch destination / forced-jump target
        uint32_t instr;         ///< real RV64 instruction encoding
        uint8_t  mem_type_size; ///< low nibble: memory_type (0=none 1=ld 2=st)
                                ///< high nibble: size code (0..3 -> 1/2/4/8 B)
        uint8_t  branch_type;   ///< BT_* below
        uint8_t  branch_taken;  ///< 0/1 conditional-branch direction
        uint8_t  exception;     ///< !=0: forced control-flow change to target
    };
    static_assert(sizeof(TraceRTLInstr) == 48,
                  "TRACERTL trace record must be exactly 48 bytes");

    /** branch_type values (measured; matches the TRACERTL TraceRTL
     *  semantics, verified 100% consistent against decoded opcodes). */
    static constexpr uint8_t BT_NONE            = 0;
    static constexpr uint8_t BT_COND_TAKEN      = 1;
    static constexpr uint8_t BT_COND_UNTAKEN    = 2;
    static constexpr uint8_t BT_DIRECT_JUMP     = 3;
    static constexpr uint8_t BT_INDIRECT_JUMP   = 4;
    static constexpr uint8_t BT_DIRECT_CALL     = 5;
    static constexpr uint8_t BT_INDIRECT_CALL   = 6;
    static constexpr uint8_t BT_RETURN          = 7;

    /** RISC-V major opcodes (bits [6:0]) used for classification and for
     *  the exception-record raw-bits suppression rule. */
    static constexpr uint32_t OP_BRANCH         = 0x63;
    static constexpr uint32_t OP_JALR           = 0x67;
    static constexpr uint32_t OP_JAL            = 0x6F;
    static constexpr uint32_t OP_FP_ARITH       = 0x53;
    static constexpr uint32_t OP_VECTOR         = 0x57; // RVV: commit side
                                                        // classifies isVector
                                                        // as FP too

    /** Unified trace stream (raw/zstd, plus gzip/xz for convenience). */
    TraceStream traceStream;
    TraceStream::Mode streamMode;
    /** Current position in file for debugging (raw only). */
    std::streampos currentPos;

    /** Current instruction index in the trace. */
    uint64_t instructionIndex;

    /** Checkpoints for rollback capability (mirrors ChampSimTraceReader:
     *  searched by seekToInstruction, cleared by reset()). */
    std::vector<TraceCheckpoint> checkpoints;

    /** One-shot diagnostic guards (warn at most once per reader). */
    bool rvcWarned = false;
    bool excWithBranchWarned = false;
    bool badBranchTypeWarned = false;
    /** Encodings seen per PC, to detect the "same PC, two encodings"
     *  artifact caused by pc_discontinuity NOP placeholders. */
    std::unordered_map<uint64_t, uint32_t> lastEncodingByPc;

  public:
    /**
     * Constructor
     * @param trace_file Path to TRACERTL trace file (raw or zstd)
     * @param name Name for statistics
     * @param map_mode Address mapping mode ("hash" or "linear")
     * @param base_addr Base address for mapping
     * @param map_size Size of mapping region
     * @param page_align Whether to align to page boundaries
     */
    TraceRTLTraceReader(const std::string &trace_file,
                        const std::string &name,
                        const std::string &map_mode = "hash",
                        uint64_t base_addr = 0x10000000UL,
                        uint64_t map_size = 0x40000000UL,
                        bool page_align = true,
                        statistics::Group *parent = nullptr);

    /** Destructor */
    ~TraceRTLTraceReader();

    bool init() override;
    bool reset() override;
    std::string getFormat() const override { return "tracertl"; }

    /** Dual-encoding diagnostic (R2 artifact): how many records re-used a
     *  PC previously seen with a different encoding. Public surface for
     *  the anchored unit tests. */
    uint64_t getMixedEncodingPcCount() const { return stats.mixedEncodingPc.value(); }

    TraceCheckpoint createCheckpoint() override;
    bool restoreCheckpoint(const TraceCheckpoint& checkpoint) override;
    bool seekToInstruction(uint64_t instrIndex) override;
    uint64_t getCurrentInstructionIndex() const override;
    bool supportsFastRandomSeek() const override
    { return streamMode == TraceStream::Mode::Raw; }

    /**
     * Configure address mapping parameters after construction
     */
    void setAddressMapping(uint64_t base, uint64_t size, const std::string &mode,
                           bool pageAlign);

  protected:
    size_t fillBuffer(size_t max_instructions) override;
    bool parseInstruction(TraceInstruction &instr) override;
    bool validateTraceFile() override;

  private:
    /** Read one 48-byte record from the stream. */
    bool readTraceRTLInstruction(TraceRTLInstr &rec);

    /** Map a raw record onto the unified TraceInstruction. */
    void convertInstruction(const TraceRTLInstr &rec,
                            TraceInstruction &trace_instr);

    /** InstType derivation; priority: branch metadata first (tracer ground
     *  truth), then memory metadata, then the encoded opcode. Branch
     *  records always win the commit-side comparison through the trace
     *  hints (hasTraceBranchInfo), and memory-before-opcode mirrors the
     *  commit-side classifier. */
    TraceInstruction::InstType determineInstType(const TraceRTLInstr &rec);

    /** Detect the container compression from the file's magic bytes
     *  (zstd/gzip/xz; anything else is treated as raw). */
    static TraceStream::Mode detectCompression(const std::string &filename);

    /** Decide which encoding to publish for this record. RVC records
     *  suppress the raw bits (synthetic fallback); exception-marker
     *  records whose encoding is a control-flow opcode are sanitized to
     *  a real NOP so the decoded classification matches the trace-side
     *  type without entering the synthetic path. */
    uint32_t sanitizeEncoding(const TraceRTLInstr &rec, bool &publish);
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACERTL_TRACE_READER_HH__
