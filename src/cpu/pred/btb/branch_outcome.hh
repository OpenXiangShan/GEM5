#ifndef __CPU_PRED_BTB_BRANCH_OUTCOME_HH__
#define __CPU_PRED_BTB_BRANCH_OUTCOME_HH__

#include <cstdint>
#include <vector>

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

using FetchTargetId = uint64_t;

/** Actual execution facts for one dynamic control-flow instruction. */
struct BranchOutcome
{
    ThreadID tid = 0;
    FetchTargetId ftqId = 0;
    InstSeqNum seqNum = 0;
    Addr pc = 0;
    /**
     * The control-flow destination, including for a not-taken conditional
     * branch. This is not necessarily the selected next PC.
     */
    Addr target = 0;
    bool taken = false;
    bool mispredicted = false;
    bool isCond = false;
    bool isIndirect = false;
    bool isDirect = false;
    bool isCall = false;
    bool isReturn = false;
    uint8_t size = 0;
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
