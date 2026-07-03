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

inline bool
endsResolvedUpdatePrefix(const ResolvedBranch &branch)
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
makeResolvedUpdateBranches(const std::vector<ResolvedBranch> &branches)
{
    std::vector<ResolvedBranch> update_branches;
    for (const auto &branch : branches) {
        update_branches.push_back(branch);
        if (endsResolvedUpdatePrefix(branch)) {
            break;
        }
    }
    return update_branches;
}

inline Addr
resolvedUpdatePrefixBoundaryPC(const std::vector<ResolvedBranch> &branches)
{
    Addr boundary = 0;
    for (const auto &branch : branches) {
        boundary = branch.pc;
        if (endsResolvedUpdatePrefix(branch)) {
            break;
        }
    }
    return boundary;
}

} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5

#endif // __CPU_PRED_BTB_BRANCH_RECORD_HH__
