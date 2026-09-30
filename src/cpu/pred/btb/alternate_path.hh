#ifndef __CPU_PRED_BTB_ALTERNATE_PATH_HH__
#define __CPU_PRED_BTB_ALTERNATE_PATH_HH__

#include <functional>

#include "cpu/pred/btb/common.hh"

namespace gem5::branch_prediction::btb_pred
{

struct AlternateCandidate
{
    FetchTargetId ftqId = 0;
    BranchInfo branch;
    bool predictedTaken = false;
};

struct AlternatePrediction
{
    FetchTarget target;
    FullBTBPrediction prediction;
};

// Installs query-local state only. Trained tables remain shared with the CPU.
using AlternateCheckpoint = std::function<void()>;

} // namespace gem5::branch_prediction::btb_pred
#endif
