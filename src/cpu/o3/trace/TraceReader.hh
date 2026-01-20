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

#include <sys/types.h>

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
  public:
    struct TraceCheckpoint
    {
        uint64_t instructionIndex;
        std::streampos filePosition;
        bool eofState;
        uint64_t seqNum;
        std::queue<TraceInstruction> bufferSnapshot;
        bool hasPending;
        TraceInstruction pending;
        bool valid;

        TraceCheckpoint() : instructionIndex(0), filePosition(0),
                            eofState(false), seqNum(0), hasPending(false),
                            pending(), valid(false) {}
    };

    struct AddrMapConfig
    {
        uint64_t base;
        uint64_t size;
        std::string mode;
        bool pageAlign;
    };

    struct PendingResolveConfig
    {
        bool fixTakenTargetMismatch = false;
    };

    TraceReader(const std::string &trace_file, const std::string &name,
                statistics::Group *parent = nullptr);
    virtual ~TraceReader() = default;

    virtual bool init() = 0;
    TraceInstruction getNextInstruction();
    virtual bool softSeekToInstruction(uint64_t instrIndex);
    void resetHistory();
    bool isEOF() const { return eofReached && instrBuffer.empty(); }
    size_t getBufferSize() const { return instrBuffer.size(); }
    const std::string& name() const { return readerName; }
    virtual bool reset() = 0;
    virtual std::string getFormat() const = 0;
    const std::string& getTraceFile() const { return traceFile; }

    const AddrMapConfig& getAddrMapConfig() const { return addrCfg; }
    void setAddrMapConfig(const AddrMapConfig &cfg) { addrCfg = cfg; }
    virtual TraceCheckpoint createCheckpoint() = 0;
    virtual bool restoreCheckpoint(const TraceCheckpoint& checkpoint) = 0;
    virtual bool seekToInstruction(uint64_t instrIndex) = 0;
    virtual uint64_t getCurrentInstructionIndex() const = 0;

  protected:
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
        pid_t pipePid = -1;
        bool eofFlag;
    };

    std::string traceFile;
    std::string readerName;
    bool eofReached;
    bool initialized;
    uint64_t currentSeqNum;
    AddrMapConfig addrCfg;
    bool hasPendingInstr = false;
    TraceInstruction pendingInstr;
    std::queue<TraceInstruction> instrBuffer;
    std::deque<TraceInstruction> historyWindow;
    uint64_t historyStartIndex = 1;
    uint64_t nextLogicalIndex = 1;
    bool replayActive = false;
    uint64_t replayIndex = 0;
    static constexpr size_t HISTORY_CAPACITY = 4096;

    void dumpInstrBuffer(const char* tag) const;
    static constexpr size_t MAX_BUFFER_SIZE = 1024;

    struct TraceReaderStats : public statistics::Group
    {
        statistics::Scalar instrRead;
        statistics::Scalar branchInstr;
        statistics::Scalar loadInstr;
        statistics::Scalar storeInstr;
        statistics::Scalar bufferUnderruns;

        TraceReaderStats(statistics::Group *parent, const std::string &name);
    } stats;

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

    static bool isApproxFallthrough(Addr pc, Addr next_pc);
    static void reconcilePendingWithNext(TraceInstruction &pending,
                                         TraceInstruction &next,
                                         const PendingResolveConfig &cfg);

    TraceCheckpoint buildCheckpoint(uint64_t instructionIndex,
                                    uint64_t currentSeqNum,
                                    bool eofState,
                                    const std::queue<TraceInstruction> &buffer,
                                    bool hasPending,
                                    const TraceInstruction &pending,
                                    const TraceStream &stream,
                                    TraceStream::Mode mode,
                                    bool allowCompressed) const;

    static void applyCheckpointState(const TraceCheckpoint &checkpoint,
                                     std::queue<TraceInstruction> &buffer,
                                     bool &hasPending,
                                     TraceInstruction &pending,
                                     uint64_t &instructionIndex,
                                     uint64_t &currentSeqNum,
                                     bool &eofReached);
    size_t drainPendingToBuffer(size_t max_instructions,
                                const PendingResolveConfig &resolveCfg,
                                bool markLastInTrace);
    void resetBufferState();
    void resetHistoryWindow(uint64_t startIndex = 1);
    bool openTraceStream(TraceStream &stream, TraceStream::Mode mode,
                         std::streampos *outPos);
    bool reopenTraceStream(TraceStream &stream, TraceStream::Mode mode,
                           std::streampos *outPos);
    static bool validateTraceFileSize(const std::string &path,
                                      std::streamsize minSize);
    bool restoreCheckpointCommon(const TraceCheckpoint& checkpoint,
                                 TraceStream &stream, TraceStream::Mode mode,
                                 bool allowCompressedRewind,
                                 std::function<bool(uint64_t)> fastForward,
                                 uint64_t &instructionIndexRef);

    virtual size_t fillBuffer(size_t max_instructions) = 0;
    virtual bool parseInstruction(TraceInstruction &instr) = 0;
    virtual bool validateTraceFile() = 0;
    void addToBuffer(const TraceInstruction &instr);
    void updateStats(const TraceInstruction &instr);
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
