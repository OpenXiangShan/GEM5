#ifndef __CPU_O3_ALTERNATE_PATH_HH__
#define __CPU_O3_ALTERNATE_PATH_HH__

#include <deque>
#include <memory>
#include <optional>
#include <vector>

#include "arch/riscv/pcstate.hh"
#include "base/statistics.hh"
#include "cpu/pred/btb/alternate_path.hh"
#include "cpu/static_inst.hh"

namespace gem5::o3
{

struct BufferedUop
{
    StaticInstPtr instruction;
    StaticInstPtr macroop;
    RiscvISA::PCState pc;
    RiscvISA::PCState nextPC;
    bool taken = false;
    unsigned prediction = 0;
};

struct AlternatePathContext
{
    branch_prediction::btb_pred::AlternateCandidate source;
    branch_prediction::btb_pred::AlternateCheckpoint predictor;
    // A predictor block may take several generation cycles to consume. Keep
    // its original query and starting context while extending its saved prefix.
    std::optional<branch_prediction::btb_pred::AlternatePrediction> blockPrediction;
    branch_prediction::btb_pred::AlternateCheckpoint blockStart;
    std::vector<branch_prediction::btb_pred::BTBEntry> blockBranches;
    unsigned blockFirstUop = 0;
    unsigned blockIndex = 0;
    std::vector<branch_prediction::btb_pred::AlternatePrediction> predictions;
    std::vector<Addr> nextPCs;
    std::vector<BufferedUop> uops;
    RiscvISA::PCState pc;
    Addr startPC = 0;
    uint64_t addressSpace = 0;
    uint64_t privilege = 0;
    uint64_t epoch = 0;
    Tick ready = 0;
    unsigned cycles = 0;
    unsigned branches = 0;
    unsigned promotedPredictions = 0;
    unsigned promotedUops = 0;
    bool complete = false;
};

struct APFStats : statistics::Group
{
    statistics::Scalar candidates, started, buffered, generatedUops;
    statistics::Scalar replayedUops, committedUops, recovered, recoveredCommitted;
    statistics::Scalar committedConditionalMisses;
    statistics::Scalar partialRecoveries, discarded, fullCycles, ftqWaitCycles;
    statistics::Scalar replayBlockedCycles, replayBlockedWithRecoveryCycles;
    statistics::Scalar activeCycles, sampledCycles;
    statistics::Scalar bufferOccupancy, bufferIdleCycles, predictorQueries;
    statistics::Scalar retiredPaths;
    statistics::Scalar generatedMicroops, crossPageReads, compressedUops;
    statistics::Vector stops, fallbacks, savedOccupancy;
    statistics::Formula replayCommitRatio, bufferIdleRatio, recoveryCoverage;
    statistics::Formula activeOccupancyRatio, meanSavedOccupancy;
    APFStats(statistics::Group *parent);
};

} // namespace gem5::o3
#endif
