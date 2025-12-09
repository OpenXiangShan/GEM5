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
    hasPendingInstr(false),
    instructionIndex(0),
    addrMapMode(map_mode),
    addrMapBase(base_addr),
    addrMapSize(map_size),
    addrPageAlign(page_align)
{
    if (isGzip(trace_file)) {
        streamMode = TraceStream::Mode::Gzip;
    } else {
        streamMode = TraceStream::Mode::Raw;
    }
}

CBP2025TraceReader::~CBP2025TraceReader()
{
    traceStream.close();
}

bool
CBP2025TraceReader::isGzip(const std::string &filename) const
{
    return filename.size() > 3 &&
           (filename.substr(filename.size() - 3) == ".gz" ||
            filename.substr(filename.size() - 7) == ".tar.gz");
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

    if (!traceStream.open(traceFile, streamMode)) {
        DPRINTF(TraceReader, "CBP2025TraceReader::init failed to open trace stream\n");
        return false;
    }

    eofReached = false;
    initialized = true;
    currentSeqNum = 0;
    instructionIndex = 0;
    hasPendingInstr = false;

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

    if (!traceStream.reopen()) {
        return false;
    }

    eofReached = false;
    currentSeqNum = 0;
    instructionIndex = 0;
    hasPendingInstr = false;
    while (!instrBuffer.empty()) {
        instrBuffer.pop();
    }
    historyWindow.clear();
    historyStartIndex = 1;
    nextLogicalIndex = 1;
    replayActive = false;
    replayIndex = 0;

    dumpInstrBuffer("after_reset");
    return true;
}

bool
CBP2025TraceReader::validateTraceFile()
{
    std::ifstream f(traceFile, std::ios::binary);
    if (!f.is_open()) {
        return false;
    }
    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    return sz > 0;
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

    // Map PC/nextPc through VA mapping to keep FS happy.
    const Addr mapped_pc = mapTracePcToVirtual(raw.pc);
    instr.setPC(mapped_pc);
    instr.setSeqNum(getNextSeqNum());
    instr.setValid(true);
    instr.setInstSizeBytes(4);
    instr.setInstType(mapInstType(cls));
    instr.setPiece(0);

    if (isBranch(cls)) {
        instr.setBranchTaken(raw.taken);
        if (raw.taken) {
            instr.setBranchTarget(mapTracePcToVirtual(raw.nextPc));
        }
    }
    if (!isBranch(cls) || (isCondBranch(cls) && !raw.taken)) {
        const Addr mapped_next = mapTracePcToVirtual(raw.nextPc);
        if (mapped_next >= mapped_pc) {
            const uint64_t delta = mapped_next - mapped_pc;
            if (delta > 0 && delta <= 8) {
                instr.setInstSizeBytes(static_cast<uint8_t>(delta));
            }
        }
    }

    if (isLoad(cls)) {
        instr.addLoadAddress(mapTraceMemToVirtual(raw.effAddr), raw.memSize);
    } else if (isStore(cls)) {
        instr.addStoreAddress(mapTraceMemToVirtual(raw.effAddr), raw.memSize);
    }

    extractRegisterDeps(raw, cls, instr);

    instructionIndex++;
    return true;
}

size_t
CBP2025TraceReader::fillBuffer(size_t max_instructions)
{
    if (eofReached || !traceStream.isOpen()) {
        return 0;
    }

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

                // 健壮性兜底：非分支且下一条 PC 非 2B 对齐时，按顺序流 pc+4 处理，
                // 避免异常 PC 打乱后续长度推断 / fallthrough。
                if (!pendingInstr.isAnyBranch() && (next_pc & 0x1)) {
                    Addr corrected_pc = curr_pc + 4;
                    current.setPC(corrected_pc);
                    next_pc = corrected_pc;
                }
                if (!pending_taken_branch && next_pc >= curr_pc) {
                    const uint64_t delta = next_pc - curr_pc;
                    // 捕获 2B/4B 等小步进以供 Fetch 生成正确指令长度。
                    if (delta > 0 && delta <= 8) {
                        pendingInstr.setInstSizeBytes(
                            static_cast<uint8_t>(delta));
                    }
                }

                if (pendingInstr.isAnyBranch()) {
                    if (pending_taken_branch &&
                        pendingInstr.getHasBranchTarget() &&
                        pendingInstr.getBranchTarget() != next_pc) {
                        // Taken branch target与下一条PC不符：视为异常式跳转，
                        // 纠正目标到下一条PC并标记 ctrlFlowChange。
                        DPRINTF(TraceReader,
                                "CBP fillBuffer: mark taken branch ctrl-flow "
                                "change pc=0x%lx tgt_fix=0x%lx nextPC=0x%lx\n",
                                pendingInstr.getPC(),
                                pendingInstr.getBranchTarget(), next_pc);
                        pendingInstr.setBranchTarget(next_pc);
                        pendingInstr.setCtrlFlowChange(true);
                        pendingInstr.setCtrlFlowTarget(next_pc);
                    } else if (!pending_taken_branch &&
                               !isApproxFallthrough(pendingInstr.getPC(),
                                                    next_pc)) {
                        DPRINTF(TraceReader,
                                "CBP fillBuffer: mark branch-not-taken "
                                "ctrl-flow change pc=0x%lx -> nextPC=0x%lx\n",
                                pendingInstr.getPC(), next_pc);
                        pendingInstr.setCtrlFlowChange(true);
                        pendingInstr.setCtrlFlowTarget(next_pc);
                    }
                } else {
                    if (!isApproxFallthrough(pendingInstr.getPC(), next_pc)) {
                        pendingInstr.setCtrlFlowChange(true);
                        pendingInstr.setCtrlFlowTarget(next_pc);
                        DPRINTF(TraceReader,
                                "CBP fillBuffer: mark non-branch ctrl-flow "
                                "change pc=0x%lx -> nextPC=0x%lx\n",
                                pendingInstr.getPC(), next_pc);
                    }
                }

                addToBuffer(pendingInstr);
                pushed++;
            }
            pendingInstr = current;
            hasPendingInstr = true;
        } else {
            eofReached = true;
            break;
        }
    }

    if (eofReached && hasPendingInstr && pushed < max_instructions) {
        pendingInstr.setLastInTrace(true);
        addToBuffer(pendingInstr);
        pushed++;
        hasPendingInstr = false;
    }

    return pushed;
}

TraceReader::TraceCheckpoint
CBP2025TraceReader::createCheckpoint()
{
    // For compressed traces we cannot reliably seek; allow checkpointing only
    // for uncompressed files by saving the file offset.
    TraceCheckpoint cp;
    cp.instructionIndex = instructionIndex;
    cp.seqNum = currentSeqNum;
    cp.eofState = eofReached;
    cp.bufferSnapshot = instrBuffer;
    cp.hasPending = hasPendingInstr;
    if (hasPendingInstr) {
        cp.pending = pendingInstr;
    }

    if (traceStream.isOpen() && streamMode == TraceStream::Mode::Raw) {
        cp.filePosition = traceStream.tell();
        cp.valid = true;
    } else {
        cp.valid = false;
    }
    return cp;
}

bool
CBP2025TraceReader::restoreCheckpoint(const TraceCheckpoint& checkpoint)
{
    if (!checkpoint.valid) {
        return false;
    }

    if (streamMode != TraceStream::Mode::Raw || !traceStream.isOpen()) {
        return false;
    }

    if (!traceStream.seek(checkpoint.filePosition)) {
        return false;
    }
    eofReached = checkpoint.eofState;
    currentSeqNum = checkpoint.seqNum;
    instructionIndex = checkpoint.instructionIndex;
    instrBuffer = checkpoint.bufferSnapshot;
    hasPendingInstr = checkpoint.hasPending;
    pendingInstr = checkpoint.pending;
    historyWindow.clear();
    historyStartIndex = 1;
    nextLogicalIndex = instructionIndex + 1;
    replayActive = false;
    replayIndex = 0;
    return true;
}

bool
CBP2025TraceReader::seekToInstruction(uint64_t instrIndex)
{
    // For now, only support soft-seek via history window (base class).
    return softSeekToInstruction(instrIndex);
}

uint64_t
CBP2025TraceReader::mapTraceAddressToVirtual(uint64_t trace_addr)
{
    if (addrMapMode == "linear") {
        return mapAddressLinear(trace_addr);
    } else {
        return mapAddressHash(trace_addr);
    }
}

uint64_t
CBP2025TraceReader::mapTracePcToVirtual(uint64_t trace_pc)
{
    // PC keeps the full mapping (page alignment etc.) so that PC sequences
    // stay consistent for external tools and difftest.
    return mapTraceAddressToVirtual(trace_pc);
}

uint64_t
CBP2025TraceReader::mapTraceMemToVirtual(uint64_t trace_addr)
{
    // Memory addresses reuse the same mapping but we additionally enforce
    // a minimal alignment so that scalar memory operations do not
    // systematically trigger RISC-V misaligned-address faults in trace mode.
    uint64_t mapped = mapTraceAddressToVirtual(trace_addr);
    // For now align to 4 bytes, which is enough for 32-bit accesses and
    // matches the ChampSim trace reader behaviour.
    mapped &= ~static_cast<uint64_t>(0x3);
    return mapped;
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

uint64_t
CBP2025TraceReader::mapAddressHash(uint64_t trace_addr)
{
    uint64_t hash = (trace_addr ^ (trace_addr >> 16)) & 0x3FFFFFFFUL;
    uint64_t mapped_addr = addrMapBase + (hash % addrMapSize);

    if (addrPageAlign) {
        const uint64_t PageSz = TheISA::PageBytes;
        mapped_addr = (mapped_addr / PageSz) * PageSz + (trace_addr % PageSz);
        if ((mapped_addr - addrMapBase) >= addrMapSize) {
            mapped_addr = addrMapBase + (trace_addr % addrMapSize);
        }
    }

    // Keep mapped address as-is to preserve compressed instruction spacing
    return mapped_addr;
}

uint64_t
CBP2025TraceReader::mapAddressLinear(uint64_t trace_addr)
{
    uint64_t mapped_addr;

    if (addrPageAlign) {
        const uint64_t PageSz = TheISA::PageBytes;
        const uint64_t page_offset = trace_addr % PageSz;
        const uint64_t trace_page = trace_addr / PageSz;
        const uint64_t pages_in_region = (addrMapSize / PageSz);
        const uint64_t mapped_page = (pages_in_region ? (trace_page % pages_in_region) : 0);
        mapped_addr = addrMapBase + (mapped_page * PageSz) + page_offset;
    } else {
        mapped_addr = addrMapBase + (trace_addr % addrMapSize);
    }

    DPRINTF(TraceReader, "CBP mapAddressLinear: 0x%lx -> 0x%lx (page_align=%d)\n",
            trace_addr, mapped_addr, addrPageAlign);
    return mapped_addr;
}

} // namespace o3
} // namespace gem5
