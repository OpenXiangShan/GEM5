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

}  // namespace test
}  // namespace btb_pred
}  // namespace branch_prediction
}  // namespace gem5

