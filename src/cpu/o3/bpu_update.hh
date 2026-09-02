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
    const Addr resolved_target = inst->staticInst->isDirectCtrl() ?
        inst->branchTarget()->instAddr() : actual_next->instAddr();

    return branch_prediction::btb_pred::BranchOutcome{
        inst->threadNumber,
        inst->getFtqId(),
        inst->seqNum,
        inst->getPC(),
        resolved_target,
        inst->branching(),
        inst->mispredicted(),
        inst->staticInst->isCondCtrl(),
        inst->staticInst->isIndirectCtrl(),
        inst->staticInst->isDirectCtrl(),
        inst->staticInst->isCall(),
        inst->staticInst->isReturn() &&
            !inst->staticInst->isNonSpeculative() &&
            !inst->staticInst->isDirectCtrl(),
        static_cast<uint8_t>(inst->getInstBytes())
    };
}

} // namespace o3
} // namespace gem5

#endif // __CPU_O3_BPU_UPDATE_HH__
