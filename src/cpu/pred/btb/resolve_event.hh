#ifndef __CPU_PRED_BTB_RESOLVE_EVENT_HH__
#define __CPU_PRED_BTB_RESOLVE_EVENT_HH__

#include <cstdint>

#include "base/types.hh"
#include "cpu/inst_seq.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

using FetchTargetId = uint64_t;

/** Execution facts for one dynamically resolved control-flow instruction. */
struct FullResolveEvent
{
    ThreadID tid = 0;
    FetchTargetId ftqId = 0;
    InstSeqNum seqNum = 0;
    Addr pc = 0;
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

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_RESOLVE_EVENT_HH__
