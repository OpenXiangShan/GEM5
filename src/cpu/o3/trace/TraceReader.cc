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

#include "base/trace.hh"
#include "config/the_isa.hh"
#include "cpu/o3/trace/CBP2025TraceReader.hh"
#include "cpu/o3/trace/ChampSimTraceReader.hh"
#include "debug/TraceReader.hh"

namespace gem5
{
namespace o3
{

void
TraceReader::dumpInstrBuffer(const char* tag) const
{
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

TraceReader::TraceReader(const std::string &trace_file, const std::string &name)
    : statistics::Group(nullptr, name.c_str()), traceFile(trace_file), readerName(name),
      eofReached(false), initialized(false), currentSeqNum(0), stats(this, name)
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
        // Debug output removed temporarily
        DPRINTF(TraceReader, "addToBuffer: Buffer full, dropping instruction PC=0x%lx (sn:%llu)\n",
                instr.getPC(), (unsigned long long)instr.getSeqNum());
        return;
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
                  const std::string &addrMapMode, bool pageAlign)
{
    // Debug: Creating trace reader with mapping parameters
    // Note: name is passed as parameter, not from object method

    if (format == "champsim") {
        auto reader = std::make_unique<ChampSimTraceReader>(trace_file, name);
        // Configure address mapping parameters
        reader->setAddressMapping(addrBase, addrSize, addrMapMode, pageAlign);
        return reader;
    } else if (format == "cbp2025") {
        return std::make_unique<CBP2025TraceReader>(trace_file, name);
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
        historyWindow.clear();
        // After seek(N), next to return is N+1; with empty history, make logicalNext == N
        historyStartIndex = instrIndex;
        replayActive = false;
        replayIndex = 0;
        nextLogicalIndex = instrIndex;
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
