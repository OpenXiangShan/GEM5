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
    stream.updateBTBEntries = {entry};
    stream.exeTaken = true;
    stream.exeBranchInfo = entry;

    tage->update(stream);
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

} // namespace test

} // namespace btb_pred

} // namespace branch_prediction

} // namespace gem5
