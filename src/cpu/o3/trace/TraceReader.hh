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

#ifndef __CPU_O3_TRACE_TRACE_READER_HH__
#define __CPU_O3_TRACE_TRACE_READER_HH__

#include <cstdio>
#include <deque>
#include <fstream>
#include <memory>
#include <queue>
#include <string>

#include "base/statistics.hh"
#include "cpu/o3/trace/TraceInstruction.hh"
#include "sim/sim_object.hh"

namespace gem5
{
namespace o3
{

/**
 * Abstract base class for trace readers.
 *
 * This class defines the interface that all trace format readers must
 * implement to provide instructions to the O3CPU fetch stage. Different
 * trace formats (ChampSim, CBP2025, etc.) can be supported by inheriting
 * from this class and implementing the pure virtual methods.
 */
class TraceReader : public statistics::Group
{
  protected:
    /**
     * Minimal stream helper supporting raw, gzip (via popen gzip -dcq), and xz.
     * Unified封装用于两个具体 TraceReader，避免重复打开/重置逻辑。
     */
    class TraceStream
    {
      public:
        enum class Mode { Raw, Gzip, Xz };

        TraceStream();
        ~TraceStream();

        bool open(const std::string &path, Mode mode);
        bool reopen();
        void close();

        bool readExact(void *dst, size_t size);
        bool seekBegin();
        bool seek(std::streampos pos);

        bool isOpen() const;
        bool eof() const { return eofFlag; }
        std::streampos tell() const;
        Mode mode() const { return modeFlag; }

      private:
        static std::string escapePath(const std::string &path);

        std::string path;
        Mode modeFlag;
        std::ifstream rawStream;
        FILE *pipeHandle;
        bool eofFlag;
    };

    /** Path to the trace file */
    std::string traceFile;

    /** Name of this trace reader instance */
    std::string readerName;

    /** Whether the trace has reached end-of-file */
    bool eofReached;

    /** Whether the trace reader has been initialized */
    bool initialized;

    /** Current instruction sequence number */
    uint64_t currentSeqNum;

    /** Buffer for pre-fetched instructions */
    std::queue<TraceInstruction> instrBuffer;

    /** History window for soft replay (recently returned instructions) */
    std::deque<TraceInstruction> historyWindow;
    /** 1-based index of the first element in history window */
    uint64_t historyStartIndex = 1;
    /** Next logical index to be returned by getNextInstruction (when not replaying) */
    uint64_t nextLogicalIndex = 1;
    /** Soft replay mode flag and cursor (1-based) */
    bool replayActive = false;
    uint64_t replayIndex = 0;
    /** Window capacity */
    static constexpr size_t HISTORY_CAPACITY = 4096;

    /** Dump current instruction buffer contents for debugging */
    void dumpInstrBuffer(const char* tag) const;

    /** Maximum size of instruction buffer */
    static constexpr size_t MAX_BUFFER_SIZE = 1024;

    /** Statistics group for trace reader */
    struct TraceReaderStats : public statistics::Group
    {
        /** Number of instructions read from trace */
        statistics::Scalar instrRead;

        /** Number of branch instructions encountered */
        statistics::Scalar branchInstr;

        /** Number of load instructions encountered */
        statistics::Scalar loadInstr;

        /** Number of store instructions encountered */
        statistics::Scalar storeInstr;

        /** Number of buffer underruns */
        statistics::Scalar bufferUnderruns;

        TraceReaderStats(statistics::Group *parent, const std::string &name);
    } stats;

    /** Address mapping parameters shared by trace readers */
    struct AddrMapConfig
    {
        uint64_t base;
        uint64_t size;
        std::string mode;
        bool pageAlign;
    };

    /** Pending-instruction reconciliation options */
    struct PendingResolveConfig
    {
        bool fixTakenTargetMismatch = false;
    };

    /** Shared address mapping helpers */
    static uint64_t mapAddressHash(uint64_t trace_addr,
                                   const AddrMapConfig &cfg);
    static uint64_t mapAddressLinear(uint64_t trace_addr,
                                     const AddrMapConfig &cfg);
    static uint64_t mapTraceAddressToVirtual(uint64_t trace_addr,
                                             const AddrMapConfig &cfg);
    static uint64_t mapTracePcToVirtual(uint64_t trace_pc,
                                        const AddrMapConfig &cfg);
    static uint64_t mapTraceMemToVirtual(uint64_t trace_addr,
                                         const AddrMapConfig &cfg);

    /** Shared pending-instruction reconciliation helper */
    static bool isApproxFallthrough(Addr pc, Addr next_pc);
    static void reconcilePendingWithNext(TraceInstruction &pending,
                                         TraceInstruction &next,
                                         const PendingResolveConfig &cfg);

  public:
    /**
     * Constructor
     * @param trace_file Path to the trace file
     * @param name Name for statistics
     */
    TraceReader(const std::string &trace_file, const std::string &name,
                statistics::Group *parent = nullptr);

    /** Virtual destructor */
    virtual ~TraceReader() = default;

    /**
     * Initialize the trace reader
     * @return true if initialization successful, false otherwise
     */
    virtual bool init() = 0;

    /**
     * Read the next instruction from the trace
     * @return TraceInstruction object, invalid if no more instructions
     */
    TraceInstruction getNextInstruction();

    /**
     * Try a soft seek using in-memory history/window.
     * If target is within history or buffered future, switch replay mode or drop-ahead.
     * Otherwise fall back to hard seek.
     */
    virtual bool softSeekToInstruction(uint64_t instrIndex);

    /** Clear runtime buffers/history (used on init/reset) */
    void resetHistory();

    /**
     * Check if the trace has reached end-of-file
     * @return true if EOF reached, false otherwise
     */
    bool isEOF() const { return eofReached && instrBuffer.empty(); }

    /**
     * Get the number of instructions currently buffered
     * @return Number of buffered instructions
     */
    size_t getBufferSize() const { return instrBuffer.size(); }

    /**
     * Get the name of this trace reader
     * @return Reader name
     */
    const std::string& name() const { return readerName; }

    /**
     * Reset the trace reader to the beginning of the trace
     * @return true if reset successful, false otherwise
     */
    virtual bool reset() = 0;

    /**
     * Get trace format identifier
     * @return String identifying the trace format
     */
    virtual std::string getFormat() const = 0;

    /**
     * Get trace file path
     * @return Path to the trace file
     */
    const std::string& getTraceFile() const { return traceFile; }

    /**
     * Checkpoint data structure for trace reader state
     */
    struct TraceCheckpoint
    {
        /** Instruction index in the trace */
        uint64_t instructionIndex;

        /** File position (for uncompressed files) */
        std::streampos filePosition;

        /** EOF state at checkpoint */
        bool eofState;

        /** Current sequence number at checkpoint */
        uint64_t seqNum;

        /** Buffer contents at checkpoint */
        std::queue<TraceInstruction> bufferSnapshot;

        /** Pending instruction snapshot (for readers with lookahead) */
        bool hasPending;
        TraceInstruction pending;

        /** Whether checkpoint is valid */
        bool valid;

        TraceCheckpoint() : instructionIndex(0), filePosition(0),
                           eofState(false), seqNum(0), hasPending(false),
                           valid(false) {}
    };

    /**
     * Create a checkpoint of the current trace reader state
     * @return TraceCheckpoint containing current state
     */
    virtual TraceCheckpoint createCheckpoint() = 0;

    /**
     * Restore trace reader state from a checkpoint
     * @param checkpoint The checkpoint to restore from
     * @return true if restore successful, false otherwise
     */
    virtual bool restoreCheckpoint(const TraceCheckpoint& checkpoint) = 0;

    /**
     * Seek to a specific instruction index in the trace
     * @param instrIndex The instruction index to seek to
     * @return true if seek successful, false otherwise
     */
    virtual bool seekToInstruction(uint64_t instrIndex) = 0;

    /**
     * Get the current instruction index in the trace
     * @return Current instruction index
     */
    virtual uint64_t getCurrentInstructionIndex() const = 0;

  protected:
    /**
     * Read instructions from the trace file and fill the buffer
     * This method should be implemented by derived classes to read
     * format-specific trace data and convert it to TraceInstruction objects
     * @param max_instructions Maximum number of instructions to read
     * @return Number of instructions actually read
     */
    virtual size_t fillBuffer(size_t max_instructions) = 0;

    /**
     * Parse a single instruction from the trace format
     * This method should be implemented by derived classes
     * @param instr Reference to TraceInstruction to populate
     * @return true if instruction was successfully parsed, false on EOF or error
     */
    virtual bool parseInstruction(TraceInstruction &instr) = 0;

    /**
     * Check if the trace file is valid and can be opened
     * @return true if file is valid, false otherwise
     */
    virtual bool validateTraceFile() = 0;

    /**
     * Add an instruction to the buffer
     * @param instr Instruction to add
     */
    void addToBuffer(const TraceInstruction &instr);

    /**
     * Update statistics based on the instruction
     * @param instr Instruction to analyze
     */
    void updateStats(const TraceInstruction &instr);

    /**
     * Get the next available sequence number
     * @return Next sequence number
     */
    uint64_t getNextSeqNum() { return currentSeqNum++; }
};

/**
 * Factory function to create trace readers based on format
 * @param format Trace format identifier ("champsim", "cbp2025", etc.)
 * @param trace_file Path to trace file
 * @param name Name for statistics
 * @return Unique pointer to trace reader, or nullptr if format unsupported
 */
std::unique_ptr<TraceReader> createTraceReader(const std::string &format,
                                               const std::string &trace_file,
                                               const std::string &name,
                                               uint64_t addrBase = 0x10000000,
                                               uint64_t addrSize = 0x40000000,
                                               const std::string &addrMapMode = "hash",
                                               bool pageAlign = true,
                                               statistics::Group *parent = nullptr);

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACE_READER_HH__
