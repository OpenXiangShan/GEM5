#ifndef __CPU_PRED_BTB_BRANCH_OUTCOME_HH__
#define __CPU_PRED_BTB_BRANCH_OUTCOME_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"
#include "cpu/pred/btb/branch_info.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

using FetchTargetId = uint64_t;

/**
 * Actual execution facts for one dynamic control-flow instruction.
 * Inherited target is the branch destination even when a conditional falls
 * through; it is not necessarily the selected next PC.
 */
struct BranchOutcome : BranchInfo
{
    ThreadID tid = 0;
    FetchTargetId ftqId = 0;
    InstSeqNum seqNum = 0;
    bool taken = false;
    bool mispredicted = false;

    BranchOutcome() = default;
    BranchOutcome(ThreadID tid, FetchTargetId ftqId, InstSeqNum seqNum,
                  const BranchInfo &branch, bool taken, bool mispredicted)
        : BranchInfo(branch), tid(tid), ftqId(ftqId), seqNum(seqNum),
          taken(taken), mispredicted(mispredicted)
    {}

};

/** Actual committed contents of one complete FetchTarget. */
struct CommittedFetchBlock
{
    ThreadID tid = 0;
    FetchTargetId ftqId = 0;
    std::vector<BranchOutcome> branches;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_BRANCH_OUTCOME_HH__
