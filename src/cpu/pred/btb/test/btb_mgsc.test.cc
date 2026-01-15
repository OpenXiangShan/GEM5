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

void
histShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history)
{
    if (shamt == 0) {
        return;
    }
    history <<= shamt;
    history[0] = taken;
}

void
pHistShiftIn(int shamt, bool taken, boost::dynamic_bitset<> &history, Addr pc, Addr target)
{
    if (shamt == 0) {
        return;
    }
    if (taken) {
        uint64_t hash = pathHash(pc, target);
        history <<= shamt;
        for (std::size_t i = 0; i < pathHashLength && i < history.size(); i++) {
            history[i] = (hash & 1) ^ history[i];
            hash >>= 1;
        }
    }
}

struct MgscHarness
{
    BTBMGSC mgsc;
    boost::dynamic_bitset<> ghr;
    boost::dynamic_bitset<> phr;
    boost::dynamic_bitset<> bwhr;
    std::vector<boost::dynamic_bitset<>> lhr;
    std::vector<FullBTBPrediction> stage_preds;

    explicit MgscHarness(std::size_t hist_len = 64)
        : mgsc(),
          ghr(hist_len, 0),
          phr(hist_len, 0),
          bwhr(hist_len, 0),
          lhr(mgsc.getNumEntriesFirstLocalHistories(), boost::dynamic_bitset<>(hist_len, 0)),
          stage_preds(2)
    {}

    void
    setOnlyGTable()
    {
        BTBMGSC::TestAccess::enableBwTable(mgsc) = false;
        BTBMGSC::TestAccess::enableLTable(mgsc) = false;
        BTBMGSC::TestAccess::enableITable(mgsc) = false;
        BTBMGSC::TestAccess::enableGTable(mgsc) = true;
        BTBMGSC::TestAccess::enablePTable(mgsc) = false;
        BTBMGSC::TestAccess::enableBiasTable(mgsc) = false;
        BTBMGSC::TestAccess::enablePCThreshold(mgsc) = false;
        BTBMGSC::TestAccess::forceUseSC(mgsc) = true;
    }

    struct StepResult
    {
        bool predicted_taken{false};
        BTBMGSC::MgscPrediction mgsc_pred{};
    };

    StepResult
    step(Addr start_pc, const BTBEntry &entry, const TageInfoForMGSC &tage_info, bool actual_taken)
    {
        for (auto &pred : stage_preds) {
            pred.bbStart = start_pc;
            pred.btbEntries = {entry};
            pred.tageInfoForMgscs.clear();
            pred.tageInfoForMgscs[entry.pc] = tage_info;
        }

        // Prediction
        boost::dynamic_bitset<> ghr_before = ghr;
        boost::dynamic_bitset<> phr_before = phr;
        boost::dynamic_bitset<> bwhr_before = bwhr;
        auto lhr_before = lhr;

        mgsc.putPCHistory(start_pc, ghr, stage_preds);
        auto meta = mgsc.getPredictionMeta();

        auto [found, pred_taken] = findCondTaken(stage_preds[1].condTakens, entry.pc);
        EXPECT_TRUE(found);

        // Snapshot per-branch MGSC prediction info (indexes/percsums/thresholds).
        StepResult result;
        result.predicted_taken = pred_taken;
        {
            const auto &preds = BTBMGSC::TestAccess::preds(mgsc);
            auto it = preds.find(entry.pc);
            EXPECT_NE(it, preds.end());
            if (it != preds.end()) {
                result.mgsc_pred = it->second;
            }
        }

        // Speculative folded-history update (use pre-update histories, like DecoupledBPUWithBTB does).
        mgsc.specUpdateHist(ghr_before, stage_preds[1]);
        mgsc.specUpdatePHist(phr_before, stage_preds[1]);
        mgsc.specUpdateBwHist(bwhr_before, stage_preds[1]);
        mgsc.specUpdateIHist(stage_preds[1]);
        mgsc.specUpdateLHist(lhr_before, stage_preds[1]);

        // Speculative external history update using predicted outcome.
        int shamt;
        bool cond_taken;
        std::tie(shamt, cond_taken) = stage_preds[1].getHistInfo();
        histShiftIn(shamt, cond_taken, ghr);

        int bw_shamt;
        bool bw_taken;
        std::tie(bw_shamt, bw_taken) = stage_preds[1].getBwHistInfo();
        histShiftIn(bw_shamt, bw_taken, bwhr);

        auto [p_pc, p_target, p_taken] = stage_preds[1].getPHistInfo();
        pHistShiftIn(2, p_taken, phr, p_pc, p_target);

        unsigned lhr_idx =
            mgsc.getPcIndex(stage_preds[1].bbStart, log2(mgsc.getNumEntriesFirstLocalHistories()));
        histShiftIn(shamt, cond_taken, lhr[lhr_idx]);

        // If mispredicted, recover folded histories and external histories with actual outcome.
        if (pred_taken != actual_taken) {
            ghr = ghr_before;
            phr = phr_before;
            bwhr = bwhr_before;
            lhr = lhr_before;

            FetchStream recover_stream;
            recover_stream.startPC = start_pc;
            recover_stream.predMetas[mgsc.getComponentIdx()] = meta;

            mgsc.recoverHist(ghr, recover_stream, shamt, actual_taken);
            mgsc.recoverPHist(phr, recover_stream, 2, actual_taken);

            bool actual_bw_taken = actual_taken && (entry.target < entry.pc);
            mgsc.recoverBwHist(bwhr, recover_stream, bw_shamt, actual_bw_taken);
            mgsc.recoverIHist(recover_stream, bw_shamt, actual_bw_taken);
            mgsc.recoverLHist(lhr, recover_stream, shamt, actual_taken);

            // Apply correct external history update.
            histShiftIn(shamt, actual_taken, ghr);
            histShiftIn(bw_shamt, actual_bw_taken, bwhr);
            pHistShiftIn(2, actual_taken, phr, entry.pc, entry.target);
            histShiftIn(shamt, actual_taken, lhr[lhr_idx]);
        }

        // Training update using prediction meta
        FetchStream update_stream;
        update_stream.startPC = start_pc;
        update_stream.updateBTBEntries = {entry};
        update_stream.updateIsOldEntry = true;
        update_stream.resolved = true;
        update_stream.exeBranchInfo = entry;
        update_stream.exeTaken = actual_taken;
        update_stream.predMetas[mgsc.getComponentIdx()] = meta;
        mgsc.update(update_stream);

        return result;
    }
};

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

TEST(BTBMGSCTest, GTableLearnsAlternatingPattern)
{
    MgscHarness h;
    h.setOnlyGTable();

    const Addr start_pc = 0x1000;
    const Addr branch_pc = 0x1000;
    auto entry = makeCondBTBEntry(branch_pc);

    // TAGE info is required by MGSC, but forceUseSC makes final decision depend on SC only.
    const TageInfoForMGSC tage_info(
        /*tage_pred_taken=*/false,
        /*tage_main_taken=*/false,
        /*tage_pred_conf_high=*/true,
        /*tage_pred_conf_mid=*/false,
        /*tage_pred_conf_low=*/false,
        /*tage_pred_alt_diff=*/false);

    // Alternating pattern: T, N, T, N... This is a classic case where pure bias can't do better than 50%,
    // while a global-history indexed table can learn distinct counters for different GHR contexts.
    const int iters = 200;
    const int warmup = 100;
    int correct_after_warmup = 0;
    int total_after_warmup = 0;
    std::set<unsigned> seen_g_indices;

    for (int i = 0; i < iters; ++i) {
        bool actual_taken = (i % 2) == 0;
        auto step = h.step(start_pc, entry, tage_info, actual_taken);

        if (!step.mgsc_pred.gIndex.empty()) {
            seen_g_indices.insert(step.mgsc_pred.gIndex[0]);
        }

        if (i >= warmup) {
            total_after_warmup++;
            if (step.predicted_taken == actual_taken) {
                correct_after_warmup++;
            }
        }
    }

    // Ensure the global table actually observes more than one context (history affects index).
    EXPECT_GE(seen_g_indices.size(), 2u);

    // After enough training, accuracy should be noticeably better than a constant predictor (~50%).
    double acc = static_cast<double>(correct_after_warmup) / static_cast<double>(total_after_warmup);
    EXPECT_GE(acc, 0.80) << "Accuracy too low for alternating pattern: " << acc;
}

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5
