#ifndef __CPU_O3_BPU_UPDATE_HH__
#define __CPU_O3_BPU_UPDATE_HH__

#include <memory>

#include "arch/riscv/pcstate.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/pred/btb/branch_outcome.hh"

namespace gem5
{

namespace o3
{

/** Convert an O3 dynamic instruction into the stage-neutral BPU protocol. */
inline branch_prediction::btb_pred::BranchOutcome
makeBranchOutcome(const DynInstPtr &inst)
{
    std::unique_ptr<PCStateBase> actual_next(inst->pcState().clone());
    inst->staticInst->advancePC(*actual_next);
    // Keep every field on the same side: the trace-aware accessors either all
    // come from trace metadata or all fall back to the static decode. Mixing
    // them let a synthetic trace encoding report a trace taken bit together
    // with static class bits, which made this consumer disagree with
    // makeBranchInfo's anchored trace overlay (TraceBranchTruth.hh).
    const bool is_direct = inst->isDirectCtrl();
    const Addr resolved_target = is_direct ?
        inst->branchTarget()->instAddr() : actual_next->instAddr();

    return branch_prediction::btb_pred::BranchOutcome{
        inst->threadNumber,
        inst->getFtqId(),
        inst->seqNum,
        inst->getPC(),
        resolved_target,
        inst->isUncondCtrl() || inst->branching(),
        inst->mispredicted(),
        inst->isCondCtrl(),
        inst->isIndirectCtrl(),
        is_direct,
        inst->isCall(),
        inst->isReturn() &&
            !inst->staticInst->isNonSpeculative() &&
            !is_direct,
        static_cast<uint8_t>(inst->getInstBytes())
    };
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_BPU_UPDATE_HH__
