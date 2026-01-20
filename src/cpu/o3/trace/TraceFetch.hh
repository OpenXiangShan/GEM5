/*
 * Copyright (c) 2025
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

#ifndef __CPU_O3_TRACE_TRACE_FETCH_HH__
#define __CPU_O3_TRACE_TRACE_FETCH_HH__

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "arch/riscv/types.hh"
#include "base/types.hh"
#include "config/the_isa.hh"
#include "cpu/inst_seq.hh"
#include "cpu/o3/comm.hh"
#include "cpu/o3/dyn_inst_ptr.hh"
#include "cpu/o3/limits.hh"
#include "cpu/o3/trace/TraceInstruction.hh"
#include "cpu/o3/trace/TraceReader.hh"

namespace gem5
{

class PCStateBase;
class ThreadContext;
struct BaseO3CPUParams;

namespace o3
{

class Fetch;

/**
 * TraceFetch encapsulates trace-driven simulation behavior which otherwise
 * would live inside the O3 Fetch stage. It owns trace-only state (reader,
 * rollback bookkeeping, wrong-path state, metadata maps) and provides a
 * small delegation surface used by Fetch.
 *
 * This class is intentionally a "friend-style" helper: it needs access to
 * some Fetch internals (decoder, fetchBuffer, CPU pointer) to preserve the
 * exact existing trace-mode behavior while moving code out of fetch.cc.
 */
class TraceFetch
{
  public:
    TraceFetch(Fetch &fetch, const BaseO3CPUParams &params);
    ~TraceFetch();

    bool enabled() const { return traceMode; }
    bool isEOF() const { return traceMode && traceReader && traceReader->isEOF(); }
    bool allowDecoupledFrontend() const { return traceDecoupledFrontend; }

    /** Trace-mode helper: initialize reader, set initial PC, prime frontend. */
    bool initTraceMode();

    /** Reset trace-only per-run/per-thread state (called from Fetch::resetStage). */
    void resetStage();

    /** Called by Fetch squash path to handle trace rollback/cleanup. */
    void handleTraceSquash(ThreadID tid, const PCStateBase &new_pc,
                           const DynInstPtr squashInst, InstSeqNum seqNum);

    /** Called by Fetch::performInstructionFetch to model coupled mispredict stalls. */
    bool maybeStallFetch(ThreadID tid);

    /** Trace-mode checkMemoryNeeds fast path (called from Fetch::checkMemoryNeeds). */
    StallReason checkMemoryNeeds(ThreadID tid, const PCStateBase &this_pc);

    /** Bind pending trace metadata to a built DynInst (called from Fetch). */
    void bindPendingTraceMetadata(ThreadID tid, const DynInstPtr &instruction,
                                  const PCStateBase &pc,
                                  o3::TraceInstruction &traceForThisInst);

    /** Called after Fetch branch prediction to perform wrong-path control/validation. */
    void postBranchPredict(ThreadID tid, const DynInstPtr &instruction,
                           const o3::TraceInstruction &traceForThisInst,
                           PCStateBase &pc, PCStateBase &next_pc,
                           bool predictedBranch);

    /** Clear per-instruction pending state after building an instruction. */
    void clearPending();

    /** Trace metadata APIs used by CPU/Commit/etc. */
    void storeTraceInstMetadata(InstSeqNum seqNum,
                                const o3::TraceInstruction &traceInstr);
    const o3::TraceInstruction* getTraceInstMetadata(InstSeqNum seqNum) const;
    bool isTraceInstruction(InstSeqNum seqNum) const;

    void cleanupTraceMetadataOnCommit(InstSeqNum seqNum);
    void maybeCreateTraceCheckpoint(InstSeqNum seqNum);
    uint64_t findTraceIndexForSeqNum(InstSeqNum seqNum) const;
    bool lookupTraceIndexForSeqNum(InstSeqNum seqNum, uint64_t &index) const;
    Addr getTracePCByIndex(uint64_t index);

    /** Whether trace mode should validate BP against trace to enter wrong-path. */
    bool bpValidationEnabled() const { return traceBPValidation; }
    bool wrongPathEnabled() const { return traceEnableWrongPath; }
    bool wrongPathActive() const { return traceWrongPathActive; }

  private:
    Fetch &fetch;

    bool initializeTraceReader();
    TheISA::MachInst createMachInstFromTrace(const o3::TraceInstruction &traceInstr);

    unsigned chooseWrongPathNopSize(ThreadID tid, Addr pc);
    StallReason fetchTraceInstruction(ThreadID tid, const PCStateBase &this_pc);
    void supplyTraceToDecoder(ThreadID tid, const PCStateBase &this_pc,
                              TheISA::MachInst machInst, Addr instrPC,
                              const char *tag);

    void enterTraceWrongPath(ThreadID tid, InstSeqNum branchSeqNum, Addr predPC,
                             Addr corrPC, bool forceMinStep,
                             const char *reason, uint64_t traceSeqNum);
    void exitTraceWrongPath(ThreadID tid, const char *reason);

    void ensureTraceStreamFilled(ThreadID tid, size_t min_count);
    void cleanupTraceMetadata(InstSeqNum seqNum);
    bool rollbackTraceReader(InstSeqNum seqNum, bool squash_itself);
    bool validateBPPrediction(const o3::TraceInstruction& traceInstr,
                              Addr predictedPC, bool predictedTaken);
    void bindTraceMetadata(const DynInstPtr &instruction,
                           const o3::TraceInstruction &traceInstr,
                           ThreadID tid);
    void validateAndConsumeTraceStream(ThreadID tid, const PCStateBase &pc);
    void maybeEnterTraceCtrlFlowWrongPath(ThreadID tid,
                                          const DynInstPtr &instruction,
                                          const o3::TraceInstruction &traceInstr,
                                          PCStateBase &pc,
                                          PCStateBase &next_pc);
    void handleTraceBPValidation(ThreadID tid, const DynInstPtr &instruction,
                                 const o3::TraceInstruction &traceInstr,
                                 const PCStateBase &next_pc,
                                 bool predictedBranch);

    void setupTraceTimingPTW(::gem5::ThreadContext *tc);

    /** Trace reader for trace-driven simulation */
    std::unique_ptr<o3::TraceReader> traceReader;
    /** Whether trace mode is enabled */
    bool traceMode = false;
    /** Trace format string (e.g., champsim, cbp2025) */
    std::string traceFormat{"champsim"};

    /** Trace timing PTW/TLB cost modeling (opt-in). */
    bool traceTimingPTW = false;
    uint64_t tracePTReservedBytes = 0;
    uint64_t tracePTLeafPageSize = 4 * 1024;
    Addr traceAddrBase = 0;
    Addr traceAddrSize = 0;
    /** Whether to train BP and use real branch instructions in trace mode */
    bool traceTrainBranches = false;
    /** Whether to validate BP against trace to trigger wrong-path mode */
    bool traceBPValidation = true;
    /** Cycles to stall fetch on mispredict in trace mode */
    Cycles traceMispredictPenalty = Cycles(8);
    /** Remaining stall cycles due to a modeled mispredict */
    Cycles traceStallRemaining = Cycles(0);
    /** Enable explicit wrong-path injection in decoupled frontend */
    bool traceEnableWrongPath = true;

    /** Explicit wrong-path simulation (decoupled frontend only) */
    bool traceWrongPathActive = false;
    Cycles traceWrongPathCyclesLeft = Cycles(0);
    Addr traceWrongPathPredPC = 0;
    Addr traceWrongPathCorrectPC = 0;
    InstSeqNum traceWrongPathBranchSeqNum = 0;
    bool traceWrongPathForceMinStep = false;

    /** Wrong-path injection mode: use trace instructions instead of NOPs */
    bool traceWrongPathUseTraceInst = false;
    o3::TraceReader::TraceCheckpoint traceWrongPathCheckpoint;

    /** Whether trace mode should honor decoupled frontend semantics */
    bool traceDecoupledFrontend = false;

    /** Map to store trace instruction metadata for cache hierarchy integration */
    std::unordered_map<InstSeqNum, std::shared_ptr<const o3::TraceInstruction>> traceInstMap;
    /** Map sequence numbers to trace instruction indices for rollback capability */
    std::unordered_map<InstSeqNum, uint64_t> seqNumToTraceIndex;

    uint64_t traceInstrConsumed = 1;

    std::deque<o3::TraceInstruction> traceExpectedStream[MaxThreads];
    static constexpr size_t TRACE_STREAM_MIN_FILL = 16;

    uint64_t traceFetchExpectedCorrectIdx[MaxThreads];

    bool pendingTraceValid = false;
    o3::TraceInstruction pendingTraceInstr;

    std::vector<o3::TraceReader::TraceCheckpoint> traceCheckpoints;
    std::vector<InstSeqNum> checkpointSeqNums;

    /** Trace checkpoint cadence (0 disables checkpointing). */
    uint64_t traceCheckpointInterval = 64;
};

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_TRACE_TRACE_FETCH_HH__
