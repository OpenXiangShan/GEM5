#ifndef __CPU_PRED_BTB_BRANCH_INFO_HH__
#define __CPU_PRED_BTB_BRANCH_INFO_HH__

#include "base/logging.hh"
#include "base/types.hh"
#include "cpu/static_inst.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{

enum BranchType
{
    BR_COND=0,
    BR_DIRECT_NORMAL=1,
    BR_DIRECT_CALL=2,
    BR_INDIRECT_NORMAL=3,
    BR_INDIRECT_RET=4,
    BR_INDIRECT_CALL=5,
    BR_INDIRECT_CALL_RET=6,
    BR_DIRECT_RET=7
};

/** Shared branch classification. Conditional and direct may both be true. */
struct BranchAttribute
{
    bool isCond = false;
    bool isIndirect = false;
    bool isDirect = false;
    bool isCall = false;
    bool isReturn = false;

    bool isUncond() const { return !isCond; }
};

/** Branch identity and destination, independent of prediction or execution. */
struct BranchInfo : BranchAttribute
{
    Addr pc = 0;
    Addr target = 0;
    uint8_t size = 0;

    BranchInfo() = default;
    BranchInfo(Addr pc, Addr target, const BranchAttribute &attribute,
               uint8_t size)
        : BranchAttribute(attribute), pc(pc), target(target), size(size)
    {}

    BranchInfo(const Addr &control_pc, const Addr &target_pc,
               const StaticInstPtr &inst, unsigned size)
        : BranchInfo(control_pc, target_pc,
                     BranchAttribute{inst->isCondCtrl(), inst->isIndirectCtrl(),
                         inst->isDirectCtrl(), inst->isCall(),
                         inst->isReturn() && !inst->isNonSpeculative() &&
                             !inst->isDirectCtrl()}, size)
    {}

    Addr getEnd() const { return pc + size; }
    int getType() const {
        if (isCond) {
            return BR_COND;
        } else if (!isIndirect) { // uncond direct
            if (isReturn) {
                fatal("jal return detected!\n");
                return BR_DIRECT_RET;
            }
            if (!isCall) {
                return BR_DIRECT_NORMAL;
            } else {
                return BR_DIRECT_CALL;
            }
        } else {  // uncond indirect
            if (!isCall) {
                if (!isReturn) {
                    return BR_INDIRECT_NORMAL; // normal indirect
                } else {
                    return BR_INDIRECT_RET; // indirect return
                }
            } else {
                if (!isReturn) { // indirect call
                    return BR_INDIRECT_CALL;
                } else { // call & return
                    return BR_INDIRECT_CALL_RET;
                }
            }
        }
    }

    bool operator < (const BranchInfo &other) const
    {
        return this->pc < other.pc;
    }

    bool operator == (const BranchInfo &other) const
    {
        return this->pc == other.pc;
    }

    bool operator > (const BranchInfo &other) const
    {
        return this->pc > other.pc;
    }

    bool operator != (const BranchInfo &other) const
    {
        return this->pc != other.pc;
    }
};

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_BRANCH_INFO_HH__
