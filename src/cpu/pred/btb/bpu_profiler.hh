#ifndef __CPU_PRED_BTB_BPU_PROFILER_HH__
#define __CPU_PRED_BTB_BPU_PROFILER_HH__

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cpu/pred/btb/common.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

/** Owns CSV branch profiles and phase snapshots across all BPU threads. */
class BPUProfiler
{
  public:
    BPUProfiler(const std::string &name, unsigned maxInstsNum);

    /** Return whether this is the first observation of the branch PC/type. */
    bool recordBranch(const FetchTarget &entry, Addr branchAddr,
                      const BranchInfo &info, bool taken, bool mispred);

    /** Return whether a BTB start PC is newly observed. */
    bool recordFetchBlock(const FetchTarget &target,
                          const PreparedUpdate &update);

    void recordInstructionCommit();
    void dump();

  private:
    enum MispredType
    {
        DIR_WRONG,
        TARGET_WRONG,
        NO_PRED,
        FAKE_LAST
    };

    struct BranchStats
    {
        Addr pc = 0;
        int branchType = 0;
        int totalCount = 0;
        int mispredCount = 0;
        int dirWrongCount = 0;
        int targetWrongCount = 0;
        int noPredCount = 0;

        BranchStats() = default;
        BranchStats(Addr pc, int type) : pc(pc), branchType(type) {}

        void incrementTotal() { totalCount++; }
        void incrementMispred(MispredType type)
        {
            mispredCount++;
            switch (type) {
              case DIR_WRONG: dirWrongCount++; break;
              case TARGET_WRONG: targetWrongCount++; break;
              case NO_PRED: noPredCount++; break;
              default: break;
            }
        }

        double getMispredRate() const
        {
            return totalCount > 0 ? (double)(mispredCount * 1000) / totalCount : 0;
        }
    };

    using BranchKey = std::pair<Addr, int>;
    using BranchStatsMap = std::map<BranchKey, BranchStats>;

    const std::string objectName;
    const unsigned maxInstsNum;

    const std::string &name() const { return objectName; }

    // Cumulative observations, also used to derive per-phase deltas.
    BranchStatsMap topMispredictsByBranch;
    std::map<std::pair<Addr, Addr>, int> topMispredicts;
    std::map<uint64_t, uint64_t> topMispredHist;
    std::map<Addr, unsigned> topMispredIndirect;
    std::unordered_map<Addr, std::pair<BTBEntry, int>> totalBTBEntries;

    // Completed phases and independent snapshots for main/sub-phase deltas.
    BranchStatsMap lastPhaseTopMispredictsByBranch;
    BranchStatsMap lastSubPhaseTopMispredictsByBranch;
    std::vector<BranchStatsMap> topMispredictsByBranchByPhase;
    std::vector<BranchStatsMap> topMispredictsByBranchBySubPhase;
    std::unordered_map<Addr, int> currentPhaseTakenBranches;
    std::unordered_map<Addr, int> currentSubPhaseTakenBranches;
    std::vector<std::unordered_map<Addr, int>> takenBranchesByPhase;
    std::vector<std::unordered_map<Addr, int>> takenBranchesBySubPhase;
    std::unordered_map<Addr, std::pair<BTBEntry, int>> lastPhaseBTBEntries;
    std::vector<std::unordered_map<Addr, std::pair<BTBEntry, int>>>
        BTBEntriesByPhase;

    int phaseIdToDump = 1;
    int numInstCommitted = 0;
    int phaseSizeByInst = 100000;
    int subPhaseIdToDump = 1;
    int subPhaseRatio = 10;

    // FTQ instruction distributions and snapshots for completed main phases.
    std::vector<int> commitFtqEntryHasInstsVector;
    std::vector<int> lastPhaseFtqEntryNumCommittedInstDist;
    std::vector<std::vector<int>> ftqEntryNumCommittedInstDistByPhase;
    std::vector<int> commitFtqEntryFetchedInstsVector;
    std::vector<int> lastPhaseFtqEntryNumFetchedInstDist;
    std::vector<std::vector<int>> ftqEntryNumFetchedInstDistByPhase;

    int subPhaseSizeByInst() const { return phaseSizeByInst / subPhaseRatio; }
    void trackTakenBranch(Addr branchAddr);
    bool processPhase(bool isSubPhase, int phaseID, int &phaseToDump,
                      BranchStatsMap &lastPhaseStats,
                      std::vector<BranchStatsMap> &phaseStatsList,
                      std::unordered_map<Addr, int> &currentPhaseBranches,
                      std::vector<std::unordered_map<Addr, int>> &phaseBranchesList);
    void processFetchDistributions(std::vector<int> &committed,
                                   std::vector<int> &fetched);
    std::unordered_map<Addr, std::pair<BTBEntry, int>> processBTBEntries();
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_BPU_PROFILER_HH__
