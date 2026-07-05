#ifndef __CPU_PRED_BTB_BRANCH_RECORD_HH__
#define __CPU_PRED_BTB_BRANCH_RECORD_HH__

#include <algorithm>
#include <cstdint>
#include <vector>

#include "base/types.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

// Actual CFI result captured when a branch resolves. Resolve update consumes it
// immediately; commit update may consume the same fact later.
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
    bool isNonSpeculative = false;
    bool isNop = false;
    uint8_t size = 0;
};

template <typename InstPtr>
void
fillResolvedBranchInstAttrs(ResolvedBranch &branch, const InstPtr &inst,
                            uint8_t size)
{
    branch.isCond = inst->isCondCtrl();
    branch.isIndirect = inst->isIndirectCtrl();
    branch.isDirect = inst->isDirectCtrl();
    branch.isCall = inst->isCall();
    branch.isReturn = inst->isReturn() &&
        !inst->isNonSpeculative() && !inst->isDirectCtrl();
    branch.isNonSpeculative = inst->isNonSpeculative();
    branch.isNop = inst->isNop();
    branch.size = size;
}

template <typename InstPtr>
ResolvedBranch
makeActualBranchFromInst(const InstPtr &inst)
{
    ResolvedBranch branch;
    branch.pc = inst->getPC();
    branch.target = inst->getNPC();
    branch.taken = inst->branching() || inst->isUncondCtrl();
    branch.mispred = inst->mispredicted();
    fillResolvedBranchInstAttrs(
        branch, inst, static_cast<uint8_t>(inst->getInstBytes()));
    return branch;
}

inline bool
endsUpdateBranchPrefix(const ResolvedBranch &branch)
{
    return branch.taken || branch.mispred;
}

inline bool
insertResolvedBranchByPC(
    std::vector<ResolvedBranch> &branches,
    const ResolvedBranch &branch)
{
    auto it = std::lower_bound(
        branches.begin(), branches.end(), branch.pc,
        [](const auto &queued, Addr pc) { return queued.pc < pc; });
    if (it != branches.end() && it->pc == branch.pc) {
        return false;
    }
    branches.insert(it, branch);
    return true;
}

inline size_t
insertResolvedBranchesByPC(
    std::vector<ResolvedBranch> &branches,
    const std::vector<ResolvedBranch> &incoming)
{
    size_t inserted = 0;
    for (const auto &branch : incoming) {
        if (insertResolvedBranchByPC(branches, branch)) {
            inserted++;
        }
    }
    return inserted;
}

inline std::vector<ResolvedBranch>
makeUpdateBranchPrefix(const std::vector<ResolvedBranch> &branches)
{
    std::vector<ResolvedBranch> update_branches;
    for (const auto &branch : branches) {
        update_branches.push_back(branch);
        if (endsUpdateBranchPrefix(branch)) {
            break;
        }
    }
    return update_branches;
}

inline Addr
updateBranchPrefixBoundaryPC(const std::vector<ResolvedBranch> &branches)
{
    Addr boundary = 0;
    for (const auto &branch : branches) {
        boundary = branch.pc;
        if (endsUpdateBranchPrefix(branch)) {
            break;
        }
    }
    return boundary;
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_BRANCH_RECORD_HH__
