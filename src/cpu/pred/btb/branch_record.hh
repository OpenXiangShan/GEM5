#ifndef __CPU_PRED_BTB_BRANCH_RECORD_HH__
#define __CPU_PRED_BTB_BRANCH_RECORD_HH__

#include <cstdint>

#include "base/types.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

struct ResolvedBranch
{
    Addr pc = 0;
    Addr target = 0;
    bool taken = false;
    bool mispred = false;
    bool isCond = false;
    bool isIndirect = false;
    bool isDirect = false;
    bool isCall = false;
    bool isReturn = false;
    uint8_t size = 0;
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_BRANCH_RECORD_HH__
