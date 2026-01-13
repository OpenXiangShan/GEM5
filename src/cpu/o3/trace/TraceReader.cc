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

#include "cpu/o3/trace/TraceReader.hh"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fstream>
#include <functional>

#include "arch/null/page_size.hh"
#include "arch/riscv/page_size.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/o3/trace/CBP2025TraceReader.hh"
#include "cpu/o3/trace/ChampSimTraceReader.hh"
#include "debug/TraceReader.hh"

namespace gem5
{
namespace o3
{

TraceReader::TraceStream::TraceStream()
    : modeFlag(Mode::Raw), pipeHandle(nullptr), eofFlag(false)
{
}

TraceReader::TraceStream::~TraceStream()
{
    close();
}

std::string
TraceReader::TraceStream::escapePath(const std::string &p)
{
    std::string safe = p;
    size_t pos = 0;
    while ((pos = safe.find("'", pos)) != std::string::npos) {
        safe.replace(pos, 1, "'\\''");
        pos += 4;
    }
    return safe;
}

bool
TraceReader::TraceStream::open(const std::string &p, Mode mode)
{
    close();
    path = p;
    modeFlag = mode;
    eofFlag = false;

    if (modeFlag == Mode::Raw) {
        rawStream.open(path, std::ios::binary);
        return rawStream.is_open();
    }

    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }

    if (pid == 0) {
        // Child: decompress to stdout.
        ::close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        ::close(fds[1]);

        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }

        const char *prog = (modeFlag == Mode::Gzip) ? "gzip" : "xz";
        const char *arg1 = (modeFlag == Mode::Gzip) ? "-dcq" : "-dc";
        char *const argv[] = {
            const_cast<char*>(prog),
            const_cast<char*>(arg1),
            const_cast<char*>("--"),
            const_cast<char*>(path.c_str()),
            nullptr,
        };
        execvp(prog, argv);
        _exit(127);
    }

    // Parent: consume decompressed bytes from pipe.
    ::close(fds[1]);
    FILE *fp = fdopen(fds[0], "r");
    if (!fp) {
        ::close(fds[0]);
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return false;
    }

    pipeHandle = fp;
    pipePid = pid;
    return true;
}

bool
TraceReader::TraceStream::reopen()
{
    if (path.empty())
        return false;
    return open(path, modeFlag);
}

void
TraceReader::TraceStream::close()
{
    if (rawStream.is_open()) {
        rawStream.close();
    }
    if (pipeHandle) {
        fclose(pipeHandle);
        pipeHandle = nullptr;
    }
    if (pipePid > 0) {
        waitpid(pipePid, nullptr, 0);
        pipePid = -1;
    }
    eofFlag = false;
}

bool
TraceReader::TraceStream::readExact(void *dst, size_t size)
{
    if (!isOpen())
        return false;

    if (modeFlag == Mode::Raw) {
        rawStream.read(reinterpret_cast<char*>(dst), size);
        auto got = rawStream.gcount();
        eofFlag = rawStream.eof();
        return got == static_cast<std::streamsize>(size);
    }

    size_t n = std::fread(dst, 1, size, pipeHandle);
    eofFlag = feof(pipeHandle);
    return n == size;
}

bool
TraceReader::TraceStream::seekBegin()
{
    if (modeFlag != Mode::Raw || !rawStream.is_open())
        return false;
    rawStream.clear();
    rawStream.seekg(0, std::ios::beg);
    eofFlag = false;
    return !rawStream.fail();
}

bool
TraceReader::TraceStream::seek(std::streampos pos)
{
    if (modeFlag != Mode::Raw || !rawStream.is_open())
        return false;
    rawStream.clear();
    rawStream.seekg(pos);
    eofFlag = false;
    return !rawStream.fail();
}

bool
TraceReader::TraceStream::isOpen() const
{
    return (modeFlag == Mode::Raw) ? rawStream.is_open() : pipeHandle != nullptr;
}

std::streampos
TraceReader::TraceStream::tell() const
{
    if (modeFlag != Mode::Raw || !rawStream.is_open())
        return std::streampos(0);
    // tellg() is non-const; cast is safe because we don't mutate stream state.
    return const_cast<std::ifstream&>(rawStream).tellg();
}

uint64_t
TraceReader::mapAddressHash(uint64_t trace_addr, const AddrMapConfig &cfg)
{
    uint64_t hash = (trace_addr ^ (trace_addr >> 16)) & 0x3FFFFFFFUL;
    uint64_t mapped_addr = cfg.base + (hash % cfg.size);

    if (cfg.pageAlign) {
        const uint64_t PageSz = TheISA::PageBytes;
        mapped_addr = (mapped_addr / PageSz) * PageSz + (trace_addr % PageSz);
        if ((mapped_addr - cfg.base) >= cfg.size) {
            mapped_addr = cfg.base + (trace_addr % cfg.size);
        }
    }

    return mapped_addr;
}

uint64_t
TraceReader::mapAddressLinear(uint64_t trace_addr, const AddrMapConfig &cfg)
{
    uint64_t mapped_addr;

    if (cfg.pageAlign) {
        const uint64_t PageSz = TheISA::PageBytes;
        panic_if(cfg.size < PageSz,
                 "TraceReader address map size (%llu) must be >= page size (%llu) "
                 "when pageAlign is enabled",
                 static_cast<unsigned long long>(cfg.size),
                 static_cast<unsigned long long>(PageSz));
        const uint64_t page_offset = trace_addr % PageSz;
        const uint64_t trace_page = trace_addr / PageSz;
        const uint64_t pages_in_region = (cfg.size / PageSz);
        const uint64_t mapped_page = (pages_in_region ? (trace_page % pages_in_region) : 0);
        mapped_addr = cfg.base + (mapped_page * PageSz) + page_offset;
    } else {
        mapped_addr = cfg.base + (trace_addr % cfg.size);
    }

    return mapped_addr;
}

uint64_t
TraceReader::mapTraceAddressToVirtual(uint64_t trace_addr, const AddrMapConfig &cfg)
{
    panic_if(cfg.size == 0,
             "TraceReader address map size must be non-zero for address mapping");

    if (cfg.mode == "linear") {
        return mapAddressLinear(trace_addr, cfg);
    } else {
        return mapAddressHash(trace_addr, cfg);
    }
}

uint64_t
TraceReader::mapTracePcToVirtual(uint64_t trace_pc, const AddrMapConfig &cfg)
{
    return mapTraceAddressToVirtual(trace_pc, cfg);
}

uint64_t
TraceReader::mapTraceMemToVirtual(uint64_t trace_addr, const AddrMapConfig &cfg)
{
    uint64_t mapped = mapTraceAddressToVirtual(trace_addr, cfg);
    mapped &= ~static_cast<uint64_t>(0x3);
    return mapped;
}

bool
TraceReader::isApproxFallthrough(Addr pc, Addr next_pc)
{
    return next_pc == pc + 2 || next_pc == pc + 4;
}

void
TraceReader::reconcilePendingWithNext(TraceInstruction &pending,
                                      TraceInstruction &next,
                                      const PendingResolveConfig &cfg)
{
    Addr next_pc = next.getPC();
    const Addr curr_pc = pending.getPC();
    const bool pending_taken_branch =
        pending.isAnyBranch() && pending.getBranchTaken();

    if (!pending.isAnyBranch() && (next_pc & 0x1)) {
        Addr corrected_pc = curr_pc + 4;
        next.setPC(corrected_pc);
        next_pc = corrected_pc;
    }

    if (!pending_taken_branch && next_pc >= curr_pc) {
        const uint64_t delta = next_pc - curr_pc;
        if (delta > 0 && delta <= 8) {
            pending.setInstSizeBytes(static_cast<uint8_t>(delta));
        }
    }

    if (pending.isAnyBranch()) {
        if (pending_taken_branch) {
            if (!pending.getHasBranchTarget()) {
                pending.setBranchTarget(next_pc);
            } else if (cfg.fixTakenTargetMismatch &&
                       pending.getBranchTarget() != next_pc) {
                pending.setBranchTarget(next_pc);
                pending.setCtrlFlowChange(true);
                pending.setCtrlFlowTarget(next_pc);
            }
        } else if (!isApproxFallthrough(curr_pc, next_pc)) {
            pending.setCtrlFlowChange(true);
            pending.setCtrlFlowTarget(next_pc);
        }
    } else if (!isApproxFallthrough(curr_pc, next_pc)) {
        pending.setCtrlFlowChange(true);
        pending.setCtrlFlowTarget(next_pc);
    }
}

size_t
TraceReader::drainPendingToBuffer(size_t max_instructions,
                                  const PendingResolveConfig &resolveCfg,
                                  bool markLastInTrace)
{
    size_t pushed = 0;
    while (pushed < max_instructions && !eofReached) {
        TraceInstruction current;
        if (parseInstruction(current)) {
            if (hasPendingInstr) {
                reconcilePendingWithNext(pendingInstr, current, resolveCfg);
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

    if (markLastInTrace && eofReached && hasPendingInstr && pushed < max_instructions) {
        pendingInstr.setLastInTrace(true);
        addToBuffer(pendingInstr);
        pushed++;
        hasPendingInstr = false;
    }

    return pushed;
}

TraceReader::TraceCheckpoint
TraceReader::buildCheckpoint(uint64_t instructionIndex,
                             uint64_t currentSeqNum,
                             bool eofState,
                             const std::queue<TraceInstruction> &buffer,
                             bool hasPending,
                             const TraceInstruction &pending,
                             const TraceStream &stream,
                             TraceStream::Mode mode,
                             bool allowCompressed) const
{
    TraceCheckpoint cp;
    cp.instructionIndex = instructionIndex;
    cp.seqNum = currentSeqNum;
    cp.eofState = eofState;
    cp.bufferSnapshot = buffer;
    cp.hasPending = hasPending;
    if (hasPending) {
        cp.pending = pending;
    }

    if (stream.isOpen()) {
        if (mode == TraceStream::Mode::Raw) {
            cp.filePosition = stream.tell();
            cp.valid = true;
        } else if (allowCompressed) {
            cp.filePosition = std::streampos(0);
            cp.valid = true;
        }
    }

    return cp;
}

void
TraceReader::applyCheckpointState(const TraceCheckpoint &checkpoint,
                                  std::queue<TraceInstruction> &buffer,
                                  bool &hasPending,
                                  TraceInstruction &pending,
                                  uint64_t &instructionIndex,
                                  uint64_t &currentSeqNum,
                                  bool &eofReached)
{
    instructionIndex = checkpoint.instructionIndex;
    currentSeqNum = checkpoint.seqNum;
    eofReached = checkpoint.eofState;

    std::queue<TraceInstruction> empty;
    std::swap(buffer, empty);
    buffer = checkpoint.bufferSnapshot;

    hasPending = checkpoint.hasPending;
    if (hasPending) {
        pending = checkpoint.pending;
    } else {
        pending.reset();
    }
}

void
TraceReader::resetBufferState()
{
    eofReached = false;
    currentSeqNum = 0;
    hasPendingInstr = false;
    pendingInstr.reset();

    std::queue<TraceInstruction> empty;
    std::swap(instrBuffer, empty);

    resetHistoryWindow(1);
}

void
TraceReader::resetHistoryWindow(uint64_t startIndex)
{
    historyWindow.clear();
    historyStartIndex = startIndex;
    nextLogicalIndex = startIndex;
    replayActive = false;
    replayIndex = 0;
}

bool
TraceReader::openTraceStream(TraceStream &stream, TraceStream::Mode mode,
                             std::streampos *outPos)
{
    if (!stream.open(traceFile, mode)) {
        return false;
    }
    if (outPos) {
        *outPos = (mode == TraceStream::Mode::Raw) ? stream.tell() : std::streampos(0);
    }
    return true;
}

bool
TraceReader::reopenTraceStream(TraceStream &stream, TraceStream::Mode mode,
                               std::streampos *outPos)
{
    if (!stream.reopen()) {
        return false;
    }
    if (outPos) {
        *outPos = (mode == TraceStream::Mode::Raw) ? stream.tell() : std::streampos(0);
    }
    return true;
}

bool
TraceReader::validateTraceFileSize(const std::string &path,
                                   std::streamsize minSize)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return false;
    }
    return file.tellg() >= minSize;
}

bool
TraceReader::restoreCheckpointCommon(const TraceCheckpoint& checkpoint,
                                     TraceStream &stream, TraceStream::Mode mode,
                                     bool allowCompressedRewind,
                                     std::function<bool(uint64_t)> fastForward,
                                     uint64_t &instructionIndexRef)
{
    if (!checkpoint.valid) {
        return false;
    }

    // Attempt to seek to saved position if raw; otherwise allow compressed rewind.
    if (stream.isOpen() && mode == TraceStream::Mode::Raw) {
        if (!stream.seek(checkpoint.filePosition)) {
            return false;
        }
    } else if (stream.isOpen() && allowCompressedRewind) {
        if (!reopenTraceStream(stream, mode, nullptr)) {
            return false;
        }
        if (!fastForward(checkpoint.instructionIndex)) {
            return false;
        }
    } else {
        return false;
    }

    applyCheckpointState(checkpoint, instrBuffer, hasPendingInstr, pendingInstr,
                         instructionIndexRef, currentSeqNum, eofReached);
    return true;
}


void
TraceReader::dumpInstrBuffer(const char* tag) const
{
    // Avoid expensive buffer dumps unless the TraceReader debug flag is enabled.
    if (!debug::TraceReader)
        return;

    std::queue<TraceInstruction> tmp = instrBuffer; // copy for non-destructive dump
    size_t sz = tmp.size();
    DPRINTF(TraceReader, "instrBuffer dump (%s): size=%lu\n", tag, (unsigned long)sz);
    size_t idx = 0;
    // panic if there are seqNum discontinuity
    uint64_t previousSeqNum = 0, firstSeqNum = 0, discontinuityPos = 0;
    while (!tmp.empty()) {
        const auto &ti = tmp.front();
        if (firstSeqNum == 0) {
            firstSeqNum = ti.getSeqNum();
        }
        if (previousSeqNum != 0) {
            if (ti.getSeqNum() != previousSeqNum + 1) {
                discontinuityPos = ti.getSeqNum();
                DPRINTF(TraceReader,
                        "TraceReader::dumpInstrBuffer: seqNum discontinuity "
                        "at idx=%lu: last=%llu current=%llu\n",
                        (unsigned long)idx,
                        (unsigned long long)previousSeqNum,
                        (unsigned long long)ti.getSeqNum());
            }
        }
        previousSeqNum = ti.getSeqNum();
        tmp.pop();
        ++idx;
    }
    if (discontinuityPos != 0) {
        panic("TraceReader::dumpInstrBuffer: seqNum discontinuity detected");
    }
}

TraceReader::TraceReaderStats::TraceReaderStats(statistics::Group *parent,
                                                 const std::string &name)
    : statistics::Group(parent, name.c_str()),
      ADD_STAT(instrRead, statistics::units::Count::get(),
               "Number of instructions read from trace"),
      ADD_STAT(branchInstr, statistics::units::Count::get(),
               "Number of branch instructions encountered"),
      ADD_STAT(loadInstr, statistics::units::Count::get(),
               "Number of load instructions encountered"),
      ADD_STAT(storeInstr, statistics::units::Count::get(),
               "Number of store instructions encountered"),
      ADD_STAT(bufferUnderruns, statistics::units::Count::get(),
               "Number of times buffer was empty when instruction requested")
{
}

TraceReader::TraceReader(const std::string &trace_file, const std::string &name,
                         statistics::Group *parent)
    : statistics::Group(parent, name.c_str()), traceFile(trace_file), readerName(name),
      eofReached(false), initialized(false), currentSeqNum(0), addrCfg({0, 0, "hash", false}),
      stats(this, "stats")
{
    // Debug output removed temporarily
}

TraceInstruction
TraceReader::getNextInstruction()
{
    // 1) Soft replay path: serve from history when active
    if (replayActive) {
        const uint64_t winBegin = historyStartIndex;
        const uint64_t winEnd = historyStartIndex + (historyWindow.empty() ? 0 : (historyWindow.size() - 1));
        if (replayIndex >= winBegin && replayIndex <= winEnd && !historyWindow.empty()) {
            const size_t off = static_cast<size_t>(replayIndex - historyStartIndex);
            auto instr = historyWindow[off];
            // Do not update nextLogicalIndex on replay; only move replay cursor.
            replayIndex++;
            DPRINTF(TraceReader,
                    "getNextInstruction[replay]: RETURN pc=0x%llx sn=%llu (idx=%llu)\n",
                    (unsigned long long)instr.getPC(),
                    (unsigned long long)instr.getSeqNum(),
                    (unsigned long long)(replayIndex - 1));
            return instr;
        }
        // Replay exhausted window, turn off and continue with normal path
        replayActive = false;
        replayIndex = 0;
    }

    // 2) Normal path: fill buffer if it's getting low
    if (instrBuffer.size() < MAX_BUFFER_SIZE / 4 && !eofReached) {
        fillBuffer(MAX_BUFFER_SIZE / 2);
    }

    // Ensure availability
    if (instrBuffer.empty()) {
        if (!eofReached) {
            fillBuffer(1);
        }
        if (instrBuffer.empty()) {
            stats.bufferUnderruns++;
            TraceInstruction invalid_instr;
            invalid_instr.setValid(false);
            DPRINTF(TraceReader, "getNextInstruction: No valid instruction available\n");
            return invalid_instr;
        }
    }

    // 3) Pop next and append to history window
    dumpInstrBuffer("before_pop");
    TraceInstruction instr = instrBuffer.front();
    instrBuffer.pop();

    // Update statistics
    updateStats(instr);

    // Track history
    historyWindow.push_back(instr);
    if (historyWindow.size() > HISTORY_CAPACITY) {
        historyWindow.pop_front();
        historyStartIndex++;
    }
    nextLogicalIndex++;

    DPRINTF(TraceReader,
            "getNextInstruction: RETURN pc=0x%llx sn=%llu\n",
            (unsigned long long)instr.getPC(),
            (unsigned long long)instr.getSeqNum());
    dumpInstrBuffer("after_pop");

    return instr;
}

void
TraceReader::addToBuffer(const TraceInstruction &instr)
{
    // Dump before modification
    dumpInstrBuffer("before_push");

    if (instrBuffer.size() >= MAX_BUFFER_SIZE) {
        panic("TraceReader::addToBuffer: buffer overflow (MAX_BUFFER_SIZE=%lu) at "
              "PC=0x%lx (sn:%llu)",
              (unsigned long)MAX_BUFFER_SIZE,
              instr.getPC(),
              (unsigned long long)instr.getSeqNum());
    }

    instrBuffer.push(instr);
    DPRINTF(TraceReader, "addToBuffer: Added instruction PC=0x%lx (sn:%llu) to buffer\n",
            instr.getPC(), (unsigned long long)instr.getSeqNum());
    // Debug: Added instruction to buffer

    // Dump after modification
    dumpInstrBuffer("after_push");
}

void
TraceReader::updateStats(const TraceInstruction &instr)
{
    stats.instrRead++;

    if (instr.isAnyBranch()) {
        stats.branchInstr++;
    }

    if (instr.getLoad()) {
        stats.loadInstr++;
    }

    if (instr.getStore()) {
        stats.storeInstr++;
    }
}

std::unique_ptr<TraceReader>
createTraceReader(const std::string &format, const std::string &trace_file,
                  const std::string &name, uint64_t addrBase, uint64_t addrSize,
                  const std::string &addrMapMode, bool pageAlign,
                  statistics::Group *parent)
{
    // Debug: Creating trace reader with mapping parameters
    // Note: name is passed as parameter, not from object method

    if (format == "champsim") {
        auto reader = std::make_unique<ChampSimTraceReader>(trace_file, name,
                                                            addrMapMode,
                                                            addrBase,
                                                            addrSize,
                                                            pageAlign,
                                                            parent);
        // Configure address mapping parameters
        reader->setAddressMapping(addrBase, addrSize, addrMapMode, pageAlign);
        return reader;
    } else if (format == "cbp2025") {
        auto reader = std::make_unique<CBP2025TraceReader>(trace_file, name,
                                                           addrMapMode,
                                                           addrBase,
                                                           addrSize,
                                                           pageAlign,
                                                           parent);
        return reader;
    } else {
        // Debug output removed temporarily
        return nullptr;
    }
}

bool
TraceReader::softSeekToInstruction(uint64_t instrIndex)
{
    // Match ChampSimTraceReader::seekToInstruction semantics:
    // after seek(N), the next getNextInstruction() returns instruction N+1 (1-based).

    const uint64_t winBegin = historyStartIndex;
    const uint64_t winEnd = historyStartIndex + (historyWindow.empty() ? 0 : (historyWindow.size() - 1));
    const uint64_t logicalNext = historyStartIndex + historyWindow.size(); // next index to return

    // 1) If (N+1) is inside history window, just replay from there
    const uint64_t want = instrIndex + 1;
    if (!historyWindow.empty() && want >= winBegin && want <= winEnd) {
        replayActive = true;
        replayIndex = want;
        DPRINTF(TraceReader,
                "softSeekToInstruction: replay to (idx+1)=%llu within window [%llu,%llu]\n",
                (unsigned long long)want,
                (unsigned long long)winBegin,
                (unsigned long long)winEnd);
        return true;
    }

    // 2) If (N+1) is the current logical next, nothing to do
    if (want == logicalNext) {
        DPRINTF(TraceReader, "softSeekToInstruction: already aligned at (idx+1)=%llu",
                (unsigned long long)want);
        return true;
    }

    // 3) If (N+1) is ahead but inside buffered future, drop-ahead
    if (want > logicalNext) {
        uint64_t drop = want - logicalNext;
        while (drop > 0 && !instrBuffer.empty()) {
            auto tmp = instrBuffer.front();
            instrBuffer.pop();
            // Move dropped items into history
            historyWindow.push_back(tmp);
            if (historyWindow.size() > HISTORY_CAPACITY) {
                historyWindow.pop_front();
                historyStartIndex++;
            }
            drop--;
        }
        if (drop == 0) {
            DPRINTF(TraceReader, "softSeekToInstruction: drop-ahead to (idx+1)=%llu using buffer\n",
                    (unsigned long long)want);
            return true;
        }
        // else, not enough buffered, fall through
    }

    // 4) Fallback to hard seek
    const bool ok = seekToInstruction(instrIndex);
    if (ok) {
        // After hard seek, clear runtime buffers/history to keep state consistent
        std::queue<TraceInstruction> empty;
        std::swap(instrBuffer, empty);
        resetHistoryWindow(instrIndex);
    }
    return ok;
}


void
TraceReader::resetHistory()
{
    // Clear buffer and history state; caller is responsible for calling init()/fill later
    std::queue<TraceInstruction> empty;
    std::swap(instrBuffer, empty);
    historyWindow.clear();
    historyStartIndex = 1;
    replayActive = false;
    replayIndex = 0;
    nextLogicalIndex = 1;
}

} // namespace o3
} // namespace gem5
