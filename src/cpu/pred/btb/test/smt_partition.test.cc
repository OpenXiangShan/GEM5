#include <gtest/gtest.h>

#include "cpu/pred/btb/timed_base_pred.hh"

namespace gem5
{
namespace branch_prediction
{
namespace btb_pred
{
namespace test
{

TEST(SmtPartition, BorrowAndReturnEitherThreadsBank)
{
    TimedBaseBTBPredictor predictor;
    bool single = false;
    predictor.setSmtTidPartitioned(true);
    predictor.setPartitionBorrowing([&single] { return single; });
    for (bool borrow : {false, true, false}) {
        single = borrow;
        // Folded-history width must not change when the lookup range widens.
        EXPECT_EQ(predictor.partitionIndexBits(10), 9);
        EXPECT_EQ(predictor.lookupIndexBits(10), borrow ? 10 : 9);
        for (ThreadID tid : {0, 1}) {
            EXPECT_EQ(predictor.partitionBegin(1024, tid), borrow ? 0 : tid * 512);
            EXPECT_EQ(predictor.partitionEnd(1024, tid), borrow ? 1024 : (tid + 1) * 512);
            for (unsigned index = 0; index < 1024; ++index) {
                EXPECT_EQ(predictor.partitionIndex(index, 1024, tid),
                          borrow ? index : tid * 512 + index % 512);
            }
        }
    }
}

TEST(SmtPartition, UnpartitionedPredictorKeepsItsFullRange)
{
    TimedBaseBTBPredictor predictor;
    predictor.setPartitionBorrowing([] { return true; });
    EXPECT_FALSE(predictor.borrowsPartitions());
    EXPECT_EQ(predictor.partitionIndexBits(10), 10);
    EXPECT_EQ(predictor.partitionIndex(700, 1024, 1), 700);
    EXPECT_EQ(predictor.partitionBegin(1024, 1), 0);
    EXPECT_EQ(predictor.partitionEnd(1024, 1), 1024);
}

} // namespace test
} // namespace btb_pred
} // namespace branch_prediction
} // namespace gem5
