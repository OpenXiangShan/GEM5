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

#include "cpu/o3/trace/TraceRTLTraceReader.hh"

#include <cstdio>
#include <fstream>

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/TraceReader.hh"

namespace gem5
{
namespace o3
{

TraceRTLTraceReader::TraceRTLTraceReader(const std::string &trace_file,
                                         const std::string &name,
                                         const std::string &map_mode,
                                         uint64_t base_addr,
                                         uint64_t map_size,
                                         bool page_align,
                                         statistics::Group *parent)
    : TraceReader(trace_file, name, parent), streamMode(TraceStream::Mode::Raw),
      currentPos(0), instructionIndex(0)
{
    // Container detection is magic-byte based: the corpus ships the same
    // record stream both as bare files and zstd-compressed files, and the
    // suffix alone is not trustworthy.
    streamMode = detectCompression(trace_file);
    hasPendingInstr = false;
    setAddrMapConfig({base_addr, map_size, map_mode, page_align});
}

TraceRTLTraceReader::~TraceRTLTraceReader()
{
    traceStream.close();
}

bool
TraceRTLTraceReader::init()
{
    DPRINTF(TraceReader, "init: Initializing TRACERTL trace reader, "
                         "initialized=%d\n", initialized);

    if (initialized) {
        return true;
    }

    if (!validateTraceFile()) {
        DPRINTF(TraceReader, "init: TRACERTL trace file validation failed\n");
        return false;
    }

    const bool compressedLike = streamMode != TraceStream::Mode::Raw;
    DPRINTF(TraceReader, "init: Opening trace file (%s): %s\n",
            compressedLike ? "compressed" : "raw", traceFile.c_str());
    if (!openTraceStream(traceStream, streamMode, &currentPos)) {
        DPRINTF(TraceReader, "init: Failed to open trace stream\n");
        return false;
    }

    resetBufferState();
    instructionIndex = 0;
    initialized = true;

    return true;
}

bool
TraceRTLTraceReader::reset()
{
    DPRINTF(TraceReader, "reset: Resetting trace reader (mode=%d)\n",
            static_cast<int>(streamMode));

    dumpInstrBuffer("before_reset");

    if (!reopenTraceStream(traceStream, streamMode, &currentPos)) {
        DPRINTF(TraceReader, "reset: Failed to reopen trace stream\n");
        return false;
    }

    resetBufferState();
    instructionIndex = 0;
    checkpoints.clear();
    lastEncodingByPc.clear();

    DPRINTF(TraceReader, "reset: Reset completed, eofReached=%d, buffer "
                         "size=%lu\n", eofReached, instrBuffer.size());
    dumpInstrBuffer("after_reset");
    return true;
}

bool
TraceRTLTraceReader::validateTraceFile()
{
    if (streamMode == TraceStream::Mode::Raw) {
        // Raw records must tile the file exactly.
        std::ifstream file(traceFile, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return false;
        }
        const std::streamsize size = file.tellg();
        if (size < static_cast<std::streamsize>(sizeof(TraceRTLInstr))) {
            return false;
        }
        return size % static_cast<std::streamsize>(sizeof(TraceRTLInstr)) == 0;
    }

    // Compressed containers: the magic was confirmed during detection, and
    // integrity is enforced by the decompressor — a corrupt or truncated
    // stream fails readExact and drains as EOF, mirroring the existing
    // gzip/xz readers' behavior.
    return true;
}

size_t
TraceRTLTraceReader::fillBuffer(size_t max_instructions)
{
    if (eofReached || !traceStream.isOpen()) {
        DPRINTF(TraceReader, "fillBuffer: Cannot read - eofReached=%d, "
                             "stream_open=%d\n",
                eofReached, traceStream.isOpen());
        return 0;
    }

    // Unlike ChampSim, TRACERTL records carry the authoritative branch
    // target from the tracer, so the look-ahead reconciliation must not
    // rewrite trace truth (fixTakenTargetMismatch stays false). The base
    // class fallbacks in reconcilePendingWithNext are no-ops for
    // well-formed records: control flow is explicit (branch target or
    // forced-jump marker), and sequential records sit exactly 4 bytes
    // apart in PC space.
    PendingResolveConfig resolveCfg;
    resolveCfg.fixTakenTargetMismatch = false;
    const size_t pushed = drainPendingToBuffer(max_instructions, resolveCfg,
                                               /*markLastInTrace=*/true);

    DPRINTF(TraceReader, "fillBuffer: Completed, pushed %lu instructions\n",
            pushed);
    return pushed;
}

bool
TraceRTLTraceReader::parseInstruction(TraceInstruction &instr)
{
    TraceRTLInstr rec;

    if (!readTraceRTLInstruction(rec)) {
        DPRINTF(TraceReader, "parseInstruction: record read failed\n");
        return false;
    }

    convertInstruction(rec, instr);
    instructionIndex++;
    return true;
}

bool
TraceRTLTraceReader::readTraceRTLInstruction(TraceRTLInstr &rec)
{
    if (eofReached || !traceStream.isOpen()) {
        DPRINTF(TraceReader, "readTraceRTLInstruction: Cannot read - "
                             "eofReached=%d, stream_open=%d\n",
                eofReached, traceStream.isOpen());
        return false;
    }

    const bool ok = traceStream.readExact(reinterpret_cast<char*>(&rec),
                                          sizeof(rec));
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
TraceRTLTraceReader::convertInstruction(const TraceRTLInstr &rec,
                                        TraceInstruction &trace_instr)
{
    trace_instr.reset();
    const auto cfg = getAddrMapConfig();

    const uint8_t mem_type = rec.mem_type_size & 0xF;
    const uint8_t size_code = rec.mem_type_size >> 4;

    trace_instr.setPC(mapTracePcToVirtual(rec.instr_pc_va, cfg));
    trace_instr.setSeqNum(getNextSeqNum());
    trace_instr.setInstSizeBytes(4);
    trace_instr.setValid(true);

    const TraceInstruction::InstType type = determineInstType(rec);
    trace_instr.setInstType(type);

    // Real instruction encoding from the trace: authoritative unless this
    // record is a suppression/sanitization case (RVC / forced-jump markers,
    // see sanitizeEncoding).
    {
        bool publish = false;
        const uint32_t bits = sanitizeEncoding(rec, publish);
        if (publish) {
            trace_instr.setInstBits(bits);
        }
    }

    // Branch metadata (branch_type != 0): the target field is the only
    // reliable source of the destination because the tracer zeroes the
    // instruction immediates ("consistent imm").
    if (rec.branch_type != BT_NONE) {
        trace_instr.setBranchTaken(rec.branch_taken != 0);
        trace_instr.setBranchTarget(mapTracePcToVirtual(rec.target, cfg));
    }

    // Forced control-flow markers (exception != 0): redirect through the
    // commit-side TraceCtrlFlowFault channel, the same one CBP2025 uses
    // for traps. When a record carries BOTH a branch_type and an
    // exception flag (never observed in the corpus), the branch channel
    // already encodes the true outcome; keep it and warn once.
    if (rec.exception != 0) {
        if (rec.branch_type != BT_NONE) {
            if (!excWithBranchWarned) {
                warn("TraceRTLTraceReader: record with both exception != 0 "
                     "and branch_type != %u at PC 0x%llx; keeping branch "
                     "semantics, ignoring forced-jump flag\n",
                     BT_NONE, (unsigned long long)rec.instr_pc_va);
                excWithBranchWarned = true;
            }
        } else {
            trace_instr.setCtrlFlowChange(true);
            trace_instr.setCtrlFlowTarget(mapTracePcToVirtual(rec.target, cfg));
        }
    }

    // Memory operations: only when the tracer marked the record as one
    // (memory_type != 0). The physical-address field is stale garbage and
    // is deliberately ignored. Values are not recorded by this format; the
    // same deterministic simulation policy as the ChampSim reader is used
    // so that cross-format runs share one data-flow noise model.
    if (mem_type == 1 || mem_type == 2) {
        const uint32_t bytes = (size_code <= 3) ? (1u << size_code) : 0;
        const Addr mapped = mapTraceMemToVirtual(rec.mem_va, cfg);
        if (mem_type == 1) {
            trace_instr.addLoadAddress(mapped, bytes);
            trace_instr.addLoadValue(rec.mem_va ^ 0xDEADBEEFULL);
        } else {
            trace_instr.addStoreAddress(mapped, bytes);
            trace_instr.addStoreValue((rec.mem_va ^ rec.instr_pc_va) +
                                      0x12345678ULL);
        }
    }

    // Dual-encoding diagnostic (R2): pc_discontinuity markers emit a NOP
    // placeholder first and the real static encoding later for the same
    // PC. Count the transitions so batch runs can quantify the artifact
    // without debug flags.
    {
        auto it = lastEncodingByPc.find(rec.instr_pc_va);
        if (it == lastEncodingByPc.end()) {
            lastEncodingByPc.emplace(rec.instr_pc_va, rec.instr);
        } else if (it->second != rec.instr) {
            ++stats.mixedEncodingPc;
            DPRINTF(TraceReader,
                    "convertInstruction: PC 0x%llx re-encoded: 0x%x -> 0x%x\n",
                    (unsigned long long)rec.instr_pc_va, it->second, rec.instr);
            it->second = rec.instr;
        }
    }
}

TraceInstruction::InstType
TraceRTLTraceReader::determineInstType(const TraceRTLInstr &rec)
{
    // Branch metadata is ground truth from the tracer. The commit-side
    // classifier honors the trace hints via hasTraceBranchInfo(), so any
    // type chosen here among the branch kinds matches at difftest time.
    switch (rec.branch_type) {
      case BT_COND_TAKEN:
      case BT_COND_UNTAKEN:
        return TraceInstruction::InstType::COND_BRANCH;
      case BT_DIRECT_JUMP:
        return TraceInstruction::InstType::UNCOND_DIRECT_BRANCH;
      case BT_INDIRECT_JUMP:
        return TraceInstruction::InstType::UNCOND_INDIRECT_BRANCH;
      case BT_DIRECT_CALL:
        return TraceInstruction::InstType::CALL_DIRECT;
      case BT_INDIRECT_CALL:
        return TraceInstruction::InstType::CALL_INDIRECT;
      case BT_RETURN:
        return TraceInstruction::InstType::RETURN;
      case BT_NONE:
        break;
      default:
        if (!badBranchTypeWarned) {
            warn("TraceRTLTraceReader: unknown branch_type %u at PC 0x%llx; "
                 "classifying from memory/opcode metadata\n",
                 rec.branch_type, (unsigned long long)rec.instr_pc_va);
            badBranchTypeWarned = true;
        }
        break;
    }

    // Memory first (FP loads/stores count as LOAD/STORE too — matches the
    // commit-side classifier, which checks isLoad()/isStore() before
    // anything else).
    if ((rec.mem_type_size & 0xF) == 1) {
        return TraceInstruction::InstType::LOAD;
    }
    if ((rec.mem_type_size & 0xF) == 2) {
        return TraceInstruction::InstType::STORE;
    }

    // Then the encoded opcode: FP arithmetic and vector instructions
    // classify as FP (the commit-side classifier treats isFloating() and
    // isVector() identically).
    const uint32_t opcode = rec.instr & 0x7F;
    if (opcode == OP_FP_ARITH || opcode == OP_VECTOR) {
        return TraceInstruction::InstType::FP;
    }

    return TraceInstruction::InstType::ALU;
}

uint32_t
TraceRTLTraceReader::sanitizeEncoding(const TraceRTLInstr &rec, bool &publish)
{
    // (a) RVC encodings: this format tiles the stream with fixed 4-byte
    // records, so a 16-bit encoding leaves undefined bytes in the
    // decoder's 4-byte slot. v1 keeps the 4-byte stride and falls back to
    // synthetic encoding (not observed in the gwt-v2 corpus: 0/132M
    // records).
    if ((rec.instr & 0x3) != 0x3) {
        if (!rvcWarned) {
            warn("TraceRTLTraceReader: compressed (RVC) encoding 0x%x at PC "
                 "0x%llx: v1 supports 32-bit encodings only, falling back to "
                 "synthetic encoding (4-byte stride preserved)\n",
                 rec.instr, (unsigned long long)rec.instr_pc_va);
            rvcWarned = true;
        }
        publish = false;
        return 0;
    }

    // (b) Forced-jump markers whose recorded encoding is a control-flow
    // opcode while the metadata says non-branch: the commit-side
    // classifier derives the type from the decoded static instruction (a
    // branch) and would panic against the trace-side type (ALU). Publish
    // a sanitized NOP encoding instead: the synthetic path is bypassed
    // entirely (no dependence on instruction-size reconciliation), the
    // decoded type stays ALU, and the PC redirect still rides the
    // ctrl-flow-change fault channel.
    if (rec.exception != 0 && rec.branch_type == BT_NONE) {
        const uint32_t opcode = rec.instr & 0x7F;
        if (opcode == OP_BRANCH || opcode == OP_JALR || opcode == OP_JAL) {
            publish = true;
            return 0x00000013; // addi x0, x0, 0 (NOP)
        }
    }
    publish = true;
    return rec.instr;
}

TraceReader::TraceStream::Mode
TraceRTLTraceReader::detectCompression(const std::string &filename)
{
    FILE *f = std::fopen(filename.c_str(), "rb");
    if (!f) {
        return TraceStream::Mode::Raw;
    }
    unsigned char magic[6] = {0, 0, 0, 0, 0, 0};
    const size_t got = std::fread(magic, 1, sizeof(magic), f);
    std::fclose(f);

    if (got >= 4 && magic[0] == 0x28 && magic[1] == 0xB5 &&
        magic[2] == 0x2F && magic[3] == 0xFD) {
        return TraceStream::Mode::Zstd;
    }
    if (got >= 2 && magic[0] == 0x1F && magic[1] == 0x8B) {
        return TraceStream::Mode::Gzip;
    }
    if (got >= 6 && magic[0] == 0xFD && magic[1] == 0x37 &&
        magic[2] == 0x7A && magic[3] == 0x58 && magic[4] == 0x5A &&
        magic[5] == 0x00) {
        return TraceStream::Mode::Xz;
    }
    return TraceStream::Mode::Raw;
}

TraceReader::TraceCheckpoint
TraceRTLTraceReader::createCheckpoint()
{
    TraceCheckpoint checkpoint = buildCheckpoint(
        instructionIndex, currentSeqNum, eofReached, instrBuffer,
        hasPendingInstr, pendingInstr, traceStream, streamMode,
        /*allowCompressed=*/true);
    DPRINTF(TraceReader,
            "createCheckpoint: hasPendingInstr=%d, pending_sn=%llu, "
            "pending_pc=0x%llx\n",
            hasPendingInstr,
            (unsigned long long)(hasPendingInstr ? pendingInstr.getSeqNum()
                                                  : 0ULL),
            (unsigned long long)(hasPendingInstr ? pendingInstr.getPC()
                                                 : 0ULL));
    return checkpoint;
}

bool
TraceRTLTraceReader::restoreCheckpoint(const TraceCheckpoint& checkpoint)
{
    auto ff = [this](uint64_t targetIndex) -> bool {
        resetBufferState();
        instructionIndex = 0;
        // Replayed records re-populate the dual-encoding diagnostic map;
        // stale "future" entries from before the rewind would skew it.
        lastEncodingByPc.clear();
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
TraceRTLTraceReader::seekToInstruction(uint64_t instrIndex)
{
    DPRINTF(TraceReader, "seekToInstruction: Seeking to instruction %lu "
                         "(current=%lu)\n", instrIndex, instructionIndex);

    // Support a 0 index as "beginning of trace" sentinel (seek(0) -> before
    // the first instruction), matching ChampSimTraceReader semantics.
    if (instrIndex == 0) {
        if (!reset()) {
            return false;
        }
        DPRINTF(TraceReader, "seekToInstruction: Reset to beginning "
                             "(index=0)\n");
        return true;
    }

    TraceCheckpoint bestCheckpoint;
    bool foundCheckpoint = false;
    for (const auto& cp : checkpoints) {
        if (cp.valid && cp.instructionIndex <= instrIndex) {
            if (!foundCheckpoint ||
                cp.instructionIndex > bestCheckpoint.instructionIndex) {
                bestCheckpoint = cp;
                foundCheckpoint = true;
            }
        }
    }

    if (foundCheckpoint) {
        DPRINTF(TraceReader, "seekToInstruction: Using checkpoint at "
                             "instrIndex=%lu\n",
                bestCheckpoint.instructionIndex);
        if (!restoreCheckpoint(bestCheckpoint)) {
            return false;
        }
    } else {
        DPRINTF(TraceReader, "seekToInstruction: No checkpoint found, "
                             "resetting to beginning\n");
        if (!reset()) {
            return false;
        }
    }

    // Read forward to the exact target (parse only; NOT enqueued).
    while (instructionIndex < instrIndex && !eofReached) {
        TraceInstruction dummy;
        if (!parseInstruction(dummy)) {
            DPRINTF(TraceReader, "seekToInstruction: Failed to read to "
                                 "target instruction\n");
            return false;
        }
    }

    DPRINTF(TraceReader, "seekToInstruction: Successfully sought to "
                         "instruction %lu\n", instructionIndex);
    return instructionIndex == instrIndex;
}

uint64_t
TraceRTLTraceReader::getCurrentInstructionIndex() const
{
    return instructionIndex;
}

void
TraceRTLTraceReader::setAddressMapping(uint64_t base, uint64_t size,
                                       const std::string &mode, bool pageAlign)
{
    setAddrMapConfig({base, size, mode, pageAlign});

    DPRINTF(TraceReader,
            "Address mapping configured: base=0x%llx, size=0x%llx, "
            "mode=%s, pageAlign=%s\n",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(size), mode.c_str(),
            pageAlign ? "true" : "false");
}

} // namespace o3
} // namespace gem5
