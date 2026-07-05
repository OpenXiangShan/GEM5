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

    constexpr Addr branch_pc = 0x1008;
    constexpr Addr branch_target = 0x2040;

    DirectionHistoryUpdate predicted_ghr;
    PathHistoryUpdate predicted_phr;
    predicted_phr.taken = false;
    manager.addSpeculativeHist(0x1000, base_ghr, base_phr,
                               predicted_ghr, predicted_phr,
                               false, false, branch_pc + 4, 1);

    DirectionHistoryUpdate actual_ghr;
    actual_ghr.shamt = 0;
    actual_ghr.taken = false;

    PathHistoryUpdate actual_phr;
    actual_phr.taken = true;
    actual_phr.pc = branch_pc;
    actual_phr.target = branch_target;
    EXPECT_TRUE(actual_phr.taken);
    EXPECT_EQ(actual_phr.pc, branch_pc);
    EXPECT_EQ(actual_phr.target, branch_target);

    const auto actual_branch =
        makeResolvedBranch(branch_pc, branch_target, true, false);
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

    constexpr Addr first_pc = 0x1008;
    constexpr Addr first_target = 0x2040;
    constexpr Addr younger_pc = 0x1010;
    constexpr Addr younger_target = 0x3000;

    DirectionHistoryUpdate first_ghr;
    PathHistoryUpdate first_phr;
    first_phr.taken = true;
    first_phr.pc = first_pc;
    first_phr.target = first_target;

    manager.addSpeculativeHist(0x1000, base_ghr, base_phr,
                               first_ghr, first_phr,
                               false, false, first_pc + 4, 1);

    auto after_first_ghr = base_ghr;
    auto after_first_phr = base_phr;
    applyDirectionHistoryUpdate(after_first_ghr, first_ghr);
    applyPathHistoryUpdate(after_first_phr, first_phr);

    DirectionHistoryUpdate younger_ghr;
    younger_ghr.shamt = 1;
    younger_ghr.taken = true;
    PathHistoryUpdate younger_phr;
    younger_phr.taken = true;
    younger_phr.pc = younger_pc;
    younger_phr.target = younger_target;

    manager.addSpeculativeHist(0x1000, after_first_ghr, after_first_phr,
                               younger_ghr, younger_phr,
                               false, false, younger_pc + 4, 2);

    DirectionHistoryUpdate actual_ghr;
    actual_ghr.shamt = 0;
    actual_ghr.taken = false;
    PathHistoryUpdate actual_phr;
    actual_phr.taken = true;
    actual_phr.pc = first_pc;
    actual_phr.target = first_target;

    const auto actual_branch =
        makeResolvedBranch(first_pc, first_target, true, false);
    manager.squash(1, actual_ghr, actual_phr, &actual_branch);

    auto expected_phr = base_phr;
    applyPathHistoryUpdate(expected_phr, actual_phr);

    auto with_younger_phr = after_first_phr;
    applyPathHistoryUpdate(with_younger_phr, younger_phr);

    EXPECT_TRUE(manager.checkPHist(expected_phr, history_bits));
    EXPECT_FALSE(manager.checkPHist(with_younger_phr, history_bits));
}

TEST(HistoryManagerTest, SpeculativeHistRecordsReturnStackFields)
{
    constexpr unsigned history_bits = 128;
    HistoryManager manager(16);

    boost::dynamic_bitset<> base_ghr(history_bits, 0);
    boost::dynamic_bitset<> base_phr(history_bits, 0);
    DirectionHistoryUpdate ghr_update;
    PathHistoryUpdate phr_update;

    manager.addSpeculativeHist(0x1000, base_ghr, base_phr,
                               ghr_update, phr_update,
                               true, false, 0x1004, 1);

    const auto &entries = manager.getSpeculativeHist();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_TRUE(entries.front().is_call);
    EXPECT_FALSE(entries.front().is_return);
    EXPECT_EQ(entries.front().retAddr, 0x1004);
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
