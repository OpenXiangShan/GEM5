#ifndef __CPU_PRED_BTB_FDIP_TARGET_HH__
#define __CPU_PRED_BTB_FDIP_TARGET_HH__

#include <memory>

#include "base/types.hh"
#include "cpu/o3/limits.hh"
#include "cpu/pred/btb/common.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

struct FdipFetchTarget
{
    ThreadID tid;
    FetchTargetId id;
    Addr startPC;
    Addr predEndPC;
    Tick predTick;
    uint64_t distanceFromFetchHead;

    FdipFetchTarget(ThreadID _tid, FetchTargetId _id, Addr _start_pc,
                    Addr _pred_end_pc, Tick _pred_tick,
                    uint64_t _distance_from_fetch_head)
        : tid(_tid),
          id(_id),
          startPC(_start_pc),
          predEndPC(_pred_end_pc),
          predTick(_pred_tick),
          distanceFromFetchHead(_distance_from_fetch_head)
    {}

    FdipFetchTarget(const FetchTarget &target, FetchTargetId _id,
                    uint64_t _distance_from_fetch_head)
        : FdipFetchTarget(target.tid, _id, target.startPC,
                          target.predEndPC, target.predTick,
                          _distance_from_fetch_head)
    {}
};

using FdipFetchTargetPtr = std::shared_ptr<FdipFetchTarget>;

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_FDIP_TARGET_HH__
