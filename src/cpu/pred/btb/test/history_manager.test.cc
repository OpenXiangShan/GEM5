#include <gtest/gtest.h>

#include <boost/dynamic_bitset.hpp>

#include "cpu/pred/btb/history_manager.hh"

namespace gem5
{

namespace branch_prediction
{

namespace btb_pred
{

namespace test
{

namespace
{

void
applyPathHistoryUpdate(boost::dynamic_bitset<> &history,
                       const PathHistoryUpdate &update)
{
    if (!update.taken || update.shamt <= 0) {
        return;
    }

    history <<= update.shamt;
    uint64_t hash = pathHash(update.pc, update.target);
    for (std::size_t i = 0; i < pathHashLength && i < history.size(); ++i) {
        history[i] = (hash & 1) ^ history[i];
        hash >>= 1;
    }
}

void
applyDirectionHistoryUpdate(boost::dynamic_bitset<> &history,
                            const DirectionHistoryUpdate &update)
{
    if (update.shamt <= 0) {
        return;
    }

    history <<= update.shamt;
    history[0] = update.taken;
}

BranchInfo
makeBranchInfo(Addr pc, Addr target, bool is_cond)
{
    BranchInfo info;
    info.pc = pc;
    info.target = target;
    info.isCond = is_cond;
    info.size = 4;
    return info;
}

ResolvedBranch
makeResolvedBranch(Addr pc, Addr target, bool taken, bool is_cond)
{
    ResolvedBranch branch;
    branch.pc = pc;
    branch.target = target;
    branch.taken = taken;
    branch.isCond = is_cond;
    branch.isDirect = is_cond;
    branch.size = 4;
    return branch;
}

} // namespace

TEST(HistoryManagerTest, PathReplayChecksRecordedSquashPathUpdate)
{
    constexpr unsigned history_bits = 128;
    HistoryManager manager(16);

    boost::dynamic_bitset<> base_ghr(history_bits, 0);
    boost::dynamic_bitset<> base_phr(history_bits, 0);
    base_ghr[3] = true;
    base_phr[5] = true;

    auto branch = makeBranchInfo(0x1008, 0x2040, false);

    DirectionHistoryUpdate predicted_ghr;
    PathHistoryUpdate predicted_phr;
    predicted_phr.taken = false;
    manager.addSpeculativeHist(0x1000, base_ghr, base_phr,
                               predicted_ghr, predicted_phr, branch, 1);

    DirectionHistoryUpdate actual_ghr;
    actual_ghr.shamt = 0;
    actual_ghr.taken = false;

    PathHistoryUpdate actual_phr;
    actual_phr.taken = true;
    actual_phr.pc = branch.pc;
    actual_phr.target = branch.target;
    EXPECT_TRUE(actual_phr.taken);
    EXPECT_EQ(actual_phr.pc, branch.pc);
    EXPECT_EQ(actual_phr.target, branch.target);

    const auto actual_branch =
        makeResolvedBranch(branch.pc, branch.target, true, branch.isCond);
    manager.squash(1, actual_ghr, actual_phr, &actual_branch);

    auto correct_ghr = base_ghr;
    auto correct_phr = base_phr;
    applyDirectionHistoryUpdate(correct_ghr, actual_ghr);
    applyPathHistoryUpdate(correct_phr, actual_phr);

    auto missing_path_update_phr = base_phr;

    EXPECT_TRUE(manager.checkGHist(correct_ghr, history_bits));
    EXPECT_TRUE(manager.checkPHist(correct_phr, history_bits));
    EXPECT_FALSE(manager.checkPHist(missing_path_update_phr, history_bits));
}

TEST(HistoryManagerTest, SquashDropsYoungerPathUpdates)
{
    constexpr unsigned history_bits = 128;
    HistoryManager manager(16);

    boost::dynamic_bitset<> base_ghr(history_bits, 0);
    boost::dynamic_bitset<> base_phr(history_bits, 0);

    auto first = makeBranchInfo(0x1008, 0x2040, false);
    auto younger = makeBranchInfo(0x1010, 0x3000, true);

    DirectionHistoryUpdate first_ghr;
    PathHistoryUpdate first_phr;
    first_phr.taken = true;
    first_phr.pc = first.pc;
    first_phr.target = first.target;

    manager.addSpeculativeHist(0x1000, base_ghr, base_phr,
                               first_ghr, first_phr, first, 1);

    auto after_first_ghr = base_ghr;
    auto after_first_phr = base_phr;
    applyDirectionHistoryUpdate(after_first_ghr, first_ghr);
    applyPathHistoryUpdate(after_first_phr, first_phr);

    DirectionHistoryUpdate younger_ghr;
    younger_ghr.shamt = 1;
    younger_ghr.taken = true;
    PathHistoryUpdate younger_phr;
    younger_phr.taken = true;
    younger_phr.pc = younger.pc;
    younger_phr.target = younger.target;

    manager.addSpeculativeHist(0x1000, after_first_ghr, after_first_phr,
                               younger_ghr, younger_phr, younger, 2);

    DirectionHistoryUpdate actual_ghr;
    actual_ghr.shamt = 0;
    actual_ghr.taken = false;
    PathHistoryUpdate actual_phr;
    actual_phr.taken = true;
    actual_phr.pc = first.pc;
    actual_phr.target = first.target;

    const auto actual_branch =
        makeResolvedBranch(first.pc, first.target, true, first.isCond);
    manager.squash(1, actual_ghr, actual_phr, &actual_branch);

    auto expected_phr = base_phr;
    applyPathHistoryUpdate(expected_phr, actual_phr);

    auto with_younger_phr = after_first_phr;
    applyPathHistoryUpdate(with_younger_phr, younger_phr);

    EXPECT_TRUE(manager.checkPHist(expected_phr, history_bits));
    EXPECT_FALSE(manager.checkPHist(with_younger_phr, history_bits));
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
