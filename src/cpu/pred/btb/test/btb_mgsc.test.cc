#include <gtest/gtest.h>

#include <vector>

#include <boost/dynamic_bitset.hpp>

#include "cpu/pred/btb/btb_mgsc.hh"

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

BTBEntry
makeCondBTBEntry(Addr pc)
{
    BTBEntry entry;
    entry.pc = pc;
    entry.target = pc + 4;
    entry.isCond = true;
    entry.valid = true;
    entry.alwaysTaken = false;
    entry.size = 4;
    return entry;
}

std::pair<unsigned, unsigned>
lineLaneForHistIndex(BTBMGSC &mgsc, Addr start_pc, Addr branch_pc, unsigned table_idx_width)
{
    const unsigned num_bits = BTBMGSC::TestAccess::numCtrsPerLineBits(mgsc);
    const unsigned idx_bits = table_idx_width - num_bits;
    Addr line_idx = BTBMGSC::TestAccess::getHistIndex(mgsc, start_pc, idx_bits, /*foldedHist=*/0);
    auto [idx1, idx2] = BTBMGSC::TestAccess::posHash(mgsc, branch_pc, line_idx);
    return {idx1, idx2};
}

std::pair<unsigned, unsigned>
lineLaneForBiasIndex(BTBMGSC &mgsc, Addr start_pc, Addr branch_pc, unsigned bias_idx_width,
                     const TageInfoForMGSC &tage_info)
{
    const unsigned num_bits = BTBMGSC::TestAccess::numCtrsPerLineBits(mgsc);
    const unsigned idx_bits = bias_idx_width - num_bits;
    Addr line_idx = BTBMGSC::TestAccess::getBiasIndex(
        mgsc, start_pc, idx_bits, tage_info.tage_main_taken, tage_info.tage_pred_conf_low);
    auto [idx1, idx2] = BTBMGSC::TestAccess::posHash(mgsc, branch_pc, line_idx);
    return {idx1, idx2};
}

void
setAllTableCountersForPc(BTBMGSC &mgsc, Addr start_pc, Addr branch_pc, const TageInfoForMGSC &tage_info,
                         int16_t bw_ctr, int16_t l_ctr, int16_t i_ctr, int16_t g_ctr, int16_t p_ctr, int16_t bias_ctr)
{
    auto &bw_table = BTBMGSC::TestAccess::bwTable(mgsc);
    auto &l_table = BTBMGSC::TestAccess::lTable(mgsc);
    auto &i_table = BTBMGSC::TestAccess::iTable(mgsc);
    auto &g_table = BTBMGSC::TestAccess::gTable(mgsc);
    auto &p_table = BTBMGSC::TestAccess::pTable(mgsc);
    auto &bias_table = BTBMGSC::TestAccess::biasTable(mgsc);

    const auto [bw_i1, bw_i2] =
        lineLaneForHistIndex(mgsc, start_pc, branch_pc, BTBMGSC::TestAccess::bwTableIdxWidth(mgsc));
    const auto [l_i1, l_i2] =
        lineLaneForHistIndex(mgsc, start_pc, branch_pc, BTBMGSC::TestAccess::lTableIdxWidth(mgsc));
    const auto [i_i1, i_i2] =
        lineLaneForHistIndex(mgsc, start_pc, branch_pc, BTBMGSC::TestAccess::iTableIdxWidth(mgsc));
    const auto [g_i1, g_i2] =
        lineLaneForHistIndex(mgsc, start_pc, branch_pc, BTBMGSC::TestAccess::gTableIdxWidth(mgsc));
    const auto [p_i1, p_i2] =
        lineLaneForHistIndex(mgsc, start_pc, branch_pc, BTBMGSC::TestAccess::pTableIdxWidth(mgsc));
    const auto [bias_i1, bias_i2] =
        lineLaneForBiasIndex(mgsc, start_pc, branch_pc, BTBMGSC::TestAccess::biasTableIdxWidth(mgsc), tage_info);

    bw_table[0][bw_i1][bw_i2] = bw_ctr;
    l_table[0][l_i1][l_i2] = l_ctr;
    i_table[0][i_i1][i_i2] = i_ctr;
    g_table[0][g_i1][g_i2] = g_ctr;
    p_table[0][p_i1][p_i2] = p_ctr;
    bias_table[0][bias_i1][bias_i2] = bias_ctr;
}

std::pair<bool, bool>
findCondTaken(const CondTakens &condTakens, Addr pc)
{
    auto it = CondTakens_find(condTakens, pc);
    if (it == condTakens.end()) {
        return {false, false};
    }
    return {true, it->second};
}

} // namespace

TEST(BTBMGSCTest, CanConstructAndCreateMetaOnEmptyInput)
{
    BTBMGSC mgsc;

    Addr start_pc = 0x1000;
    boost::dynamic_bitset<> history(64, 0);
    std::vector<FullBTBPrediction> stage_preds(2);
    for (auto &pred : stage_preds) {
        pred.bbStart = start_pc;
    }

    mgsc.putPCHistory(start_pc, history, stage_preds);

    auto meta = mgsc.getPredictionMeta();
    EXPECT_NE(meta, nullptr);
    EXPECT_TRUE(stage_preds[0].condTakens.empty());
    EXPECT_TRUE(stage_preds[1].condTakens.empty());
}

TEST(BTBMGSCTest, GateHighConfUsesSCWhenStrong)
{
    BTBMGSC mgsc;
    Addr start_pc = 0x1000;
    Addr branch_pc = 0x1000;
    auto entry = makeCondBTBEntry(branch_pc);

    boost::dynamic_bitset<> history(64, 0);
    std::vector<FullBTBPrediction> stage_preds(2);
    for (auto &pred : stage_preds) {
        pred.bbStart = start_pc;
        pred.btbEntries = {entry};
        pred.tageInfoForMgscs[branch_pc] = TageInfoForMGSC(
            /*tage_pred_taken=*/false,
            /*tage_main_taken=*/false,
            /*tage_pred_conf_high=*/true,
            /*tage_pred_conf_mid=*/false,
            /*tage_pred_conf_low=*/false,
            /*tage_pred_alt_diff=*/false);
    }

    const auto &tage_info = stage_preds[0].tageInfoForMgscs[branch_pc];
    // total_sum = 9 + 9 + 1 + 1 - 1 - 1 = 18, total_thres = 35 => high-conf uses SC when abs(sum) > thres/2 (=17)
    setAllTableCountersForPc(mgsc, start_pc, branch_pc, tage_info,
                             /*bw=*/4, /*l=*/4, /*i=*/0, /*g=*/0, /*p=*/-1, /*bias=*/-1);

    mgsc.putPCHistory(start_pc, history, stage_preds);

    auto [found, taken] = findCondTaken(stage_preds[1].condTakens, branch_pc);
    ASSERT_TRUE(found);
    EXPECT_TRUE(taken); // overridden by SC

    const auto &preds = BTBMGSC::TestAccess::preds(mgsc);
    auto it = preds.find(branch_pc);
    ASSERT_NE(it, preds.end());
    EXPECT_TRUE(it->second.use_mgsc);
}

TEST(BTBMGSCTest, GateHighConfBypassWhenWeak)
{
    BTBMGSC mgsc;
    Addr start_pc = 0x1000;
    Addr branch_pc = 0x1000;
    auto entry = makeCondBTBEntry(branch_pc);

    boost::dynamic_bitset<> history(64, 0);
    std::vector<FullBTBPrediction> stage_preds(2);
    for (auto &pred : stage_preds) {
        pred.bbStart = start_pc;
        pred.btbEntries = {entry};
        pred.tageInfoForMgscs[branch_pc] = TageInfoForMGSC(
            /*tage_pred_taken=*/false,
            /*tage_main_taken=*/false,
            /*tage_pred_conf_high=*/true,
            /*tage_pred_conf_mid=*/false,
            /*tage_pred_conf_low=*/false,
            /*tage_pred_alt_diff=*/false);
    }

    const auto &tage_info = stage_preds[0].tageInfoForMgscs[branch_pc];
    // total_sum = 9 + 9 + 1 + 1 - 1 - 3 = 16, total_thres = 35 => high-conf bypass when abs(sum) <= 17
    setAllTableCountersForPc(mgsc, start_pc, branch_pc, tage_info,
                             /*bw=*/4, /*l=*/4, /*i=*/0, /*g=*/0, /*p=*/-1, /*bias=*/-2);

    mgsc.putPCHistory(start_pc, history, stage_preds);

    auto [found, taken] = findCondTaken(stage_preds[1].condTakens, branch_pc);
    ASSERT_TRUE(found);
    EXPECT_FALSE(taken); // fall back to tage_pred_taken

    const auto &preds = BTBMGSC::TestAccess::preds(mgsc);
    auto it = preds.find(branch_pc);
    ASSERT_NE(it, preds.end());
    EXPECT_FALSE(it->second.use_mgsc);
}

TEST(BTBMGSCTest, ForceUseSCOverridesTage)
{
    BTBMGSC mgsc;
    BTBMGSC::TestAccess::forceUseSC(mgsc) = true;

    Addr start_pc = 0x1000;
    Addr branch_pc = 0x1000;
    auto entry = makeCondBTBEntry(branch_pc);

    boost::dynamic_bitset<> history(64, 0);
    std::vector<FullBTBPrediction> stage_preds(2);
    for (auto &pred : stage_preds) {
        pred.bbStart = start_pc;
        pred.btbEntries = {entry};
        pred.tageInfoForMgscs[branch_pc] = TageInfoForMGSC(
            /*tage_pred_taken=*/true,
            /*tage_main_taken=*/true,
            /*tage_pred_conf_high=*/true,
            /*tage_pred_conf_mid=*/false,
            /*tage_pred_conf_low=*/false,
            /*tage_pred_alt_diff=*/false);
    }

    const auto &tage_info = stage_preds[0].tageInfoForMgscs[branch_pc];
    // total_sum = 1 + 1 + 1 + 1 - 3 - 3 = -2 => forceUseSC makes final pred not-taken.
    setAllTableCountersForPc(mgsc, start_pc, branch_pc, tage_info,
                             /*bw=*/0, /*l=*/0, /*i=*/0, /*g=*/0, /*p=*/-2, /*bias=*/-2);

    mgsc.putPCHistory(start_pc, history, stage_preds);

    auto [found, taken] = findCondTaken(stage_preds[1].condTakens, branch_pc);
    ASSERT_TRUE(found);
    EXPECT_FALSE(taken);

    const auto &preds = BTBMGSC::TestAccess::preds(mgsc);
    auto it = preds.find(branch_pc);
    ASSERT_NE(it, preds.end());
    EXPECT_TRUE(it->second.use_mgsc);
}

TEST(BTBMGSCTest, UpdateOnlyOnWrongOrLowMargin)
{
    BTBMGSC mgsc;
    Addr start_pc = 0x1000;
    Addr branch_pc = 0x1000;
    auto entry = makeCondBTBEntry(branch_pc);

    boost::dynamic_bitset<> history(64, 0);
    std::vector<FullBTBPrediction> stage_preds(2);
    for (auto &pred : stage_preds) {
        pred.bbStart = start_pc;
        pred.btbEntries = {entry};
        pred.tageInfoForMgscs[branch_pc] = TageInfoForMGSC(
            /*tage_pred_taken=*/false,
            /*tage_main_taken=*/false,
            /*tage_pred_conf_high=*/true,
            /*tage_pred_conf_mid=*/false,
            /*tage_pred_conf_low=*/false,
            /*tage_pred_alt_diff=*/false);
    }

    const auto &tage_info = stage_preds[0].tageInfoForMgscs[branch_pc];
    // Make a very confident SC prediction (large positive sum).
    setAllTableCountersForPc(mgsc, start_pc, branch_pc, tage_info,
                             /*bw=*/31, /*l=*/31, /*i=*/0, /*g=*/0, /*p=*/0, /*bias=*/0);

    // Prediction
    mgsc.putPCHistory(start_pc, history, stage_preds);
    auto meta = mgsc.getPredictionMeta();

    const auto [bw_i1, bw_i2] =
        lineLaneForHistIndex(mgsc, start_pc, branch_pc, BTBMGSC::TestAccess::bwTableIdxWidth(mgsc));
    auto &bw_table = BTBMGSC::TestAccess::bwTable(mgsc);
    int16_t before = bw_table[0][bw_i1][bw_i2];

    // Update with correct outcome (taken): should NOT train because abs(sum) >= total_thres and prediction correct.
    {
        FetchStream stream;
        stream.startPC = start_pc;
        stream.updateBTBEntries = {entry};
        stream.updateIsOldEntry = true;
        stream.resolved = true;
        stream.exeBranchInfo = entry;
        stream.exeTaken = true;
        stream.predMetas[mgsc.getComponentIdx()] = meta;
        mgsc.update(stream);
        EXPECT_EQ(bw_table[0][bw_i1][bw_i2], before);
    }

    // Update with wrong outcome (not taken): should train (decrement signed counter).
    {
        FetchStream stream;
        stream.startPC = start_pc;
        stream.updateBTBEntries = {entry};
        stream.updateIsOldEntry = true;
        stream.resolved = true;
        stream.exeBranchInfo = entry;
        stream.exeTaken = false;
        stream.predMetas[mgsc.getComponentIdx()] = meta;
        mgsc.update(stream);
        EXPECT_EQ(bw_table[0][bw_i1][bw_i2], static_cast<int16_t>(before - 1));
    }
}

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5
