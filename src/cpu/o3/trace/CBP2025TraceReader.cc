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

#include "cpu/o3/trace/CBP2025TraceReader.hh"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "arch/riscv/page_size.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "debug/TraceReader.hh"

namespace gem5
{
namespace o3
{

CBP2025TraceReader::CBP2025TraceReader(const std::string &trace_file,
                                       const std::string &name,
                                       const std::string &map_mode,
                                       uint64_t base_addr,
                                       uint64_t map_size,
                                       bool page_align,
                                       statistics::Group *parent)
  : TraceReader(trace_file, name, parent),
    streamMode(TraceStream::Mode::Raw),
    instructionIndex(0)
{
    if (isGzip(trace_file)) {
        streamMode = TraceStream::Mode::Gzip;
    } else {
        streamMode = TraceStream::Mode::Raw;
    }
    setAddrMapConfig({base_addr, map_size, map_mode, page_align});
}

CBP2025TraceReader::~CBP2025TraceReader()
{
    traceStream.close();
}

TraceReader::AddrMapConfig
CBP2025TraceReader::addrCfgSnapshot() const
{
    return getAddrMapConfig();
}

bool
CBP2025TraceReader::isGzip(const std::string &filename) const
{
    if (filename.size() >= 7 &&
        filename.substr(filename.size() - 7) == ".tar.gz") {
        return true;
    }
    return filename.size() >= 3 &&
           filename.substr(filename.size() - 3) == ".gz";
}

bool
CBP2025TraceReader::init()
{
    DPRINTF(TraceReader, "CBP2025TraceReader::init (initialized=%d)\n", initialized);
    if (initialized) {
        return true;
    }

    if (!validateTraceFile()) {
        DPRINTF(TraceReader, "CBP2025TraceReader::init validation failed\n");
        return false;
    }

    if (!openTraceStream(traceStream, streamMode, nullptr)) {
        DPRINTF(TraceReader, "CBP2025TraceReader::init failed to open trace stream\n");
        return false;
    }

    resetBufferState();
    instructionIndex = 0;
    initialized = true;

    return true;
}

bool
CBP2025TraceReader::reset()
{
    DPRINTF(TraceReader, "CBP2025TraceReader::reset\n");
    dumpInstrBuffer("before_reset");

    if (!initialized) {
        return init();
    }

    if (!reopenTraceStream(traceStream, streamMode, nullptr)) {
        return false;
    }

    resetBufferState();
    instructionIndex = 0;

    dumpInstrBuffer("after_reset");
    return true;
}

bool
CBP2025TraceReader::validateTraceFile()
{
    return validateTraceFileSize(traceFile,
                                 static_cast<std::streamsize>(sizeof(CBPInstr)));
}

bool
CBP2025TraceReader::readBytes(void *dst, size_t size)
{
    if (!traceStream.readExact(dst, size)) {
        eofReached = traceStream.eof();
        return false;
    }
    return true;
}

bool
CBP2025TraceReader::readCBPInstruction(CBPInstr &out)
{
    out = CBPInstr();

    if (eofReached || !traceStream.isOpen()) {
        return false;
    }

    if (!readBytes(&out.pc, sizeof(out.pc))) {
        eofReached = true;
        return false;
    }

    out.nextPc = out.pc + 4;

    if (!readBytes(&out.type, sizeof(out.type))) {
        eofReached = true;
        return false;
    }

    const CBPInstClass cls = static_cast<CBPInstClass>(out.type);

    if (isMem(cls)) {
        if (!readBytes(&out.effAddr, sizeof(out.effAddr))) return false;
        if (!readBytes(&out.memSize, sizeof(out.memSize))) return false;
        if (!readBytes(&out.baseUpd, sizeof(out.baseUpd))) return false;
        if (isStore(cls)) {
            if (!readBytes(&out.hasRegOffset, sizeof(out.hasRegOffset))) return false;
        }
    }

    if (isBranch(cls)) {
        uint8_t taken = 0;
        if (!readBytes(&taken, sizeof(taken))) return false;
        out.taken = taken != 0;
        if (!isCondBranch(cls)) {
            // trace writer asserts taken for uncond
            out.taken = true;
        }
        if (out.taken) {
            if (!readBytes(&out.nextPc, sizeof(out.nextPc))) return false;
        }
    }

    if (!readBytes(&out.numInRegs, sizeof(out.numInRegs))) return false;
    out.inRegs.resize(out.numInRegs);
    for (uint8_t i = 0; i < out.numInRegs; ++i) {
        if (!readBytes(&out.inRegs[i], sizeof(uint8_t))) return false;
    }

    if (!readBytes(&out.numOutRegs, sizeof(out.numOutRegs))) return false;
    out.outRegs.resize(out.numOutRegs);
    for (uint8_t i = 0; i < out.numOutRegs; ++i) {
        if (!readBytes(&out.outRegs[i], sizeof(uint8_t))) return false;
    }

    // Consume output values to advance the stream; we don't yet model values.
    for (uint8_t i = 0; i < out.numOutRegs; ++i) {
        uint64_t val = 0;
        if (!readBytes(&val, sizeof(val))) return false;
        if (!regIsInt(out.outRegs[i])) {
            // upper 64 bits
            uint64_t val_hi = 0;
            if (!readBytes(&val_hi, sizeof(val_hi))) return false;
        }
    }

    return true;
}

TraceInstruction::InstType
CBP2025TraceReader::mapInstType(CBPInstClass t) const
{
    using TI = TraceInstruction::InstType;
    switch (t) {
      case CBPInstClass::ALU: return TI::ALU;
      case CBPInstClass::LOAD: return TI::LOAD;
      case CBPInstClass::STORE: return TI::STORE;
      case CBPInstClass::COND_BR: return TI::COND_BRANCH;
      case CBPInstClass::UNCOND_DIR_BR: return TI::UNCOND_DIRECT_BRANCH;
      case CBPInstClass::UNCOND_IND_BR: return TI::UNCOND_INDIRECT_BRANCH;
      case CBPInstClass::FP: return TI::FP;
      // XiangShan front-end/commit currently does not distinguish SLOW_ALU;
      // map it to ALU to avoid InstType mismatch.
      case CBPInstClass::SLOW_ALU: return TI::ALU;
      case CBPInstClass::CALL_DIR: return TI::CALL_DIRECT;
      case CBPInstClass::CALL_IND: return TI::CALL_INDIRECT;
      case CBPInstClass::RET: return TI::RETURN;
      case CBPInstClass::UNDEF:
      default: return TI::UNDEFINED;
    }
}

bool CBP2025TraceReader::isLoad(CBPInstClass t) { return t == CBPInstClass::LOAD; }
bool CBP2025TraceReader::isStore(CBPInstClass t) { return t == CBPInstClass::STORE; }
bool CBP2025TraceReader::isMem(CBPInstClass t) { return isLoad(t) || isStore(t); }
bool CBP2025TraceReader::isBranch(CBPInstClass t)
{
    return t == CBPInstClass::COND_BR || t == CBPInstClass::UNCOND_DIR_BR ||
           t == CBPInstClass::UNCOND_IND_BR || t == CBPInstClass::CALL_DIR ||
           t == CBPInstClass::CALL_IND || t == CBPInstClass::RET;
}
bool CBP2025TraceReader::isCondBranch(CBPInstClass t) { return t == CBPInstClass::COND_BR; }
bool CBP2025TraceReader::regIsInt(uint8_t reg)
{
    // mirror cbp2025: 0-31 int, 31=SP, 64 flags, 65 zero; >=32 & <64 are SIMD
    return reg < 32 || reg == 64 || reg == 65;
}

bool
CBP2025TraceReader::parseInstruction(TraceInstruction &instr)
{
    CBPInstr raw;
    if (!readCBPInstruction(raw)) {
        return false;
    }

    instr.reset();
    const CBPInstClass cls = static_cast<CBPInstClass>(raw.type);
    const auto cfg = getAddrMapConfig();

    // Map PC/nextPc through VA mapping to keep FS happy.
    const Addr mapped_pc = mapTracePcToVirtual(raw.pc, cfg);
    instr.setPC(mapped_pc);
    instr.setSeqNum(getNextSeqNum());
    instr.setValid(true);
    instr.setInstSizeBytes(4);
    instr.setInstType(mapInstType(cls));
    instr.setPiece(0);

    if (isBranch(cls)) {
        instr.setBranchTaken(raw.taken);
        if (raw.taken) {
            instr.setBranchTarget(mapTracePcToVirtual(raw.nextPc, cfg));
        }
    }
    if (!isBranch(cls) || (isCondBranch(cls) && !raw.taken)) {
        const Addr mapped_next = mapTracePcToVirtual(raw.nextPc, cfg);
        if (mapped_next >= mapped_pc) {
            const uint64_t delta = mapped_next - mapped_pc;
            if (delta > 0 && delta <= 8) {
                instr.setInstSizeBytes(static_cast<uint8_t>(delta));
            }
        }
    }

    if (isLoad(cls)) {
        instr.addLoadAddress(mapTraceMemToVirtual(raw.effAddr, cfg), raw.memSize);
    } else if (isStore(cls)) {
        instr.addStoreAddress(mapTraceMemToVirtual(raw.effAddr, cfg), raw.memSize);
    }

    extractRegisterDeps(raw, cls, instr);

    instructionIndex++;
    return true;
}

void
CBP2025TraceReader::extractRegisterDeps(const CBPInstr &raw, CBPInstClass cls,
                                        TraceInstruction &instr)
{
    auto mapIntReg = [&](uint8_t r) -> uint8_t {
        // CBP2025: 31=SP, 30=LR, 64=flags, 65=zero, 32-63=SIMD/FP
        if (r == 0 || r == 65) return 0;
        if (r == 64) return 0;   // flags best-effort to x0
        if (r == 31) return 2;   // SP
        if (r == 30) return 1;   // LR (AArch64 x30) -> RISC-V RA(x1)
        // Some ARM-origin CALL_IND traces carry x5 (not link reg). Map to a
        // benign GPR (x28=t3) to avoid alt-RA semantics while retaining deps.
        if (cls == CBPInstClass::CALL_IND && r == 5) {
            constexpr uint8_t harmless = 28;
            DPRINTF(TraceReader,
                    "[TRACE-ENC] CBP CALL_IND reg %u mapped to x%u to avoid alt-RA\n",
                    r, harmless);
            return harmless;
        }
        // IP not defined as GPR; keep 0
        if (r == 26) return 0;
        if (r < 32)  return r;   // general purpose
        DPRINTF(TraceReader, "[TRACE-ENC] CBP reg %u unmapped -> x0\n", r);
        return 0;
    };
    auto mapFpReg = [](uint8_t r) -> uint8_t {
        if (r >= 32 && r < 64) return static_cast<uint8_t>(r - 32); // f0..f31
        return 0;
    };

    for (auto reg : raw.inRegs) {
        auto mapped = regIsInt(reg) ? mapIntReg(reg) : mapFpReg(reg);
        instr.addSrcReg(mapped);
    }
    for (auto reg : raw.outRegs) {
        auto mapped = regIsInt(reg) ? mapIntReg(reg) : mapFpReg(reg);
        instr.addDstReg(mapped);
    }
}

size_t
CBP2025TraceReader::fillBuffer(size_t max_instructions)
{
    if (eofReached || !traceStream.isOpen()) {
        return 0;
    }

    PendingResolveConfig resolveCfg;
    resolveCfg.fixTakenTargetMismatch = true;
    return drainPendingToBuffer(max_instructions, resolveCfg,
                                /*markLastInTrace=*/true);
}

TraceReader::TraceCheckpoint
CBP2025TraceReader::createCheckpoint()
{
    // For compressed traces we cannot reliably seek; allow checkpointing only
    // for uncompressed files by saving the file offset.
    return buildCheckpoint(instructionIndex, currentSeqNum, eofReached,
                           instrBuffer, hasPendingInstr, pendingInstr,
                           traceStream, streamMode,
                           /*allowCompressed=*/false);
}

bool
CBP2025TraceReader::restoreCheckpoint(const TraceCheckpoint& checkpoint)
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

    if (!restoreCheckpointCommon(checkpoint, traceStream, streamMode,
                                 /*allowCompressedRewind=*/false, ff,
                                 instructionIndex)) {
        return false;
    }
    resetHistoryWindow(instructionIndex + 1);
    return true;
}

bool
CBP2025TraceReader::seekToInstruction(uint64_t instrIndex)
{
    // For now, only support soft-seek via history window (base class).
    return softSeekToInstruction(instrIndex);
}

} // namespace o3
} // namespace gem5
