#include <gtest/gtest.h>

#include <memory>

#include "cpu/pred/btb/common.hh"
#include "cpu/pred/btb/microtage.hh"

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
makeCondEntry(Addr pc, int ctr = -1, bool always_taken = false)
{
    BTBEntry entry;
    entry.valid = true;
    entry.pc = pc;
    entry.target = pc + 4;
    entry.size = 4;
    entry.isCond = true;
    entry.ctr = ctr;
    entry.alwaysTaken = always_taken;
    return entry;
}

FullBTBPrediction
makeStagePred(Addr start_pc, const std::vector<BTBEntry> &entries,
              const CondTakens &cond_takens)
{
    FullBTBPrediction pred;
    pred.tid = 0;
    pred.asidHash = 0;
    pred.bbStart = start_pc;
    pred.btbEntries = entries;
    pred.condTakens = cond_takens;
    return pred;
}

} // namespace

class MicroTAGES3UpdateTest : public ::testing::Test
{
  protected:
    void
    SetUp() override
    {
        tage = std::make_unique<MicroTAGE>(4, 1, 32, 4);
        tage->usingS3Pred = true;
    }

    void
    primePrediction(MicroTAGE *tage, const FullBTBPrediction &seed)
    {
        auto copied = seed;
        std::vector<FullBTBPrediction> stage_preds(1);
        stage_preds[0] = copied;
        tage->putPCHistory(seed.bbStart, boost::dynamic_bitset<>(64), stage_preds);
    }

    bool
    predictTaken(MicroTAGE *predictor, Addr start_pc, const BTBEntry &entry)
    {
        std::vector<FullBTBPrediction> stage_preds(1);
        stage_preds[0].tid = 0;
        stage_preds[0].bbStart = start_pc;
        stage_preds[0].btbEntries = {entry};
        predictor->putPCHistory(start_pc, boost::dynamic_bitset<>(64), stage_preds);
        Addr branch_pc = entry.pc;
        auto it = CondTakens_find(stage_preds[0].condTakens, branch_pc);
        return it != stage_preds[0].condTakens.end() && it->second;
    }

    std::unique_ptr<MicroTAGE> tage;
};

TEST_F(MicroTAGES3UpdateTest, FunctionalUpdateBypassedWhenUsingS3Pred)
{
    BTBEntry entry = makeCondEntry(0x1000, -1);
    FullBTBPrediction seed = makeStagePred(0x1000, {entry}, {{entry.pc, false}});
    primePrediction(tage.get(), seed);

    FetchTarget stream;
    stream.startPC = 0x1000;
    stream.exeTaken = true;
    stream.exeBranchInfo = entry;
    PreparedUpdate update;
    update.btbEntries = {entry};
    update.isOldEntry = true;

    tage->update(stream, update);
    EXPECT_FALSE(predictTaken(tage.get(), 0x1000, entry));
}

TEST_F(MicroTAGES3UpdateTest, LearnsFromS3TeacherDirection)
{
    BTBEntry entry = makeCondEntry(0x1000, -1);
    FullBTBPrediction seed = makeStagePred(0x1000, {entry}, {{entry.pc, false}});
    primePrediction(tage.get(), seed);
    bool before = predictTaken(tage.get(), 0x1000, entry);

    FullBTBPrediction teacher = makeStagePred(0x1000, {entry}, {{entry.pc, true}});
    tage->updateUsingS3Pred(teacher);

    bool after = predictTaken(tage.get(), 0x1000, entry);
    EXPECT_FALSE(before);
    EXPECT_TRUE(after);
}

TEST_F(MicroTAGES3UpdateTest, S3NoHitStatsMirrorPredictionAccounting)
{
    BTBEntry entry = makeCondEntry(0x1800, -1);
    FullBTBPrediction seed = makeStagePred(0x1800, {entry}, {{entry.pc, false}});
    primePrediction(tage.get(), seed);

    FullBTBPrediction teacher = makeStagePred(0x1800, {entry}, {{entry.pc, true}});
    tage->updateUsingS3Pred(teacher);

    EXPECT_EQ(tage->tageStats.s3UpdateEntries, 1ULL);
    EXPECT_EQ(tage->tageStats.s3UpdateNoHitUseBim, 1ULL);
    EXPECT_EQ(tage->tageStats.s3UpdateUseAlt, 1ULL);
    EXPECT_EQ(tage->tageStats.updateNoHitUseBim, 0ULL);
    EXPECT_EQ(tage->tageStats.updateUseAlt, 0ULL);
}

TEST_F(MicroTAGES3UpdateTest, StopsTrainingAfterFirstTakenControl)
{
    BTBEntry first = makeCondEntry(0x1000, -1);
    BTBEntry second = makeCondEntry(0x1004, -1);
    FullBTBPrediction seed = makeStagePred(0x1000, {first, second},
                                           {{first.pc, false}, {second.pc, false}});
    primePrediction(tage.get(), seed);

    FullBTBPrediction teacher = makeStagePred(0x1000, {first, second},
                                              {{first.pc, true}, {second.pc, true}});
    tage->updateUsingS3Pred(teacher);

    EXPECT_TRUE(predictTaken(tage.get(), 0x1000, first));
    EXPECT_FALSE(predictTaken(tage.get(), 0x1000, second));
}

TEST_F(MicroTAGES3UpdateTest, MissingPredictionMetaSkipsSafely)
{
    FullBTBPrediction teacher = makeStagePred(0x2000, {makeCondEntry(0x2000)},
                                              {{0x2000, true}});
    EXPECT_NO_THROW(tage->updateUsingS3Pred(teacher));
}

TEST_F(MicroTAGES3UpdateTest, S3AllocationCountersStaySplitFromNormalUpdateCounters)
{
    BTBEntry entry = makeCondEntry(0x3000, -1);
    FullBTBPrediction seed = makeStagePred(0x3000, {entry}, {{entry.pc, false}});
    primePrediction(tage.get(), seed);

    auto meta = tage->threadMeta[0];
    ASSERT_NE(meta, nullptr);

    uint64_t allocated_table = 0;
    uint64_t allocated_index = 0;
    uint64_t allocated_way = 0;
    bool allocated = tage->handleNewEntryAllocation(
        seed.bbStart, entry, true, 0, meta, seed.asidHash,
        MicroTAGE::TrainingMode::S3Update,
        allocated_table, allocated_index, allocated_way);

    EXPECT_TRUE(allocated);
    EXPECT_EQ(tage->tageStats.s3UpdateAllocSuccess, 1ULL);
    EXPECT_EQ(tage->tageStats.updateAllocSuccess, 0ULL);
}

TEST_F(MicroTAGES3UpdateTest, S3AllocationFailureAndResetCountersStaySplit)
{
    BTBEntry entry = makeCondEntry(0x3800, -1);
    FullBTBPrediction seed = makeStagePred(0x3800, {entry}, {{entry.pc, false}});
    primePrediction(tage.get(), seed);

    auto meta = tage->threadMeta[0];
    ASSERT_NE(meta, nullptr);

    const unsigned blocked_table = tage->numPredictors - 1;
    Addr blocked_index = tage->getTageIndex(
        seed.bbStart, blocked_table, meta->indexFoldedHist[blocked_table].get(),
        seed.asidHash);
    auto &blocked_way = tage->tageTable[blocked_table][blocked_index][0];
    blocked_way = MicroTAGE::TageEntry(0x55, 3, entry.pc);
    blocked_way.useful = true;

    tage->usefulResetCnt = 255;

    uint64_t allocated_table = 0;
    uint64_t allocated_index = 0;
    uint64_t allocated_way = 0;
    bool allocated = tage->handleNewEntryAllocation(
        seed.bbStart, entry, true, blocked_table, meta, seed.asidHash,
        MicroTAGE::TrainingMode::S3Update,
        allocated_table, allocated_index, allocated_way);

    EXPECT_FALSE(allocated);
    EXPECT_EQ(tage->tageStats.s3UpdateAllocFailure, 1ULL);
    EXPECT_EQ(tage->tageStats.updateAllocFailure, 0ULL);
    EXPECT_EQ(tage->tageStats.s3UpdateResetU, 1ULL);
    EXPECT_EQ(tage->tageStats.updateResetU, 0ULL);
    EXPECT_EQ(tage->tageStats.s3UpdateAllocFailureNoValidTable, 1ULL);
    EXPECT_EQ(tage->tageStats.updateAllocFailureNoValidTable, 0ULL);
}

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
